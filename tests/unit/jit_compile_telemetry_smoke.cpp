#include "../../src/jit/compile_telemetry.hpp"

#include <cassert>
#include <cstdint>

int main() {
    xyo::JitCompileTelemetry telemetry;
    const auto started = xyo::JitCompileTelemetry::Clock::now();
    telemetry.syncCompile.record(true, started);
    telemetry.asyncPrepare.record(false, started);
    telemetry.asyncFinish.record(true, started);
    telemetry.asyncStaleDiscarded.fetch_add(1, std::memory_order_relaxed);

    assert(telemetry.syncCompile.attempts == 1);
    assert(telemetry.syncCompile.successes == 1);
    assert(telemetry.asyncPrepare.attempts == 1);
    assert(telemetry.asyncPrepare.successes == 0);
    assert(telemetry.asyncFinish.attempts == 1);
    assert(telemetry.asyncFinish.successes == 1);
    assert(telemetry.asyncStaleDiscarded == 1);

    telemetry.report(); // Prints only if XYO_JIT_COMPILE_STATS=1
}
