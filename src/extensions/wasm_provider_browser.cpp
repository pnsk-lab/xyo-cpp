// xyo-cpp: ブラウザ / Emscripten 向け WASM 拡張プロバイダ。
//
// ネイティブの wasm3 プロバイダ（wasm_provider.cpp）と同じ ABI を、
// ブラウザの WebAssembly API で実装する。拡張モジュールは xyo の線形メモリ
// とは別の Memory を持ち、引数・戻り値は JS 側でコピーする。
#include "wasm_provider.hpp"

#if defined(__EMSCRIPTEN__)

#include <emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../core/cast.hpp"
#include "../core/fileio.hpp"
#include "../core/sstr.hpp"
#include "../model/blocks.hpp"

namespace xyo {
namespace {

struct WasmArg {
    uint32_t tag;
    double num;
    uint32_t str_ptr;
    uint32_t str_len;
};
static_assert(sizeof(WasmArg) == 24, "WasmArg ABI size");

EM_JS(int, xyo_ext_js_create, (const void* bytes, int len), {
  try {
    var copy = new Uint8Array(HEAPU8.buffer, bytes, len).slice();
    var mod = new WebAssembly.Module(copy);
    var state = { instance: null, args: [], nargs: 0 };
    var env = {
      xyo_arg_count: function() { return state.nargs | 0; },
      xyo_arg_tag: function(i) {
        var a = state.args[i];
        return a ? (a.tag | 0) : 0;
      },
      xyo_arg_num: function(i) {
        var a = state.args[i];
        return a ? +a.num : 0;
      },
      xyo_arg_str_copy: function(i, dst, cap) {
        var a = state.args[i];
        if (!a || a.tag !== 2 || !state.instance) return 0;
        var mem = new Uint8Array(state.instance.exports.memory.buffer);
        var n = Math.min(cap | 0, a.bytes ? a.bytes.length : 0);
        if (dst < 0 || dst + n > mem.length) return 0;
        if (n) mem.set(a.bytes.subarray(0, n), dst);
        return n | 0;
      },
      xyo_log: function(ptr, len) {
        if (!state.instance) return;
        var mem = new Uint8Array(state.instance.exports.memory.buffer);
        if (ptr >= 0 && len >= 0 && ptr + len <= mem.length) {
          var s = new TextDecoder().decode(mem.subarray(ptr, ptr + len));
          console.error('[ext] ' + s);
        }
      }
    };
    var inst = new WebAssembly.Instance(mod, { env: env });
    state.instance = inst;
    if (typeof inst.exports.xyo_ext_abi !== 'function' ||
        (inst.exports.xyo_ext_abi() | 0) !== 1) {
      console.error('xyo: extension ABI mismatch');
      return -1;
    }
    if (typeof inst.exports.xyo_ext_call !== 'function' ||
        typeof inst.exports.xyo_ext_alloc !== 'function' ||
        !inst.exports.memory) {
      console.error('xyo: extension missing exports');
      return -1;
    }
    var list = Module.xyoExtInstances || (Module.xyoExtInstances = []);
    list.push(state);
    return (list.length - 1) | 0;
  } catch (e) {
    console.error('xyo: extension instantiate failed: ' + e);
    return -1;
  }
});

EM_JS(int, xyo_ext_js_call,
      (int handle, int op, const void* args, int nargs, void* ret, int retCap), {
  try {
    var list = Module.xyoExtInstances || [];
    var state = list[handle];
    if (!state || !state.instance) return -1;
    var inst = state.instance;
    var xyo = HEAPU8;
    var dv = new DataView(xyo.buffer);
    var argBase = args | 0;
    var parsed = [];
    for (var i = 0; i < nargs; ++i) {
      var off = argBase + i * 24;
      var tag = dv.getUint32(off, true);
      var num = dv.getFloat64(off + 8, true);
      var sp = dv.getUint32(off + 16, true);
      var sl = dv.getUint32(off + 20, true);
      var bytes = (tag === 2 && sl > 0) ? xyo.slice(sp, sp + sl) : null;
      parsed.push({tag: tag, num: num, bytes: bytes});
    }
    state.args = parsed;
    state.nargs = nargs | 0;

    var mem = new Uint8Array(inst.exports.memory.buffer);
    var extArgs = inst.exports.xyo_ext_alloc(nargs * 24) | 0;
    if (nargs > 0 && !extArgs) return -2;
    for (var i = 0; i < nargs; ++i) {
      var a = parsed[i];
      mem = new Uint8Array(inst.exports.memory.buffer);
      var off = extArgs + i * 24;
      var sp = 0;
      var sl = 0;
      if (a.tag === 2) {
        sl = a.bytes ? a.bytes.length : 0;
        sp = inst.exports.xyo_ext_alloc(sl + 1) | 0;
        if (!sp) return -2;
        mem = new Uint8Array(inst.exports.memory.buffer);
        if (sl) mem.set(a.bytes, sp);
        mem[sp + sl] = 0;
      }
      var v = new DataView(mem.buffer);
      v.setUint32(off, a.tag, true);
      v.setFloat64(off + 8, a.num, true);
      v.setUint32(off + 16, sp, true);
      v.setUint32(off + 20, sl, true);
    }

    var extRet = inst.exports.xyo_ext_alloc(24) | 0;
    if (!extRet) return -2;
    var status = inst.exports.xyo_ext_call(op | 0, extArgs, nargs | 0,
                                           extRet) | 0;
    if (status !== 0) return status;

    mem = new Uint8Array(inst.exports.memory.buffer);
    var rv = new DataView(mem.buffer);
    var rtag = rv.getUint32(extRet, true);
    var rnum = rv.getFloat64(extRet + 8, true);
    var rsp = rv.getUint32(extRet + 16, true);
    var rlen = rv.getUint32(extRet + 20, true);
    var out = HEAPU8;
    var outView = new DataView(out.buffer);
    var strPtr = 0;
    var strLen = 0;
    if (rtag === 2 && rlen > 0) {
      if (rsp + rlen > mem.length) return -3;
      if (24 + rlen > retCap) return -4;
      out.set(mem.subarray(rsp, rsp + rlen), ret + 24);
      strPtr = (ret + 24) | 0;
      strLen = rlen | 0;
    }
    outView.setUint32(ret, rtag, true);
    outView.setFloat64(ret + 8, rnum, true);
    outView.setUint32(ret + 16, strPtr, true);
    outView.setUint32(ret + 20, strLen, true);
    return 0;
  } catch (e) {
    console.error('xyo: extension call failed: ' + e);
    return -1;
  }
});

EM_JS(void, xyo_ext_js_destroy, (int handle), {
  var list = Module.xyoExtInstances;
  if (list && handle >= 0 && handle < list.length) list[handle] = null;
});

bool readFileBytes(const std::string& path, std::vector<uint8_t>* out,
                   std::string* error) {
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in) {
        if (error) *error = "cannot read wasm file: " + path;
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    if (out->empty()) {
        if (error) *error = "empty wasm file: " + path;
        return false;
    }
    return true;
}

struct WasmExtension {
    ExtensionDef def;
    std::unordered_map<std::string, uint32_t> ops;
    int handle = -1;
    std::mutex mutex;

