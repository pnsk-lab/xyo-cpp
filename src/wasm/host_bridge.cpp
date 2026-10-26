// xyo-cpp: wasm JIT のホストブリッジ実装（EM_JS）。
//
// 生成モジュールは次の import を期待する:
//   env.memory : ランタイムの WebAssembly.Memory（-sIMPORTED_MEMORY で JS 提供）
//   env.h      : ヘルパディスパッチャ (i32 id, i32 a, i32 b) -> i32
//                 実体はランタイムの xyo_wasm_h（-sEXPORTED_FUNCTIONS で公開）
//
// これにより生成コードは「ランタイムのメモリを直接読み書き」し
// 「ランタイムのヘルパを呼び戻す」ことができる。yield しないスクリプト
// だけを対象にするため、中断・再開は不要（1 回の呼び出しで完走する）。
#include "host_bridge.hpp"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

EM_JS(int, xyo___instantiate, (const void* ptr, int len), {
  try {
    var mods = Module['xyoModules'] || (Module['xyoModules'] = []);
    var mem = Module['wasmMemory'] || (typeof wasmMemory !== 'undefined' ? wasmMemory : null);
    if (!mem) return -1;
    var bytes = new Uint8Array(mem.buffer).slice(ptr, ptr + len);
    var mod = new WebAssembly.Module(bytes);
    var inst = new WebAssembly.Instance(mod, {
      env: {
        memory: mem,
        h: Module['_xyo_wasm_h'],
      },
    });
    mods.push(inst);
    return mods.length - 1;
  } catch (e) {
    if (Module['printErr'])
      Module['printErr']('xyo: wasm instantiate failed: ' + e);
    return -1;
  }
});

EM_JS(void, xyo___invoke, (int handle, int ctx), {
  var mods = Module['xyoModules'];
  if (!mods) return;
  var inst = mods[handle];
  if (inst && inst.exports && inst.exports.f) inst.exports.f(ctx);
});

namespace xyo::wasm {

int instantiate(const void* bytes, int len) {
    return xyo___instantiate(bytes, len);
}

void invoke(int handle, int ctxPtr) { xyo___invoke(handle, ctxPtr); }

} // namespace xyo::wasm

#else  // !EMSCRIPTEN

namespace xyo::wasm {

int instantiate(const void*, int) { return -1; }
void invoke(int, int) {}

} // namespace xyo::wasm

#endif
