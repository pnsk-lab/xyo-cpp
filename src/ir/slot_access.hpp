// Shared raw Scratch Value-slot access contract for LLVM and WASM.
//
// Slot contents are exactly the 64-bit NaN-boxed Value representation.
// Reads and writes do not coerce or canonicalize: the caller chooses whether
// to check a numeric guard, convert to f64, or publish a GC-visible Value.
// Both native variable pointers and WASM (base,index) address slots through
// the same contract, while retaining their established runtime ownership.
#pragma once

#include "../core/value.hpp"

#include <cstddef>
#include <cstdint>

namespace xyo::ir {

inline constexpr uint32_t kSlotBytes = sizeof(xyo::Value);
static_assert(kSlotBytes == 8);

// Guaranteed equivalent to Value::isNumber for canonical tagged inputs.
inline constexpr bool isNumericBits(uint64_t bits) {
    return ((bits >> 48) & 0xFFF8ULL) != 0x7FF8ULL ||
           bits == xyo::val::kNan;
}

// Backend must implement loadSlot(address) and storeSlot(address, bits).
// Address is native llvm::Value* or a portable logical slot index.
template <typename Backend, typename Address>
auto lowerSlotLoad(Backend& backend, Address address) {
    return backend.loadSlot(address);
}

template <typename Backend, typename Address, typename Bits>
void lowerSlotStore(Backend& backend, Address address, Bits bits) {
    backend.storeSlot(address, bits);
}

// A numeric fast path may proceed only on a numeric value; strings/bools
// require the caller's explicit fallback or bailout. This predicate does
// not consider a string containing digits to be numeric: coercion is a
// separate Scratch operation.
template <typename Backend, typename Bits>
auto lowerNumericSlotGuard(Backend& backend, Bits bits) {
    return backend.isNumericBits(bits);
}

} // namespace xyo::ir
