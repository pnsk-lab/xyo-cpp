// RT-001: capture REAL in-flight JIT caller PC at the rooted
// asynchronous reporter, compare with a suspended native Fiber frame.
// The saved RIP belongs to the host switch. This is NOT a GC safepoint.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/engine/runtime.hpp"
#include "../../src/engine/thread.hpp"
#include "../../src/engine/fiber.hpp"
#include "../../src/engine/translate_service.hpp"
#include "../../src/jit/jit.hpp"
#include "../../src/load/sb3.hpp"

#ifdef XYO_RT001_HAVE_LIBUNWIND
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <sys/ucontext.h>
#endif

#include <dlfcn.h>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <thread>
#include <chrono>
#include <vector>

using namespace xyo;

// This symbol is always present in LLVMBackend: plugin OFF returns 0,
// plugin ON queries only its currently live ORC ResourceTracker leases.
extern "C" int xyo_rt001_orc_fde_live_pc(
    uintptr_t pc, uintptr_t* start, uintptr_t* end);

#ifdef XYO_RT001_HAVE_LIBUNWIND
// Borrowed genuine JIT .eh_frame FDE, owned by ORC until Runtime
// teardown. This strictly test-local libunwind registration holds a
// stable remote-table pointer and is always cancelled BEFORE ORC
// removes JIT code. Not a GC root or a native Fiber safepoint.
class ScopedTestJitFde {
public:
    ScopedTestJitFde(uintptr_t start, uintptr_t lookupPc, void* fde) {
        if (!start || !fde || lookupPc <= start ||
            lookupPc - start >= 4096) return;
        const auto distance = reinterpret_cast<intptr_t>(fde) -
                              static_cast<intptr_t>(start);
        if (distance < INT32_MIN || distance > INT32_MAX) return;
        table_[0] = 0;
        table_[1] = static_cast<int32_t>(distance);
        info_.start_ip = start;
        // This is a bounded test lookup window, NOT a certified JIT
        // code-size measurement. Require the actual returned FDE to
        // contain the precise lookup PC before treating it as real.
        info_.end_ip = start + 4096;
        info_.format = UNW_INFO_FORMAT_REMOTE_TABLE;
        info_.u.rti.segbase = start;
        info_.u.rti.table_data =
            reinterpret_cast<unw_word_t>(table_);
        // libunwind expects a count of unw_word_t (64-bit words),
        // NOT the number of int32 entries. The pair is ONE 64-bit word.
        info_.u.rti.table_len = sizeof(table_) / sizeof(unw_word_t);
        _U_dyn_register(&info_);
        active_ = true;
        unw_flush_cache(unw_local_addr_space, 0, ~unw_word_t{0});
    }
    ScopedTestJitFde(const ScopedTestJitFde&) = delete;
    ScopedTestJitFde& operator=(const ScopedTestJitFde&) = delete;
    ~ScopedTestJitFde() { cancel(); }

    bool active() const { return active_; }
    void cancel() {
        if (!active_) return;
        _U_dyn_cancel(&info_);
        unw_flush_cache(unw_local_addr_space, 0, ~unw_word_t{0});
        active_ = false;
    }
private:
    int32_t table_[2]{};
    unw_dyn_info_t info_{};
    bool active_ = false;
};
#endif

