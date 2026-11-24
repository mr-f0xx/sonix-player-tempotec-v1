export const types = ['PK', 'LSC', 'HSC', 'LPQ', 'HPQ'];
export const usesGain = type => type !== 'LPQ' && type !== 'HPQ';
export const clamp = (value, min, max) => Math.min(max, Math.max(min, value));
export function newPreset() {
  return { preamp: 0, bands: [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
    .map(freq => ({ on: false, type: 'PK', freq, gain: 0, q: 0.71 })) };
}

function ranged(value, min, max, places) {
  if (!Number.isFinite(value)) throw new Error('Enter a number in each PEQ field.');
  return Number(clamp(value, min, max).toFixed(places));
}

export function normalize(preset) {
  if (preset.bands.length !== 10) throw new Error('A Sonix preset has ten bands.');
  return { preamp: ranged(preset.preamp, -15, 6, 1), bands: preset.bands.map(b => {
    if (!types.includes(b.type)) throw new Error('Unsupported filter type.');
    return { on: b.on, type: b.type, freq: ranged(b.freq, 20, 20000, 0),
      gain: usesGain(b.type) ? ranged(b.gain, -15, 15, 1) : 0, q: ranged(b.q, 0.1, 10, 2) };
  }) };
}

export function serializePreset(preset) {
  const { preamp, bands } = normalize(preset);
  return [`Preamp: ${preamp.toFixed(1)} dB`, ...bands.map((b, i) =>
    `Filter ${i + 1}: ${b.on ? 'ON' : 'OFF'} ${b.type} Fc ${b.freq} Hz` +
    (usesGain(b.type) ? ` Gain ${b.gain.toFixed(1)} dB` : '') + ` Q ${b.q.toFixed(2)}`), ''].join('\n');
}

// Deliberately accept the single-curve subset we can display and round-trip.
// Do not silently drop extra channels, filters, or unsupported APO directives.
export function parsePreset(text) {
  const preset = newPreset();
  const aliases = { PEQ: 'PK', MODAL: 'PK', LS: 'LSC', HS: 'HSC', LP: 'LPQ', HP: 'HPQ' };
  const n = '([+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+))';
  const preamp = new RegExp(`^Preamp:\\s*${n}\\s+dB$`, 'i');
  const filter = new RegExp(`^Filter(?:\\s+\\d+)?:\\s*(ON|OFF)\\s+(\\w+)\\s+Fc\\s+${n}\\s+Hz` +
    `(?:\\s+Gain\\s+${n}\\s+dB)?(?:\\s+Q\\s+${n})?$`, 'i');
  let count = 0;
  for (const raw of text.trim().split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const p = line.match(preamp);
    if (p) { preset.preamp = Number(p[1]); continue; }
    const match = line.match(filter);
    if (!match || count === 10) throw new Error('Use a single APO curve with up to ten supported filters (Fc, Gain and Q).');
    const [, on, code, freq, gain = '0', q = '0.71'] = match;
    const type = aliases[code.toUpperCase()] ?? code.toUpperCase();
    preset.bands[count++] = { on: on.toUpperCase() === 'ON', type, freq: Number(freq), gain: Number(gain), q: Number(q) };
  }
  if (!count) throw new Error('No PEQ filters found.');
  return normalize(preset);
}

// Same RBJ coefficients as eq.c, including Q-width shelves. No headroom or
// output clipping simulation: this is just the small-signal filter response.
function coefficients({ type, freq, gain, q }, rate) {
  const w = 2 * Math.PI * freq / rate, c = Math.cos(w), alpha = Math.sin(w) / (2 * q);
  const A = 10 ** (gain / 40);
  if (type === 'PK') return [[1 + alpha * A, -2 * c, 1 - alpha * A], [1 + alpha / A, -2 * c, 1 - alpha / A]];
  if (!usesGain(type)) {
    const high = type === 'HPQ', b = (1 + (high ? c : -c)) / 2;
    return [[b, (high ? -2 : 2) * b, b], [1 + alpha, -2 * c, 1 - alpha]];
  }
  const p = A + 1, m = A - 1, s = 2 * Math.sqrt(A) * alpha;
  return type === 'LSC' ? [[A * (p - m * c + s), 2 * A * (m - p * c), A * (p - m * c - s)],
    [p + m * c + s, -2 * (m + p * c), p + m * c - s]] :
    [[A * (p + m * c + s), -2 * A * (m + p * c), A * (p + m * c - s)],
      [p - m * c + s, 2 * (m - p * c), p - m * c - s]];
}

export function response(preset, rate = 48000) {
  const filters = preset.bands.filter(b => b.on && b.freq * 2 < rate).map(b => coefficients(b, rate));
  return freq => {
    const w = 2 * Math.PI * freq / rate;
    const mag = ([a, b, c]) => Math.hypot(a + b * Math.cos(w) + c * Math.cos(2 * w), b * Math.sin(w) + c * Math.sin(2 * w));
    return filters.reduce((db, [num, den]) => db + 20 * Math.log10(mag(num) / mag(den)), preset.preamp);
  };
}
