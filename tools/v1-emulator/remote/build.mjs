import { build } from 'esbuild';
import { readdir, readFile } from 'node:fs/promises';

const licenses = await Promise.all((await readdir('node_modules/@yume-chan')).sort().map(async name =>
  `@yume-chan/${name}\n${await readFile(`node_modules/@yume-chan/${name}/LICENSE`, 'utf8')}`));
await build({ entryPoints: ['usb.js'], bundle: true, minify: true, format: 'esm',
  target: 'chrome100', outfile: '../public/remote-usb.bundle.js',
  banner: { js: `/*!\n${licenses.join('\n').replaceAll('*/', '* /')}\n*/` } });
