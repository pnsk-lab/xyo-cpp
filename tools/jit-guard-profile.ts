#!/usr/bin/env bun
/**
 * PROF-005: profile LLVM JIT fallback entries on original ScratchBench
 * standalone fixtures. Instrumented execution time is NOT a speed score.
 *
 * bun tools/jit-guard-profile.ts --bin board/.prof-005-cmake/xyo
 *   --fixtures build/scratchbench-cause-probes/projects --out board/prof-005
 */
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readdirSync, writeFileSync } from "node:fs";
import { resolve, join } from "node:path";
import { analyzeJitGuardLog } from "./jit-guard-analyze";

export type GuardRun = {
  fixture: string;
  ok: boolean;
  exit: number | null;
  done: boolean;
  timedOut: boolean;
  elapsedMs: number;
  guard: ReturnType<typeof analyzeJitGuardLog>;
  error?: string;
};
type Options = {
  bin: string;
  fixtures: string;
  names: string[];
  out: string;
  timeoutMs: number;
};

function parse(args: string[]): Options {
  const kv = new Map<string, string>();
  for (let i = 0; i < args.length; i += 2) {
    if (!args[i]?.startsWith("--") || !args[i + 1])
      throw Error("usage: --bin PATH --fixtures DIR [--names a,b] [--out DIR] [--timeout 90]");
    kv.set(args[i].slice(2), args[i + 1]);
  }
  const bin = kv.get("bin");
  const fixtures = kv.get("fixtures");
  if (!bin || !fixtures) throw Error("--bin and --fixtures are required");
  const dir = resolve(fixtures);
  const names = kv.has("names")
    ? kv.get("names")!.split(",").map((v) => v.trim()).filter(Boolean)
    : readdirSync(dir).filter((v) => v.endsWith(".sb3"))
      .map((v) => v.slice(0, -4)).sort();
  const timeoutS = Number(kv.get("timeout") ?? "90");
  if (!names.length || !Number.isFinite(timeoutS) || timeoutS <= 0 || timeoutS > 3600)
    throw Error("Invalid fixture selection or timeout");
  return {
    bin: resolve(bin),
    fixtures: dir,
    names,
    out: resolve(kv.get("out") ?? "board/prof-005"),
    timeoutMs: timeoutS * 1000,
  };
}

export function verifyDone(stdout: string): boolean {
  return stdout.split(/\r?\n/).some((s) => s.trim() === "DONE");
}

export function rankGuardKinds(rows: GuardRun[]) {
  const kinds = new Map<string, { hits: number; fixtures: number }>();
  for (const row of rows) {
    if (!row.ok) continue;
    for (const [kind, counts] of Object.entries(row.guard.byKind)) {
      const data = kinds.get(kind) ?? { hits: 0, fixtures: 0 };
      data.hits += counts.hits;
      data.fixtures++;
      kinds.set(kind, data);
    }
  }
  return [...kinds].map(([kind, data]) => ({ kind, ...data }))
    .sort((a, b) => b.hits - a.hits || a.kind.localeCompare(b.kind));
}

export function runGuardFixture(options: Options, name: string): GuardRun {
  if (!/^[A-Za-z0-9_-]+$/.test(name))
    throw Error("invalid fixture name: " + name);
  const file = join(options.fixtures, name + ".sb3");
  if (!existsSync(file)) throw Error("missing fixture: " + file);
  const started = performance.now();
  const result = spawnSync(options.bin, [
    "run", file, "--print-list", "output", "--fps", "30",
  ], {
    env: { ...process.env, XYO_JIT_MODE: "hotspot", XYO_JIT_GUARD_STATS: "1" },
    timeout: options.timeoutMs,
    maxBuffer: 16 * 1024 * 1024,
    encoding: "utf8",
  });
  const stdout = result.stdout ?? "";
  const stderr = result.stderr ?? "";
  writeFileSync(join(options.out, name + ".stdout.log"), stdout);
  writeFileSync(join(options.out, name + ".stderr.log"), stderr);
  const done = verifyDone(stdout);
  const timedOut = result.error?.message.includes("ETIMEDOUT") ?? false;
  return {
    fixture: name,
    ok: result.status === 0 && done,
    exit: result.status,
    done,
    timedOut,
    elapsedMs: Math.round(performance.now() - started),
    guard: analyzeJitGuardLog(stderr),
    ...(result.error ? { error: result.error.message } : {}),
  };
}

if (import.meta.main) {
  try {
    const options = parse(process.argv.slice(2));
    if (!existsSync(options.bin)) throw Error("binary not found: " + options.bin);
    mkdirSync(options.out, { recursive: true });
    const rows: GuardRun[] = [];
    for (const name of options.names) {
      const row = runGuardFixture(options, name);
      rows.push(row);
      writeFileSync(join(options.out, "results.json"),
        JSON.stringify({
          binary: options.bin, fixtures: options.fixtures,
          runs: rows, rankedKinds: rankGuardKinds(rows),
        }, null, 2) + "\n");
      console.log(name + ": " + (row.ok ? "done" : "FAILED") +
        " hits=" + row.guard.hits + " sites=" + row.guard.sites +
        " wall_ms=" + row.elapsedMs);
    }
    console.log("Ranked Guard categories (instrumented runs, not a throughput benchmark):");
    for (const row of rankGuardKinds(rows))
      console.log("  " + row.kind + ": " + row.hits +
                  " across " + row.fixtures + " fixtures");
    if (rows.some((row) => !row.ok)) process.exitCode = 1;
  } catch (error) {
    console.error(error);
    process.exitCode = 2;
  }
}
