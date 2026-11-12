#!/usr/bin/env bun
/**
 * Analyze diagnostic JIT deoptimization events without modifying OSR code.
 *
 * WARNING: Only parses events that the runtime actually logs.
 * This is NOT a complete deoptimization event counter.
 */
import { readFileSync } from "node:fs";

export type DeoptKind =
  | "numeric_cfg_rollback"
  | "native_handoff_adopted"
  | "native_handoff_refused"
  | "nested_osr_deopt_inner"
  | "nested_osr_deopt_outer";

export type DirectDeoptPath = {
  attempts: number;
  resumed: number;
  refused: number;
};

export type DeoptSummary = {
  /** Diagnostic lines only; may overlap with direct counters. */
  observed: number;
  byKind: Record<string, number>;
  byReason: Record<string, number>;
  /** Direct counts emitted at shutdown by XYO_JIT_DEOPT_STATS=1. */
  direct: {
    attempts: number;
    resumed: number;
    refused: number;
    byPath: Record<string, DirectDeoptPath>;
    byReason: Record<string, number>;
  };
};

function add(target: Record<string, number>, key: string): void {
  target[key] = (target[key] ?? 0) + 1;
}

export function analyzeJitDeoptLog(log: string): DeoptSummary {
  const byKind: Record<string, number> = {};
  const byReason: Record<string, number> = {};
  const direct: DeoptSummary["direct"] = {
    attempts: 0, resumed: 0, refused: 0, byPath: {}, byReason: {},
  };
  let observed = 0;
  const record = (kind: DeoptKind, reason: string): void => {
    observed++;
    add(byKind, kind);
    add(byReason, reason);
  };

  for (const line of log.split(/\r?\n/)) {
    const path = /^\[jit-deopt\] path=([a-z_]+) attempts=(\d+) resumed=(\d+) refused=(\d+) rate=\d+(?:\.\d+)?%$/.exec(line);
    if (path) {
      const attempts = Number(path[2]);
      const resumed = Number(path[3]);
      const refused = Number(path[4]);
      if (Number.isSafeInteger(attempts) &&
          Number.isSafeInteger(resumed) &&
          Number.isSafeInteger(refused) &&
          resumed + refused === attempts) {
        const name = path[1];
        const prev = direct.byPath[name] ?? {
          attempts: 0, resumed: 0, refused: 0,
        };
        direct.byPath[name] = {
          attempts: prev.attempts + attempts,
          resumed: prev.resumed + resumed,
          refused: prev.refused + refused,
        };
        direct.attempts += attempts;
        direct.resumed += resumed;
        direct.refused += refused;
      }
      continue;
    }
    const reason = /^\[jit-deopt\] reason=([a-z_]+) count=(\d+)$/.exec(line);
    if (reason) {
      const count = Number(reason[2]);
      if (Number.isSafeInteger(count))
        direct.byReason[reason[1]] = (direct.byReason[reason[1]] ?? 0) + count;
      continue;
    }
    if (line.includes("[native-simple-deopt]")) {
      const adopted = /\badopted=([01])\b/.exec(line);
      if (adopted)
        record(
          adopted[1] === "1" ? "native_handoff_adopted" : "native_handoff_refused",
          adopted[1] === "1" ? "native_handoff_adopted" : "native_handoff_refused",
        );
    } else if (line.includes("xyo-common-cfg: deopt")) {
      const reason = /\breason=([^\s]+)/.exec(line)?.[1] ?? "unspecified";
      record("numeric_cfg_rollback", "numeric:" + reason);
    } else if (line.includes("[jit-osr-inflight] deopt")) {
      const inner = line.includes("deopt inner ");
      const reason = line.includes("source-changed=1")
        ? "nested_osr:source_changed" : "nested_osr:unspecified";
      record(inner ? "nested_osr_deopt_inner" : "nested_osr_deopt_outer", reason);
    }
  }

  return { observed, byKind, byReason, direct };
}

function sortedEntries(data: Record<string, number>): [string, number][] {
  return Object.entries(data).sort(
    ([ak, av], [bk, bv]) => bv - av || ak.localeCompare(bk),
  );
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  const json = args.includes("--json");
  const paths = args.filter((arg) => arg !== "--json");
  if (paths.length > 1 || paths.some((arg) => arg.startsWith("-") && arg !== "-")) {
    console.error("Usage: bun tools/jit-deopt-analyze.ts [--json] [logfile|-]");
    process.exit(2);
  }
  const log = readFileSync(paths.length && paths[0] !== "-" ? paths[0] : 0, "utf8");
  const result = analyzeJitDeoptLog(log);
  if (json) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    console.log("Direct deopt attempts: " + result.direct.attempts +
                " (resumed " + result.direct.resumed +
                ", refused " + result.direct.refused + ")");
    for (const [name, data] of Object.entries(result.direct.byPath).sort()) {
      console.log("  " + name + ": " + data.resumed + "/" +
                  data.attempts + " resumed");
    }
    console.log("Direct reasons:");
    for (const [reason, count] of sortedEntries(result.direct.byReason))
      console.log("  " + reason + ": " + count);
    console.log("Observed diagnostic events: " + result.observed);
    for (const [kind, count] of sortedEntries(result.byKind))
      console.log("  " + kind + ": " + count);
    console.log("Reasons:");
    for (const [reason, count] of sortedEntries(result.byReason))
      console.log("  " + reason + ": " + count);
    console.log(
      "Note: direct counters cover only instrumented deopt paths; " +
      "diagnostic lines may duplicate direct events and must not be summed.",
    );
  }
}
