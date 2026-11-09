#!/usr/bin/env bun
/**
 * Scratch corpus -> candidate superinstructions for xyo-cpp.
 *
 * sample: HTTP Range sample of the user-provided public Hugging Face CSV.
 * fetch: retrieve only publicly shared project.json files (NO assets).
 * analyze: static opcode / statement n-gram analysis, no code execution.
 *
 * All large data stays under .scratch/ (gitignored).
 */
import { createHash } from "node:crypto";
import { mkdir, readFile, readdir, writeFile } from "node:fs/promises";
import { join } from "node:path";

const CSV_URL =
  "https://huggingface.co/datasets/KangpingJP/scratch-ff/resolve/main/projects.csv";
const DEFAULT_DIR = ".scratch/quickening-corpus";

type ProjectRow = {
  username: string;
  project_id: number;
  project_title: string;
  project_url: string;
  project_public_date: string;
};
type Block = {
  opcode?: string;
  next?: string | null;
  inputs?: Record<string, unknown>;
  shadow?: boolean;
  topLevel?: boolean;
  fields?: Record<string, unknown>;
};
type Project = {
  targets?: Array<{ isStage?: boolean; blocks?: Record<string, Block> }>;
  meta?: unknown;
};
type Statistic = Record<string, number>;

function option(args: string[], key: string, fallback: string): string {
  const index = args.indexOf("--" + key);
  return index < 0 ? fallback : (args[index + 1] ?? fallback);
}
function intOption(args: string[], key: string, fallback: number): number {
  const number = Number(option(args, key, String(fallback)));
  if (!Number.isSafeInteger(number) || number < 0) {
    throw new Error("Invalid --" + key);
  }
  return number;
}
function bump(counter: Statistic, key: string): void {
  counter[key] = (counter[key] ?? 0) + 1;
}
function top(counter: Statistic, max = 30): Array<[string, number]> {
  return Object.entries(counter)
    .sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]))
    .slice(0, max);
}
function hashText(value: string): string {
  return createHash("sha256").update(value).digest("hex");
}
function hashRank(id: number, seed: string): string {
  return hashText(seed + ":" + id);
}
function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/** Parse RFC4180 records, including quoted commas, quotes and newlines. */
export function parseCsv(text: string): string[][] {
  const result: string[][] = [];
  let row: string[] = [];
  let field = "";
  let quoted = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i]!;
    if (c === '"') {
      if (quoted && text[i + 1] === '"') {
        field += '"';
        i++;
      } else {
        quoted = !quoted;
      }
    } else if (c === "," && !quoted) {
      row.push(field);
      field = "";
    } else if ((c === "\r" || c === "\n") && !quoted) {
      if (c === "\r" && text[i + 1] === "\n") i++;
      row.push(field);
      if (row.some((v) => v !== "")) result.push(row);
      row = [];
      field = "";
    } else {
      field += c;
    }
  }
  // Discard unterminated final records, since range reads end mid-row.
  return result;
}

function toProjectRow(fields: string[]): ProjectRow | undefined {
  if (fields.length !== 5) return;
  const id = Number(fields[1]);
  const date = fields[4] ?? "";
  if (!Number.isSafeInteger(id) || id <= 0 ||
      !/^\d{4}-\d\d-\d\dT/.test(date) ||
      fields[3] !== "https://scratch.mit.edu/projects/" + id + "/") return;
  return {
    username: fields[0]!, project_id: id, project_title: fields[2]!,
    project_url: fields[3]!, project_public_date: date,
  };
}

