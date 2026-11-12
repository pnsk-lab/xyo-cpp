#!/usr/bin/env bun
/**
 * Measure time from spawning xyo to the first stdout line for a prebuilt
 * native AOT object, using alternating same-binary A/B runs.
 *
 * Examples:
 *   bun tools/bench-startup-aot-ab.ts \
 *     --bin ./build/xyo --project parity.sb3 --aot parity.o \
 *     --env XYO_SB3_DIRECT_BUILD --a 0 --b 1 \
 *     --cpu 10 --pairs 20 --prefix 'Scratch Parity Test:'
 *
 * Does not include AOT compilation time. Requires the runtime to produce an
 * output line (e.g. '--print-list output' on the Scratch Parity fixture).
 */
import { spawn } from "node:child_process";
import { access } from "node:fs/promises";
import path from "node:path";

export interface Settings {
  binary: string;
  project: string;
  object: string;
  environmentVariable: string;
  a: string;
  b: string;
  cpu: number | null;
  pairs: number;
  timeoutMs: number;
  firstLinePrefix: string;
  extraArgs: string[];
}

export function parseArgs(args: string[]): Settings {
  const options = new Map<string, string>();
  const valid = new Set([
    "--bin", "--project", "--aot", "--env", "--a", "--b",
    "--cpu", "--pairs", "--timeout-ms", "--prefix",
  ]);
  let extraArgs: string[] = [];
  for (let i = 0; i < args.length; ++i) {
    const name = args[i];
    if (name === "--") {
      extraArgs = args.slice(i + 1);
      break;
    }
    if (!valid.has(name)) throw new Error("Unknown argument: " + name);
    if (options.has(name)) throw new Error("Duplicate option: " + name);
    const value = args[++i];
    if (value === undefined) throw new Error("Missing value for " + name);
    options.set(name, value);
  }

  const required = (name: string) => {
    const value = options.get(name);
    if (!value) throw new Error("Required option missing: " + name);
    return value;
  };
  const integer = (name: string, fallback: number, min: number) => {
    const value = options.get(name);
    if (value === undefined) return fallback;
    if (!/^[0-9]+$/.test(value) || !Number.isSafeInteger(Number(value)) ||
        Number(value) < min) {
      throw new Error("Invalid " + name + ": " + value);
    }
    return Number(value);
  };
  if (extraArgs.some((argument) => argument === "--aot" ||
      argument.startsWith("--aot="))) {
    throw new Error("Do not override the prebuilt AOT object via extra arguments");
  }
  const environmentVariable = required("--env");
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(environmentVariable)) {
    throw new Error("Invalid environment variable name");
  }
  const cpu = options.has("--cpu") ? integer("--cpu", 0, 0) : null;
  return {
    binary: path.resolve(required("--bin")),
    project: path.resolve(required("--project")),
    object: path.resolve(required("--aot")),
    environmentVariable,
    a: required("--a"),
    b: required("--b"),
    cpu,
    pairs: integer("--pairs", 20, 1),
    timeoutMs: integer("--timeout-ms", 30000, 100),
    firstLinePrefix: options.get("--prefix") ?? "",
    extraArgs,
  };
}

export interface Summary {
  aMedianMs: number;
  bMedianMs: number;
  pairedSavingsMedianMs: number;
  speedupGeometricMean: number;
  bWins: number;
  pairs: number;
}

export function median(values: number[]): number {
  if (values.length === 0) throw new Error("Cannot summarize zero samples");
  const sorted = [...values].sort((x, y) => x - y);
  const mid = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
}

export function summarize(a: number[], b: number[]): Summary {
  if (a.length !== b.length || a.length === 0) {
    throw new Error("Expected equal nonempty sets of A/B samples");
  }
  if ([...a, ...b].some((n) => !Number.isFinite(n) || n <= 0)) {
    throw new Error("Durations must be positive finite numbers");
  }
  return {
    aMedianMs: median(a),
    bMedianMs: median(b),
    pairedSavingsMedianMs: median(a.map((v, i) => v - b[i])),
    speedupGeometricMean: Math.exp(
      a.reduce((sum, v, i) => sum + Math.log(v / b[i]), 0) / a.length,
    ),
    bWins: a.filter((v, i) => v > b[i]).length,
    pairs: a.length,
  };
}

