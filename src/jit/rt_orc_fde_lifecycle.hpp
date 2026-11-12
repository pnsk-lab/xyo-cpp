// RT-001: opt-in ORC/JITLink -> libunwind FDE lifetime diagnostic.
//
// This is *not* a native Fiber safepoint, stack-map root provider,
// or permission for GC to collect while any native Fiber is suspended.
// Only compile/link this header for experimental Linux x86-64 LLVM builds.
#pragma once

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>

#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <dlfcn.h>

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace xyo::rt {

struct GnuJitFdeBases {
    void* tbase;
    void* dbase;
    void* func;
};
using GnuJitFindFde = void* (*)(void*, GnuJitFdeBases*);

// Read-only diagnostic index. Every interval is published only AFTER
// libunwind has accepted its exact GNU JIT FDE. It is withdrawn BEFORE
// its ORC ResourceTracker can unmap the backing code and .eh_frame.
// This is NOT a lease, a GC root or permission to unwind arbitrary
// mutable JIT code on another thread.
class OrcFdeActiveIndex {
public:
    void add(uintptr_t start, uintptr_t end) {
        std::lock_guard guard(mu_);
        if (start && end > start) entries_[start] = end;
    }
    void erase(uintptr_t start) {
        std::lock_guard guard(mu_);
        entries_.erase(start);
    }
    bool contains(uintptr_t pc, uintptr_t* start, uintptr_t* end) {
        std::lock_guard guard(mu_);
        auto it = entries_.upper_bound(pc);
        if (it == entries_.begin()) return false;
        --it;
        if (pc < it->first || pc >= it->second) return false;
        if (start) *start = it->first;
        if (end) *end = it->second;
        return true;
    }
private:
    std::mutex mu_;
    std::map<uintptr_t, uintptr_t> entries_;
};
inline OrcFdeActiveIndex& activeOrcFdeIndex() {
    static OrcFdeActiveIndex index;
    return index;
}


// Owned stable storage. libunwind retains BOTH the remote table address
// and unw_dyn_info_t until _U_dyn_cancel is invoked.
class OrcFdeLease {
public:
    static std::unique_ptr<OrcFdeLease>
    create(uintptr_t pcStart, size_t functionSize, void* fde) {
        if (!fde || !pcStart || functionSize < 2 ||
            functionSize > 0x100000 || pcStart > UINTPTR_MAX-functionSize)
            return {};
        const auto offset = reinterpret_cast<intptr_t>(fde) -
                            static_cast<intptr_t>(pcStart);
        if (offset < INT32_MIN || offset > INT32_MAX) return {};
        auto lease = std::unique_ptr<OrcFdeLease>(
            new OrcFdeLease(pcStart, functionSize, int32_t(offset)));
        _U_dyn_register(&lease->info_);
        unw_flush_cache(unw_local_addr_space, 0, ~unw_word_t{0});
        unw_proc_info_t info{};
        if (unw_get_proc_info_by_ip(unw_local_addr_space, pcStart+1,
                                    &info, nullptr) < 0 ||
            info.start_ip != pcStart ||
            info.end_ip != pcStart+functionSize) {
            _U_dyn_cancel(&lease->info_);
            unw_flush_cache(unw_local_addr_space, 0, ~unw_word_t{0});
            lease->active_ = false;
            return {};
        }
        return lease;
    }
    uintptr_t start() const { return uintptr_t(info_.start_ip); }
    uintptr_t end() const { return uintptr_t(info_.end_ip); }
    OrcFdeLease(const OrcFdeLease&) = delete;
    OrcFdeLease& operator=(const OrcFdeLease&) = delete;
    ~OrcFdeLease() { cancel(); }

    void cancel() {
        if (!active_) return;
        _U_dyn_cancel(&info_);
        active_ = false;
        unw_flush_cache(unw_local_addr_space, 0, ~unw_word_t{0});
    }

private:
    OrcFdeLease(uintptr_t start, size_t size, int32_t offset) {
        table_[0] = 0;
        table_[1] = offset;
        info_.start_ip = start;
        info_.end_ip = start+size;
        info_.format = UNW_INFO_FORMAT_REMOTE_TABLE;
        info_.u.rti.segbase = start;
        info_.u.rti.table_data = reinterpret_cast<unw_word_t>(table_);
        info_.u.rti.table_len = sizeof(table_)/sizeof(unw_word_t);
    }
    int32_t table_[2]{};
    unw_dyn_info_t info_{};
    bool active_ = true;
};

class OrcJitFdeLifecyclePlugin final
    : public llvm::orc::LinkGraphLinkingLayer::Plugin {
public:
    static std::shared_ptr<OrcJitFdeLifecyclePlugin> create() {
        void* lib = dlopen("libgcc_s.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (!lib) return {};
        auto find = reinterpret_cast<GnuJitFindFde>(
            dlsym(lib, "_Unwind_Find_FDE"));
        if (!find) { dlclose(lib); return {}; }
        return std::shared_ptr<OrcJitFdeLifecyclePlugin>(
            new OrcJitFdeLifecyclePlugin(lib, find));
    }

    ~OrcJitFdeLifecyclePlugin() override {
        // Destruction order: cancel libunwind entries BEFORE dlclose.
        {
            std::lock_guard guard(mutex_);
            for (auto& [key, leases] : active_)
                for (const auto& lease : leases)
                    activeOrcFdeIndex().erase(lease->start());
            active_.clear();
            pending_.clear();
        }
        dlclose(lib_);
    }

    void modifyPassConfig(
        llvm::orc::MaterializationResponsibility& responsibility,
        llvm::jitlink::LinkGraph&,
        llvm::jitlink::PassConfiguration& passes) override {
        auto* identity = &responsibility;
        passes.PostFixupPasses.push_back(
            [this, identity](llvm::jitlink::LinkGraph& graph)
                -> llvm::Error {
                std::vector<Symbol> functions;
                for (auto* sym : graph.defined_symbols()) {
                    if (!sym->hasName() || !sym->isCallable() ||
                        !(*sym->getName()).starts_with("xyo_script_"))
                        continue;
                    const size_t extent = sym->getSize();
                    if (extent < 2 || extent > 0x100000) continue;
                    functions.push_back({
                        sym->getAddress().getValue(), extent
                    });
                }
                std::lock_guard guard(mutex_);
                pending_[identity] = std::move(functions);
                return llvm::Error::success();
            });
    }

    llvm::Error notifyEmitted(
        llvm::orc::MaterializationResponsibility& responsibility) override {
        std::vector<Symbol> functions;
        {
            std::lock_guard guard(mutex_);
            auto item = pending_.find(&responsibility);
            if (item == pending_.end())
                return llvm::Error::success();
            functions = std::move(item->second);
            pending_.erase(item);
        }
        if (functions.empty()) return llvm::Error::success();

        // Either an entire module's discovered FDEs are published, or
        // staged registrations are canceled and no partial state remains.
        std::vector<std::unique_ptr<OrcFdeLease>> staged;
        staged.reserve(functions.size());
        for (const Symbol& fn : functions) {
            GnuJitFdeBases bases{};
            void* fde = findFde_(reinterpret_cast<void*>(fn.start+1),
                                 &bases);
            if (!fde || reinterpret_cast<uintptr_t>(bases.func) != fn.start) {
                std::fprintf(stderr,
                    "[rt001-orc-fde] phase=emit fde_missing=1 "
                    "gc_safe=0\n");
                return llvm::Error::success();
            }
            auto lease = OrcFdeLease::create(fn.start, fn.size, fde);
            if (!lease) {
                std::fprintf(stderr,
                    "[rt001-orc-fde] phase=emit invalid_extent=1 "
                    "gc_safe=0\n");
                return llvm::Error::success();
            }
            staged.push_back(std::move(lease));
        }
        const auto key =
            responsibility.getResourceTracker()->getKeyUnsafe();
        {
            std::lock_guard guard(mutex_);
            auto& entries = active_[key];
            for (auto& lease : staged) {
                activeOrcFdeIndex().add(lease->start(), lease->end());
                entries.push_back(std::move(lease));
            }
        }
        std::fprintf(stderr,
            "[rt001-orc-fde] phase=emit registered=%zu gc_safe=0\n",
            functions.size());
        return llvm::Error::success();
    }

    llvm::Error notifyFailed(
        llvm::orc::MaterializationResponsibility& responsibility) override {
        std::lock_guard guard(mutex_);
        pending_.erase(&responsibility);
        return llvm::Error::success();
    }

    llvm::Error notifyRemovingResources(
        llvm::orc::JITDylib&, llvm::orc::ResourceKey key) override {
        std::lock_guard guard(mutex_);
        const auto item = active_.find(key);
        if (item == active_.end()) return llvm::Error::success();
        const size_t count = item->second.size();
        // This callback runs before JITLinkMemoryManager unmaps code/FDE.
        // Do not retain borrowed FDE bytes after this point.
        for (const auto& lease : item->second)
            activeOrcFdeIndex().erase(lease->start());
        active_.erase(item);
        std::fprintf(stderr,
            "[rt001-orc-fde] phase=remove canceled=%zu gc_safe=0\n",
            count);
        return llvm::Error::success();
    }

    void notifyTransferringResources(
        llvm::orc::JITDylib&, llvm::orc::ResourceKey dst,
        llvm::orc::ResourceKey src) override {
        std::lock_guard guard(mutex_);
        const auto item = active_.find(src);
        if (item == active_.end()) return;
        auto& entries = active_[dst];
        for (auto& entry : item->second)
            entries.push_back(std::move(entry));
        active_.erase(item);
        std::fprintf(stderr,
            "[rt001-orc-fde] phase=transfer gc_safe=0\n");
    }

private:
    struct Symbol { uintptr_t start; size_t size; };
    OrcJitFdeLifecyclePlugin(void* lib, GnuJitFindFde find)
        : lib_(lib), findFde_(find) {}

    void* lib_;
    GnuJitFindFde findFde_;
    std::mutex mutex_;
    std::map<llvm::orc::MaterializationResponsibility*,
             std::vector<Symbol>> pending_;
    std::map<llvm::orc::ResourceKey,
             std::vector<std::unique_ptr<OrcFdeLease>>> active_;
};

inline void installOrcFdeLifecycleProbe(llvm::orc::LLJIT& jit,
                                         const char* tier) {
    const char* flag = std::getenv("XYO_RT_ORC_FDE_REGISTRATION");
    if (!flag || flag[0] != '1') return;
    auto plugin = OrcJitFdeLifecyclePlugin::create();
    if (!plugin) {
        std::fprintf(stderr,
            "[rt001-orc-fde] phase=install tier=%s unavailable=1 "
            "gc_safe=0\n", tier);
        return;
    }
    // LLVM23 LLJIT uses ObjectLinkingLayer on Linux x86-64.
    auto& layer = static_cast<llvm::orc::ObjectLinkingLayer&>(
        jit.getObjLinkingLayer());
    layer.addPlugin(std::move(plugin));
    std::fprintf(stderr,
        "[rt001-orc-fde] phase=install tier=%s ready=1 gc_safe=0\n",
        tier);
}

} // namespace xyo::rt
