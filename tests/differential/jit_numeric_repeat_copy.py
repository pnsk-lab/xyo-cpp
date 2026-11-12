#!/usr/bin/env python3
"""Parity and actual-lowering checks for default-on numeric-repeat scalar copies.

Usage: python3 tests/differential/jit_numeric_repeat_copy.py --binary PATH --out NEW_DIR
Covers exact Value tags (via listcontents), signed zero (reciprocal), copy
ordering, nonnumeric guard fallback, zero trips, 64-tick boundaries and nonwarp.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_add, S_call, S_repeat, S_set, add, div, make_project,
    mod, mul, num, op, sub, text, var, write_sb3,
)
from bench_structure_cliffs import project as structure_project


def fixture(count, source, warp, ordering, path="integer"):
    g = Gen(["src", "a", "b", "x"], ["probe", "checks"])
    update = S_set("x", mod(add(mul(var("x"), num(17)), num(23)), num(65521)))
    if path in ("float", "float_bail"):
        operand = add(var("x"), num(1)) if path == "float" else sub(var("x"), num(100))
        update = S_set("x", op("operator_mathop", {"OPERATOR": "sqrt"}, NUM=operand))
    body = [S_set("a", var("src")), S_set("b", var("a")), update]
    if ordering:
        # Read-before-write and read-after-write must not use stale carriers.
        body = [S_set("a", var("x")), update, S_set("b", var("x")),
                S_set("src", var("a"))]
    g.define("kernel", [], [S_repeat(num(count), body)], warp=warp)
    g.script([
        S_set("src", source),
        S_set("a", text("A") if path == "integer" else add(num(0), num(0))),
        S_set("b", text("B") if path == "integer" else add(num(0), num(0))),
        S_set("x", add(num(0), num(1))), S_call("kernel"),
        S_add("probe", var("b")), S_add("probe", text("Z")),
        S_add("checks", var("x")), S_add("checks", var("src")),
        S_add("checks", var("a")), S_add("checks", var("b")),
        S_add("checks", div(num(1), var("b"))),
        S_add("checks", op("data_listcontents", {"LIST": "probe"})),
    ])
    return make_project(g.vars, g.lists, g)


def run(binary, sb3, mode, copy, out, label, dump=False, overrides=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_GPU": "0", "XYO_PAR": "0",
                "XYO_PARFOR": "0"})
    if copy is not None:
        env["XYO_JIT_NUMERIC_REPEAT_COPY"] = str(copy)
    env.update(overrides or {})
    if dump:
        env["XYO_JIT_DUMP"] = "1"
        env["XYO_JIT_DUMP2"] = "1"
    p = subprocess.run([str(binary), "run", str(sb3), "--jit", mode,
                        "--frames", "100000"], cwd=ROOT, env=env,
                       text=True, capture_output=True, timeout=60)
    (out / f"{label}.stdout").write_text(p.stdout)
    (out / f"{label}.stderr").write_text(p.stderr)
    if p.returncode:
        raise RuntimeError(f"{label}: exit {p.returncode}")
    data = json.loads(p.stdout[p.stdout.index("{"):p.stdout.rindex("}") + 1])
    return data["Bench"]["checks"], p.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    binary, out = args.binary.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources = [add(num(0), num(7)), div(num(1), num(2)), mul(num(-1), num(0)),
               text("7"), text("0017"), text("abc"), text(""),
               text("NaN"), text("Infinity"), add(num(0), num(2**53))]
    rows = []
    for warp in (True, False):
        for ordering in (False, True):
            for source_index, source in enumerate(sources):
                for count in (0, 1, 63, 64, 65, 129):
                    label = f"w{int(warp)}-o{int(ordering)}-s{source_index}-n{count}"
                    sb3 = out / f"{label}.sb3"
                    write_sb3(sb3, fixture(count, source, warp, ordering))
                    expected, _ = run(binary, sb3, "off", 0, out, label + "-interp")
                    for enabled in (None, 0, 1):
                        actual, _ = run(binary, sb3, "eager", enabled, out,
                                        label + f"-jit{enabled}")
                        if actual != expected:
                            raise RuntimeError(f"{label} copy={enabled}: {actual} != {expected}")
                    rows.append({"case": label, "expected": expected})
    # Explicitly exercise boxed/f64 copies, not merely their integer guard
    # fallbacks. Numeric a/b initial states allow immediate fast-path entry.
    special_sources = sources + [div(num(0), num(0)), div(num(1), num(0)),
                                 div(num(-1), num(0))]
    for path in ("boxed", "float", "float_bail"):
        overrides = ({"XYO_JIT_NUMERIC_REPEAT_SSA": "1",
                      "XYO_JIT_NUMERIC_REPEAT_INT": "0",
                      "XYO_JIT_NUMERIC_REPEAT_FLOAT": "0"} if path == "boxed" else {})
        for index, source in enumerate(special_sources):
            for count in (1, 65, 129):
                label = f"{path}-s{index}-n{count}"
                sb3 = out / f"{label}.sb3"
                write_sb3(sb3, fixture(count, source, True, False, path))
                expected, _ = run(binary, sb3, "off", 0, out, label + "-interp")
                for enabled in (None, 0, 1):
                    actual, ir = run(binary, sb3, "eager", enabled, out,
                                     label + f"-jit{enabled}", dump=(index == 0),
                                     overrides=overrides)
                    if actual != expected:
                        raise RuntimeError(f"{label} copy={enabled}: {actual} != {expected}")
                    if enabled != 0 and index == 0:
                        marker = "rep.numeric.fast" if path == "boxed" else "rep.float.fast"
                        if marker not in ir:
                            raise RuntimeError(f"{label}: missing {marker} lowering")
                rows.append({"case": label, "expected": expected})
    # A parity-only test could pass even when the new optimization never runs.
    # This numeric-initialized fixture must emit the path only when enabled.
    sb3 = out / "lowering.sb3"
    write_sb3(sb3, structure_project(129, "unrelated_write", 1))
    baseline, off_ir = run(binary, sb3, "eager", 0, out, "lowering-off", True)
    optimized, on_ir = run(binary, sb3, "eager", 1, out, "lowering-on", True)
    if baseline != optimized or "rep.numeric.fast" not in on_ir or "rep.numeric.fast" in off_ir:
        raise RuntimeError("copy SSA path was not selected exclusively with copies enabled")
    default, default_ir = run(binary, sb3, "eager", None, out, "lowering-default", True)
    if default != optimized or "rep.numeric.fast" not in default_ir:
        raise RuntimeError("unset toggle did not select default-on copy SSA")
    (out / "results.json").write_text(json.dumps(rows, indent=2))
    print(f"PASS {len(rows)} fixtures / {len(rows)*3} JIT-vs-interpreter comparisons; "
          "copy SSA lowering default/ON/OFF verified")


if __name__ == "__main__":
    main()
