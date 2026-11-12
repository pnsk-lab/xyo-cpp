// LAYQ (Load-Adaptive Yield Quantum) policy unit tests.
// Pure policy math only (no engine dependency): target/next/shift,
// competitor derivation, pow2 rounding, config clamping.
#include <cstdio>

#include "jit/layq.hpp"

namespace {
using namespace xyo;
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL layq: %s\n", what);
    }
}
} // namespace

int test_layq_main() {
    // pow2 rounding.
    check(layqRoundDownPow2(0) == 1, "round(0)=1");
    check(layqRoundDownPow2(1) == 1, "round(1)=1");
    check(layqRoundDownPow2(64) == 64, "round(64)=64");
    check(layqRoundDownPow2(100) == 64, "round(100)=64");
    check(layqRoundDownPow2(16383) == 8192, "round(16383)=8192");
    check(layqRoundDownPow2(16384) == 16384, "round(16384)=16384");

    // shift range: log2(qmax/qmin).
    check(layqShiftRange(16384, 64) == 8, "shift pure 8");
    check(layqShiftRange(256, 64) == 2, "shift call 2");
    check(layqShiftRange(64, 64) == 0, "shift equal 0");

    // target: halve per competitor, floor at qmin.
    check(layqTarget(0, 16384, 64) == 16384, "solo pure max");
    check(layqTarget(1, 16384, 64) == 8192, "one competitor halves");
    check(layqTarget(2, 16384, 64) == 4096, "two competitors quarter");
    check(layqTarget(7, 16384, 64) == 128, "seven competitors");
    check(layqTarget(8, 16384, 64) == 64, "eight competitors floor");
    check(layqTarget(100, 16384, 64) == 64, "burst clamps at qmin");
    check(layqTarget(0, 256, 64) == 256, "solo call max");
    check(layqTarget(1, 256, 64) == 128, "call one competitor");
    check(layqTarget(2, 256, 64) == 64, "call two competitors floor");
    check(layqTarget(50, 256, 64) == 64, "call burst floor");

    // next-quantum policy is snap-to-target (stateless): the first poll
    // after entry jumps straight to the load-derived target, so tiny
    // loops that never poll keep today's cadence exactly and long loops
    // converge in one poll. (A gradual ramp was measured to waste the
    // first ~16k iterations on short loops; see the LAYQ benchmark notes.)
    // layqTarget() above is the whole policy; no separate step function.
    {
        uint64_t q = layqTarget(0, 16384, 64);
        check(q == 16384, "snap target solo max");
    }

    // competitors: estimate includes self, saturate at zero.
    check(layqCompetitors(0) == 0, "empty estimate");
    check(layqCompetitors(1) == 0, "solo");
    check(layqCompetitors(101) == 100, "fanout");

    // defaults match the design doc.
    {
        LayqConfig c;
        check(c.qmin == 64 && c.qmaxPure == 16384 && c.qmaxCall == 256 &&
                  c.qfixed == 64,
              "default config");
    }
    std::printf("layq: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
