/**
 * scratch-bench の CI 出力を検証し、GitHub Actions の Step Summary に表示する。
 *
 * ベンチ本体は「Parity の一部が不一致」でも成功終了するため、未完了・
 * 実行失敗と、既知の不一致を区別して検査する。
 * 性能数値は共有 GitHub Runner のノイズが大きいので合否判定には使わない。
 */
import fs from "node:fs";
import path from "node:path";

type Run = { ok: boolean; error?: string };
type Summary = { median?: number };
type BenchResult = {
  suite: { id: string };
  runtime: { id: string; name: string; variant?: { id: string; name: string } | null };
  runs: Run[];
  stats: { run_ms?: Summary | null };
  results: unknown;
  error?: string;
  skipped?: string;
};

type ParityData = {
  verdict: string;
  passed: number | null;
  total: number | null;
};

type PerfData = Record<string, { ops_per_ms?: { median?: number } | null }>;

const resultDirs = [
  { suite: "performance", dir: "scratch-bench/results/ci-performance", optional: false },
  { suite: "performance", dir: "scratch-bench/results/ci-turbowarp", optional: true },
  { suite: "parity", dir: "scratch-bench/results/ci-parity", optional: false },
] as const;

// 既知の差異を理由に 100% を要求しない。急激な意味論回帰は検出する。
// scratch-bench の fixture / 採点方法を更新する場合はこの基準も再評価する。
const MIN_PARITY_RATE = 0.96;
const XYO_VARIANTS = ["hotspot", "interpreter", "eager", "aot"] as const;
const TURBOWARP_VARIANTS = ["compiler", "interpreter"] as const;

const expected = new Set([
  ...XYO_VARIANTS.map((variant) => `performance/xyo-cpp/${variant}`),
  ...XYO_VARIANTS.map((variant) => `parity/xyo-cpp/${variant}`),
]);

const errors: string[] = [];
const warnings: string[] = [];
const rows: string[] = [];
const seen = new Set<string>();

const fail = (message: string): void => {
  errors.push(message);
};

const metric = (result: BenchResult, report: (message: string) => void): string => {
  if (result.suite.id === "parity") {
    const parity = result.results as ParityData | null;
    const passed = parity?.passed;
    const total = parity?.total;
    if (
      !parity || !["passed", "failed"].includes(parity.verdict) ||
      !Number.isInteger(passed) || !Number.isInteger(total) ||
      total === null || passed === null || total <= 0 ||
      passed < 0 || passed > total
    ) {
      report(`Invalid parity result: ${result.runtime.name}`);
      return "invalid";
    }
    const rate = passed / total;
    if (result.runtime.id === "xyo-cpp" && rate < MIN_PARITY_RATE) {
      report(`Parity below ${(MIN_PARITY_RATE * 100).toFixed(0)}%: ${result.runtime.variant?.id} ${passed}/${total}`);
    }
    return `${passed}/${total} (${(rate * 100).toFixed(2)}%)`;
  }

  const perf = result.results as PerfData | null;
  const values = perf ? Object.values(perf) : [];
  if (
    values.length === 0 ||
    values.some((v) => !Number.isFinite(v?.ops_per_ms?.median) || (v.ops_per_ms?.median ?? 0) <= 0)
  ) {
    report(`Invalid performance result: ${result.runtime.name} (${result.runtime.variant?.id})`);
    return "invalid";
  }
  return `${values.length} workloads`;
};

for (const { suite, dir, optional } of resultDirs) {
  if (!fs.existsSync(dir)) {
    if (!optional) fail(`Missing results directory: ${dir}`);
    continue;
  }
  for (const file of fs.readdirSync(dir).filter((name) => name.endsWith(".json") && name !== "meta.json")) {
    const filepath = path.join(dir, file);
    let result: BenchResult;
    try {
      result = JSON.parse(fs.readFileSync(filepath, "utf8")) as BenchResult;
    } catch (error) {
      (optional ? warnings.push.bind(warnings) : fail)(`Cannot parse ${filepath}: ${String(error)}`);
      continue;
    }
    const variant = result.runtime?.variant?.id ?? "default";
    const id = `${result.suite?.id}/${result.runtime?.id}/${variant}`;
    const isOptional = optional && result.runtime?.id === "turbowarp";
    const allowed = expected.has(id) || (isOptional && TURBOWARP_VARIANTS.some((v) => v === variant));
    if (!allowed || result.suite.id !== suite || seen.has(id)) {
      (optional ? warnings.push.bind(warnings) : fail)(`Unexpected or duplicate result: ${filepath} (${id})`);
      continue;
    }
    seen.add(id);
    const report = isOptional ? (message: string) => { warnings.push(message); } : fail;

    const expectedRuns = suite === "performance" ? 3 : 1;
    if (result.error || result.skipped || !Array.isArray(result.runs) ||
        result.runs.length !== expectedRuns || result.runs.some((run) => !run.ok)) {
      report(`Failed/incomplete run: ${id} (${result.error ?? result.skipped ?? result.runs?.find((r) => !r.ok)?.error ?? "no runs"})`);
    }
    const label = result.runtime.variant?.name ?? variant;
    const runTime = result.stats?.run_ms?.median;
    const duration = typeof runTime === "number" && Number.isFinite(runTime)
      ? `${runTime.toFixed(1)} ms`
      : "—";
    rows.push(`| ${suite} | ${result.runtime.name} | ${label} | ${metric(result, report)} | ${duration} |`);
  }
}

for (const id of expected) {
  if (!seen.has(id)) fail(`Missing result: ${id}`);
}

const getScratchBenchSha = (): string => {
  const result = Bun.spawnSync(["git", "-C", "scratch-bench", "rev-parse", "HEAD"], {
    stdout: "pipe",
    stderr: "ignore",
  });
  return result.exitCode === 0 ? result.stdout.toString().trim() : "unavailable";
};

const performanceLog = ".bench/performance.log";
const completeReport = fs.existsSync(performanceLog)
  ? [
    "<details><summary>Full performance report and scores</summary>",
    "",
    "```text",
    fs.readFileSync(performanceLog, "utf8").trim(),
    "```",
    "",
    "</details>",
    "",
  ]
  : [];

const markdown = [
  "## scratch-bench",
  "",
  `xyo-cpp: \`${process.env.GITHUB_SHA ?? "local"}\` / scratch-bench: \`${getScratchBenchSha()}\``,
  "",
  "| Suite | Runtime | Variant | Result | Median run |",
  "| --- | --- | --- | ---: | ---: |",
  ...rows.sort(),
  "",
  `Parity gate: ≥ ${(MIN_PARITY_RATE * 100).toFixed(0)}%. Performance is reported only (not a noisy timing gate).`,
  "",
  ...completeReport,
  ...(errors.length ? ["### Errors", "", ...errors.map((error) => `- ${error}`), ""] : []),
  ...(warnings.length ? ["### Reference warnings", "", ...warnings.map((warning) => `- ${warning}`), ""] : []),
].join("\n");

console.log(markdown);
if (process.env.GITHUB_STEP_SUMMARY) {
  fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, markdown + "\n");
}
if (errors.length > 0) process.exitCode = 1;