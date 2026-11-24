import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { mkdtemp, mkdir, readFile, readdir, lstat, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { promisify } from 'node:util';
import test from 'node:test';
import { deviceFiles, playlistText, quote, safeName } from '../public/remote-files.js';
import { newPreset, parsePreset, response, serializePreset } from '../public/remote-peq.js';

const exec = promisify(execFile);

test('APO output round-trips Sonix filter types and applies firmware ranges', () => {
  const preset = parsePreset(`\uFEFF# setup\r\nPreamp: -19 dB\r\nFilter 1: ON PK Fc 1000 Hz Gain 5.3 dB Q 1.33\r\nFilter 2: OFF HPQ Fc 75 Hz Q 0.71\r\nFilter 3: ON LS Fc 5 Hz Gain 3 dB Q 0.7\r\n`);
  assert.equal(preset.preamp, -15);
  assert.deepEqual(preset.bands[2], { on: true, type: 'LSC', freq: 20, gain: 3, q: 0.7 });
  const text = serializePreset(preset);
  assert.match(text, /^Filter 2: OFF HPQ Fc 75 Hz Q 0.71$/m);
  assert.deepEqual(parsePreset(text), preset);
  assert.throws(() => parsePreset(`${text}Filter 11: ON PK Fc 300 Hz Gain 2 dB Q 1\n`), /ten/);
  assert.throws(() => parsePreset('Channel: L\nFilter: ON PK Fc 20 Hz'), /single APO/);
});

test('PEQ preview uses the correct on/off, preamp, centre gain, shelves and pass response', () => {
  const preset = newPreset();
  preset.preamp = -3;
  assert.equal(response(preset)(1000), -3);
  const band = preset.bands[5];
  Object.assign(band, { on: true, gain: 9 });
  assert.ok(Math.abs(response(preset)(1000) - 6) < 1e-9);
  band.type = 'LSC';
  assert.ok(Math.abs(response(preset)(20) - 6) < 0.1);
  band.type = 'HSC';
  assert.ok(Math.abs(response(preset)(20000) - 6) < 0.1);
  band.type = 'LPQ';
  assert.ok(response(preset)(20000) < -40);
  band.type = 'HPQ';
  assert.ok(response(preset)(20) < -40);
});

test('playlist format preserves order and metadata, and names cannot escape their output folders', () => {
  assert.equal(playlistText('\uFEFF#EXTM3U\r\n#EXTINF:10,Titre\r\nMusic/é.flac\r\nMusic/b.flac\r\n'),
    '#EXTM3U\n#EXTINF:10,Titre\nMusic/é.flac\nMusic/b.flac\n');
  assert.equal(playlistText('Music/a.flac'), '#EXTM3U\nMusic/a.flac\n');
  assert.throws(() => playlistText('# comment only'), /track path/);
  assert.throws(() => playlistText('a\0b'), /control/);
  assert.throws(() => playlistText('é'.repeat(501)), /1,001/);
  for (const name of ['../x', '/tmp/x', 'a; rm x', 'a\nb', '.hidden']) assert.throws(() => safeName(name));
  assert.equal(safeName(' My Kit 2 '), 'My Kit 2');
});

// Exercise the actual shell templates against a temporary simulated card, and
// emulate ADB sync without requiring a USB device. The legacy mock rejects exec:.
async function fixture(t, { legacy = false, failWrite = false } = {}) {
  const card = await mkdtemp(join(tmpdir(), 'sonix-live-'));
  t.after(() => rm(card, { recursive: true, force: true }));
  const mounts = join(card, 'mounts');
  await writeFile(mounts, `card ${card} vfat rw 0 0\n`);
  const local = path => path.replace('/mnt/sd_0', card);
  const adb = {
    async createSocket(service) {
      if (legacy && service.startsWith('exec:')) throw Error('unknown service');
      const command = service.slice(service.indexOf(':') + 1).replaceAll('/mnt/sd_0', card).replaceAll('/proc/mounts', quote(mounts));
      let output;
      try { output = (await exec('sh', ['-c', command])).stdout; }
      catch (error) { output = error.stdout + error.stderr; }
      if (legacy) output = output.replaceAll('\n', '\r\n');
      return { readable: new Blob([output]).stream(), close: async () => {} };
    },
    async sync() {
      return {
        lstat: path => lstat(local(path)),
        read: path => ReadableStream.from([readFile(local(path))]),
        async write({ filename, file }) {
          await writeFile(local(filename), Buffer.from(await new Response(file).arrayBuffer()));
          if (failWrite) throw Error('simulated sync failure');
        },
        dispose: async () => {},
      };
    },
  };
  return { card, mounts, files: deviceFiles(adb) };
}

test('legacy ADB: round-trip a preset and import file, and download the exact debug log bytes', async t => {
  const { card, files, mounts } = await fixture(t, { legacy: true });
  const peq = serializePreset(newPreset());
  await files.savePreset('Studio', peq);
  assert.deepEqual(await files.listPresets(), ['Studio.txt']);
  assert.equal(await files.readPreset('Studio.txt'), peq);
  await assert.rejects(files.savePreset('Studio', 'replacement'), /existing/);
  assert.equal(await files.readPreset('Studio.txt'), peq);
  await assert.rejects(files.readPreset('../Studio.txt'), /Choose/);
  const playlist = "Show's picks.M3U8";
  await writeFile(join(card, playlist), 'Music/live.flac');
  await files.savePlaylist('New Playlist', 'Music/bar.flac');
  await mkdir(join(card, 'Playlist'));
  await writeFile(join(card, 'Playlist', 'Backup.m3u'), 'Music/backed-up.flac');
  assert.deepEqual(await files.listPlaylists(), ['New Playlist.m3u', playlist, 'Playlist/Backup.m3u']);
  assert.equal(await files.readPlaylist('Playlist/Backup.m3u'), 'Music/backed-up.flac');
  assert.equal(await files.readPlaylist(playlist), 'Music/live.flac');
  await mkdir(join(card, '.local'));
  await assert.rejects(files.downloadLog(), /not found/);
  const payload = Buffer.from([0, 1, 2, 0xff, 13, 10]);
  await writeFile(join(card, '.local', 'sonix_player.log'), payload);
  assert.deepEqual(Buffer.from(await (await files.downloadLog()).arrayBuffer()), payload);
  await writeFile(mounts, '');
  await assert.rejects(files.savePlaylist('Not Mounted', 'Track.flac'), /not mounted/);
});

function wav(name) {
  const data = Buffer.alloc(46);
  data.write('RIFF'); data.writeUInt32LE(38, 4); data.write('WAVEfmt ', 8);
  data.writeUInt32LE(16, 16); data.writeUInt16LE(1, 20); data.writeUInt16LE(1, 22);
  data.writeUInt32LE(8000, 24); data.writeUInt32LE(16000, 28); data.writeUInt16LE(2, 32);
  data.writeUInt16LE(16, 34); data.write('data', 36); data.writeUInt32LE(2, 40);
  return new File([data], name);
}

test('a new drum kit keeps gaps and publishes numbered files without modifying existing kits', async t => {
  const { card, files } = await fixture(t);
  const pads = Array(16).fill(null);
  pads[0] = wav('Kick.wav'); pads[15] = wav("Snare's.wav");
  await files.saveKit('New Kit', pads, () => {});
  assert.deepEqual(await readdir(join(card, 'Drum Kits', 'New Kit')), ['01 Kick.wav', '16 Snare_s.wav']);
  await assert.rejects(files.saveKit('New Kit', pads, () => {}), /existing/);
  pads[1] = new File(['test'], 'fake.wav');
  await assert.rejects(files.saveKit('Bad Kit', pads, () => {}), /WAV/);
  assert.deepEqual(await readdir(join(card, 'Drum Kits')), ['New Kit']);
});

test('an interrupted push removes its staging file when still connected', async t => {
  const { card, files } = await fixture(t, { failWrite: true });
  await assert.rejects(files.savePreset('Studio', 'test'), /simulated sync failure/);
  assert.deepEqual(await readdir(join(card, 'PEQ')), []);
});
