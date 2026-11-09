#!/usr/bin/env python3
"""Regression: par-for native callbacks must not use lazy LLVM materialization.

The JIT passes an internal LLVM function pointer to the native xyo_par_for
helper. Its body must already be executable on the very first iteration.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))
from gen_algo_bench import (  # noqa: E402
    Gen, S_add, S_call, S_change, S_repeat, S_set, make_project,
    num, var, write_sb3,
)


def make_append_fixture(path: Path) -> None:
    gen = Gen(["i", "done"], ["out"])
    gen.define("fill", [], [
        S_set("i", num(0)),
        S_repeat(num(1000), [
            S_change("i", num(1)),
            S_add("out", var("i")),
        ]),
        S_set("done", num(1)),
    ], warp=True)
    gen.script([S_call("fill")])
    write_sb3(str(path), make_project(gen.vars.keys(), gen.lists.keys(), gen))


def make_plain_fixture(path: Path) -> None:
    gen = Gen(["i", "done"], [])
    gen.define("plain", [], [S_set("i", num(7))], warp=True)
    gen.script([S_call("plain"), S_set("done", num(1))])
    write_sb3(str(path), make_project(gen.vars.keys(), gen.lists.keys(), gen))


def run(binary: Path, project: Path, mode: str) -> tuple[dict, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_MODE": mode,
        "XYO_JIT_LAZY_MODE": "on",
        "XYO_JIT_LAZY_CALLBACK_TRACE": "1",
        "XYO_JIT_TIMING": "1",
        "XYO_PARFOR": "1",
        "XYO_PARFOR_MIN": "512",
        "XYO_PARFOR_PARALLEL_MIN": "2048",
        "XYO_PARFOR_DEBUG": "1",
        "XYO_HOT_INVOCATIONS": "1",
        "XYO_DUMPVARS": "json",
    })
    result = subprocess.run(
        [str(binary), "run", str(project), "--frames", "3000"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=60,
    )
    if result.returncode:
        raise AssertionError(
            f"mode={mode} returncode={result.returncode}\n"
            f"stdout={result.stdout[-1600:]}\nstderr={result.stderr[-3000:]}"
        )
    start = result.stdout.find("{")
    end = result.stdout.rfind("}")
    assert start >= 0 and end > start, result.stdout[-2000:]
    state = json.loads(result.stdout[start:end + 1])["Bench"]
    return state, result.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    append = out / "parfor-callback.sb3"
    plain = out / "plain-lazy-control.sb3"
    make_append_fixture(append)
    make_plain_fixture(plain)

    for mode in ("off", "eager", "hotspot"):
        state, stderr = run(native, append, mode)
        assert state["i"] == "1000" and state["done"] == "1", state
        # The callback must write *every* reserved list slot in source order,
        # not merely update the final induction variable.
        assert state["out"] == [str(i) for i in range(1, 1001)], (
            "parfor list values/order diverged", mode,
            state["out"][:8], state["out"][-8:],
        )
        if mode != "off":
            assert "reason=parfor-callback" in stderr, (
                "native callback guard did not trigger", mode, stderr[-2500:]
            )
            assert "tier=eager" in stderr, (
                "callback script was deferred", mode, stderr[-2500:]
            )
            assert "parfor: sequential trip=1000" in stderr, (
                "native callback was never exercised", mode, stderr[-2500:]
            )
        print(f"PASS native callback mode={mode} i={state['i']}", flush=True)

    state, stderr = run(native, plain, "hotspot")
    assert state["i"] == "7" and state["done"] == "1", state
    assert "tier=lazy" in stderr, (
        "no-callback control unexpectedly disabled lazy JIT", stderr[-2500:]
    )
    assert "reason=parfor-callback" not in stderr
    print("PASS control: without parfor callback lazy JIT remains enabled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
