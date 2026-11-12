// RT-001: sampled PC of a suspended REAL LLVM native Fiber.
// Linux x86-64 SysV's FiberRegs.rip is the return from
// xyo_fiber_switch, NOT an LLVM StackMap program counter.
// This must never be treated as a machine root location.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/engine/runtime.hpp"
#include "../../src/engine/thread.hpp"
#include "../../src/engine/fiber.hpp"
#include "../../src/jit/jit.hpp"
#include "../../src/load/sb3.hpp"
#include "../../src/model/target.hpp"

#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <sys/ucontext.h>
#include <dlfcn.h>
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using namespace xyo;

static void env(const char* name, const char* value) {
    assert(setenv(name, value, 1) == 0);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    env("XYO_JIT_MODE", "eager");
    env("XYO_JIT_LAZY_MODE", "off");
    env("XYO_JIT_CALLFRAME", "1");
    env("XYO_JIT_SSA_LIVENESS", "1");
    env("XYO_RT_CAPTURE_LLVM_SSA", "1");
    env("XYO_JIT_LAZY_TYPED_SSA", "1");
    env("XYO_RT_REQUIRE_GENERATED_SSA_ROOTS", "1");
    env("XYO_RT_MACHINE_STACKMAP_PROBE", "1");
    env("XYO_PAR", "0");
    env("XYO_PARFOR", "0");
    env("XYO_GPU", "0");
    env("XYO_JIT_ASYNC", "0");
    setJitMode(JitMode::Eager);

    std::ifstream file(argv[1], std::ios::binary);
    assert(file);
    std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()};
    assert(!bytes.empty());

    Runtime rt;
    auto loaded = loadSb3File(rt, bytes.data(), bytes.size());
    assert(loaded.ok);
    rt.currentStepTime = 0.25;
    rt.greenFlag();
    unsigned observed = 0;
    unsigned withCapture = 0;
    for (unsigned step = 0; step < 15000; ++step) {
        if (rt.threads.empty()) break;
        rt.step();
        observed = withCapture = 0;
        for (Thread* t : rt.threads) {
            if (!t || !t->isCompiled || !t->fiber ||
                !t->fiber->started() || t->fiber->finished())
                continue;
            const auto* compiled =
                static_cast<const CompiledScript*>(t->compiled);
            if (!compiled || !compiled->entry) continue;
            const uintptr_t savedPc =
                static_cast<uintptr_t>(t->fiber->regs()->rip);
            const uintptr_t jitEntry =
                reinterpret_cast<uintptr_t>(compiled->entry);
            assert(savedPc && jitEntry);
            Dl_info savedInfo{}, jitInfo{};
            const bool hostText =
                dladdr(reinterpret_cast<const void*>(savedPc),
                       &savedInfo) != 0 && savedInfo.dli_fname;
            const bool jitHasElfOwner =
                dladdr(reinterpret_cast<const void*>(jitEntry),
                       &jitInfo) != 0;
            // In a real SysV Fiber context switch, saved RIP resolves
            // to host text rather than anonymous ORC JIT code.
            if (!hostText || jitHasElfOwner) {
                std::cerr << "FAIL RT001 PC: hostText=" << hostText
                          << " jitElf=" << jitHasElfOwner << "\n";
                return 2;
            }
            if (const char* flush = std::getenv("XYO_RT001_UNW_FLUSH_CACHE");
                flush && flush[0] == '1')
                unw_flush_cache(unw_local_addr_space, 0, ~unw_word_t(0));
            // True DWARF-unwinder trial from the actual paused SysV
            // switch record. Unlike raw stack-word scanning this uses
            // FDE rules from the live host/JIT executable images.
            unw_context_t uc{};
            assert(unw_getcontext(&uc) == 0);
            const auto* fr = t->fiber->regs();
            auto& g = uc.uc_mcontext.gregs;
            g[REG_RIP] = fr->rip;
            g[REG_RSP] = fr->rsp;
            g[REG_RBP] = fr->rbp;
            g[REG_RBX] = fr->rbx;
            g[REG_R12] = fr->r12;
            g[REG_R13] = fr->r13;
            g[REG_R14] = fr->r14;
            g[REG_R15] = fr->r15;
            unw_cursor_t cursor{};
            const int init = unw_init_local(&cursor, &uc);
            int cfiFrames = 0, jitFrames = 0, jitCfi = 0;
            int jitOneByteFallbacks = 0;
            uintptr_t jitReturnOffset = 0;
            uintptr_t jitCfiStartOffset = UINTPTR_MAX;
            uintptr_t jitCfiEndOffset = 0;
            uintptr_t previousSp = 0;
            if (init == 0) {
                for (int n = 0; n < 40; ++n) {
                    unw_word_t ip = 0, sp = 0;
                    if (unw_get_reg(&cursor, UNW_REG_IP, &ip) < 0 ||
                        unw_get_reg(&cursor, UNW_REG_SP, &sp) < 0)
                        break;
                    const auto low = reinterpret_cast<uintptr_t>(
                        t->fiber->stackBase()) - (1u << 20);
                    const auto top = reinterpret_cast<uintptr_t>(
                        t->fiber->stackBase());
                    if (sp < low || sp > top ||
                        (previousSp && sp < previousSp))
                        break;
                    previousSp = sp;
                    unw_proc_info_t info{};
                    const bool cfi =
                        unw_get_proc_info(&cursor, &info) == 0 &&
                        info.start_ip <= ip && ip < info.end_ip;
                    cfiFrames += cfi;
                    for (const auto& [key, fn] : compiled->procedures) {
                        const uintptr_t start =
                            reinterpret_cast<uintptr_t>(fn);
                        if (ip >= start && ip - start < 32768) {
                            jitFrames++;
                            // libunwind can synthesize a 1-byte
                            // "procedure" for frame-pointer fallback.
                            // This is NOT the JIT's real DWARF FDE.
                            const bool realJitFde = cfi &&
                                info.start_ip <= start &&
                                info.end_ip > ip &&
                                info.end_ip - info.start_ip > 1;
                            jitCfi += realJitFde;
                            jitOneByteFallbacks += cfi &&
                                info.start_ip == ip &&
                                info.end_ip == ip + 1;
                            jitReturnOffset = ip - start;
                            // RT-001: certify the *address ranges*
                            // of native RBP-relative StackMap spill
                            // locations against this paused Fiber's
                            // mapped stack. This does NOT certify the
                            // ownership or interpretation of their i64
                            // contents. Export distances, never raw
                            // host virtual addresses.
                            if (const char* probe = std::getenv(
                                    "XYO_RT001_YIELD_SPILL_BOUNDS_PROBE");
                                probe && probe[0] == '1') {
                                unw_word_t machineRbp = 0;
                                const int machineRc = unw_get_reg(
                                    &cursor, UNW_X86_64_RBP, &machineRbp);
                                const uintptr_t high =
                                    reinterpret_cast<uintptr_t>(
                                        t->fiber->stackBase());
                                const uintptr_t stackLow =
                                    high - (uintptr_t(1) << 20);
                                const uintptr_t stoppedSp =
                                    static_cast<uintptr_t>(fr->rsp);
                                const bool fpValid = realJitFde &&
                                    machineRc == 0 &&
                                    machineRbp >= stackLow &&
                                    machineRbp < high &&
                                    stoppedSp >= stackLow &&
                                    stoppedSp <= machineRbp;
                                std::cerr
                                    << "[rt001-yield-spill-bounds] thread="
                                    << t->topBlock
                                    << " jit_return_offset="
                                    << jitReturnOffset
                                    << " genuine_fde=" << realJitFde
                                    << " frame_pointer_valid=" << fpValid
                                    << " rbp_depth="
                                    << (fpValid ? high - machineRbp : 0)
                                    << " stopped_sp_depth="
                                    << (fpValid ? high - stoppedSp : 0)
                                    << " stack_capacity=" << (1u << 20)
                                    << " memory_values_proven=0"
                                    << " ownership_proven=0 gc_safe=0\n";
                            }
                            // Test-only: query machine registers at the
                            // REAL JIT caller frame, not at xyo_fiber_switch
                            // and not at a synthetic, raw-stack candidate.
                            // x86-64 R13/R14 are DWARF numbers 13/14,
                            // which can be compared with a StackMap
                            // Register R#13 / Register R#14 location.
                            // Matching JitCtx is ONLY one ABI value,
                            // not a GC-root completeness guarantee.
                            if (const char* probe = std::getenv(
                                    "XYO_RT001_YIELD_REGISTER_PROBE");
                                probe && probe[0] == '1') {
                                unw_word_t r13 = 0, r14 = 0;
                                unw_word_t rbx = 0, r12 = 0;
                                const int rc13 = unw_get_reg(
                                    &cursor, UNW_X86_64_R13, &r13);
                                const int rc14 = unw_get_reg(
                                    &cursor, UNW_X86_64_R14, &r14);
                                const int rcbx = unw_get_reg(
                                    &cursor, UNW_X86_64_RBX, &rbx);
                                const int rc12 = unw_get_reg(
                                    &cursor, UNW_X86_64_R12, &r12);
                                const uintptr_t ctx =
                                    reinterpret_cast<uintptr_t>(t->jitCtx);
                                std::cerr
                                    << "[rt001-yield-register] thread="
                                    << t->topBlock
                                    << " jit_return_offset="
                                    << jitReturnOffset
                                    << " ctx_available=" << (ctx != 0)
                                    << " dwarf13_valid=" << (rc13 == 0)
                                    << " dwarf13_ctx_match="
                                    << (rc13 == 0 && r13 == ctx)
                                    << " dwarf14_valid=" << (rc14 == 0)
                                    << " dwarf14_ctx_match="
                                    << (rc14 == 0 && r14 == ctx)
                                    << " dwarf3_valid=" << (rcbx == 0)
                                    << " dwarf3_ctx_match="
                                    << (rcbx == 0 && rbx == ctx)
                                    << " dwarf12_valid=" << (rc12 == 0)
                                    << " dwarf12_ctx_match="
                                    << (rc12 == 0 && r12 == ctx)
                                    << " genuine_fde=" << realJitFde
                                    << " machine_roots_proven=0 gc_safe=0\n";
                            }
                            if (cfi) {
                                jitCfiStartOffset = info.start_ip - start;
                                jitCfiEndOffset = info.end_ip - start;
                            }
                            break;
                        }
                    }
                    if (unw_step(&cursor) <= 0) break;
                }
            }
            // The GNU unwinder and libunwind may consult distinct
            // runtime unwind registrations. Query GNU libgcc_s's
            // diagnostic FDE lookup against the EXACT active JIT
            // return address, not a stale raw stack-scan candidate.
            // Merely finding GNU FDE bytes is never GC-safe evidence.
            if (const char* probe = std::getenv("XYO_RT001_GNU_FDE_PROBE");
                probe && probe[0] == '1') {
                struct GnuEhBases {
                    void* tbase;
                    void* dbase;
                    void* func;
                };
                using FindFde = void* (*)(void*, GnuEhBases*);
                void* libgcc = dlopen("libgcc_s.so.1",
                                      RTLD_LAZY | RTLD_LOCAL);
                auto* symbol = libgcc
                    ? dlsym(libgcc, "_Unwind_Find_FDE") : nullptr;
                bool found = false;
                bool ownerMatches = false;
                if (symbol && jitReturnOffset > 0 &&
                    !compiled->procedures.empty()) {
                    const auto procedure = reinterpret_cast<uintptr_t>(
                        compiled->procedures.begin()->second);
                    GnuEhBases bases{};
                    auto lookup = reinterpret_cast<FindFde>(symbol);
                    void* fde = lookup(
                        reinterpret_cast<void*>(
                            procedure + jitReturnOffset - 1),
                        &bases);
                    found = fde != nullptr;
                    ownerMatches = found &&
                        reinterpret_cast<uintptr_t>(bases.func) ==
                            procedure;
                    // Standalone diagnostic only: construct ONE
                    // libunwind remote-table entry backed by GNU's
                    // already-registered REAL JIT .eh_frame FDE.
                    // This does not forge/unwind using made-up CFI:
                    // the referenced FDE is the genuine relocated
                    // JIT FDE returned by libgcc_s.
                    if (const char* dyn = std::getenv(
                            "XYO_RT001_LIBUNWIND_DYNAMIC_BRIDGE");
                        dyn && dyn[0] == '1' && ownerMatches) {
                        const intptr_t relative =
                            reinterpret_cast<intptr_t>(fde) -
                            static_cast<intptr_t>(procedure);
                        const bool offsetFits =
                            relative >= INT32_MIN &&
                            relative <= INT32_MAX;
                        bool registeredFde = false;
                        bool correctOwner = false;
                        if (offsetFits) {
                            int32_t table[2] = {
                                0, static_cast<int32_t>(relative)
                            };
                            unw_dyn_info_t dynamicInfo{};
                            dynamicInfo.start_ip = procedure;
                            dynamicInfo.end_ip = procedure + 32768;
                            dynamicInfo.format =
                                UNW_INFO_FORMAT_REMOTE_TABLE;
                            dynamicInfo.u.rti.segbase = procedure;
                            dynamicInfo.u.rti.table_data =
                                reinterpret_cast<unw_word_t>(table);
                            dynamicInfo.u.rti.table_len =
                                sizeof(table) / sizeof(unw_word_t);
                            std::fprintf(stderr, "[rt001-stage] stage_before_old\n");
                            unw_proc_info_t oldInfo{};
                            const int oldQuery =
                                unw_get_proc_info_by_ip(
                                    unw_local_addr_space,
                                    procedure + jitReturnOffset - 1,
                                    &oldInfo, nullptr);
                            const bool oldRealFde =
                                oldQuery == 0 &&
                                oldInfo.start_ip == procedure &&
                                oldInfo.end_ip - oldInfo.start_ip > 64;
                            std::fprintf(stderr, "[rt001-stage] stage_before_register\n");
                            _U_dyn_register(&dynamicInfo);
                            unw_flush_cache(
                                unw_local_addr_space, 0,
                                ~unw_word_t(0));
                            std::fprintf(stderr, "[rt001-stage] stage_before_cursor\n");
                            unw_proc_info_t registered{};
                            const int query = unw_get_proc_info(
                                &cursor, &registered);
                            std::fprintf(stderr, "[rt001-stage] stage_before_direct\n");
                            unw_proc_info_t directInfo{};
                            const int directQuery =
                                unw_get_proc_info_by_ip(
                                    unw_local_addr_space,
                                    procedure + jitReturnOffset - 1,
                                    &directInfo, nullptr);
                            const bool directFound = directQuery == 0 &&
                                directInfo.start_ip <= procedure +
                                    jitReturnOffset - 1 &&
                                directInfo.end_ip > procedure +
                                    jitReturnOffset - 1 &&
                                directInfo.end_ip -
                                    directInfo.start_ip > 64;
                            // Recreate the cursor from the SAVED
                            // original FiberRegs-derived ucontext.
                            // An existing cursor can cache a synthesized
                            // one-byte procedure even after registration.
                            std::fprintf(stderr, "[rt001-stage] stage_before_fresh\n");
                            unw_cursor_t fresh{};
                            const int newInit =
                                unw_init_local(&fresh, &uc);
                            bool newCursorRealFde = false;
                            int newCursorSteps = 0;
                            if (newInit == 0) {
                                for (int n = 0; n < 40; ++n) {
                                    unw_word_t newPc = 0;
                                    if (unw_get_reg(&fresh, UNW_REG_IP,
                                                    &newPc) < 0)
                                        break;
                                    if (newPc >= procedure &&
                                        newPc - procedure < 32768) {
                                        unw_proc_info_t newInfo{};
                                        newCursorRealFde =
                                            unw_get_proc_info(
                                                &fresh, &newInfo) == 0 &&
                                            newInfo.start_ip == procedure &&
                                            newInfo.end_ip >
                                                newPc &&
                                            newInfo.end_ip -
                                                newInfo.start_ip > 64;
                                        break;
                                    }
                                    if (unw_step(&fresh) <= 0) break;
                                    ++newCursorSteps;
                                }
                            }
                            std::fprintf(stderr, "[rt001-stage] stage_after_fresh\n");
                            std::cerr
                                << "[rt001-libunwind-direct] thread="
                                << t->topBlock
                                << " cursor_rc=" << query
                                << " by_ip_rc=" << directQuery
                                << " by_ip_real_fde=" << directFound
                                << " before_registered=" << oldRealFde
                                << " fresh_cursor_fde="
                                << newCursorRealFde
                                << " fresh_steps=" << newCursorSteps
                                << " runtime_fde_bytes="
                                << (directFound ? directInfo.end_ip -
                                    directInfo.start_ip : 0)
                                << " safe=0\n";
                            registeredFde = directFound;
                            correctOwner =
                                registeredFde &&
                                directInfo.start_ip == procedure;
                            std::cerr
                                << "[rt001-libunwind-bridge] thread="
                                << t->topBlock
                                << " offset_fits=" << offsetFits
                                << " fde_found=" << registeredFde
                                << " owner_matches=" << correctOwner
                                << " native_safepoint_verified=0"
                                << " gc_safe=0\n";
                            std::fprintf(stderr, "[rt001-stage] stage_before_cancel\n");
                            _U_dyn_cancel(&dynamicInfo);
                            unw_flush_cache(
                                unw_local_addr_space, 0,
                                ~unw_word_t(0));
                            // The dynamic registration references
                            // stack-local search-table bytes. It must
                            // be removed before the local data dies.
                            // Query a NEW cursor after cancellation,
                            // never keep a borrowed FDE handle alive.
                            unw_proc_info_t cancelled{};
                            const int cancelledQuery =
                                unw_get_proc_info_by_ip(
                                    unw_local_addr_space,
                                    procedure + jitReturnOffset - 1,
                                    &cancelled, nullptr);
                            const bool cancelledStillReal =
                                cancelledQuery == 0 &&
                                cancelled.start_ip == procedure &&
                                cancelled.end_ip - cancelled.start_ip > 64;
                            std::cerr
                                << "[rt001-libunwind-cancel] thread="
                                << t->topBlock
                                << " real_fde_after_cancel="
                                << cancelledStillReal
                                << " machine_roots_proven=0 gc_safe=0\n";
                            if (oldRealFde || !registeredFde ||
                                !correctOwner || !newCursorRealFde ||
                                cancelledStillReal) {
                                std::cerr
                                    << "FAIL RT001: transient JIT FDE "
                                    << "registration/in-memory-read\n";
                                return 5;
                            }
                        } else {
                            std::cerr
                                << "[rt001-libunwind-bridge] thread="
                                << t->topBlock
                                << " offset_fits=0 fde_found=0"
                                << " owner_matches=0"
                                << " native_safepoint_verified=0"
                                << " gc_safe=0\n";
                        }
                    }
                }
                std::cerr << "[rt001-gnu-fde] thread="
                          << t->topBlock
                          << " lookup_available=" << bool(symbol)
                          << " registered_fde_found=" << found
                          << " owner_matches=" << ownerMatches
                          << " machine_frame_verified=0 gc_safe=0\n";
                if (libgcc) dlclose(libgcc);
            }
            std::cerr << "[rt001-cfi-trace] thread=" << t->topBlock
                      << " init=" << init << " cfi_frames=" << cfiFrames
                      << " jit_frames=" << jitFrames
                      << " jit_cfi=" << jitCfi
                      << " one_byte_fallbacks=" << jitOneByteFallbacks
                      << " jit_return_offset=" << jitReturnOffset
                      << " jit_cfi_start=" << jitCfiStartOffset
                      << " jit_cfi_end=" << jitCfiEndOffset
                      << " machine_frame_verified="
                      << (jitFrames == 1 && jitCfi == 1 &&
                          jitReturnOffset != 0)
                      << " stackmap_pc_matched=0 gc_safe=0\n";
            // A recovered return PC is useful, but this build
            // gives libunwind only a synthetic 1-byte JIT range.
            // Refuse to claim a fully CFI-certified machine frame.
            const bool validatedFde =
                jitCfi == 1 && jitOneByteFallbacks == 0;
            const bool syntheticFallback =
                jitCfi == 0 && jitOneByteFallbacks == 1 &&
                jitCfiStartOffset == jitReturnOffset &&
                jitCfiEndOffset == jitReturnOffset + 1;
            if (init != 0 || cfiFrames < 3 || jitFrames != 1 ||
                (!validatedFde && !syntheticFallback) ||
                jitReturnOffset == 0) {
                std::cerr << "FAIL RT001: unwinder regression or "
                          << "unexpected DWARF-FDE ownership\n";
                return 4;
            }
            ++observed;
            if (t->nativeRtSsaCapture) ++withCapture;
            if (t->nativeRtSsaCapture) {
                // This is a diagnostic SEARCH for possible return
                // addresses, not stack unwinding: values can be stale,
                // duplicated or unrelated pointer-sized words.
                unsigned candidateReturns = 0;
                uintptr_t firstCandidate = 0;
                const auto sp = reinterpret_cast<uintptr_t>(
                    t->fiber->suspendedSp());
                const auto base = reinterpret_cast<uintptr_t>(
                    t->fiber->stackBase());
                if (sp != 0 && sp < base && base - sp < (1u << 20)) {
                    for (uintptr_t scan = (sp + 7) & ~uintptr_t(7);
                         scan + 8 <= base; scan += 8) {
                        uintptr_t word = *reinterpret_cast<uintptr_t*>(
                            scan);
                        for (const auto& [key, fn] :
                             compiled->procedures) {
                            const auto start =
                                reinterpret_cast<uintptr_t>(fn);
                            // Large conservative diagnostic interval
                            // only. This CANNOT certify a function's
                            // executable extent or a call-frame owner.
                            if (word >= start &&
                                word - start < 32768) {
                                ++candidateReturns;
                                if (!firstCandidate)
                                    firstCandidate = word - start;
                                break;
                            }
                        }
                    }
                }
                std::cerr << "[rt001-fiber-pc] thread=" << t->topBlock
                          << " saved_pc_class=host_native_yield"
                          << " jit_entry_class=orc_generated"
                          << " stackmap_pc_matched=0"
                          << " last_source_site="
                          << t->nativeRtSsaCapture->siteId
                          << " procedure_count="
                          << compiled->procedures.size()
                          << " potential_stack_returns="
                          << candidateReturns
                          << " first_return_offset="
                          << firstCandidate
                          << " machine_frame_verified=0 gc_safe=0\n";
            }
        }
        if (observed == 2 && withCapture == 2) break;
        // Observe only after both Fibers have detached source captures
        // and remain suspended. Never confuse a recent site with PC.
    }
    if (observed != 2 || withCapture != 2) {
        std::cerr << "FAIL RT001: no two live captured native Fibers"
                  << " observed=" << observed
                  << " captured=" << withCapture << "\n";
        return 3;
    }
    assert(FiberPool::unsafeCount() >= 2);
    // Recheck that temporary stack-local libunwind records were
    // unregistered before these Fibers run again. Their normal
    // compiled continuation must not reference expired FDE tables.
    if (const char* resume = std::getenv(
            "XYO_RT001_RESUME_AFTER_CANCEL");
        resume && resume[0] == '1') {
        unsigned steps = 0;
        while (!rt.threads.empty() && steps < 250000) {
            rt.step();
            ++steps;
        }
        if (!rt.threads.empty()) {
            std::cerr << "FAIL RT001: resumed Fiber did not finish "
                      << steps << " ticks\n";
            return 6;
        }
        bool matched = false;
        for (Target* target : rt.targets) {
            if (!target || !target->sprite ||
                target->sprite->name != u"Bench")
                continue;
            Variable* y = target->lookupVariableById("v_y");
            matched = y && y->value.isNumber() &&
                y->value.asNumberUnchecked() == 48005.0;
        }
        if (!matched) {
            std::cerr << "FAIL RT001: variable parity after temporary "
                      << "FDE cancellation\n";
            return 7;
        }
        std::cerr << "[rt001-libunwind-resume] completed=1 y=48005"
                  << " dynamic_fde_cancelled=1 gc_safe=0\n";
    }
    std::cout
        << "PASS RT-001 saved Fiber RIPs are host switch return PCs; "
        << "source checkpoint is available, but no matching StackMap "
        << "safepoint; GC remains disabled\n";
}
