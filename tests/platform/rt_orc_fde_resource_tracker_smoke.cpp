// RT-001: A real LLVM ORC ResourceTracker lifetime experiment.
//
// The scoped lease owns libunwind's dynamic registration, but NOT the
// underlying JIT code/FDE. Its destructor MUST cancel the registration
// before ResourceTracker::remove() can unmap ORC code. This standalone
// diagnostic never enables native Fiber GC or installs a runtime hook.
//
// Linux x86-64 / LLVM 23 / GNU libgcc_s / libunwind only.
#ifdef NDEBUG
#undef NDEBUG
#endif
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <dlfcn.h>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

struct GnuBases {
    void* tbase;
    void* dbase;
    void* func;
};
using FindGnuFde = void* (*)(void*, GnuBases*);

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "FAIL RT-001 ORC FDE lifecycle: %s\n", message);
    std::exit(1);
}
void must(bool ok, const char* message) {
    if (!ok) fail(message);
}
void mustNoError(llvm::Error err, const char* message) {
    if (err) {
        llvm::consumeError(std::move(err));
        fail(message);
    }
}
template<class T> T expected(llvm::Expected<T> result, const char* msg) {
    if (!result) {
        llvm::consumeError(result.takeError());
        fail(msg);
    }
    return std::move(*result);
}

struct FdeLease {
    // IMPORTANT: libunwind retains pointers to BOTH fields until cancel.
    int32_t indexedFde[2]{};
    unw_dyn_info_t registration{};
    bool active = false;
    uintptr_t functionStart = 0;
    uintptr_t functionEnd = 0;

    FdeLease(uintptr_t start, void* fde, size_t executableBound)
        : functionStart(start), functionEnd(start + executableBound) {
        must(start && fde && executableBound >= 64 &&
             functionEnd > functionStart, "bad FDE registration parameters");
        const intptr_t displacement =
            reinterpret_cast<intptr_t>(fde) -
            static_cast<intptr_t>(start);
        must(displacement >= INT32_MIN && displacement <= INT32_MAX,
             "FDE beyond supported signed 32-bit distance");
        indexedFde[0] = 0;
        indexedFde[1] = static_cast<int32_t>(displacement);
        registration.start_ip = functionStart;
        registration.end_ip = functionEnd;
        registration.format = UNW_INFO_FORMAT_REMOTE_TABLE;
        registration.u.rti.segbase = functionStart;
        registration.u.rti.table_data =
            reinterpret_cast<unw_word_t>(indexedFde);
        registration.u.rti.table_len =
            sizeof(indexedFde) / sizeof(unw_word_t);
        _U_dyn_register(&registration);
        active = true;
        unw_flush_cache(unw_local_addr_space, 0, ~unw_word_t{0});
    }
    FdeLease(const FdeLease&) = delete;
    FdeLease& operator=(const FdeLease&) = delete;
    ~FdeLease() { close(); }

    void close() {
        if (!active) return;
        _U_dyn_cancel(&registration);
        active = false;
        unw_flush_cache(unw_local_addr_space, 0, ~unw_word_t{0});
    }
};

bool libunwindKnowsRealJitFde(uintptr_t functionStart) {
    // libunwind's ip-to-FDE lookup uses the *interior* instruction PC.
    // A tiny leaf JIT has a real 10-byte unwind extent, which must not
    // be confused with the old synthetic 1-byte cursor fallback.
    const uintptr_t pc = functionStart + 1;
    unw_proc_info_t info{};
    const int rc = unw_get_proc_info_by_ip(
        unw_local_addr_space, pc, &info, nullptr);
    return rc == 0 && info.start_ip == functionStart &&
           info.end_ip > pc && info.end_ip - info.start_ip >= 2 &&
           info.end_ip - functionStart <= 256;
}

struct TrackedModule {
    llvm::orc::ResourceTrackerSP tracker;
    // This vector always releases *before* tracker->remove().
    std::vector<std::unique_ptr<FdeLease>> leases;
    bool removed = false;

    explicit TrackedModule(llvm::orc::ResourceTrackerSP resource)
        : tracker(std::move(resource)) {}

    TrackedModule(const TrackedModule&) = delete;
    TrackedModule& operator=(const TrackedModule&) = delete;
    ~TrackedModule() {
        // No implicit unmapping from a destructor that cannot return an
        // Error: tests explicitly call remove() and verify the result.
        if (!removed) std::abort();
    }

