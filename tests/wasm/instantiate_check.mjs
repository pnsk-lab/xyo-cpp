// xyo-cpp: wasm エンコーダ（src/wasm/wasm_emit.hpp）が生成した
// モジュールを実際に instantiate して動かす検証。
//
//   1. `XYO_WASM_EMIT_OUT=/tmp/xyo_emit_b.wasm ./build/tests/xyo-tests`
//      で生成物を書き出す
//   2. `node tests/wasm/instantiate_check.mjs /tmp/xyo_emit_b.wasm`
//
// 検証内容: 生成モジュールが import した memory を共有し、import した
// ヘルパを呼び、共有メモリへ書き戻せること（= ランタイムから見た
// "生成コードが自分のデータを読み書きし、自分のヘルパを呼ぶ" 経路）。
import { readFileSync } from 'node:fs';

const path = process.argv[2];
if (!path) {
  console.error('usage: node instantiate_check.mjs <module.wasm>');
  process.exit(2);
}
const bytes = readFileSync(path);
console.log(`module: ${path} (${bytes.length} bytes)`);

// ランタイム側の共有メモリ（xyo は -sIMPORTED_MEMORY で JS 提供のものを使う）
const memory = new WebAssembly.Memory({ initial: 256, maximum: 32768 });
// ランタイム側ヘルパ（実際は Emscripten の exported function を渡す）
const helper_add = (x) => (x + 100) | 0;

const { instance } = await WebAssembly.instantiate(bytes, {
  env: { memory, helper_add },
});

const mem = new Uint8Array(memory.buffer);
const addr = 4;
const ret = instance.exports.f(addr);
const stored = new Int32Array(memory.buffer)[addr >> 2];

console.log(`f(${addr}) = ${ret}, mem[${addr}] = ${stored}`);
const ok = ret === 104 && stored === 104;
console.log(ok ? 'PASS' : 'FAIL');
process.exit(ok ? 0 : 1);
