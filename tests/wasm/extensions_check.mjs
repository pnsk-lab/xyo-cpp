// xyo-cpp: Emscripten(Node) ビルドで WASM 拡張を実行する検証。
//
//   node tests/wasm/extensions_check.mjs build-wasm-ext/xyo.js
//
// 前提: headless Emscripten ビルド（-sNODERAWFS）と
//       examples/extensions/wasm_demo が存在すること。
import { spawnSync } from 'node:child_process';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '../..');
const js = process.argv[2];
if (!js) {
  console.error('usage: node extensions_check.mjs <build-wasm-ext/xyo.js>');
  process.exit(2);
}
const fixture = resolve(root, 'tests/fixtures/extensions/wasm_demo.sb3');
const env = {
  ...process.env,
  XYO_EXTENSION_PATH: resolve(root, 'examples/extensions'),
};
const result = spawnSync(
    process.execPath,
    [resolve(js), 'run', fixture, '--print-list', 'out', '--frames', '3'],
    { env, encoding: 'utf8' });
process.stdout.write(result.stdout || '');
process.stderr.write(result.stderr || '');
const lines = (result.stdout || '').trim().split(/\r?\n/).filter(Boolean);
const ok = result.status === 0 &&
    lines.join('|') === '42|3|hello from wasm|6';
console.log(ok ? 'PASS' : 'FAIL');
process.exit(ok ? 0 : 1);
