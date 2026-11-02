#!/usr/bin/env python3
"""Fixed-work string kernels; warm up, alternate binaries, verify checksums."""
import argparse
import json
import os
import statistics
from bench import ROOT, run_xyo, run_tw
from gen_algo_bench import (Gen, S_call, S_change, S_if, S_repeat, S_set,
                            add, arg, div, driver, join, make_project, num,
                            op, text, var, write_sb3)


def generate(kind, scale, warp=True):
    g = Gen(["s", "i", "sum", "t0", "t1", "check"], ["times"])
    length = lambda: op("operator_length", STRING=var("s"))
    if kind in ("join", "join_number", "join_observe", "join_observe_last"):
        count = 2000 if kind.startswith("join_observe") else 20000
        rhs = add(num(12), num(0)) if kind == "join_number" else text("ab")
        body = [S_set("s", text("")), S_repeat(num(count), [
            S_set("s", join(var("s"), rhs))] + ([
                S_set("i", op("operator_letter_of",
                              LETTER=length() if kind == "join_observe_last" else num(1),
                              STRING=var("s")))
            ] if kind.startswith("join_observe") else [])), S_change("sum", length())]
        expected = scale * count * 2
    elif kind in ("letter", "contains", "short_join"):
        if kind == "short_join":
            inner = [S_set("s", join(text("abc"), text("def"))),
                     S_change("sum", length())]
            setup = []
            expected = scale * 20000 * 6
        elif kind == "letter":
            setup = [S_set("s", text("")), S_repeat(num(100), [
                S_set("s", join(var("s"), text("abcdefghij")))])]
            inner = [S_set("i", op("operator_letter_of", LETTER=num(501), STRING=var("s"))),
                     S_change("sum", op("operator_length", STRING=var("i")))]
            expected = scale * 20000
        else:
            setup = [S_set("s", text("abcdefghij" * 100))]
            inner = [S_if(op("operator_contains", STRING1=var("s"), STRING2=text("j9")),
                          [S_change("sum", num(1000000))]),
                     S_if(op("operator_contains", STRING1=var("s"), STRING2=text("hij")),
                          [S_change("sum", num(1))])]
            expected = scale * 20000
        body = setup + [S_repeat(num(20000), inner)]
    elif kind == "short_dynamic":
        # Unlike short_join's literals, this input changes on every iteration;
        # V8 cannot simply fold the entire concatenation to one constant.
        body = [S_set("i", num(0)), S_repeat(num(20000), [
            S_change("i", num(1)),
            S_set("s", join(op("operator_mod", NUM1=var("i"), NUM2=num(10)), text(":"))),
            S_change("sum", length())])]
        expected = scale * 20000 * 2
    elif kind == "prefix_fraction":
        # Non-interned numeric prefixes exercise short-result allocation fusion.
        body = [S_set("i", num(0)), S_repeat(num(5000), [
            S_change("i", num(1)),
            S_set("s", join(div(var("i"), num(4)), text(":"))),
            S_change("sum", length())])]
        expected = scale * sum(len(format(i / 4, "g") + ":") for i in range(1, 5001))
    elif kind == "cast":
        body = [S_set("i", num(0)), S_repeat(num(5000), [
            S_change("i", num(1)), S_set("s", join(div(var("i"), num(4)), text(""))),
            S_change("sum", add(var("s"), num(0)))])]
        expected = scale * 3125625
    else:
        raise ValueError(kind)
    g.define("bench %s", ["n"], [S_set("sum", num(0)), S_repeat(arg("n"), body)], warp=warp)
    wanted = {"join": "ab" * 20000, "join_number": "12" * 20000,
              "join_observe": "ab" * 2000, "join_observe_last": "ab" * 2000,
              "short_join": "abcdef",
              "letter": "abcdefghij" * 100, "contains": "abcdefghij" * 100,
              "cast": "1250", "short_dynamic": "0:", "prefix_fraction": "1250:"}[kind]
    # Prefix with nonnumeric text so Scratch equality cannot treat distinct
    # huge digit strings as the same Infinity. Verification is outside timing.
    valid = op("operator_equals", OPERAND1=join(text("_"), var("s")),
               OPERAND2=text("_" + wanted))
    if kind in ("letter", "join_observe", "join_observe_last"):
        valid = op("operator_and", OPERAND1=valid,
                   OPERAND2=op("operator_equals", OPERAND1=var("i"),
                               OPERAND2=text("b" if kind == "join_observe_last" else "a")))
    g.define("verify", [], [S_set("check", text("invalid string contents")),
                           S_if(valid, [S_set("check", var("sum"))])])
    expected = str(expected)
    g.script(driver([], [S_call("bench %s", [num(1)])],
                    S_call("bench %s", [num(scale)]), S_call("verify")))
    return make_project(g.vars.keys(), g.lists.keys(), g), expected


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--xyo", default=os.path.join(ROOT, "build/xyo"))
    ap.add_argument("--before")
    ap.add_argument("--tw", action="store_true")
    ap.add_argument("--non-warp", action="store_true",
                    help="generate the measured procedure with warp=false")
    ap.add_argument("--cpu", type=int)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--scale", type=int, default=40)
    ap.add_argument("--kinds", default="join,join_number,join_observe,cast,short_join,short_dynamic,letter,contains")
    ap.add_argument("--json", required=True)
    args = ap.parse_args()
    if args.scale < 1 or args.reps < 1:
        ap.error("scale and reps must be positive")
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    modes = (["before"] if args.before else []) + ["after"] + (["tw"] if args.tw else [])
    warp = not args.non_warp
    result = {"scale": args.scale, "reps": args.reps, "cpu": args.cpu,
              "warp": warp, "rows": {}}
    for kind in args.kinds.split(","):
        project, expected = generate(kind, args.scale, warp=warp)
        path = os.path.join(ROOT, "build/string-perf", kind + ".sb3")
        write_sb3(path, project)
        runs = {m: [] for m in modes}
        for rep in range(args.reps):
            order = modes[rep % len(modes):] + modes[:rep % len(modes)]
            for mode in order:
                r = run_tw(path, "tw-compiled") if mode == "tw" else run_xyo(
                    os.path.abspath(args.before if mode == "before" else args.xyo), path, "xyo-jit")
                if str(r["check"]) != expected:
                    raise RuntimeError(f"{kind}/{mode}: {r['check']} != {expected}")
                runs[mode].append(r)
        row = {m: {"median_ms": statistics.median(statistics.median(r["times"]) for r in rs),
                   "runs": rs} for m, rs in runs.items()}
        result["rows"][kind] = {"expected": expected, "modes": row}
        print(kind, " ".join(f"{m}={row[m]['median_ms']:.2f}ms" for m in modes), flush=True)
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, "w") as f:
            json.dump(result, f, indent=2)
            f.write("\n")


if __name__ == "__main__":
    main()
