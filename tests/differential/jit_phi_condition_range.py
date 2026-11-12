#!/usr/bin/env python3
"""Verify path-local numeric bound narrowing after an exact-integer PHI.

The branch-local proof must never escape to the sibling or merge. Conditions
with dynamic RHS, fractional bounds and mutable or mixed-type sources reject
the new interval facts. Exact output is compared with the interpreter.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tests/differential"), str(ROOT / "tests/bench/algos")]
from gen_algo_bench import (
    S_add, S_change, S_if, S_ifelse, S_set,
    add, floor_, gt, item, lt, make_project, num, round_, sub, text, var,
    write_sb3,
)
from jit_list_proven_i32_index import ForEachGen


def fixture(size: int, kind: str):
    g = ForEachGen(["i", "temp", "sum"], ["values", "output"])
    i, temp = var("i"), var("temp")
    positive = floor_(add(i, num(1)))
    negative = round_(sub(i, num(size // 2)))
    if kind in ("mixed", "both_mixed", "swapped_mixed"):
        positive = text("7")
    initial = S_ifelse(
        lt(i, num(size // 2)),
        [S_set("temp", negative)],
        [S_set("temp", positive)],
    )
    read = S_change("sum", item("values", temp))
    if kind == "positive_true":
        follow = [S_if(gt(temp, num(0)), [read])]
    elif kind == "positive_false":
        follow = [S_ifelse(lt(temp, num(1)),
                            [S_change("sum", num(-1))], [read])]
    elif kind == "positive_both":
        follow = [S_ifelse(gt(temp, num(0)),
                            [read], [S_change("sum", num(-1)), read])]
    elif kind == "both_false_positive":
        follow = [S_ifelse(lt(temp, num(1)), [read], [read])]
    elif kind == "swapped_positive_both":
        follow = [S_ifelse(lt(num(0), temp), [read], [read])]
    elif kind == "swapped_bound_one":
        follow = [S_ifelse(gt(num(1), temp), [read], [read])]
    elif kind == "swapped_positive_then_outside":
        follow = [S_ifelse(lt(num(0), temp), [read], [read]), read]
    elif kind == "swapped_fractional":
        follow = [S_ifelse(lt(num(0.5), temp), [read], [read])]
    elif kind == "swapped_string":
        follow = [S_ifelse(lt(text("0"), temp), [read], [read])]
    elif kind == "swapped_mixed":
        follow = [S_ifelse(lt(num(0), temp), [read], [read])]
    elif kind == "swapped_mutates":
        follow = [S_ifelse(lt(num(0), temp),
                            [S_set("temp", text("7")), read], [read])]
    elif kind == "both_fractional":
        follow = [S_ifelse(gt(temp, num(0.5)), [read], [read])]
    elif kind == "both_string_rhs":
        follow = [S_ifelse(gt(temp, text("0")), [read], [read])]
    elif kind == "both_mixed":
        follow = [S_ifelse(gt(temp, num(0)), [read], [read])]
    elif kind == "both_mutates":
        follow = [S_ifelse(gt(temp, num(0)),
                            [S_set("temp", text("7")), read], [read])]
    elif kind == "both_then_outside":
        follow = [S_ifelse(gt(temp, num(0)), [read], [read]), read]
    elif kind == "both_negative_bound":
        follow = [S_ifelse(gt(temp, num(-1)), [read], [read])]
    elif kind == "positive_then_outside":
        follow = [S_if(gt(temp, num(0)), [read]), read]
    elif kind == "nested_positive":
        follow = [S_if(gt(temp, num(0)),
                       [S_if(gt(temp, num(1)), [read])])]
    elif kind == "negative_gt_minus1":
        follow = [S_if(gt(temp, num(-1)), [read])]
    elif kind == "fractional_rhs":
        follow = [S_if(gt(temp, num(0.5)), [read])]
    elif kind == "string_rhs":
        follow = [S_if(gt(temp, text("0")), [read])]
    elif kind == "mixed":
        follow = [S_if(gt(temp, num(0)), [read])]
    elif kind == "mutates":
        follow = [S_if(gt(temp, num(0)),
                       [S_set("temp", text("7")), read])]
    elif kind == "false_negative":
        follow = [S_ifelse(lt(temp, num(0)),
                            [S_change("sum", num(1))], [read])]
    else:
        raise ValueError(kind)

    g.script([
        S_set("sum", num(0)),
        ("foreach", "i", num(size),
         [S_set("temp", text("unset")), initial, *follow]),
        S_add("output", var("sum")),
        S_add("output", var("temp")),
    ])
    return make_project(
        g.vars.keys(), g.lists.keys(), g, {"values": [1] * (size + 10)}
    )


def run(binary: Path, project: Path, tier: str, setting, dump=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(
        XYO_JIT_PHI_INTEGER_RANGE="1",
        XYO_JIT_PHI_RANGE_BRANCH_FORWARD="1",
        XYO_JIT_PHI_RANGE_IFELSE_FORWARD="1",
        XYO_JIT_PHI_NESTED_BRANCH_FORWARD="1",
        XYO_JIT_LOCAL_VAR_FORWARD="1",
        XYO_JIT_LAZY_TYPED_SSA="1",
        XYO_JIT_LIST_PROVEN_I32_INDEX="1",
        XYO_JIT_UNARY_INTEGER_RANGE="1",
        XYO_JIT_LAZY_MODE="off",
        XYO_PARFOR="0",
        XYO_GPU="0",
    )
    if setting is not None:
        env["XYO_JIT_PHI_CONDITION_RANGE"] = str(int(setting))
    if dump:
        env["XYO_JIT_DUMP2"] = "1"
    p = subprocess.run(
        [str(binary), "run", str(project), "--jit", tier,
         "--print-list", "output"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=90,
    )
    if p.returncode:
        raise RuntimeError(
            (project.name, tier, setting, p.returncode, p.stderr[-900:])
        )
    return tuple(line.strip() for line in p.stdout.splitlines()
                 if line.strip()), p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=ROOT /
        "build/multi-bench-20261111/phi-condition-range")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    cases = (
        "positive_true", "positive_false", "positive_both",
        "both_false_positive", "swapped_positive_both",
        "swapped_bound_one", "swapped_positive_then_outside",
        "swapped_fractional", "swapped_string", "swapped_mixed",
        "swapped_mutates", "both_fractional", "both_string_rhs",
        "both_mixed", "both_mutates", "both_then_outside",
        "both_negative_bound", "positive_then_outside", "nested_positive",
        "negative_gt_minus1", "fractional_rhs", "string_rhs", "mixed",
        "mutates", "false_negative",
    )
    for case in cases:
        project = args.out / f"{case}.sb3"
        write_sb3(project, fixture(512, case))
        expected, _ = run(binary, project, "off", 0)
        for tier in ("eager", "hotspot"):
            for setting in (0, 1, None):
                actual, _ = run(binary, project, tier, setting)
                assert actual == expected, (
                    case, tier, setting, expected, actual
                )
        cvtt = {}
        narrowed = {}
        for flag in (0, 1, None):
            _, ir = run(binary, project, "eager", flag, dump=True)
            label = "default" if flag is None else str(flag)
            (args.out / f"{case}-{label}.ll").write_text(ir)
            cvtt[label] = ir.count(
                "call i64 @llvm.x86.sse2.cvttsd2si64("
            )
            narrowed[label] = ir.count("fptoui double")
        assert cvtt["default"] == cvtt["1"], (case, cvtt)
        assert narrowed["default"] == narrowed["1"], (case, narrowed)
        if case == "positive_both":
            assert cvtt["1"] == cvtt["0"] - 1, (case, cvtt)
            assert narrowed["1"] > narrowed["0"], (case, narrowed)
        elif case == "swapped_positive_both":
            assert cvtt["1"] <= cvtt["0"], (case, cvtt)
            assert narrowed["1"] > narrowed["0"], (case, narrowed)
        else:
            assert cvtt["1"] == cvtt["0"], (case, cvtt)
            assert narrowed["1"] == narrowed["0"], (case, narrowed)
        print("PASS", case, "output", expected,
              "cvtt", cvtt, "native_i32", narrowed, flush=True)
    print("PASS all branch-refined PHI differential cases", flush=True)


if __name__ == "__main__":
    main()
