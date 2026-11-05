import { copyFile, mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve, join } from 'node:path';

const root = resolve(import.meta.dirname, '..');
const source = resolve(root, 'native/.llvm-work/wasm/bin');
const destination = resolve(root, 'public/llvm-assets');
await mkdir(destination, { recursive: true });

for (const [tool, name] of [['llc', 'llc-browser'], ['lld', 'lld-browser']]) {
  const script = await readFile(join(source, tool + '.js'), 'utf8');
  if (!script.includes(tool + '.wasm')) throw Error('unknown LLVM Emscripten module naming convention');
  const renamed = script.replaceAll(tool + '.wasm', name + '.wasm');
  await writeFile(join(destination, name + '.mjs'), renamed);
  await copyFile(join(source, tool + '.wasm'), join(destination, name + '.wasm'));
  console.log('installed', name);
}
