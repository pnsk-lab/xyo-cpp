// xyo-cpp: native WASM 拡張プロバイダ（vendored wasm3）。
#include "wasm_provider.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../core/cast.hpp"
#include "../core/fileio.hpp"
#include "../core/sstr.hpp"
#include "../model/blocks.hpp"

#if !defined(__EMSCRIPTEN__) && defined(XYO_WASM_EXT)
extern "C" {
#include "wasm3.h"
}
#endif

namespace xyo {
namespace {

/// 拡張メモリ上の引数 / 戻り値。wasm 側の同じレイアウトと一致させる。
struct WasmArg {
    uint32_t tag;      // 0=undefined 1=number 2=string 3=bool
    double num;
    uint32_t str_ptr;
    uint32_t str_len;
};
static_assert(sizeof(WasmArg) == 24, "WasmArg ABI size");

struct WasmCallState {
    const ExtensionBlockDef* block = nullptr;
    const ArgValues* args = nullptr;
    uint32_t nargs = 0;
    uint32_t args_ptr = 0;
    uint32_t ret_ptr = 0;
};

#if !defined(__EMSCRIPTEN__) && defined(XYO_WASM_EXT)

struct WasmExtension {
    ExtensionDef def;
    std::vector<uint8_t> bytes;
    IM3Environment env = nullptr;
    IM3Runtime runtime = nullptr;
    IM3Module module = nullptr;
    IM3Function call_fn = nullptr;
    IM3Function alloc_fn = nullptr;
    std::unordered_map<std::string, uint32_t> ops;
    WasmCallState* current = nullptr;
    std::mutex mutex;

    ~WasmExtension() {
        if (runtime) m3_FreeRuntime(runtime);
        if (env) m3_FreeEnvironment(env);
    }

    uint8_t* memory(size_t* size = nullptr) {
        size_t s = 0;
        uint8_t* p = m3_GetMemory(module, &s, 0);
        if (size) *size = s;
        return p;
    }

    uint32_t alloc(uint32_t n) {
        if (!alloc_fn || n == 0) return 0;
        m3_SetResourceLimit(runtime, c_m3Limit_GasUnits, 10'000'000);
        if (m3_CallV(alloc_fn, int32_t(n)) != nullptr) return 0;
        int32_t ptr = 0;
        if (m3_GetResultsV(alloc_fn, &ptr) != nullptr) return 0;
        return ptr <= 0 ? 0 : uint32_t(ptr);
    }

    bool readArg(uint32_t index, WasmArg* out) const {
        if (!current || index >= current->nargs) return false;
        size_t size = 0;
        WasmExtension* self = const_cast<WasmExtension*>(this);
        uint8_t* mem = self->memory(&size);
        const size_t off =
            size_t(current->args_ptr) + size_t(index) * sizeof(WasmArg);
        if (!mem || off > size || sizeof(WasmArg) > size - off) return false;
        std::memcpy(out, mem + off, sizeof(WasmArg));
        return true;
    }

