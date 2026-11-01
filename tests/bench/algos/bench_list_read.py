#!/usr/bin/env python3
"""Compare list-read workloads without mixing up numbers and numeric strings.

python3 tests/bench/algos/bench_list_read.py --before /path/to/old/xyo --cpu 8
Each process warms up, then measures three runs; process order is rotated.
"""
import argparse
import json
import os
import platform
import statistics

from bench import ROOT, HERE, run_tw, run_xyo
from gen_algo_bench import (
    Gen, S_add, S_call, S_change, S_delall, S_if, S_repeat, S_set,
    _cycle_i_body, add, arg, driver, gen_listread, gt, item, join,
    make_project, num, text, var, write_sb3,
)


def generate(kind, n):
    if kind == "string_same":
        return gen_listread(n), n * 7
    g = Gen(["i", "k", "sum", "t0", "t1", "check"], ["L", "times"])
    if kind == "number_same":
        value = add(num(7), num(0))  # An actual numeric Value, not a literal string.
    elif kind == "string_varying":
        value = join(var("k"), text(""))
    else:
        value = add(var("k"), num(0))
    g.define("fill", [], [
        S_delall("L"), S_set("k", num(0)),
        S_repeat(num(1000), [
            S_change("k", num(1)),
            S_if(gt(var("k"), num(10)), [S_set("k", num(1))]),
            S_add("L", value),
        ]), S_set("sum", num(0)),
    ], x=0, y=300)
    g.define("bench %s", ["n"], [
        S_set("i", num(0)),
        S_repeat(arg("n"), _cycle_i_body([S_change("sum", item("L", var("i")))])),
    ], x=0, y=500)
    g.define("verify", [], [S_set("check", var("sum"))], x=500, y=500)
    g.script(driver([S_call("fill")], [S_call("bench %s", [num(1000)])],
                    S_call("bench %s", [num(n)]), S_call("verify")), x=0, y=0)
    return make_project(g.vars.keys(), g.lists.keys(), g), (n * 7 if kind == "number_same" else n * 11 // 2)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--xyo", default=os.path.join(ROOT, "build", "xyo"))
    ap.add_argument("--before", help="saved baseline executable; use identical runtime options")
    ap.add_argument("--cpu", type=int, help="pin this process and all child processes to a CPU (Linux)")
    ap.add_argument("--n", type=int, default=50000000, help="reads, a positive multiple of 1000")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--json", default=os.path.join(HERE, "results-list-read.json"))
    args = ap.parse_args()
    if args.n < 1000 or args.n % 1000 or args.reps < 1:
        ap.error("--n must be a positive multiple of 1000; --reps must be positive")
    if args.cpu is not None:
        if not hasattr(os, "sched_setaffinity"):
            ap.error("--cpu requires os.sched_setaffinity")
        os.sched_setaffinity(0, {args.cpu})
    outdir = os.path.join(HERE, "out", "list-read")
    result = {"n": args.n, "reps": args.reps, "cpu": args.cpu,
              "machine": platform.machine(), "rows": {}}
    modes = (["before"] if args.before else []) + ["after", "tw-compiled"]
    print("workload          " + " ".join("%12s" % mode for mode in modes), flush=True)
    for kind in ["string_same", "number_same", "number_varying", "string_varying"]:
        project, expected = generate(kind, args.n)
        path = os.path.join(outdir, kind + ".sb3")
        write_sb3(path, project)
        runs = {mode: [] for mode in modes}
        for rep in range(args.reps):
            rotate = rep % len(modes)
            for mode in modes[rotate:] + modes[:rotate]:
                r = (run_tw(path, "tw-compiled") if mode == "tw-compiled" else
                     run_xyo(os.path.abspath(args.before if mode == "before" else args.xyo), path, "xyo-jit"))
                if str(r["check"]) != str(expected):
                    raise RuntimeError("%s / %s: checksum %s != %s" % (kind, mode, r["check"], expected))
                runs[mode].append(r)
        row = {mode: {"median_ms": statistics.median(statistics.median(r["times"]) for r in samples),
                      "runs": samples} for mode, samples in runs.items()}
        result["rows"][kind] = {"expected_check": str(expected), "modes": row}
        print("%-18s" % kind + " ".join("%10.1fms" % row[mode]["median_ms"] for mode in modes), flush=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
    with open(args.json, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)
        f.write("\n")
    print("wrote " + args.json)


if __name__ == "__main__":
    main()