function shuffleByYear(
  rows: ProjectRow[], count: number, seed: string, minYear: number,
): ProjectRow[] {
  const years = new Map<number, ProjectRow[]>();
  const unique = new Set<number>();
  for (const row of rows) {
    const year = Number(row.project_public_date.slice(0, 4));
    if (year < minYear || unique.has(row.project_id)) continue;
    unique.add(row.project_id);
    const bucket = years.get(year) ?? [];
    bucket.push(row);
    years.set(year, bucket);
  }
  const values = [...years.entries()].sort((a, b) => a[0] - b[0]);
  for (const [, bucket] of values) {
    bucket.sort((a, b) =>
      hashRank(a.project_id, seed).localeCompare(hashRank(b.project_id, seed)));
  }
  const selected: ProjectRow[] = [];
  for (let n = 0; selected.length < count; n++) {
    let hadAny = false;
    for (const [, bucket] of values) {
      if (n >= bucket.length) continue;
      hadAny = true;
      selected.push(bucket[n]!);
      if (selected.length === count) break;
    }
    if (!hadAny) break;
  }
  return selected;
}

async function rangeRead(start: number, end: number): Promise<Response> {
  const response = await fetch(CSV_URL, {
    headers: { Range: "bytes=" + start + "-" + end },
    signal: AbortSignal.timeout(30000),
  });
  if (response.status !== 206) {
    throw new Error("HTTP Range unsupported, status=" + response.status);
  }
  return response;
}
function lcg(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 4294967296;
  };
}

async function sample(args: string[]): Promise<void> {
  const outDir = option(args, "dir", DEFAULT_DIR);
  const windows = intOption(args, "windows", 96);
  const bytes = intOption(args, "window-bytes", 65536);
  const count = intOption(args, "count", 140);
  const minYear = intOption(args, "min-year", 2020);
  const seed = intOption(args, "seed", 1618);
  if (!windows || bytes < 2048 || windows > 1024 || count > 50000)
    throw new Error("Invalid sampling size");
  const head = await rangeRead(0, Math.min(4095, bytes - 1));
  const fullSize = Number(head.headers.get("content-range")?.split("/")[1]);
  if (!Number.isSafeInteger(fullSize) || fullSize < 1024) {
    throw new Error("Missing CSV size from Content-Range");
  }
  await head.arrayBuffer();
  const random = lcg(seed);
  const rows: ProjectRow[] = [];
  const sampled: Array<{ start: number; count: number }> = [];
  for (let i = 0; i < windows; i++) {
    const center = Math.min(0.9999,
      Math.max(0, (i + 0.2 + 0.6 * random()) / windows));
    const start = Math.floor(center * Math.max(0, fullSize - bytes));
    const end = Math.min(fullSize - 1, start + bytes - 1);
    const response = await rangeRead(start, end);
    const text = new TextDecoder().decode(await response.arrayBuffer());
    const trimmed = start === 0 ? text : text.slice(text.indexOf("\n") + 1);
    const parsed = parseCsv(trimmed).map(toProjectRow).filter(
      (r): r is ProjectRow => r !== undefined);
    rows.push(...parsed);
    sampled.push({ start, count: parsed.length });
    if ((i + 1) % 16 === 0) {
      console.error("range windows:", i + 1, "/", windows,
        "candidate rows:", rows.length);
    }
  }
  const chosen = shuffleByYear(rows, count, String(seed), minYear);
  await mkdir(outDir, { recursive: true });
  await writeFile(join(outDir, "selected.jsonl"),
    chosen.map((r) => JSON.stringify(r)).join("\n") + "\n");
  const byYear: Statistic = {};
  for (const r of chosen) bump(byYear, r.project_public_date.slice(0, 4));
  const report = {
    source: CSV_URL, sourceBytes: fullSize, windows, windowBytes: bytes,
    observedRows: rows.length, estimatedTotalRows: Math.round(
      rows.length / sampled.reduce((v, x) => v + (Math.min(fullSize, x.start + bytes) - x.start), 0) * fullSize),
    selected: chosen.length, minYear, seed,
    byYear: Object.fromEntries(top(byYear, 99)), sampledRanges: sampled,
    note: "HTTP byte-range sample, not an exhaustive or strictly row-uniform sample",
  };
  await writeFile(join(outDir, "sample_report.json"), JSON.stringify(report, null, 2) + "\n");
  console.log(JSON.stringify({ dir: outDir, ...report, sampledRanges: undefined }, null, 2));
}

