import { clamp, newPreset, normalize, parsePreset, response, serializePreset, usesGain } from './remote-peq.js';
import { deviceFiles, playlistText, safeName, textLimit } from './remote-files.js';

const $ = id => document.getElementById(`remote-${id}`);
const panel = document.getElementById('tab-remote');
const connectButton = $('connect'), disconnectButton = $('disconnect');
const deviceControls = panel.querySelectorAll('[data-needs-usb]');
const connectionStatus = $('status');
let connectV1, session, busy = false, closing = false;

function status(message, error = false, target = connectionStatus) {
  target.textContent = message;
  target.dataset.error = String(error);
}
function controls() {
  connectButton.disabled = !connectV1 || Boolean(session) || busy || closing;
  disconnectButton.disabled = !session || closing;
  deviceControls.forEach(control => { control.disabled = !session || busy || closing; });
}
function resultFor(element) {
  const card = element.closest('.remote-card');
  if (card.contains(connectionStatus)) return connectionStatus;
  let result = card.querySelector('.remote-result');
  if (!result) {
    result = document.createElement('p');
    result.className = 'remote-result';
    result.setAttribute('role', 'status');
    card.append(result);
  }
  return result;
}
function lost(active, message, error = false) {
  if (session !== active) return;
  session = undefined;
  busy = false;
  fillSelect($('peq-select'), 'Choose a preset', []);
  fillSelect($('playlist-select'), 'Choose a playlist file', []);
  status(message, error);
  controls();
}
function action(id, work, usb = false) {
  const element = $(id);
  element.addEventListener('click', async () => {
    const active = session;
    if (usb && (!active || busy || closing)) return;
    const target = resultFor(element);
    const report = (message, error) => status(message, error, target);
    if (usb) {
      busy = true;
      controls();
      active.operationTarget = target;
      report('Working…');
    }
    try {
      const message = await work(active, text => { if (session === active) report(text); });
      if ((!usb || session === active) && message) report(message);
    } catch (error) {
      if (!usb || session === active) report(error.message || String(error), true);
    } finally {
      if (usb && session === active) {
        active.operationTarget = undefined;
        busy = false;
        controls();
      }
    }
  });
}

if (!window.isSecureContext || !navigator.usb || document.permissionsPolicy?.allowsFeature('usb') === false ||
    document.featurePolicy?.allowsFeature('usb') === false) {
  status('USB is unavailable here. Open the HTTPS site directly in a WebUSB browser; local editing still works.');
} else {
  import('./remote-usb.bundle.js').then(transport => {
    connectV1 = transport.connectV1;
    controls();
  }).catch(() => status('USB transport was not built for this site. Local editing still works.', true));
}
connectButton.addEventListener('click', async () => {
  if (!connectV1 || busy || session) return;
  busy = true;
  controls();
  status('Choose your V1. Approve USB debugging on the player if prompted.');
  try {
    const adb = await connectV1();
    if (!adb) { status('No device selected.'); return; }
    const active = session = deviceFiles(adb);
    const disconnected = () => {
      if (active.operationTarget && session === active) status('Transfer interrupted: V1 disconnected.', true, active.operationTarget);
      lost(active, 'V1 disconnected. You can reconnect without losing your edits.');
    };
    active.disconnected.then(disconnected, disconnected);
    status('V1 connected over USB.');
  } catch (error) {
    status(error.name === 'NotFoundError' ? 'No device selected.' :
      `Connection failed: ${error.message}. Check ADB mode, your data cable and whether another app is using USB.`, error.name !== 'NotFoundError');
  } finally {
    busy = false;
    controls();
  }
});
disconnectButton.addEventListener('click', async () => {
  const active = session;
  if (!active || closing) return;
  closing = true;
  if (active.operationTarget) status('Transfer interrupted by disconnect.', true, active.operationTarget);
  lost(active, 'Disconnecting…');
  try { await active.close(); status('Disconnected.'); }
  catch (error) { status(`Could not close USB cleanly: ${error.message}. Unplug the V1 before reconnecting.`, true); }
  finally { closing = false; controls(); }
});

