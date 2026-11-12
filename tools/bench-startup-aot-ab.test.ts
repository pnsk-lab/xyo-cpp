import { afterEach, expect, test } from "bun:test";
import * as fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {
  measure,
  median,
  parseArgs,
  summarize,
  type Settings,
} from "./bench-startup-aot-ab.ts";

const directories: string[] = [];

afterEach(() => {
  for (const directory of directories.splice(0)) {
    fs.rmSync(directory, { force: true, recursive: true });
  }
});

function args(): string[] {
  return [
    "--bin", "./build/xyo", "--project", "project.sb3",
    "--aot", "project.o", "--env", "XYO_SB3_DIRECT_BUILD",
    "--a", "0", "--b", "1",
  ];
}

function fixture(source: string): Settings {
  const cacheBase = path.join(os.homedir(), ".cache");
  fs.mkdirSync(cacheBase, { recursive: true });
  const root = fs.mkdtempSync(path.join(cacheBase, "xyo-startup-ab-"));
  directories.push(root);
  const binary = path.join(root, "fake-xyo");
  const project = path.join(root, "project.sb3");
  const object = path.join(root, "project.o");
  fs.writeFileSync(binary, source, { mode: 0o700 });
  fs.writeFileSync(project, "dummy project");
  fs.writeFileSync(object, "dummy AOT object");
  return {
    binary,
    project,
    object,
    environmentVariable: "XYO_SB3_DIRECT_BUILD",
    a: "0",
    b: "1",
    cpu: null,
    pairs: 1,
    timeoutMs: 1500,
    firstLinePrefix: "Scratch Parity Test:",
    extraArgs: [],
  };
}

test("A/B settings validate CPU and pair count", () => {
  const settings = parseArgs([
    ...args(), "--cpu", "10", "--pairs", "20",
    "--prefix", "Scratch Parity Test:",
  ]);
  expect(settings.cpu).toBe(10);
  expect(settings.pairs).toBe(20);
  expect(settings.a).toBe("0");
  expect(settings.b).toBe("1");
  expect(settings.firstLinePrefix).toBe("Scratch Parity Test:");
});

test("invalid settings are rejected", () => {
  expect(() => parseArgs([...args(), "--cpu", "-1"])).toThrow();
  expect(() => parseArgs([...args(), "--pairs", "0"])).toThrow();
  expect(() => parseArgs([...args(), "--pairs", "2.5"])).toThrow();
  expect(() => parseArgs([...args(), "--unexpected", "yes"])).toThrow();
  expect(() => parseArgs(args().slice(0, -1))).toThrow();
  expect(() => parseArgs([
    ...args(), "--env", "danger-var",
  ])).toThrow();
  expect(() => parseArgs([...args(), "--", "--aot", "different.o"])).toThrow();
  expect(() => parseArgs([...args(), "--", "--aot=different.o"])).toThrow();
});

test("median and paired geometric-mean speedup", () => {
  expect(median([5, 1, 9, 3])).toBe(4);
  expect(median([3, 1, 2])).toBe(2);
  const stats = summarize([200, 220, 240, 260], [100, 110, 120, 130]);
  expect(stats.aMedianMs).toBe(230);
  expect(stats.bMedianMs).toBe(115);
  expect(stats.pairedSavingsMedianMs).toBe(115);
  expect(stats.speedupGeometricMean).toBeCloseTo(2);
  expect(stats.bWins).toBe(4);
  expect(() => summarize([100], [])).toThrow();
  expect(() => summarize([-1], [1])).toThrow();
});

test("requires an actual native AOT load and valid first output", async () => {
  const settings = fixture([
    "#!/bin/sh",
    "echo 'aot: 479 scripts precompiled (8423 slots)' >&2",
    "echo 'Scratch Parity Test: generated'",
    "sleep 3",
  ].join("\n") + "\n");
  const time = await measure(settings, settings.a);
  expect(time).toBeGreaterThan(0);
  expect(time).toBeLessThan(1000);
});

test("rejects JIT fallback despite first output", async () => {
  const settings = fixture([
    "#!/bin/sh",
    "echo 'aot: load failed (bad object)' >&2",
    "echo 'Scratch Parity Test: generated'",
  ].join("\n") + "\n");
  await expect(measure(settings, settings.a)).rejects.toThrow(/Native AOT/);
});

test("rejects unrelated first output", async () => {
  const settings = fixture([
    "#!/bin/sh",
    "echo 'aot: 5 scripts precompiled (10 slots)' >&2",
    "echo 'unrelated line'",
  ].join("\n") + "\n");
  await expect(measure(settings, settings.a)).rejects.toThrow(/Unexpected first output/);
});

test("times out if the project never produces a first line", async () => {
  const settings = fixture("#!/bin/sh\nsleep 3\n");
  settings.timeoutMs = 200;
  await expect(measure(settings, settings.a)).rejects.toThrow(/Timed out/);
});
