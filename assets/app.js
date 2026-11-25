(() => {
  const api = 'https://api.github.com/repos/mr-f0xx/sonix-player-tempotec-v1/releases/latest';
  const releases = 'https://github.com/mr-f0xx/sonix-player-tempotec-v1/releases/latest';
  const version = document.querySelector('#release-version');
  const date = document.querySelector('#release-date');
  const notes = document.querySelector('#release-notes');
  const button = document.querySelector('#download-button');
  const releaseLink = document.querySelector('#release-link');

  // Release notes are untrusted Markdown: extract a readable short preview, never HTML.
  function highlights(body) {
    const lines = (body || '').split(/\r?\n/).map(line => line.trim())
      .filter(line => line && !/^#{1,6}\s/.test(line) && !/^</.test(line));
    return lines.slice(0, 2).map(line => line
      .replace(/^[-*+]\s+/, '').replace(/\[([^\]]+)\]\([^)]*\)/g, '$1')
      .replace(/\s+by @\S+ in https?:\/\/\S+.*$/, '')
      .replace(/[*_`#]/g, '').trim()).filter(Boolean).join(' · ') || 'See the release on GitHub for full details.';
  }

  fetch(api, { headers: { Accept: 'application/vnd.github+json' } })
    .then(response => { if (!response.ok) throw new Error('Release unavailable'); return response.json(); })
    .then(release => {
      version.textContent = release.tag_name || 'Latest release';
      const published = new Date(release.published_at);
      date.textContent = release.published_at && !Number.isNaN(published.valueOf())
        ? new Intl.DateTimeFormat('en', { year: 'numeric', month: 'short', day: 'numeric', timeZone: 'UTC' }).format(published) : '—';
      notes.textContent = highlights(release.body);
      if (release.html_url && release.html_url.startsWith('https://github.com/')) releaseLink.href = release.html_url;
      const asset = (release.assets || []).find(item => item.name.toLowerCase() === 'v1.upt') ||
        (release.assets || []).find(item => item.name.toLowerCase().endsWith('.upt'));
      if (asset && asset.browser_download_url && asset.browser_download_url.startsWith('https://github.com/')) {
        button.href = asset.browser_download_url;
        button.querySelector('span').textContent = 'DOWNLOAD ' + asset.name.toUpperCase();
      } else {
        button.querySelector('span').textContent = 'VIEW LATEST RELEASE';
        notes.textContent += ' No .upt asset found; check the release page.';
      }
    })
    .catch(() => {
      version.textContent = 'Release unavailable';
      date.textContent = '—';
      notes.textContent = 'Could not load release details. Visit GitHub to download the firmware.';
      button.href = releases;
      button.querySelector('span').textContent = 'VIEW LATEST RELEASE';
    });
})();
