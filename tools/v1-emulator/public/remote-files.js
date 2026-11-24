// Only these Sonix paths are exposed. Names originating on the card are quoted too.
const CARD = '/mnt/sd_0';
const PEQ = `${CARD}/PEQ`;
const PLAYLIST_BACKUPS = `${CARD}/Playlist`;
const PLAYLIST_PATTERNS = ['*.[mM]3[uU]', '*.[mM]3[uU]8'];
const KITS = `${CARD}/Drum Kits`;
const LOG = `${CARD}/.local/sonix_player.log`;
const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8', { fatal: true });
export const quote = value => `'${value.replaceAll("'", "'\\''")}'`;
export const textLimit = 256 * 1024;
export const wavLimit = 16 * 1024 * 1024;

export function safeName(value) {
  const name = value.trim();
  if (!/^[a-zA-Z0-9][a-zA-Z0-9 _-]{0,47}$/.test(name)) {
    throw new Error('Use 1–48 letters, numbers, spaces, hyphens or underscores, starting with a letter or number.');
  }
  return name;
}

export function playlistText(value) {
  const lines = value.replace(/^\uFEFF/, '').replace(/\r\n?/g, '\n').split('\n');
  const tracks = lines.filter(line => line.trim() && !line.trimStart().startsWith('#'));
  if (!tracks.length || lines.some(line => /[\x00-\x08\x0b-\x1f]/.test(line) || encoder.encode(line).length > 1000)) {
    throw new Error('Add at least one track path and keep each line under 1,001 UTF-8 bytes, without control characters.');
  }
  const text = (lines[0] === '#EXTM3U' ? '' : '#EXTM3U\n') + lines.join('\n').trimEnd() + '\n';
  if (encoder.encode(text).length > textLimit) throw new Error('Playlist is larger than 256 KiB.');
  return text;
}

async function checkWav(file) {
  if (!file || file.size < 44 || file.size > wavLimit) throw new Error('Each pad needs a WAV no larger than 16 MiB.');
  const data = new DataView(await file.slice(0, 12).arrayBuffer());
  if (data.getUint32(0) !== 0x52494646 || data.getUint32(8) !== 0x57415645) {
    throw new Error(`${file.name} is not a RIFF WAV.`);
  }
  // Walk headers without reading audio into memory. Require the PCM format
  // advertised by the player; codecs in a RIFF container are not enough.
  let validFormat = false;
  for (let offset = 12, chunks = 0; offset + 8 <= file.size && chunks < 256; chunks++) {
    const chunk = new DataView(await file.slice(offset, offset + 8).arrayBuffer());
    const id = chunk.getUint32(0), size = chunk.getUint32(4, true);
    offset += 8;
    if (offset + size > file.size) break;
    if (id === 0x666d7420 && size >= 16) { // fmt
      const fmt = new DataView(await file.slice(offset, offset + 16).arrayBuffer());
      validFormat = fmt.getUint16(0, true) === 1 && [1, 2].includes(fmt.getUint16(2, true)) &&
        fmt.getUint32(4, true) >= 8000 && fmt.getUint32(4, true) <= 96000 &&
        [8, 16, 24, 32].includes(fmt.getUint16(14, true));
    }
    if (id === 0x64617461 && size && validFormat) return; // data
    offset += size + (size % 2);
  }
  throw new Error(`${file.name}: use mono or stereo PCM WAV, 8–96 kHz, 8/16/24/32-bit.`);
}

function checkSize(size, limit) {
  if (size > limit) throw new Error(`File exceeds the ${limit / 1024} KiB download limit.`);
}

async function collect(stream, limit) {
  const reader = stream.getReader();
  const chunks = [];
  let size = 0;
  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) return new Blob(chunks);
      size += value.length;
      checkSize(size, limit);
      chunks.push(value);
    }
  } finally {
    reader.releaseLock();
  }
}