    void addLease(uintptr_t start, void* fde) {
        must(!removed, "cannot acquire lease on defunct ResourceTracker");
        // This test intentionally emits tiny leaf functions. Limit
        // libunwind's lookup interval to 256B rather than 32KiB to
        // avoid overlapping independent ORC code allocations. A
        // production registrar must use the actual FDE PC range.
        leases.push_back(std::make_unique<FdeLease>(start, fde, 256));
    }

    void transferTo(TrackedModule& destination) {
        must(!removed && !destination.removed && this != &destination,
             "invalid ORC tracker transfer");
        // ORC transfers code ownership; the FDE leases must follow
        // exactly that same lifetime boundary.
        tracker->transferTo(*destination.tracker);
        for (auto& lease : leases)
            destination.leases.push_back(std::move(lease));
        leases.clear();
    }

    void remove() {
        must(!removed, "resource tracker removed twice");
        leases.clear();
        // Critical invariant: no libunwind FDE registration is allowed to
        // outlive its JITLink memory allocation.
        mustNoError(tracker->remove(), "ORC ResourceTracker remove failed");
        removed = true;
    }
};

llvm::orc::ThreadSafeModule testModule(const std::string& name, int value) {
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>(name, *context);
    llvm::IRBuilder<> b(*context);
    auto* function = llvm::Function::Create(
        llvm::FunctionType::get(b.getInt32Ty(), false),
        llvm::GlobalValue::ExternalLinkage, name, *module);
    // An explicit unwind table requires JITLink to produce a real FDE.
    function->setUWTableKind(llvm::UWTableKind::Async);
    auto* entry = llvm::BasicBlock::Create(*context, "entry", function);
    b.SetInsertPoint(entry);
    b.CreateRet(b.getInt32(value));
    return llvm::orc::ThreadSafeModule(
        std::move(module), std::move(context));
}

uintptr_t materializeAndRegister(llvm::orc::LLJIT& jit,
                                 TrackedModule& owner,
                                 const std::string& name, int value,
                                 FindGnuFde findFde) {
    mustNoError(jit.addIRModule(owner.tracker, testModule(name, value)),
                "ORC module submission failed");
    const auto address = expected(jit.lookup(name), "ORC symbol missing");
    const uintptr_t start = address.getValue();
    GnuBases bases{};
    void* fde = findFde(reinterpret_cast<void*>(start), &bases);
    must(fde && reinterpret_cast<uintptr_t>(bases.func) == start,
         "GNU has no exact JIT-owned FDE");
    // The default LLVM ORC GNU registration is not the registration
    // understood by this libunwind; confirm before installing a lease.
    must(!libunwindKnowsRealJitFde(start),
         "libunwind unexpectedly has an unleased JIT FDE");
    owner.addLease(start, fde);
    must(libunwindKnowsRealJitFde(start),
         "scoped libunwind FDE lease not visible");
    using Entry = int (*)();
    auto fn = reinterpret_cast<Entry>(start);
    must(fn() == value, "materialized native function gave wrong result");
    return start;
}

} // namespace

