import { copyFile, mkdir, stat } from 'node:fs/promises';
import { resolve, join } from 'node:path';

const editor = resolve(process.argv[2] ?? '../xyo-editor');
const source = join(editor, 'packages/llvm-wasm/.work');
const destination = resolve(import.meta.dirname, '..', 'public/llvm-assets');
const files = [
  ['llvm19-browser-proof/llc-browser.mjs', 'llc-browser.mjs'],
  ['llvm19-browser-proof/llc-browser.wasm', 'llc-browser.wasm'],
  ['llvm19-lld-browser-proof/lld-browser.mjs', 'lld-browser.mjs'],
  ['llvm19-lld-browser-proof/lld-browser.wasm', 'lld-browser.wasm'],
];
await mkdir(destination, { recursive: true });
for (const [src, dst] of files) {
  const from = join(source, src);
  await stat(from);
  await copyFile(from, join(destination, dst));
  console.log(dst);
}