export function deviceFiles(adb) {
  // Old stock adbd does not necessarily offer shell v2 exit status. A private
  // marker confirms completion; never infer success from an empty shell reply.
  async function shell(command) {
    const marker = `SONIX_OK_${crypto.randomUUID().replaceAll('-', '')}`;
    const cmd = `${command} && printf '\\n${marker}\\n'`;
    let socket;
    let pty = false;
    try { socket = await adb.createSocket(`exec:${cmd}`); }
    catch {
      // Older adbd builds may only implement shell:. Fall back only if
      // opening the service failed, never retry a command already sent.
      pty = true;
      socket = await adb.createSocket(`shell:${cmd}`);
    }
    let output;
    try { output = await (await collect(socket.readable, textLimit)).text(); }
    finally { await socket.close().catch(() => {}); }
    if (pty) output = output.replaceAll('\r\n', '\n');
    const suffix = `\n${marker}\n`;
    if (!output.endsWith(suffix)) throw new Error(output.trim() || 'The player rejected the operation. Check the card and free space.');
    return output.slice(0, -suffix.length);
  }
  // /mnt and /data can be symlinks; /proc/mounts uses the physical path.
  const mountCheck = `root=$(cd ${quote(CARD)} && pwd -P) && grep -qF " $root " /proc/mounts`;
  const mounted = () => shell(mountCheck).catch(() => {
    throw new Error('The V1 microSD is not mounted or USB was disconnected. Insert the card and leave mass storage off.');
  });
  const usingSync = async work => {
    const sync = await adb.sync();
    try { return await work(sync); } finally { await sync.dispose(); }
  };
  async function read(path, limit) {
    await mounted();
    await shell(`[ -f ${quote(path)} ] && [ ! -L ${quote(path)} ]`).catch(() => {
      throw new Error(`File not found: ${path}`);
    });
    return usingSync(async sync => {
      const stat = await sync.lstat(path);
      checkSize(stat.size, limit);
      return collect(sync.read(path), limit);
    });
  }
  const write = (path, blob) => usingSync(sync => sync.write({ filename: path, file: blob.stream(), permission: 0o644 }));
  const absent = path => `[ ! -e ${quote(path)} ] && [ ! -L ${quote(path)} ]`;
  const mkdir = path => `mkdir -p ${quote(path)} && [ ! -L ${quote(path)} ]`;
  const stagingPath = dir => `${dir}/.sonix-remote-${crypto.randomUUID()}`;
  async function publish(temp, dest) {
    // Stock BusyBox 1.31 supports -n but not -T. Check both before and
    // after the rename: -n can report success without moving anything, or
    // mv can nest the source if an outside writer just created a directory.
    const nested = `${dest}/${temp.slice(temp.lastIndexOf('/') + 1)}`;
    await shell(`${mountCheck} && ${absent(dest)} && mv -n ${quote(temp)} ${quote(dest)} && ${absent(temp)} && ` +
      `(if [ -e ${quote(nested)} ]; then mv -n ${quote(nested)} ${quote(temp)}; false; else [ -e ${quote(dest)} ]; fi)`);
  }
  async function writeNew(dir, name, text) {
    const dest = `${dir}/${name}`, temp = stagingPath(dir);
    await mounted();
    await shell(`${mountCheck} && ${mkdir(dir)} && ${absent(dest)}`).catch(error => {
      throw new Error(`Cannot create ${name}; check for an existing file or a read-only card. ${error.message}`);
    });
    try {
      await write(temp, new Blob([text]));
      await publish(temp, dest);
    } catch (error) {
      await shell(`${mountCheck} && rm -f ${quote(temp)}`).catch(() => {});
      throw error;
    }
  }
  async function list(dir, ...patterns) {
    await mounted();
    // Globs are fixed, not user inputs. An absent directory means no files yet.
    const names = await shell(`(for f in ${patterns.map(pattern => `${quote(dir)}/${pattern}`).join(' ')}; do [ -f "$f" ] && printf '%s\\000' "\${f##*/}"; done; :)`);
    return names.split('\0').filter(Boolean).sort((a, b) => a.localeCompare(b));
  }
  const readText = async (dir, name) => {
    if (!name || name === '.' || name === '..' || /[/\0]/.test(name)) throw new Error('Choose a file on the player.');
    return decoder.decode(await (await read(`${dir}/${name}`, textLimit)).arrayBuffer());
  };
  return {
    disconnected: adb.disconnected,
    close: () => adb.close(),
    listPresets: () => list(PEQ, '*.[tT][xX][tT]'),
    readPreset: name => readText(PEQ, name),
    savePreset: (name, text) => writeNew(PEQ, `${safeName(name)}.txt`, text),
    async listPlaylists() {
      const imports = await list(CARD, ...PLAYLIST_PATTERNS);
      const backups = await list(PLAYLIST_BACKUPS, ...PLAYLIST_PATTERNS);
      return [...imports, ...backups.map(name => `Playlist/${name}`)];
    },
    readPlaylist: name => name.startsWith('Playlist/') ?
      readText(PLAYLIST_BACKUPS, name.slice('Playlist/'.length)) : readText(CARD, name),
    savePlaylist: (name, text) => writeNew(CARD, `${safeName(name)}.m3u`, playlistText(text)),
    downloadLog: () => read(LOG, 32 * 1024 * 1024),
    async saveKit(name, pads, progress) {
      const dest = `${KITS}/${safeName(name)}`, temp = stagingPath(KITS);
      if (pads.length !== 16 || !pads.some(Boolean)) throw new Error('Place a WAV on at least one pad.');
      for (const file of pads.filter(Boolean)) await checkWav(file);
      await mounted();
      await shell(`${mountCheck} && ${mkdir(KITS)} && ${absent(dest)} && mkdir ${quote(temp)}`).catch(error => {
        throw new Error(`Cannot create kit; check for an existing name or a read-only card. ${error.message}`);
      });
      try {
        for (const [index, file] of pads.entries()) {
          if (!file) continue;
          const number = String(index + 1).padStart(2, '0');
          progress(`Uploading pad ${number}…`);
          const label = file.name.replace(/\.wav$/i, '').replace(/[^a-zA-Z0-9 _-]/g, '_').slice(0, 48).trim();
          await write(`${temp}/${number}${label ? ` ${label}` : ''}.wav`, file);
        }
        await publish(temp, dest);
      } catch (error) {
        await shell(`${mountCheck} && rm -rf ${quote(temp)}`).catch(() => {});
        throw error;
      }
    },
  };
}
