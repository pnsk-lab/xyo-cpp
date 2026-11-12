// Real LLVM PLaMo Fiber: repeated green-flag cycles and partial retirement
// must not allow GC while ANY native activation still owns SSA.
#include "core/gc.hpp"
#include "engine/fiber.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "engine/translate_service.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <iterator>
#include <vector>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

// Linux-only RSS sampling (physical resident pages). Does not conflate
// virtual reservations for Fiber stacks with physically resident memory.
static size_t rssKiB() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream values(line.substr(6));
            size_t kib = 0;
            values >> kib;
            return kib;
        }
    }
    return 0;  // unsupported platform
}

int main(int argc, char** argv) {
    using namespace xyo;
    assert(argc == 2);
    setenv("XYO_JIT_PLAMO_FIBER", "1", 1);
    setenv("XYO_PLAMO_TRANSLATE", "1", 1);
    setenv("XYO_RT_REQUIRE_GENERATED_SSA_ROOTS", "1", 1);
    setenv("XYO_JIT_MODE", "eager", 1);
    setenv("XYO_JIT_ASYNC", "0", 1);
    setenv("XYO_PAR", "0", 1);
    setenv("XYO_GPU", "0", 1);
    setJitMode(JitMode::Eager);

    std::ifstream input(argv[1], std::ios::binary);
    assert(input.good());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    assert(!bytes.empty());

    auto& gc = Gc::get();
    Runtime rt;
    assert(loadSb3File(rt, bytes.data(), bytes.size()).ok);
    const int kCycles = [] {
        if (const char* value = std::getenv("XYO_PLAMO_GC_CYCLES")) {
            const int parsed = std::atoi(value);
            if (parsed > 0 && parsed <= 2048) return parsed;
        }
        return 128;
    }();
    const size_t rssBaselineKiB = rssKiB();
    size_t peakRssKiB = rssBaselineKiB;
    size_t rssAfterFirstKiB = 0;
    size_t rssAfterLastKiB = 0;
    size_t peakNative = 0;
    size_t peakOwned = 0;
    size_t peakRecycled = 0;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        // Reset only the test's local translation cache between green flags.
        // This guarantees each cycle exercises actual pending Fibers.
        // Production cache semantics are intentionally unchanged.
        if (cycle != 0) {
            rt.translationService->cancelAll();
            rt.translationService.reset();
        }
        rt.greenFlag();
        // Clone hats start in the next scheduler tick. A native Fiber is
        // only counted after its reporter has issued an actual HTTP request.
        std::vector<Thread*> pending;
        for (int tick = 0; tick < 80; ++tick) {
            rt.step();
            pending.clear();
            for (Thread* thread : rt.threads)
                if (thread && thread->fiber &&
                    thread->status == STATUS_PROMISE_WAIT &&
                    !thread->isKilled)
                    pending.push_back(thread);
            if (pending.size() == 4) break;
        }
        assert(pending.size() == 4 && "expected four suspended native reporters");
        assert(FiberPool::unsafeCount() >= 4);
        peakNative = std::max(peakNative, FiberPool::unsafeCount());
        peakRssKiB = std::max(peakRssKiB, rssKiB());
        peakOwned = std::max(peakOwned, rt.ownedThreads.size());
        peakRecycled = std::max(peakRecycled, rt.reusableThreads.size());
        const uint64_t collected = gc.collectCount();
        for (int i = 0; i < 64; ++i) {
            gc.collect();
            gc.collectIfNeeded();
            assert(gc.collectCount() == collected);
        }
        assert(gc.explicitCollectionPending());

        // Stop only one request. Other suspended Fibers must still
        // protect their LLVM SSA values from an explicit collection.
        rt.stopThread(pending.front());
        assert(FiberPool::unsafeCount() >= 3);
        gc.collect();
        gc.collectIfNeeded();
        assert(gc.collectCount() == collected);
        assert(gc.explicitCollectionPending());

        // Stop all and make the pending GC eligible again. Repeat
        // in the SAME Runtime so no stale thread/fiber survives restart.
        rt.stopAll();
        assert(FiberPool::unsafeCount() == 0);
        gc.collectIfNeeded();
        assert(gc.collectCount() > collected);
        assert(!gc.explicitCollectionPending());
        assert(rt.threads.empty());
        const size_t afterKiB = rssKiB();
        peakRssKiB = std::max(peakRssKiB, afterKiB);
        if (!rssAfterFirstKiB) rssAfterFirstKiB = afterKiB;
        rssAfterLastKiB = afterKiB;
        if ((cycle + 1) % 32 == 0 || cycle + 1 == kCycles) {
            std::printf("MEM cycle=%d rss_kib=%zu peak_rss_kib=%zu "
                        "owned=%zu reusable=%zu fibers=%zu\n",
                        cycle + 1, afterKiB, peakRssKiB,
                        rt.ownedThreads.size(), rt.reusableThreads.size(),
                        FiberPool::activeCount());
        }
    }
    assert(FiberPool::unsafeCount() == 0);
    assert(FiberPool::activeCount() == 0);
    // The Runtime's normal ownership collector has a high-water
    // threshold of 4096. After >4096 cumulative thread creations,
    // unreachable Thread objects must be recycled instead of growing
    // without bound across green flags.
    assert(peakRecycled > 0 && "test did not exercise thread recycling");
    assert(peakOwned < 4096 &&
           "unreachable stopped threads accumulated beyond threshold");
    assert(rt.ownedThreads.size() < 4096 &&
           "stopped native threads were not reclaimed");
    std::printf("PASS %d native multi-Fiber restarts peak_native=%zu "
                "peak_owned_threads=%zu final_owned=%zu "
                "peak_recycled=%zu recycled=%zu\n",
                kCycles, peakNative, peakOwned, rt.ownedThreads.size(),
                peakRecycled, rt.reusableThreads.size());
    std::printf("MEM_SUMMARY baseline_kib=%zu first_kib=%zu "
                "final_kib=%zu peak_kib=%zu growth_kib=%lld "
                "cycles=%d\n",
                rssBaselineKiB, rssAfterFirstKiB, rssAfterLastKiB,
                peakRssKiB,
                static_cast<long long>(rssAfterLastKiB) -
                    static_cast<long long>(rssAfterFirstKiB),
                kCycles);
}
