// Standalone native/emscripten parity smoke for shared Scratch host effects.
// Compile/link against xyo's runtime; no LLVM or Binaryen API is required.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "ir/host_effect_abi.hpp"
#include "ir/scratch_effect.hpp"
#include "core/gc.hpp"
#include "core/sstr.hpp"
#include <cassert>
#include <cstdio>
#include <cstdint>

using namespace xyo;

int main() {
    Gc::DeferCollect keepLive; // The test has no Target::Variable roots.
    using ir::HostEffectOp;
    const auto s = [](const char* v) {
        return Value::string(SStr::fromUtf8(v)).bits;
    };
    const auto n = [](double v) { return Value::num(v).bits; };
    const auto call = [](HostEffectOp op, uint64_t lhs, uint64_t rhs = 0,
                         int32_t mode = 0) {
        uint64_t result = val::kUndefined;
        assert(ir::invokeHostEffect(op, lhs, rhs, mode, result));
        return Value{result};
    };
    auto joined = call(HostEffectOp::Join, s("Hello "), s("世界"));
    assert(SStr::toUtf8(joined.asStrPtr()) == "Hello 世界");
    const auto letter = call(HostEffectOp::LetterOf, n(2), s("ABC"));
    assert(SStr::toUtf8(letter.asStrPtr()) == "B");
    assert(call(HostEffectOp::StringLength, s("A🌟")).asNumberUnchecked() == 3);
    assert(call(HostEffectOp::Contains, s("AbCd"), s("BC")).asBool());
    assert(!call(HostEffectOp::Contains, s("foo"), s("bar")).asBool());
    assert(!call(HostEffectOp::ToBoolean, s("false")).asBool());
    assert(!call(HostEffectOp::ToBoolean, s("0")).asBool());
    assert(call(HostEffectOp::ToBoolean, s("abc")).asBool());
    assert(call(HostEffectOp::Compare, s("15"), n(5), 1).asBool());
    assert(call(HostEffectOp::Compare, s("abc"), s("ABC"), 2).asBool());
    assert(!call(HostEffectOp::Compare, s("abc"), s("xyz"), 2).asBool());
    uint64_t unchanged = 0x1234;
    assert(!ir::invokeHostEffect(HostEffectOp::Compare,
                                  n(1), n(2), 3, unchanged));
    assert(unchanged == 0x1234);
    assert(!ir::invokeHostEffect(static_cast<HostEffectOp>(255),
                                  n(1), n(2), 0, unchanged));
    assert(unchanged == 0x1234);

    const auto external = ir::externalEffectFlags("event_broadcastandwait");
    assert(ir::hasEffect(external, ir::ScratchEffect::Observable));
    assert(ir::hasEffect(external, ir::ScratchEffect::MaySpawn));
    assert(!ir::mayReplaySpeculatively(external));
    assert(!ir::mayReplaySpeculatively(
        ir::externalEffectFlags("pen_stamp")));
    assert(!ir::mayReplaySpeculatively(ir::ScratchEffect::Unknown));
    assert(ir::mayReplaySpeculatively(
        ir::ScratchEffect::ReadState | ir::ScratchEffect::MayAllocate));
    assert(ir::describeHostEffect(HostEffectOp::Join).mayAllocate);
    std::puts("PASS shared host effect ABI: 22 assertions");
}
