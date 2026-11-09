#!/usr/bin/env python3
"""OSR parity and safety for top-level constant-count Scratch repeats.

A compiler worker produces two entries: a normal script and a continuation
starting at the interpreter's suspended top-level repeat. The latter must
run exactly the remaining iterations and the trailing suffix, never replay
prefix side effects.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/"tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_set, S_change, S_repeat, S_until, S_call,
    S_add, length, item, num, var, lt, gt,
    make_project, write_sb3,
)


def create_case(name, path):
    if name == "long":
        repetitions = 250000
        g = Gen(["sum", "done", "prefix"], [])
        body = [
            S_set("sum", num(0)), S_change("sum", num(7)),
            S_set("prefix", num(11)),
            S_repeat(num(repetitions), [S_change("sum", num(1))]),
            S_change("sum", num(3)), S_set("done", num(1)),
        ]
        expected = {"sum": str(repetitions + 10), "prefix": "11", "done": "1"}
    elif name == "fractional":
        repetitions = 250001
        g = Gen(["sum", "done", "prefix"], [])
        body = [
            S_set("sum", num(100)), S_set("prefix", num(17)),
            S_repeat(num(250000.5), [S_change("sum", num(-1))]),
            S_change("sum", num(6)), S_set("done", num(1)),
        ]
        expected = {"sum": str(106 - repetitions), "prefix": "17", "done": "1"}
    elif name == "list":
        repetitions = 120000
        g = Gen(["sum", "done", "count", "first"], ["items"])
        body = [
            S_set("sum", num(0)),
            S_repeat(num(repetitions), [
                S_add("items", num(7)),
                S_change("sum", num(1)),
            ]),
            S_set("count", length("items")),
            S_set("first", item("items", num(1))),
            S_set("done", num(1)),
        ]
        expected = {"sum": str(repetitions), "count": str(repetitions),
                    "first": "7", "done": "1"}
    elif name == "nested":
        repetitions = 20000
        g = Gen(["sum", "done", "prefix"], [])
        body = [
            S_set("sum", num(3)), S_set("prefix", num(9)),
            S_repeat(num(repetitions), [
                S_repeat(num(3), [S_change("sum", num(1))])]),
            S_set("done", num(1)),
        ]
        expected = {"sum": str(repetitions * 3 + 3), "prefix": "9", "done": "1"}
    elif name in ("until", "while"):
        g = Gen(["sum", "done", "prefix"], [])
        boundary = 210000
        predicate = (gt(var("sum"), num(boundary)) if name == "until"
                     else lt(var("sum"), num(boundary)))
        body = [
            S_set("sum", num(3)), S_set("prefix", num(19)),
            S_until(predicate, [S_change("sum", num(1))]),
            S_change("sum", num(5)), S_set("done", num(1)),
        ]
        final = boundary + (1 if name == "until" else 0)
        expected = {"sum": str(final + 5), "prefix": "19", "done": "1"}
    elif name == "dynamic":
        g = Gen(["sum", "done", "limit"], [])
        body = [
            S_set("sum", num(0)), S_set("limit", num(1500)),
            S_repeat(var("limit"), [S_change("sum", num(1))]),
            S_set("done", num(1)),
        ]
        expected = {"sum": "1500", "limit": "1500", "done": "1"}
    elif name == "procedure":
        g = Gen(["sum", "done"], [])
        g.define("work", [], [
            S_repeat(num(1500), [S_change("sum", num(1))])],
            warp=False)
        body = [S_set("sum", num(0)), S_call("work"), S_set("done", num(1))]
        expected = {"sum": "1500", "done": "1"}
    elif name == "two":
        g = Gen(["sum", "done"], [])
        body = [
            S_set("sum", num(0)),
            # Keep the first loop alive until the asynchronous LLVM compile
            # completes, including on fast hosts. Otherwise a correct VM can
            # finish before it has any opportunity to promote into OSR.
            S_repeat(num(2000000), [S_change("sum", num(1))]),
            S_repeat(num(11), [S_change("sum", num(2))]),
            S_set("done", num(1)),
        ]
        expected = {"sum": "2000022", "done": "1"}
    else:
        raise ValueError(name)
    g.script(body)
    if name == "while":
        for block in g.blocks.values():
            if block["opcode"] == "control_repeat_until":
                block["opcode"] = "control_while"
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return expected


def run(binary, project, tier, async_mode, osr_mode,
        conditional=False, frames=400000):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("XYO_")}
    env.update(
        XYO_JIT_ASYNC="1" if async_mode else "0",
        XYO_JIT_OSR="1" if osr_mode else "0",
        XYO_JIT_OSR_CONDITIONAL="1" if conditional else "0",
        XYO_JIT_OSR_TIMING="1", XYO_JIT_ASYNC_TIMING="1",
        XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0",
        XYO_PARFOR="0",
    )
    p = subprocess.run(
        [str(binary), "run", str(project), "--jit", tier,
         "--frames", str(frames)],
        env=env, cwd=ROOT, capture_output=True, text=True, timeout=75,
    )
    if p.returncode:
        raise RuntimeError((project.name, tier, osr_mode,
                            p.returncode, p.stderr[-850:], p.stdout[-850:]))
    output = p.stdout
    values = json.loads(output[output.index("{"):output.rindex("}") + 1])["Bench"]
    return values, p.stderr


def main():
    binary = (Path(sys.argv[1]) if len(sys.argv) > 1
              else ROOT/"build/xyo").resolve()
    out = ROOT/"build/async-jit/osr"
    out.mkdir(parents=True, exist_ok=True)
    for name in ("long", "fractional", "list", "nested", "until", "while",
                 "dynamic", "procedure", "two"):
        project = out / (name + ".sb3")
        expected = create_case(name, project)
        base, _ = run(binary, project, "off", False, False)
        assert all(base.get(k) == v for k, v in expected.items()), (
            name, {k: base.get(k) for k in expected}, expected)
        no_osr, _ = run(binary, project, "hotspot", True, False)
        # Conditional loop OSR is measured separately; the default path
        # must remain unchanged because it regressed on the tested corpus.
        if name in ("until", "while"):
            default, default_trace = run(
                binary, project, "hotspot", True, True)
            assert default == base and "[jit-osr] promoted" not in default_trace
        with_osr, trace = run(
            binary, project, "hotspot", True, True,
            conditional=name in ("until", "while"))
        if base != no_osr or base != with_osr:
            for key in base:
                if base.get(key) != no_osr.get(key) or base.get(key) != with_osr.get(key):
                    if isinstance(base[key], list):
                        raise AssertionError((name, key, "list mismatch",
                                              len(base[key]), len(no_osr.get(key, [])),
                                              len(with_osr.get(key, []))))
                    raise AssertionError((name, key, base[key],
                                          no_osr.get(key), with_osr.get(key)))
        did_promote = "[jit-osr] promoted" in trace
        if name in ("long", "two", "fractional", "until", "while") and not did_promote:
            raise AssertionError((name, "OSR never triggered", trace[-1400:]))
        if name in ("dynamic", "procedure") and did_promote:
            raise AssertionError((name, "unsafe OSR", trace[-1400:]))
        print("PASS", name, "promoted" if did_promote else "fallback",
              "sum", base["sum"], flush=True)


if __name__ == "__main__":
    main()
