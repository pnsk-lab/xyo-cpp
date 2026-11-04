#!/usr/bin/env python3
"""Differential fixtures for provable and unprovable Scratch for-each ranges.

Run:
    python3 tests/differential/jit_induction_ranges.py \
        --binary build/induction-range-xyo

Builds *ordinary* Scratch projects; the compiler cannot inspect fixture IDs
or benchmark labels. Compares interpreter, normal JIT, and optimized JIT.
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (Gen, S_add, S_change, S_if, S_set, add,
                            gt, make_project, mod, mul, num, sub, var, write_sb3)


def make_fixture(mode: str, limit: float, *, repeated: bool = False):
    g = Gen(["i", "sum", "n"], ["out"])
    # The index is updated by the for-each itself. Its body never yields
    # for safe modes; alternate modes deliberately invalidate the proof.
    body = [
        S_change("sum", mod(var("i"), num(7))),
        S_change("sum", mod(add(var("i"), num(3)), num(5))),
        S_change("sum", mod(mul(var("i"), num(2)), num(13))),
    ]
    if mode == "index_write":
        body = [S_set("i", num(-9))] + body
    elif mode == "dynamic_write":
        body = [S_change("n", num(-1))] + body
    elif mode == "nested_if":
        body.append(S_if(gt(var("i"), num(3)),
                         [S_change("sum", num(2))]))
    elif mode == "negative_result":
        body.append(S_change("sum", mod(sub(var("i"), num(20)), num(9))))
    elif mode == "zero_result":
        body.append(S_change("sum", mod(sub(var("i"), num(1)), num(7))))
    elif mode != "bounded" and mode != "dynamic":
        raise ValueError(mode)

    d = g.define("bench", [], [S_set("sum", num(0)),
                                S_set("n", num(limit))], warp=True)
    last = g.blocks[d]["next"]
    while g.blocks[last]["next"]:
        last = g.blocks[last]["next"]
    loop = g.new("control_for_each", parent=last,
                 fields={"VARIABLE": g.field("VARIABLE", "i")})
    g.blocks[last]["next"] = loop
    bound = var("n") if mode in ("dynamic", "dynamic_write") else num(limit)
    g.blocks[loop]["inputs"]["VALUE"] = g.emit_value(bound, loop)
    first = g.chain(body, loop)
    g.blocks[loop]["inputs"]["SUBSTACK"] = [2, first]
    tail = g.chain([S_add("out", var("sum")),
                    S_add("out", var("i"))], loop)
    g.blocks[loop]["next"] = tail
    g.script([S_add("out", num(314159)), ("call", "bench", [])])
    return make_project(g.vars.keys(), g.lists.keys(), g)


def run(binary: Path, sb3: Path, mode: str,
        *, foreach_counter: bool = False) -> tuple[str, ...]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env["XYO_GPU"] = "0"
    env["XYO_PARFOR"] = "0"
    env["XYO_JIT_INDUCTION_RANGES"] = "1" if mode == "on" else "0"
    env["XYO_JIT_FOREACH_INT_COUNTER"] = (
        "1" if mode == "on" and foreach_counter else "0")
    argv = [str(binary), "run", str(sb3), "--jit",
            "off" if mode == "interpreter" else "eager",
            "--print-list", "out"]
    p = subprocess.run(argv, cwd=ROOT, env=env, text=True,
                       capture_output=True, timeout=90)
    if p.returncode:
        raise RuntimeError(f"{sb3.name}/{mode} failed {p.returncode}:"
                           f" {p.stdout[-300:]} {p.stderr[-1200:]}")
    # --print-list writes values to stdout and CLI diagnostics to stderr.
    return tuple(line.strip() for line in p.stdout.splitlines()
                 if line.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path,
                        default=ROOT / "build/induction-range-fixtures")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--foreach-i32", action="store_true",
                        help="compare both induction range analysis and an "
                             "exact i32 for-each internal counter")
    opts = parser.parse_args()
    binary = opts.binary.resolve()
    opts.out.mkdir(parents=True, exist_ok=True)
    cases = {
        "small": ("bounded", 29),
        "fractional": ("bounded", 7.4),
        "small_fraction": ("bounded", 0.2),
        "zero": ("bounded", 0),
        "negative_bound": ("bounded", -4),
        "non_integer": ("bounded", 11.75),
        "dynamic": ("dynamic", 37),
        "dynamic_write": ("dynamic_write", 37),
        "index_write": ("index_write", 25),
        "nested_if": ("nested_if", 25),
        "negative_result": ("negative_result", 25),
        "zero_result": ("zero_result", 25),
        "large": ("bounded", 150000),
    }
    if opts.quick:
        cases.pop("large")
    data = []
    for name, (mode, limit) in cases.items():
        sb3 = opts.out / f"{name}.sb3"
        write_sb3(sb3, make_fixture(mode, limit))
        readings = {opt: run(binary, sb3, opt,
                             foreach_counter=opts.foreach_i32)
                    for opt in ("interpreter", "off", "on")}
        assert readings["interpreter"] == readings["off"] == readings["on"], (
            f"{name} mismatch: {readings}")
        data.append({"case": name, "limit": limit, "mode": mode,
                     "result": readings["on"]})
        print(f"PASS {name}: {readings['on']}", flush=True)
    (opts.out / "results.json").write_text(json.dumps(data, indent=2))
    print(f"PASS all {len(data)} differential fixtures")


if __name__ == "__main__":
    main()
