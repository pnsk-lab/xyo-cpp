#!/usr/bin/env python3
"""Exercise repeated native par-for callbacks in one long-lived JIT instance.

Compare complete ordered Scratch lists with Interpreter, Eager, guarded
HotSpot, and opt-in lazy HotSpot. A new process is not spawned per batch.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

from jit_lazy_parfor_partition import (
    Gen, S_add, S_call, S_change, S_repeat, S_set,
    make_project, num, var, write_sb3,
)

DISPATCH = re.compile(r"^parfor: parallel trip=(\d+)\b", re.M)
GROUPED = re.compile(
    r"^\[jit-lazy-partition\] requested=\d+ materialized=\d+ callbacks=2$",
    re.M,
)


def fixture(path: Path, trip: int, rounds: int) -> None:
    gen = Gen(["i", "j", "done"], ["a", "b"])
    gen.define("fill", [], [
        S_set("i", num(0)),
        S_repeat(num(trip), [S_change("i", num(1)), S_add("a", var("i"))]),
        S_set("j", num(0)),
        S_repeat(num(trip), [S_change("j", num(1)), S_add("b", var("j"))]),
        S_change("done", num(1)),
    ], warp=True)
    gen.script([S_set("done", num(0)),
                S_repeat(num(rounds), [S_call("fill")])])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(gen.vars.keys(), gen.lists.keys(), gen))


def run(binary: Path, project: Path, mode: str, optin: bool,
        timeout: int) -> tuple[dict, str, float]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_MODE": mode, "XYO_JIT_LAZY_MODE": "on",
        "XYO_JIT_LAZY_CALLBACK_TRACE": "1",
        "XYO_PARFOR": "1", "XYO_PARFOR_MIN": "512",
        "XYO_PARFOR_PARALLEL_MIN": "2048", "XYO_PARFOR_CHUNK": "512",
        "XYO_PARFOR_DEBUG": "1", "XYO_HOT_INVOCATIONS": "1",
        "XYO_DUMPVARS": "json",
    })
    if optin:
        env["XYO_JIT_LAZY_PARFOR_PARTITION"] = "1"
    started = time.perf_counter()
    proc = subprocess.run(
        [str(binary), "run", str(project), "--frames", "3000"],
        capture_output=True, text=True, env=env, timeout=timeout,
    )
    elapsed = time.perf_counter() - started
    if proc.returncode:
        raise AssertionError(
            f"mode={mode} optin={optin} exit={proc.returncode}\n"
            f"stdout={proc.stdout[-500:]}\nstderr={proc.stderr[-1500:]}"
        )
    start, end = proc.stdout.find("{"), proc.stdout.rfind("}")
    if start < 0 or end <= start:
        raise AssertionError(f"missing final JSON: {proc.stdout[-500:]}")
    return json.loads(proc.stdout[start:end + 1])["Bench"], proc.stderr, elapsed


def verify(state: dict, trip: int, rounds: int) -> str:
    assert state["done"] == str(rounds), ("done", state["done"])
    assert state["i"] == str(trip) and state["j"] == str(trip)
    expected = [str(i) for i in range(1, trip + 1)] * rounds
    for name in ("a", "b"):
        assert state[name] == expected, (
            "incorrect list order/value", name, len(state[name])
        )
    return hashlib.sha256(json.dumps(
        state, sort_keys=True, separators=(",", ":")
    ).encode()).hexdigest()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--native", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--trip", type=int, default=4096)
    ap.add_argument("--rounds", type=int, default=16)
    ap.add_argument("--repeat-optin", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=90)
    args = ap.parse_args()
    if args.rounds < 2 or args.trip < 2048 or args.repeat_optin < 1:
        ap.error("requires rounds>=2, trip>=2048 and repeat-optin>=1")
    binary = args.native.resolve(strict=True)
    project = args.out / f"parfor-longrun-{args.trip}x{args.rounds}.sb3"
    fixture(project, args.trip, args.rounds)
    cases = [
        ("off", False), ("eager", False),
        ("hotspot", False), ("hotspot", True),
    ] + [("hotspot", True)] * (args.repeat_optin - 1)
    reference = None
    for mode, optin in cases:
        state, log, elapsed = run(binary, project, mode, optin, args.timeout)
        digest = verify(state, args.trip, args.rounds)
        assert reference in (None, digest), (
            "final Scratch state changed", mode, optin, digest, reference
        )
        reference = digest
        if mode != "off":
            count = sum(int(x) == args.trip for x in DISPATCH.findall(log))
            assert count == 2 * args.rounds, (
                "native dispatch count mismatch", mode, optin, count
            )
            if optin:
                assert "reason=parfor-partition-opt-in" in log
                assert GROUPED.search(log), "callback grouping not observed"
            else:
                assert "reason=parfor-callback" in log
        print(f"PASS mode={mode} optin={int(optin)} "
              f"dispatches={2 * args.rounds if mode != 'off' else 0} "
              f"elapsed={elapsed:.3f}s digest={digest[:16]}", flush=True)
    print("PASS repeated callback reuse / exact Scratch ordering")


if __name__ == "__main__":
    main()
