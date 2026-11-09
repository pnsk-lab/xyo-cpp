#!/usr/bin/env python3
"""Scheduler-side native LLVM CallFrame export at real Fiber yields.

The image has independent GC roots and immutable source-return symbols.
It is validated here as DATA ONLY: arbitrary SSA/effect resume remains
unsupported, so the original native Fiber must continue normally.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from native_callframe_mirror import fixture, execute

ROOT = Path(__file__).resolve().parents[2]
CASES = (
    "nonwarp_string",
    "string_recursive",
    "loop_recursive",
    "branch_return",
    "empty_string_arg",
)
PROBE = re.compile(
    r"\[source-handoff\] script=\S+ valid=1 "
    r"depth=(\d+) roots=(\d+) revision=(\d+)"
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    native = args.native.resolve()

    for case in CASES:
        project = fixture(args.out, case)
        expected, _, _ = execute(native, project, "off", False)
        env = dict(
            os.environ,
            XYO_JIT_COMMON_CFG="0",
            XYO_JIT_CALLFRAME="1",
            XYO_RT_SOURCE_HANDOFF_PROBE="1",
            XYO_JIT_VERBOSE="1",
            XYO_DUMPVARS="json",
            XYO_EXIT_ON_DONE="1",
            XYO_PAR="0",
        )
        proc = subprocess.run(
            [str(native), "run", str(project), "--jit", "eager",
             "--fps", "30", "--frames", "200"],
            cwd=ROOT, env=env, capture_output=True, text=True, timeout=120,
        )
        if proc.returncode:
            raise AssertionError(f"{case}: native failure: {proc.stderr[-1600:]}")
        match = re.search(r'(?s)(\{\s*"Bench".*\})', proc.stdout)
        if not match:
            raise AssertionError(f"{case}: missing Scratch vars: {proc.stdout[-500:]}")
        actual = match.group(1)
        if json.loads(expected) != json.loads(actual):
            raise AssertionError(f"{case}: interpreter/native mismatch\n{expected}\n{actual}")

        handoffs = [tuple(map(int, m.groups())) for m in PROBE.finditer(proc.stderr)]
        if not handoffs or "[source-handoff]" not in proc.stderr:
            raise AssertionError(f"{case}: scheduler did not export any frames")
        if "[source-handoff]" in proc.stderr and "valid=0" in proc.stderr:
            raise AssertionError(f"{case}: scheduler exported an invalid image")
        # The enclosing script can yield after its last procedure returns.
        # A root-only frame at that point is valid; procedure yields must
        # still preserve their full call ancestry.
        if any(depth < 1 or revision <= 0 for depth, _, revision in handoffs):
            raise AssertionError(f"{case}: invalid source image metadata: {handoffs[:5]}")
        max_depth = max(h[0] for h in handoffs)
        if max_depth < 2:
            raise AssertionError(f"{case}: no active procedure captured")
        max_roots = max(h[1] for h in handoffs)
        if case in ("nonwarp_string", "string_recursive") and max_roots == 0:
            raise AssertionError(f"{case}: GC-rooted string arguments missing")
        if case in ("string_recursive", "loop_recursive") and max_depth < 3:
            raise AssertionError(f"{case}: recursive frames missing")
        print(
            f"PASS {case}: scheduler native handoffs={len(handoffs)} "
            f"max_depth={max_depth} rooted_args={max_roots}",
            flush=True,
        )
    print(f"PASS {len(CASES)} scheduler-owned GC-rooted native handoffs", flush=True)


if __name__ == "__main__":
    main()
