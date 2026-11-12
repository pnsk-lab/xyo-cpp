import { describe, expect, test } from "bun:test";
import { BENCHMARKS } from "../scratch-bench/site/shared/benchmarks.ts";
import { analyzeResult, compareResults, parseArgs } from "./scratchbench-target.ts";

function fixture(rate = 100) {
  return {
    suite: { id: "performance", source_hash: "fixture-hash" },
    results: Object.fromEntries(BENCHMARKS.map(b => [b.id, { ops_per_ms: { median: rate } }])),
    runs: [{ ok: true, data: Object.fromEntries(BENCHMARKS.map(b => [b.id, { checksum: "expected" }])) }],
  };
}

describe("official ScratchBench target diagnostics", () => {
  test("all rates 100 produce official score 100 and normalized exponents", () => {
    const a = analyzeResult(fixture());
    expect(a.official.total).toBeCloseTo(100, 10);
    expect(a.official.coverage).toBe(1);
    expect(Object.values(a.categories).flatMap(c => c.items).reduce((s, i) => s + i.exponent, 0)).toBeCloseTo(1, 12);
    expect(a.categories.variables.items[0].exponent).toBeCloseTo(1 / 27, 12);
    expect(a.categories.applications.items[0].exponent).toBeCloseTo(1 / 30, 12);
    expect(a.categories.math.items[0].exponent).toBeCloseTo(1 / 81, 12);
  });
  test("one doubled variable item has exact alpha log contribution", () => {
    const base = fixture(), candidate = fixture();
    candidate.results.vars_many.ops_per_ms.median = 200;
    const c = compareResults(base, candidate);
    expect(c.totalRatio).toBeCloseTo(2 ** (1 / 27), 12);
    expect(c.summedItemLogGain).toBeCloseTo(Math.log(2) / 27, 12);
    expect(c.totalLogGain).toBeCloseTo(c.summedItemLogGain, 12);
    expect(c.items.find(i => i.id === "vars_many")!.logContribution).toBeCloseTo(Math.log(2) / 27, 12);
  });
  test("complete positive coverage required even when official 80% rule would allow it", () => {
    for (const bad of [0, -1, NaN, Infinity, null, undefined, "100"]) {
      const f: any = fixture();
      f.results.loops_empty.ops_per_ms.median = bad;
      expect(() => analyzeResult(f)).toThrow("Complete positive coverage required");
    }
    const f: any = fixture();
    delete f.results.app_ast;
    expect(() => analyzeResult(f)).toThrow("46/47");
  });
  test("comparison rejects missing and mismatching source hashes", () => {
    const other: any = fixture();
    other.suite.source_hash = "different";
    expect(() => compareResults(fixture(), other)).toThrow("source_hash");
    delete other.suite.source_hash;
    expect(() => compareResults(other, other)).toThrow("source_hash");
  });
  test("summary scenarios and required multipliers use weighted geometric math", () => {
    const a = analyzeResult(fixture(), 200);
    expect(a.remainingFactor).toBeCloseTo(2, 12);
    expect(a.remainingLogGain).toBeCloseTo(Math.log(2), 12);
    expect(a.categories.applications.requiredMultiplierAlone).toBeCloseTo(64, 8);
    expect(a.categories.loops.requiredMultiplierAlone).toBeCloseTo(512, 8);
    expect(a.categories.scheduler.requiredMultiplierAlone).toBeCloseTo(2 ** (9 / 1.25), 8);
    expect(a.categories.applications.scenarios[0].total).toBeCloseTo(100 * 2 ** (1 / 6), 10);
    expect(a.categories.loops.scenarios[1].total).toBeCloseTo(100 * 10 ** (1 / 9), 10);
  });
  test("checksums report non-scheduler mismatches but ignore scheduler differences", () => {
    const base = fixture(), candidate = fixture();
    candidate.runs[0].data.sched_threads.checksum = "variable timing";
    expect(compareResults(base, candidate).checksumComparison.every(c => c.match)).toBe(true);
    candidate.runs[0].data.app_ast.checksum = "wrong";
    const c = compareResults(base, candidate);
    expect(c.checksumComparison.find(x => x.id === "app_ast")!.match).toBe(false);
    expect(c.warnings.some(w => w.includes("semantically invalid"))).toBe(true);
  });
  test("missing checksum data and raw provenance are explicit", () => {
    const f: any = fixture();
    f.runs = [];
    f.runtime = { version: "custom", detail: { binary: "/example/xyo" } };
    f.host = { cpu: "example", affinity: "8" };
    f.repo = { sha: "not-a-runtime-version" };
    const a = analyzeResult(f);
    expect(a.provenance.runtime).toEqual(f.runtime);
    expect(a.provenance.host).toEqual(f.host);
    expect(a.checksums.every(c => c.consistent === null)).toBe(true);
    expect(a.warnings.some(w => w.includes("No run-level"))).toBe(true);
  });
  test("argument validation rejects unsafe or ambiguous options", () => {
    expect(parseArgs(["base.json", "--compare", "candidate.json", "--target", "200000"]).target).toBe(200000);
    for (const args of [[], ["base.json", "--out", "base.json"], ["base.json", "--target", "NaN"], ["base.json", "--compare"], ["base.json", "--unknown"], ["base.json", "extra.json"], ["base.json", "--target", "2", "--target", "3"]]) {
      expect(() => parseArgs(args)).toThrow();
    }
  });
});
