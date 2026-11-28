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

  let count;
  function render() {
    if (count === undefined) return;
    value.textContent = new Intl.NumberFormat(document.documentElement.lang || 'en').format(count);
    badge.hidden = false;
  }
  document.addEventListener('sonix:languagechange', render);
  // Hide rather than claim an inaccurate or partial total when GitHub is offline
  // or rate-limits a visitor. The rest of the page works without this request.
  loadCount().then(result => { count = result; render(); }).catch(() => {});
})();
