#!/usr/bin/env python3
"""True post-native-yield in-flight source mutation with bounded deopt.

The native LLVM Fiber MUST already have executed committed Scratch effects
and returned control through an actual warp scheduler yield. C++ mutates
an already-executed source prefix WITHOUT resetCache at that safe point.
With the in-flight guard enabled, the Fiber must be abandoned (no more LLVM
instructions) and its original interpreter counted-repeat state restored.

All original GC-rooted arguments, suffixes, loop counters and the complete
Scratch variable state must match plain Interpreter execution.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
from jit_nested_warp_parent_repeat_osr import fixture as parent_fixture

PROMOTION = re.compile(r"\[jit-osr-nested\] promoted ")
RESTORE = re.compile(r"\[jit-osr-nested\] restored caller")
PROOF = re.compile(r"\[jit-osr-source-proof\] calls=")
DEOPT = re.compile(
    r"\[jit-osr-inflight\] deopt remaining=([0-9.]+) "
    r"source-changed=1"
)
YIELD = re.compile(r"\[osr-inflight\] edited while suspended remaining=")

EDIT = re.compile(
    r"\[osr-silent-edit\] injected block=\S+ "
    r"revision-before=(\d+) revision-after=(\d+) "
    r"digest-changed=1 tested-proof=([01])"
)


def execute(harness: Path, project: Path, mode: str):
    cmd = [str(harness.resolve()), str(project.resolve()), mode]
    run = subprocess.run(
        cmd, cwd=ROOT, capture_output=True, text=True, timeout=210,
    )
    assert run.returncode == 0, (
        mode, run.returncode, run.stdout[-1600:], run.stderr[-3000:]
    )
    states = {}
    for line in run.stdout.splitlines():
        if line.startswith("STATE "):
            key, value = line[6:].split("=", 1)
            assert key not in states, line
            states[key] = value
    assert len(states) >= 10, (mode, states, run.stdout)
    return states, run.stderr


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--harness", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--iterations", type=int, default=8_000_000)
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    project = parent_fixture(
        args.out / "source-edited-after-compile.sb3",
        parent_count=3, inner_iterations=args.iterations, gc=True,
    )

    baseline, _ = execute(args.harness, project, "baseline")
    assert baseline["v_sum"] == f"n:{6 * args.iterations}", baseline
    assert baseline["v_entered"] == "n:3", baseline
    assert baseline["v_afterInner"] == "n:3", baseline
    assert baseline["v_done"] == "s:1", baseline
    assert baseline["v_times"] == "s:17", baseline

    for mode in ("positive", "unguarded", "guarded", "versioned"):
        values, stderr = execute(args.harness, project, mode)
        assert values == baseline, (mode, values, baseline)
        promotions = len(PROMOTION.findall(stderr))
        restorations = len(RESTORE.findall(stderr))
        proofs = len(PROOF.findall(stderr))
        edits = EDIT.findall(stderr)
        deopts = DEOPT.findall(stderr)
        assert promotions > 0, (mode, stderr[-2400:])
        if mode == "positive":
            assert restorations == promotions == proofs, (
                mode, promotions, restorations, proofs, stderr[-2400:]
            )
            assert not edits and not deopts, stderr[-1200:]
        elif mode == "unguarded":
            assert restorations == promotions == proofs, (
                mode, promotions, restorations, proofs, stderr[-2400:]
            )
            assert len(edits) == 1 and not deopts, stderr[-2000:]
        else:
            assert len(deopts) == 1 and float(deopts[0]) >= 0, (
                mode, deopts, stderr[-2500:]
            )
            # The native Fiber was abandoned instead of returning through
            # the normal completed-loop successor, so it is never counted
            # as a regular restored-caller native completion.
            assert restorations < promotions, (
                mode, promotions, restorations, stderr[-2500:]
            )
            assert len(edits) == 1 and YIELD.search(stderr), stderr[-1800:]
        if mode != "positive":
            before, after, proof_flag = edits[0]
            assert proof_flag == "1", edits
            assert (before != after) == (mode == "versioned"), edits
        print(
            f"PASS mode={mode} native-promotions={promotions} "
            f"restores={restorations} proof={proofs} edits={len(edits)} "
            f"post-yield-deopts={len(deopts)} all-variables-identical",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
