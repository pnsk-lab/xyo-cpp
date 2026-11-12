#include "../../src/analysis/cfg_type_hint_profitability.hpp"

#include <cassert>
#include <cstddef>
#include <limits>

int main() {
    using xyo::analysis::cfgNumericShadowWorthAnalyzing;
    assert(!cfgNumericShadowWorthAnalyzing(0, 1));
    assert(!cfgNumericShadowWorthAnalyzing(1, 1));
    assert(!cfgNumericShadowWorthAnalyzing(2, 1));
    assert(cfgNumericShadowWorthAnalyzing(3, 1));
    assert(cfgNumericShadowWorthAnalyzing(4, 2));
    assert(!cfgNumericShadowWorthAnalyzing(3, 2));
    assert(!cfgNumericShadowWorthAnalyzing(50, 40));
    assert(!cfgNumericShadowWorthAnalyzing(100, 0));
    assert(!cfgNumericShadowWorthAnalyzing(
        std::numeric_limits<unsigned>::max(),
        std::numeric_limits<std::size_t>::max()));
}