static uintptr_t inspectPausedFiber(int argc, char** argv) {
    assert(argc >= 3 && argc <= 5); // path, tier, resume?, second?
    bool naturalResume = false, secondWait = false;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "resume") == 0) {
            assert(!naturalResume);
            naturalResume = true;
        } else if (std::strcmp(argv[i], "second") == 0) {
            assert(!secondWait);
            secondWait = true;
        } else assert(false && "unexpected resume/site argument");
    }
    const bool lazy = std::strcmp(argv[2], "lazy") == 0;
    assert(!lazy || !secondWait);
    assert(lazy || std::strcmp(argv[2], "eager") == 0);
    assert(setenv("XYO_JIT_MODE", "eager", 1) == 0);
    assert(setenv("XYO_JIT_LAZY_MODE", lazy ? "on" : "off", 1) == 0);
    assert(setenv("XYO_JIT_PLAMO_FIBER", "1", 1) == 0);
    assert(setenv("XYO_RT_REQUIRE_GENERATED_SSA_ROOTS", "1", 1) == 0);
    assert(setenv("XYO_JIT_PLAMO_ROOT_LIVE_VARS", "1", 1) == 0);
    assert(setenv("XYO_RT_TRACE_ASYNC_CALLSITE_PC", "1", 1) == 0);
    assert(setenv("XYO_RT_POSTOPT_ASYNC_ROOT_AUDIT", "1", 1) == 0);
    assert(setenv("XYO_RT_ASYNC_MACHINE_STACKMAP_PROBE", "1", 1) == 0);
    assert(setenv("XYO_JIT_ASYNC", "0", 1) == 0);
    assert(setenv("XYO_PAR", "0", 1) == 0);
    assert(setenv("XYO_GPU", "0", 1) == 0);
    setJitMode(JitMode::Eager);
    setJitLazyMode(lazy ? JitLazyMode::On : JitLazyMode::Off);

    std::ifstream file(argv[1], std::ios::binary);
    assert(file);
    std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()};
    assert(!bytes.empty());

    Runtime rt;
    const auto load = loadSb3File(rt, bytes.data(), bytes.size());
    assert(load.ok);
    rt.currentStepTime = 0.25;
    rt.greenFlag();
    Thread* parked = nullptr;
    const CompiledScript* compiled = nullptr;
    for (unsigned i = 0; i < 500 && !parked; ++i) {
        rt.step();
        for (Thread* thread : rt.threads) {
            if (!thread || !thread->isCompiled || !thread->compiled ||
                !thread->fiber || !thread->fiber->started() ||
                thread->fiber->finished() || thread->isKilled ||
                thread->status != STATUS_PROMISE_WAIT ||
                thread->nativeAsyncVariableRoots.empty() ||
                !thread->nativeAsyncReporterReturnPc ||
                (secondWait &&
                 thread->nativeAsyncReporterInputWords != 2))
                continue;
            parked = thread;
            compiled = static_cast<const CompiledScript*>(thread->compiled);
            break;
        }
        // A second async translation must be externally resolved
        // before the next site is reached. Never fake completion.
        if (secondWait && !parked)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(3));
    }
    assert(parked && compiled && compiled->entry);
    Fiber* fiber = parked->fiber;
    const uintptr_t callerPc = parked->nativeAsyncReporterReturnPc;
    const uintptr_t helperFp = parked->nativeAsyncReporterFramePtr;
    const uintptr_t savedPc = static_cast<uintptr_t>(fiber->regs()->rip);
    const uintptr_t savedSp =
        reinterpret_cast<uintptr_t>(fiber->suspendedSp());
    const uintptr_t stackTop =
        reinterpret_cast<uintptr_t>(fiber->stackBase());
    const uintptr_t jitEntry =
        reinterpret_cast<uintptr_t>(compiled->entry);

    Dl_info host{}, jit{};
    const bool hostSavedPc = dladdr(reinterpret_cast<void*>(savedPc),
                                    &host) != 0 && host.dli_fname;
    const bool anonymousJitCaller =
        dladdr(reinterpret_cast<void*>(callerPc), &jit) == 0;
    // The root helper's ABI frame was captured while executing inside
    // that Fiber. Validate its return address via the *active* frame
    // pointer, not by scanning arbitrary stale words.
    const bool frameInStack = savedSp && stackTop &&
        savedSp < helperFp &&
        helperFp <= stackTop - 2 * sizeof(uintptr_t) &&
        helperFp % alignof(uintptr_t) == 0;
    uintptr_t physicalReturn = 0;
    if (frameInStack)
        std::memcpy(&physicalReturn,
                    reinterpret_cast<const void*>(
                        helperFp + sizeof(uintptr_t)),
                    sizeof(physicalReturn));
    const bool frameRetMatches = frameInStack &&
        physicalReturn == callerPc;
    uintptr_t jitFramePtr = 0;
    if (frameInStack)
        std::memcpy(&jitFramePtr,
                    reinterpret_cast<const void*>(helperFp),
                    sizeof(jitFramePtr));
    // The rooted bridge is noinline and has a real SysV RBP frame.
    // Its ABI-saved caller RBP is the actual native JIT frame base.
    // A stale stack word with a plausible PC cannot satisfy this
    // active-frame chain plus the direct return-address check above.
    const bool jitFrameInStack =
        frameInStack && jitFramePtr > helperFp &&
        jitFramePtr <= stackTop - 2 * sizeof(uintptr_t) &&
        jitFramePtr % alignof(uintptr_t) == 0;
    const uintptr_t rootBuffer =
        parked->nativeAsyncReporterInputBuffer;
    const uint32_t rootWords =
        parked->nativeAsyncReporterInputWords;
    const bool rootInputInJitFrame =
        jitFrameInStack && rootWords >= 1 && rootWords <= 64 &&
        rootBuffer >= savedSp && rootBuffer < jitFramePtr &&
        uintptr_t(rootWords) * sizeof(uint64_t) <=
            jitFramePtr - rootBuffer;
    const uintptr_t rootInputRbpOffset =
        rootInputInJitFrame ? jitFramePtr - rootBuffer : 0;
    const bool insideEntry =
        callerPc > jitEntry && callerPc - jitEntry < 65536;
    const uintptr_t entryOffset = insideEntry ? callerPc - jitEntry : 0;
    // LLVM's eager entry is the real JIT function, while ORC lazy
    // materialization may hand out a call-through trampoline. Query
    // the genuine registered JIT .eh_frame owner, if available.
    // An FDE is a code-range bound, not proof of a Fiber unwind.
    struct GnuEhBases { void* tbase; void* dbase; void* func; };
    using FindFde = void* (*)(void*, GnuEhBases*);
    void* gcc = dlopen("libgcc_s.so.1", RTLD_LAZY | RTLD_LOCAL);
    auto* findFde = gcc
        ? reinterpret_cast<FindFde>(dlsym(gcc, "_Unwind_Find_FDE"))
        : nullptr;
    GnuEhBases fdeBases{};
    void* fde = findFde
        ? findFde(reinterpret_cast<void*>(callerPc - 1), &fdeBases)
        : nullptr;
    const uintptr_t fdeEntry =
        reinterpret_cast<uintptr_t>(fdeBases.func);
    const bool fdeOwnerMatches = fde && fdeEntry > 0 &&
        callerPc > fdeEntry && callerPc - fdeEntry < 65536;
    const uintptr_t fdeOffset =
        fdeOwnerMatches ? callerPc - fdeEntry : 0;
    if (gcc) dlclose(gcc);

    // Walk the host's real CFI from the paused switch context.
    // At the ORC frame libunwind may fall back to the RBP chain;
    // report jit_cfi=0 rather than claiming a certified JIT unwind.
    // Never use a stale stack-word scan as a register source.
    int unwindInit = -1, unwindFrames = 0, unwindJit = 0;
    int unwindRbxCtx = 0, unwindRbpFrame = 0;
    int unwindSaved = 0, unwindGnuFde = 0;
    uint32_t unwindRegisterMask = 0;
    uint32_t unwindSavedContextDifferenceMask = 0;
    uint64_t fallbackCalleeValues[6]{};
    bool fallbackCalleeValid[6]{};
    // In the two-reporter O3 fixture, the first reporter's live
    // boxed i64 is in DWARF R15 and also in rootBuffer[1] at site 2.
    int unwindLiveI64MatchesRoot = -1;
