// RT-001: real ORC JITLink lifecycle callback experiment.
// A test-only plugin creates JIT FDE leases when code is EMITTED and
// cancels them in notifyRemovingResources BEFORE code is unmapped.
// Do not confuse unwind metadata with a complete GC root StackMap.
#ifdef NDEBUG
#undef NDEBUG
#endif
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <dlfcn.h>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Error.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {
using llvm::orc::ResourceKey;
using llvm::orc::MaterializationResponsibility;

struct Bases { void* tbase; void* dbase; void* func; };
using FindFde = void* (*)(void*, Bases*);

[[noreturn]] void fail(const char* msg) {
    std::fprintf(stderr, "FAIL RT001 ORC plugin: %s\n", msg);
    std::exit(1);
}
void require(bool okay, const char* msg) {
    if (!okay) fail(msg);
}
void noError(llvm::Error error, const char* msg) {
    if (error) {
        llvm::consumeError(std::move(error));
        fail(msg);
    }
}
template <typename T>
T checked(llvm::Expected<T> value, const char* msg) {
    if (!value) {
        llvm::consumeError(value.takeError());
        fail(msg);
    }
    return std::move(*value);
}

class FdeLease {
public:
    FdeLease(uintptr_t start, size_t size, void* fde) {
        require(start && size >= 2 && size <= 65536 && fde,
                "invalid JIT symbol extent");
        require(start <= UINTPTR_MAX-size, "JIT extent overflow");
        const auto displacement =
            reinterpret_cast<intptr_t>(fde) - static_cast<intptr_t>(start);
        require(displacement >= INT32_MIN && displacement <= INT32_MAX,
                "FDE exceeds libunwind signed 32-bit range");
        table_[0] = 0;
        table_[1] = static_cast<int32_t>(displacement);
        info_.start_ip = start;
        info_.end_ip = start + size;
        info_.format = UNW_INFO_FORMAT_REMOTE_TABLE;
        info_.u.rti.segbase = start;
        info_.u.rti.table_data = reinterpret_cast<unw_word_t>(table_);
        info_.u.rti.table_len = sizeof(table_)/sizeof(unw_word_t);
        _U_dyn_register(&info_);
        unw_flush_cache(unw_local_addr_space, 0, ~unw_word_t{0});
    }
    FdeLease(const FdeLease&) = delete;
    FdeLease& operator=(const FdeLease&) = delete;
    ~FdeLease() {
        _U_dyn_cancel(&info_);
        unw_flush_cache(unw_local_addr_space, 0, ~unw_word_t{0});
    }
private:
    int32_t table_[2]{};
    unw_dyn_info_t info_{};
};

bool isRegistered(uintptr_t start, size_t size) {
    unw_proc_info_t info{};
    int rc = unw_get_proc_info_by_ip(unw_local_addr_space,
                                     start + 1, &info, nullptr);
    return rc == 0 && info.start_ip == start &&
           info.end_ip == start + size;
}

struct SymbolInfo {
    uintptr_t start;
    size_t size;
};

class RealFdeLifecyclePlugin final
    : public llvm::orc::LinkGraphLinkingLayer::Plugin {
public:
    explicit RealFdeLifecyclePlugin(FindFde fde)
        : findFde_(fde) {}

    void modifyPassConfig(MaterializationResponsibility& mr,
                          llvm::jitlink::LinkGraph&,
                          llvm::jitlink::PassConfiguration& config) override {
        // MR remains valid through notifyEmitted/notifyFailed. A map
        // keyed by MR (rather than ResourceKey) prevents concurrent
        // materializations from stealing each other's pending symbols.
        auto* identity = &mr;
        config.PostFixupPasses.push_back(
            [this, identity](llvm::jitlink::LinkGraph& graph)
                -> llvm::Error {
                std::vector<SymbolInfo> symbols;
                for (auto* symbol : graph.defined_symbols()) {
                    if (!symbol->hasName() ||
                        !symbol->isCallable() ||
                        !(*symbol->getName()).starts_with("rt001_auto_"))
                        continue;
                    const size_t size = symbol->getSize();
                    if (size < 2 || size > 65536)
                        return llvm::make_error<llvm::StringError>(
                            "invalid JIT callback FDE symbol extent",
                            llvm::inconvertibleErrorCode());
                    symbols.push_back({symbol->getAddress().getValue(),
                                       size});
                }
                if (symbols.empty())
                    return llvm::make_error<llvm::StringError>(
                        "no JIT callable symbols survived post-fixup",
                        llvm::inconvertibleErrorCode());
                std::lock_guard lock(mutex_);
                pending_[identity] = std::move(symbols);
                return llvm::Error::success();
            });
    }

    llvm::Error notifyEmitted(
        MaterializationResponsibility& mr) override {
        std::lock_guard lock(mutex_);
        auto item = pending_.find(&mr);
        if (item == pending_.end())
            return llvm::make_error<llvm::StringError>(
                "JIT materialization lacks post-fixup symbols",
                llvm::inconvertibleErrorCode());
        auto symbols = std::move(item->second);
        pending_.erase(item);
        const ResourceKey key =
            mr.getResourceTracker()->getKeyUnsafe();
        auto& leases = active_[key];
        for (const auto& symbol : symbols) {
            Bases bases{};
            void* fde = findFde_(
                reinterpret_cast<void*>(symbol.start+1), &bases);
            if (!fde ||
                reinterpret_cast<uintptr_t>(bases.func) != symbol.start)
                return llvm::make_error<llvm::StringError>(
                    "GNU JIT FDE not registered during notifyEmitted",
                    llvm::inconvertibleErrorCode());
            if (isRegistered(symbol.start, symbol.size))
                return llvm::make_error<llvm::StringError>(
                    "libunwind FDE unexpectedly registered twice",
                    llvm::inconvertibleErrorCode());
            leases.push_back(std::make_unique<FdeLease>(
                symbol.start, symbol.size, fde));
            if (!isRegistered(symbol.start, symbol.size))
                return llvm::make_error<llvm::StringError>(
                    "JIT FDE libunwind registration mismatch",
                    llvm::inconvertibleErrorCode());
            ++registered_;
        }
        return llvm::Error::success();
    }

    llvm::Error notifyFailed(
        MaterializationResponsibility& mr) override {
        std::lock_guard lock(mutex_);
        pending_.erase(&mr);
        return llvm::Error::success();
    }

    llvm::Error notifyRemovingResources(
        llvm::orc::JITDylib&, ResourceKey key) override {
        std::lock_guard lock(mutex_);
        auto item = active_.find(key);
        if (item == active_.end())
            return llvm::Error::success();
        // This callback precedes JITLinkMemoryManager deallocation.
        // Registrations refer to real FDE addresses and MUST be
        // canceled before the memory manager unmaps those addresses.
        removed_ += item->second.size();
        item->second.clear();
        active_.erase(item);
        return llvm::Error::success();
    }

    void notifyTransferringResources(llvm::orc::JITDylib&,
                                    ResourceKey dst,
                                    ResourceKey src) override {
        std::lock_guard lock(mutex_);
        auto item = active_.find(src);
        if (item == active_.end())
            return;
        auto& destination = active_[dst];
        for (auto& lease : item->second)
            destination.push_back(std::move(lease));
        active_.erase(item);
        ++transfers_;
    }

    size_t liveCount() const {
        std::lock_guard lock(mutex_);
        size_t result = 0;
        for (const auto& [key, leases] : active_)
            result += leases.size();
        return result;
    }
    size_t registered() const { return registered_; }
    size_t removed() const { return removed_; }
    size_t transfers() const { return transfers_; }

private:
    FindFde findFde_;
    mutable std::mutex mutex_;
    std::map<MaterializationResponsibility*,
             std::vector<SymbolInfo>> pending_;
    std::map<ResourceKey,
             std::vector<std::unique_ptr<FdeLease>>> active_;
    size_t registered_ = 0;
    size_t removed_ = 0;
    size_t transfers_ = 0;
};

llvm::orc::ThreadSafeModule moduleWithFde(
    const std::string& name, int returnValue) {
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>(name, *context);
    llvm::IRBuilder<> b(*context);
    auto* fn = llvm::Function::Create(
        llvm::FunctionType::get(b.getInt32Ty(), false),
        llvm::GlobalValue::ExternalLinkage, name, *module);
    fn->setUWTableKind(llvm::UWTableKind::Async);
    b.SetInsertPoint(
        llvm::BasicBlock::Create(*context, "entry", fn));
    b.CreateRet(b.getInt32(returnValue));
    return llvm::orc::ThreadSafeModule(
        std::move(module), std::move(context));
}

uintptr_t emit(llvm::orc::LLJIT& jit,
               llvm::orc::ResourceTrackerSP tracker,
               const std::string& symbol, int returnValue) {
    noError(jit.addIRModule(tracker,
                moduleWithFde(symbol, returnValue)),
            "ORC module emit failure");
    const uintptr_t start =
        checked(jit.lookup(symbol), "ORC JIT lookup failed").getValue();
    using Fn = int (*)();
    require(reinterpret_cast<Fn>(start)() == returnValue,
            "compiled JIT function produced incorrect value");
    return start;
}

} // namespace

