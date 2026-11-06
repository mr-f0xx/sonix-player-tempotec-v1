/* Visual demo only. Geometry mirrors src/gui/nowplaying/vinylayout.c at the
 * V1's default text metrics; the 360x640 canvas is a labelled concept, not a
 * second device profile. No audio, network requests or firmware calls. */
const $ = (id) => document.getElementById(id);
const screen = $('screen');
const DEMO_INFO_H = 38; // browser font metrics; firmware measures its own faces
let canvas = 'native', zoom = 1, playing = false, favourite = false, missingArt = false;
let progress = 280, repeatMode = 0, toastTimer, playTimer;
const tracks = [
  ['Canned Heat', 'Jamiroquai'],
  ['Virtual Insanity', 'Jamiroquai'],
  ['Cosmic Girl', 'Jamiroquai'],
];
let track = 0;

function layoutBox(w, h, deckH) {
  const top = 10, gap = 6, sideMargin = 24, recordPct = 86, recessPct = 10;
  const peekPct = Math.floor(recordPct / 2) - recessPct;
  const deckY = h - deckH;
  const controlsY = h - 44 - 4 - 2;
  const room = deckY - top - gap;
  let side = Math.min(Math.floor(room * 100 / (100 + peekPct)), deckY - top, w - 2 * sideMargin);
  side = Math.max(2, side & ~1);
  let d = Math.max(2, (Math.floor(side * recordPct / 100)) & ~1);
  const cy = top + side - Math.floor(side * recessPct / 100);
  return { artX: (w - side) / 2, artY: top, side, recordX: w / 2 - d / 2, recordY: cy - d / 2, d, deckY };
}
function setBox(el, x, y, w, h) {
  Object.assign(el.style, { left: `${x}px`, top: `${y}px`, width: `${w}px`, height: `${h}px` });
}
function renderGeometry() {
  const w = canvas === 'native' ? 240 : 360;
  const h = canvas === 'native' ? 320 : 640;
  // Reference canvas keeps the same compact deck rather than scaling type.
  const deckH = 4 + DEMO_INFO_H + 4 + 10 + 4 + 44 + 4 + 2;
  const b = layoutBox(w, h, deckH);
  document.documentElement.style.setProperty('--canvas-w', `${w}px`);
  document.documentElement.style.setProperty('--canvas-h', `${h}px`);
  document.documentElement.style.setProperty('--zoom', zoom);
  setBox($('sleeve'), b.artX, b.artY, b.side, b.side);
  setBox($('record'), b.recordX, b.recordY, b.d, b.d);
  $('deck').style.top = `${b.deckY}px`;
  // Clean native-resolution grooves: even diameters and a 2 px lifted stroke,
  // mirroring VINYL_GROOVE_* in vinylayout.h. No bitmap texture is scaled.
  [0, 1, 2].forEach((i) => {
    const g = document.querySelector(`.g${i + 1}`);
    let gd = b.d - Math.floor(b.d * (12 + i * 10) / 100);
    gd &= ~1; if (gd < 4) gd = 4;
    g.style.width = `${gd}px`; g.style.height = `${gd}px`;
  });
  const label = Math.floor(b.d * .28);
  Object.assign(document.querySelector('.record-label').style, { width: `${label}px`, height: `${label}px` });
  // Reference pressing mark: a thin rounded stroke leaning down-right in the
  // exposed lower arc. Percentages mirror VINYL_MARK_* in vinylayout.h; the
  // CSS rotation matches the firmware's -15 degrees around the stroke centre.
  const markW = Math.max(1, Math.round(b.d / 173));
  const markH = Math.max(2, Math.floor(b.d * .18));
  const markX1 = Math.floor(b.d * .56);
  const markY1 = Math.floor(b.d * .71);
  const markDx = Math.max(1, Math.floor(b.d * .05));
  Object.assign(document.querySelector('.shine').style, {
    left: `${markX1 + Math.floor(markDx / 2) - Math.floor(markW / 2)}px`,
    top: `${markY1}px`, width: `${markW}px`, height: `${markH}px`,
  });
  $('canvasLabel').textContent = canvas === 'native' ? 'V1 native · 240 × 320' : 'Reference concept · 360 × 640';
  $('zoomLabel').textContent = `${zoom * 100}%`;
  $('stageNote').textContent = canvas === 'native'
    ? 'Exact firmware panel size. The record clears every control and stays static.'
    : 'Tall reference canvas, not the V1 panel. The native view is the device-sized layout.';
  $('nativeBtn').classList.toggle('active', canvas === 'native');
  $('referenceBtn').classList.toggle('active', canvas === 'reference');
  $('nativeBtn').setAttribute('aria-pressed', canvas === 'native');
  $('referenceBtn').setAttribute('aria-pressed', canvas === 'reference');
}
function notify(text) {
  clearTimeout(toastTimer); $('toast').textContent = text; $('toast').classList.add('show');
  toastTimer = setTimeout(() => $('toast').classList.remove('show'), 1800);
}
function updateSeek() {
  $('seek').value = progress;
  $('seek').style.background = `linear-gradient(to right, var(--accent) 0 ${progress / 10}%, #555 ${progress / 10}% 100%)`;
}
function togglePlay() {
  playing = !playing;
  $('playBtn').setAttribute('aria-pressed', playing);
  $('playBtn').setAttribute('aria-label', playing ? 'Pause' : 'Play');
  clearInterval(playTimer);
  if (playing) playTimer = setInterval(() => { progress = (progress + 2) % 1001; updateSeek(); }, 500);
}
function toggleFav() {
  favourite = !favourite; $('favBtn').setAttribute('aria-pressed', favourite);
  $('favBtn').setAttribute('aria-label', favourite ? 'Remove from favourites' : 'Add to favourites');
  notify(favourite ? 'Added to favourites' : 'Removed from favourites');
}
function changeTrack(delta) {
  track = (track + delta + tracks.length) % tracks.length;
  $('songTitle').textContent = tracks[track][0]; $('songArtist').textContent = tracks[track][1];
  progress = 0; updateSeek(); notify(`Track ${track + 1} of ${tracks.length}`);
}
function toggleArt() {
  missingArt = !missingArt;
  $('sleeve').querySelector('img').hidden = missingArt;
  $('sleeve').querySelector('.art-fallback').hidden = !missingArt;
  $('artToggle').textContent = missingArt ? '▣ Show artwork' : '▣ Missing art';
}
$('playBtn').onclick = togglePlay; $('playOutside').onclick = togglePlay;
$('favBtn').onclick = toggleFav; $('favOutside').onclick = toggleFav;
$('prevBtn').onclick = () => changeTrack(-1); $('nextBtn').onclick = () => changeTrack(1);
$('seek').oninput = (e) => { progress = Number(e.target.value); updateSeek(); };
$('repeatBtn').onclick = () => {
  repeatMode = (repeatMode + 1) % 3; $('repeatBtn').dataset.mode = repeatMode;
  const names = ['Repeat off', 'Repeat all', 'Repeat one'];
  $('repeatBtn').setAttribute('aria-label', names[repeatMode]); notify(names[repeatMode]);
};
$('moreBtn').onclick = () => notify('On the V1: Music → Display options → Vinyl');
$('backBtn').onclick = () => notify('Swipe right to return to your library');
$('artToggle').onclick = toggleArt;
$('nativeBtn').onclick = () => { canvas = 'native'; zoom = 1; $('zoomSelect').value = '1'; renderGeometry(); };
$('referenceBtn').onclick = () => { canvas = 'reference'; zoom = 1; $('zoomSelect').value = '1'; renderGeometry(); };
$('zoomSelect').onchange = (e) => { zoom = Number(e.target.value); renderGeometry(); };

