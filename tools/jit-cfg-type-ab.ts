#!/usr/bin/env bun
/**
 * PROF-006: paired, uninstrumented A/B of CFG numeric shadow hints.
 * The benchmark's own rate includes the project's 100ms rounds; process
 * wall time is recorded separately for startup and compilation.
 *
 * bun tools/jit-cfg-type-ab.ts --bin board/.prof-005-cmake/xyo \
 *   --sb3 scratch-bench/.cache/build/performance.sb3 \
 *   --out board/prof-006/full-ab --pairs 1 --cpu 14
 *
 * Does not modify the ScratchBench fixture, runtime source, or defaults.
 */
import { spawnSync } from "node:child_process";
import { mkdirSync, writeFileSync } from "node:fs";
import { resolve, join } from "node:path";

export type BenchRow = {
  id: string;
  totalMs: number;
  operations: number;
  rate: number;
  bestRate: number;
  checksum: string;
};
export type RunResult = {
  mode: "off" | "on";
  index: number;
  wallMs: number;
  exit: number | null;
  done: boolean;
  rows: Record<string, BenchRow>;
  error?: string;
};
export type CaseSummary = {
  id: string;
  offRate: number;
  onRate: number;
  ratio: number;
  checksumsEqual: boolean;
};

export function parseBenchRows(output: string): Record<string, BenchRow> {
  const rows: Record<string, BenchRow> = {};
  for (const line of output.split(/\r?\n/)) {
    const fields = line.split("|").map((s) => s.trim());
    if (fields.length !== 6 || !/^[a-z]+_[a-z0-9_]+$/.test(fields[0]))
      continue;
    const numbers = fields.slice(1, 5).map(Number);
    if (numbers.some((n) => !Number.isFinite(n))) continue;
    if (Object.hasOwn(rows, fields[0]))
      throw Error("Duplicate performance row: " + fields[0]);
    rows[fields[0]] = {
      id: fields[0],
      totalMs: numbers[0],
      operations: numbers[1],
      rate: numbers[2],
      bestRate: numbers[3],
      checksum: fields[5],
    };
  }
  return rows;
}

export function summarizeCases(runs: RunResult[]): CaseSummary[] {
  const off = runs.filter((r) => r.mode === "off" && r.exit === 0 && r.done);
  const on = runs.filter((r) => r.mode === "on" && r.exit === 0 && r.done);
  if (!off.length || !on.length) return [];
  const median = (numbers: number[]): number => {
    const sorted = numbers.slice().sort((a, b) => a - b);
    const i = Math.floor(sorted.length / 2);
    return sorted.length % 2 ? sorted[i] : (sorted[i - 1] + sorted[i]) / 2;
  };
  const baseline = new Set(Object.keys(off[0].rows));
  const result: CaseSummary[] = [];
  for (const id of baseline) {
    const offRows = off.map((r) => r.rows[id]).filter(Boolean);
    const onRows = on.map((r) => r.rows[id]).filter(Boolean);
    if (offRows.length !== off.length || onRows.length !== on.length) continue;
    const offRate = median(offRows.map((r) => r.rate));
    const onRate = median(onRows.map((r) => r.rate));
    if (!(offRate > 0 && onRate > 0)) continue;
    const checksums = [...offRows, ...onRows].map((r) => r.checksum);
    result.push({
      id,
      offRate,
      onRate,
      ratio: onRate / offRate,
      checksumsEqual: checksums.every((s) => s === checksums[0]),
    });
  }
  return result.sort((a, b) => a.ratio - b.ratio);
}

function options(argv: string[]) {
  const kv = new Map<string, string>();
  for (let i = 0; i < argv.length; i += 2) {
    if (!argv[i]?.startsWith("--") || !argv[i + 1])
      throw Error("Expected --bin PATH --sb3 PATH --out DIR --pairs N [--cpu N]");
    kv.set(argv[i].slice(2), argv[i + 1]);
  }
  const pairs = Number(kv.get("pairs") ?? "1");
  if (!Number.isInteger(pairs) || pairs < 1 || pairs > 10)
    throw Error("--pairs must be an integer between 1 and 10");
  if (!kv.get("bin") || !kv.get("sb3"))
    throw Error("--bin and --sb3 are required");
  const cpu = kv.get("cpu");
  if (cpu !== undefined && !/^\d+$/.test(cpu))
    throw Error("--cpu must be a nonnegative integer");
  return {
    bin: resolve(kv.get("bin")!),
    sb3: resolve(kv.get("sb3")!),
    out: resolve(kv.get("out") ?? "board/prof-006/full-ab"),
    cpu,
    pairs,
  };
}

