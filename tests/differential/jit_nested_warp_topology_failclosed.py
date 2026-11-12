#!/usr/bin/env python3
"""OSR-015: fail closed on editor changes to a *live* native source path.

Unlike the successful deopts of OSR-014, the Scratch topology no longer
contains one of the compiled nested counted loops. The runtime must
discard the suspended LLVM Fiber AND terminate the script rather than
reconstructing an unsafe Interpreter continuation.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).parent))
from jit_nested_warp_quad_yield_deopt import quad_fixture
from jit_nested_warp_inflight_body_edit import run_mode, PROMOTION, EDIT

DISCONNECT = re.compile(
    r"\[osr-topology\] severed edge=(\d) prefix=([^\s]+) "
    r"child=([^\s]+) revision=(\d+)->(\d+)"
)
STOP = re.compile(
    r"\[osr-topology\] fail-closed sum=(\d+) "
    r"no post-edit effects; suffix not executed"
)
UNSAFE_RESUME = re.compile(r"\[jit-osr-inflight\] deopt quad ")


def run(harness: Path, project: Path, *,
        outer_count: int, block_count: bool, late: bool):
    total = 3 * outer_count * 2 * 2 * 17 * 2
    os.environ["XYO_OSR_EXPECTED_SUM"] = str(total)
    os.environ["XYO_OSR_EXPECTED_TIMES"] = "s:7"

    # A positive control must promote on the *same fixture*.
    reference, baseline_trace = run_mode(
        harness, project, "baseline", count=block_count
    )
    assert reference["v_sum"] == f"n:{total}", reference
    positive, native_trace = run_mode(
        harness, project, "positive", count=block_count
    )
    assert positive == reference, (positive, reference)
    assert PROMOTION.search(native_trace), native_trace[-2400:]

    for edge, name in enumerate(("outer", "middle", "deep")):
        mode = f"disconnect-{name}" + ("-late" if late else "")
        state, trace = run_mode(
            harness, project, mode, count=block_count
        )
        disconnections = DISCONNECT.findall(trace)
        assert len(disconnections) == 1, (mode, trace[-2800:])
        actual, prefix, child, before, after = disconnections[0]
        assert int(actual) == edge, (mode, disconnections)
        assert int(after) > int(before), (mode, disconnections)
        stopped = STOP.findall(trace)
        assert len(stopped) == 1, (mode, trace[-3200:])
        edit = EDIT.findall(trace)
        assert len(edit) == 1, (mode, trace[-3200:])
        remaining, flag, committed, counted, yields = edit[0]
        assert flag == "1", (mode, edit)
        assert int(yields) >= (4 if late else 1), (mode, edit)
        assert 0 < int(committed) < total, (mode, committed)
        assert int(stopped[0]) == int(committed), (
            mode, stopped, committed
        )
        assert state["v_sum"] == "n:" + committed, (mode, state)
        assert state["v_branchCount"] == "s:0", (mode, state)
        # The source was disconnected while its warp call was live;
        # if the runtime accidentally proceeds to the parent suffix
        # or root completion this must be caught.
        assert state["v_done"] != "s:1", (mode, state)
        assert not UNSAFE_RESUME.search(trace), (
            mode, "unsafe source was resumed", trace[-3200:]
        )
        assert len(PROMOTION.findall(trace)) >= 1, (
            mode, "no real native OSR before source edit"
        )
        if block_count:
            assert int(state["__block_count"][2:]) < int(
                reference["__block_count"][2:]
            ), (mode, state, reference)
        print(
            f"PASS {mode}: source edge {edge} detached; "
            f"native committed={committed}; "
            f"no stale writes or unsafe Interpreter resume; "
            f"blocks={state.get('__block_count', 'off')}",
            flush=True,
        )


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--harness", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path)
    # A 40k outer loop can finish before async LLVM has compiled
    # the source on loaded shared hosts, particularly in count-off
    # mode. Keep this failure observable and extend the workload.
    p.add_argument("--outer-count", type=int, default=100_000)
    p.add_argument("--without-block-count", action="store_true")
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    project = quad_fixture(
        a.out / "phase4-detach-control-source.sb3",
        outer_count=a.outer_count,
    )
    for late in (False, True):
        run(
            a.harness, project, outer_count=a.outer_count,
            block_count=not a.without_block_count, late=late,
        )


if __name__ == "__main__":
    main()
