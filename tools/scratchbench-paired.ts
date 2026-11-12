import { mkdir, copyFile, chmod, readFile, writeFile } from "node:fs/promises";
import { resolve, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { buildSuite } from "../scratch-bench/src/build.ts";
import { measureSuite, statsOf } from "../scratch-bench/src/measure-suite.ts";
import { performance, type PerfRecord } from "../scratch-bench/src/suites.ts";
import { summarize } from "../scratch-bench/src/stats.ts";
import { hostInfo, repoInfo } from "../scratch-bench/src/hostinfo.ts";
import { BENCHMARKS } from "../scratch-bench/site/shared/benchmarks.ts";
import { scoreRuntime, SCORE_VERSION } from "../scratch-bench/site/shared/scoring.ts";
import type { SuiteResult } from "../scratch-bench/src/report.ts";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const REGION = "XYO_JIT_NUMERIC_REGION";
const MIXED = "XYO_JIT_NUMERIC_REGION_MIXED";
const FACTS = "XYO_JIT_NUMERIC_REGION_FACT_GUARDS";
export const PROFILES: Record<string, Record<string, string>> = {
  default: { [REGION]: "0", [MIXED]: "0", [FACTS]: "0" },
  region: { [REGION]: "1", [MIXED]: "0", [FACTS]: "0" },
  mixed: { [REGION]: "1", [MIXED]: "1", [FACTS]: "0" },
  facts: { [REGION]: "1", [MIXED]: "1", [FACTS]: "1" },
  hybrid: { [REGION]: "1", [MIXED]: "1", [FACTS]: "1", XYO_JIT_NUMERIC_REGION_COST_SELECT: "1" },
  lists: { [REGION]: "1", [MIXED]: "1", [FACTS]: "1", XYO_JIT_NUMERIC_REGION_COST_SELECT: "1", XYO_JIT_NUMERIC_REGION_READ_LIST: "1" },
  list_finite: { [REGION]: "1", [MIXED]: "1", [FACTS]: "1", XYO_JIT_NUMERIC_REGION_COST_SELECT: "1", XYO_JIT_NUMERIC_REGION_READ_LIST: "1", XYO_JIT_NUMERIC_REGION_LIST_FINITE: "1" },
};
const HELP = "Usage: bun tools/scratchbench-paired.ts --binary PATH --out NEWDIR [--blocks 3] [--profiles default,mixed,facts] [--cpu 12] [--source-revision REV]\nRuns the canonical full performance suite sequentially with a frozen binary and SB3. Output directory must not exist.";
export function parseArgs(args: string[]) {
  if (args.length === 1 && args[0] === "--help") return null;
  const values: Record<string, string> = {};
  for (let i = 0; i < args.length; i += 2) {
    const key = args[i], value = args[i + 1];
    if (!["--binary", "--out", "--blocks", "--profiles", "--cpu", "--source-revision"].includes(key)) throw new Error(`Unknown option ${key}`);
    if (!value || value.startsWith("--") || key in values) throw new Error(`Missing or repeated option ${key}`);
    values[key] = value;
  }
  if (!values["--binary"] || !values["--out"]) throw new Error(HELP);
  const blocks = Number(values["--blocks"] ?? 3);
  if (!Number.isSafeInteger(blocks) || blocks < 1) throw new Error("--blocks must be a positive integer");
  const cpu = values["--cpu"] ?? "12";
  if (!/^\d+$/.test(cpu) || !Number.isSafeInteger(Number(cpu))) throw new Error("--cpu must be a single nonnegative CPU number");
  const profiles = (values["--profiles"] ?? "default,mixed,facts").split(",");
  if (!profiles.length || profiles.some(p => !Object.hasOwn(PROFILES, p)) || new Set(profiles).size !== profiles.length) throw new Error("Invalid or duplicate profiles");
  return { binary: resolve(values["--binary"]), out: resolve(values["--out"]), blocks, cpu, profiles, sourceRevision: values["--source-revision"] ?? null };
}
export function runOrder(profiles: string[], block: number) {
  // Fixed-seed Fisher-Yates, paired with reversal of the preceding block's order.
  let seed = (0x5eed005 + Math.floor(block / 2)) >>> 0;
  const out = [...profiles];
  for (let i = out.length - 1; i > 0; i--) {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    const j = seed % (i + 1);
    [out[i], out[j]] = [out[j], out[i]];
  }
  return block % 2 ? out.reverse() : out;
}
export function validateData(data: Record<string, PerfRecord>, reference?: Record<string, string>) {
  if (!data || typeof data !== "object") throw new Error("Missing performance data");
  const expected = new Set(BENCHMARKS.map(b => b.id));
  const unknown = Object.keys(data).filter(id => !expected.has(id));
  if (unknown.length) throw new Error(`Unknown performance IDs: ${unknown.join(",")}`);
  for (const b of BENCHMARKS) {
    const row = data[b.id];
    if (!row || !Number.isFinite(row.ops_per_ms) || row.ops_per_ms <= 0 || !Number.isFinite(row.total_ms) || row.total_ms <= 0) throw new Error(`Missing/invalid performance row ${b.id}`);
    if (b.category !== "scheduler") {
      if (typeof row.checksum !== "string" || !row.checksum.length) throw new Error(`Missing checksum ${b.id}`);
      if (reference && row.checksum !== reference[b.id]) throw new Error(`Checksum mismatch ${b.id}: ${row.checksum} != ${reference[b.id]}`);
    }
  }
}
const sha = (bytes: Uint8Array) => new Bun.CryptoHasher("sha256").update(bytes).digest("hex");
const hashFile = async (p: string) => sha(await readFile(p));
const save = (p: string, data: unknown) => writeFile(p, JSON.stringify(data, null, 2) + "\n");
const git = (args: string[]) => {
  const result = Bun.spawnSync(["git", ...args], { cwd: ROOT, stdout: "pipe", stderr: "pipe" });
  return result.exitCode === 0 ? result.stdout.toString().trim() : null;
};
type Row = { block: number; position: number; profile: string; command: string[]; env: Record<string, string>; timestamp: string; run: SuiteResult["runs"][number]; total?: number | null; validationError?: string };

export async function main(args: string[]) {
  const o = parseArgs(args);
  if (!o) { console.log(HELP); return; }
  const bytes = await readFile(o.binary);
  if (bytes.length < 4 || bytes.subarray(0, 4).toString("hex") !== "7f454c46") throw new Error("--binary must be an ELF executable");
  for (const profile of o.profiles) for (const [key, value] of Object.entries(PROFILES[profile])) {
    if (value === "1" && !bytes.includes(Buffer.from(key))) throw new Error(`Binary lacks requested feature literal ${key}; refusing unsupported profile ${profile}`);
  }
  await mkdir(o.out); // Exclusive: no recursive=true and no reuse of an existing directory.
  const frozenBinary = join(o.out, "runtime"), frozenInput = join(o.out, "performance.sb3");
  const rows: Row[] = [];
  const metadata: Record<string, any> = {
    schema_version: 1, status: "initializing", timestamp: new Date().toISOString(), options: o,
    host: hostInfo(), rootGitHead: git(["rev-parse", "HEAD"]), rootGitDirty: git(["status", "--porcelain", "--untracked-files=no"]),
    benchmarkRepo: repoInfo(), sourceRevision: o.sourceRevision,
    sourceRevisionMeaning: "Caller-supplied runtime source revision; root/benchmark git HEAD does not identify the input binary.",
    sourceBinary: o.binary, sourceBinarySha256Before: sha(bytes), frozenBinary,
    profiles: Object.fromEntries(o.profiles.map(p => [p, PROFILES[p]])), cpuAffinity: o.cpu,
    orderSeed: "0x5eed005 + floor(block/2); adjacent block reversed", timeoutMs: 180000,
    scoreVersion: SCORE_VERSION, warnings: ["Feature literals establish presence only, not that a runtime honors flags.", "CPU affinity does not control frequency, temperature, or background load.", "Paired per-run TOTALs are descriptive; official aggregate TOTAL scores per-item median rates."],
  };
  try {
    await writeFile(frozenBinary, bytes, { flag: "wx" });
    await chmod(frozenBinary, 0o555);
    await copyFile(fileURLToPath(import.meta.url), join(o.out, "scratchbench-paired.ts"));
    metadata.driverSha256 = await hashFile(join(o.out, "scratchbench-paired.ts"));
    metadata.scoringSha256 = await hashFile(join(ROOT, "scratch-bench/site/shared/scoring.ts"));
    metadata.benchmarksSha256 = await hashFile(join(ROOT, "scratch-bench/site/shared/benchmarks.ts"));
    const built = await buildSuite(performance, { offline: true, log: line => console.error(line) });
    if (built.hash !== "e8bd5be3e7345088") throw new Error(`Unexpected canonical performance source hash ${built.hash}`);
    metadata.suite = { id: "performance", source_hash: built.hash };
    await copyFile(built.sb3, frozenInput);
    await chmod(frozenInput, 0o444);
    metadata.frozenInputSha256Before = await hashFile(frozenInput);
    metadata.frozenBinarySha256Before = await hashFile(frozenBinary);
    const cleanEnv = Object.fromEntries(Object.entries(process.env).filter(([k, v]) => !k.startsWith("XYO_") && v !== undefined)) as Record<string, string>;
    metadata.removedXYOKeys = Object.keys(process.env).filter(k => k.startsWith("XYO_"));
    metadata.status = "running";
    await save(join(o.out, "metadata.json"), metadata);
    let reference: Record<string, string> | undefined;
    for (let block = 0; block < o.blocks; block++) {
      const order = runOrder(o.profiles, block);
      for (let position = 0; position < order.length; position++) {
        const profile = order[position];
        const env = { ...cleanEnv, XYO_JIT_MODE: "hotspot", ...PROFILES[profile] };
        const command = ["taskset", "-c", o.cpu, frozenBinary, "run", frozenInput, "--print-list", "output", "--fps", "30"];
        console.error(`block ${block + 1}/${o.blocks}, profile ${profile}`);
        const measured = await measureSuite(performance, { display: false, command: () => ({ cmd: command, cwd: ROOT, env, projectLine: /^(.*)$/ }) }, frozenInput, { runs: 1, timeoutMs: metadata.timeoutMs, displayKind: "x11", log: line => console.error(line) });
        const run = measured.runs[0];
        const relevantEnv = Object.fromEntries(Object.entries(env).filter(([key]) => key.startsWith("XYO_") || ["PATH", "LD_LIBRARY_PATH", "LD_PRELOAD", "OMP_NUM_THREADS", "MALLOC_CONF", "GLIBC_TUNABLES"].includes(key)));
        const row: Row = { block: block + 1, position, profile, command, env: relevantEnv, timestamp: new Date().toISOString(), run };
        rows.push(row);
        await save(join(o.out, "runs.json"), rows);
        try {
          if (!run?.ok) throw new Error(run?.error ?? "Official measurement failed");
          const data = run.data as Record<string, PerfRecord>;
          validateData(data, reference);
          reference ??= Object.fromEntries(BENCHMARKS.filter(b => b.category !== "scheduler").map(b => [b.id, data[b.id].checksum]));
          row.total = scoreRuntime(Object.fromEntries(BENCHMARKS.map(b => [b.id, data[b.id].ops_per_ms]))).total;
          await save(join(o.out, "runs.json"), rows);
        } catch (error) {
          row.validationError = (error as Error).message;
          await save(join(o.out, "runs.json"), rows);
          throw error;
        }
      }
    }
    const summary: Record<string, any> = {};
    for (const profile of o.profiles) {
      const selected = rows.filter(r => r.profile === profile);
      const runs = selected.map(r => r.run);
      const results = Object.fromEntries(BENCHMARKS.map(b => [b.id, {
        ops_per_ms: summarize(runs.map(r => (r.data as Record<string, PerfRecord>)[b.id].ops_per_ms)),
        total_ms: summarize(runs.map(r => (r.data as Record<string, PerfRecord>)[b.id].total_ms)),
      }]));
      const result: SuiteResult = {
        schema_version: 1, timestamp: new Date().toISOString(), suite: metadata.suite, repo: metadata.benchmarkRepo, host: metadata.host,
        runtime: { id: "xyo-cpp", name: "xyo-cpp", kind: "native", version: "custom", launcher: "headless", display: null,
          detail: { binary: frozenBinary, binary_sha256: metadata.frozenBinarySha256Before, source_revision: o.sourceRevision ?? "unknown", cpu_affinity: o.cpu, profile, fps: "30", ...PROFILES[profile] },
          variant: { id: "hotspot", name: "JIT (hotspot)", default: true } },
        runs, stats: statsOf(runs), results,
      };
      await mkdir(join(o.out, profile));
      await save(join(o.out, profile, "performance-xyo-cpp.json"), result);
      summary[profile] = { official: scoreRuntime(Object.fromEntries(BENCHMARKS.map(b => [b.id, results[b.id].ops_per_ms!.median]))), perRunTotalsDescriptive: summarize(selected.map(r => r.total!)) };
    }
    const baseline = o.profiles.includes("default") ? "default" : o.profiles[0];
    metadata.pairedPerRunTotalsDescriptive = rows.filter(r => r.profile !== baseline).map(r => {
      const base = rows.find(b => b.block === r.block && b.profile === baseline)!;
      return { block: r.block, profile: r.profile, baseline, total: r.total, baselineTotal: base.total, ratio: r.total! / base.total! };
    });
    await save(join(o.out, "summary.json"), summary);
    metadata.status = "complete";
  } catch (error) {
    metadata.status = "failed";
    metadata.error = (error as Error).message;
    throw error;
  } finally {
    metadata.sourceBinarySha256After = await hashFile(o.binary).catch(() => null);
    metadata.frozenBinarySha256After = await hashFile(frozenBinary).catch(() => null);
    metadata.frozenInputSha256After = await hashFile(frozenInput).catch(() => null);
    const stable = metadata.sourceBinarySha256Before === metadata.sourceBinarySha256After && metadata.frozenBinarySha256Before === metadata.frozenBinarySha256After && metadata.frozenInputSha256Before === metadata.frozenInputSha256After;
    metadata.integrityStable = stable;
    if (metadata.status === "complete" && !stable) { metadata.status = "failed"; metadata.error = "Binary/input hash changed during measurement"; }
    await save(join(o.out, "metadata.json"), metadata);
    if (metadata.status === "failed" && metadata.error === "Binary/input hash changed during measurement") throw new Error(metadata.error);
  }
}
if (import.meta.main) main(process.argv.slice(2)).catch(error => { console.error(error.message); process.exitCode = 1; });
