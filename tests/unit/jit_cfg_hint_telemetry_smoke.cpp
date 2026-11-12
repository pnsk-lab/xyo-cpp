#include "../../src/analysis/cfg_type_hint_telemetry.hpp"
#include <cassert>

int main() {
    using namespace xyo::analysis;
    if (!CfgTypeHintTelemetry::enabled()) return 0;
    auto& s = CfgTypeHintTelemetry::get();
    s.noteEligible(false);
    s.noteEligible(true);
    const auto start = CfgTypeHintTelemetry::Clock::now();
    s.noteAnalysis(start, 29, false);
    s.notePromotion();
    assert(s.eligible.load() == 2);
    assert(s.skippedProfitability.load() == 1);
    assert(s.analyses.load() == 1);
    assert(s.analyzedNodes.load() == 29);
    assert(s.promoted.load() == 1);
}