#ifdef XYO_RT001_HAVE_LIBUNWIND
    unw_context_t context{};
    if (unw_getcontext(&context) == 0) {
        const auto* regs = fiber->regs();
        auto& raw = context.uc_mcontext.gregs;
        raw[REG_RIP] = regs->rip;
        raw[REG_RSP] = regs->rsp;
        raw[REG_RBP] = regs->rbp;
        raw[REG_RBX] = regs->rbx;
        raw[REG_R12] = regs->r12;
        raw[REG_R13] = regs->r13;
        raw[REG_R14] = regs->r14;
        raw[REG_R15] = regs->r15;
        unw_cursor_t cursor{};
        unwindInit = unw_init_local(&cursor, &context);
        uintptr_t lastSp = 0;
        if (unwindInit == 0) {
            for (int n = 0; n < 48; ++n) {
                unw_word_t ip = 0, sp = 0;
                if (unw_get_reg(&cursor, UNW_REG_IP, &ip) < 0 ||
                    unw_get_reg(&cursor, UNW_REG_SP, &sp) < 0)
                    break;
                if (sp < savedSp || sp > stackTop ||
                    (lastSp && sp < lastSp))
                    break;
                lastSp = sp;
                ++unwindFrames;
                if (ip == callerPc) {
                    ++unwindJit;
                    unw_word_t rbx = 0, rbp = 0;
                    if (unw_get_reg(&cursor, UNW_X86_64_RBX, &rbx) == 0)
                        unwindRbxCtx = rbx ==
                            reinterpret_cast<uintptr_t>(parked->jitCtx);
                    if (unw_get_reg(&cursor, UNW_X86_64_RBP, &rbp) == 0)
                        unwindRbpFrame = rbp == jitFramePtr;
                    constexpr int kCalleeRegs[] = {
                        UNW_X86_64_RBX, UNW_X86_64_RBP,
                        UNW_X86_64_R12, UNW_X86_64_R13,
                        UNW_X86_64_R14, UNW_X86_64_R15
                    };
                    const uintptr_t savedContextRegs[] = {
                        regs->rbx, regs->rbp, regs->r12,
                        regs->r13, regs->r14, regs->r15
                    };
                    static_assert(sizeof(kCalleeRegs) / sizeof(int) ==
                                  sizeof(savedContextRegs) /
                                      sizeof(uintptr_t));
                    for (unsigned i = 0; i < 6; ++i) {
                        unw_word_t value = 0;
                        const int number = kCalleeRegs[i];
                        if (unw_get_reg(&cursor, number, &value) < 0)
                            continue;
                        ++unwindSaved;
                        fallbackCalleeValues[i] =
                            static_cast<uint64_t>(value);
                        fallbackCalleeValid[i] = true;
                        unwindRegisterMask |= 1u << number;
                        if (value != savedContextRegs[i])
                            unwindSavedContextDifferenceMask |= 1u << number;
                        if (secondWait && rootInputInJitFrame &&
                            number == UNW_X86_64_R15) {
                            uint64_t rootedWord = 0;
                            std::memcpy(&rootedWord,
                                reinterpret_cast<const void*>(
                                    rootBuffer + sizeof(uint64_t)),
                                sizeof(rootedWord));
                            unwindLiveI64MatchesRoot =
                                static_cast<uint64_t>(value) == rootedWord;
                        }
                    }
                    unw_proc_info_t proc{};
                    unwindGnuFde =
                        unw_get_proc_info(&cursor, &proc) == 0 &&
                        proc.start_ip == fdeEntry &&
                        proc.end_ip > callerPc;
                    break;
                }
                if (unw_step(&cursor) <= 0) break;
            }
        }
    }
