#!/usr/bin/env python3
"""IR-001: actual O2/O3 post-optimizer LLVM effect-write audit & fail-closed.

The compiler audit is deliberately not a source-state/deopt certificate.
It should preserve Scratch state while denying an IR-001 diagnostic loop
checkpoint when optimized IR contains unaccounted stores or opaque calls.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/differential"))
from ir001_effect_source_trace import CHECKPOINT, repeat_fixture, run


AUDIT = re.compile(
    r"\[ir001-postopt-write\] tier=(\w+) events=(\d+) "
    r"checkpoints=(\d+) barriers=(\d+) writes=(\d+) "
    r"unguarded=(\d+) unguarded_opaque=(\d+) proven_local_calls=(\d+) "
    r"local_stack=(\d+) isolated_stack=(\d+) "
    r"unguarded_isolated_stack=(\d+) "
    r"checkpoint_only_functions=(\d+) fenced=(\d+) executable=(\d)"
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    project = repeat_fixture(args.out / "ir001-postopt-audit-loop.sb3")
    keys = {
        "XYO_JIT_ASYNC": "0",
        "XYO_JIT_CALLFRAME": "1",
        "XYO_RT_ROOT_ONLY_DEOPT": "1",
        "XYO_IR001_OPT_WRITE_AUDIT": "0",
        "XYO_JIT_OPT_LEVEL": "2",
    }
    original = {k: os.environ.get(k) for k in keys}
    try:
        os.environ.update(keys)
        for tier in ("2", "3"):
            os.environ["XYO_JIT_OPT_LEVEL"] = tier
            os.environ["XYO_IR001_OPT_WRITE_AUDIT"] = "0"
            baseline, stats, unmodified_log = run(
                args.native, project, journal=True, trace=True)
            assert baseline.get("x") == "33" and baseline.get("done") == "1"
            before = CHECKPOINT.findall(unmodified_log)
            assert len(before) == 3 and all(
                row[3] == "1" and row[9] == "0" for row in before), before

            os.environ["XYO_IR001_OPT_WRITE_AUDIT"] = "1"
            audited, audited_stats, diagnostic = run(
                args.native, project, journal=True, trace=True)
            assert audited == baseline and audited_stats == stats
            reports = AUDIT.findall(diagnostic)
            assert reports, diagnostic[-3500:]
            assert all(row[-1] == "0" for row in reports), reports
            assert sum(int(row[1]) for row in reports) >= 1, reports
            assert sum(int(row[12]) for row in reports) >= 1, reports
            assert sum(int(row[5]) + int(row[6]) for row in reports) >= 1
            assert all(int(row[10]) <= int(row[9]) <= int(row[8])
                       for row in reports), reports
            after = CHECKPOINT.findall(diagnostic)
            assert len(after) == 3, diagnostic[-3500:]
            assert all(
                row[3] == "0" and row[7] == "1" and row[8] == "0"
                and row[9] == "1" for row in after), after
            print(
                f"PASS native LLVM O{tier} postopt audit: "
                f"{sum(int(row[12]) for row in reports)} fenced checkpoint(s), "
                f"{sum(int(row[9]) for row in reports)} isolated stack store(s), "
                "Scratch parity retained", flush=True)

    finally:
        for key, value in original.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


if __name__ == "__main__":
    main()
