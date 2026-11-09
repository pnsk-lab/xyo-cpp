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

const options = { wasmMemory: memory, arguments: args };

// Optional Binaryen optimization for generated HotSpot WASM.
// Use either Binaryen's npm module or the local N-API adapter built from
// web/binaryen-node. Both run synchronously and return actual optimized bytes.
if (process.env.XYO_BINARYEN_JS === '1' &&
    process.env.XYO_BINARYEN_NODE_ADDON) {
  throw new Error('Choose either XYO_BINARYEN_JS or XYO_BINARYEN_NODE_ADDON');
}
if (process.env.XYO_BINARYEN_NODE_ADDON) {
  const addon = require(resolve(
    process.cwd(), process.env.XYO_BINARYEN_NODE_ADDON));
  if (!addon.available()) {
    throw new Error('Binaryen N-API adapter could not load libbinaryen.so');
  }
  const levelText = process.env.XYO_BINARYEN_OPT_LEVEL || '2';
  if (!/^[0-3]$/.test(levelText))
    throw new Error('XYO_BINARYEN_OPT_LEVEL must be 0..3');
  let count = 0;
  options.xyoOptimizeWasm = bytes => {
    if (!WebAssembly.validate(bytes)) {
      throw new Error('refusing to optimize invalid WebAssembly');
    }
    const result = addon.optimize(Buffer.from(bytes), Number(levelText));
    if (!WebAssembly.validate(result)) {
      throw new Error('Binaryen N-API produced invalid WebAssembly');
    }
    ++count;
    if (process.env.XYO_BINARYEN_DIAGNOSTICS === '1') {
      console.error('[binaryen-native] optimized #' + count +
        ': ' + bytes.byteLength + ' -> ' + result.byteLength + ' bytes');
    }
    return result;
  };
}
if (process.env.XYO_BINARYEN_JS === '1') {
  const { default: binaryen } = await import('binaryen');
  options.xyoOptimizeWasm = (bytes) => {
    const module = binaryen.readBinary(bytes);
    try {
      module.optimize();
      return module.emitBinary();
    } finally {
      module.dispose();
    }
  };
}
try {
  await createXyo(options);
} catch (e) {
  // -sEXIT_RUNTIME=1 では main 復帰時に ExitStatus が throw される。
  if (e && e.name === 'ExitStatus') {
    process.exitCode = e.status | 0;
  } else {
    throw e;
  }
}
