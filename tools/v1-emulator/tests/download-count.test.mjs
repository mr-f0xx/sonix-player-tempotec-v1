import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const source = await readFile(new URL('../../../assets/download-count.js', import.meta.url), 'utf8');

async function run(fetch) {
  const badge = { hidden: true };
  const value = { textContent: '' };
  const snapshotLabel = { hidden: true };
  const snapshotDate = { textContent: '' };
  const listeners = {};
  const elements = { '#download-count': badge, '#download-count-value': value, '#download-count-snapshot': snapshotLabel, '#download-count-date': snapshotDate };
  const document = {
    documentElement: { lang: 'en' },
    querySelector: selector => elements[selector],
    addEventListener: (name, callback) => { listeners[name] = callback; },
  };
  vm.runInNewContext(source, { document, fetch, Intl, Number });
  await new Promise(resolve => setTimeout(resolve, 20));
  return { badge, value, snapshotLabel, snapshotDate, document, listeners };
}

test('counts firmware across pages only and formats for the selected language', async () => {
  const first = Array.from({ length: 100 }, () => ({ assets: [] }));
  first[0] = { assets: [{ name: 'v1.upt', download_count: 1234 }, { name: 'themes.zip', download_count: 900 }] };
  let requests = 0;
  const result = await run(async url => {
    if (url.startsWith('./')) return { ok: false };
    return { ok: true, json: async () => {
      requests++;
      assert.match(url, /per_page=100&page=\d+/);
      return requests === 1 ? first : [{ assets: [{ name: 'other.UPT', download_count: 10 }] }];
    } };
  });
  assert.equal(requests, 2);
  assert.equal(result.badge.hidden, false);
  assert.equal(result.value.textContent, '1,244');
  result.document.documentElement.lang = 'de';
  result.listeners['sonix:languagechange']();
  assert.equal(result.value.textContent, '1.244');
});

test('does not show a partial count when a later page fails', async () => {
  let requests = 0;
  const result = await run(async url => url.startsWith('./') ? { ok: false } : ++requests === 1
    ? { ok: true, json: async () => Array.from({ length: 100 }, () => ({ assets: [{ name: 'v1.upt', download_count: 1 }] })) }
    : { ok: false });
  assert.equal(result.badge.hidden, true);
});