int main() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    auto jit = checked(llvm::orc::LLJITBuilder().create(),
                       "cannot initialize ORC LLJIT");
    auto* gcc = dlopen("libgcc_s.so.1", RTLD_NOW | RTLD_LOCAL);
    require(gcc != nullptr, "missing GNU unwinder");
    auto findFde = reinterpret_cast<FindFde>(
        dlsym(gcc, "_Unwind_Find_FDE"));
    require(findFde != nullptr, "missing GNU FDE resolver");

    auto plugin = std::make_shared<RealFdeLifecyclePlugin>(findFde);
    auto& linker = static_cast<llvm::orc::ObjectLinkingLayer&>(
        jit->getObjLinkingLayer());
    linker.addPlugin(plugin);

    auto& jd = jit->getMainJITDylib();
    auto first = jd.createResourceTracker();
    auto second = jd.createResourceTracker();
    auto a = emit(*jit, first, "rt001_auto_a", 11);
    auto b = emit(*jit, second, "rt001_auto_b", 19);
    require(plugin->liveCount() == 2 && plugin->registered() == 2,
            "ORC callbacks did not register both module FDEs");
    require(isRegistered(a, 10) && isRegistered(b, 10),
            "post-emission FDE registrations missing");

    noError(first->remove(), "ORC first resource removal failed");
    require(plugin->liveCount() == 1 && plugin->removed() == 1 &&
            !isRegistered(a, 10) && isRegistered(b, 10),
            "first FDE wasn't canceled prior to unrelated code");
    noError(second->remove(), "ORC second resource removal failed");
    require(plugin->liveCount() == 0 && plugin->removed() == 2 &&
            !isRegistered(b, 10),
            "second FDE survived its code resource");

    auto src = jd.createResourceTracker();
    auto dst = jd.createResourceTracker();
    auto moved = emit(*jit, src, "rt001_auto_move", 29);
    src->transferTo(*dst);
    noError(src->remove(), "original transferred tracker remove failed");
    require(plugin->transfers() == 1 && plugin->liveCount() == 1 &&
            isRegistered(moved, 10),
            "FDE registration didn't follow ORC code transfer");
    noError(dst->remove(), "transferred tracker remove failed");
    require(plugin->liveCount() == 0 &&
            !isRegistered(moved, 10),
            "FDE leaked after new owner removal");

    for (int i = 0; i < 5; ++i) {
        auto tracker = jd.createResourceTracker();
        const std::string name = "rt001_auto_cycle_" +
                                 std::to_string(i);
        auto ptr = emit(*jit, tracker, name, 30+i);
        require(isRegistered(ptr, 10), "reload didn't register FDE");
        noError(tracker->remove(), "cyclic resource remove failed");
        require(plugin->liveCount() == 0 &&
                !isRegistered(ptr, 10),
                "generation-specific FDE survived code removal");
    }
    require(plugin->registered() == plugin->removed(),
            "FDE registration/unregistration counts unequal");
    std::printf(
        "PASS RT-001 ORC JITLink lifecycle plugin: "
        "emitted=%zu removed=%zu transferred=%zu live=%zu "
        "machine_roots_proven=0 gc_safe=0\n",
        plugin->registered(), plugin->removed(),
        plugin->transfers(), plugin->liveCount());
    dlclose(gcc);
}