int main() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::orc::LLJITBuilder builder;
    if (const char* mode = std::getenv("XYO_RT001_CODEGEN_OPT")) {
        auto host = expected(
            llvm::orc::JITTargetMachineBuilder::detectHost(),
            "ORC host codegen builder unavailable");
        if (std::string(mode) == "none") {
            host.setCodeGenOptLevel(llvm::CodeGenOptLevel::None);
        } else if (std::string(mode) == "less") {
            host.setCodeGenOptLevel(llvm::CodeGenOptLevel::Less);
        } else if (std::string(mode) == "default") {
            host.setCodeGenOptLevel(llvm::CodeGenOptLevel::Default);
        } else {
            fail("unknown experimental native codegen level");
        }
        builder.setJITTargetMachineBuilder(std::move(host));
    }
    auto jit = expected(builder.create(), "ORC LLJIT creation failed");
    void* gcc = dlopen("libgcc_s.so.1", RTLD_LAZY | RTLD_LOCAL);
    must(gcc != nullptr, "GNU unwinder missing");
    auto findFde = reinterpret_cast<FindGnuFde>(
        dlsym(gcc, "_Unwind_Find_FDE"));
    must(findFde != nullptr, "_Unwind_Find_FDE missing");

    auto& jd = jit->getMainJITDylib();
    TrackedModule first(jd.createResourceTracker());
    TrackedModule second(jd.createResourceTracker());
    const uintptr_t firstPtr = materializeAndRegister(
        *jit, first, "rt001_fde_lifecycle_a", 17, findFde);
    const uintptr_t secondPtr = materializeAndRegister(
        *jit, second, "rt001_fde_lifecycle_b", 31, findFde);
    must(firstPtr != secondPtr, "ORC module addresses must differ");
    must(libunwindKnowsRealJitFde(firstPtr) &&
         libunwindKnowsRealJitFde(secondPtr),
         "two live FDE registrations not independent");
    std::puts("[rt001-orc-fde] two_allocations=1 two_fdes=1 gc_safe=0");

    first.remove();
    must(!libunwindKnowsRealJitFde(firstPtr),
         "unregistered FDE still visible after tracker removal");
    // ORC's original GNU registration must also follow the
    // ResourceTracker. Querying the *integer address* must not
    // dereference the now-unmapped executable memory.
    GnuBases removedBases{};
    must(!findFde(reinterpret_cast<void*>(firstPtr), &removedBases),
         "ORC kept an invalid GNU FDE after freeing the code resource");
    GnuBases survivingBases{};
    must(findFde(reinterpret_cast<void*>(secondPtr), &survivingBases) &&
         reinterpret_cast<uintptr_t>(survivingBases.func) == secondPtr,
         "other ORC GNU FDE vanished with unrelated tracker");
    must(libunwindKnowsRealJitFde(secondPtr),
         "removing first tracker invalidated second FDE");
    {
        auto removedLookup = jit->lookup("rt001_fde_lifecycle_a");
        if (removedLookup)
            fail("removed ORC symbol erroneously still resolvable");
        llvm::consumeError(removedLookup.takeError());
    }
    second.remove();
    must(!libunwindKnowsRealJitFde(secondPtr),
         "second FDE survived its ResourceTracker removal");
    GnuBases secondRemovedBases{};
    must(!findFde(reinterpret_cast<void*>(secondPtr),
                  &secondRemovedBases),
         "second GNU FDE survived ResourceTracker removal");

    TrackedModule replacement(jd.createResourceTracker());
    const uintptr_t newPtr = materializeAndRegister(
        *jit, replacement, "rt001_fde_lifecycle_a", 101, findFde);
    must(libunwindKnowsRealJitFde(newPtr),
         "recompiled ORC JIT has no newly registered FDE");
    replacement.remove();
    must(!libunwindKnowsRealJitFde(newPtr),
         "replacement FDE survived removal");

    // A resource transfer must also migrate the libunwind lease.
    TrackedModule transferSource(jd.createResourceTracker());
    TrackedModule transferDestination(jd.createResourceTracker());
    const uintptr_t moved = materializeAndRegister(
        *jit, transferSource, "rt001_fde_lifecycle_transferred", 61,
        findFde);
    transferSource.transferTo(transferDestination);
    must(transferSource.leases.empty() &&
         transferDestination.leases.size() == 1,
         "FDE lease did not follow ResourceTracker transfer");
    transferSource.remove();
    must(libunwindKnowsRealJitFde(moved),
         "removing old owner incorrectly canceled transferred FDE");
    transferDestination.remove();
    must(!libunwindKnowsRealJitFde(moved),
         "transferred FDE survived new owner removal");

    // Repeated removal and re-materialization may reuse the SAME
    // executable allocation address (ABA). Registrations must never
    // be keyed only to that address without a tracker generation.
    for (unsigned generation = 0; generation < 4; ++generation) {
        TrackedModule cycle(jd.createResourceTracker());
        const auto name = "rt001_fde_lifecycle_cycle_" +
                          std::to_string(generation);
        const uintptr_t ptr = materializeAndRegister(
            *jit, cycle, name, 70 + int(generation), findFde);
        cycle.remove();
        must(!libunwindKnowsRealJitFde(ptr),
             "FDE registration leaked across code lifetime");
    }

    dlclose(gcc);
    std::puts("PASS RT-001 ORC ResourceTracker: two independent FDEs,"
              " cancel-before-code-free, resource transfer, replacement,"
              " four generational reloads; GC veto unchanged");
}
