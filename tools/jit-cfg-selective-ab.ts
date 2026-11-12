#!/usr/bin/env bun
/**
 * PROF-007: 3-way clean A/B of CFG type hints.
 * --bin FILE --fixtures DIR --names a,b --reps 4 --cpu 14 --out DIR
 */
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { resolve, join } from "node:path";
import { parseBenchRows } from "./jit-cfg-type-ab";

export type Mode = "off" | "broad" | "selective";
export type Reading = {
  name: string;
  mode: Mode;
  iteration: number;
  rate: number;
  checksum: string;
  wallMs: number;
  ok: boolean;
  error?: string;
};

export function schedule(round: number): Mode[] {
  const schedules: Mode[][] = [
    ["off", "broad", "selective"],
    ["selective", "broad", "off"],
    ["broad", "off", "selective"],
    ["selective", "off", "broad"],
    ["off", "selective", "broad"],
    ["broad", "selective", "off"],
  ];
  return [...schedules[round % schedules.length]];
}

export function median(data: number[]): number {
  if (!data.length) throw Error("empty sample");
  const values = [...data].sort((a, b) => a - b);
  const half = Math.floor(values.length / 2);
  return values.length % 2
    ? values[half]
    : (values[half - 1] + values[half]) / 2;
}

export function summarize(readings: Reading[]) {
  if (readings.some((x) => !x.ok)) throw Error("incomplete readings");
  const byMode = (mode: Mode) => median(
    readings.filter((x) => x.mode === mode).map((x) => x.rate),
  );
  const off = byMode("off"), broad = byMode("broad");
  const selective = byMode("selective");
  return {
    off, broad, selective,
    broadRatio: broad / off,
    selectiveRatio: selective / off,
    selectiveVsBroad: selective / broad,
  };
}

function options(argv: string[]) {
  const map = new Map<string, string>();
  for (let i = 0; i < argv.length; i += 2) {
    if (!argv[i]?.startsWith("--") || !argv[i + 1])
      throw Error("expected --key value pairs");
    map.set(argv[i].slice(2), argv[i + 1]);
  }
  const bin = map.get("bin"), fixtures = map.get("fixtures");
  if (!bin || !fixtures) throw Error("--bin and --fixtures are required");
  const reps = Number(map.get("reps") ?? "4");
  if (!Number.isInteger(reps) || reps < 1 || reps > 40)
    throw Error("Invalid --reps");
  const names = (map.get("names") ?? "math_mandel")
    .split(",").filter(Boolean);
  if (!names.length || names.some((n) => !/^[a-z0-9_]+$/.test(n)))
    throw Error("Invalid --names");
  const cpu = map.get("cpu");
  if (cpu && !/^\d+$/.test(cpu)) throw Error("Invalid CPU number");
  return {
    bin: resolve(bin), fixtures: resolve(fixtures), names, reps, cpu,
    out: resolve(map.get("out") ?? "board/prof-007/isolated-ab"),
  };
}

function measure(
  binary: string, fixture: string, name: string,
  mode: Mode, iteration: number, cpu?: string,
): Reading {
  const env = { ...process.env };
  for (const key of Object.keys(env))
    if (key.startsWith("XYO_")) delete env[key];
  env.XYO_JIT_MODE = "hotspot";
  env.XYO_JIT_GUARD_STATS = "0";
  env.XYO_JIT_CFG_TYPE_HINTS = mode === "off" ? "0" : "1";
  env.XYO_JIT_CFG_TYPE_HINTS_SELECTIVE = mode === "selective" ? "1" : "0";
  const argv = [binary, "run", fixture, "--print-list", "output", "--fps", "30"];
  if (cpu) argv.unshift("taskset", "-c", cpu);
  const start = performance.now();
  const run = spawnSync(argv[0], argv.slice(1), {
    env, encoding: "utf8", timeout: 30_000, maxBuffer: 4 * 1024 * 1024,
  });
  const row = parseBenchRows(run.stdout ?? "")[name];
  const done = (run.stdout ?? "").split(/\r?\n/)
    .some((line) => line.trim() === "DONE");
  const ok = run.status === 0 && done && row !== undefined;
  return {
    name, mode, iteration, rate: row?.rate ?? 0,
    checksum: row?.checksum ?? "",
    wallMs: Math.round(performance.now() - start), ok,
    ...(ok ? {} : { error: run.error?.message ?? (run.stderr ?? "").slice(-1200) }),
  };
}

if (import.meta.main) {
  try {
    const o = options(process.argv.slice(2));
    if (!existsSync(o.bin)) throw Error("missing binary " + o.bin);
    mkdirSync(o.out, { recursive: true });
    const results: Record<string, Reading[]> = {};
    for (const name of o.names) {
      const fixture = join(o.fixtures, name + ".sb3");
      if (!existsSync(fixture)) throw Error("missing fixture " + fixture);
      const readings: Reading[] = [];
      for (let i = 0; i < o.reps; i++) {
        for (const mode of schedule(i)) {
          const reading = measure(o.bin, fixture, name, mode, i, o.cpu);
          readings.push(reading);
          results[name] = readings;
          writeFileSync(join(o.out, "results.json"),
            JSON.stringify({ options: o, results }, null, 2) + "\n");
          console.log(name + " i=" + i + " mode=" + mode +
            " rate=" + reading.rate + " checksum=" + reading.checksum +
            " ok=" + reading.ok);
          if (!reading.ok) throw Error(name + ": " + (reading.error ?? "failed"));
          if (!name.startsWith("sched_") &&
              readings.some((x) => x.checksum !== readings[0].checksum))
            throw Error(name + ": deterministic checksum mismatch");
        }
      }
      console.log(name + " ratios=" + JSON.stringify(summarize(readings)));
    }
  } catch (error) {
    console.error(error);
    process.exitCode = 1;
  }
}
