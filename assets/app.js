(() => {
  const api = 'https://api.github.com/repos/mr-f0xx/sonix-player-tempotec-v1/releases/latest';
  const releases = 'https://github.com/mr-f0xx/sonix-player-tempotec-v1/releases/latest';
  const version = document.querySelector('#release-version');
  const date = document.querySelector('#release-date');
  const notes = document.querySelector('#release-notes');
  const button = document.querySelector('#download-button');
  const releaseLink = document.querySelector('#release-link');
  const themeButton = document.querySelector('#theme-button');

  // Release notes are untrusted Markdown: extract a readable short preview, never HTML.
  function highlights(body) {
    const lines = (body || '').split(/\r?\n/).map(line => line.trim())
      .filter(line => line && !/^#{1,6}\s/.test(line) && !/^</.test(line));
    return lines.slice(0, 2).map(line => line
      .replace(/^[-*+]\s+/, '').replace(/\[([^\]]+)\]\([^)]*\)/g, '$1')
      .replace(/\s+by @\S+ in https?:\/\/\S+.*$/, '')
      .replace(/[*_`#]/g, '').trim()).filter(Boolean).join(' · ') || window.sonixI18n.t('rel_notes_external');
  }

  function uptAsset(release) {
    const assets = (release && release.assets) || [];
    return assets.find(item => item.name && item.name.toLowerCase() === 'v1.upt') ||
      assets.find(item => item.name && item.name.toLowerCase().endsWith('.upt'));
  }

  function themeAsset(release) {
    const assets = (release && release.assets) || [];
    return assets.find(item => /icon.*theme|theme.*icon/i.test(item.name || '')) ||
      assets.find(item => (item.name || '').toLowerCase().endsWith('.zip') && !/sonix-web/i.test(item.name));
  }

  let shownRelease = false;
  let lastRelease = null;
  let unavailable = false;

  function render(release) {
    if (!release || typeof release !== 'object') return;
    lastRelease = release;
    unavailable = false;
    const asset = uptAsset(release);
    version.textContent = release.tag_name || 'Latest release';

    const published = new Date(release.published_at);
    date.textContent = release.published_at && !Number.isNaN(published.valueOf())
      ? new Intl.DateTimeFormat(document.documentElement.lang || 'en', { year: 'numeric', month: 'short', day: 'numeric', timeZone: 'UTC' }).format(published)
      : '—';

    notes.textContent = highlights(release.body);
    if (release.html_url && release.html_url.startsWith('https://github.com/')) releaseLink.href = release.html_url;
    const themes = themeAsset(release);
    if (themeButton) {
      themeButton.href = themes && themes.browser_download_url && themes.browser_download_url.startsWith('https://github.com/')
        ? themes.browser_download_url
        : './assets/sonix-themes-icons.zip';
    }

    if (asset && asset.browser_download_url && asset.browser_download_url.startsWith('https://github.com/')) {
      button.href = asset.browser_download_url;
      button.querySelector('span').textContent = window.sonixI18n.t('btn_download') + ' ' + asset.name.toUpperCase();
    } else {
      button.href = release.html_url && release.html_url.startsWith('https://github.com/') ? release.html_url : releases;
      button.querySelector('span').textContent = window.sonixI18n.t('view_release').replace(/<[^>]*>/g, '');
      notes.textContent += window.sonixI18n.t('rel_no_asset');
    }
    shownRelease = true;
  }

  function showUnavailable() {
    if (shownRelease) return;
    unavailable = true;
    version.textContent = window.sonixI18n.t('rel_unavailable');
    date.textContent = '—';
    notes.textContent = window.sonixI18n.t('rel_failed');
    button.href = releases;
    button.querySelector('span').textContent = window.sonixI18n.t('view_release').replace(/<[^>]*>/g, '');
  }

  function refreshLanguage() {
    if (lastRelease) render(lastRelease);
    else if (unavailable) {
      shownRelease = false;
      showUnavailable();
    }
  }

  document.addEventListener('sonix:languagechange', refreshLanguage);
  refreshLanguage();

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
      navToggle.querySelector('span').textContent = window.sonixI18n.t(open ? 'nav_close' : 'nav_menu');
    };
    navToggle.addEventListener('click', () => setOpen(!nav.classList.contains('open')));
    nav.addEventListener('click', event => {
      if (event.target.closest('a')) setOpen(false);
    });
  }

  // Crossfade real theme screenshots inside the device's display area.
  // The hidden layer is filled with the *upcoming* image while it is still
  // invisible, so a fade can never reveal a stale now-playing frame first.
  const frontLayer = document.querySelector('#device-display');
  const backLayer = document.querySelector('#device-display-next');
  if (frontLayer && backLayer && !window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
    const screens = [
      { src: './screenshots/ember/now-playing.png', label: 'Amber · Now playing' },
      { src: './screenshots/tokyonight/now-playing.png', label: 'Tokyo Night · Now playing' },
      { src: './screenshots/ember/main-menu.png', label: 'Amber · Main menu' },
      { src: './screenshots/tokyonight/main-menu.png', label: 'Tokyo Night · Main menu' },
      { src: './screenshots/ember/music.png', label: 'Amber · Music library' },
      { src: './screenshots/tokyonight/music.png', label: 'Tokyo Night · Music library' },
      { src: './screenshots/ember/settings.png', label: 'Amber · Settings' },
      { src: './screenshots/tokyonight/settings.png', label: 'Tokyo Night · Settings' },
    ];
    const HOLD_MS = 8000;
    const FADE_MS = 1400;

    // Warm the browser cache before the first swap.
    const warm = new Map();
    screens.forEach(screen => {
      const image = new Image();
      image.src = screen.src;
      warm.set(screen.src, image.decode ? image.decode().catch(() => {}) : Promise.resolve());
    });

    const wait = ms => new Promise(resolve => window.setTimeout(resolve, ms));

    let front = frontLayer;
    let back = backLayer;
    let index = 0;
    let nextIndex = 1;
    let fading = false;

    async function prepare(layer, position) {
      const screen = screens[position];
      layer.src = screen.src;
      layer.alt = 'Sonix Player ' + screen.label + ' screen shown on the V1 display';
      try {
        if (layer.decode) await layer.decode();
        else await (warm.get(screen.src) || Promise.resolve());
      } catch (_) {}
      // The layer is still transparent here; the next screen is ready to fade in.
    }

    async function advance() {
      if (fading) return;
      fading = true;

      if (document.hidden) {
        fading = false;
        window.setTimeout(advance, HOLD_MS);
        return;
      }

      back.setAttribute('aria-hidden', 'false');
      front.setAttribute('aria-hidden', 'true');
      back.style.opacity = '1';
      front.style.opacity = '0';
      [front, back] = [back, front];
      index = nextIndex;
      nextIndex = (index + 1) % screens.length;

      await wait(FADE_MS);
      back.setAttribute('aria-hidden', 'true');
      await prepare(back, nextIndex);
      fading = false;
      window.setTimeout(advance, HOLD_MS);
    }

    prepare(back, nextIndex).then(() => window.setTimeout(advance, HOLD_MS));
  }
})();
