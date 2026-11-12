import { expect, test } from "bun:test";
import { analyzeJitGuardLog } from "./jit-guard-analyze";

test("aggregates multiple guard logs without merging kinds", () => {
  const log = [
    "[jit-guard] kind=numeric_profile sites=2 hits=15",
    "[jit-guard] kind=numeric_list sites=1 hits=4",
    "[jit-guard] kind=numeric_profile sites=1 hits=5",
    "[jit-deopt] reason=source_changed count=2",
  ].join("\n");
  expect(analyzeJitGuardLog(log)).toEqual({
    sites: 4,
    hits: 24,
    byKind: {
      numeric_profile: { sites: 3, hits: 20 },
      numeric_list: { sites: 1, hits: 4 },
    },
  });
});

test("does not read invalid or partial lines", () => {
  const log = [
    "[jit-guard] kind=eq sites=2 hits=NaN",
    "[jit-guard] kind=eq sites=2 hits=-1",
    "[jit-guard] kind=eq sites=3 hits=9007199254740992",
    "prefix [jit-guard] kind=eq sites=1 hits=5",
    "[jit-guard] kind=eq sites=1 hits=5",
  ].join("\n");
  expect(analyzeJitGuardLog(log).hits).toBe(5);
  expect(analyzeJitGuardLog(log).sites).toBe(1);
});

test("handles empty log", () => {
  expect(analyzeJitGuardLog("")).toEqual({
    sites: 0, hits: 0, byKind: {},
  });
});
