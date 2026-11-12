#!/usr/bin/env python3
"""Exercise native LLVM ORC lazy materialization with the large code model.

This is a CPU-only CTest; it does not need NVIDIA NGX or OpenGL.
Both HotSpot and Eager must actually choose the ORC lazy tier and
run the 148-assertion calculator Scratch fixture to completion.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument(
        "--code-model", choices=("large", "auto"), default="large",
        help="large: force Large; auto: exercise compiler default (ASan auto-Large)")
    parser.add_argument(
        "--project", type=Path,
        default=Path(__file__).resolve().parents[1] /
                "fixtures" / "calculator_tests.sb3",
    )
    args = parser.parse_args()
    env = os.environ.copy()
    env.pop("XYO_JIT_DISABLE", None)
    env.pop("XYO_JIT_MODE", None)
    if args.code_model == "large":
        env["XYO_JIT_CODE_MODEL"] = "large"
    else:
        env.pop("XYO_JIT_CODE_MODEL", None)
    env["XYO_JIT_COMPILE_STATS"] = "1"
    env["XYO_JIT_TIMING"] = "1"

    for mode in ("hotspot", "eager"):
        cmd = [
            str(args.binary.resolve()), "test", str(args.project.resolve()),
            "--jit", mode, "--jit-lazy", "on",
        ]
        result = subprocess.run(
            cmd, env=env, capture_output=True, text=True,
            check=False, timeout=80,
        )
        if (
            result.returncode or
            not re.search(r"assertions: 148 pass, 0 fail", result.stdout) or
            "tier=lazy" not in result.stderr or
            "[jit-compile] sync attempts=1 success=1" not in result.stderr
        ):
            raise RuntimeError(
                f"Large-model ORC lazy {mode} failed: exit={result.returncode}"
                f"\n{result.stdout[-500:]}\n{result.stderr[-2500:]}"
            )
        print(
            f"{mode}: 148/148 Scratch assertions, ORC lazy, "
            f"code_model={args.code_model}",
            flush=True,
        )
    print(
        f"PASS: ORC {args.code_model} code model across HotSpot and Eager"
    )


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
