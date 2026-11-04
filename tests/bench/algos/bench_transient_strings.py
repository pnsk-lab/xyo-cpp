#!/usr/bin/env python3
"""Workload-independent A/B benchmarks for the JIT transient-string path.

The only different runtime settings between configurations are:
XYO_JIT_TRANSIENT_STRINGS and XYO_JIT_TRANSIENT_PACKED. All work is specified
as plain Scratch blocks and all outputs are compared to a Python oracle.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

from bench import ROOT, parse_xyo
from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_repeat, S_set,
    add, arg, driver, join, make_project, num, op, text, var, write_sb3
)

COUNT = 10000

def generate(kind: str, n: int):
    if kind == "dynamic_add":
        rhs = add(var("v"), num(1))
        formatted = lambda v: str(v + 1)
    elif kind == "dynamic_value":
        rhs = var("v")
        formatted = lambda v: str(v)
    elif kind == "dynamic_div":
        rhs = op("operator_divide", NUM1=var("v"), NUM2=num(2))
        formatted = lambda v: str(v/2).removesuffix(".0")
    elif kind == "packed":
        rhs = text("ab")
        formatted = lambda v: "ab"
    elif kind == "nonpacked":
        rhs = text("abcdef")
        formatted = lambda v: "abcdef"
    else:
        raise ValueError(kind)
    g = Gen(["s", "v", "sum", "check", "t0", "t1"], ["times"])
    length = lambda: op("operator_length", STRING=var("s"))
    g.define("bench %s", ["n"], [
        S_set("v", num(1)), S_set("sum", num(0)),
        S_repeat(arg("n"), [
            S_set("s", text("")),
            S_repeat(num(COUNT), [S_set("s", join(var("s"), rhs))]),
            S_change("sum", length()),
            S_change("v", num(1)),
        ]),
    ], warp=True)
    expected = COUNT * sum(len(formatted(i)) for i in range(1, n+1))
    last = formatted(n) * COUNT
    verified = op("operator_equals",
        OPERAND1=join(text("_"), var("s")),
        OPERAND2=text("_" + last))
    g.define("verify", [], [
        S_set("check", text("invalid contents")),
        S_if(verified, [S_set("check", var("sum"))]),
    ])
    g.script(driver([], [S_call("bench %s", [num(1)])],
                    S_call("bench %s", [num(n)]), S_call("verify")))
    return make_project(g.vars.keys(), g.lists.keys(), g), expected


def run(bin_path, project, env):
    settings = {**os.environ, "XYO_DUMPVARS":"json", "XYO_STRING_APPEND_LOG":"0", **env}
    proc = subprocess.run([str(bin_path), "run", str(project), "--jit", "eager"],
                          env=settings, capture_output=True, timeout=240)
    if proc.returncode:
        raise RuntimeError(proc.stderr.decode(errors="replace")[-2000:])
    return parse_xyo(proc.stdout)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--xyo", default=str(Path(ROOT)/"build"/"xyo"))
    ap.add_argument("--cpu", type=int, default=12)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--scale", type=int, default=60)
    ap.add_argument("--kinds", default="dynamic_add,dynamic_value,dynamic_div,packed,nonpacked")
    ap.add_argument("--json", required=True)
    args=ap.parse_args()
    os.sched_setaffinity(0, {args.cpu})
    data={}
    for kind in args.kinds.split(","):
        project,expected=generate(kind,args.scale)
        path=Path(ROOT)/"build"/"append-log"/f"transient-{kind}.sb3"
        write_sb3(str(path),project)
        samples={"off":[],"on":[],"batch_off":[],"packed_off":[]}
        for rep in range(args.reps):
            # Rotate the same three configurations in a blocked experiment.
            order=list(samples)
            order=order[rep%len(order):]+order[:rep%len(order)]
            for mode in order:
                settings={
                    "XYO_JIT_TRANSIENT_STRINGS": "0" if mode=="off" else "1",
                    "XYO_JIT_TRANSIENT_PACKED": "0" if mode=="packed_off" else "1",
                    "XYO_JIT_TRANSIENT_BATCH": "0" if mode=="batch_off" else "1",
                }
                result=run(args.xyo,path,settings)
                if str(result["check"])!=str(expected):
                    raise RuntimeError(f"{kind} {mode}: {result['check']} != {expected}")
                samples[mode].append(result)
        med={k:statistics.median(statistics.median(r["times"]) for r in v)
             for k,v in samples.items()}
        data[kind]={"medians_ms":med,"expected":expected,"reps":samples}
        print(kind," ".join(f"{k}={v:.3f}ms" for k,v in med.items()),flush=True)
        Path(args.json).parent.mkdir(parents=True,exist_ok=True)
        Path(args.json).write_text(json.dumps(data,indent=2)+"\n")

if __name__=="__main__":
    main()
