// IR-001: pointer-free Scratch immediate validation shared by native and no-JIT.
#pragma once
#include "../core/value.hpp"
#include <cstdint>

namespace xyo::ir {
inline bool pointerFreeScratchImmediate(uint64_t bits) {
    const Value value{bits};
    if (value.isNumber()) {
        // Reject uncanonical +NaN payloads, even if the isNumber()
        // classification would accept a noncanonical negative NaN.
        const uint64_t exp = (bits >> 52) & 0x7ff;
        const uint64_t fraction = bits & 0x000fffffffffffffull;
        if (exp == 0x7ff && fraction && bits != val::kNan)
            return false;
        return true;
    }
    return value.isBool() || value.isColor() ||
           value.isEmptyString() || value.isUndefined();
}

} // namespace xyo::ir
