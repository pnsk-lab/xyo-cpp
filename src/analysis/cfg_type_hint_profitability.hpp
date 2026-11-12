#pragma once

#include <cstddef>
#include <cstdint>

namespace xyo::analysis {

// Cheap compile-time profitability filter for CFG-guided eager numeric shadows.
//
// A static numeric use is a hint, NOT a proof of a runtime numeric value.
// analyzeTypeFlow and its ordinary conservative safety checks must still prove
// all reachable writes Number-only before changing the shadow strategy.
//
// Guard against heavy write streams (each eager write maintains a numeric
// shadow), and against one-off numeric use where the lazy shadow is cheaper.
// Test writes via division instead of multiplication to avoid overflow.
inline constexpr bool cfgNumericShadowWorthAnalyzing(
    unsigned numericUses, std::size_t unknownWrites) noexcept {
    return unknownWrites > 0 && numericUses >= 3 &&
           numericUses / unknownWrites >= 2;
}

} // namespace xyo::analysis
