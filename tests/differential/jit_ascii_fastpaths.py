#!/usr/bin/env python3
"""Compare punctuation equality and generic ASCII-set contains across JIT paths.

Run: python3 tests/differential/jit_ascii_fastpaths.py [path/to/xyo]
The test intentionally exercises heap strings, non-strings, empty strings,
Unicode folds and the specialized constant-ASCII-literal paths.
"""
import json
import os
import random
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_add, S_call, S_change, S_delall, S_if, S_set, S_until,
    add, arg, eq, gt, item, length, make_project,
    num, op, sub, text, var, write_sb3,
)


def build_fixture(out):
    sets = {
        "digits": "0123456789",
        "vowels": "aeiou",
        "operators": "+-<>[]",
        "alternating": "02468",
        "uppercase": "ABCD",
        "punctuation": "!?@#",
        "single": "z",
        "empty_set": "",
        "unicode_set": "日本語",
        "large_set": "0123456789abcdefghijklmnopqrstuvwx",
    }
    # Holdout alphabets deliberately unrelated to benchmark literals. The
    # generated sets vary in cardinality, ordering, and upper/lowercase.
    rng = random.Random(20261105)
    alphabet = "".join(chr(i) for i in range(33, 127))
    for i, size in enumerate((2, 5, 9, 20, 32)):
        sets[f"holdout_{i}"] = "".join(rng.sample(alphabet, size))
    g = Gen(["candidate", "eq_forward", "eq_reverse", "iterations",
             *sets.keys()], [])
    cases = [
        *(text(c) for c in ["+", "-", "<", ">", "[", "]", "a", "A", "N", "none",
                             "0", "5", "", " ", "++", "01", "＋", "K", "İ", "🍣",
                             "i", "I", "o", "O", "z", "Z", "@", "#", "8",
                             "b", "D", "日", "日本", "á", "ß"]),
        num(5), num(0), num(-1), num(1000),
    ]
    body = []
    for item in cases:
        body.append(S_set("candidate", item))
        for literal in ["+", "-", "<", ">", "[", "]", "n", "none"]:
            body.extend([
                S_if(eq(var("candidate"), text(literal)),
                     [S_change("eq_forward", num(1))]),
                S_if(eq(text(literal), var("candidate")),
                     [S_change("eq_reverse", num(1))]),
            ])
        for key, hay in sets.items():
            body.append(S_if(op("operator_contains", STRING1=text(hay),
                                STRING2=var("candidate")),
                             [S_change(key, num(1))]))
        body.append(S_change("iterations", num(1)))
    g.define("scan", [], body, warp=True)
    g.script([
        S_set("eq_forward", num(0)),
        S_set("eq_reverse", num(0)),
        *(S_set(key, num(0)) for key in sets),
        S_set("iterations", num(0)),
        S_call("scan"),
    ])
    write_sb3(out, make_project(g.vars.keys(), g.lists.keys(), g))
    return len(cases), list(sets)


def build_recursive_fixture(out):
    # Recurse through a warp-only graph containing a native list append,
    # numeric list read and a repeat-until loop. The latter performs warp
    # timer checks, which must not force interpreter frame materialization.
    g = Gen(["gate", "sum", "stacklen"], ["stack"])
    name = "recursive %s"
    g.define(name, ["depth"], [
        S_set("gate", num(0)),
        S_until(eq(var("gate"), num(1)), [S_set("gate", num(1))]),
        S_add("stack", arg("depth")),
        S_change("sum", arg("depth")),
        S_if(gt(arg("depth"), num(0)), [
            S_call(name, [sub(arg("depth"), num(1))]),
        ]),
        S_change("sum", item("stack", length("stack"))),
    ], warp=True)
    g.script([
        S_delall("stack"), S_set("sum", num(0)),
        S_call(name, [num(120)]),
        S_set("stacklen", length("stack")),
    ])
    write_sb3(out, make_project(g.vars.keys(), g.lists.keys(), g))


def run(binary, sb3, jit, env_vars, keys=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_GPU": "0", "XYO_PAR": "0"})
    env.update(env_vars)
    p = subprocess.run([str(binary), "run", str(sb3), "--jit", jit,
                        "--frames", "10"], cwd=ROOT, env=env,
                       text=True, capture_output=True, timeout=60)
    if p.returncode:
        raise RuntimeError(f"{jit}/{env_vars}: {p.stderr[-1500:]}")
    payload = p.stdout[p.stdout.index("{"):p.stdout.rindex("}") + 1]
    d = json.loads(payload)["Bench"]
    if keys is None:
        keys = ["eq_forward", "eq_reverse", "digits", "vowels",
                "operators", "alternating", "uppercase", "punctuation",
                "single", "empty_set", "unicode_set", "large_set",
                "iterations"]
    return {k: d[k] for k in keys}


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xyo"
    outdir = ROOT / "build/diagnose-brainfuck-ast"
    outdir.mkdir(parents=True, exist_ok=True)
    sb3 = outdir / "ascii-fastpaths.sb3"
    count, set_names = build_fixture(sb3)
    all_keys = ["eq_forward", "eq_reverse", *set_names, "iterations"]
    configurations = [
        ("interpreter", "off", {}),
        ("eager default", "eager", {}),
        ("hotspot default", "hotspot", {}),
        ("eager generic equality", "eager", {"XYO_JIT_EQ_GENERIC": "1"}),
        ("eager generic contains", "eager",
         {"XYO_JIT_ASCII_SET_CONTAINS": "0"}),
    ]
    expected = None
    for label, jit, config in configurations:
        result = run(binary, sb3, jit, config, keys=all_keys)
        if expected is None:
            expected = result
        assert result == expected, (label, expected, result)
        assert result["iterations"] == str(count), result
        print("PASS", label, result, flush=True)

    recursion = outdir / "recursive-warp-lists.sb3"
    build_recursive_fixture(recursion)
    expected = None
    for label, jit, config in [
        ("recursive interpreter", "off", {}),
        ("recursive eager", "eager", {}),
        ("recursive hotspot", "hotspot", {}),
        ("recursive eager with frames", "eager",
         {"XYO_JIT_RECURSIVE_FRAME_ELIDE": "0"}),
    ]:
        result = run(binary, recursion, jit, config,
                     keys=["gate", "sum", "stacklen"])
        if expected is None:
            expected = result
        assert result == expected, (label, expected, result)
        assert result == {"gate": "1", "sum": "7260", "stacklen": "121"}, result
        print("PASS", label, result, flush=True)


if __name__ == "__main__":
    main()
