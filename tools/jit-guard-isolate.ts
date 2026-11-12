#!/usr/bin/env bun
/**
 * Isolate each unmodified ScratchBench case into a separate headless .sb3.
 * The original source and kernel bodies are never changed.
 *
 * bun tools/jit-guard-isolate.ts --names list_read,app_raycast --out board/prof-005/isolated
 */
import { spawnSync } from "node:child_process";
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

type BenchCase = { name: string; ops: number; source: string; start: number; end: number };
const suiteNames = ["loops", "vars", "math", "strings", "lists", "algos", "apps", "sched"];

function discover(source: string): Map<string, BenchCase> {
  const cases = new Map<string, BenchCase>();
  for (const suite of suiteNames) {
    const file = join(source, "Stage", suite + ".fractch");
    const code = readFileSync(file, "utf8");
    const re = /@bench_end\("([a-z_]+)",\s*(\d+)\);/g;
    for (const m of code.matchAll(re)) {
      const name = m[1];
      const end = m.index! + m[0].length;
      const start = code.lastIndexOf("  @bench_begin();", m.index);
      if (start < 0) throw Error("No bench_begin for " + name);
      if (cases.has(name)) throw Error("Duplicate case: " + name);
      cases.set(name, { name, ops: Number(m[2]), source: code, start, end });
    }
  }
  return cases;
}

function buildFixture(
  source: string,
  rootOut: string,
  bench: BenchCase,
  fractch: string,
): void {
  const base = join(rootOut, "src", bench.name);
  const sb3 = join(rootOut, bench.name + ".sb3");
  rmSync(base, { recursive: true, force: true });
  cpSync(source, base, { recursive: true });
  const entry = join(base, "Stage", "main.fractch");
  let main = readFileSync(entry, "utf8");
  for (const suite of suiteNames) {
    const call = "  @suite_" + suite + "();";
    if (!main.includes(call)) throw Error("Missing " + call);
    main = main.replace(call, "");
  }
  let body: string;
  if (bench.name.startsWith("sched_")) {
    // Same dependency initialization as the source suite.
    body = "  @sched_spawn();\n  @" + bench.name + "();";
  } else {
    body = bench.source.slice(bench.start, bench.end);
    // The original list suite pre-fills the list once before the read,
    // replace, delete and search kernels.
    if (bench.name.startsWith("list_") && bench.name !== "list_append")
      body = "  @k_list_fill();\n" + body;
  }
  const done = '  @log_text("DONE");';
  if (!main.includes(done)) throw Error("Missing suite completion marker");
  main = main.replace(done, body + "\n\n" + done);
  writeFileSync(entry, main);
  for (const command of [
    [fractch, "check", base],
    [fractch, "to", sb3, "from", base],
  ]) {
    const proc = spawnSync(command[0], command.slice(1), {
      cwd: resolve("scratch-bench"),
      encoding: "utf8",
      maxBuffer: 4 * 1024 * 1024,
    });
    if (proc.status !== 0)
      throw Error("fractch failure in " + bench.name + ": " + proc.stderr);
  }
  console.log("Built " + bench.name + " (original ops/unit " + bench.ops + ")");
}

if (import.meta.main) {
  try {
    const source = resolve("scratch-bench/performance/project");
    const cases = discover(source);
    if (cases.size !== 47) throw Error("Expected 47 original cases, found " + cases.size);
    const args = process.argv.slice(2);
    const namesArg = args.indexOf("--names");
    const outArg = args.indexOf("--out");
    const names = namesArg < 0
      ? [...cases.keys()]
      : args[namesArg + 1]?.split(",").filter(Boolean) ?? [];
    const out = resolve(outArg < 0 ? "board/prof-005/isolated" : args[outArg + 1]);
    const fractch = resolve("scratch-bench/node_modules/.bin/fractch");
    if (!existsSync(fractch)) throw Error("Missing local fractch binary: " + fractch);
    mkdirSync(out, { recursive: true });
    for (const name of names) {
      const bench = cases.get(name);
      if (!bench) throw Error("Unknown ScratchBench case " + name);
      buildFixture(source, out, bench, fractch);
    }
  } catch (err) {
    console.error(err);
    process.exitCode = 1;
  }
}
