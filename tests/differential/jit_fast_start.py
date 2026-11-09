#!/usr/bin/env python3
"""Exercise startup tier selection on an independently generated Scratch SB3.

The fixture has several procedures and substantial LLVM IR, independent of
ScratchBench names, and compares all visible variables across compilation
tiers. The startup latency itself belongs to the separate A/B benchmark;
CTest does not make unstable wall-clock assertions.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import Gen, S_change, S_call, S_set, S_repeat, make_project, num, write_sb3


def make_fixture(path: Path):
    g = Gen(["sum", "done"], [])
    procedure_names = ("alpha", "beta", "gamma", "delta")
    for name in procedure_names:
        # Each is a distinct procedure, not a benchmark-specific function.
        g.define(name, [], [S_change("sum", num(1)) for _ in range(250)],
                 warp=True)
    g.script([
        S_set("sum", num(0)),
        # A real control loop ensures the HotSpot policy compiles the script
        # on first invocation. It has no visible side effects.
        S_repeat(num(1), []),
        *(S_call(name) for name in procedure_names),
        S_set("done", num(1)),
    ])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))


def execute(binary: Path, path: Path, tier: str, fast_start: bool):
    env = {key: val for key, val in os.environ.items()
           if not key.startswith("XYO_")}
    env.update(XYO_DUMPVARS="json", XYO_JIT_TIMING="1",
               XYO_JIT_FAST_START="1" if fast_start else "0",
               XYO_GPU="0", XYO_PAR="0", XYO_PARFOR="0")
    proc = subprocess.run(
        [str(binary), "run", str(path), "--jit", tier, "--frames", "300"],
        env=env, capture_output=True, text=True, cwd=ROOT, timeout=90)
    if proc.returncode:
        raise RuntimeError((tier, fast_start, proc.returncode,
                            proc.stderr[-600:], proc.stdout[-600:]))
    output = proc.stdout
    project = json.loads(
        output[output.index("{"):output.rindex("}")+1])["Bench"]
    assert project["done"] == "1", project
    assert project["sum"] == "1000", project
    return project, proc.stderr


def main():
    binary = (Path(sys.argv[1]) if len(sys.argv) > 1
              else ROOT/"build/xyo").resolve()
    out = ROOT/"build/startup-profile"
    out.mkdir(parents=True, exist_ok=True)
    sb3 = out/"jit-faststart-parity.sb3"
    make_fixture(sb3)
    baseline, _ = execute(binary, sb3, "off", False)
    normal, normal_log = execute(binary, sb3, "hotspot", False)
    fast, fast_log = execute(binary, sb3, "hotspot", True)
    eager, _ = execute(binary, sb3, "eager", True)
    assert normal == fast == baseline == eager, (normal, fast, baseline, eager)
    assert re.search(r"tier=eager", normal_log), normal_log[-700:]
    assert re.search(r"tier=lazy", fast_log), fast_log[-700:]
    print("PASS: 4 generic procedures; eager/lazy/HotSpot/interpreter parity",
          flush=True)


if __name__ == "__main__":
    main()
