// xyo-cpp: スクリプト→wasm コンパイラが生成したモジュールを実行して
// 意味論を検証する。
//
//   XYO_WASM_COMPILE_OUT=/tmp/xyo_compile.wasm ./build/tests/xyo-tests
//   node tests/wasm/compile_check.mjs /tmp/xyo_compile.wasm
//
// 期待値（test_wasm_compile.cpp のプログラム）:
//   v0 = 0; repeat 1000 { v0 = v0 + 1 }; v1 = v0*2; v2 = 7-2; v3 = 10/4
//   → 1000, 2000, 5, 2.5
import { readFileSync } from 'node:fs';

const path = process.argv[2];
if (!path) {
  console.error('usage: node compile_check.mjs <module.wasm>');
  process.exit(2);
}
const bytes = readFileSync(path);
const memory = new WebAssembly.Memory({ initial: 256, maximum: 32768 });
const { instance } = await WebAssembly.instantiate(bytes, {
  env: { memory },
});

// slots: i64[4] を線形メモリ上に確保（先頭 64 バイトを使う）
const SLOTS = 64;
const ret = instance.exports.f(SLOTS);
const view = new BigInt64Array(memory.buffer, SLOTS, 4);
const nums = Array.from(view, (b) => new Float64Array(new BigInt64Array([b]).buffer)[0]);

console.log(`bytes=${bytes.length} ret=${ret}`);
console.log(`v0=${nums[0]} v1=${nums[1]} v2=${nums[2]} v3=${nums[3]}`);

const ok =
  ret === 0 &&
  nums[0] === 1000 &&
  nums[1] === 2000 &&
  nums[2] === 5 &&
  nums[3] === 2.5;
console.log(ok ? 'PASS' : 'FAIL');
process.exit(ok ? 0 : 1);
