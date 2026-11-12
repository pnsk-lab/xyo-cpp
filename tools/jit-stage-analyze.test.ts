import { expect, test } from "bun:test";
import { parseJitTiming, summarizeJitTiming } from "./jit-stage-analyze";

test("parse LLVM sync stage, expensive pass, and compile summary", () => {
  const log = [
    "[jit-timing] abc emit=2.300ms opt=14.250ms submit=0.005ms lookup=27.500ms total=44.200ms funcs=3 ir=459 tier=eager",
    "[jit-pass] abc 9.340ms 13 InstCombinePass",
    "[jit-compile] sync attempts=1 success=1 total_ms=44.400",
  ].join("\n");
  const parsed = parseJitTiming(log);
  expect(parsed.stages).toEqual([{
    script: "abc", emitMs: 2.3, optMs: 14.25,
    submitMs: 0.005, lookupMs: 27.5, totalMs: 44.2,
    functions: 3, irInstructions: 459, tier: "eager",
  }]);
  expect(parsed.passes).toEqual([{
    script: "abc", name: "InstCombinePass",
    exclusiveMs: 9.34, invocations: 13,
  }]);
  expect(parsed.syncCompileMs).toBe(44.4);
});

test("lazy and async logs are separate from sync stage totals", () => {
  const parsed = parseJitTiming([
    "[jit-timing] script with space emit=1.000ms opt=0.000ms submit=0.010ms lookup=5.000ms total=6.200ms funcs=6 ir=7500 tier=lazy",
    "[jit-lazy-ir] 39.200ms",
    "[jit-proc-map] function=xyo_proc_abc key=k_app_life|w",
    "[jit-lazy-detail] first=xyo_proc_abc funcs=1 ir=987 opt_ms=39.200",
    "[jit-async] some-script finished 83.300ms",
    "[jit-async] other-script finished 16.700ms",
  ].join("\n"));
  expect(parsed.stages[0].script).toBe("script with space");
  expect(parsed.lazyOptimizationMs).toEqual([39.2]);
  expect(parsed.asyncFinishMs).toEqual([83.3, 16.7]);
  const summary = summarizeJitTiming([parsed]);
  expect(summary.totalMs).toBe(6.2);
  expect(summary.lazy).toBe(1);
  expect(summary.lazyOptimizationMs).toBe(39.2);
  expect(summary.lazyModuleCount).toBe(1);
  expect(summary.topLazyModules).toEqual([{
    firstFunction: "xyo_proc_abc", sourceProcedure: "k_app_life|w",
    functions: 1, irInstructions: 987, optMs: 39.2,
  }]);
  expect(summary.asyncFinishMs).toBe(100);
});

test("aggregates multiple logs and pass names without double counting nested passes", () => {
  const a = parseJitTiming([
    "[jit-timing] a emit=1.000ms opt=3.000ms submit=0.100ms lookup=4.000ms total=8.500ms funcs=1 ir=9 tier=eager",
    "[jit-pass] a 6.000ms 2 LoopVectorizePass",
  ].join("\n"));
  const b = parseJitTiming([
    "[jit-timing] b emit=2.000ms opt=4.000ms submit=0.200ms lookup=5.000ms total=11.500ms funcs=1 ir=100 tier=eager",
    "[jit-pass] b 7.000ms 1 LoopVectorizePass",
  ].join("\n"));
  const summary = summarizeJitTiming([a, b]);
  expect(summary.scripts).toBe(2);
  expect(summary.emitMs).toBe(3);
  expect(summary.optMs).toBe(7);
  expect(summary.lookupMs).toBe(9);
  expect(summary.totalMs).toBe(20);
  expect(summary.unaccountedMs).toBeCloseTo(0.7);
  expect(summary.passSummary).toEqual([{
    name: "LoopVectorizePass", exclusiveMs: 13, invocations: 3,
  }]);
  expect(summary.passListingIncomplete).toBe(true);
});

test("ignores non-matching, truncated, and ordinary runtime stderr", () => {
  expect(parseJitTiming("[jit-timing] incomplete\n[jit-guard] kind=numeric sites=1 hits=10").stages)
    .toHaveLength(0);
});

test("counts declaration-only lazy IR partitions with empty first function", () => {
  const parsed = parseJitTiming(
    "[jit-lazy-detail] first= funcs=0 ir=0 opt_ms=0.291",
  );
  expect(parsed.lazyModules).toEqual([{
    firstFunction: "", functions: 0,
    irInstructions: 0, optMs: 0.291,
  }]);
  expect(summarizeJitTiming([parsed]).lazyModuleCount).toBe(1);
});
