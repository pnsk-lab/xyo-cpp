#include "../../src/jit/compile_telemetry.hpp"

#include <cassert>
#include <cstddef>

int main() {
    xyo::JitCompileTelemetry t;
    t.noteRefusal(xyo::JitRefusalReason::NumericSlotGuard);
    t.noteRefusal(xyo::JitRefusalReason::NumericSlotGuard);
    t.noteRefusal(xyo::JitRefusalReason::InvalidSource);
    const auto slot = static_cast<size_t>(xyo::JitRefusalReason::NumericSlotGuard);
    const auto invalid = static_cast<size_t>(xyo::JitRefusalReason::InvalidSource);
    assert(t.refusals[slot].load() == 2);
    assert(t.refusals[invalid].load() == 1);
    t.report();
    return 0;
}
