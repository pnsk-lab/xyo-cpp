#!/usr/bin/env python3
"""Regression + A/B timings for Unicode string comparisons in real Scratch SB3.

Run: python3 tests/differential/string_unicode_fast.py [path/to/xyo] [--bench]
--bench pins a CPU (on supported OS) and reports 7 random-order A/B medians.
"""
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_repeat, S_set,
    eq, make_project, num, op, text, var, write_sb3,
)

CASES = {
    "japanese": (
        "漢" * 400 + "東京の天気",
        "東京",
        "漢" * 400 + "東京の天気",
    ),
    "mixed": (
        "漢" * 400 + "東京HELLO",
        "hello",
        "漢" * 400 + "東京hello",
    ),
    "unicode_case": (
        "KΟΣİstanbul東京",
        "kοσ",
        "kοσi\u0307stanbul東京",
    ),
    "emoji": (
        "今日🍣は東京😀です",
        "東京😀",
        "今日🍣は東京😀です",
    ),
}


def make_project_for(source: str, needle: str, expected: str, count: int):
    g = Gen(["source", "needle", "target", "matches", "equals", "iterations"], [])
    body = [
        S_repeat(num(count), [
            S_if(op("operator_contains",
                    STRING1=var("source"), STRING2=var("needle")),
                 [S_change("matches", num(1))]),
            S_if(eq(var("source"), var("target")),
                 [S_change("equals", num(1))]),
            S_change("iterations", num(1)),
        ])
    ]
    g.define("stringloop", [], body, warp=True)
    g.script([
        S_set("source", text(source)),
        S_set("needle", text(needle)),
        S_set("target", text(expected)),
        S_call("stringloop", []),
    ])
    return make_project(g.vars.keys(), g.lists.keys(), g)


def run(binary: Path, sb3: Path, jit: str, fast: bool):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_STRING_UNICODE_FAST": "1" if fast else "0",
        "XYO_GPU": "0", "XYO_PARFOR": "0", "XYO_PAR": "0",
        "XYO_DUMPVARS": "json",
    })
    start = time.perf_counter()
    p = subprocess.run(
        [str(binary), "run", str(sb3), "--jit", jit, "--frames", "500"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=60,
    )
    elapsed_ms = (time.perf_counter() - start) * 1000
    if p.returncode != 0:
        raise RuntimeError(f"{sb3.name}/{jit}/{fast}: {p.stderr[-1000:]} {p.stdout[-500:]}")
    text_out = p.stdout
    result = json.loads(text_out[text_out.index("{"):text_out.rindex("}") + 1])["Bench"]
    return result, elapsed_ms


def main():
    benchmark = "--bench" in sys.argv[1:]
    paths = [arg for arg in sys.argv[1:] if arg != "--bench"]
    binary = Path(paths[0]).resolve() if paths else ROOT / "build/xyo"
    out = ROOT / "build/string-unicode-fixtures"
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(1618)
    if benchmark and hasattr(os, "sched_getaffinity"):
        cpu = min(os.sched_getaffinity(0))
        try:
            os.sched_setaffinity(0, {cpu})
            print("Pinned to CPU", cpu, flush=True)
        except (AttributeError, OSError):
            pass

    for name, (source, needle, expected) in CASES.items():
        sb3 = out / f"{name}.sb3"
        write_sb3(sb3, make_project_for(source, needle, expected, 1000))
        for tier in ("off", "eager", "hotspot"):
            samples = {False: [], True: []}
            expected_result = None
            rounds = 7 if benchmark else 1
            for _ in range(rounds):
                order = [False, True]
                if benchmark:
                    rng.shuffle(order)
                for enabled in order:
                    result, elapsed = run(binary, sb3, tier, fast=enabled)
                    if expected_result is None:
                        expected_result = result
                    assert result == expected_result, (name, tier, enabled)
                    assert result["iterations"] == "1000", (name, tier, result)
                    samples[enabled].append(elapsed)
            old = statistics.median(samples[False])
            new = statistics.median(samples[True])
            print(f"{name:13s} {tier:7s} PASS old={old:.2f}ms "
                  f"new={new:.2f}ms ratio={old/new:.2f}x "
                  f"matches={expected_result['matches']} "
                  f"equals={expected_result['equals']}", flush=True)

if __name__ == "__main__":
    main()
