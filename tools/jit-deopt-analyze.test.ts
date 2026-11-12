import { describe, expect, test } from "bun:test";
import { analyzeJitDeoptLog } from "./jit-deopt-analyze";

describe("JIT deoptimization diagnostic log analyzer", () => {
  test("classifies native handoff, numeric rollback, and nested OSR", () => {
    const events = [
      "[native-simple-deopt] script=abc adopted=1",
      "[native-simple-deopt] script=def adopted=0",
      "xyo-common-cfg: deopt script=ghi committed=1 source_changed=1 reason=source_changed slices=2 rollbacks=1",
      "[jit-osr-inflight] deopt inner outer=10 inner=3 source-changed=1",
      "[jit-osr-inflight] deopt remaining=7 source-changed=1; interpreter owns original loop",
      "xyo-jit: guarded warp induction nop",
    ].join("\n");

    const result = analyzeJitDeoptLog(events);
    expect(result.observed).toBe(5);
    expect(result.byKind).toEqual({
      native_handoff_adopted: 1,
      native_handoff_refused: 1,
      numeric_cfg_rollback: 1,
      nested_osr_deopt_inner: 1,
      nested_osr_deopt_outer: 1,
    });
    expect(result.byReason["numeric:source_changed"]).toBe(1);
    expect(result.byReason["nested_osr:source_changed"]).toBe(2);
  });

  test("ignores unrelated messages and reports absent reason", () => {
    const result = analyzeJitDeoptLog(
      "[jit-compile] sync attempts=1 success=1 total_ms=1.0\n" +
      "xyo-common-cfg: deopt script=test\n" +
      "[native-simple-deopt] script=test adopted=?\n",
    );
    expect(result.observed).toBe(1);
    expect(result.byReason["numeric:unspecified"]).toBe(1);
  });

  test("does not confuse existing log markers with one another", () => {
    const result = analyzeJitDeoptLog(
      "[jit-osr-inflight] deopt remaining=12 source-changed=1\n" +
      "[jit-osr-inflight] nested OSR accepted\n",
    );
    expect(result.observed).toBe(1);
    expect(result.byKind.nested_osr_deopt_outer).toBe(1);
  });
});

test("parses direct runtime deopt counters separately from diagnostic logs", () => {
  const log = [
    "[jit-deopt] path=native_handoff attempts=2 resumed=1 refused=1 rate=50.00%",
    "[jit-deopt] path=nested_triple attempts=1 resumed=1 refused=0 rate=100.00%",
    "[jit-deopt] reason=source_changed count=1",
    "[jit-deopt] reason=handoff_refused count=1",
    "[native-simple-deopt] script=test adopted=1",
  ].join("\n");
  const result = analyzeJitDeoptLog(log);
  expect(result.direct).toEqual({
    attempts: 3,
    resumed: 2,
    refused: 1,
    byPath: {
      native_handoff: { attempts: 2, resumed: 1, refused: 1 },
      nested_triple: { attempts: 1, resumed: 1, refused: 0 },
    },
    byReason: { source_changed: 1, handoff_refused: 1 },
  });
  // The diagnostic is a duplicate observation, not another direct event.
  expect(result.observed).toBe(1);
});

test("rejects inconsistent direct deopt totals without corrupting summary", () => {
  const log = [
    "[jit-deopt] path=numeric_cfg attempts=2 resumed=2 refused=2 rate=100.00%",
    "[jit-deopt] reason=backend_failure count=1",
  ].join("\n");
  const result = analyzeJitDeoptLog(log);
  expect(result.direct.attempts).toBe(0);
  expect(result.direct.byReason.backend_failure).toBe(1);
});
