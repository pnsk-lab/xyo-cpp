/**
 * scratch-bench parity gate regression tests, using standalone fixture output.
 * No downloaded benchmark suite or native executable is required.
 */
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

type Parity = {
  passed: number;
  total: number;
  verdict: "passed" | "failed";
};

const script = path.resolve(import.meta.dir, "scratch-bench-summary.ts");
const variants = ["hotspot", "interpreter", "eager", "aot"] as const;
const scratchDir = fs.mkdtempSync(path.join(os.tmpdir(), "xyo-scratch-parity-gate-"));

const writeJson = (directory: string, name: string, result: unknown): void => {
  const dest = path.join(scratchDir, "scratch-bench", "results", directory);
  fs.mkdirSync(dest, { recursive: true });
  fs.writeFileSync(path.join(dest, name + ".json"), JSON.stringify(result));
};

const result = (suite: string, variant: string, values: unknown) => ({
  suite: { id: suite },
  runtime: {
    id: "xyo-cpp",
    name: "xyo-cpp",
    variant: { id: variant, name: variant },
  },
  runs: Array.from({ length: suite === "performance" ? 3 : 1 }, () => ({ ok: true })),
  stats: { run_ms: { median: 10 } },
  results: values,
});

const runCase = (
  title: string,
  overrides: Partial<Record<(typeof variants)[number], Partial<Parity>>>,
  expectedExit: number,
  expectedMessage: string,
): void => {
  for (const variant of variants) {
    const parity = { verdict: "passed", passed: 2123, total: 2123, ...overrides[variant] };
    writeJson("ci-parity", variant, result("parity", variant, parity));
    writeJson("ci-performance", variant, result("performance", variant, {
      representative: { ops_per_ms: { median: 1 } },
    }));
  }

  const env = { ...process.env };
  delete env.GITHUB_STEP_SUMMARY;
  const child = Bun.spawnSync(["bun", script], {
    cwd: scratchDir,
    env,
    stdout: "pipe",
    stderr: "pipe",
  });
  const output = child.stdout.toString() + child.stderr.toString();
  assert.equal(child.exitCode, expectedExit, `${title}: unexpected exit code\n${output}`);
  assert.ok(output.includes(expectedMessage), `${title}: expected ${expectedMessage}\n${output}`);
  console.log(`PASS ${title}`);
};

try {
  runCase("all four variants 2123/2123", {}, 0, "Parity gate: 100%");
  runCase("larger suite 2124/2124", { hotspot: { passed: 2124, total: 2124 } },
    0, "2124/2124 (100.00%)");
  runCase("single mismatch 2122/2123",
    { hotspot: { passed: 2122 } },
    1, "Parity must be 100%: hotspot 2122/2123");
  runCase("truncated suite 2122/2122",
    { eager: { passed: 2122, total: 2122 } },
    1, "Parity suite too small: eager 2122 checks");
  runCase("failed verdict even though full count",
    { aot: { verdict: "failed" } },
    1, "Parity must be 100%: aot 2123/2123");
  runCase("missing count",
    { interpreter: { passed: -1 } },
    1, "Invalid parity result");
} finally {
  fs.rmSync(scratchDir, { recursive: true, force: true });
}