async function requestJson(url: string, maxBytes: number): Promise<unknown> {
  for (let attempt = 0; attempt < 3; attempt++) {
    const response = await fetch(url, { signal: AbortSignal.timeout(20000) });
    if (response.status === 429 || response.status >= 500) {
      if (attempt < 2) {
        await sleep(2000 * (attempt + 1));
        continue;
      }
    }
    if (!response.ok) throw new Error("HTTP " + response.status);
    const length = Number(response.headers.get("content-length") ?? 0);
    if (length > maxBytes) throw new Error("project too large (" + length + ")");
    const data = await response.arrayBuffer();
    if (data.byteLength > maxBytes) throw new Error("project exceeds limit");
    return JSON.parse(new TextDecoder().decode(data));
  }
  throw new Error("HTTP retry limit");
}

async function fetchProjects(args: string[]): Promise<void> {
  const dir = option(args, "dir", DEFAULT_DIR);
  const limit = intOption(args, "limit", 30);
  const delay = intOption(args, "delay-ms", 500);
  const maxBytes = intOption(args, "max-project-bytes", 8_000_000);
  if (!limit || limit > 50000 || delay < 100) throw new Error("invalid fetch configuration");
  const lines = (await readFile(join(dir, "selected.jsonl"), "utf8"))
    .split("\n").filter(Boolean);
  const items = lines.map((l) => JSON.parse(l) as ProjectRow);
  const projectDir = join(dir, "projects");
  await mkdir(projectDir, { recursive: true });
  const report: Array<{ id: number; status: string; bytes?: number }> = [];
  for (const row of items.slice(0, limit)) {
    const file = join(projectDir, row.project_id + ".json");
    if (await Bun.file(file).exists()) {
      report.push({ id: row.project_id, status: "cached" });
      continue;
    }
    try {
      const meta = await requestJson(
        "https://api.scratch.mit.edu/projects/" + row.project_id, 250000);
      const m = meta as Record<string, unknown>;
      if (m.public !== true || m.is_published !== true ||
          m.visibility !== "visible" || typeof m.project_token !== "string") {
        report.push({ id: row.project_id, status: "not-public" });
        continue;
      }
      const project = await requestJson(
        "https://projects.scratch.mit.edu/" + row.project_id +
        "?token=" + encodeURIComponent(m.project_token), maxBytes);
      const p = project as Project;
      if (!Array.isArray(p.targets)) {
        report.push({ id: row.project_id, status: "non-scratch3" });
        continue;
      }
      const serialized = JSON.stringify(project);
      await writeFile(file, serialized);
      report.push({ id: row.project_id, status: "saved", bytes: Buffer.byteLength(serialized) });
    } catch (error) {
      report.push({ id: row.project_id, status: String(error).slice(0, 100) });
    }
    if (report.length % 10 === 0) {
      console.error("fetched:", report.length, "/", Math.min(limit, items.length));
    }
    await sleep(delay);
  }
  await writeFile(join(dir, "fetch_report.json"),
    JSON.stringify({ source: "Scratch public project API", entries: report }, null, 2) + "\n");
  console.log(JSON.stringify({
    attempted: report.length,
    saved: report.filter((x) => x.status === "saved").length,
    cached: report.filter((x) => x.status === "cached").length,
    failures: report.filter((x) => !["saved", "cached"].includes(x.status)),
  }, null, 2));
}

function isBlock(x: unknown): x is Block {
  return x !== null && typeof x === "object" && !Array.isArray(x);
}
function inputId(value: unknown): string | undefined {
  if (!Array.isArray(value)) return;
  if (typeof value[1] === "string" && value[1]) return value[1];
  if (typeof value[2] === "string" && value[2]) return value[2];
}

