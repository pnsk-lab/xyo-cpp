// Native PLaMo Fiber GC safety: a suspended LLVM frame must postpone
// explicit collections until it is retired by stopAll.
#include "core/gc.hpp"
#include "core/sstr.hpp"
#include "engine/fiber.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "engine/translate_service.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

// Release builds define NDEBUG; these checks must remain active.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

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
    std::ifstream stream(argv[1], std::ios::binary);
    assert(stream.good());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    assert(!bytes.empty());
    auto& gc = Gc::get();
    const uint64_t initialCollections = gc.collectCount();
    {
        Runtime rt;
        const auto load = loadSb3File(rt, bytes.data(), bytes.size());
        assert(load.ok);
        rt.greenFlag();
        bool suspended = false;
        for (int i = 0; i < 50 && !suspended; ++i) {
            rt.step();
            if (!rt.translationService || !rt.translationService->hasPending())
                continue;
            for (Thread* t : rt.threads) {
                if (t->fiber && t->status == STATUS_PROMISE_WAIT &&
                    !t->isKilled) {
                    suspended = true;
                    break;
                }
            }
        }
        assert(suspended && "expected an actual native Fiber awaiting HTTP");
        assert(FiberPool::unsafeCount() > 0);
        const uint64_t count = gc.collectCount();
        const size_t bytesBeforePressure = gc.bytesAllocated();
        // Allocate 10,000 distinct GC-managed strings on the runtime
        // thread while LLVM SSA is suspended in another Fiber. These
        // objects are intentionally ephemeral, not stored in Scratch.
        constexpr int kPressureStrings = 10000;
        for (int i = 0; i < kPressureStrings; ++i) {
            std::u16string data(160, u'x');
            auto suffix = std::to_string(i);
            for (size_t j = 0; j < suffix.size(); ++j)
                data[data.size() - suffix.size() + j] =
                    static_cast<char16_t>(suffix[j]);
            SStr* str = SStr::create(data);
            assert(str && str->len == data.size());
            if ((i % 500) == 0) {
                gc.collect();
                gc.collectIfNeeded();
                assert(gc.collectCount() == count);
            }
        }
        const size_t bytesDuringPressure = gc.bytesAllocated();
        assert(bytesDuringPressure > bytesBeforePressure + 1000000);
        std::printf("PASS 10000 distinct GC strings pressure_kib=%zu\n",
                    (bytesDuringPressure - bytesBeforePressure) / 1024);
        for (int i = 0; i < 128; ++i) {
            gc.collect();
            gc.collectIfNeeded();
            assert(gc.collectCount() == count &&
                   "collected while native Fiber held uncheckpointed SSA");
        }
        assert(gc.explicitCollectionPending());
        std::printf("PASS 128 forced collections deferred, active native=%zu\n",
                    FiberPool::unsafeCount());
        // Give the localhost mock time to accept the in-flight request
        // before stopAll cancels it; the GC assertions above remain valid.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        rt.stopAll();
        // The next scheduler tick finishes / retires detached Fiber state.
        for (int i = 0; i < 10 && FiberPool::unsafeCount(); ++i)
            rt.step();
        assert(FiberPool::unsafeCount() == 0);
        gc.collectIfNeeded();
        assert(gc.collectCount() > count);
        assert(!gc.explicitCollectionPending());
        const size_t bytesAfterCollection = gc.bytesAllocated();
        std::printf("MEM_GC before_kib=%zu suspended_kib=%zu after_kib=%zu\n",
                    bytesBeforePressure / 1024, bytesDuringPressure / 1024,
                    bytesAfterCollection / 1024);
        assert(bytesAfterCollection < bytesDuringPressure &&
               "GC-managed string pressure did not drain after retirement");
        std::puts("PASS deferred GC drained only after native Fiber retirement");
    }
    assert(FiberPool::unsafeCount() == 0);
    assert(gc.collectCount() > initialCollections);
}