function run(binary: string, sb3: string, mode: "off" | "on",
             index: number, cpu?: string): RunResult & { logs: { stdout: string; stderr: string } } {
  const args = [binary, "run", sb3, "--print-list", "output", "--fps", "30"];
  const cmd = cpu === undefined ? args : ["taskset", "-c", cpu, ...args];
  const env = { ...process.env };
  for (const key of [
    "XYO_JIT_TYPE_FLOW", "XYO_JIT_LAZY_TYPED_SSA",
    "XYO_JIT_STATIC_TYPE_SITES", "XYO_JIT_LOCAL_READ_FORWARD",
  ]) delete env[key];
  env.XYO_JIT_MODE = "hotspot";
  env.XYO_JIT_GUARD_STATS = "0";
  env.XYO_JIT_CFG_TYPE_HINTS = mode === "on" ? "1" : "0";
  const started = performance.now();
  const result = spawnSync(cmd[0], cmd.slice(1), {
    env, encoding: "utf8", timeout: 180_000, maxBuffer: 16 * 1024 * 1024,
  });
  const stdout = result.stdout ?? "";
  const stderr = result.stderr ?? "";
  return {
    mode, index, wallMs: Math.round(performance.now() - started),
    exit: result.status,
    done: stdout.split(/\r?\n/).some((s) => s.trim() === "DONE"),
    rows: parseBenchRows(stdout),
    ...(result.error ? { error: result.error.message } : {}),
    // stdout and stderr are persisted separately by the caller.
    logs: { stdout, stderr },
  };
}

if (import.meta.main) {
  try {
    const opts = options(process.argv.slice(2));
    mkdirSync(opts.out, { recursive: true });
    const runs: RunResult[] = [];
    for (let pair = 0; pair < opts.pairs; pair++) {
      const order: ("off" | "on")[] = pair % 2
        ? ["on", "off", "off", "on"]
        : ["off", "on", "on", "off"];
      for (const mode of order) {
        const index = runs.length;
        const { logs, ...result } = run(opts.bin, opts.sb3, mode, index, opts.cpu);
        writeFileSync(join(opts.out, String(index) + "-" + mode + ".stdout.log"),
                      logs.stdout);
        writeFileSync(join(opts.out, String(index) + "-" + mode + ".stderr.log"),
                      logs.stderr);
        runs.push(result);
        writeFileSync(join(opts.out, "results.json"),
                      JSON.stringify({ options: opts, runs,
                        cases: summarizeCases(runs) }, null, 2) + "\n");
        const checksumBad = Object.values(result.rows).filter(
          (row) => !row.id.startsWith("sched_") &&
                   runs[0]?.rows[row.id] &&
                   runs[0].rows[row.id].checksum !== row.checksum,
        ).map((row) => row.id);
        console.log(
          "RUN " + index + " " + mode +
          " exit=" + result.exit + " done=" + result.done +
          " rows=" + Object.keys(result.rows).length +
          " wallMs=" + result.wallMs +
          " deterministicChecksumMismatches=" + checksumBad.join(","),
        );
        if (result.exit !== 0 || !result.done ||
            Object.keys(result.rows).length !== 47 || checksumBad.length)
          throw Error("Incomplete or divergent run " + index + ": " +
                      (result.error ?? checksumBad.join(",")));
      }
    }
    const cases = summarizeCases(runs);
    console.log("Done. Results are diagnostic paired medians; not official weighted scores.");
    for (const row of cases.filter((r) => r.ratio < 0.9 || r.ratio > 1.1))
      console.log(row.id + " ratio_on_off=" + row.ratio.toFixed(3) +
                  " stable_checksum=" + row.checksumsEqual);
  } catch (error) {
    console.error(error);
    process.exitCode = 1;
  }
}