/** Only linear next-block sequences are counted as statement n-grams. */
export function analyzeProject(project: Project): {
  blocks: number;
  opcodes: Statistic;
  pairs: Statistic;
  ngrams: Statistic;
  inputEdges: Statistic;
  fingerprint: string;
} {
  const opcodes: Statistic = {}, pairs: Statistic = {}, ngrams: Statistic = {},
    inputEdges: Statistic = {};
  let blocks = 0;
  const signatures: string[] = [];
  for (const target of project.targets ?? []) {
    const map = target.blocks ?? {};
    const seen = new Set<string>();
    const linear = (id: string): string[] => {
      const ops: string[] = [], loopGuard = new Set<string>();
      for (let cur: string | null = id; cur && !loopGuard.has(cur); ) {
        loopGuard.add(cur);
        const block: Block | undefined = map[cur];
        if (!isBlock(block) || !block.opcode) break;
        if (!block.shadow) ops.push(block.opcode);
        cur = block.next ?? null;
      }
      return ops;
    };
    const visit = (id: string): void => {
      if (!id || seen.has(id)) return;
      seen.add(id);
      const b = map[id];
      if (!isBlock(b) || !b.opcode) return;
      if (!b.shadow) {
        blocks++;
        bump(opcodes, b.opcode);
      }
      for (const [inputName, value] of Object.entries(b.inputs ?? {})) {
        const nested = inputId(value);
        if (nested && map[nested]) {
          // SUBSTACK is a control-flow branch, not a reporter operand.
          if (!b.shadow && map[nested]?.opcode && !inputName.startsWith("SUBSTACK")) {
            bump(inputEdges, b.opcode + " <- " + map[nested]!.opcode);
          }
          visit(nested);
        }
      }
      if (b.next) visit(b.next);
    };
    const roots = Object.entries(map).filter(([, b]) =>
      isBlock(b) && b.topLevel).map(([id]) => id);
    // In broken/legacy exports topLevel might be absent; inspect all remaining blocks.
    for (const id of roots) {
      const seq = linear(id);
      visit(id);
    }
    for (const id of Object.keys(map)) visit(id);
    // Conservative structural dedup: ignore random block IDs and literal contents.
    for (const b of Object.values(map)) {
      if (!isBlock(b) || b.shadow || !b.opcode) continue;
      const inputs = Object.entries(b.inputs ?? {}).sort(([a], [c]) => a.localeCompare(c))
        .map(([name, value]) => {
          const nested = inputId(value);
          return name + ":" + (nested && map[nested]?.opcode
            ? map[nested]!.opcode : "literal");
        }).join(",");
      const next = b.next ? (map[b.next]?.opcode ?? "missing") : "";
      signatures.push(b.opcode + "|" + (b.topLevel ? "root" : "") +
        "|" + next + "|" + inputs);
    }
    // Count only edges whose endpoints are real statement blocks.
    for (const b of Object.values(map)) {
      if (!isBlock(b) || b.shadow || !b.opcode || !b.next) continue;
      const successor = map[b.next];
      if (isBlock(successor) && !successor.shadow && successor.opcode) {
        bump(pairs, b.opcode + " > " + successor.opcode);
      }
    }
    const considered = new Set<string>();
    for (const [id, b] of Object.entries(map)) {
      if (!isBlock(b) || b.shadow || !b.opcode) continue;
      if (considered.has(id)) continue;
      // Enumerate all 3..5-grams, including those beginning mid-chain.
      const seq = linear(id);
      for (let length = 3; length <= 5; length++) {
        if (seq.length >= length) {
          bump(ngrams, seq.slice(0, length).join(" > "));
        }
      }
      considered.add(id);
    }
  }
  return {
    blocks, opcodes, pairs, ngrams, inputEdges,
    fingerprint: hashText(signatures.sort().join("\n")),
  };
}

