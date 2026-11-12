import { expect, test } from "bun:test";
import {
  parseBenchRows,
  summarizeCases,
  type BenchRow,
  type RunResult,
} from "./jit-cfg-type-ab";

const makeRow = (id: string, rate: number, checksum = "42"): BenchRow => ({
  id, rate, checksum, totalMs: 600,
  operations: rate * 600, bestRate: rate,
});
const makeRun = (
  mode: "off" | "on",
  rows: BenchRow[],
): RunResult => ({
  mode, index: 0, wallMs: 1000, exit: 0, done: true,
  rows: Object.fromEntries(rows.map((row) => [row.id, row])),
});

test("parses standard ScratchBench lines, ignoring headers and DONE", () => {
  const output = [
    "id | total_ms | ops | ops_per_ms | best_ops_per_ms | checksum",
    "math_mandel | 603 | 12000 | 19.900 | 20.100 | 1957",
    "sched_threads | 401 | 123 | 0.307 | 0.5 | 12",
    "DONE",
  ].join("\n");
  const rows = parseBenchRows(output);
  expect(Object.keys(rows)).toHaveLength(2);
  expect(rows.math_mandel).toEqual({
    id: "math_mandel", totalMs: 603, operations: 12000,
    rate: 19.9, bestRate: 20.1, checksum: "1957",
  });
});

test("reports rate ratios and checksum agreement per case", () => {
  const rows = summarizeCases([
    makeRun("off", [makeRow("math_mandel", 20)]),
    makeRun("on", [makeRow("math_mandel", 30)]),
    makeRun("on", [makeRow("math_mandel", 10, "different")]),
    makeRun("off", [makeRow("math_mandel", 24)]),
  ]);
  expect(rows).toHaveLength(1);
  expect(rows[0].ratio).toBeCloseTo(20 / 22);
  expect(rows[0].checksumsEqual).toBe(false);
});

test("ignores failed or incomplete runs", () => {
  const failed = makeRun("on", [makeRow("math_mandel", 500)]);
  failed.done = false;
  expect(summarizeCases([
    makeRun("off", [makeRow("math_mandel", 20)]),
    failed,
    makeRun("on", [makeRow("math_mandel", 40)]),
  ])[0].ratio).toBe(2);
});
