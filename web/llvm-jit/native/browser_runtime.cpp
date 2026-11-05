// Browser runtime bridge for the current xyo-cpp tree.
// Adapted to the xyo-editor unified host contract (MIT).
#include "engine/runtime.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "core/cast.hpp"
#include "core/sstr.hpp"
#include "model/variable.hpp"
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>

extern "C" void xyo_llvm_release(uint32_t handle);

namespace {
struct Instance {
    std::unique_ptr<xyo::Runtime> runtime = std::make_unique<xyo::Runtime>();
    bool loaded = false;
    std::string error;
    std::string snapshot;
};
std::map<uint32_t, std::unique_ptr<Instance>> instances;
uint32_t nextHandle = 1;
bool busy = false;
struct Enter {
    bool acquired = false;
    Enter() { if (!busy) { busy = true; acquired = true; } }
    ~Enter() { if (acquired) busy = false; }
};
Instance* find(uint32_t handle) {
    auto it = instances.find(handle);
    return it == instances.end() ? nullptr : it->second.get();
}
std::string quote(std::string_view str) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : str) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(char(c)); }
        else if (c < 0x20) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else out.push_back(char(c));
    }
    return out + "\"";
}
} // namespace

extern "C" {

// Called by the vendored LLVM side-module host; both ABIs use one guard.
int xyo_browser_internal_enter() {
    if (busy) return 0;
    busy = true;
    return 1;
}
void xyo_browser_internal_leave() { busy = false; }
xyo::Runtime* xyo_browser_internal_runtime(uint32_t handle) {
    auto* entry = find(handle);
    return entry && entry->loaded ? entry->runtime.get() : nullptr;
}
void xyo_browser_internal_error(uint32_t handle, const char* error) {
    if (auto* entry = find(handle)) entry->error = error ? error : "";
}
void xyo_browser_internal_clear(uint32_t handle) {
    if (auto* entry = find(handle)) entry->error.clear();
}

// A runtime-derived ABI descriptor. The browser compares every field with
// the LLVM compiler's metadata before loading code that shares our memory.
const char* xyo_browser_abi_descriptor() {
    static const std::string abi = [] {
        std::string out = "xyo-browser-ir-abi-v2";
        auto add = [&](const char* key, size_t n) {
            out += ';'; out += key; out += '='; out += std::to_string(n);
        };
        add("ptr_size", sizeof(void*)); add("ptr_align", alignof(void*));
        add("ctx_size", sizeof(xyo::JitCtx)); add("ctx_align", alignof(xyo::JitCtx));
        add("ctx_thread", offsetof(xyo::JitCtx, thread));
        add("ctx_target", offsetof(xyo::JitCtx, target));
        add("ctx_runtime", offsetof(xyo::JitCtx, runtime));
        add("ctx_var_slots", offsetof(xyo::JitCtx, varSlotData));
        add("variable_size", sizeof(xyo::Variable));
        add("variable_align", alignof(xyo::Variable));
        add("variable_value", offsetof(xyo::Variable, value));
        add("variable_list", offsetof(xyo::Variable, list));
        add("value_size", sizeof(xyo::Value)); add("value_align", alignof(xyo::Value));
        add("sstr_size", sizeof(xyo::SStr)); add("sstr_align", alignof(xyo::SStr));
        add("sstr_len", offsetof(xyo::SStr, len));
        add("sstr_num_cache", offsetof(xyo::SStr, numCache));
        add("sstr_num_state", offsetof(xyo::SStr, numState));
        auto addHex = [&](const char* key, uint64_t bits) {
            char buf[17]; std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(bits));
            out += ';'; out += key; out += '='; out += buf;
        };
        addHex("tag_base", xyo::val::kTagBase);
        addHex("tag_str", xyo::val::kTagStr);
        addHex("tag_color", xyo::val::kTagColor);
        addHex("tag_nan", xyo::val::kNan);
        addHex("tag_false", xyo::val::kFalse);
        addHex("tag_true", xyo::val::kTrue);
        addHex("tag_emptystr", xyo::val::kEmptyStr);
        addHex("tag_undefined", xyo::val::kUndefined);
        addHex("tag_ptrmask", xyo::val::kPtrMask);
        out += ";helper_xyo_varslot_init=void(ptr,i64,ptr,ptr,i32)";
        out += ";helper_xyo_to_num=double(i64)";
        return out;
    }();
    return abi.c_str();
}

uint32_t xyo_browser_create() {
    Enter lock;
    if (!lock.acquired || !nextHandle) return 0;
    // This module has no native fibers or ORC; never start the warp clock thread.
    xyo::setJitMode(xyo::JitMode::Off);
    uint32_t handle = nextHandle++;
    instances[handle] = std::make_unique<Instance>();
    return handle;
}
int xyo_browser_destroy(uint32_t handle) {
    // Release while runtime and its GC-root providers are still alive.
    xyo_llvm_release(handle);
    Enter lock;
    if (!lock.acquired) return 0;
    return instances.erase(handle) ? 1 : 0;
}
int xyo_browser_load_sb3(uint32_t handle, const uint8_t* data, size_t length) {
    xyo_llvm_release(handle);
    Enter lock;
    if (!lock.acquired || !data || !length || length > 16u * 1024u * 1024u) return 0;
    auto* instance = find(handle);
    if (!instance) return 0;
    instance->loaded = false;
    auto fresh = std::make_unique<xyo::Runtime>();
    auto result = xyo::loadSb3File(*fresh, data, length);
    if (!result.ok) { instance->error = result.error; return 0; }
    instance->runtime = std::move(fresh);
    instance->loaded = true;
    instance->error.clear();
    return 1;
}
int xyo_browser_green_flag(uint32_t handle) {
    Enter lock;
    auto* instance = find(handle);
    if (!lock.acquired || !instance || !instance->loaded) return 0;
    instance->runtime->greenFlag();
    return 1;
}
int xyo_browser_step(uint32_t handle) {
    Enter lock;
    auto* instance = find(handle);
    if (!lock.acquired || !instance || !instance->loaded) return 0;
    instance->runtime->step();
    return 1;
}
int xyo_browser_stop(uint32_t handle) {
    Enter lock;
    auto* instance = find(handle);
    if (!lock.acquired || !instance || !instance->loaded) return 0;
    instance->runtime->stopAll();
    return 1;
}
const char* xyo_browser_error(uint32_t handle) {
    auto* instance = find(handle);
    return instance ? instance->error.c_str() : "invalid handle";
}
const char* xyo_browser_snapshot(uint32_t handle) {
    Enter lock;
    auto* instance = find(handle);
    if (!lock.acquired || !instance || !instance->loaded) return nullptr;
    std::string json = "{\"targets\":[";
    bool firstTarget = true;
    for (auto* target : instance->runtime->targets) {
        if (!target) continue;
        if (!firstTarget) json += ",";
        firstTarget = false;
        json += "{\"id\":" + quote(target->id) + ",\"variables\":{";
        bool firstVar = true;
        for (const auto& [id, variable] : target->variables) {
            if (variable.type != xyo::Variable::kScalarType) continue;
            if (!firstVar) json += ",";
            firstVar = false;
            json += quote(id) + ":" + quote(xyo::SStr::toUtf8(xyo::cast::toString(variable.value)));
        }
        json += "}}";
    }
    json += "]}";
    instance->snapshot = std::move(json);
    return instance->snapshot.c_str();
}
} // extern "C"