#endif
    const bool useOrcPlugin = [] {
        const char* value = std::getenv("XYO_RT001_TEST_USE_ORC_FDE");
        return value && value[0] == '1';
    }();
    const bool useTemporaryFde = [] {
        const char* value = std::getenv("XYO_RT001_TEST_REGISTER_FDE");
        return value && value[0] == '1';
    }();
    assert(!useOrcPlugin || !useTemporaryFde);
    uintptr_t activeStart = 0, activeEnd = 0;
    const bool liveOrcOwner = useOrcPlugin &&
        xyo_rt001_orc_fde_live_pc(
            callerPc - 1, &activeStart, &activeEnd) &&
        activeStart == fdeEntry &&
        activeEnd > callerPc && activeEnd - activeStart <= 4096;

    int fdeRegistered = 0, fdeByIpBefore = -1, fdeByIpDuring = 0;
    int fdeFreshReached = 0, fdeFreshCursor = 0;
    int fdeFreshRegs = 0, fdeFreshRegsMatch = 0;
    int fdeFreshRbxCtx = 0, fdeFreshRbpFrame = 0;
    int fdeFreshLiveI64Root = -1;
    uint64_t varSlotCfiRegisterMask[3]{};
    uint64_t fdeRealCodeBytes = 0;
    int fdeByIpAfter = -1, fdeRegistrationRemoved = 0;
