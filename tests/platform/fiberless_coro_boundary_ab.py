#!/usr/bin/env python3
"""A/B parity sweep for the source-certified native LLVM stackless coroutine.

Runs each tiny Scratch project once on native Fiber and once on the LLVM
stackless coroutine, validating the *same* Scratch result and no fiber
acquisition. Defaults to using the existing built xyo binary.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

CASES = [
    (0, "1", "0"),
    (1, "1", "1"),
    (2, "1", "2"),
    (3, "1", "3"),
    (8, "1", "8"),
    (64, "1", "64"),
    (100, "1", "100"),
    (1, "-1", "-1"),
    (3, "-1", "-3"),
    (8, "2", "16"),
    (8, "0", "0"),
    (9, "0.5", "4.5"),
]

def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--fixture", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--cpu", type=int)
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.out / "xyo-frozen"
    shutil.copy2(args.binary.resolve(), binary)
    binary.chmod(binary.stat().st_mode | 0o111)
    with zipfile.ZipFile(args.fixture) as f:
        project = json.loads(f.read("project.json"))
    stage = project["targets"][0]
    blocks = stage["blocks"]
    base_env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    base_env.update({
        "XYO_JIT_MODE": "eager",
        "XYO_JIT_OPT_LEVEL": "1",
        "XYO_YIELD_QMODE": "off",
        "XYO_DUMPVARS": "json",
        "XYO_JIT_CORO_DIRECT": "1",
        "XYO_JIT_CORO_SIMPLE_LOOP": "1",
        "XYO_JIT_CORO_TRACE": "1",
        "XYO_JIT_FIBERLESS_STATS": "1",
    })
    if args.cpu is not None and hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, {args.cpu})
    records = []
    for count, delta, expected in CASES:
        blocks["loop"]["inputs"]["TIMES"][1][1] = str(count)
        blocks["inc"]["inputs"]["VALUE"][1][1] = delta
        fixture = args.out / f"repeat-{count}-delta-{delta.replace('.', '_')}.sb3"
        with zipfile.ZipFile(fixture, "w", zipfile.ZIP_DEFLATED) as f:
            f.writestr("project.json", json.dumps(project))
        for stackless in (False, True):
            env = dict(base_env, XYO_JIT_FIBERLESS="1" if stackless else "0")
            proc = subprocess.run(
                [str(binary.resolve()), "run", str(fixture.resolve()),
                 "--fps", "0"],
                capture_output=True, text=True, env=env,
                timeout=35,
            )
            m = re.search(r'"s"\s*:\s*"([^"]+)"', proc.stdout)
            actual = m.group(1) if m else None
            if proc.returncode or actual != expected:
                raise AssertionError(
                    f"case={count},{delta} mode={'coro' if stackless else 'fiber'} "
                    f"exit={proc.returncode} actual={actual!r} expected={expected!r}\n"
                    f"stderr={proc.stderr[-1500:]}\nstdout={proc.stdout[-1500:]}")
            if stackless and not re.search(r"fiber_acquires=0", proc.stderr):
                raise AssertionError(
                    f"native Fiber acquisition in stackless case {count},{delta}: "
                    f"{proc.stderr[-400:]}")
            if stackless and not re.search(r"candidate=1 lowered=1", proc.stderr):
                raise AssertionError(
                    f"LLVM coroutine was not generated for {count},{delta}: "
                    f"{proc.stderr[-1000:]}")
            if stackless and not re.search(r"interpreted_yield_scripts=0", proc.stderr):
                raise AssertionError(
                    f"unexpected interpreter fallback for {count},{delta}: "
                    f"{proc.stderr[-1000:]}")
            records.append({
                "repeat": count, "delta": delta, "mode": "coro" if stackless else "fiber",
                "actual": actual,
                "project_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest()
            })
            print(f"repeat={count} delta={delta} mode="
                  f"{'coro' if stackless else 'fiber'} actual={actual}", flush=True)
    output = {
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "cases": len(CASES),
        "runs": len(records),
        "records": records,
    }
    (args.out / "results.json").write_text(json.dumps(output, indent=2) + "\n")
    print(f"PASS {len(records)} runs, {len(CASES)} paired cases", flush=True)

if __name__ == "__main__":
    main()
