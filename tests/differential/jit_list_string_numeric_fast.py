#!/usr/bin/env python3
"""Differential test for numeric-string fast-path on list-item reporters.

Compares JIT (fast path enabled and disabled) against interpreter on real
Scratch list inputs. Also covers mixed lists, invalid indices and mutations.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/differential"))
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from numeric_list import build_case
from gen_algo_bench import write_sb3

CASES = (
    "numeric", "mixed", "mixed_numeric_string", "mixed_bool",
    "mixed_last", "mixed_nonnumeric", "out_of_bounds",
    "fractional_index", "empty", "replace_string",
    "replace_number", "append_string", "clear", "other_script",
)


def run(binary: Path, path: Path, jit: str, fast: bool, direct_miss: bool = False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_LIST_STRING_NUM_FAST": "1" if fast else "0",
        "XYO_JIT_LIST_STRING_MISS_DIRECT": "1" if direct_miss else "0",
        "XYO_DUMPVARS": "json", "XYO_GPU": "0", "XYO_PARFOR": "0",
        "XYO_JIT_MODE": jit,
    })
    p = subprocess.run(
        [str(binary), "run", str(path), "--jit=" + jit],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=60,
    )
    if p.returncode:
        raise RuntimeError(f"{path.name}, {jit}, fast={fast}: {p.stderr[-1000:]}")
    start, end = p.stdout.find("{"), p.stdout.rfind("}")
    if start < 0 or end < start:
        raise RuntimeError(f"missing JSON: {path.name}, {jit}, fast={fast}")
    return json.loads(p.stdout[start:end+1])["Bench"]


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xyo"
    directory = ROOT / "build/life-numeric-string-parity"
    directory.mkdir(exist_ok=True, parents=True)
    for kind in CASES:
        project = directory / f"{kind}.sb3"
        write_sb3(project, build_case(kind))
        baseline = run(binary, project, "off", False)
        for jit in ("eager", "hotspot"):
            old = run(binary, project, jit, False)
            new = run(binary, project, jit, True)
            cold = run(binary, project, jit, True, direct_miss=True)
            if (old != new or new != cold or
                    (kind != "other_script" and new != baseline)):
                raise AssertionError((kind, jit, baseline, old, new, cold))
        print(kind, "PASS", flush=True)
    print(f"PASS {len(CASES)} Scratch projects, interpreter/eager/hotspot, fast ON/OFF, cold ON/OFF", flush=True)


if __name__ == "__main__":
    main()
