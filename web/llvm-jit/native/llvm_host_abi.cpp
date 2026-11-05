// Unified LLVM host: runs LLVM-compiled side modules inside the same xyo-cpp
// WASM runtime instance that also interprets the rest of the project.
//
// Why this exists: the product needs xyo-cpp's full feature set (interpreter,
// motion, pen, audio, renderer bridge) *and* LLVM-compiled scripts, sharing one
// Runtime so variables, sprites and clones are not duplicated between paths.
// The browser ABI in browser.cpp stays the public interpreter surface; these
// entry points add per-handle execution sessions on top of the same Runtime.
//
// Discipline mirrors real-host/host.cpp: bind slots only into module globals
// owned by this session, require an explicit expect-then-seal sequence, and
// never invoke an unsealed or foreign binding. dlopen handles are owned by the
// session that opened them and cannot be closed while it is alive.
#include "core/gc.hpp"
#include "core/sstr.hpp"
#include "core/value.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "model/target.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

extern "C" {
int xyo_browser_internal_enter(void);
void xyo_browser_internal_leave(void);
xyo::Runtime* xyo_browser_internal_runtime(uint32_t handle);
void xyo_browser_internal_error(uint32_t handle, const char* message);
void xyo_browser_internal_clear(uint32_t handle);
}

namespace {

struct Session {
    uint32_t handle = 0;
    xyo::Runtime* runtime = nullptr;
    std::unique_ptr<xyo::Thread> thread;
    xyo::JitCtx ctx{};
    xyo::Target* target = nullptr;
    std::string top;
    std::vector<void**> slots;
    std::vector<std::string> strings;
    size_t expectedSlots = 0;
    bool sealed = false;
    std::string error;

    Session() {
        xyo::Gc::get().addRootProvider(this, [this](xyo::Gc& gc) {
            for (auto* p : slots) gc.markPtr(*p);
        });
        liveSessions().insert(this);
    }
    ~Session() {
        liveSessions().erase(this);
        for (auto* p : slots) {
            slotOwners().erase(p);
            *p = nullptr;
        }
        for (auto it = handleOwners().begin(); it != handleOwners().end();) {
            if (it->second == this) it = handleOwners().erase(it);
            else ++it;
        }
        xyo::Gc::get().removeRootProvider(this);
    }
    static std::map<void**, Session*>& slotOwners() {
        static std::map<void**, Session*> owners;
        return owners;
    }
    static std::map<void*, Session*>& handleOwners() {
        static std::map<void*, Session*> owners;
        return owners;
    }
    static std::set<Session*>& liveSessions() {
        static std::set<Session*> live;
        return live;
    }
};

std::map<uint32_t, std::unique_ptr<Session>>& sessions() {
    static std::map<uint32_t, std::unique_ptr<Session>> all;
    return all;
}

Session* find(uint32_t handle) {
    auto it = sessions().find(handle);
    return it == sessions().end() ? nullptr : it->second.get();
}

// Every entry point shares browser.cpp's reentrancy guard so LLVM work can
// never re-enter the interpreter ABI mid-call.
struct Guard {
    bool entered;
    Guard() : entered(xyo_browser_internal_enter() != 0) {}
    ~Guard() { if (entered) xyo_browser_internal_leave(); }
};

void setError(uint32_t handle, const std::string& message) {
    xyo_browser_internal_error(handle, message.c_str());
}

} // namespace

