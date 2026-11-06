#!/usr/bin/env python3
"""A/B parity of loop-invariant trigonometry across yield and Scratch writes."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import Gen, S_set, S_change, S_repeat, add, num, text, var, op, make_project, write_sb3


def trig(kind, arg):
    return op("operator_mathop", {"OPERATOR": kind}, NUM=arg)


def fixture(case):
    g = Gen(["angle", "sum"], [])
    start = text("45") if case == "numeric_string" else num(45)
    if case == "negative_zero":
        start = num(-0.0)
    if case == "nonnumeric":
        start = text("xyz")
    body = [S_change("sum", add(trig("sin", var("angle")),
                                trig("cos", var("angle"))))]
    if case == "written":
        body.append(S_change("angle", num(0.25)))
    count = 0 if case == "zero" else 800
    g.script([S_set("angle", start), S_set("sum", num(0)),
              S_repeat(num(count), body)])
    if case == "other_thread":
        g.script([S_repeat(num(50), [
            S_set("angle", num(90)),
            S_set("angle", text("30")),
        ])], x=240)
    return make_project(g.vars.keys(), g.lists.keys(), g)


def run(binary, path, enabled):
    env = {k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update(XYO_JIT_TRIG_HOIST="1" if enabled else "0",
               XYO_DUMPVARS="json", XYO_GPU="0", XYO_PARFOR="0")
    proc = subprocess.run([str(binary), "run", str(path), "--jit=eager"],
                          cwd=ROOT, env=env, capture_output=True,
                          text=True, timeout=120)
    if proc.returncode:
        raise RuntimeError(f"{path.name}: {proc.stderr[-1500:]}")
    return json.loads(proc.stdout[proc.stdout.index("{"):proc.stdout.rindex("}")+1])["Bench"]


def main():
    binary = Path(sys.argv[1] if len(sys.argv)>1 else ROOT / "build/xyo").resolve()
    out = ROOT / "build/trig-hoist"
    out.mkdir(parents=True, exist_ok=True)
    for case in ["constant", "numeric_string", "negative_zero", "nonnumeric", "written", "zero", "other_thread"]:
        path = out / (case + ".sb3")
        write_sb3(path, fixture(case))
        baseline, optimized = run(binary, path, False), run(binary, path, True)
        assert baseline == optimized, (case, baseline, optimized)
        print(case, "PASS", optimized, flush=True)


if __name__ == "__main__":
    main()
