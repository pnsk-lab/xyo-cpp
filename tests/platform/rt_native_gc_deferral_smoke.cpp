// RT-001: deterministic native Fiber auto-GC deferral primitive test.
// The source-certified LLVM handoff uses Gc::DeferCollect for the period
// where a newly generated boxed string may live only in SSA registers.
// This test forces the actual heap threshold to be crossed, proves no
// automatic collect takes place in the scope, and proves auto collection
// is restored afterwards. It does not simulate an LLVM Fiber.
#include "core/gc.hpp"
#include "core/sstr.hpp"
#include "engine/fiber.hpp"
#include "ir/gc_value_handles.hpp"

// Release CMake defines NDEBUG. Keep the checks active in the
// actual production-mode CTest instead of compiling them away.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

int main() {
    using namespace xyo;
    auto& gc = Gc::get();
    constexpr size_t threshold = 1u << 20;
    gc.setThreshold(threshold);

    const std::u16string large(8192, u'R');
    auto* original = SStr::create(large);
    const auto originalValue = Value::string(original);
    const auto originalHeader = reinterpret_cast<GcHeader*>(original) - 1;
    ir::GcValueHandles rooted(gc);
    const auto handle = rooted.retain(originalValue);
    assert(handle);

    const uint64_t before = gc.collectCount();
    size_t afterBytes = 0;
    {
        Gc::DeferCollect defer;
        for (int i = 0; i < 140; ++i) {
            // A large SStr is a real Gc::alloc allocation, not the
            // small-object slab fast path; 140 x ~16 KiB > 2 MiB.
            auto* transient = SStr::create(large);
            assert(transient != nullptr);
        }
        afterBytes = gc.bytesAllocated();
        assert(afterBytes > threshold * 2);
        assert(gc.collectCount() == before);
    }

    // Heap threshold is still exceeded. The very next real large
    // allocation must trigger automatic collection again.
    auto* fresh = SStr::create(large);
    assert(fresh != nullptr);
    assert(gc.collectCount() > before);

    // Survives the collection through an exact root, independent of
    // compiler SSA lifetime or conservative scheduler stack scanning.
    const auto retained = rooted.resolve(*handle);
    assert(retained && retained->bits == originalValue.bits);
    assert(gc.isObject(originalHeader));
    assert(originalHeader->type == GcType::Str);
    // A direct collection request is NOT covered by the older
    // automatic-only DeferCollect. The new native-only scope delays
    // explicit collection too, even across nested helper scopes.
    // Use a high threshold so the pending request (not heap pressure)
    // is the sole reason the next safe-point collect runs.
    gc.setThreshold(128u << 20);
    const uint64_t beforeExplicit = gc.collectCount();
    assert(!gc.explicitCollectionPending());
    {
        Gc::DeferCollect deferAutomatic;
        Gc::DeferExplicitCollect deferExplicit;
        {
            Gc::DeferExplicitCollect nestedExplicit;
            gc.collect();
            assert(gc.collectCount() == beforeExplicit);
            assert(gc.explicitCollectionPending());
        }
        gc.collect();
        gc.collectIfNeeded();
        assert(gc.collectCount() == beforeExplicit);
        assert(gc.explicitCollectionPending());
        // The nested guard is gone, but the outer native activation
        // still disallows any sweeping.
    }
    assert(gc.explicitCollectionPending());
    gc.collectIfNeeded();
    assert(gc.collectCount() == beforeExplicit + 1);
    assert(!gc.explicitCollectionPending());
    assert(rooted.resolve(*handle)->bits == originalValue.bits);
    assert(rooted.release(*handle));

    // The GC barrier counts only native activations that have started
    // and may still resume. A freshly leased Fiber has NO LLVM SSA.
    // After its entry returns, a finished-but-unreleased Fiber also
    // cannot execute any more SSA. Suspended Fibers remain unsafe,
    // including those with no registered Scratch root providers.
    const char* requireRoots =
        std::getenv("XYO_RT_REQUIRE_GENERATED_SSA_ROOTS");
    if (requireRoots && requireRoots[0] == '1') {
        assert(FiberPool::activeCount() == 0);
        assert(FiberPool::unsafeCount() == 0);
        const auto suspendOnce = +[](void*) {
            Fiber* current = currentFiber();
            assert(current);
            current->yield();
        };
        Fiber* first = FiberPool::acquire(suspendOnce, nullptr);
        Fiber* second = FiberPool::acquire(suspendOnce, nullptr);
        assert(first && second);
        assert(FiberPool::activeCount() == 2);
        assert(FiberPool::unsafeCount() == 0);
        const uint64_t beforeNative = gc.collectCount();
        gc.collect(); // Two unstarted Fibers cannot block GC.
        assert(gc.collectCount() == beforeNative + 1);
        assert(!gc.explicitCollectionPending());

        first->resume();
        second->resume();
        assert(FiberPool::unsafeCount() == 2);
        const uint64_t beforeSuspended = gc.collectCount();
        gc.collect();
        assert(gc.collectCount() == beforeSuspended);
        assert(gc.explicitCollectionPending());

        first->resume(); // Entry finished; lease still held.
        assert(first->finished());
        assert(FiberPool::activeCount() == 2);
        assert(FiberPool::unsafeCount() == 1);
        gc.collectIfNeeded();
        assert(gc.collectCount() == beforeSuspended);
        assert(gc.explicitCollectionPending());
        FiberPool::release(first);
        assert(FiberPool::unsafeCount() == 1);
        gc.collectIfNeeded();
        assert(gc.collectCount() == beforeSuspended);

        second->resume();
        assert(second->finished());
        assert(FiberPool::activeCount() == 1);
        assert(FiberPool::unsafeCount() == 0);
        gc.collectIfNeeded(); // Last activation FINISHED, not yet released.
        assert(gc.collectCount() == beforeSuspended + 1);
        assert(!gc.explicitCollectionPending());
        FiberPool::release(second);
        assert(FiberPool::activeCount() == 0);

        // Explicit retirement of an in-flight Fiber also revokes
        // its GC veto. Exercise the pool-reused Fiber state reset.
        Fiber* retired = FiberPool::acquire(suspendOnce, nullptr);
        assert(retired && !retired->started());
        assert(FiberPool::unsafeCount() == 0);
        retired->resume();
        assert(FiberPool::unsafeCount() == 1);
        gc.collect();
        assert(gc.explicitCollectionPending());
        FiberPool::release(retired); // suspended; never resumed again.
        assert(FiberPool::unsafeCount() == 0);
        gc.collectIfNeeded();
        assert(!gc.explicitCollectionPending());
        assert(FiberPool::activeCount() == 0);
    } else {
        // Default-off mode must not pay the new unsafe-Fiber atomic
        // updates even when a Fiber is genuinely suspended.
        const auto suspendOnce = +[](void*) {
            currentFiber()->yield();
        };
        Fiber* ordinary = FiberPool::acquire(suspendOnce, nullptr);
        assert(ordinary);
        ordinary->resume();
        assert(FiberPool::activeCount() == 1);
        assert(FiberPool::unsafeCount() == 0);
        FiberPool::release(ordinary);
        assert(FiberPool::activeCount() == 0);
    }
    std::printf(
        "PASS RT-001: threshold=%zu allocated=%zu automatic GC "
        "deferred during native activation, explicit GC postponed "
        "across nested scopes and fulfilled once at safe point, exact "
        "source SStr root intact\n",
        threshold, afterBytes);
}
