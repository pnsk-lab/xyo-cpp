#!/usr/bin/env python3
"""Differentially validate affine modulo and numeric SSA facts at edge values.

Run a small Scratch project per input through interpreter, baseline JIT,
optimized HotSpot and optimized eager JIT. Preserve NaN, Infinity, signed-zero,
numeric strings and the i32/i53 integer boundaries without changing the
Scratch runtime or the benchmark's source.
"""
from __future__ import annotations
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))
from gen_algo_bench import (Gen, S_call, S_change, S_repeat, S_set,
                            add, arg, make_project, mod, mul, num, text, var,
                            write_sb3)

CASES = [
    (num(0), num(7)), (text("-0"), num(7)),
    (num(1.25), num(7)), (num(-1.25), num(7)),
    (num(2147483647), num(7)), (num(2147483648), num(7)),
    (num(2**52), num(7)), (num(-(2**52)), num(7)),
    (num(1e308), num(7)), (num(-1e308), num(-7)),
    (text("Infinity"), num(7)), (text("-Infinity"), num(-7)),
    (text("NaN"), num(7)), (text("not a number"), num(7)),
    (text("   "), num(7)), (text("3.25"), num(0)),
    (num(7), num(-7)), (num(-7), num(-7)),
    (num(3.5), num(1e-300)), (num(-3.5), num(1e308)),
]
MODES = [
    ("interpreter", "off", "0", "0"),
    ("baseline-jit", "hotspot", "0", "0"),
    ("optimized-jit", "hotspot", "1", "1"),
    ("optimized-eager", "eager", "1", "1"),
]

def project(lhs, rhs):
    gen = Gen(["x", "sum", "check"], [])
    # Inner expression is intentionally nested and contains no benchmark
    # dispatch. It is the same Scratch expression in all execution modes.
    delta = add(mul(mod(var("x"), rhs), num(31)), num(1))
    gen.define("accumulate %s", ["n"], [
        S_set("sum", num(0)),
        S_repeat(arg("n"), [S_change("sum", delta)]),
    ], warp=True)
    gen.script([
        S_set("x", lhs),
        S_call("accumulate %s", [num(193)]),
        S_set("check", var("sum")),
    ])
    return make_project(gen.vars.keys(), gen.lists.keys(), gen)

def evaluate(binary, sb3, mode, finite, ranges):
    env = {k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update(XYO_DUMPVARS="json", XYO_JIT_FINITE_ARITH_DELTA=finite,
               XYO_JIT_ARITH_RANGE_FACTS=ranges)
    result = subprocess.run(
        [str(binary), "run", str(sb3), "--jit", mode], cwd=ROOT,
        env=env, capture_output=True, text=True, timeout=30,
    )
    if result.returncode:
        raise AssertionError(
            f"{mode}/{finite}/{ranges}: exited {result.returncode}: "
            f"{result.stderr[-500:]}"
        )
    start = result.stdout.index("{")
    end = result.stdout.rindex("}")
    bench = json.loads(result.stdout[start:end+1])["Bench"]
    return {key: bench[key] for key in ["x","sum","check"]}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/xyo")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build/v1-jit-arith-edge-results.json")
    args = parser.parse_args()
    binary = args.binary.resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    rows=[]
    with tempfile.TemporaryDirectory(prefix="xyo-jit-arith-", dir=output.parent) as tmp:
        path = Path(tmp) / "case.sb3"
        for i,(lhs,rhs) in enumerate(CASES):
            write_sb3(str(path), project(lhs,rhs))
            samples={}
            for variant, mode, finite, ranges in MODES:
                samples[variant]=evaluate(binary,path,mode,finite,ranges)
            expected=samples["interpreter"]
            if any(sample!=expected for sample in samples.values()):
                raise AssertionError(f"case {i}: {lhs}, {rhs}, {samples}")
            rows.append({"case":i,"lhs":lhs,"rhs":rhs,"result":expected})
            print(f"passed {i+1}/{len(CASES)}", flush=True)
    output.write_text(json.dumps(rows,ensure_ascii=False,indent=2))
    print(f"SUCCESS: {len(CASES)} edge cases x 4 execution modes")

if __name__=="__main__":
    main()
