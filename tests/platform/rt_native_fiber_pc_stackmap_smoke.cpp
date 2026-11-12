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
    std::cout
        << "PASS RT-001 saved Fiber RIPs are host switch return PCs; "
        << "source checkpoint is available, but no matching StackMap "
        << "safepoint; GC remains disabled\n";
}
