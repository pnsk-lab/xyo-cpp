#!/usr/bin/env python3
"""Differential test for guarded resumption of promoted Scratch variables.

Runs the same SB3 with the epoch guard enabled and disabled, and ensures
numeric/string/changed-by-other-script values match. No benchmark-specific
code enters the runtime.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_change, S_repeat, S_set,
    make_project, num, text, var, write_sb3,
)


def make_fixture(kind, count=800):
    g = Gen(["source", "sum", "i", "snapshot"], [])
    if kind == "numeric":
        initial = num(7)
    elif kind == "string":
        initial = text("7")
    elif kind == "whitespace":
        initial = text(" ")
    elif kind == "negative_zero":
        initial = num(0)  # The project JSON initializes it to -0.0 below.
    elif kind == "nonnumeric":
        initial = text("not a number")
    else:
        initial = text("7")
    start = [] if kind == "negative_zero" else [S_set("source", initial)]
    g.script(start + [
        S_set("sum", num(0)),
        S_set("i", num(0)),
        # Non-warp loop: every iteration hands control to other scripts.
        S_repeat(num(count), [
            S_change("sum", var("source")),
            S_change("i", num(1)),
        ]),
        S_set("snapshot", var("source")),
    ])
    if kind == "other_script":
        # The other green-flag thread writes to exactly the variable the
        # promoted loop reads. Include strings and numeric values.
        g.script([
            S_repeat(num(40), [
                S_set("source", text("9")),
                S_set("source", num(3)),
            ])
        ], x=250)
    project = make_project(g.vars.keys(), g.lists.keys(), g)
    if kind in ("string", "whitespace", "other_script", "nonnumeric"):
        initial_strings = {"string": "7", "whitespace": " ",
                           "other_script": "7", "nonnumeric": "not a number"}
        project["targets"][1]["variables"][g.vars["source"]][1] = initial_strings[kind]
    if kind == "negative_zero":
        project["targets"][1]["variables"][g.vars["source"]][1] = -0.0
    return project


def execute(binary, path, guard):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_EPOCH_GUARD": "1" if guard else "0",
        "XYO_DUMPVARS": "json",
        "XYO_GPU": "0", "XYO_PARFOR": "0",
        "XYO_GC_THRESHOLD": "8",
    })
    p = subprocess.run([str(binary), "run", str(path), "--jit", "eager"],
                       cwd=ROOT, env=env, text=True, capture_output=True,
                       timeout=120)
    if p.returncode:
        raise RuntimeError(f"{path.name} guard={guard}: {p.stderr[-1000:]}")
    try:
        data = json.loads(p.stdout[p.stdout.find("{"):p.stdout.rfind("}") + 1])
        return data["Bench"]
    except (ValueError, KeyError) as err:
        raise RuntimeError(f"Could not read values from {path.name}: {p.stdout[-1200:]}") from err


def main():
    binary = (ROOT / "build/xyo").resolve()
    if len(sys.argv) > 1:
        binary = Path(sys.argv[1]).resolve()
    outdir = ROOT / "build/epoch-guard"
    outdir.mkdir(parents=True, exist_ok=True)
    for kind in ["numeric", "string", "whitespace", "nonnumeric", "negative_zero", "other_script"]:
        project = outdir / f"{kind}.sb3"
        write_sb3(project, make_fixture(kind))
        baseline = execute(binary, project, guard=False)
        new = execute(binary, project, guard=True)
        assert baseline == new, f"{kind}: {baseline!r} != {new!r}"
        print(kind, "PASS", json.dumps(new, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