#ifdef XYO_RT001_HAVE_LIBUNWIND
    if ((useTemporaryFde || (useOrcPlugin && liveOrcOwner)) &&
        unwindInit == 0 && fdeOwnerMatches) {
        auto hasRealFde = [&](unw_proc_info_t* result = nullptr) {
            unw_proc_info_t info{};
            const int rc = unw_get_proc_info_by_ip(
                unw_local_addr_space, callerPc - 1, &info, nullptr);
            const bool matches = rc == 0 &&
                info.start_ip == fdeEntry &&
                info.end_ip > callerPc &&
                info.end_ip - info.start_ip > 64 &&
                info.end_ip - info.start_ip <= 4096;
            if (result) *result = info;
            return matches;
        };
        // Never do a libunwind by-IP lookup on unregistered code.
        // The ORC path is guarded by an active ResourceTracker lease.
        // The scoped path registers its own test-only lease.
        {
            std::unique_ptr<ScopedTestJitFde> scoped;
            if (useTemporaryFde)
                scoped = std::make_unique<ScopedTestJitFde>(
                    fdeEntry, callerPc, fde);
            fdeRegistered = scoped ? scoped->active() : liveOrcOwner;
            if (fdeRegistered) {
                unw_proc_info_t direct{};
                fdeByIpDuring = hasRealFde(&direct);
                if (fdeByIpDuring)
                    fdeRealCodeBytes = direct.end_ip - direct.start_ip;
                unw_cursor_t fresh{};
                if (unw_init_local(&fresh, &context) == 0) {
                    uintptr_t lastSp = 0;
                    for (int n = 0; n < 48; ++n) {
                        unw_word_t ip = 0, sp = 0;
                        if (unw_get_reg(&fresh, UNW_REG_IP, &ip) < 0 ||
                            unw_get_reg(&fresh, UNW_REG_SP, &sp) < 0)
                            break;
                        if (sp < savedSp || sp > stackTop ||
                            (lastSp && sp < lastSp))
                            break;
                        lastSp = sp;
                        if (ip == callerPc) {
                            fdeFreshReached = 1;
                            unw_proc_info_t info{};
                            fdeFreshCursor =
                                unw_get_proc_info(&fresh, &info) == 0 &&
                                info.start_ip == fdeEntry &&
                                info.end_ip > callerPc &&
                                info.end_ip - info.start_ip > 64 &&
                                info.end_ip - info.start_ip <= 4096 &&
                                fdeByIpDuring &&
                                info.end_ip == direct.end_ip;
                            constexpr int kCalleeRegs[] = {
                                UNW_X86_64_RBX, UNW_X86_64_RBP,
                                UNW_X86_64_R12, UNW_X86_64_R13,
                                UNW_X86_64_R14, UNW_X86_64_R15
                            };
                            for (unsigned i = 0; i < 6; ++i) {
                                unw_word_t value = 0;
                                if (unw_get_reg(&fresh, kCalleeRegs[i],
                                                &value) < 0)
                                    continue;
                                ++fdeFreshRegs;
                                // Read-only comparison: no arbitrary GC
                                // pointer dereference, root registration
                                // or manipulation of a borrowed Variable*.
                                for (size_t slot = 1; slot <= 2 &&
                                         slot < parked->jitVarSlots.size();
                                     ++slot) {
                                    if (parked->jitVarSlots[slot] &&
                                        uint64_t(value) ==
                                            reinterpret_cast<uintptr_t>(
                                                parked->jitVarSlots[slot]))
                                        varSlotCfiRegisterMask[slot] |=
                                            uint64_t{1} << kCalleeRegs[i];
                                }
                                if (fallbackCalleeValid[i] &&
                                    uint64_t(value) ==
                                        fallbackCalleeValues[i])
                                    ++fdeFreshRegsMatch;
                                if (i == 0)
                                    fdeFreshRbxCtx =
                                        value == reinterpret_cast<uintptr_t>(
                                            parked->jitCtx);
                                if (i == 1)
                                    fdeFreshRbpFrame =
                                        value == jitFramePtr;
                                if (secondWait && i == 5 &&
                                    rootInputInJitFrame) {
                                    uint64_t rootedWord = 0;
                                    std::memcpy(&rootedWord,
                                        reinterpret_cast<const void*>(
                                            rootBuffer + sizeof(uint64_t)),
                                        sizeof(rootedWord));
                                    fdeFreshLiveI64Root =
                                        uint64_t(value) == rootedWord;
                                }
                            }
                            break;
                        }
                        if (unw_step(&fresh) <= 0) break;
                    }
                }
                if (scoped) {
                    scoped->cancel();
                    fdeRegistrationRemoved = !scoped->active();
                }
            }
        }
        // The cancellation proof is the scoped registration lifetime,
        // not an unsafe lookup on unmapped or unregistered JIT FDE.
    }
