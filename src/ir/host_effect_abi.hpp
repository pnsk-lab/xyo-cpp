// Backend-neutral host-effect ABI for Scratch boxed Values.
//
// The value arguments are exact 64-bit Scratch NaN-box bits. The callee
// resolves all GC-managed strings using Scratch's existing runtime helpers.
// WASM must pass *staged* slots and keep all input Values rooted (or defer
// collection) until the corresponding transaction commits or rolls back.
// Returned GC Values must also be rooted before collection is re-enabled.
// No host pointer, Variable*, Thread* or GC object is cached in generated IR.
#pragma once

#include "../rt/runtime_helpers.hpp"
#include "gc_value_handles.hpp"
#include <cstdint>

namespace xyo::ir {

enum class HostEffectOp : uint8_t {
    StringLength = 16,
    Compare = 17,
    Join = 18,
    LetterOf = 19,
    Contains = 20,
    ToBoolean = 22,
};

enum class HostEffectCommit : uint8_t {
    // Repeated computation is semantically pure, but any new heap Value
    // remains speculative and MUST NOT escape an aborted transaction.
    Transactional,
    // Observable effects (render, broadcast, clones, audio, I/O) must only
    // run after a replay-safe continuation/commit boundary is established.
    CommitOnly,
    Unsupported
};

struct HostEffectDescriptor {
    HostEffectCommit commit;
    uint8_t inputWords;
    bool mayAllocate;
    bool returnsTaggedValue;
};

constexpr HostEffectDescriptor describeHostEffect(HostEffectOp op) {
    switch (op) {
    case HostEffectOp::StringLength:
    case HostEffectOp::ToBoolean:
        return {HostEffectCommit::Transactional, 1, true, true};
    case HostEffectOp::Compare:
    case HostEffectOp::Contains:
        return {HostEffectCommit::Transactional, 2, true, true};
    case HostEffectOp::Join:
    case HostEffectOp::LetterOf:
        return {HostEffectCommit::Transactional, 2, true, true};
    }
    return {HostEffectCommit::Unsupported, 0, false, false};
}

// The ABI returns a tagged Scratch Value in all cases. WASM's historical
// env.h import converts numeric/bool results to an f64 in the scratch buffer,
// whereas LLVM consumes the tagged result directly. These conversions happen
// strictly at the respective adapter boundary.
inline bool invokeHostEffect(HostEffectOp op, uint64_t lhs, uint64_t rhs,
                             int32_t mode, uint64_t& result) {
    const Value left{lhs}, right{rhs};
    Value output = Value::undefined();
    switch (op) {
    case HostEffectOp::StringLength:
        output = Value::num(rt::strLength(left));
        break;
    case HostEffectOp::Compare: {
        if (mode < 0 || mode > 2) return false;
        const double order = cast::compare(left, right);
        output = Value::boolean(mode == 0 ? order < 0 :
                                mode == 1 ? order > 0 : order == 0);
        break;
    }
    case HostEffectOp::Join:
        output = rt::join(left, right);
        break;
    case HostEffectOp::LetterOf:
        output = rt::letterOf(left, right);
        break;
    case HostEffectOp::Contains:
        output = Value::boolean(rt::strContains(left, right));
        break;
    case HostEffectOp::ToBoolean:
        output = Value::boolean(cast::toBoolean(left));
        break;
    default:
        return false;
    }
    result = output.bits;
    return true;
}

// Safe across scheduler yields: the executable stores only a token. The
// owner must release the token when the frame or speculative branch retires.
// Invalid input handles / unsupported ops fail closed without publication.
inline std::optional<RootedValueHandle> invokeRootedHostEffect(
    GcValueHandles& roots, HostEffectOp op, RootedValueHandle lhs,
    RootedValueHandle rhs = {}, int32_t mode = 0) {
    const HostEffectDescriptor descriptor = describeHostEffect(op);
    if (descriptor.commit != HostEffectCommit::Transactional ||
        (descriptor.inputWords != 1 && descriptor.inputWords != 2))
        return std::nullopt;
    const auto first = roots.resolve(lhs);
    if (!first) return std::nullopt;
    const auto second = descriptor.inputWords == 2
        ? roots.resolve(rhs) : std::optional<Value>{Value::undefined()};
    if (!second) return std::nullopt;
    uint64_t result = 0;
    if (!invokeHostEffect(op, first->bits, second->bits, mode, result))
        return std::nullopt;
    return roots.retain(Value{result});
}

static_assert(sizeof(Value) == sizeof(uint64_t));
static_assert(describeHostEffect(HostEffectOp::Join).inputWords == 2);
static_assert(describeHostEffect(HostEffectOp::Contains).commit ==
              HostEffectCommit::Transactional);
} // namespace xyo::ir
