#!/usr/bin/env python3
"""Real live no-revision source-edit rejection of async nested-warp LLVM OSR.

The C++ harness controls Runtime::step and mutates an already-executed root
assignment AFTER the exact compiled nested OSR becomes ready, but BEFORE the
next Runtime::step can adopt it. It intentionally does NOT call resetCache.
No production test hook, fake image, or fabricated source revision is used.

Controls:
  baseline:  Interpreter, no mutation
  positive:  proof ON, no mutation -> genuine native promotion and return
  unguarded: proof OFF, silent source edit -> native promotion is possible
  guarded:   proof ON, SAME silent source edit -> no stale native promotion
  versioned: proof ON, edit + version bump -> no stale native promotion
All Scratch variable values must be exactly identical.
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
        parent_count=3, inner_iterations=args.iterations,
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
        assert restorations == promotions, (mode, stderr[-2400:])

        if mode == "positive":
            assert promotions > 0 and proofs == promotions, (
                mode, promotions, proofs, stderr[-2600:]
            )
            assert not edits, stderr[-2600:]
        elif mode == "unguarded":
            assert promotions > 0 and proofs == 0, (
                mode, promotions, proofs, stderr[-2600:]
            )
        else:
            assert promotions == proofs == 0, (
                mode, promotions, proofs, stderr[-2600:]
            )

        if mode != "positive":
            assert len(edits) == 1, (
                mode, edits, stderr[-2600:]
            )
            before, after, proof_flag = edits[0]
            assert proof_flag == ("0" if mode == "unguarded" else "1")
            if mode == "versioned":
                assert before != after, edits
            else:
                assert before == after, edits
        print(
            f"PASS mode={mode} native-promotions={promotions} "
            f"restores={restorations} proof={proofs} edits={len(edits)} "
            "all-variables-identical",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
