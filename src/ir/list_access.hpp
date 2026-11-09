// Shared numeric-list specialization contract.
//
// SList objects and their Value buffers are managed by the Scratch runtime.
// A backend must acquire the list through a rooted Variable, and must not
// retain SList::items across calls that can allocate, yield or reallocate.
// The monotonic numericOnly flag is a guard, never an assumption inferred
// from an observed list's contents.
#pragma once

#include "../core/slist.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace xyo::ir {

inline constexpr uint32_t kListLengthOffset =
    static_cast<uint32_t>(offsetof(SList, len));
inline constexpr uint32_t kListNumericOnlyOffset =
    static_cast<uint32_t>(offsetof(SList, numericOnly));
inline constexpr uint32_t kListItemsOffset =
    static_cast<uint32_t>(offsetof(SList, items));
static_assert(sizeof(Value) == sizeof(uint64_t));

// A plain numeric Scratch index is 1-based and rounded down; this helper
// rejects all special/non-number inputs so those reach the exact runtime
// listIndex parser (including "last", "random", and numeric strings).
inline bool numericListIndex(double value, uint32_t length,
                             uint32_t& zeroBased) {
    if (!std::isfinite(value) || value < 1.0 ||
        value >= static_cast<double>(length) + 1.0)
        return false;
    zeroBased = static_cast<uint32_t>(std::floor(value)) - 1;
    return true;
}

struct NumericListItem {
    uint64_t bits = 0;
    bool available = false;
};

// Caller must hold a live GC root for the list throughout this synchronous
// operation. Numeric specialization is strictly opt-in through numericOnly.
inline NumericListItem readNumericListItem(const SList* list, double index) {
    if (!list || !list->numericOnly) return {};
    uint32_t offset = 0;
    if (!numericListIndex(index, list->len, offset)) return {};
    const Value value = list->get0(offset);
    // Defensive check for mutation paths that accidentally skipped the
    // monotonic flag; fall back rather than silently treating a string
    // pointer as an IEEE754 double.
    if (!value.isNumber()) return {};
    return {value.bits, true};
}

// The lowering adapter returns a backend-native i1 boolean guard for the
// list's current numericOnly flag.  LLVM reads a rooted SList directly;
// WASM must instead request a host helper with an owned Variable identity.
template <typename Backend, typename ListRef>
auto lowerNumericListGuard(Backend& backend, ListRef list) {
    return backend.numericListGuard(list);
}

} // namespace xyo::ir