extern "C" {

// Creates the execution session for one green-flag hat on an already loaded
// project. The Runtime is shared: the compiled script reads and writes the same
// variables the interpreter uses, so no commit-back step is needed.
int xyo_llvm_prepare(uint32_t handle, const char* hatId) {
    Guard guard;
    if (!guard.entered) return 0;
    auto* runtime = xyo_browser_internal_runtime(handle);
    if (!runtime || !hatId || !*hatId) {
        setError(handle, "LLVM host requires a loaded project and a hat id");
        return 0;
    }
    xyo_browser_internal_clear(handle);
    sessions().erase(handle);
    auto session = std::make_unique<Session>();
    session->handle = handle;
    session->runtime = runtime;
    session->top = hatId;
    for (auto* target : runtime->targets) {
        if (!target || !target->isOriginal || !target->blocks) continue;
        if (!target->blocks->getBlock(hatId)) continue;
        session->target = target;
        break;
    }
    if (!session->target) {
        setError(handle, "hat block not found in any target");
        return 0;
    }
    session->thread = std::make_unique<xyo::Thread>(session->top);
    session->thread->target = session->target;
    session->ctx.thread = session->thread.get();
    session->ctx.target = session->target;
    session->ctx.runtime = runtime;
    session->ctx.sequencer = runtime->sequencer.get();
    sessions()[handle] = std::move(session);
    return 1;
}

int xyo_llvm_expect_slots(uint32_t handle, uint32_t count) {
    Guard guard;
    if (!guard.entered) return 0;
    auto* session = find(handle);
    if (!session || count == 0 || count > 64) return 0;
    if (session->expectedSlots != 0 || !session->slots.empty()) return 0;
    session->expectedSlots = count;
    return 1;
}

// kind follows upstream AotSlotKind. The address is registered before any SStr
// is allocated, matching loadAot. key is raw bytes, not necessarily text.
int xyo_llvm_bind_slot(uint32_t handle, void** slot, int kind, const uint8_t* key, size_t size) {
    Guard guard;
    if (!guard.entered) return 0;
    auto* session = find(handle);
    if (!session || !session->target || !slot || !key || *slot) return 0;
    for (auto* p : session->slots) if (p == slot) return 0;
    if (kind != 0 && kind != 1 && kind != 2 && kind != 3) return 0;
    if ((kind == 0 || kind == 1) && size && std::memchr(key, 0, size)) return 0;
    if (kind == 3 && (size & 1)) return 0;
    {
        auto it = Session::slotOwners().find(slot);
        if (it != Session::slotOwners().end() && it->second != session) return 0;
    }
    if (session->expectedSlots > 0 && session->slots.size() >= session->expectedSlots) return 0;
    void* value = nullptr;
    if (kind == 0 || kind == 1) {
        session->strings.emplace_back(reinterpret_cast<const char*>(key), size);
        value = const_cast<char*>(session->strings.back().c_str());
    } else if (kind == 2) {
        std::string s(reinterpret_cast<const char*>(key), size);
        auto sep = s.find('\1');
        size_t ti = 0;
        std::string id = s;
        if (sep != std::string::npos) {
            if (!sep) return 0;
            for (size_t i = 0; i < sep; ++i) {
                if (s[i] < '0' || s[i] > '9' || ti > 1000000) return 0;
                ti = ti * 10 + size_t(s[i] - '0');
            }
            id = s.substr(sep + 1);
        }
        if (ti >= session->runtime->targets.size() || !session->runtime->targets[ti] ||
            !session->runtime->targets[ti]->blocks) return 0;
        auto* blocks = session->runtime->targets[ti]->blocks;
        if (!blocks->getBlock(id)) return 0;
        value = blocks->getCachedExecute(id);
        if (!value) return 0;
    }
    session->slots.push_back(slot);
    if (kind == 3) {
        std::u16string units(size / 2, u'\0');
        if (size) std::memcpy(units.data(), key, size);
        value = xyo::SStr::create(units);
    }
    *slot = value;
    Session::slotOwners()[slot] = session;
    if (session->expectedSlots > 0 && session->slots.size() == session->expectedSlots) {
        session->sealed = true;
    }
    return 1;
}

int xyo_llvm_is_sealed(uint32_t handle) {
    Guard guard;
    if (!guard.entered) return 0;
    auto* session = find(handle);
    return session && session->sealed ? 1 : 0;
}

// The caller preflights the Wasm bytes and the import allowlist before this.
void* xyo_llvm_dlopen(uint32_t handle, const char* path) {
    Guard guard;
    if (!guard.entered) return nullptr;
    auto* session = find(handle);
    if (!session || !path) {
        setError(handle, "LLVM dlopen requires a prepared session and a path");
        return nullptr;
    }
    void* opened = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!opened) {
        setError(handle, "dlopen failed");
        return nullptr;
    }
    Session::handleOwners()[opened] = session;
    return opened;
}

void* xyo_llvm_dlsym(void* opened, const char* name) {
    Guard guard;
    if (!guard.entered) return nullptr;
    return opened && name ? dlsym(opened, name) : nullptr;
}

int xyo_llvm_dlclose(void* opened) {
    Guard guard;
    if (!guard.entered) return -1;
    if (!opened) return -1;
    auto it = Session::handleOwners().find(opened);
    if (it != Session::handleOwners().end() && Session::liveSessions().count(it->second)) return -1;
    Session::handleOwners().erase(opened);
    return dlclose(opened);
}

// Silent refusal by design: unsealed or foreign bindings never execute.
void xyo_llvm_invoke(uint32_t handle, void* entry) {
    Guard guard;
    if (!guard.entered) return;
    auto* session = find(handle);
    if (!entry || !session || !session->target || !session->sealed) return;
    reinterpret_cast<xyo::ScriptFn>(entry)(&session->ctx);
}

void xyo_llvm_release(uint32_t handle) {
    Guard guard;
    if (!guard.entered) return;
    sessions().erase(handle);
}

