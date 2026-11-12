#!/usr/bin/env bun
/**
 * Aggregate opt-in LLVM JIT slow-path counters from stderr logs.
 * Counts are diagnostic; the instrumented binary is not a performance baseline.
 */
import { readFileSync } from "node:fs";

export type GuardEntry = { sites: number; hits: number };
export type GuardSummary = {
  sites: number;
  hits: number;
  byKind: Record<string, GuardEntry>;
};

export function analyzeJitGuardLog(log: string): GuardSummary {
  const result: GuardSummary = { sites: 0, hits: 0, byKind: {} };
  for (const line of log.split(/\r?\n/)) {
    const m = /^\[jit-guard\] kind=([a-z_]+) sites=(\d+) hits=(\d+)$/.exec(
      line.trim(),
    );
    if (!m) continue;
    const sites = Number(m[2]);
    const hits = Number(m[3]);
    if (!Number.isSafeInteger(sites) || !Number.isSafeInteger(hits)) continue;
    const current = result.byKind[m[1]] ?? { sites: 0, hits: 0 };
    current.sites += sites;
    current.hits += hits;
    result.byKind[m[1]] = current;
    result.sites += sites;
    result.hits += hits;
  }
  return result;
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  const json = args.includes("--json");
  const paths = args.filter((value) => value !== "--json");
  if (paths.length > 1 || paths.some((value) => value !== "-" && value.startsWith("-"))) {
    console.error("Usage: bun tools/jit-guard-analyze.ts [--json] [logfile|-]");
    process.exit(2);
  }
  const input = paths.length && paths[0] !== "-" ? paths[0] : 0;
  const result = analyzeJitGuardLog(readFileSync(input, "utf8"));
  if (json) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    console.log("LLVM slow-path entries: " + result.hits +
                " (preoptimization sites: " + result.sites + ")");
    for (const [kind, entry] of Object.entries(result.byKind)
      .sort(([a, va], [b, vb]) => vb.hits - va.hits || a.localeCompare(b))) {
      console.log("  " + kind + ": hits=" + entry.hits +
                  " sites=" + entry.sites);
    }
    console.log("Instrumentation adds overhead; run throughput tests with the flag off.");
  }
}
