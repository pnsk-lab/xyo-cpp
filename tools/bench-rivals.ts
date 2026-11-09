#!/usr/bin/env bun
/**
 * Compare every ScratchBench performance workload against each rival's best
 * measured rate. Use reports from compatible suite source hashes and CPUs.
 *
 * bun tools/bench-rivals.ts --ours scratch-bench/results/<dir>/performance-xyo-cpp.json \
 *   --rivals scratch-bench/results/<dir>/performance-mistwarp.json,... \
 *   --out scratch-bench/results/all-rivals-20261108
 */
import { readFileSync, mkdirSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";

interface Stat {
  ops_per_ms: { median: number; n: number };
  checksum?: unknown;
}
interface Report {
  suite: { id: string; source_hash: string };
  runtime: { id: string; name: string };
  timestamp: string;
  host: { cpu?: string; arch?: string };
  results: Record<string, Stat>;
}
interface Row {
  item: string;
  category: string;
  xyo: number;
  fastestRival: number;
  fastestRuntime: string;
  ratio: number;
  rank: "win" | "lose" | "tie";
  rivals: Record<string, number>;
}

const getArg = (key: string): string | undefined => {
  const index = process.argv.indexOf(key);
  return index < 0 ? undefined : process.argv[index + 1];
};

const readReport = (filename: string): Report => {
  const report = JSON.parse(readFileSync(filename, "utf8")) as Report;
  if (report.suite?.id !== "performance" ||
      !report.suite.source_hash || !report.results ||
      !report.runtime?.id || !report.timestamp)
    throw new Error(`invalid performance result: ${filename}`);
  return report;
};

const oursPath = getArg("--ours");
const rivalsArg = getArg("--rivals");
const outPrefix = getArg("--out");
if (!oursPath || !rivalsArg || !outPrefix) {
  console.error("usage: bun tools/bench-rivals.ts --ours FILE --rivals FILE,FILE,... --out PATH_PREFIX");
  process.exit(2);
}

const rivalPaths = rivalsArg.split(",").filter(Boolean);
const ours = readReport(oursPath);
const rivals = rivalPaths.map((path) => ({ path, report: readReport(path) }));
const expected = ["mistwarp", "turbowarp", "nitrobolt", "scratch-everywhere", "scratch"];
const actual = new Set(rivals.map(({ report }) => report.runtime.id));
if (new Set(rivals.map(({ report }) => report.runtime.id)).size !== rivals.length)
  throw new Error("duplicate rival runtime");
const missing = expected.filter((name) => !actual.has(name));
if (missing.length) console.warn(`WARNING: missing rivals: ${missing.join(", ")}`);
for (const { path, report } of rivals) {
  if (report.suite.source_hash !== ours.suite.source_hash)
    throw new Error(`suite source hash mismatch: ${path}`);
  if (report.host.cpu !== ours.host.cpu || report.host.arch !== ours.host.arch)
    throw new Error(`different CPU/architecture: ${path}`);
}

const rows: Row[] = [];
for (const [item, value] of Object.entries(ours.results)) {
  const xyo = value.ops_per_ms.median;
  const values: Record<string, number> = {};
  for (const { report } of rivals) {
    const rival = report.results[item];
    if (rival) values[report.runtime.id] = rival.ops_per_ms.median;
  }
  if (Object.keys(values).length !== rivals.length)
    throw new Error(`missing rival workload: ${item}`);
  const fastestRuntime = Object.keys(values).reduce(
    (best, next) => values[next] > values[best] ? next : best);
  const fastestRival = values[fastestRuntime];
  rows.push({
    item, category: item.split("_")[0]!, xyo,
    fastestRival, fastestRuntime, rivals: values,
    ratio: xyo / fastestRival,
    rank: xyo > fastestRival ? "win" : xyo === fastestRival ? "tie" : "lose",
  });
}
const order = rows.slice().sort((a, b) => a.ratio - b.ratio);
const counts = {
  total: rows.length,
  win: rows.filter((row) => row.rank === "win").length,
  lose: rows.filter((row) => row.rank === "lose").length,
  tie: rows.filter((row) => row.rank === "tie").length,
};
const category = [...new Set(rows.map((r) => r.category))].sort().map((cat) => {
  const sub = rows.filter((row) => row.category === cat);
  return { category: cat, count: sub.length, won: sub.filter((r) => r.rank === "win").length };
});
const data = {
  note: "Different dates and single-run estimates are indicative, not a controlled paired A/B measurement.",
  suite_hash: ours.suite.source_hash,
  host: ours.host,
  xyo: { path: oursPath, timestamp: ours.timestamp },
  rivals: rivals.map(({ path, report }) => ({ id: report.runtime.id, path, timestamp: report.timestamp })),
  counts, category, workloads: order,
};
const prefix = resolve(outPrefix);
mkdirSync(dirname(prefix), { recursive: true });
writeFileSync(prefix + ".json", JSON.stringify(data, null, 2) + "\n");

const markdown = [
  "# ScratchBench xyo-cpp vs five rival runtimes",
  "",
  `Suite hash: \`${ours.suite.source_hash}\`. CPU: ${ours.host.cpu}.`,
  "",
  `Total: ${counts.total}; wins: ${counts.win}; losses: ${counts.lose}; ties: ${counts.tie}.`,
  "",
  "Single-run results from different days. Re-run on the same host with alternated order before concluding a small gap has closed.",
  "",
  "| Category | Winning workloads | All workloads |",
  "|---|---:|---:|",
  ...category.map((c) => `| ${c.category} | ${c.won} | ${c.count} |`),
  "",
  "| Workload | xyo-cpp ops/ms | Rival best ops/ms | Best rival | xyo / best |",
  "|---|---:|---:|---|---:|",
  ...order.map((row) =>
    `| ${row.item} | ${row.xyo.toFixed(2)} | ${row.fastestRival.toFixed(2)} | ${row.fastestRuntime} | ${row.ratio.toFixed(3)}× |`),
  "",
].join("\n");
writeFileSync(prefix + ".md", markdown);
console.log(`Wrote ${prefix}.json and .md: ${counts.win}/${counts.total} wins`);