function download(name, blob) {
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = name;
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30000);
}
function fillSelect(select, label, names) {
  select.replaceChildren(new Option(label, ''), ...names.map(name => new Option(name, name)));
}
function openLocal(id, consume) {
  $(id).addEventListener('change', async event => {
    const input = event.currentTarget, file = input.files[0], target = resultFor(input);
    if (!file) return;
    input.value = '';
    try {
      if (file.size > textLimit) throw new Error('File is larger than 256 KiB.');
      const text = new TextDecoder('utf-8', { fatal: true }).decode(await file.arrayBuffer());
      consume(text);
      status(`Opened ${file.name}.`, false, target);
    } catch (error) { status(error.message, true, target); }
  });
}
action('log', async active => {
  const blob = await active.downloadLog();
  if (session === active) download('sonix_player.log', blob);
  return 'Debug log downloaded. Review it before sharing.';
}, true);

let preset = newPreset(), selected = 0;
const chart = $('curve');
const bounds = { left: 36, right: 406, top: 12, bottom: 161 };
const xOf = freq => bounds.left + Math.log(freq / 20) / Math.log(1000) * (bounds.right - bounds.left);
const yOf = db => bounds.top + (24 - clamp(db, -24, 24)) / 48 * (bounds.bottom - bounds.top);
const fields = Object.fromEntries(['on', 'type', 'freq', 'gain', 'q'].map(key => [key, $(`band-${key}`)]));
const bandButtons = preset.bands.map((_, index) => {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'btn btn-ghost btn-sm';
  button.textContent = String(index + 1);
  button.setAttribute('aria-label', `Select band ${index + 1}`);
  button.onclick = () => { selected = index; syncFields(); draw(); };
  $('bands').append(button);
  return button;
});
function svg(tag, attrs, text) {
  const node = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const [key, value] of Object.entries(attrs)) node.setAttribute(key, value);
  if (text !== undefined) node.textContent = text;
  return node;
}
for (const freq of [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]) {
  const x = xOf(freq);
  chart.append(svg('line', { x1: x, x2: x, y1: bounds.top, y2: bounds.bottom, class: 'remote-grid' }),
    svg('text', { x, y: 176, 'text-anchor': 'middle', class: 'remote-axis' }, freq < 1000 ? freq : `${freq / 1000}k`));
}
for (const db of [-24, -12, 0, 12, 24]) {
  const y = yOf(db);
  chart.append(svg('line', { x1: bounds.left, x2: bounds.right, y1: y, y2: y, class: 'remote-grid' }),
    svg('text', { x: 29, y: y + 3, 'text-anchor': 'end', class: 'remote-axis' }, `${db > 0 ? '+' : ''}${db}`));
}
const curve = svg('path', { class: 'remote-response' });
const points = svg('g', {});
chart.append(curve, points);
function draw() {
  const dbAt = response(preset);
  const path = Array.from({ length: 241 }, (_, i) => {
    const freq = 20 * 1000 ** (i / 240);
    return `${i ? 'L' : 'M'}${xOf(freq).toFixed(2)},${yOf(dbAt(freq)).toFixed(2)}`;
  }).join(' ');
  curve.setAttribute('d', path);
  points.replaceChildren(...preset.bands.flatMap((b, index) => b.on || index === selected ? [svg('circle', {
    cx: xOf(b.freq), cy: yOf(b.gain), r: index === selected ? 6 : 5,
    class: `remote-point${index === selected ? ' selected' : ''}`, 'data-band': index,
  })] : []));
  bandButtons.forEach((button, i) => {
    button.setAttribute('aria-pressed', String(i === selected));
    button.title = preset.bands[i].on ? 'On' : 'Off';
  });
}
function syncFields() {
  const band = preset.bands[selected];
  for (const [key, input] of Object.entries(fields)) {
    if (key === 'on') input.checked = band.on;
    else input.value = band[key];
  }
  fields.gain.disabled = !usesGain(band.type);
  $('preamp').value = preset.preamp;
}
for (const [key, input] of Object.entries({ ...fields, preamp: $('preamp') })) {
  input.addEventListener('change', () => {
    const value = input.type === 'number' ? input.valueAsNumber : key === 'on' ? input.checked : input.value;
    if (input.type === 'number' && !Number.isFinite(value)) { syncFields(); return; }
    if (key === 'preamp') preset.preamp = value;
    else preset.bands[selected][key] = value;
    preset = normalize(preset);
    syncFields(); draw();
  });
}
function movePoint(event) {
  const p = new DOMPoint(event.clientX, event.clientY).matrixTransform(chart.getScreenCTM().inverse());
  const band = preset.bands[selected];
  band.freq = Math.round(20 * 1000 ** clamp((p.x - bounds.left) / (bounds.right - bounds.left), 0, 1));
  if (usesGain(band.type)) band.gain = Math.round(clamp(24 - 48 * (p.y - bounds.top) / (bounds.bottom - bounds.top), -15, 15) * 10) / 10;
  band.on = true;
  syncFields(); draw();
}
let pointer;
chart.addEventListener('pointerdown', event => {
  if (pointer !== undefined || event.button !== 0) return;
  if (event.target.dataset.band !== undefined) selected = Number(event.target.dataset.band);
  pointer = event.pointerId;
  chart.setPointerCapture(pointer);
  movePoint(event);
});
chart.addEventListener('pointermove', event => { if (event.pointerId === pointer) movePoint(event); });
chart.addEventListener('lostpointercapture', event => { if (event.pointerId === pointer) pointer = undefined; });
function loadPreset(text) {
  preset = parsePreset(text);
  selected = 0;
  syncFields(); draw();
}
syncFields(); draw();
openLocal('peq-file', loadPreset);
action('peq-export', () => {
  download(`${safeName($('peq-name').value)}.txt`, new Blob([serializePreset(preset)], { type: 'text/plain' }));
  return 'APO preset exported.';
});
action('peq-save', async active => {
  await active.savePreset($('peq-name').value, serializePreset(preset));
  return 'Preset saved. Apply it from the PEQ preset menu on the V1.';
}, true);

