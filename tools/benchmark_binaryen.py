#!/usr/bin/env python3
"""Measure xyo's standalone WASM emission and Binaryen overhead on identical SB3s.

This is a microbenchmark, not a full ScratchBench score. The same module is
instantiated and executed repeatedly on Node.js, independent of the xyo GUI.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests" / "bench" / "algos"))
from gen_algo_bench import Gen, S_set, S_repeat, add, num, var, make_project, write_sb3  # noqa: E402

NODE_RUNNER = r"""
const fs = require('node:fs');
const {performance} = require('node:perf_hooks');
const data = fs.readFileSync(process.argv[2]);
const runs = Number(process.argv[3]);
const iterations = Number(process.argv[4]);
const t0 = performance.now();
const wasmModule = new WebAssembly.Module(data);
const compileMs = performance.now() - t0;
const memory = new WebAssembly.Memory({initial: 256});
const t1 = performance.now();
const instance = new WebAssembly.Instance(wasmModule, {env: {memory}});
const instantiateMs = performance.now() - t1;
const f = instance.exports.f;
const slots = new Float64Array(memory.buffer, 64, 2);
// Warm the JS wrapper and WASM function before timing.
for (let i = 0; i < 1000; i++) { slots[0] = 0; if (f(64) !== 0) process.exit(2); }
if (slots[0] !== 1000 || slots[1] !== 1000) throw Error('incorrect WASM result after warm-up');
const samples = [];
for (let j = 0; j < runs; j++) {
  slots[0] = 0;
  const start = performance.now();
  for (let i = 0; i < iterations; i++) {
    if (f(64) !== 0) process.exit(3);
  }
  const elapsed = performance.now() - start;
  if (slots[0] !== 1000 || slots[1] !== 1000) throw Error('incorrect WASM result');
  samples.push(elapsed * 1e6 / iterations);
}
console.log(JSON.stringify({compile_ms: compileMs, instantiate_ms: instantiateMs, ns_per_call: samples}));
"""

def median(values: list[float]) -> float:
    return statistics.median(values)

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("binary", type=Path)
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--iterations", type=int, default=10000)
    ap.add_argument("--level", type=int, default=2)
    ap.add_argument("--output", type=Path)
    args = ap.parse_args()
    if args.runs < 1 or args.iterations < 1 or args.level not in range(4):
        ap.error("runs and iterations must be positive, level must be 0..3")
    node = shutil.which("node")
    if not node:
        ap.error("node must be installed")
    binary = args.binary.resolve()
    results = []
    with tempfile.TemporaryDirectory(prefix="xyo-binaryen-bench-") as tmp:
        directory = Path(tmp)
        gen = Gen(["x", "y"], [])
        top_id = gen.script([
            S_set("x", num(0)),
            S_repeat(num(1000), [S_set("x", add(var("x"), num(1)))]),
            S_set("y", var("x")),
        ])
        project = directory / "project.sb3"
        write_sb3(str(project), make_project(["x", "y"], [], gen))
        runner = directory / "run.cjs"
        runner.write_text(NODE_RUNNER)
        for name, backend, engine in (
            ("builtin", "builtin", None),
            ("binaryen-library", "binaryen", "library"),
            ("binaryen-external", "binaryen", "external"),
        ):
            output = directory / (name + ".wasm")
            command = [str(binary), "wasm", str(project), "--script", top_id,
                       "--backend", backend, "--opt", str(args.level), "-o", str(output)]
            env = os.environ.copy()
            if engine:
                env["XYO_BINARYEN_ENGINE"] = engine
            measurements = []
            for _ in range(args.runs):
                start = time.perf_counter_ns()
                process = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
                elapsed = (time.perf_counter_ns() - start) / 1e6
                if process.returncode:
                    if not measurements:
                        print(f"SKIP {name}: {process.stderr.strip()}", file=sys.stderr)
                        break
                    raise RuntimeError(f"{name} failed: {process.stderr}")
                measurements.append(elapsed)
            if not measurements:
                continue
            executions = []
            for _ in range(args.runs):
                output_process = subprocess.run(
                    [node, str(runner), str(output), str(args.runs), str(args.iterations)],
                    capture_output=True, text=True, timeout=90)
                if output_process.returncode:
                    raise RuntimeError(output_process.stderr or output_process.stdout)
                executions.append(json.loads(output_process.stdout))
            result = {
                "backend": name,
                "bytes": output.stat().st_size,
                "end_to_end_compile_ms_median": median(measurements),
                "node_module_compile_ms_median": median([x["compile_ms"] for x in executions]),
                "node_instantiate_ms_median": median([x["instantiate_ms"] for x in executions]),
                "execute_ns_per_call_median": median(
                    [median(x["ns_per_call"]) for x in executions]),
            }
            results.append(result)
            print(json.dumps(result, ensure_ascii=False))
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(results, indent=2, ensure_ascii=False) + "\n")
    return 0 if results else 1

if __name__ == "__main__":
    raise SystemExit(main())
