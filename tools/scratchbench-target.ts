import { readFile, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { BENCHMARKS, CATEGORY_IDS, CATEGORIES, benchmarksIn } from "../scratch-bench/site/shared/benchmarks.ts";
import { scoreRuntime, SCORE_VERSION, type Medians } from "../scratch-bench/site/shared/scoring.ts";

type Json = Record<string, any>;
export function analyzeResult(input: Json, target = 200000) {
  if (!(Number.isFinite(target) && target > 0)) throw new Error("Target must be finite and positive");
  const medians: Medians = Object.fromEntries(BENCHMARKS.map(b => [b.id, input.results?.[b.id]?.ops_per_ms?.median]));
  const missing = BENCHMARKS.filter(b => {
    const v = medians[b.id];
    return typeof v !== "number" || !Number.isFinite(v) || v <= 0;
  }).map(b => b.id);
  if (missing.length) throw new Error(`Complete positive coverage required (${BENCHMARKS.length - missing.length}/${BENCHMARKS.length}); invalid: ${missing.join(", ")}`);
  const official = scoreRuntime(medians);
  if (official.total === null) throw new Error("Official score is unavailable");
  const weightSum = CATEGORY_IDS.reduce((s, c) => s + CATEGORIES[c].weight, 0);
  const categories = Object.fromEntries(CATEGORY_IDS.map(c => {
    const items = benchmarksIn(c);
    const itemWeightSum = items.reduce((s, b) => s + (b.weight ?? 1), 0);
    const exponent = CATEGORIES[c].weight / weightSum;
    return [c, {
      ...official.categories[c], exponent,
      items: items.map(b => ({ id: b.id, rate: medians[b.id] as number, exponent: exponent * (b.weight ?? 1) / itemWeightSum })),
      requiredMultiplierAlone: Math.exp(Math.log(target / official.total!) / exponent),
      scenarios: [2, 10].map(multiplier => ({ multiplier, total: official.total! * multiplier ** exponent })),
    }];
  }));
  const warnings = ["Benchmark repository SHA is not runtime binary/source identity; metadata does not prove identical CPU affinity or load."];
  const runs = Array.isArray(input.runs) ? input.runs : [];
  const checksums = BENCHMARKS.filter(b => b.category !== "scheduler").map(b => {
    const values = runs.map((r: Json) => r.data?.[b.id]?.checksum ?? null);
    const available = values.filter((v: any) => v !== null);
    return { id: b.id, values, available: available.length, consistent: available.length ? available.every((v: any) => v === available[0]) : null };
  });
  if (!runs.length) warnings.push("No run-level checksum data available.");
  else if (checksums.some(c => c.available < runs.length)) warnings.push("Some non-scheduler run-level checksums are unavailable.");
  if (runs.some((r: Json) => r.ok !== true)) warnings.push("Some recorded runs were not successful; inspect raw runs.");
  if (checksums.some(c => c.consistent === false)) warnings.push("Non-scheduler checksums differ between runs.");
  return {
    scoreVersion: SCORE_VERSION, target, official, categories,
    remainingFactor: target / official.total, remainingLogGain: Math.log(target / official.total),
    provenance: { suite: input.suite ?? null, runtime: input.runtime ?? null, host: input.host ?? null, repo: input.repo ?? null, timestamp: input.timestamp ?? null,
      rawMetadata: Object.fromEntries(Object.entries(input).filter(([key]) => !["results", "runs", "stats"].includes(key))),
    },
    checksums, warnings,
  };
}

export function compareResults(base: Json, candidate: Json, target = 200000) {
  const hash = base.suite?.source_hash;
  if (typeof hash !== "string" || !hash || hash !== candidate.suite?.source_hash) throw new Error("Comparison requires matching nonempty suite.source_hash");
  const a = analyzeResult(base, target), b = analyzeResult(candidate, target);
  const items = CATEGORY_IDS.flatMap(c => a.categories[c].items.map(item => {
    const other = b.categories[c].items.find(x => x.id === item.id)!;
    const ratio = other.rate / item.rate;
    return { id: item.id, category: c, exponent: item.exponent, base: item.rate, candidate: other.rate, ratio, logContribution: item.exponent * Math.log(ratio) };
  }));
  const checksumComparison = a.checksums.map(c => {
    const other = b.checksums.find(x => x.id === c.id)!;
    const left = c.values.filter((v: any) => v !== null), right = other.values.filter((v: any) => v !== null);
    return { id: c.id, base: c.values, candidate: other.values, match: left.length && right.length ? [...left, ...right].every(v => v === left[0]) : null };
  });
  const warnings = [...a.warnings, ...b.warnings, "Matching suite hash does not establish comparable runtime configuration or CPU conditions."];
  if (checksumComparison.some(c => c.match === false)) warnings.push("Non-scheduler checksum mismatch: performance comparison may be semantically invalid.");
  if (checksumComparison.some(c => c.match === null)) warnings.push("Some non-scheduler checksum comparisons are unavailable.");
  return { base: a, candidate: b, totalRatio: b.official.total! / a.official.total!, totalLogGain: Math.log(b.official.total! / a.official.total!), summedItemLogGain: items.reduce((s, i) => s + i.logContribution, 0), items, checksumComparison, warnings: [...new Set(warnings)] };
}

export function parseArgs(args: string[]) {
  let input: string | undefined, compare: string | undefined, out: string | undefined, target = 200000;
  const seen = new Set<string>();
  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (["--compare", "--target", "--out"].includes(arg)) {
      if (seen.has(arg)) throw new Error(`Repeated option ${arg}`);
      seen.add(arg);
      const value = args[++i];
      if (!value || value.startsWith("--")) throw new Error(`Missing value for ${arg}`);
      if (arg === "--compare") compare = value;
      else if (arg === "--out") out = value;
      else target = Number(value);
    } else if (arg.startsWith("--")) throw new Error(`Unknown option ${arg}`);
    else if (input) throw new Error("Only one base JSON path is accepted");
    else input = arg;
  }
  if (!input) throw new Error("Usage: bun tools/scratchbench-target.ts BASE.json [--compare CANDIDATE.json] [--target 200000] [--out NEW.json]");
  if (!Number.isFinite(target) || target <= 0) throw new Error("Target must be finite and positive");
  if (out && [input, compare].filter(Boolean).some(p => resolve(p!) === resolve(out!))) throw new Error("Output must not replace an input");
  return { input, compare, target, out };
}

export async function main(args: string[]) {
  const options = parseArgs(args);
  const base = JSON.parse(await readFile(options.input, "utf8"));
  const report = options.compare ? compareResults(base, JSON.parse(await readFile(options.compare, "utf8")), options.target) : analyzeResult(base, options.target);
  const text = JSON.stringify(report, null, 2) + "\n";
  // Exclusive creation protects existing files, including symlinks, without a check/write race.
  if (options.out) await writeFile(options.out, text, { flag: "wx" });
  console.log(text.trimEnd());
}

if (import.meta.main) {
  main(process.argv.slice(2)).catch(error => { console.error(error.message); process.exitCode = 1; });
}