#endif
    if (const char* flag = std::getenv("XYO_RT001_TEST_VARSLOT_CFI");
        flag && flag[0] == '1') {
        const bool boundArray =
            parked->jitCtx &&
            parked->jitCtx->varSlotData ==
                parked->jitVarSlots.data();
        assert(boundArray && parked->jitVarSlots.size() >= 3);
        for (unsigned slot = 1; slot <= 2; ++slot) {
            // Gc::isObject uses exact registered allocation membership,
            // not speculative dereference of the candidate address.
            const bool gcHeader =
                Gc::get().isObject(parked->jitVarSlots[slot]);
            std::fprintf(stderr,
                "[rt001-async-varslot-cfi] tier=%s site=%u slot=%u "
                "reg_mask=%llu ctx_array=1 gc_header=%u "
                "ownership_proven=0 gc_safe=0\n",
                lazy ? "lazy" : "eager",
                secondWait ? 2u : 1u, slot,
                static_cast<unsigned long long>(
                    varSlotCfiRegisterMask[slot]),
                unsigned(gcHeader));
        }
    }
    std::cerr << "[rt001-async-unwind-fde] tier="
              << (lazy ? "lazy" : "eager")
              << " registration=" << fdeRegistered
              << " owner=" << (useOrcPlugin ? "orc" : "scoped")
              << " orc_live=" << int(liveOrcOwner)
              << " before=" << fdeByIpBefore
              << " during=" << fdeByIpDuring
              << " fresh_reached=" << fdeFreshReached
              << " fresh_cfi=" << fdeFreshCursor
              << " fde_bytes=" << fdeRealCodeBytes
              << " fresh_regs=" << fdeFreshRegs
              << " regs_match=" << fdeFreshRegsMatch
              << " rbx_ctx=" << fdeFreshRbxCtx
              << " rbp_frame=" << fdeFreshRbpFrame
              << " live_i64_root=" << fdeFreshLiveI64Root
              << " canceled=" << fdeRegistrationRemoved
              << " after=" << fdeByIpAfter
              << " machine_roots_proven=0 gc_safe=0\n";

    std::cerr << "[rt001-async-unwind] tier="
              << (lazy ? "lazy" : "eager")
              << " init=" << unwindInit
              << " frames=" << unwindFrames
              << " jit_reached=" << unwindJit
              << " rbx_ctx=" << unwindRbxCtx
              << " rbp_frame=" << unwindRbpFrame
              << " saved_callee_regs=" << unwindSaved
              << " jit_cfi=" << unwindGnuFde
              << " register_mask=" << unwindRegisterMask
              << " saved_context_diff_mask="
              << unwindSavedContextDifferenceMask
              << " live_i64_root_match=" << unwindLiveI64MatchesRoot
              << " rbp_from_saved_sp="
              << (jitFrameInStack ? jitFramePtr - savedSp : 0)
              << " stack_top_from_rbp="
              << (jitFrameInStack ? stackTop - jitFramePtr : 0)
              << " gc_safe=0\n";

    std::cerr << "[rt001-async-paused-caller] tier="
              << (lazy ? "lazy" : "eager")
              << " site=" << (secondWait ? 2 : 1)
              << " saved_pc_host=" << int(hostSavedPc)
              << " jit_caller_anonymous=" << int(anonymousJitCaller)
              << " frame_in_stack=" << int(frameInStack)
              << " frame_return_matched=" << int(frameRetMatches)
              << " jit_frame_in_stack=" << int(jitFrameInStack)
              << " input_in_jit_frame=" << int(rootInputInJitFrame)
              << " input_rbp_offset=" << rootInputRbpOffset
              << " input_words=" << rootWords
              << " entry_region=" << int(insideEntry)
              << " entry_offset=" << entryOffset
              << " fde_owner=" << int(fdeOwnerMatches)
              << " fde_offset=" << fdeOffset
              << " owners=" << parked->nativeAsyncVariableRoots.size()
              << " stackmap_pc_matched=0 machine_frame_verified=0 "
                 "gc_safe=0\n";
    assert(hostSavedPc && anonymousJitCaller &&
           frameInStack && frameRetMatches &&
           jitFrameInStack && rootInputInJitFrame &&
           rootWords == (secondWait ? 2u : 1u) &&
           parked->nativeAsyncVariableRoots.size() == 1);
    assert(FiberPool::unsafeCount() >= 1);

    if (naturalResume) {
        // Resolve the real localhost translation, then let the Fiber
        // return normally. Its PC/frame marker must be restored, not
        // kept as a stale active code location after the final reporter.
        for (unsigned i = 0; i < 400 &&
                 FiberPool::unsafeCount(); ++i) {
            rt.step();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(3));
        }
    } else {
        // Cancellation does not unwind helper's C++ stack. Even if its
        // physical return frame remains in mapped Fiber memory,
        // retirement must clear every debug address and GC owner.
        rt.stopAll();
        for (unsigned i = 0; i < 16 &&
                 FiberPool::unsafeCount(); ++i)
            rt.step();
    }
    assert(FiberPool::unsafeCount() == 0);
    assert(parked->nativeAsyncReporterReturnPc == 0);
    assert(parked->nativeAsyncReporterFramePtr == 0);
    assert(parked->nativeAsyncReporterInputBuffer == 0);
    assert(parked->nativeAsyncReporterInputWords == 0);
    assert(parked->nativeAsyncVariableRoots.empty());
    if (useOrcPlugin) {
        uintptr_t currentStart = 0, currentEnd = 0;
        const bool retained = xyo_rt001_orc_fde_live_pc(
            callerPc - 1, &currentStart, &currentEnd);
        assert(retained && currentStart == fdeEntry &&
               currentEnd == activeEnd && liveOrcOwner &&
               fdeFreshCursor && fdeFreshRegsMatch == 6);
        std::fprintf(stderr,
            "[rt001-orc-fde-index] phase=after_fiber "
            "registered=1 gc_safe=0\n");
    }
    std::cout << "PASS RT-001 paused physical async reporter caller frame: "
              << (lazy ? "lazy" : "eager")
              << " exact return-PC and "
              << (naturalResume ? "normal resume" : "cancellation")
              << " cleanup; GC veto retained\n";
    return callerPc;
}

