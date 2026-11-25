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

  function uptAsset(release) {
    const assets = (release && release.assets) || [];
    return assets.find(item => item.name && item.name.toLowerCase() === 'v1.upt') ||
      assets.find(item => item.name && item.name.toLowerCase().endsWith('.upt'));
  }

  let shownRelease = false;

  function render(release) {
    if (!release || typeof release !== 'object') return;
    const asset = uptAsset(release);
    version.textContent = release.tag_name || 'Latest release';

    const published = new Date(release.published_at);
    date.textContent = release.published_at && !Number.isNaN(published.valueOf())
      ? new Intl.DateTimeFormat('en', { year: 'numeric', month: 'short', day: 'numeric', timeZone: 'UTC' }).format(published)
      : '—';

    notes.textContent = highlights(release.body);
    if (release.html_url && release.html_url.startsWith('https://github.com/')) releaseLink.href = release.html_url;

    if (asset && asset.browser_download_url && asset.browser_download_url.startsWith('https://github.com/')) {
      button.href = asset.browser_download_url;
      button.querySelector('span').textContent = 'DOWNLOAD ' + asset.name.toUpperCase();
    } else {
      button.href = release.html_url && release.html_url.startsWith('https://github.com/') ? release.html_url : releases;
      button.querySelector('span').textContent = 'VIEW LATEST RELEASE';
      notes.textContent += ' No .upt asset found; check the release page.';
    }
    shownRelease = true;
  }

  function showUnavailable() {
    if (shownRelease) return;
    version.textContent = 'Release unavailable';
    date.textContent = '—';
    notes.textContent = 'Could not load release details. Visit GitHub to download the firmware.';
    button.href = releases;
    button.querySelector('span').textContent = 'VIEW LATEST RELEASE';
  }

  // The workflow refreshes this JSON at deploy time, so the strip works even
  // when api.github.com is rate-limited or blocked in the visitor's browser.
  const local = fetch('./assets/release.json?t=' + Date.now(), { cache: 'no-store' })
    .then(response => { if (!response.ok) throw new Error('No local release snapshot'); return response.json(); })
    .then(render);

  local
    .catch(() => {})
    .finally(() => {
      fetch(api + '?t=' + Date.now(), {
        headers: { Accept: 'application/vnd.github+json' },
        cache: 'no-store',
      })
        .then(response => { if (!response.ok) throw new Error('Release unavailable'); return response.json(); })
        .then(live => {
          // Never replace a working download with a rate-limit/error payload.
          if (shownRelease && !uptAsset(live)) return;
          render(live);
        })
        .catch(showUnavailable);
    });

  // Simple mobile navigation toggle. The desktop nav stays plain links.
  const navToggle = document.querySelector('#nav-toggle');
  const nav = document.querySelector('#primary-nav');
  if (navToggle && nav) {
    const setOpen = open => {
      nav.classList.toggle('open', open);
      navToggle.setAttribute('aria-expanded', String(open));
      navToggle.querySelector('span').textContent = open ? 'CLOSE' : 'MENU';
    };
    navToggle.addEventListener('click', () => setOpen(!nav.classList.contains('open')));
    nav.addEventListener('click', event => {
      if (event.target.closest('a')) setOpen(false);
    });
  }

  // Cycle the real V1 screenshots inside the device's display area.
  const deviceScreen = document.querySelector('#device-display');
  if (deviceScreen && !window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
    const screens = [
      { src: './screenshots/ember/now-playing.png', label: 'Now playing' },
      { src: './screenshots/ember/main-menu.png', label: 'Main menu' },
      { src: './screenshots/ember/music.png', label: 'Music library' },
      { src: './screenshots/ember/streaming.png', label: 'Streaming' },
      { src: './screenshots/ember/settings.png', label: 'Settings' },
      { src: './screenshots/ember/wireless.png', label: 'Wireless' },
    ];
    screens.forEach(screen => { const image = new Image(); image.src = screen.src; });
    let index = 0;
    window.setInterval(() => {
      deviceScreen.style.opacity = '0';
      window.setTimeout(() => {
        index = (index + 1) % screens.length;
        deviceScreen.src = screens[index].src;
        deviceScreen.alt = 'Sonix Player ' + screens[index].label + ' screen shown on the V1 display';
        deviceScreen.style.opacity = '1';
      }, 280);
    }, 3200);
  }
})();
