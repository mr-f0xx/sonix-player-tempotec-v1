(() => {
  const badge = document.querySelector('#download-count');
  const value = document.querySelector('#download-count-value');
  if (!badge || !value) return;

  // Count only currently published firmware assets, not themes, checksums,
  // source archives, or individual people. A replaced asset loses its old count.
  async function loadCount() {
    const endpoint = 'https://api.github.com/repos/mr-f0xx/sonix-player-tempotec-v1/releases';
    let page = 1;
    let total = 0;
    let firmwareAssets = 0;
    while (true) {
      const response = await fetch(`${endpoint}?per_page=100&page=${page}`, {
        headers: { Accept: 'application/vnd.github+json' },
      });
      if (!response.ok) throw new Error('Release counts unavailable');
      const releases = await response.json();
      if (!Array.isArray(releases)) throw new Error('Invalid release list');
      for (const release of releases) {
        if (release.draft) continue;
        if (!Array.isArray(release.assets)) throw new Error('Invalid release assets');
        for (const asset of release.assets) {
          if (!/\.upt$/i.test(asset.name || '')) continue;
          if (!Number.isSafeInteger(asset.download_count) || asset.download_count < 0) throw new Error('Invalid download count');
          total += asset.download_count;
          firmwareAssets++;
        }
      }
      if (releases.length < 100) break;
      page++;
    }
    if (!firmwareAssets || !Number.isSafeInteger(total)) throw new Error('No firmware assets');
    return total;
  }

  const snapshotLabel = document.querySelector('#download-count-snapshot');
  const snapshotDate = document.querySelector('#download-count-date');
  let count;
  let asOf;
  let liveLoaded = false;
  function render() {
    if (count === undefined) return;
    const locale = document.documentElement.lang || 'en';
    value.textContent = new Intl.NumberFormat(locale).format(count);
    snapshotLabel.hidden = !asOf;
    if (asOf) {
      snapshotDate.dateTime = asOf;
      snapshotDate.textContent = new Intl.DateTimeFormat(locale, { year: 'numeric', month: 'short', day: 'numeric', timeZone: 'UTC' }).format(new Date(asOf));
    }
    badge.hidden = false;
  }
  document.addEventListener('sonix:languagechange', render);

  // A dated same-origin snapshot remains useful when GitHub is blocked or
  // rate-limits the visitor. Never overwrite a live result with the snapshot.
  fetch('./assets/download-count.json', { cache: 'no-cache' })
    .then(response => { if (!response.ok) throw new Error('Snapshot unavailable'); return response.json(); })
    .then(snapshot => {
      if (liveLoaded || !Number.isSafeInteger(snapshot.count) || snapshot.count < 0 ||
          typeof snapshot.asOf !== 'string' || !Number.isFinite(Date.parse(snapshot.asOf))) return;
      count = snapshot.count;
      asOf = snapshot.asOf;
      render();
    }).catch(() => {});
  loadCount().then(result => { liveLoaded = true; count = result; asOf = undefined; render(); }).catch(() => {});
})();