const pads = Array(16).fill(null);
const padRows = pads.map((_, index) => {
  const tile = document.createElement('div');
  tile.className = 'remote-pad';
  const number = String(index + 1).padStart(2, '0');
  const title = document.createElement('strong');
  title.textContent = `Pad ${number}`;
  const name = document.createElement('span');
  name.className = 'remote-pad-name';
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = '.wav,audio/wav';
  input.setAttribute('aria-label', `Choose WAV for pad ${number}`);
  const buttons = document.createElement('div');
  buttons.className = 'remote-actions';
  const addButton = (label, description, work) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'btn btn-ghost btn-sm';
    button.textContent = label;
    button.setAttribute('aria-label', `${description} pad ${number}`);
    button.onclick = () => { work(); refreshPads(); };
    buttons.append(button);
    return button;
  };
  const swap = target => { [pads[index], pads[target]] = [pads[target], pads[index]]; };
  const left = addButton('←', 'Move sample left from', () => swap(index - 1));
  const right = addButton('→', 'Move sample right from', () => swap(index + 1));
  const clear = addButton('×', 'Clear', () => { pads[index] = null; });
  input.onchange = () => {
    if (input.files[0]) pads[index] = input.files[0];
    input.value = '';
    refreshPads();
  };
  tile.append(title, name, input, buttons);
  $('pads').append(tile);
  return { name, left, right, clear };
});
function refreshPads() {
  padRows.forEach(({ name, left, right, clear }, i) => {
    name.textContent = pads[i]?.name || 'Empty';
    name.title = name.textContent;
    left.disabled = !pads[i] || i === 0;
    right.disabled = !pads[i] || i === 15;
    clear.disabled = !pads[i];
  });
}
refreshPads();
action('kit-save', async (active, progress) => {
  await active.saveKit($('kit-name').value, [...pads], progress);
  return 'Kit saved. Load it from Tape Deck on the V1.';
}, true);

openLocal('playlist-file', text => { $('playlist').value = text; });
action('playlist-export', () => {
  download(`${safeName($('playlist-name').value)}.m3u`, new Blob([playlistText($('playlist').value)], { type: 'audio/x-mpegurl' }));
  return 'M3U exported.';
});
action('playlist-save', async active => {
  await active.savePlaylist($('playlist-name').value, $('playlist').value);
  return 'Import file saved. Use Playlists → Import on the V1.';
}, true);

for (const [prefix, list, read, consume] of [
  ['peq', 'listPresets', 'readPreset', loadPreset],
  ['playlist', 'listPlaylists', 'readPlaylist', text => { $('playlist').value = text; }],
]) {
  action(`${prefix}-list`, async active => {
    const names = await active[list]();
    if (session === active) fillSelect($(`${prefix}-select`), 'Choose a file', names);
    return names.length ? `${names.length} file(s) found.` : 'No compatible files found on the player.';
  }, true);
  action(`${prefix}-load`, async active => {
    const name = $(`${prefix}-select`).value;
    const text = await active[read](name);
    if (session === active) consume(text);
    return `Opened ${name}. Save with a new name to keep the original.`;
  }, true);
}
