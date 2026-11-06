#!/usr/bin/env python3
"""Parity for an IR-resident empty repeat scheduler with exact poll boundaries."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_repeat, S_set, S_change, make_project,
    num, text, var, write_sb3,
)


def fixture(out: Path, warp: bool) -> list[str]:
    # For non-warp tests keep the total number of yields bounded.
    cases = [
        (num(0), False),
        (num(1), False),
        (num(63), False),
        (num(64), False),
        (num(65), False),
        (num(127), False),
        (num(128), False),
        (num(129), False),
        (num(2.5), False),
        (num(-1), False),
        (text("abc"), False),
        (text("65"), False),
        (num(100001) if warp else num(3), False),
        (num(4096) if warp else num(4), True),
    ]
    keys = [f"checkpoint_{i}" for i in range(len(cases))]
    g = Gen(["dynamic", "counter", "done", *keys], [])
    statements = [S_set("counter", num(0))]
    for i, (count, dynamic) in enumerate(cases):
        if dynamic:
            statements += [S_set("dynamic", count)]
            count = var("dynamic")
        statements += [
            S_repeat(count, []),
            S_change("counter", num(1)),
            S_set(keys[i], var("counter")),
        ]
    statements.append(S_set("done", num(1)))
    g.define("test", [], statements, warp=warp)
    g.script([S_call("test")])
    write_sb3(out, make_project(g.vars.keys(), g.lists.keys(), g))
    return [*keys, "done", "counter"]


def run(binary: Path, sb3: Path, mode: str, enabled: bool, keys: list[str],
        extra_env: dict[str, str] | None = None):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("XYO_")}
    env.update(XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0",
               XYO_PARFOR="0", XYO_JIT_INLINE_EMPTY_SCHED=str(int(enabled)))
    env.update(extra_env or {})
    proc = subprocess.run(
        [str(binary), "run", str(sb3), "--jit", mode, "--frames", "650"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=90,
    )
    if proc.returncode:
        raise RuntimeError((sb3.name, mode, enabled, proc.returncode,
                            proc.stderr[-600:]))
    output = proc.stdout
    data = json.loads(output[output.index("{"):output.rindex("}")+1])["Bench"]
    return {key: data[key] for key in keys}


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT/"build/xyo"
    out = ROOT/"build/llvm-scheduler-study"
    out.mkdir(exist_ok=True, parents=True)
    for warp in [True, False]:
        path = out / ("empty-warp.sb3" if warp else "empty-nonwarp.sb3")
        keys = fixture(path, warp)
        baseline = run(binary, path, "off", False, keys)
        assert baseline["done"] == "1", (warp, baseline)
        for tier in ["eager", "hotspot"]:
            for enabled in [False, True]:
                for clock in ["1", "0"]:
                    actual = run(
                        binary, path, tier, enabled, keys,
                        {"XYO_JIT_WARP_CLOCK": clock})
                    assert actual == baseline, (
                        warp, tier, enabled, clock, actual, baseline)
                    print("PASS", warp, tier, enabled, clock, flush=True)


if __name__ == "__main__":
    main()
