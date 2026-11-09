#!/usr/bin/env python3
"""Scratch string storage A/B on actual generated SB3 projects.

Each project performs a warmup, 3 internally timed invocations, and an
out-of-band correctness check. Subprocess order is shuffled with a fixed
seed, no XYO_* environment variables are inherited, and specialized
transient-loop compilation can be toggled separately.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench import parse_xyo
from gen_algo_bench import (
    Gen, S_add, S_call, S_change, S_delall, S_if, S_repeat,
    S_set, add, driver, eq, join, make_project, mod, num, op,
    text, var, write_sb3,
)

ROOT = Path(__file__).resolve().parents[3]
MODES = ("rope", "flat", "append-log")
CASES = ("append", "read_first", "read_last", "contains",
         "branch", "random_read", "numeric_join", "short_join", "list_contents")


def letter(index, value):
    return op("operator_letter_of", LETTER=index, STRING=value)


def length(value):
    return op("operator_length", STRING=value)


def generate(name, multiplier=1):
    g = Gen(["s", "saved", "fork", "probe", "i", "sum", "hits",
             "t0", "t1", "check"], ["times", "pieces"])
    n = 1024
    outer = 24
    setup = [S_set("s", text("abcdefghijklmnop")),
             S_set("probe", text("")), S_set("sum", num(0)),
             S_set("hits", num(0))]
    pre_timing = []
    verify_suffix = []
    expected_s = ""
    if name in ("append", "read_first", "read_last", "contains", "numeric_join"):
        n = 1024 if name != "contains" else 384
        outer = (24 if name != "contains" else 18) * multiplier
        unit = num(12) if name == "numeric_join" else text("ab")
        inner = [S_set("s", join(var("s"), unit))]
        if name == "read_first":
            inner.append(S_set("probe", letter(num(1), var("s"))))
        if name == "read_last":
            inner.append(S_set("probe", letter(length(var("s")), var("s"))))
        if name == "contains":
            inner.append(S_if(op("operator_contains", STRING1=var("s"),
                                 STRING2=text("ab")), [S_change("hits", num(1))]))
        body = [S_repeat(num(outer), setup + [S_repeat(num(n), inner)])]
        expected_s = "abcdefghijklmnop" + ("12" if name == "numeric_join" else "ab") * n
        if name == "read_first":
            verify_suffix = [eq(var("probe"), text("a"))]
        if name == "read_last":
            verify_suffix = [eq(var("probe"), text("b"))]
        if name == "contains":
            verify_suffix = [eq(var("hits"), num(n))]
    elif name == "branch":
        n, outer = 1400, 18 * multiplier
        body = [S_repeat(num(outer), setup + [S_repeat(num(n), [
            S_set("saved", var("s")),
            S_set("fork", join(var("saved"), text("x"))),
            S_set("s", join(var("saved"), text("y"))),
        ])])]
        expected_s = "abcdefghijklmnop" + "y" * n
        verify_suffix = [eq(var("fork"), text("abcdefghijklmnop" + "y" * (n-1) + "x"))]
    elif name == "random_read":
        long_str = "ab日本" * 1024
        # Construct the long string using real joins. Creating a single long
        # literal would always produce flat storage regardless of the mode.
        # Build once before timing; timed calls access the same immutable value.
        g.define("build_random_string", [], [
            S_set("s", text("")),
            S_repeat(num(1024), [
                S_set("s", join(var("s"), text("ab日本"))),
            ]),
        ], warp=True)
        pre_timing = [S_call("build_random_string")]
        body = [S_set("sum", num(0)), S_set("i", num(0)),
                S_repeat(num(65000 * multiplier), [
                    S_change("i", num(1)),
                    S_set("probe", letter(add(mod(var("i"), num(len(long_str))),
                                               num(1)), var("s"))),
                    S_change("sum", length(var("probe"))),
                ])]
        expected_s = long_str
        verify_suffix = [eq(var("sum"), num(65000 * multiplier))]
    elif name == "short_join":
        body = [S_set("s", text("")), S_set("i", num(0)),
                S_set("sum", num(0)),
                S_repeat(num(80000 * multiplier), [
                    S_change("i", num(1)),
                    S_set("s", join(mod(var("i"), num(10)), text(":"))),
                    S_change("sum", length(var("s"))),
                ])]
        expected_s = "0:"
        verify_suffix = [eq(var("sum"), num(160000 * multiplier))]
    elif name == "list_contents":
        # Operator join is not involved in list contents. Useful as a control.
        body = [S_delall("pieces"), S_set("sum", num(0)),
                S_repeat(num(300), [S_add("pieces", text("A"))]),
                S_repeat(num(450 * multiplier), [
                    S_set("s", op("data_listcontents", {"LIST": "pieces"})),
                    S_change("sum", length(var("s"))),
                ])]
        expected_s = "A" * 300
        verify_suffix = [eq(var("sum"), num(450 * 300 * multiplier))]
    else:
        raise ValueError(name)

    g.define("bench_" + name, [], body, warp=True)
    validation = eq(join(text("_"), var("s")), text("_" + expected_s))
    for expr in verify_suffix:
        validation = op("operator_and", OPERAND1=validation, OPERAND2=expr)
    verify = [S_set("check", text("BAD")),
              S_if(validation, [S_set("check", text("OK"))])]
    g.define("verify", [], verify, warp=True)
    g.script(pre_timing + driver(
        [], [S_call("bench_" + name)], S_call("bench_" + name),
        S_call("verify")))
    return make_project(g.vars.keys(), g.lists.keys(), g)


def run_one(binary, file, mode, jit, transient, cpu=None,
            leaf="utf16", short_intern=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_GPU": "0", "XYO_PAR": "0",
                "XYO_STRING_STORAGE": mode, "XYO_STRING_LEAF": leaf,
                "XYO_STRING_SHORT_INTERN": "1" if short_intern else "0",
                "XYO_JIT_TRANSIENT_STRINGS": "1" if transient else "0"})
    command = [str(binary), "run", str(file), "--jit", jit]
    pin = None
    if cpu is not None and hasattr(os, "sched_setaffinity"):
        pin = lambda: os.sched_setaffinity(0, {cpu})
    start = time.perf_counter_ns()
    p = subprocess.run(command, cwd=ROOT, env=env, text=True,
                       capture_output=True, timeout=150, preexec_fn=pin)
    wall = (time.perf_counter_ns() - start) / 1e6
    if p.returncode:
        raise RuntimeError(f"{file} {mode}/{jit}: exit={p.returncode}\n{p.stderr[-1500:]}")
    parsed = parse_xyo(p.stdout.encode())
    if str(parsed["check"]) != "OK":
        raise RuntimeError(f"{file} {mode}/{jit}: check={parsed['check']!r}")
    return {"times_ms": parsed["times"], "median_ms": statistics.median(parsed["times"]),
            "wall_ms": wall}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, default=ROOT / "build-list-epoch/xyo")
    ap.add_argument("--output", type=Path, default=ROOT / "build-list-epoch/string-storage-study")
    ap.add_argument("--cases", default=",".join(CASES))
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--multiplier", type=int, default=1,
                    help="repeat workloads more times to reduce 1ms clock quantization")
    ap.add_argument("--jit", default="eager", choices=["eager", "off", "hotspot"])
    ap.add_argument("--transient", action="store_true")
    ap.add_argument("--leaf", default="utf16", choices=["utf16", "latin1"])
    ap.add_argument("--short-intern", action="store_true")
    ap.add_argument("--cpu", type=int, default=-1)
    args = ap.parse_args()
    args.binary = args.binary.resolve()
    cases = args.cases.split(",")
    if not set(cases) <= set(CASES):
        ap.error("unknown case")
    if args.reps < 1 or args.multiplier < 1:
        ap.error("reps and multiplier must be >= 1")
    args.output.mkdir(parents=True, exist_ok=True)
    inputs = {}
    for case in cases:
        path = args.output / f"{case}.sb3"
        write_sb3(path, generate(case, args.multiplier))
        inputs[case] = {"path": str(path),
                        "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    results = {"binary": str(args.binary), "config": {
        "jit": args.jit, "transient": args.transient, "leaf": args.leaf,
        "short_intern": args.short_intern,
        "repetitions": args.reps, "cpu": args.cpu,
        "work_multiplier": args.multiplier,
        "scope": "In-Scratch timed code, not cold process/JIT startup",
    }, "inputs": inputs, "rows": {}}
    rng = random.Random(20261107)
    for case in cases:
        samples = {m: [] for m in MODES}
        for rep in range(args.reps):
            order = list(MODES)
            rng.shuffle(order)
            for mode in order:
                measurement = run_one(args.binary, inputs[case]["path"], mode,
                                      args.jit, args.transient,
                                      args.cpu if args.cpu >= 0 else None,
                                      leaf=args.leaf,
                                      short_intern=args.short_intern)
                samples[mode].append(measurement)
        summary = {m: {"median_ms": statistics.median(
            v["median_ms"] for v in samples[m]), "samples": samples[m]}
                   for m in MODES}
        results["rows"][case] = summary
        print(f"{case:14s} " + " ".join(
            f"{m}={summary[m]['median_ms']:.2f}ms" for m in MODES), flush=True)
        (args.output / "results.json").write_text(
            json.dumps(results, indent=2, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()
