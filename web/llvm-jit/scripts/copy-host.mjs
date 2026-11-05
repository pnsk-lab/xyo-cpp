import { existsSync } from 'node:fs';
import { copyFile, mkdir, stat } from 'node:fs/promises';
import { resolve, join } from 'node:path';

const root = resolve(import.meta.dirname, '..');
const build = resolve(root, 'native/build');
const out = resolve(root, 'public/llvm-assets');
const required = ['xyo_browser_unified.mjs', 'xyo_browser_unified.wasm'];
const optional = ['xyo_browser_ir.mjs', 'xyo_browser_ir.wasm'];
await mkdir(out, { recursive: true });

for (const name of required) {
  const src = join(build, name);
  await stat(src);
  await copyFile(src, join(out, name));
  console.log('copied', name);
}
const available = optional.map(name => existsSync(join(build, name)));
if (available.every(Boolean)) {
  for (const name of optional) {
    await copyFile(join(build, name), join(out, name));
    console.log('copied optional C++ compiler', name);
  }
} else if (available.some(Boolean)) {
  throw Error('incomplete C++ compiler artifact pair: refusing a partial install');
} else {
  console.log('optional xyo_browser_ir compiler not built; restricted TypeScript IR frontend remains available');
}