export async function measure(settings: Settings, mode: string): Promise<number> {
  const command = settings.cpu === null
    ? settings.binary
    : "taskset";
  const arguments_ = [
    ...(settings.cpu === null ? [] : ["-c", String(settings.cpu), settings.binary]),
    "play", settings.project, "--aot", settings.object,
    "--print-list", "output", "--list-limit", "200000",
    ...settings.extraArgs,
  ];
  const env = {
    ...process.env,
    SDL_VIDEODRIVER: "offscreen",
    SDL_AUDIODRIVER: "dummy",
    XYO_AOT_PROGRESS: "0",
    XYO_EXIT_ON_DONE: "1",
    [settings.environmentVariable]: mode,
  };
  const started = performance.now();
  const child = spawn(command, arguments_, {
    env,
    stdio: ["ignore", "pipe", "pipe"],
    detached: process.platform !== "win32",
  });
  const kill = () => {
    if (child.pid && process.platform !== "win32") {
      try { process.kill(-child.pid, "SIGKILL"); } catch { /* already finished */ }
    } else {
      child.kill("SIGKILL");
    }
  };
  let firstLine: string | null = null;
  let elapsed = NaN;
  let stdout = "";
  let stderr = "";
  let error: Error | null = null;
  let timedOut = false;

  const timer = setTimeout(() => {
    timedOut = true;
    kill();
  }, settings.timeoutMs);

  child.stdout.on("data", (chunk: Buffer) => {
    if (firstLine !== null) return;
    stdout += chunk.toString("utf8");
    const index = stdout.indexOf("\n");
    if (index >= 0) {
      firstLine = stdout.slice(0, index).replace(/\r$/, "");
      elapsed = performance.now() - started;
      kill();
    } else if (stdout.length > 65536) {
      error = new Error("No first output line in 64KiB");
      kill();
    }
  });
  child.stderr.on("data", (chunk: Buffer) => {
    if (stderr.length <= 1024 * 1024) stderr += chunk.toString("utf8");
  });
  child.on("error", (err) => { error = err; kill(); });
  await new Promise<void>((resolve) => child.on("close", () => resolve()));
  clearTimeout(timer);

  if (error) throw error;
  if (timedOut) throw new Error("Timed out waiting for the first Scratch output");
  if (firstLine === null || firstLine.length === 0) {
    throw new Error("xyo exited before the first stdout line; stderr: " + stderr.slice(-500));
  }
  if (!firstLine.startsWith(settings.firstLinePrefix)) {
    throw new Error("Unexpected first output: " + firstLine.slice(0, 240));
  }
  if (/aot:\s*load failed/i.test(stderr) ||
      !/aot:\s*[1-9][0-9]* scripts precompiled/.test(stderr)) {
    throw new Error("Native AOT did not load (JIT fallback prohibited): " + stderr.slice(-500));
  }
  return elapsed;
}

export async function run(settings: Settings): Promise<Summary> {
  if (process.platform !== "linux") {
    throw new Error("This benchmark requires Linux (taskset and process groups)");
  }
  await Promise.all([
    access(settings.binary), access(settings.project), access(settings.object),
  ]);
  const a: number[] = [];
  const b: number[] = [];
  for (let i = 0; i < settings.pairs; ++i) {
    const order = i % 2 === 0 ? (["a", "b"] as const) : (["b", "a"] as const);
    for (const label of order) {
      const ms = await measure(settings, label === "a" ? settings.a : settings.b);
      (label === "a" ? a : b).push(ms);
    }
    console.log(
      `Pair ${i + 1}/${settings.pairs}: A ${a[i].toFixed(1)}ms; B ${b[i].toFixed(1)}ms`,
    );
  }
  const result = summarize(a, b);
  console.log(
    `A median ${result.aMedianMs.toFixed(1)}ms; B median ${result.bMedianMs.toFixed(1)}ms; ` +
    `paired saving ${result.pairedSavingsMedianMs.toFixed(1)}ms; ` +
    `speedup geometric mean ${result.speedupGeometricMean.toFixed(4)}x; ` +
    `B wins ${result.bWins}/${result.pairs}`,
  );
  return result;
}

if (import.meta.main) {
  try {
    await run(parseArgs(process.argv.slice(2)));
  } catch (error) {
    console.error(error);
    process.exitCode = 1;
  }
}
