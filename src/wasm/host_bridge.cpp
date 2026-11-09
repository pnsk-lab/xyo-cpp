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
#include "../ir/execution_abi.hpp"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

EM_JS(int, xyo___has_host_optimizer, (), {
  return typeof Module['xyoOptimizeWasm'] === 'function' ? 1 : 0;
});

EM_JS(int, xyo___instantiate, (const void* ptr, int len, int optimize), {
  try {
    var mods = Module['xyoModules'] || (Module['xyoModules'] = []);
    var mem = Module['wasmMemory'] || (typeof wasmMemory !== 'undefined' ? wasmMemory : null);
    if (!mem) return -1;
    var bytes = new Uint8Array(mem.buffer).slice(ptr, ptr + len);
    if (optimize) {
      var optimizer = Module['xyoOptimizeWasm'];
      if (typeof optimizer !== 'function') return -1;
      var result = optimizer(bytes);
      if (result instanceof Uint8Array) bytes = result;
      else if (result instanceof ArrayBuffer) bytes = new Uint8Array(result);
      else if (ArrayBuffer.isView(result))
        bytes = new Uint8Array(result.buffer, result.byteOffset, result.byteLength);
      else throw new TypeError('xyoOptimizeWasm must return binary bytes');
    }
    var mod = new WebAssembly.Module(bytes);
    var inst = new WebAssembly.Instance(mod, {
      env: {
        memory: mem,
        h: Module['_xyo_wasm_h'],
      },
    });
    var free = Module['xyoFreeModules'];
    if (free && free.length) {
      var handle = free.pop();
      mods[handle] = inst;
      return handle;
    }
    mods.push(inst);
    return mods.length - 1;
  } catch (e) {
    if (Module['printErr'])
      Module['printErr']('xyo: wasm instantiate failed: ' + e);
    return -1;
  }
});

EM_JS(int, xyo___invoke, (int handle, int ctx), {
  try {
    var mods = Module['xyoModules'];
    var inst = mods && mods[handle];
    if (!inst || !inst.exports || typeof inst.exports.f !== 'function') return -1;
    return (inst.exports.f(ctx) | 0);
  } catch (e) {
    if (Module['printErr'])
      Module['printErr']('xyo: wasm invocation failed: ' + e);
    return -1;
  }
});

EM_JS(double, xyo___invoke_numeric, (int handle, int slots, int frame), {
  try {
    var mods = Module['xyoModules'];
    var inst = mods && mods[handle];
    if (!inst || !inst.exports || typeof inst.exports.f !== 'function') return -1;
    var status = inst.exports.f(slots, frame);
    return Number(status);
  } catch (e) {
    if (Module['printErr'])
      Module['printErr']('xyo: numeric CFG invocation failed: ' + e);
    return -1;
  }
});

EM_JS(void, xyo___release, (int handle), {
  var mods = Module['xyoModules'];
  if (mods && handle >= 0 && handle < mods.length) {
    mods[handle] = null;
    var free = Module['xyoFreeModules'] || (Module['xyoFreeModules'] = []);
    free.push(handle);
  }
});

namespace xyo::wasm {

bool hasHostOptimizer() { return xyo___has_host_optimizer() != 0; }

int instantiate(const void* bytes, int len, bool optimizeWithHost) {
    return xyo___instantiate(bytes, len, int(optimizeWithHost));
}

int invoke(int handle, int ctxPtr) { return xyo___invoke(handle, ctxPtr); }

uint64_t invokeNumeric(int handle, int slotsPtr, int framePtr) {
    const double status = xyo___invoke_numeric(handle, slotsPtr, framePtr);
    if (status == 0.0)
        return ir::NumericExecutionAbi::status(ir::NumericRunStatus::Completed);
    if (status == 1.0)
        return ir::NumericExecutionAbi::status(ir::NumericRunStatus::Yielded);
    // Traps, missing module handles, and invalid results all map to the
    // SAME portable rollback code that native LLVM emits for failed
    // resumable CFGs. Nothing has yet been published to Scratch variables.
    return ir::NumericExecutionAbi::status(ir::NumericRunStatus::Failed);
}

void release(int handle) { xyo___release(handle); }

} // namespace xyo::wasm

#else  // !EMSCRIPTEN

namespace xyo::wasm {

bool hasHostOptimizer() { return false; }
int instantiate(const void*, int, bool) { return -1; }
int invoke(int, int) { return -1; }
uint64_t invokeNumeric(int, int, int) { return UINT64_MAX; }

void release(int) {}

} // namespace xyo::wasm

#endif
