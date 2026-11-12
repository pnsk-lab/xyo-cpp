#include "../../src/jit/guard_fallback_telemetry.hpp"
#include <cassert>
#include <cstddef>

int main() {
    using namespace xyo;
    const bool on = JitGuardSlowStats::enabled();
    if (on) {
        auto& stats = JitGuardSlowStats::get();
        stats.noteSite(JitGuardSlowKind::NumericProfile);
        stats.noteSite(JitGuardSlowKind::NumericProfile);
        stats.noteHit(uint32_t(JitGuardSlowKind::NumericProfile));
        stats.noteHit(uint32_t(JitGuardSlowKind::NumericProfile));
        stats.noteHit(uint32_t(JitGuardSlowKind::NumericProfile));
        stats.noteHit(uint32_t(JitGuardSlowKind::Count)); // invalid id ignored
        const size_t kind = size_t(JitGuardSlowKind::NumericProfile);
        assert(stats.sites[kind].load() == 2);
        assert(stats.hits[kind].load() == 3);
    }
    return 0;
}
