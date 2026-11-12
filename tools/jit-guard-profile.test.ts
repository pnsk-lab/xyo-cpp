import { expect, test } from "bun:test";
import { rankGuardKinds, verifyDone, type GuardRun } from "./jit-guard-profile";

const sample = (fixture: string, hits: number, kind: string, ok = true): GuardRun => ({
  fixture, ok, exit: ok ? 0 : 1, done: ok, timedOut: false,
  elapsedMs: 1000,
  guard: { hits, sites: 1, byKind: { [kind]: { hits, sites: 1 } } },
});

test("DONE must occupy a whole line", () => {
  expect(verifyDone("header\nDONE\n")).toBe(true);
  expect(verifyDone("DONENESS\n")).toBe(false);
  expect(verifyDone("error: DONE pending\n")).toBe(false);
});

test("sort guard hits from successful fixtures only", () => {
  const list = [
    sample("a", 7, "numeric_profile"),
    sample("b", 12, "typed_ssa_cache_miss"),
    sample("c", 9, "numeric_profile"),
    sample("failed", 100, "numeric_list", false),
  ];
  expect(rankGuardKinds(list)).toEqual([
    { kind: "numeric_profile", hits: 16, fixtures: 2 },
    { kind: "typed_ssa_cache_miss", hits: 12, fixtures: 1 },
  ]);
});
