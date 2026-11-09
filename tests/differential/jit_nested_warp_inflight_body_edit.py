#!/usr/bin/env python3
"""After a *real native Fiber yield*, modify a reachable loop-body write.

Without an in-flight guard, old LLVM keeps writing sum, despite real
Blocks::resetCache changing data_changevariableby's VARIABLE id.
With the bounded source guard, native is discarded and the still-live
interpreter executes the edited statement instead. Verify no lost work
or duplicate side effects, full state, and exact executed block count.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/differential"))
from jit_nested_warp_parent_repeat_osr import fixture

PROMOTION = re.compile(r"\[jit-osr-nested\] promoted ")
RESTORE = re.compile(r"\[jit-osr-nested\] restored caller")
DEOPT = re.compile(
    r"\[jit-osr-inflight\] deopt remaining=([0-9.]+) source-changed=1"
)
EDIT = re.compile(
    r"\[osr-body-edit\] suspended remaining=([0-9.eE+\-]+) "
    r"guard=([01]) committed-sum=([0-9.]+) blocks=(\d+) yields=(\d+)"
)


def run_mode(harness: Path, project: Path, mode: str, *, count: bool):
    env = dict(os.environ, XYO_BLOCK_COUNT="1" if count else "0")
    proc = subprocess.run(
        [str(harness.resolve()), str(project.resolve()), mode],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=240,
    )
    assert proc.returncode == 0, (
        mode, proc.returncode, proc.stdout[-1400:], proc.stderr[-4000:]
    )
    state = {}
    for line in proc.stdout.splitlines():
        if line.startswith("STATE "):
            name, value = line[6:].split("=", 1)
            assert name not in state, line
            state[name] = value
    assert len(state) == (11 if count else 10), (mode, state)
    return state, proc.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--harness", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--iterations", type=int, default=8_000_000)
    ap.add_argument("--without-block-count", action="store_true")
    args = ap.parse_args()
    count = not args.without_block_count
    args.out.mkdir(parents=True, exist_ok=True)
    sb3 = fixture(
        args.out / "live-body-sum-to-branch-count.sb3",
        parent_count=3, inner_iterations=args.iterations, gc=True,
    )
    total = 6 * args.iterations

    expected, _ = run_mode(args.harness, sb3, "baseline", count=count)
    assert expected["v_sum"] == f"n:{total}", expected
    assert expected["v_branchCount"] == "s:0", expected
    if count:
        assert expected["__block_count"].startswith("n:"), expected
    positive, trace = run_mode(args.harness, sb3, "positive", count=count)
    assert positive == expected, (positive, expected)
    assert len(PROMOTION.findall(trace)) == 3, trace[-3000:]
    assert len(RESTORE.findall(trace)) == 3, trace[-3000:]
    print("PASS positive: native promotion/restoration 3/3, baseline parity", flush=True)

    results = {}
    for mode, deopt in (
        ("unguarded", False), ("guarded", True),
        ("unguarded-late", False), ("guarded-late", True),
    ):
        state, trace = run_mode(args.harness, sb3, mode, count=count)
        edit = EDIT.findall(trace)
        assert len(edit) == 1, (mode, trace[-3000:])
        remaining, flag, committed, at_count, yields = edit[0]
        assert flag == ("1" if deopt else "0"), (mode, edit)
        assert int(yields) >= (4 if mode.endswith("-late") else 1), (
            mode, yields, edit
        )
        assert 0 < float(committed) < total, (mode, edit)
        if count:
            assert 0 <= int(at_count) < int(expected["__block_count"][2:])
        assert len(PROMOTION.findall(trace)) == 1, (mode, trace[-2700:])
        assert len(DEOPT.findall(trace)) == int(deopt), (
            mode, trace[-2700:]
        )
        assert len(RESTORE.findall(trace)) == (0 if deopt else 1), (
            mode, trace[-2700:]
        )

        assert int(state["v_sum"][2:]) + int(state["v_branchCount"][2:]) == total, state
        if count:
            assert state["__block_count"] == expected["__block_count"], (
                mode, state["__block_count"], expected["__block_count"]
            )
        for field in expected:
            if field not in ("v_sum", "v_branchCount"):
                assert state[field] == expected[field], (mode, field, state[field], expected[field])
        if deopt:
            # Every subsequent iteration (including later parent repeats)
            # must honor the newly edited target instead of stale native code.
            assert state["v_sum"] == f"n:{int(float(committed))}", (
                mode, edit, state
            )
            assert state["v_branchCount"] == f"n:{total-int(float(committed))}", state
        else:
            assert int(state["v_sum"][2:]) > float(committed), (
                mode, "negative control did not run stale native body", edit, state
            )
        results[mode] = (state, float(committed))
        print(
            f"PASS {mode}: edits=1 native=1 deopts={int(deopt)} "
            f"sum={state['v_sum']} branch={state['v_branchCount']} "
            f"blocks={state.get('__block_count', 'disabled')}",
            flush=True,
        )

    # In guarded mode the rewritten Scratch target is used on every
    # remaining execution; without the guard, *some* old LLVM effects land.
    # The precise source-change timestamp can differ slightly across runs.
    for suffix in ("", "-late"):
        assert (
            results["guarded" + suffix][0]["v_sum"]
            != results["unguarded" + suffix][0]["v_sum"]
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