// Reads a variable from the shared Runtime without going through the snapshot
// JSON, so callers can verify committed values cheaply.
xyo::Variable* xyo_llvm_variable(uint32_t handle, const char* id) {
    Guard guard;
    if (!guard.entered) return nullptr;
    auto* session = find(handle);
    if (!session || !session->target || !id) return nullptr;
    return session->target->lookupVariableById(id);
}

uint64_t xyo_llvm_variable_bits(xyo::Variable* variable) {
    return variable ? variable->value.bits : 0;
}


} // extern "C"

namespace {

// Segment bindings: a straight-line run needs only its string/slot storage and
// the dlopen handle. No Thread and no JitCtx are created here — the runtime
// supplies the executing thread's context, and `xyo_varslot_init` resolves the
// variables on every call.
struct SegmentBinding {
    std::vector<void**> slots;
    std::vector<std::string> strings;
    size_t expected = 0;
    bool sealed = false;
    SegmentBinding() { live().push_back(this); }
    ~SegmentBinding() {
        live().erase(std::remove(live().begin(), live().end(), this), live().end());
        for (auto* slot : slots) { slotOwners().erase(slot); *slot = nullptr; }
    }
    static std::vector<SegmentBinding*>& live() { static std::vector<SegmentBinding*> all; return all; }
    static std::map<void**, SegmentBinding*>& slotOwners() { static std::map<void**, SegmentBinding*> owners; return owners; }
};

std::map<uint32_t, std::vector<std::unique_ptr<SegmentBinding>>>& segmentBindings() {
    static std::map<uint32_t, std::vector<std::unique_ptr<SegmentBinding>>> all;
    return all;
}

SegmentBinding* currentSegment(uint32_t handle) {
    auto it = segmentBindings().find(handle);
    return it == segmentBindings().end() || it->second.empty() ? nullptr : it->second.back().get();
}

} // namespace

extern "C" {

// Starts a fresh binding for one segment; the host then binds exactly the
// slots the compiler reported and registers the resulting function.
int xyo_llvm_segment_open(uint32_t handle) {
    Guard guard;
    if (!guard.entered) return 0;
    segmentBindings()[handle].push_back(std::make_unique<SegmentBinding>());
    return 1;
}

int xyo_llvm_segment_expect(uint32_t handle, uint32_t count) {
    Guard guard;
    if (!guard.entered) return 0;
    auto* binding = currentSegment(handle);
    if (!binding || count == 0 || count > 64) return 0;
    if (binding->expected != 0 || !binding->slots.empty()) return 0;
    binding->expected = count;
    return 1;
}

int xyo_llvm_segment_bind(uint32_t handle, void** slot, int kind, const uint8_t* key, size_t size) {
    Guard guard;
    if (!guard.entered) return 0;
    auto* binding = currentSegment(handle);
    if (!binding || !slot || !key || *slot) return 0;
    for (auto* existing : binding->slots) if (existing == slot) return 0;
    // Only VarId (0/1) and StrLit (3) are meaningful for a straight-line run;
    // BlockCached would need the live target and is not used here.
    if (kind != 0 && kind != 1 && kind != 3) return 0;
    if (kind != 3 && size && std::memchr(key, 0, size)) return 0;
    if (kind == 3 && (size & 1)) return 0;
    {
        auto owner = SegmentBinding::slotOwners().find(slot);
        if (owner != SegmentBinding::slotOwners().end()) return 0;
    }
    if (binding->expected > 0 && binding->slots.size() >= binding->expected) return 0;
    void* value = nullptr;
    if (kind == 3) {
        std::u16string units(size / 2, u'\0');
        if (size) std::memcpy(units.data(), key, size);
        value = xyo::SStr::create(units);
    } else {
        binding->strings.emplace_back(reinterpret_cast<const char*>(key), size);
        value = const_cast<char*>(binding->strings.back().c_str());
    }
    binding->slots.push_back(slot);
    *slot = value;
    SegmentBinding::slotOwners()[slot] = binding;
    if (binding->expected > 0 && binding->slots.size() == binding->expected) binding->sealed = true;
    return 1;
}

int xyo_llvm_segment_sealed(uint32_t handle) {
    Guard guard;
    if (!guard.entered) return 0;
    auto* binding = currentSegment(handle);
    return binding && binding->sealed ? 1 : 0;
}

void* xyo_llvm_segment_dlopen(uint32_t handle, const char* path) {
    Guard guard;
    if (!guard.entered) return nullptr;
    if (!currentSegment(handle) || !path) return nullptr;
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

void* xyo_llvm_segment_dlsym(void* opened, const char* name) {
    Guard guard;
    if (!guard.entered) return nullptr;
    return opened && name ? dlsym(opened, name) : nullptr;
}

} // extern "C"
