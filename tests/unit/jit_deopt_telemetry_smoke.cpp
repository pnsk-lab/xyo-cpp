#include "../../src/jit/deopt_telemetry.hpp"
#include <cassert>
#include <cstddef>

int main() {
    using namespace xyo;
    assert(numericDeoptReason("source-changed")
           == JitDeoptReason::SourceChanged);
    assert(numericDeoptReason("commit-conflict")
           == JitDeoptReason::CommitConflict);
    auto& stats = JitDeoptTelemetry::get();
    noteJitDeopt(JitDeoptPath::NativeHandoff,
                 JitDeoptReason::ExplicitHandoff, true);
    noteJitDeopt(JitDeoptPath::NativeHandoff,
                 JitDeoptReason::HandoffRefused, false);
    noteJitDeopt(JitDeoptPath::NumericCfg,
                 numericDeoptReason("invalid-bindings"), false);
    noteJitDeopt(JitDeoptPath::NestedTriple,
                 JitDeoptReason::SourceChanged, true);
    if (JitDeoptTelemetry::enabled()) {
        const auto& handoff = stats.paths[
            static_cast<size_t>(JitDeoptPath::NativeHandoff)];
        assert(handoff.attempts.load() == 2);
        assert(handoff.resumed.load() == 1);
        assert(handoff.refused.load() == 1);
        const auto& triple = stats.paths[
            static_cast<size_t>(JitDeoptPath::NestedTriple)];
        assert(triple.attempts.load() == 1);
        assert(triple.resumed.load() == 1);
    } else {
        const auto& handoff = stats.paths[
            static_cast<size_t>(JitDeoptPath::NativeHandoff)];
        assert(handoff.attempts.load() == 0);
    }
    return 0;
}