int main(int argc, char** argv) {
    const char* flag = std::getenv("XYO_RT001_TEST_USE_ORC_FDE");
    const bool usePlugin = flag && flag[0] == '1';
    const char* repeat = std::getenv("XYO_RT001_TEST_RECREATE_RUNTIME");
    const unsigned rounds = repeat && repeat[0] == '1' ? 2u : 1u;
    assert(!repeat || repeat[0] != '1' || usePlugin);
    for (unsigned generation = 0; generation < rounds; ++generation) {
        // Distinct live Runtime/ORC JIT instances in ONE process:
        // the index must not keep a stale registered PC from the
        // previous instance, even if a new JIT reuses its address.
        const uintptr_t callerPc = inspectPausedFiber(argc, argv);
        if (!usePlugin) continue;
        // Runtime/LLVMBackend destroyed: ORC must have removed its
        // exact FDE lease BEFORE unmapping the JIT code. Querying the
        // index, not libunwind, is safe after code destruction.
        const bool leaked = xyo_rt001_orc_fde_live_pc(
            callerPc - 1, nullptr, nullptr);
        std::fprintf(stderr,
            "[rt001-orc-fde-index] phase=after_runtime "
            "registered=%d gc_safe=0\n", int(leaked));
        assert(!leaked && "JIT FDE registered past Runtime destruction");
    }
    return 0;
}