    Value invoke(const std::string& opcode, const ArgValues& args) {
        auto it = ops.find(opcode);
        if (it == ops.end() || !call_fn) return Value::undefined();
        std::lock_guard<std::mutex> lock(mutex);
        const ExtensionBlockDef& block = def.blocks[it->second];

        const uint32_t nargs = uint32_t(block.args.size());
        uint32_t argsPtr = 0;
        if (nargs) {
            argsPtr = alloc(nargs * sizeof(WasmArg));
            if (!argsPtr) return Value::undefined();
        }
        for (uint32_t i = 0; i < nargs; ++i) {
            const ExtensionArgDef& argDef = block.args[i];
            const Value v = args.get(argDef.name);
            WasmArg a{};

            auto writeString = [&]() -> bool {
                SStr* s = cast::toString(v);
                const std::string utf8 =
                    s ? SStr::toUtf8(s->view()) : std::string();
                const uint32_t p = alloc(uint32_t(utf8.size() + 1));
                if (!p) return false;
                size_t size = 0;
                uint8_t* mem = memory(&size);
                if (!mem || size_t(p) > size ||
                    utf8.size() + 1 > size - size_t(p))
                    return false;
                std::memcpy(mem + p, utf8.data(), utf8.size());
                mem[p + utf8.size()] = 0;
                a.tag = 2;
                a.str_ptr = p;
                a.str_len = uint32_t(utf8.size());
                return true;
            };

            if (argDef.type == "number" || argDef.type == "angle") {
                a.tag = 1;
                a.num = cast::toNumber(v);
            } else if (argDef.type == "boolean") {
                a.tag = 3;
                a.num = cast::toBoolean(v) ? 1.0 : 0.0;
            } else if (argDef.type == "string" || argDef.type == "dropdown") {
                if (!writeString()) return Value::undefined();
            } else if (v.isNumber()) {
                a.tag = 1;
                a.num = v.asNumberUnchecked();
            } else if (v.isBool()) {
                a.tag = 3;
                a.num = v.asBool() ? 1.0 : 0.0;
            } else if (v.isString()) {
                if (!writeString()) return Value::undefined();
            } else {
                a.tag = 0;
            }
            size_t size = 0;
            uint8_t* mem = memory(&size);
            const size_t off = size_t(argsPtr) + size_t(i) * sizeof(WasmArg);
            if (!mem || off > size || sizeof(WasmArg) > size - off)
                return Value::undefined();
            std::memcpy(mem + off, &a, sizeof(WasmArg));
        }

        const uint32_t retPtr = alloc(sizeof(WasmArg));
        if (!retPtr) return Value::undefined();

        WasmCallState state;
        state.block = &block;
        state.args = &args;
        state.nargs = nargs;
        state.args_ptr = argsPtr;
        state.ret_ptr = retPtr;
        current = &state;
        m3_SetResourceLimit(runtime, c_m3Limit_GasUnits, 100'000'000);
        const M3Result call = m3_CallV(call_fn, int32_t(it->second),
                                       int32_t(argsPtr), int32_t(nargs),
                                       int32_t(retPtr));
        current = nullptr;
        if (call != nullptr) {
            static const bool debug =
                std::getenv("XYO_WASM_DEBUG") != nullptr;
            if (debug)
                std::fprintf(stderr, "[ext] %s trapped: %s\n",
                             opcode.c_str(), call);
            return Value::undefined();
        }

        int32_t status = 0;
        if (m3_GetResultsV(call_fn, &status) != nullptr || status != 0)
            return Value::undefined();

        size_t size = 0;
        uint8_t* mem = memory(&size);
        if (!mem || size_t(retPtr) > size ||
            sizeof(WasmArg) > size - size_t(retPtr))
            return Value::undefined();
        WasmArg ret{};
        std::memcpy(&ret, mem + retPtr, sizeof(WasmArg));
        switch (ret.tag) {
        case 1:
            return Value::num(ret.num);
        case 2: {
            if (!ret.str_ptr || !ret.str_len) return Value::emptyStr();
            if (size_t(ret.str_ptr) > size ||
                size_t(ret.str_len) > size - size_t(ret.str_ptr))
                return Value::undefined();
            const std::string utf8(
                reinterpret_cast<const char*>(mem) + ret.str_ptr, ret.str_len);
            return Value::string(SStr::create(utf8ToU16(utf8)));
        }
        case 3:
            return Value::boolean(ret.num != 0.0);
        default:
            return Value::undefined();
        }
    }
};

WasmExtension* extOf(IM3Runtime runtime) {
    return static_cast<WasmExtension*>(m3_GetUserData(runtime));
}

m3ApiRawFunction(host_arg_count) {
    m3ApiReturnType(int32_t)
    WasmExtension* ext = extOf(runtime);
    m3ApiReturn(ext && ext->current ? int32_t(ext->current->nargs) : 0);
}

m3ApiRawFunction(host_arg_tag) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, index)
    WasmArg arg{};
    WasmExtension* ext = extOf(runtime);
    if (!ext || index < 0 || !ext->readArg(uint32_t(index), &arg))
        m3ApiReturn(0);
    m3ApiReturn(int32_t(arg.tag));
}

m3ApiRawFunction(host_arg_num) {
    m3ApiReturnType(double)
    m3ApiGetArg(int32_t, index)
    WasmArg arg{};
    WasmExtension* ext = extOf(runtime);
    if (!ext || index < 0 || !ext->readArg(uint32_t(index), &arg))
        m3ApiReturn(0.0);
    m3ApiReturn(arg.num);
}

m3ApiRawFunction(host_arg_str_copy) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, index)
    m3ApiGetArg(int32_t, dst)
    m3ApiGetArg(int32_t, cap)
    WasmExtension* ext = extOf(runtime);
    if (!ext || !ext->current || index < 0 || dst < 0 || cap < 0)
        m3ApiReturn(0);
    const ExtensionBlockDef* block = ext->current->block;
    if (!block || size_t(index) >= block->args.size()) m3ApiReturn(0);

    const Value v = ext->current->args->get(block->args[index].name);
    SStr* s = cast::toString(v);
    const std::string utf8 = s ? SStr::toUtf8(s->view()) : std::string();
    const size_t n = std::min<size_t>(size_t(cap), utf8.size());
    const size_t size = m3_GetMemorySizeAt(_mem);
    if (size_t(dst) > size || n > size - size_t(dst)) m3ApiReturn(0);
    std::memcpy(static_cast<uint8_t*>(_mem) + dst, utf8.data(), n);
    m3ApiReturn(int32_t(n));
}

m3ApiRawFunction(host_log) {
    m3ApiGetArg(int32_t, ptr)
    m3ApiGetArg(int32_t, len)
    if (ptr >= 0 && len >= 0) {
        const size_t size = m3_GetMemorySizeAt(_mem);
        if (size_t(ptr) <= size && size_t(len) <= size - size_t(ptr)) {
            std::fprintf(stderr, "[ext] %.*s\n", len,
                         static_cast<const char*>(_mem) + ptr);
        }
    }
    m3ApiSuccess();
}

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

