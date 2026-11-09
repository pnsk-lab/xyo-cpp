// Force a real moving-state boundary in the nonmoving Scratch GC while
// all object ownership exists *only* in the portable handle registry.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/host_effect_abi.hpp"
#include "../../src/ir/gc_value_handles.hpp"
#include "../../src/ir/gc_call_frame_values.hpp"
#include "../../src/core/sstr.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <utility>

using namespace xyo;
using namespace xyo::ir;

static Value str(const char* s) {
    return Value::string(SStr::fromUtf8(s));
}

int main() {
    Gc& gc = Gc::get();
    GcValueHandles roots(gc);
    auto hello = roots.retain(str("Hello "));
    auto world = roots.retain(str("世界 🌟"));
    assert(hello && world && *hello != *world);
    assert(roots.size() == 2);
    assert(!roots.resolve(RootedValueHandle{}));
    assert(!roots.resolve(RootedValueHandle{UINT64_MAX}));

    // No Target::Variable is involved. Explicit root provider must mark
    // both SStr objects even if the original local pointer is gone.
    gc.collect();
    assert(SStr::toUtf8(roots.resolve(*hello)->asStrPtr()) == "Hello ");
    assert(SStr::toUtf8(roots.resolve(*world)->asStrPtr()) == "世界 🌟");

    const auto result = invokeRootedHostEffect(
        roots, HostEffectOp::Join, *hello, *world);
    assert(result);
    gc.collect();
    assert(SStr::toUtf8(roots.resolve(*result)->asStrPtr()) ==
           "Hello 世界 🌟");

    const auto length = invokeRootedHostEffect(
        roots, HostEffectOp::StringLength, *result);
    assert(length && roots.resolve(*length)->asNumberUnchecked() == 11);
    const auto contains = invokeRootedHostEffect(
        roots, HostEffectOp::Contains, *result, *world);
    assert(contains && roots.resolve(*contains)->asBool());

    auto index = roots.retain(Value::num(2));
    assert(index);
    auto letter = invokeRootedHostEffect(
        roots, HostEffectOp::LetterOf, *index, *result);
    assert(letter);
    gc.collect();
    assert(SStr::toUtf8(roots.resolve(*letter)->asStrPtr()) == "e");

    // Invalid enum, invalid compare mode and nonexistent second argument
    // must leave the current rooted table unchanged.
    const std::size_t count = roots.size();
    assert(!invokeRootedHostEffect(roots, static_cast<HostEffectOp>(255),
                                   *hello, *world));
    assert(!invokeRootedHostEffect(roots, HostEffectOp::Compare,
                                   *hello, *world, 9));
    assert(!invokeRootedHostEffect(roots, HostEffectOp::Join, *hello, {}));
    assert(roots.size() == count);

    const auto other = roots.retain(str("replacement"));
    assert(other);
    assert(roots.update(*hello, *roots.resolve(*other)));
    assert(SStr::toUtf8(roots.resolve(*hello)->asStrPtr()) == "replacement");
    assert(roots.release(*other));
    gc.collect();
    assert(SStr::toUtf8(roots.resolve(*hello)->asStrPtr()) == "replacement");

    // No allocator slot ABA: released tokens are forever stale, including
    // after the provider is destroyed and a new arena is constructed.
    const uint64_t oldToken = result->token;
    assert(roots.release(*result));
    assert(!roots.release(*result));
    assert(!roots.resolve(*result));
    {
        GcValueHandles nested(gc);
        auto kept = nested.retain(str("nested"));
        assert(kept && kept->token != oldToken);
        assert(!nested.resolve(*result));
        gc.collect();
        assert(SStr::toUtf8(nested.resolve(*kept)->asStrPtr()) == "nested");
    }

    // Moves preserve unique ownership, detach transfers responsibility to
    // the long-lived CallFrame scheduler, explicit release ends the root.
    auto temp = roots.retain(str("scoped"));
    assert(temp);
    {
        ScopedValueHandle a(roots, *temp);
        ScopedValueHandle b(std::move(a));
        assert(b.get() == *temp);
        gc.collect();
        assert(SStr::toUtf8(roots.resolve(*temp)->asStrPtr()) == "scoped");
        assert(b.detach() == *temp);
    }
    assert(roots.resolve(*temp));
    assert(roots.release(*temp));
    assert(!roots.resolve(*temp));
    assert(roots.release(*hello));
    assert(roots.release(*world));
    assert(roots.release(*length));
    assert(roots.release(*contains));
    assert(roots.release(*index));
    assert(roots.release(*letter));
    assert(roots.size() == 0);

    // Simulate a scheduler-owned recursive CallFrame stack: only the
    // sidecar's opaque handles survive suspended frame boundaries.
    {
        RootedCallFrameValues frames(gc);
        assert(frames.push({})); // root procedure
        for (unsigned depth = 0; depth < 12; ++depth) {
            const uint64_t argv[] = {
                str("recursive 🌟").bits, Value::num(double(depth)).bits,
                Value::boolean(depth & 1).bits
            };
            assert(frames.push(argv));
            // Each frame holds one rooted string and two immediate Values.
            assert(frames.liveHeapRoots() == depth + 1);
            gc.collect();
            const auto saved = frames.argument(depth + 1, 0);
            assert(saved && SStr::toUtf8(saved->asStrPtr()) == "recursive 🌟");
            assert(frames.argument(depth + 1, 1)->asNumberUnchecked() ==
                   double(depth));
            assert(frames.argument(depth + 1, 2)->asBool() == bool(depth & 1));
        }
        assert(!frames.argument(13, 0));
        assert(!frames.argument(4, 3));
        const auto replacement = str("new value");
        assert(frames.update(5, 0, replacement.bits));
        gc.collect();
        assert(SStr::toUtf8(frames.argument(5, 0)->asStrPtr()) == "new value");
        assert(frames.liveHeapRoots() == 12);
        assert(frames.update(5, 0, Value::emptyStr().bits));
        assert(frames.liveHeapRoots() == 11);
        assert(frames.argument(5, 0)->isEmptyString());
        while (frames.depth() > 1) {
            assert(frames.pop());
            gc.collect();
        }
        assert(frames.liveHeapRoots() == 0);
        assert(frames.depth() == 1);
        frames.clear(); // deopt/abort should drop all remaining frames
        assert(frames.depth() == 0);
        assert(!frames.pop());
    }

    std::cout << "PASS rooted Value handles: forced GC, Unicode, "
                 "stale tokens, host effects and ownership" << '\n';
}
