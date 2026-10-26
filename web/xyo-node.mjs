// xyo-cpp: Node 用ローダ（MODULARIZE 形式 = XYO_WASM_JIT ビルド）。
//
// ランタイムは -sIMPORTED_MEMORY なので、WebAssembly.Memory を JS が
// 生成して渡す。この Memory を実行時生成モジュール（wasm JIT）も
// import するため、生成コードとランタイムが同じ線形メモリを共有する。
//
//   node web/xyo-node.mjs build-wasm-jit/xyo.js wasm-selftest
//   node web/xyo-node.mjs build-wasm-jit/xyo.js run project.sb3 --frames 60
import { createRequire } from 'node:module';
import { resolve } from 'node:path';

const require = createRequire(import.meta.url);

const jsPath = process.argv[2];
const args = process.argv.slice(3);
if (!jsPath) {
  console.error('usage: node web/xyo-node.mjs <xyo.js> [xyo args...]');
  process.exit(2);
}

// require はローダからの相対で解決されるため、cwd 基準の絶対パスにする。
const createXyo = require(resolve(process.cwd(), jsPath));
// -sMAXIMUM_MEMORY=2147483648 に合わせる（32768 ページ = 2GiB）。
const memory = new WebAssembly.Memory({ initial: 256, maximum: 32768 });

try {
  await createXyo({ wasmMemory: memory, arguments: args });
} catch (e) {
  // -sEXIT_RUNTIME=1 では main 復帰時に ExitStatus が throw される。
  if (e && e.name === 'ExitStatus') {
    process.exitCode = e.status | 0;
  } else {
    throw e;
  }
}
