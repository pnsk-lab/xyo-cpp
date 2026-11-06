#!/usr/bin/env python3
"""Differential regression for numeric-only Scratch lists."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_set, S_change, S_repeat, S_replace, S_add, S_delall,
    add, item, num, text, var, make_project, write_sb3,
)


def build_case(kind):
    g = Gen(["sum", "idx"], ["L"])
    body = [S_change("sum", item("L", var("idx")))]
    if kind == "replace_string":
        body.append(S_replace("L", num(1), text("9")))
    if kind == "replace_number":
        body.append(S_replace("L", num(1), num(9)))
    if kind == "append_string":
        body.append(S_add("L", text("5")))
    if kind == "clear":
        body.append(S_delall("L"))
    idx = (text("last") if kind == "mixed_last" else
           num(2) if kind in {"mixed_numeric_string", "mixed_nonnumeric"} else
           num(3) if kind == "mixed_bool" else
           num(100) if kind == "out_of_bounds" else
           num(1.5) if kind == "fractional_index" else
           num(1))
    g.script([S_set("sum", num(0)), S_set("idx", idx),
              S_repeat(num(120), body)])
    if kind == "other_script":
        g.script([S_repeat(num(30), [
            S_replace("L", num(1), text("11")),
            S_replace("L", num(1), num(3)),
        ])], x=250)
    values = (
        [] if kind == "empty" else
        [4, "not a number", False] if kind == "mixed_nonnumeric" else
        [4, "7", False] if kind.startswith("mixed") else
        [4, 5, 6]
    )
    return make_project(g.vars.keys(), g.lists.keys(), g,
                        list_values={"L": values})


def run(binary, path, optimized, jit="eager"):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(XYO_JIT_NUMERIC_LIST="1" if optimized else "0",
               XYO_DUMPVARS="json", XYO_GPU="0", XYO_PARFOR="0")
    r = subprocess.run([str(binary), "run", str(path), "--jit="+jit],
                       cwd=ROOT, env=env, capture_output=True,
                       text=True, timeout=120)
    if r.returncode:
        raise RuntimeError(f"{path} opt={optimized}: {r.stderr[-2000:]}")
    return json.loads(r.stdout[r.stdout.index("{"):r.stdout.rindex("}")+1])["Bench"]


def main():
    binary = (ROOT / "build/xyo").resolve()
    outdir = ROOT / "build/numeric-list"
    outdir.mkdir(exist_ok=True, parents=True)
    for case in ["numeric", "mixed", "mixed_numeric_string", "mixed_bool",
                 "mixed_last", "mixed_nonnumeric", "out_of_bounds",
                 "fractional_index", "empty", "replace_string",
                 "replace_number", "append_string", "clear", "other_script"]:
        path = outdir / (case + ".sb3")
        write_sb3(path, build_case(case))
        old, new = run(binary, path, False), run(binary, path, True)
        assert old == new, (case, old, new)
        if case != "other_script":
            interpreted = run(binary, path, False, jit="off")
            assert old == interpreted, (case, "interpreter", old, interpreted)
        print(case, "PASS", {k:v for k,v in new.items() if k != "L"}, flush=True)


if __name__ == "__main__":
    main()
