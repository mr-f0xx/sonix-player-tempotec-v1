import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const source = await readFile(new URL('../../../assets/download-count.js', import.meta.url), 'utf8');
async function run(fetch) {
  const elements = Object.fromEntries(['download-count', 'download-count-value', 'download-count-snapshot', 'download-count-date'].map(id => ['#' + id, { hidden: true, textContent: '' }]));
  const document = { documentElement: { lang: 'en' }, querySelector: s => elements[s], addEventListener() {} };
  vm.runInNewContext(source, { document, fetch, Intl, Number, Date });
  await new Promise(resolve => setTimeout(resolve, 30));
  return elements;
}

test('displays a dated snapshot when the GitHub API fails', async () => {
  const elements = await run(async url => url.startsWith('./')
    ? { ok: true, json: async () => ({ count: 2420, asOf: '2026-11-28T08:42:06Z' }) }
    : { ok: false });
  assert.equal(elements['#download-count'].hidden, false);
  assert.equal(elements['#download-count-value'].textContent, '2,420');
  assert.equal(elements['#download-count-snapshot'].hidden, false);
  assert.equal(elements['#download-count-date'].textContent, 'Nov 28, 2026');
});

test('a slow snapshot never overwrites the live total', async () => {
  const elements = await run(async url => {
    if (url.startsWith('./')) {
      await new Promise(resolve => setTimeout(resolve, 10));
      return { ok: true, json: async () => ({ count: 5, asOf: '2026-11-28T08:42:06Z' }) };
    }
    return { ok: true, json: async () => [{ assets: [{ name: 'v1.upt', download_count: 7 }] }] };
  });
  assert.equal(elements['#download-count-value'].textContent, '7');
  assert.equal(elements['#download-count-snapshot'].hidden, true);
});
