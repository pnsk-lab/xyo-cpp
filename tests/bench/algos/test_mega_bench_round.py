#!/usr/bin/env python3
"""Integration tests for the exact original ScratchBench bench_round AST.

Requires the actual .sb3 input, not a manually paraphrased benchmark body.
Executes the same project in native Fiber and source-proven LLVM modes,
checks stable Scratch variable effects, and verifies that mutated source
trees are rejected rather than silently skipping unknown operations.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

from bench_mega_outer_until import fixture, graft_real_bench_round


def check(ok: bool, message: str) -> None:
    if not ok:
        raise AssertionError(message)
    print(f"PASS: {message}", flush=True)


def run_project(binary: Path, root: Path, project: dict, label: str,
                enabled: bool) -> tuple[dict, bool]:
    path = root / f"{label}.sb3"
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("project.json", json.dumps(project))
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
        "XYO_JIT_MEGA_UNTIL_BENCH_ROUND": "1" if enabled else "0",
        "XYO_JIT_MEGA_NORMALIZE_YIELD": "1",
        "XYO_DUMPVARS": "json",
        "XYO_JIT_MEGA_BATCH_TRACE": "1",
    })
    proc = subprocess.run([str(binary), "run", str(path), "--fps", "0"],
                          cwd=root, env=env, capture_output=True,
                          text=True, timeout=60)
    (root / f"{label}.out").write_text(proc.stdout)
    (root / f"{label}.err").write_text(proc.stderr)
    if proc.returncode != 0:
        raise AssertionError(
            f"{label}: exit={proc.returncode}: {proc.stderr[-2000:]}")
    match = re.search(r'\{\s*"Stage"\s*:\s*\{.*?\}\s*\}', proc.stdout, re.S)
    if not match:
        raise AssertionError(f"{label}: missing Stage vars: {proc.stdout[-1000:]}")
    values = json.loads(match.group())["Stage"]
    return values, "[mega-yield]" in proc.stderr


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cpu", type=int, default=16)
    args = parser.parse_args()
    os.sched_setaffinity(0, {args.cpu})
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()

    def make(outer: int, budget: float, round_initial: int = 0) -> dict:
        proj = fixture(outer, budget, post_measure=True)
        graft_real_bench_round(proj, args.source)
        proj["targets"][0]["variables"]["b_round"][1] = round_initial
        return proj

    for case, repetitions, budget, initial_round in [
        ("normal", 3, 14.0, 0),
        ("first-round-only", 1, 14.0, 0),
        ("expired", 3, -1.0, 0),
        ("zero-repeat", 0, 14.0, 0),
        ("initial-round-positive", 2, 14.0, 1),
    ]:
        project = make(repetitions, budget, initial_round)
        stable = []
        for on in [False, True]:
            label = f"{case}-{'mega' if on else 'fiber'}"
            data, installed = run_project(binary, out, project, label, on)
            check(installed == on, f"{label}: exact source selects proper executor")
            check(int(float(data["b_round"])) == initial_round + repetitions,
                  f"{label}: round increment exactly once per outer iteration")
            check(float(data["sum"]) == 100,
                  f"{label}: no original Scratch side effects omitted")
            if repetitions > 0:
                check(float(data["b_n"]) == float(data["hits"]),
                      f"{label}: last snapshot matches receiver-updated counter")
            else:
                check(float(data["b_n"]) == -1,
                      f"{label}: zero-trip outer loop leaves snapshot untouched")
            check(float(data["b_chunks"]) >= 0,
                  f"{label}: chunk aggregate remains valid")
            if budget < 0 or repetitions == 0:
                check(float(data["total"]) == 0,
                      f"{label}: no receiver is launched for expired/zero condition")
                check(float(data["b_chunks"]) == 0,
                      f"{label}: no phantom chunks")
            if repetitions == 1 and initial_round == 0:
                check(float(data["b_total"]) == 0 and
                      float(data["b_chunks"]) == 0 and
                      float(data["b_bestrate"]) == 0,
                      f"{label}: first round excludes conditional aggregations")
            if initial_round > 0 and budget > 0:
                check(float(data["b_chunks"]) > 0,
                      f"{label}: positive round processes guarded aggregations")
            stable.append({
                key: float(data[key])
                for key in ("sum", "b_round", "b_chunks")
                if budget < 0 or repetitions <= 1
            })
        if budget < 0 or repetitions <= 1:
            # For time-bounded normal cases counts vary naturally; exact
            # equality is only meaningful when there are no broadcasts.
            check(stable[0] == stable[1],
                  f"{case}: fixed-effect Fiber and Mega results are identical")

    mutated = make(3, -1)
    nodes = mutated["targets"][0]["blocks"]
    ms_multiply = next(
        node for key, node in nodes.items()
        if key.startswith("~") and node["opcode"] == "operator_multiply")
    ms_multiply["inputs"]["NUM2"][1][1] = "86400001"
    changed, installed = run_project(
        binary, out, mutated, "rejected-multiplier", True)
    check(not installed,
          "unsupported bench_round arithmetic is rejected by source proof")
    check(float(changed["b_round"]) == 3 and
          float(changed["sum"]) == 100,
          "fallback runs entire non-certified procedure without omission")

    mutation = make(3, -1)
    nodes = mutation["targets"][0]["blocks"]
    round_increment = next(
        node for key, node in nodes.items()
        if key.startswith("~") and
        node["opcode"] == "data_changevariableby" and
        node.get("fields", {}).get("VARIABLE", [""])[0] == "b_round")
    round_increment["next"] = "additionalEffect"
    nodes["additionalEffect"] = {
        "opcode": "data_changevariableby", "next": None,
        "inputs": {"VALUE": [1, [4, "1000"]]},
        "fields": {"VARIABLE": ["sum", "sum"]},
    }
    changed, installed = run_project(
        binary, out, mutation, "rejected-extra-effect", True)
    check(not installed,
          "additional post-procedure effect rejects Mega specialization")
    check(float(changed["b_round"]) == 3 and
          float(changed["sum"]) == 3100,
          "native Fiber executes the extra effect three times")

    print("All exact real-bench-round source and effect tests PASS.")


if __name__ == "__main__":
    main()