const char* resultText(IM3Runtime runtime) {
    M3ErrorInfo info{};
    m3_GetErrorInfo(runtime, &info);
    return info.message ? info.message : "wasm3 error";
}

bool linkHostFunctions(WasmExtension* ext, std::string* error) {
    struct Entry {
        const char* name;
        const char* signature;
        M3RawCall fn;
    };
    const Entry entries[] = {
        {"xyo_arg_count", "i()", &host_arg_count},
        {"xyo_arg_tag", "i(i)", &host_arg_tag},
        {"xyo_arg_num", "F(i)", &host_arg_num},
        {"xyo_arg_str_copy", "i(iii)", &host_arg_str_copy},
        {"xyo_log", "v(ii)", &host_log},
    };
    for (const Entry& e : entries) {
        const M3Result r =
            m3_LinkRawFunction(ext->module, "env", e.name, e.signature, e.fn);
        // The extension may not declare every optional host import.
        if (r != nullptr && r != m3Err_functionLookupFailed) {
            if (error) *error = std::string("link ") + e.name + ": " + r;
            return false;
        }
    }
    return true;
}

bool loadModule(WasmExtension* ext, const std::string& path,
                std::string* error) {
    if (!readFileBytes(path, &ext->bytes, error)) return false;

    ext->env = m3_NewEnvironment();
    if (!ext->env) {
        if (error) *error = "wasm3 environment creation failed";
        return false;
    }
    ext->runtime = m3_NewRuntime(ext->env, 64 * 1024, ext);
    if (!ext->runtime) {
        if (error) *error = "wasm3 runtime creation failed";
        return false;
    }
    m3_SetResourceLimit(ext->runtime, c_m3Limit_MemoryBytes,
                        64ull * 1024 * 1024);
    // Gas must be armed before a body is compiled (m3_FindFunction / call).
    m3_SetResourceLimit(ext->runtime, c_m3Limit_GasUnits, 100'000'000);

    const M3Result parsed =
        m3_ParseModule(ext->env, &ext->module, ext->bytes.data(),
                       uint32_t(ext->bytes.size()));
    if (parsed != nullptr) {
        if (error) *error = std::string("parse: ") + parsed;
        return false;
    }
    const M3Result loaded = m3_LoadModule(ext->runtime, ext->module);
    if (loaded != nullptr) {
        if (error) *error = std::string("load: ") + loaded;
        return false;
    }
    if (!linkHostFunctions(ext, error)) return false;

    IM3Function abi = nullptr;
    M3Result r = m3_FindFunction(&abi, ext->runtime, "xyo_ext_abi");
    if (r != nullptr) {
        if (error) *error = std::string("missing xyo_ext_abi: ") + r;
        return false;
    }
    m3_SetResourceLimit(ext->runtime, c_m3Limit_GasUnits, 10'000'000);
    int32_t abiVersion = 0;
    r = m3_CallV(abi);
    if (r == nullptr) r = m3_GetResultsV(abi, &abiVersion);
    if (r != nullptr || abiVersion != 1) {
        if (error)
            *error = std::string("unsupported extension ABI: ") +
                     resultText(ext->runtime);
        return false;
    }

    r = m3_FindFunction(&ext->call_fn, ext->runtime, "xyo_ext_call");
    if (r != nullptr) {
        if (error) *error = std::string("missing xyo_ext_call: ") + r;
        return false;
    }
    m3_SetResourceLimit(ext->runtime, c_m3Limit_GasUnits, 100'000'000);
    r = m3_FindFunction(&ext->alloc_fn, ext->runtime, "xyo_ext_alloc");
    if (r != nullptr) {
        if (error) *error = std::string("missing xyo_ext_alloc: ") + r;
        return false;
    }
    return true;
}

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

#endif // XYO_WASM_EXT

} // namespace

#if !defined(__EMSCRIPTEN__)
bool wasmExtensionProviderAvailable() {
#if defined(XYO_WASM_EXT)
    return true;
#else
    return false;
#endif
}

bool registerWasmExtension(Runtime& rt, ExtensionRegistry& registry,
                           const ExtensionDef& def,
                           const std::string& wasmPath,
                           std::string* error) {
#if !defined(XYO_WASM_EXT)
    (void)rt;
    (void)registry;
    (void)def;
    (void)wasmPath;
    if (error)
        *error = "WASM extension provider is not built (XYO_WASM_EXT=OFF)";
    return false;
#else
    auto impl = std::make_unique<WasmExtension>();
    impl->def = def;
    for (uint32_t i = 0; i < def.blocks.size(); ++i)
        impl->ops[def.blocks[i].opcode] = i;

    if (!loadModule(impl.get(), wasmPath, error)) return false;

    return registry.add(rt,
                        std::make_unique<WasmInstance>(def, std::move(impl)),
                        error);
#endif
}

#endif // !__EMSCRIPTEN__

} // namespace xyo