    ~WasmExtension() {
        if (handle >= 0) xyo_ext_js_destroy(handle);
    }

    Value invoke(const std::string& opcode, const ArgValues& values) {
        auto it = ops.find(opcode);
        if (it == ops.end() || handle < 0) return Value::undefined();
        std::lock_guard<std::mutex> lock(mutex);
        const ExtensionBlockDef& block = def.blocks[it->second];
        const uint32_t nargs = uint32_t(block.args.size());

        std::vector<WasmArg> args(nargs);
        std::vector<std::string> strings(nargs);
        for (uint32_t i = 0; i < nargs; ++i) {
            const ExtensionArgDef& argDef = block.args[i];
            const Value v = values.get(argDef.name);
            WasmArg& a = args[i];
            if (argDef.type == "number" || argDef.type == "angle") {
                a.tag = 1;
                a.num = cast::toNumber(v);
            } else if (argDef.type == "boolean") {
                a.tag = 3;
                a.num = cast::toBoolean(v) ? 1.0 : 0.0;
            } else if (argDef.type == "string" || argDef.type == "dropdown" ||
                       v.isString()) {
                SStr* s = cast::toString(v);
                strings[i] = s ? SStr::toUtf8(s->view()) : std::string();
                a.tag = 2;
            } else if (v.isNumber()) {
                a.tag = 1;
                a.num = v.asNumberUnchecked();
            } else if (v.isBool()) {
                a.tag = 3;
                a.num = v.asBool() ? 1.0 : 0.0;
            } else {
                a.tag = 0;
            }
        }
        // 文字列ポインタは vector の構築後に確定する。
        for (uint32_t i = 0; i < nargs; ++i) {
            if (args[i].tag != 2) continue;
            args[i].str_ptr =
                uint32_t(reinterpret_cast<uintptr_t>(strings[i].data()));
            args[i].str_len = uint32_t(strings[i].size());
        }

        std::vector<uint8_t> ret(sizeof(WasmArg) + 65536);
        const int status = xyo_ext_js_call(handle, int(it->second), args.data(),
                                           int(nargs), ret.data(),
                                           int(ret.size()));
        if (status != 0) {
            static const bool debug = std::getenv("XYO_WASM_DEBUG") != nullptr;
            if (debug)
                std::fprintf(stderr, "[ext] %s failed (js status %d)\n",
                             opcode.c_str(), status);
            return Value::undefined();
        }
        WasmArg out{};
        std::memcpy(&out, ret.data(), sizeof(WasmArg));
        switch (out.tag) {
        case 1:
            return Value::num(out.num);
        case 2: {
            if (!out.str_len) return Value::emptyStr();
            const char* text =
                reinterpret_cast<const char*>(ret.data()) + sizeof(WasmArg);
            return Value::string(SStr::create(utf8ToU16(
                std::string_view(text, out.str_len))));
        }
        case 3:
            return Value::boolean(out.num != 0.0);
        default:
            return Value::undefined();
        }
    }
};

class WasmInstance final : public ExtensionInstance {
public:
    WasmInstance(ExtensionDef def, std::unique_ptr<WasmExtension> impl)
        : def_(std::move(def)), impl_(std::move(impl)) {}

    const ExtensionDef& def() const override { return def_; }

    Value call(const std::string& opcode, const ArgValues& args,
               BlockUtility&) override {
        return impl_->invoke(opcode, args);
    }

private:
    ExtensionDef def_;
    std::unique_ptr<WasmExtension> impl_;
};

} // namespace

bool wasmExtensionProviderAvailable() { return true; }

bool registerWasmExtension(Runtime& rt, ExtensionRegistry& registry,
                           const ExtensionDef& def,
                           const std::string& wasmPath,
                           std::string* error) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(wasmPath, &bytes, error)) return false;
    const int handle = xyo_ext_js_create(bytes.data(), int(bytes.size()));
    if (handle < 0) {
        if (error) *error = "browser WebAssembly rejected " + wasmPath;
        return false;
    }

    auto impl = std::make_unique<WasmExtension>();
    impl->def = def;
    impl->handle = handle;
    for (uint32_t i = 0; i < def.blocks.size(); ++i)
        impl->ops[def.blocks[i].opcode] = i;

    return registry.add(rt,
                        std::make_unique<WasmInstance>(def, std::move(impl)),
                        error);
}

} // namespace xyo

#endif // __EMSCRIPTEN__
