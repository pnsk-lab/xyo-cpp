#!/usr/bin/env python3
"""RT-001: generate and run the actual suspended native varslot editor smoke.

The C++ harness edits the Target::variables map while a live LLVM Fiber is
suspended, then proves subsequent compiled instructions update the new node.
"""
import argparse
import os
from pathlib import Path
import subprocess

from native_simple_resume import fixture


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--smoke", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--fast", action="store_true")
    parser.add_argument("--indexed", action="store_true")
    parser.add_argument("--stage-shadow", action="store_true")
    parser.add_argument("--stage-global", action="store_true")
    args = parser.parse_args()

    if args.indexed and not args.fast:
        parser.error("--indexed requires --fast")
    if args.stage_shadow and args.stage_global:
        parser.error("choose only one owner transition")
    source = fixture(args.out, "varslot_guard_editor_mutation")
    env = dict(os.environ)
    env["XYO_RT_GUARD_NATIVE_VAR_SLOTS_FAST"] = (
        "1" if args.fast else "0"
    )
    env["XYO_RT_GUARD_NATIVE_VAR_SLOTS_INDEXED"] = (
        "1" if args.indexed else "0"
    )
    env["XYO_JIT_VAR_GUARD_INLINE_TRACE"] = (
        "1" if args.indexed and
        env.get("XYO_JIT_VAR_GUARD_INLINE") == "1" else "0"
    )
    env["XYO_RT001_VARSLOT_EDITOR_STAGE_SHADOW"] = (
        "1" if args.stage_shadow else "0"
    )
    env["XYO_RT001_VARSLOT_EDITOR_STAGE_GLOBAL"] = (
        "1" if args.stage_global else "0"
    )
    completed = subprocess.run(
        [str(args.smoke.resolve()), str(source.resolve())],
        env=env, text=True,
        capture_output=True,
        timeout=120,
    )
    if completed.returncode:
        raise RuntimeError(
            f"native varslot editor smoke failed ({completed.returncode})\n"
            f"{completed.stdout}\n{completed.stderr}"
        )
    if "PASS RT-001 native LLVM Fiber editor mutation" not in completed.stdout:
        raise AssertionError(
            "test did not verify the resumed native Fiber:\n"
            f"{completed.stdout}\n{completed.stderr}"
        )
    print(completed.stdout.strip())
    print(completed.stderr.strip())


if __name__ == "__main__":
    main()
