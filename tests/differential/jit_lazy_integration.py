#!/usr/bin/env python3
"""End-to-end checks of released HotSpot/eager procedure materialization policies."""
from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zipfile
import json

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench" / "algos"))
from jit_lazy_procedure_fixture import make_project  # noqa: E402

def run_case(binary: Path, project: Path, jit: str, lazy: str,
             expected_tier: str, expected_value: str) -> None:
    env = os.environ.copy()
    env.update({
        "XYO_JIT_TIMING": "1",
        "XYO_HOT_INVOCATIONS": "1",
        "XYO_SEED": "1",
        "XYO_PAR": "0",
    })
    # Explicit CLI controls must override the inherited preview environment.
    env["XYO_JIT_LAZY_MODE"] = "off" if lazy != "off" else "on"
    args = [str(binary), "run", str(project),
            "--jit", jit, "--jit-lazy", lazy,
            "--print-list", "output"]
    result = subprocess.run(args, env=env, text=True, capture_output=True,
                            timeout=70, check=False)
    label = f"{jit}/{lazy}/{project.stem}"
    if result.returncode != 0:
        raise AssertionError(
            f"{label}: exited {result.returncode}\n{result.stderr[-1800:]}")
    actual_value = result.stdout.strip().splitlines()[-1:]
    if actual_value != [expected_value]:
        raise AssertionError(
            f"{label}: expected {expected_value!r}, got {actual_value!r}")
    tiers = re.findall(r"\[jit-timing\].*?\btier=(\w+)", result.stderr)
    if expected_tier not in tiers:
        raise AssertionError(
            f"{label}: expected tier {expected_tier!r}, got {tiers!r}")
    print(f"PASS {label}: {expected_value} (tier={expected_tier})")

def main() -> int:
    if len(sys.argv) != 2:
        print("usage: jit_lazy_integration.py /path/to/xyo", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xyo-jit-lazy-") as temp:
        for gate, expected in ((0, "1"), (1, "2521")):
            path = Path(temp) / f"gate{gate}.sb3"
            with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
                z.writestr("project.json", json.dumps(make_project(60, 18, gate)))
            for policy, tier in (("auto", "lazy"), ("on", "lazy"), ("off", "eager")):
                run_case(binary, path, "hotspot", policy, tier, expected)
            # Eager must keep the classic JIT strategy unless explicitly opted in.
            run_case(binary, path, "eager", "auto", "eager", expected)
        # Many unconditional call sites should select eager, even though
        # the project contains the same 60 additional procedure definitions.
        direct = Path(temp) / "many_direct_calls.sb3"
        hot_calls = 24
        with zipfile.ZipFile(direct, "w", compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr("project.json",
                       json.dumps(make_project(60, 18, 0, hot_calls)))
        value = 0
        for _ in range(hot_calls):
            value += value % 7 + 1
        run_case(binary, direct, "hotspot", "auto", "eager", str(value))
        run_case(binary, direct, "hotspot", "on", "lazy", str(value))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