/* Export the screen itself at exact canvas pixels, without the surrounding
 * studio chrome or display scaling. Drawn locally; the cropped demo art is
 * same-origin, so the canvas is not tainted. */
$('exportBtn').onclick = async () => {
  const w = canvas === 'native' ? 240 : 360, h = canvas === 'native' ? 320 : 640;
  const b = layoutBox(w, h, 4 + DEMO_INFO_H + 4 + 10 + 4 + 44 + 4 + 2);
  const c = document.createElement('canvas'); c.width = w; c.height = h;
  const ctx = c.getContext('2d'); ctx.fillStyle = '#000'; ctx.fillRect(0, 0, w, h);
  const circle = (x, y, d, fill, stroke, lw = 2) => {
    ctx.beginPath(); ctx.arc(x + d / 2, y + d / 2, d / 2 - lw / 2, 0, Math.PI * 2);
    if (fill) { ctx.fillStyle = fill; ctx.fill(); }
    if (stroke) { ctx.strokeStyle = stroke; ctx.lineWidth = lw; ctx.stroke(); }
  };
  circle(b.recordX, b.recordY, b.d, '#000', '#fff');
  const grooveColors = ['#5a5a5a', '#4a4a4a', '#3a3a3a'];
  [0, 1, 2].forEach(i => { let gd = b.d - Math.floor(b.d * (12 + i * 10) / 100); gd &= ~1; if (gd < 4) gd = 4; circle(b.recordX + (b.d - gd) / 2, b.recordY + (b.d - gd) / 2, gd, null, grooveColors[i], 2); });
  const ld = Math.floor(b.d * .28); circle(b.recordX + (b.d - ld) / 2, b.recordY + (b.d - ld) / 2, ld, '#000', '#fff');
  const markW = Math.max(1, Math.round(b.d / 173)), markH = Math.max(2, Math.floor(b.d * .18));
  const markX1 = Math.floor(b.d * .56), markY1 = Math.floor(b.d * .71), markDx = Math.max(1, Math.floor(b.d * .05));
  ctx.save(); ctx.translate(b.recordX + 2 + markX1 + Math.floor(markDx / 2) - Math.floor(markW / 2) + markW / 2, b.recordY + 2 + markY1 + markH / 2);
  ctx.rotate(-15 * Math.PI / 180); ctx.fillStyle = '#fff'; ctx.beginPath(); ctx.roundRect(-markW / 2, -markH / 2, markW, markH, markW / 2); ctx.fill(); ctx.restore();
  ctx.save(); ctx.beginPath(); ctx.roundRect(b.artX, b.artY, b.side, b.side, 12); ctx.clip();
  if (!missingArt) { const img = $('sleeve').querySelector('img'); await img.decode(); ctx.drawImage(img, b.artX + 2, b.artY + 2, b.side - 4, b.side - 4); }
  else { ctx.fillStyle = '#17171b'; ctx.fillRect(b.artX, b.artY, b.side, b.side); ctx.fillStyle = '#8b8b95'; ctx.font = '62px Arial'; ctx.textAlign = 'center'; ctx.fillText('♪', w / 2, b.artY + b.side / 2 + 20); }
  ctx.restore(); ctx.beginPath(); ctx.roundRect(b.artX + 1, b.artY + 1, b.side - 2, b.side - 2, 11); ctx.strokeStyle = '#fff'; ctx.lineWidth = 2; ctx.stroke();
  ctx.textAlign = 'left'; ctx.fillStyle = '#fff'; ctx.font = '15px Arial'; ctx.fillText($('songTitle').textContent, 12, b.deckY + 17, w - 124);
  ctx.fillStyle = '#c7c7cf'; ctx.font = '12px Arial'; ctx.fillText($('songArtist').textContent, 12, b.deckY + 33, w - 124);
  ctx.textAlign = 'right'; ctx.fillStyle = '#bdbdc6'; ctx.font = '10px Arial'; ctx.fillText('16/44.1 FLAC', w - 12, b.deckY + 38);
  const sy = b.deckY + 47; ctx.fillStyle = '#555'; ctx.fillRect(12, sy, w - 24, 4); ctx.fillStyle = '#ff5a4e'; ctx.fillRect(12, sy, (w - 24) * progress / 1000, 4);
  circle(12 + (w - 24) * progress / 1000 - 5, sy - 3, 10, '#ff5a4e', '#fff');
  const ty = h - 28;
  const tri = (x, y, size, flip, color) => { ctx.save(); ctx.translate(x,y); if(flip) ctx.scale(-1,1); ctx.fillStyle=color; ctx.beginPath(); ctx.moveTo(-size/2,-size/2); ctx.lineTo(size/2,0); ctx.lineTo(-size/2,size/2); ctx.closePath(); ctx.fill(); ctx.restore(); };
  ctx.fillStyle='#fff'; ctx.fillRect(w/2-65,ty-10,3,20); tri(w/2-51,ty,22,true,'#fff');
  ctx.fillRect(w/2+62,ty-10,3,20); tri(w/2+51,ty,22,false,'#fff');
  if(playing){ ctx.fillStyle='#ff5a4e'; ctx.fillRect(w/2-11,ty-13,8,26); ctx.fillRect(w/2+3,ty-13,8,26); }
  else tri(w/2+2,ty,30,false,'#ff5a4e');
  ctx.strokeStyle='#92929c'; ctx.lineWidth=2; ctx.lineCap='round';
  ctx.beginPath(); ctx.moveTo(14,ty-5); ctx.lineTo(28,ty-5); ctx.lineTo(24,ty-9); ctx.moveTo(28,ty+5); ctx.lineTo(14,ty+5); ctx.lineTo(18,ty+9); ctx.stroke();
  ctx.fillStyle='#92929c'; [ty-8,ty,ty+8].forEach(y=>{ctx.beginPath();ctx.arc(w-22,y,2,0,Math.PI*2);ctx.fill();});
  if(favourite){ ctx.fillStyle='#ffd66b'; ctx.font='20px Arial'; ctx.textAlign='right'; ctx.fillText('★',w-16,b.deckY+24); }
  ctx.strokeStyle = '#fff'; ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(23, 14); ctx.lineTo(15, 22); ctx.lineTo(23, 30); ctx.stroke();
  const a = document.createElement('a'); a.download = `sonix-vinyl-${w}x${h}.png`; a.href = c.toDataURL('image/png'); a.click();
  notify(`Saved ${w} × ${h} PNG`);
};
renderGeometry(); updateSeek();
