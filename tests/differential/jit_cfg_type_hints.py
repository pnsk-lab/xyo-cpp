#!/usr/bin/env python3
"""CFG-proven copy writes: interpreter parity and guarded numeric promotion."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_ifelse, S_repeat, S_set,
    add, gt, num, text, var, make_project, write_sb3,
)


def fixture(path, warp):
    outputs = [f"out_{i}" for i in range(6)]
    gen = Gen(["source", "copy", "sum", "done", *outputs], [])
    commands = [S_set("sum", num(0))]
    for i, times in enumerate([0, 1, 2, 64, 65, 129]):
        body = [
            S_set("source", add(var("sum"), num(1))),
            S_set("copy", var("source")),
            S_change("sum", var("copy")),
            S_change("sum", var("copy")),
            S_ifelse(gt(var("sum"), num(100)),
                     [S_set("source", add(var("sum"), num(-7)))],
                     [S_set("source", add(var("sum"), num(3)))]),
            S_set("copy", var("source")),
            S_change("sum", var("copy")),
        ]
        commands.extend([S_repeat(num(times), body),
                         S_set(outputs[i], var("sum"))])
    commands.append(S_set("done", num(1)))
    gen.define("work", [], commands, warp=warp)
    gen.script([S_call("work")])
    write_sb3(path, make_project(gen.vars.keys(), gen.lists.keys(), gen))
    return outputs + ["source", "copy", "sum", "done"]


def mixed_fixture(path, warp):
    gen = Gen(["source", "copy", "sum", "done"], [])
    body = [
        S_ifelse(gt(var("sum"), num(10)),
                 [S_set("source", text("not numeric"))],
                 [S_set("source", add(var("sum"), num(1)))]),
        S_set("copy", var("source")),
        S_change("sum", var("copy")),
        S_change("sum", var("copy")),
    ]
    gen.define("mixed", [], [S_set("sum", num(0)),
                               S_repeat(num(40), body),
                               S_set("done", num(1))], warp=warp)
    gen.script([S_call("mixed")])
    write_sb3(path, make_project(gen.vars.keys(), gen.lists.keys(), gen))
    return ["source", "copy", "sum", "done"]


def execute(binary, project, keys, tier, hints):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(XYO_JIT_CFG_TYPE_HINTS="1" if hints else "0",
               XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0",
               XYO_JIT_VERBOSE="1")
    p = subprocess.run([str(binary), "run", str(project), "--jit", tier,
                        "--frames", "1200"],
                       cwd=ROOT, env=env, capture_output=True, text=True,
                       timeout=110)
    if p.returncode:
        raise RuntimeError((project.name, tier, hints, p.returncode,
                            p.stderr[-900:]))
    raw = p.stdout
    data = json.loads(raw[raw.index("{"):raw.rindex("}") + 1])["Bench"]
    return ({k: data[k] for k in keys}, p.stderr)


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xyo"
    dest = ROOT / "build/cfg-typed-promotion"
    dest.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        path = dest / ("warp.sb3" if warp else "nonwarp.sb3")
        keys = fixture(path, warp)
        expected, _ = execute(binary, path, keys, "off", False)
        optimized_once = False
        for tier, hints in [("eager", False), ("eager", True),
                            ("hotspot", True), ("eager", True)]:
            actual, err = execute(binary, path, keys, tier, hints)
            if actual != expected:
                raise AssertionError((warp, tier, hints, expected, actual))
            optimized_once |= "cfg numeric-shadow" in err
            print("PASS", warp, tier, hints, flush=True)
        if not optimized_once:
            raise AssertionError("CFG numeric promotion did not activate")

        # A join of String and Number must remain mixed. The cost hint must
        # not specialize a variable that can receive a string on any path.
        mixed = dest / ("mixed-warp.sb3" if warp else "mixed-nonwarp.sb3")
        mixed_keys = mixed_fixture(mixed, warp)
        baseline, _ = execute(binary, mixed, mixed_keys, "off", False)
        actual, log = execute(binary, mixed, mixed_keys, "eager", True)
        if actual != baseline:
            raise AssertionError(("mixed", warp, baseline, actual))
        if "cfg numeric-shadow" in log:
            raise AssertionError("mixed-type write incorrectly promoted")
        print("PASS mixed guarded", warp, flush=True)


if __name__ == "__main__":
    main()
