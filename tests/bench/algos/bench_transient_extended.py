#!/usr/bin/env python3
"""End-to-end Scratch semantics probes for escape-aware multi-statement appends.

Builds real SB3 projects, tests independent JIT/interpreter/reversion behavior,
and verifies that the optimized path is actually selected (or rejected).
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from gen_algo_bench import (
    Gen, S_add, S_change, S_repeat, S_until, S_set, S_call, add, join, lt, gt, op,
    make_project, num, text, var, write_sb3,
)

TEST_ROOT = Path(__file__).resolve().parents[3]
OUT = TEST_ROOT / "build" / "append-log" / "extended-cases"

def generate(kind, n, warp=True):
    g = Gen(["s", "v", "i", "other"], ["trace"])
    start = [S_set("s", text("START")), S_set("i", num(0)), S_set("v", num(7))]
    fmt = []
    if kind == "long_literal":
        body = [S_set("s", join(var("s"), text("AbcXYZ0123456789")))]
        fmt = ["AbcXYZ0123456789"] * n
    elif kind == "unicode_literal":
        body = [S_set("s", join(var("s"), text("漢字😀")))]
        fmt = ["漢字😀"] * n
    elif kind == "multi_literal":
        body = [
            S_set("s", join(var("s"), text("ab"))),
            S_set("s", join(var("s"), text("cd"))),
            S_set("s", join(var("s"), text("e😁"))),
        ]
        fmt = ["abcde😁"] * n
    elif kind == "nested_literal":
        body = [S_set("s", join(var("s"),
                        join(text("L"), join(text("M"), text("N")))))]
        fmt = ["LMN"] * n
    elif kind == "nested_dynamic":
        body = [S_set("s", join(var("s"),
                        join(text("["), join(var("v"), text("]")))))]
        fmt = ["[7]"] * n
    elif kind == "multi_counter":
        body = [
            S_change("i", num(1)),
            S_set("s", join(var("s"),
                  join(text("n:"), join(add(var("i"), num(1)), text(";"))))),
        ]
        fmt = [f"n:{i+2};" for i in range(n)]
    elif kind == "counter_after":
        body = [
            S_set("s", join(var("s"), var("i"))),
            S_change("i", num(1)),
        ]
        fmt = [str(i) for i in range(n)]
    elif kind == "multi_mixed":
        body = [
            S_set("s", join(var("s"), join(text("<"), var("i")))),
            S_set("other", add(var("i"), num(1))),
            S_set("s", join(var("s"), join(var("other"), text(">")))),
            S_change("i", num(1)),
        ]
        fmt = [f"<{i}{i+1}>" for i in range(n)]
    elif kind == "until_counter":
        # Repeat until i >= n; condition depends on an independent counter.
        body = None
        fmt = [f"U{i}" for i in range(n)]
    elif kind == "while_counter":
        body = None
        fmt = [f"W{i}" for i in range(n)]
    elif kind == "foreach_counter":
        body = None
        fmt = [f"F{i}" for i in range(1, n + 1)]
    elif kind == "until_observes_string":
        body = None
        fmt = ["x"] * n
    elif kind == "observes_destination":
        body = [
            S_set("s", join(var("s"), text("x"))),
            S_set("other", var("s")),
        ]
        fmt = ["x"] * n
    elif kind == "side_effect_list":
        body = [
            S_set("s", join(var("s"), text("x"))),
            S_add("trace", var("s")),
        ]
        fmt = ["x"] * n
    elif kind == "mutates_destination":
        body = [
            S_set("s", join(var("s"), text("x"))),
            S_change("s", num(1)),
        ]
        fmt = None
    elif kind == "multi_destination":
        start.append(S_set("other", text("OTHER")))
        body = [
            S_set("s", join(var("s"), text("A"))),
            S_set("other", join(var("other"), text("B"))),
        ]
        fmt = ["A"] * n
    elif kind == "multi_destination_interleaved":
        start.append(S_set("other", text("OTHER")))
        body = [
            S_set("s", join(var("s"), text("L"))),
            S_set("other", join(var("other"), text("日本語"))),
            S_set("s", join(var("s"), text("R"))),
        ]
        fmt = ["LR"] * n
    elif kind == "two_destinations":
        body = [
            S_set("s", join(var("s"), text("x"))),
            S_set("other", join(var("other"), text("y"))),
        ]
        fmt = ["x"] * n
    else:
        raise ValueError(kind)
    if kind == "until_counter":
        loop = S_until(gt(var("i"), num(n - 1)), [
            S_set("s", join(var("s"), join(text("U"), var("i")))),
            S_change("i", num(1)),
        ])
    elif kind == "while_counter":
        loop = S_until(lt(var("i"), num(n)), [
            S_set("s", join(var("s"), join(text("W"), var("i")))),
            S_change("i", num(1)),
        ])
    elif kind == "foreach_counter":
        loop = S_repeat(num(n), [
            S_set("s", join(var("s"), join(text("F"), var("i")))),
        ])
    elif kind == "until_observes_string":
        loop = S_until(gt(op("operator_length", STRING=var("s")),
                          num(4+n)), [
            S_set("s", join(var("s"), text("x"))),
        ])
    else:
        loop = S_repeat(num(n), body)
    g.define("work", [], start + [loop], warp=warp)
    if kind in {"while_counter", "foreach_counter"}:
        for block in g.blocks.values():
            if kind == "while_counter" and block["opcode"] == "control_repeat_until":
                block["opcode"] = "control_while"
            if kind == "foreach_counter" and block["opcode"] == "control_repeat":
                block["opcode"] = "control_for_each"
                block["fields"]["VARIABLE"] = ["i", g.vars["i"]]
                block["inputs"]["VALUE"] = block["inputs"].pop("TIMES")
    g.script([S_call("work")])
    proj = make_project(g.vars.keys(), g.lists.keys(), g)
    return proj, ("START" + "".join(fmt)) if fmt is not None else None

CASES = [
    "long_literal", "unicode_literal", "multi_literal",
    "nested_literal", "nested_dynamic", "multi_counter",
    "counter_after", "multi_mixed", "observes_destination",
    "side_effect_list", "mutates_destination", "two_destinations",
    "until_counter", "while_counter", "foreach_counter",
    "until_observes_string", "multi_destination",
    "multi_destination_interleaved",
]
NEGATIVE = {
    "observes_destination", "side_effect_list", "mutates_destination",
    "until_observes_string",
}

def run(binary, project, env):
    e = dict(os.environ)
    e.update(env)
    e["XYO_DUMPVARS"] = "json"
    e["XYO_STRING_APPEND_LOG"] = "0"
    e["XYO_JIT_VERBOSE"] = "1"
    p = subprocess.run([str(binary), "run", str(project), "--jit", "eager"],
        env=e, capture_output=True, timeout=150)
    if p.returncode:
        raise RuntimeError(
            f"{project.name} failed rc={p.returncode}: {p.stderr.decode(errors='replace')[-1500:]}")
    data = p.stdout.decode(errors="replace")
    first = data.index("{")
    last = data.rindex("}")
    output = json.loads(data[first:last+1])
    diagnostic = p.stderr.decode(errors="replace")
    return output["Bench"], ("transient MATCH" in diagnostic or
                              "transient MULTI" in diagnostic)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=192)
    ap.add_argument("--binary", default=str(TEST_ROOT / "build" / "xyo"))
    ap.add_argument("--out", default=str(OUT))
    ap.add_argument("--cases", default=",".join(CASES))
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for name in args.cases.split(","):
        project, expected = generate(name,args.n)
        path = out / f"{name}.sb3"
        write_sb3(str(path),project)
        outcomes={}
        for mode in ("off", "on"):
            data, selected = run(args.binary,path,
                {"XYO_JIT_TRANSIENT_STRINGS": "1" if mode=="on" else "0"})
            outcomes[mode]=data
            if expected is not None and data["s"] != expected:
                raise RuntimeError(f"{name} {mode} wrong output: {data['s'][:100]!r} vs {expected[:100]!r}")
            if mode == "on" and (name not in NEGATIVE) != selected:
                raise RuntimeError(f"{name} unexpected optimized path: {selected}")
            if mode == "off" and selected:
                raise RuntimeError(f"{name} selected optimized path while disabled")
        if name == "multi_destination":
            assert outcomes["on"]["other"] == "OTHER" + "B"*args.n
        if name == "multi_destination_interleaved":
            assert outcomes["on"]["other"] == "OTHER" + "日本語"*args.n
        if outcomes["on"] != outcomes["off"]:
            raise RuntimeError(f"{name}: A/B output mismatch")
        print(f"{name}: correct length={len(outcomes['on']['s'])} optimized={name not in NEGATIVE}",flush=True)

if __name__ == "__main__":
    main()
