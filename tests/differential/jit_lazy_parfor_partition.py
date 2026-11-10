#!/usr/bin/env python3
"""Opt-in ORC co-partition test: address-taken par-for callback stays callable.

The default guard is still tested by jit_lazy_parfor_callback.py. Compare final
Scratch values with eager and interpreter, and verify both sequential and
actual worker-pool callbacks with two bodies in the same warp procedure.
"""
from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))
from gen_algo_bench import (  # noqa: E402
    Gen, S_add, S_call, S_change, S_repeat, S_set, make_project,
    num, var, write_sb3,
)


def fixture(path: Path, trip: int) -> None:
    gen = Gen(["i", "j", "done"], ["a", "b"])
    gen.define("fill", [], [
        S_set("i", num(0)),
        S_repeat(num(trip), [
            S_change("i", num(1)),
            S_add("a", var("i")),
        ]),
        S_set("j", num(0)),
        S_repeat(num(trip), [
            S_change("j", num(1)),
            S_add("b", var("j")),
        ]),
        S_set("done", num(1)),
    ], warp=True)
    gen.script([S_call("fill")])
    write_sb3(str(path), make_project(gen.vars.keys(), gen.lists.keys(), gen))


def run(binary: Path, project: Path, mode: str, partition: bool) -> tuple[dict, str]:
    env = {key: val for key, val in os.environ.items() if not key.startswith("XYO_")}
    env.update({
        "XYO_JIT_MODE": mode,
        "XYO_JIT_LAZY_MODE": "on",
        "XYO_JIT_LAZY_CALLBACK_TRACE": "1",
        "XYO_JIT_TIMING": "1",
        "XYO_PARFOR": "1",
        "XYO_PARFOR_MIN": "512",
        "XYO_PARFOR_PARALLEL_MIN": "2048",
        "XYO_PARFOR_CHUNK": "512",
        "XYO_PARFOR_DEBUG": "1",
        "XYO_HOT_INVOCATIONS": "1",
        "XYO_DUMPVARS": "json",
    })
    if partition:
        env["XYO_JIT_LAZY_PARFOR_PARTITION"] = "1"
    result = subprocess.run(
        [str(binary), "run", str(project), "--frames", "3000"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=90,
    )
    if result.returncode:
        raise AssertionError(
            f"{mode=} {partition=} exit={result.returncode}\n"
            f"stderr={result.stderr[-2500:]}\nstdout={result.stdout[-500:]}")
    start, end = result.stdout.find("{"), result.stdout.rfind("}")
    assert start >= 0 and end > start, result.stdout[-1000:]
    state = json.loads(result.stdout[start:end+1])["Bench"]
    return state, result.stderr


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--native", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    binary = args.native.resolve(strict=True)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    for trip, parallel in ((1000, False), (8192, True)):
        project = out / f"two-parfor-{trip}.sb3"
        fixture(project, trip)
        expected = [str(i) for i in range(1, trip+1)]
        for mode, partition in (
            ("off", False), ("eager", False),
            ("hotspot", False), ("hotspot", True)
        ):
            state, log = run(binary, project, mode, partition)
            assert state["done"] == "1", (mode, partition, state["done"])
            assert state["i"] == str(trip), state["i"]
            assert state["j"] == str(trip), state["j"]
            assert state["a"] == expected, ("a", mode, partition, state["a"][:5])
            assert state["b"] == expected, ("b", mode, partition, state["b"][:5])
            if mode != "off":
                assert ("parfor: parallel" if parallel else "parfor: sequential") in log, (
                    mode, partition, log[-1600:])
                assert ("tier=lazy" if partition else "tier=eager") in log, (
                    mode, partition, log[-1600:])
                if partition:
                    assert "reason=parfor-partition-opt-in" in log
                    assert re.search(
                        r"^\[jit-lazy-partition\] .*callbacks=2$", log, re.M
                    ), ("two callback bodies not co-materialized", log[-2000:])
                else:
                    assert "reason=parfor-callback" in log
            if mode == "hotspot" and partition and parallel:
                # Repeated fresh processes exercise real worker-pool execution,
                # callback pointer validity and teardown without warm JIT state.
                for _ in range(8):
                    repeated, again = run(binary, project, mode, partition)
                    assert repeated == state, "parallel state changed between runs"
                    assert "parfor: parallel" in again
            print(f"PASS {mode=} {partition=} {trip=} {parallel=}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