async function analyze(args: string[]): Promise<void> {
  const dir = option(args, "dir", DEFAULT_DIR);
  const projectDir = join(dir, "projects");
  const files = (await readdir(projectDir)).filter((p) => p.endsWith(".json")).sort();
  const merged = { opcodes: {} as Statistic, pairs: {} as Statistic,
    ngrams: {} as Statistic, inputEdges: {} as Statistic };
  const seen = new Set<string>();
  const presence = { opcodes: {} as Statistic, pairs: {} as Statistic,
    ngrams: {} as Statistic, inputEdges: {} as Statistic };
  const capped = { opcodes: {} as Statistic, pairs: {} as Statistic,
    ngrams: {} as Statistic, inputEdges: {} as Statistic };
  const biggest: Array<{file: string; blocks: number}> = [];
  let blocks = 0, used = 0, duplicate = 0, errors = 0;
  for (const file of files) {
    try {
      const project = JSON.parse(await readFile(join(projectDir, file), "utf8")) as Project;
      const result = analyzeProject(project);
      if (seen.has(result.fingerprint)) { duplicate++; continue; }
      seen.add(result.fingerprint);
      used++;
      blocks += result.blocks;
      biggest.push({ file, blocks: result.blocks });
      for (const key of ["opcodes", "pairs", "ngrams", "inputEdges"] as const) {
        for (const [name, number] of Object.entries(result[key])) {
          merged[key][name] = (merged[key][name] ?? 0) + number;
          presence[key][name] = (presence[key][name] ?? 0) + 1;
          capped[key][name] = (capped[key][name] ?? 0) + Math.min(number, 3);
        }
      }
    } catch (e) { errors++; console.error("invalid", file, String(e)); }
  }
  const report = {
    scannedFiles: files.length, structurallyUniqueProjects: used,
    duplicate, errors, blocks,
    warnings: [
      "These are static block occurrences, NOT dynamic execution frequencies.",
      "Statement ngrams follow next pointers only; no cross-branch / loop fusion assumed.",
      "Project JSON only; assets are not downloaded, and scripts are not executed.",
    ],
    topOpcodes: top(merged.opcodes, 50),
    topStatementPairs: top(merged.pairs, 50),
    topStatementNgrams: top(merged.ngrams, 50),
    topInputEdges: top(merged.inputEdges, 50),
    byProjectCoverage: {
      opcodes: top(presence.opcodes, 50),
      pairs: top(presence.pairs, 50),
      ngrams: top(presence.ngrams, 50),
      inputEdges: top(presence.inputEdges, 50),
    },
    cappedPerProject: {
      pairs: top(capped.pairs, 50), inputEdges: top(capped.inputEdges, 50),
    },
    biggest: biggest.sort((a,b) => b.blocks - a.blocks).slice(0, 15),
    counts: merged,
  };
  await writeFile(join(dir, "analysis.json"), JSON.stringify(report, null, 2) + "\n");
  console.log(JSON.stringify({
    scannedFiles: report.scannedFiles, structurallyUniqueProjects: used, duplicate,
    errors, blocks, topOpcodes: report.topOpcodes.slice(0, 20),
    topStatementPairs: report.topStatementPairs.slice(0, 20),
    topInputEdges: report.topInputEdges.slice(0, 20),
    topPairsByProjectCoverage: report.byProjectCoverage.pairs.slice(0, 20),
    topInputEdgesByProjectCoverage: report.byProjectCoverage.inputEdges.slice(0, 20),
    biggest: report.biggest.slice(0, 5),
  }, null, 2));
}

if (import.meta.main) {
  const [command, ...args] = Bun.argv.slice(2);
  try {
    if (command === "sample") await sample(args);
    else if (command === "fetch") await fetchProjects(args);
    else if (command === "analyze") await analyze(args);
    else {
      console.log("Usage: bun tools/quickening_corpus.ts <sample|fetch|analyze> [options]");
      process.exitCode = 2;
    }
  } catch (error) {
    console.error(error);
    process.exitCode = 1;
  }
}
