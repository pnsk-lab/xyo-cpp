#!/usr/bin/env python3
"""Validate the opt-in, final outer-loop variable copy in Mega LLVM."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

from bench_mega_outer_until import fixture, block, graft_real_bench_round

ROOT = Path(__file__).resolve().parents[3]
BINARY = ROOT / "build-scheduler-rewrite-20261111/xyo"

def project(outer_count=3):
    p = fixture(outer_count, -1.0, post_measure=True)
    stage = p["targets"][0]
    blocks = stage["blocks"]
    blocks["final"] = block(
        "data_setvariableto",
        inputs={"VALUE": [2, "finalReader"]},
        fields={"VARIABLE": ["b_chk", "b_chk"]})
    blocks["finalReader"] = block(
        "data_variable", fields={"VARIABLE": ["b_n", "b_n"]})
    stage["variables"]["b_chk"] = ["b_chk", -7]
    return p

def run(p, enabled, label, directory):
    path = directory / (label + ".sb3")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as out:
        out.writestr("project.json", json.dumps(p))
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_MODE": "eager",
        "XYO_PAR": "0",
        "XYO_YIELD_QMODE": "off",
        "XYO_JIT_MEGA_BATCH_YIELD": "1",
        "XYO_JIT_MEGA_SINGLE_SENDER": "1",
        "XYO_JIT_MEGA_UNTIL_BCAST": "1",
        "XYO_JIT_MEGA_UNTIL_OUTER": "1",
        "XYO_JIT_MEGA_UNTIL_POST_SET": "1",
        "XYO_JIT_MEGA_UNTIL_OUTER_POST_SET": "1" if enabled else "0",
        "XYO_JIT_MEGA_UNTIL_BENCH_ROUND": "1",
        "XYO_JIT_MEGA_NORMALIZE_YIELD": "1",
        "XYO_DUMPVARS": "json",
        "XYO_JIT_MEGA_BATCH_TRACE": "1",
    })
    process = subprocess.run(
        [str(BINARY), "run", str(path), "--fps", "0"],
        cwd=directory, env=env, capture_output=True, text=True, timeout=30)
    if process.returncode:
        raise AssertionError((label, process.returncode, process.stderr[-1000:]))
    match = re.search(r'\{\s*"Stage"\s*:\s*\{.*?\}\s*\}', process.stdout, re.S)
    assert match, (label, process.stdout[-1000:])
    values = json.loads(match.group())["Stage"]
    return values, "[mega-yield]" in process.stderr

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="Original sched-8.sb3")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mega-outer-copy-") as root:
        directory = Path(root)
        for n in (0, 1, 3):
            source = project(n)
            results = []
            for on in (False, True):
                values, installed = run(source, on, f"copy-{n}-{on}", directory)
                assert installed == on, (n, on, installed)
                expected = -1 if n == 0 else 0
                assert float(values["b_chk"]) == expected, (n, on, values)
                assert float(values["sum"]) == n * 10, (n, on, values)
                results.append((float(values["b_chk"]), float(values["sum"])))
            assert results[0] == results[1], (n, results)
            print(f"PASS copy {n} repeats: Fiber/Mega parity", flush=True)
        if args.source:
            source = project(2)
            graft_real_bench_round(source, args.source)
            stage = source["targets"][0]
            stage["variables"]["b_n"][1] = 0
            for enabled in (False, True):
                values, installed = run(source, enabled, f"combined-{enabled}", directory)
                assert installed == enabled, (enabled, installed)
                assert float(values["b_chk"]) == float(values["b_n"]), values
                assert float(values["b_round"]) == 2, values
                assert float(values["sum"]) == 0, values
            print("PASS original bench_round + outer final setter", flush=True)
        source = project()
        blocks = source["targets"][0]["blocks"]
        blocks["final"]["next"] = "extra"
        blocks["extra"] = block(
            "data_changevariableby",
            inputs={"VALUE": [1, [4, "10"]]},
            fields={"VARIABLE": ["sum", "sum"]})
        values, installed = run(source, True, "reject-extra", directory)
        assert not installed and float(values["sum"]) == 40, (installed, values)
        print("PASS unsupported following statement falls back to Fiber", flush=True)
        source = project()
        source["targets"][0]["blocks"]["finalReader"]["opcode"] = "sensing_dayssince2000"
        values, installed = run(source, True, "reject-reporter", directory)
        assert not installed
        print("PASS unexpected reporter falls back to Fiber", flush=True)

if __name__ == "__main__":
    main()
