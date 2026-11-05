import { copyFile, mkdir, stat } from 'node:fs/promises';
import { resolve, join } from 'node:path';

const root = resolve(import.meta.dirname, '..');
const build = resolve(root, 'native/build');
const out = resolve(root, 'public/llvm-assets');
const files = ['xyo_browser_unified.mjs', 'xyo_browser_unified.wasm'];
await mkdir(out, { recursive: true });
for (const name of files) {
  const source = join(build, name);
  await stat(source);
  await copyFile(source, join(out, name));
  console.log('copied', name);
}
