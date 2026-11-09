// Backend-independent immediate Scratch Value classification/coercion.
// Heap strings require the actual Scratch UTF-16 parser and are deliberately
// left to the runtime (not guessed from NaN-boxed pointer payloads).
#pragma once

#include "../core/value.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace xyo::ir {

enum class ImmediateKind : uint8_t {
    Number, BooleanFalse, BooleanTrue, EmptyString,
    HeapString, Color, Undefined
};

inline constexpr ImmediateKind classifyValueBits(uint64_t bits) {
    if (bits == val::kNan || ((bits >> 48) & 0xFFF8) != 0x7FF8)
        return ImmediateKind::Number;
    if (bits == val::kFalse) return ImmediateKind::BooleanFalse;
    if (bits == val::kTrue) return ImmediateKind::BooleanTrue;
    if (bits == val::kEmptyStr) return ImmediateKind::EmptyString;
    if ((bits >> 48) == 0x7FF9) return ImmediateKind::HeapString;
    if ((bits >> 48) == 0x7FFA) return ImmediateKind::Color;
    return ImmediateKind::Undefined;
}

// The direct Scratch ToNumber fast path: true=1, false/empty/undefined/
// color=0, canonical numeric NaN=0, signed zero is preserved. Heap strings
// must use cast::toNumber; this routine never dereferences a GC pointer.
struct ImmediateNumber {
    double value;
    bool needsStringParse;
};

inline ImmediateNumber toNumberImmediate(uint64_t bits) {
    switch (classifyValueBits(bits)) {
    case ImmediateKind::Number: {
        if (bits == val::kNan) return {0.0, false};
        double number;
        std::memcpy(&number, &bits, sizeof(number));
        return {std::isnan(number) ? 0.0 : number, false};
    }
    case ImmediateKind::BooleanTrue: return {1.0, false};
    case ImmediateKind::HeapString: return {0.0, true};
    case ImmediateKind::BooleanFalse:
    case ImmediateKind::EmptyString:
    case ImmediateKind::Color:
    case ImmediateKind::Undefined: return {0.0, false};
    }
    return {0.0, true};
}

// A backend-specific emitter interprets these same kinds. The backend must
// explicitly preserve the heap-string fallback instead of treating it as 0.
template <typename Backend, typename Bits>
auto lowerImmediateNumber(Backend& backend, Bits bits) {
    return backend.immediateNumber(bits);
}

} // namespace xyo::ir
