#!/usr/bin/env python3
"""Guarded read-only list regions: bounds, projection tags and flat-layout guards."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (Gen, S_add, S_call, S_change, S_ifelse, S_repeat, S_set,
    add, div, eq, item, make_project, mod, mul, num, op, text, var, write_sb3)


def fixture(count, kind, values):
    g = Gen(["i", "acc", "temp"], ["data", "indices", "probe", "checks"])
    index = add(mod(mul(var("i"), num(7919)), num(64)), num(1))
    if kind == "fractional": index = add(mod(var("i"), num(5)), num(0.75))
    if kind == "outside": index = var("i")
    if kind == "last": index = text("last")
    if kind == "random": index = text("random")
    if kind == "nested": index = item("indices", add(mod(var("i"), num(4)), num(1)))
    read = item("data", index)
    body = [S_change("i", num(1))]
    if kind == "copy": body += [S_set("temp", read)]
    elif kind == "compare":
        body += [S_ifelse(eq(read, num(0)), [S_change("acc", num(1))], [S_change("acc", num(2))])]
    elif kind == "write": body += [S_add("data", var("i"))]
    else: body += [S_change("acc", read)]
    g.define("kernel", [], [S_repeat(num(count), body)], warp=True)
    prelude = []
    data = values
    if kind == "nonfinite":
        data = []
        prelude = [S_add("data", div(num(1), num(0))), S_add("data", div(num(-1), num(0))),
                   S_add("data", div(num(0), num(0))), S_add("data", mul(num(-1), num(0)))]
    g.script(prelude + [S_set("i", add(num(0), num(0))), S_set("acc", add(num(0), num(0))),
              S_set("temp", add(num(0), num(0))), S_call("kernel"),
              S_add("checks", var("i")), S_add("checks", var("acc")), S_add("checks", var("temp")),
              S_add("checks", div(num(1), var("temp"))), S_add("checks", div(num(1), var("acc"))),
              S_add("probe", var("temp")), S_add("probe", text("Z")),
              S_add("checks", op("data_listcontents", {"LIST": "probe"}))])
    return make_project(g.vars, g.lists, g, {"data": data, "indices": [1, "2", "last", "random"]})


def run(binary, sb3, mode, profile, storage, out, label, dump=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_GPU": "0", "XYO_PAR": "0", "XYO_PARFOR": "0",
                "XYO_LIST_STORAGE": storage})
    if profile == "off":
        env.update({"XYO_JIT_NUMERIC_REGION": "0", "XYO_JIT_NUMERIC_REGION_MIXED": "0",
                    "XYO_JIT_NUMERIC_REGION_FACT_GUARDS": "0", "XYO_JIT_NUMERIC_REGION_READ_LIST": "0",
                    "XYO_JIT_NUMERIC_REGION_LIST_FINITE": "0"})
    elif profile == "default":
        pass
    elif profile == "lists":
        env.update({"XYO_JIT_NUMERIC_REGION": "1", "XYO_JIT_NUMERIC_REGION_MIXED": "1",
                    "XYO_JIT_NUMERIC_REGION_FACT_GUARDS": "1", "XYO_JIT_NUMERIC_REGION_READ_LIST": "1",
                    "XYO_JIT_NUMERIC_REGION_LIST_FINITE": "0"})
    if dump: env.update({"XYO_JIT_DUMP": "1", "XYO_JIT_DUMP2": "1"})
    p = subprocess.run([str(binary), "run", str(sb3), "--jit", mode, "--frames", "100000"],
                       cwd=ROOT, env=env, capture_output=True, text=True, timeout=90)
    (out / f"{label}.stdout").write_text(p.stdout)
    (out / f"{label}.stderr").write_text(p.stderr)
    if p.returncode: raise RuntimeError(f"{label}: exit {p.returncode}: {p.stderr[-800:]}")
    data = json.loads(p.stdout[p.stdout.index("{"):p.stdout.rindex("}") + 1])["Bench"]
    return data, p.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args()
    binary, out = args.binary.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    arrays = [list(range(1, 65)), ["0", "1", "0017", " ", "abc", "NaN", "0.5", "-0"],
              [True, False, 1, "1", "0", " ", -0.0], [], [7] * 64]
    rows = []
    def check(label, sb, storage="flat", lowering=None):
        path = out / f"{label}.sb3"; write_sb3(path, sb)
        expected, _ = run(binary, path, "off", "off", storage, out, label + "-interp")
        for profile in ("off", "lists", "finite"):
            actual, ir = run(binary, path, "eager", profile, storage, out,
                             label + "-" + profile, dump=lowering is not None)
            if actual != expected: raise RuntimeError(f"{label}/{profile}: {actual} != {expected}")
            if lowering is not None:
                selected = "mixed.list.bounded" in ir
                if selected != (lowering and profile != "off"):
                    raise RuntimeError(f"unexpected list lowering {label}/{profile}")
        rows.append({"case": label, "storage": storage, "expected": expected})
        (out / "results.json").write_text(json.dumps(rows, indent=2))
    kinds = ("sum", "copy", "compare", "fractional", "outside", "nonfinite")
    for kind in kinds:
        for index, data in enumerate(arrays[:2] if args.quick else arrays):
            for count in ((65,) if args.quick else (0, 1, 16, 63, 64, 65, 129)):
                check(f"{kind}-a{index}-n{count}", fixture(count, kind, data))
    for kind in ("last", "random", "nested", "write"):
        check(kind, fixture(65, kind, [7] * 64))
    for storage in ("sbo4", "sbo8", "flat15", "deque", "gap", "chunked", "adaptive"):
        check(f"storage-{storage}", fixture(129, "sum", list(range(1, 65))), storage)
    for kind in ("sum", "copy", "compare", "fractional", "nonfinite"):
        check(f"lowering-{kind}", fixture(129, kind, [1, "2", " ", True]), lowering=True)
    # With all experiment flags unset, the validated list path is the default.
    default_sb = fixture(129, "sum", list(range(1, 65)))
    default_path = out / "default-list.sb3"
    write_sb3(default_path, default_sb)
    _, default_ir = run(binary, default_path, "eager", "default", "flat", out,
                        "default-list", dump=True)
    if "mixed.list.bounded" not in default_ir:
        raise RuntimeError("readonly list region is not enabled by default")
    _, disabled_ir = run(binary, default_path, "eager", "off", "flat", out,
                         "disabled-list", dump=True)
    if "mixed.list.bounded" in disabled_ir:
        raise RuntimeError("explicit readonly-list opt-out was ignored")
    for kind in ("write", "last", "random"):
        check(f"rejected-{kind}", fixture(65, kind, [7] * 64), lowering=False)
    print(f"PASS {len(rows)} list fixtures / {len(rows)*3} JIT comparisons; bounds/tags/storage guards verified")


if __name__ == "__main__":
    main()
