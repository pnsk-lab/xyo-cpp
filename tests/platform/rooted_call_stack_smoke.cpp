// The real ScratchCallStack + GC-rooted sidecar, tested as one ABI.
// Assertions must remain enabled under CMake Release and Emscripten -O3.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "../../src/ir/gc_call_frame_values.hpp"
#include "../../src/core/sstr.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>

using namespace xyo;
using namespace xyo::ir;

namespace {

uint64_t num(double x) {
    return Value::num(x).bits;
}

uint64_t str(const char* text) {
    return Value::string(SStr::fromUtf8(text)).bits;
}

std::string strValue(Value value) {
    assert(value.isString());
    return SStr::toUtf8(value.asStrPtr());
}

} // namespace

int main() {
    Gc& gc = Gc::get();
    RootedScratchCallStack stack({3, 4, 48}, gc);
    assert(stack.valid() && stack.depth() == 0);

    // Real root frame, then recursive calls with mixed-type arguments.
    assert(stack.push(0, CallFrameAbi::kNoPc, 1, {}, 0));
    assert(stack.valid() && stack.depth() == 1);
    for (uint32_t i = 0; i < 36; ++i) {
        const uint64_t arguments[] = {
            str("recursive 世界🌟"),
            num(double(i)),
            Value::boolean((i & 1) != 0).bits
        };
        const uint64_t rev = stack.revision();
        assert(stack.push(1, 100 + i, 200 + i, arguments, 2));
        assert(stack.revision() > rev);
        assert(stack.valid() && stack.depth() == i + 2);
        assert(stack.rootedArguments() == i + 1);

        // The numeric scratch frame MUST NOT hold a raw SStr* tagged Value.
        const auto raw = stack.frames().argument(0);
        assert(raw && *raw == val::kUndefined);
        assert(stack.frames().argument(1) == num(double(i)));
        assert(stack.frames().argument(2) == val::kUndefined);

        // The sidecar resolves exact Scratch Values, even across forced GC.
        gc.collect();
        assert(strValue(*stack.argument(i + 1, 0)) == "recursive 世界🌟");
        assert(stack.argument(i + 1, 1)->asNumberUnchecked() == double(i));
        assert(stack.argument(i + 1, 2)->asBool() == bool(i & 1));

        const uint64_t pc = 500 + i;
        assert(stack.setResumePc(pc));
        assert(stack.setNumericLocal(0, num(double(i * 2))));
        assert(stack.frames().top()->resumePc == pc);
        assert(stack.frames().local(0) == num(double(i * 2)));
        assert(stack.frames().validateImage());
    }

    // A failed push must not accidentally allocate roots or update PC.
    const uint32_t depthBefore = stack.depth();
    const std::size_t rootsBefore = stack.rootedArguments();
    const auto revBefore = stack.revision();
    const uint64_t extra[] = {str("invalid"), num(4)};
    assert(!stack.push(3, CallFrameAbi::kNoPc, 4, extra, 2));
    assert(!stack.push(3, 1, CallFrameAbi::kNoPc, extra, 2));
    assert(!stack.push(3, 1, 4, extra, 5));
    assert(stack.depth() == depthBefore);
    assert(stack.rootedArguments() == rootsBefore);
    assert(stack.revision() == revBefore);

    // Retain all parents while popping through nested recursion. Every
    // child return restores the exact caller returnPC and releases just
    // that frame's string root.
    for (int i = 35; i >= 0; --i) {
        gc.collect();
        const auto returnPc = stack.pop();
        assert(returnPc && *returnPc == uint64_t(100 + i));
        assert(stack.depth() == uint32_t(i + 1));
        assert(stack.rootedArguments() == uint32_t(i));
        assert(stack.frames().validateImage());
        if (i) {
            assert(strValue(*stack.argument(uint32_t(i), 0)) ==
                   "recursive 世界🌟");
            assert(stack.frames().top()->resumePc == uint64_t(100 + i));
        }
    }
    assert(stack.frames().top()->resumePc == 100);
    assert(stack.pop() == CallFrameAbi::kNoPc);
    assert(stack.depth() == 0 && stack.rootedArguments() == 0);
    assert(!stack.pop());

    // Abort/deopt releases roots of all remaining frames.
    assert(stack.push(0, CallFrameAbi::kNoPc, 1, {}, 0));
    const uint64_t call[] = {str("deopt"), num(1)};
    assert(stack.push(1, 88, 10, call, 0));
    gc.collect();
    assert(strValue(*stack.argument(1, 0)) == "deopt");
    stack.clear();
    assert(stack.depth() == 0 && stack.rootedArguments() == 0);
    gc.collect();

    // Capacity limit is enforced before acquisition, and no handle leaks.
    RootedScratchCallStack bounded({1, 0, 2}, gc);
    assert(bounded.push(0, CallFrameAbi::kNoPc, 1, {}, 0));
    const uint64_t one[] = {str("bounded")};
    assert(bounded.push(1, 2, 3, one, 0));
    assert(bounded.rootedArguments() == 1);
    assert(!bounded.push(1, 4, 5, one, 0));
    assert(bounded.rootedArguments() == 1);
    bounded.clear();
    assert(bounded.rootedArguments() == 0);

    std::cout << "PASS rooted ScratchCallStack: recursive mixed Value frames, "
                 "GC after yield, exact PC, atomic push/pop, rollback" << '\n';
}
