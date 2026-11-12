#!/usr/bin/env python3
"""Common typed numeric CFG parity, joins, calls and transactional NaN bailout."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (Gen, S_add, S_call, S_change, S_ifelse, S_repeat,
                            S_set, add, arg, div, eq, lt, make_project, mod,
                            mul, num, op, sub, text, var, write_sb3)
from numeric_region_cases import KINDS, project


def special(count, kind, acc_init=None):
    g = Gen(["x", "acc", "i", "temp", "source"], ["checks", "probe", "effects"])
    body = [S_change("i", num(1)), S_set("x", mod(add(var("x"), num(1)), num(97)))]
    if kind in ("mixed_arith", "mixed_phi"):
        floating = div(var("x"), num(4))
        body = [S_change("i", num(1)),
                S_set("x", mod(add(mul(var("x"), num(31)), var("i")), num(1000003)))]
        if kind == "mixed_phi":
            body += [S_ifelse(eq(mod(var("i"), num(2)), num(0)),
                              [S_set("temp", floating)],
                              [S_set("temp", mul(var("x"), num(2)))])]
            floating = var("temp")
        body += [S_set("acc", sub(add(var("acc"), floating), mul(var("i"), num(0.5))))]
    elif kind == "nan_boundary":
        d = sub(num(65), var("i"))
        body = [S_change("i", num(1)), S_set("temp", div(d, d)),
                S_ifelse(eq(var("temp"), var("temp")),
                         [S_change("x", num(1))], [S_change("x", num(1000))])]
    elif kind == "nan_in_helper":
        d = sub(num(65), var("i"))
        g.define("step %s", ["old"], [S_change("i", num(1)),
                 S_set("temp", div(d, d)),
                 S_ifelse(eq(var("temp"), var("temp")),
                          [S_set("x", add(arg("old"), num(1)))],
                          [S_set("x", add(arg("old"), num(1000)))])], warp=True)
        body = [S_call("step %s", [var("x")])]
    elif kind == "signed_integer":
        body = [S_change("i", num(1)), S_change("x", num(-1)),
                S_ifelse(lt(var("x"), num(-10)),
                         [S_change("acc", var("x"))], [S_change("acc", num(1))])]
    elif kind == "negative_mod":
        body = [S_change("i", num(1)), S_set("x", mod(sub(var("x"), num(1)), num(-3)))]
    elif kind == "dynamic_mod":
        body = [S_change("i", num(1)),
                S_set("temp", add(mod(var("i"), num(7)), num(1))),
                S_set("x", mod(add(mul(var("x"), num(17)), num(23)), var("temp")))]
    elif kind == "negative_zero":
        body = [S_set("temp", mul(var("x"), num(-1))),
                S_ifelse(eq(var("x"), num(0)),
                         [S_set("x", var("temp"))],
                         [S_set("x", div(var("temp"), num(2)))])]
    elif kind == "phi_discovery":
        body = [S_ifelse(lt(var("i"), num(64)),
                         [S_set("temp", add(var("x"), num(2)))],
                         [S_set("source", add(var("x"), num(3)))]),
                S_set("x", div(add(var("temp"), var("source")), num(2))),
                S_change("i", num(1))]
    elif kind == "unreached_nan":
        body = [S_ifelse(lt(var("x"), num(1000)),
                         [S_set("x", div(add(var("x"), num(1)), num(2)))],
                         [S_set("x", div(num(0), num(0)))])]
    elif kind == "string_compare":
        body = [S_ifelse(eq(var("source"), num(0)),
                         [S_change("x", num(1))], [S_change("x", num(2))])]
    elif kind in ("literal_set", "literal_argument_copy"):
        if kind == "literal_set":
            body = [S_set("x", text("0017"))]
        else:
            g.define("copy %s", ["value"], [S_set("x", arg("value"))], warp=True)
            body = [S_call("copy %s", [text("0017")])]
    elif kind == "whitespace_literal_compare":
        body = [S_ifelse(eq(text(" "), num(0)),
                         [S_change("x", num(1))], [S_change("x", num(2))])]
    elif kind == "list_effect":
        body += [S_add("effects", var("x"))]
    elif kind == "argument_capture":
        g.define("capture %s %s", ["old", "next"],
                 [S_set("x", add(num(0), num(0))),
                  S_set("temp", add(arg("old"), arg("next"))),
                  S_set("x", mod(var("temp"), num(97)))], warp=True)
        body = [S_call("capture %s %s", [var("x"), add(var("x"), num(1))])]
    elif kind == "lexical_shadow":
        g.define("inner %s", ["value"], [S_set("x", mod(add(arg("value"), num(2)), num(97)))], warp=True)
        g.define("outer %s", ["value"], [S_call("inner %s", [add(arg("value"), num(1))]),
                 S_set("temp", add(arg("value"), num(3)))], warp=True)
        body = [S_call("outer %s", [var("x")])]
    g.define("kernel", [], [S_repeat(num(count), body)], warp=True)
    g.script([S_set("x", add(num(0), num(0))),
              S_set("acc", acc_init if acc_init is not None else add(num(0), num(0))),
              S_set("i", add(num(0), num(0))), S_set("temp", add(num(0), num(0))),
              S_set("source", text(" ") if kind == "string_compare" else add(num(0), num(0))),
              S_call("kernel"), S_add("checks", var("x")), S_add("checks", var("i")),
              S_add("checks", var("temp")), S_add("checks", var("acc")),
              S_add("checks", div(num(1), var("x"))),
              S_add("checks", div(num(1), var("acc"))),
              S_add("probe", var("x")), S_add("probe", text("Z")),
              S_add("checks", op("data_listcontents", {"LIST": "probe"}))])
    return make_project(g.vars, g.lists, g)


def run(binary, path, mode, region, out, label, dump=False, profile="region"):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_GPU": "0", "XYO_PAR": "0", "XYO_PARFOR": "0"})
    if region == 0:
        env["XYO_JIT_NUMERIC_REGION"] = "0"
    elif region == 1:
        env["XYO_JIT_NUMERIC_REGION"] = "1"
    if region == 1 and profile in ("mixed", "facts", "hybrid"):
        env["XYO_JIT_NUMERIC_REGION_MIXED"] = "1"
    elif region == 0:
        env["XYO_JIT_NUMERIC_REGION_MIXED"] = "0"
    if region == 1 and profile in ("facts", "hybrid"):
        env["XYO_JIT_NUMERIC_REGION_FACT_GUARDS"] = "1"
    elif region == 0:
        env["XYO_JIT_NUMERIC_REGION_FACT_GUARDS"] = "0"
    if region == 1 and profile == "hybrid":
        env["XYO_JIT_NUMERIC_REGION_COST_SELECT"] = "1"
    elif region == 0:
        env["XYO_JIT_NUMERIC_REGION_COST_SELECT"] = "0"
        env["XYO_JIT_NUMERIC_REGION_READ_LIST"] = "0"
        env["XYO_JIT_NUMERIC_REGION_LIST_FINITE"] = "0"
    if dump:
        env.update({"XYO_JIT_DUMP": "1", "XYO_JIT_DUMP2": "1"})
    p = subprocess.run([str(binary), "run", str(path), "--jit", mode, "--frames", "100000"],
                       cwd=ROOT, env=env, text=True, capture_output=True, timeout=90)
    (out / f"{label}.stdout").write_text(p.stdout)
    (out / f"{label}.stderr").write_text(p.stderr)
    if p.returncode:
        raise RuntimeError(f"{label}: exit {p.returncode}; {p.stderr[-800:]}")
    data = json.loads(p.stdout[p.stdout.index("{"):p.stdout.rindex("}") + 1])["Bench"]
    for key in ("times", "t0", "t1"):
        data.pop(key, None)
    return data, p.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--profile", choices=("region", "mixed", "facts", "hybrid"), default="region")
    args = ap.parse_args()
    binary, out = args.binary.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    records = []
    def check(label, fixture, lowering=None):
        path = out / f"{label}.sb3"
        write_sb3(path, fixture)
        expected, _ = run(binary, path, "off", 0, out, label + "-interp")
        for enabled in (0, 1):
            actual, ir = run(binary, path, "eager", enabled, out,
                             label + f"-region{enabled}", dump=lowering is not None, profile=args.profile)
            if actual != expected:
                raise RuntimeError(f"{label} region={enabled}: {actual} != {expected}")
            selected = "rep.region.fast" in ir or "rep.mixed.fast" in ir
            if lowering is not None and selected != (lowering and enabled == 1):
                raise RuntimeError(f"{label}: unexpected region lowering with flag={enabled}")
        records.append({"case": label, "expected": expected})
        (out / "results.json").write_text(json.dumps(records, indent=2))
    sources = [(add(num(0), num(1)), add(num(0), num(0))),
               (text("1"), text("0")), (text("abc"), add(num(0), num(0))),
               (div(num(1), num(2)), div(num(1), num(2))),
               (mul(num(-1), num(0)), add(num(0), num(0))),
               (div(num(0), num(0)), add(num(0), num(0))),
               (div(num(1), num(0)), add(num(0), num(0))),
               (add(num(0), num(2**40)), add(num(0), num(0)))]
    kinds = ("inline", "phi_temp", "all_combined") if args.quick else KINDS
    counts = (0, 65) if args.quick else (0, 1, 63, 64, 65, 129)
    for kind in kinds:
        for warp in ((True,) if args.quick else (True, False)):
            for index, (x, acc) in enumerate(sources[:2] if args.quick else sources):
                for count in counts:
                    check(f"{kind}-w{warp}-s{index}-n{count}",
                          project(count, kind, samples=1, warp=warp, x=x, acc=acc))
    for kind in ("inline", "if_else"):
        for count in (1, 65, 129):
            check(f"float-{kind}-{count}", project(count, kind, samples=1, floating=True))
    for kind in ("mixed_arith", "mixed_phi", "nan_boundary", "nan_in_helper", "signed_integer", "negative_mod", "dynamic_mod",
                 "negative_zero", "phi_discovery", "unreached_nan",
                 "string_compare", "literal_set", "literal_argument_copy", "whitespace_literal_compare",
                 "list_effect", "argument_capture", "lexical_shadow"):
        for count in ((65,) if args.quick else (0, 1, 63, 64, 65, 66, 129)):
            check(f"special-{kind}-{count}", special(count, kind))
    for kind in KINDS:
        check(f"lowering-{kind}", project(129, kind, samples=1), lowering=True)
    for kind in ("inline", "if_else"):
        check(f"lowering-float-{kind}", project(129, kind, samples=1, floating=True), lowering=True)
    check("lowering-signed", special(129, "signed_integer"), lowering=True)
    for kind in ("mixed_arith", "mixed_phi"):
        check(f"lowering-{kind}", special(129, kind), lowering=True)
        for index, initial in enumerate((div(num(1), num(0)), div(num(-1), num(0)),
                                         div(num(0), num(0)), mul(num(-1), num(0)), text("abc"))):
            check(f"mixed-input-{kind}-{index}", special(129, kind, acc_init=initial))
    for kind in ("literal_set", "literal_argument_copy", "whitespace_literal_compare", "list_effect"):
        check(f"rejected-{kind}", special(65, kind), lowering=False)
    # Unset selects the new default; explicit zero must restore the legacy route.
    path = out / "lowering-inline.sb3"
    _, default_ir = run(binary, path, "eager", None, out, "default-unset", dump=True, profile=args.profile)
    if "rep.region.fast" not in default_ir and "rep.mixed.fast" not in default_ir:
        raise RuntimeError("numeric region default path was not selected")
    _, disabled_ir = run(binary, path, "eager", 0, out, "explicit-off", dump=True, profile=args.profile)
    if "rep.region.fast" in disabled_ir or "rep.mixed.fast" in disabled_ir:
        raise RuntimeError("explicit numeric-region opt-out was ignored")
    print(f"PASS {len(records)} fixtures / {len(records)*2} comparisons; region CFG and fallback verified")


if __name__ == "__main__":
    main()
