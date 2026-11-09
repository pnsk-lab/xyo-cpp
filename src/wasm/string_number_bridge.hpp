// Exact Scratch heap-string -> f64 conversion for transactional WASM slots.
// The source pointer must be owned/rooted by a live Target::Variable. This
// function does not take ownership of it or make it a GC root itself.
#pragma once

#include "../core/cast.hpp"
#include "../core/value.hpp"
#include <cstdint>
#include <cstring>

namespace xyo::wasm {

inline bool convertHeapStringNumberSlot(void* slot) {
    if (!slot) return false;
    uint64_t bits = 0;
    std::memcpy(&bits, slot, sizeof(bits));
    const Value source{bits};
    // Do not dereference arbitrary numeric/tag payloads as heap pointers.
    // Empty string uses the immediate coercion path, not this helper.
    if (!source.isString() || source.isEmptyString()) return false;
    const double number = cast::toNumber(source);
    std::memcpy(slot, &number, sizeof(number));
    return true;
}

} // namespace xyo::wasm
