#!/usr/bin/env python3
"""Differential tests for bounded (return PC, numeric argument frame) WASM LIFO."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_repeat, S_set, S_add,
    add, arg, lt, num, text, var, make_project, write_sb3,
)
from wasm_nested_continuation import run


CASES = (
    "two_sites_one_arg",
    "two_sites_zero_args",
    "one_site_two_args",
    "two_sites_two_args",
    "two_sites_fuel_overflow",
    "two_sites_list_effect",
    "two_sites_nonwarp",
    "three_sites_rejected",
    "two_sites_string_initial",
    "two_sites_dynamic_next",
)


def make_case(folder: Path, kind: str) -> Path:
    gen = Gen(["x", "y", "z", "i"], ["items"])
    zero_args = kind == "two_sites_zero_args"
    two_args = kind in ("one_site_two_args", "two_sites_two_args")
    signature = "leaf" if zero_args else "leaf %s %s" if two_args else "leaf %s"
    names = [] if zero_args else ["a", "b"] if two_args else ["a"]
    overflow = kind == "two_sites_fuel_overflow"
    list_effect = kind == "two_sites_list_effect"
    nonwarp = kind == "two_sites_nonwarp"
    string_initial = kind == "two_sites_string_initial"
    bool_next = kind == "two_sites_dynamic_next"
    three = kind == "three_sites_rejected"
    one = kind == "one_site_two_args"
    limit = 150 if overflow else 5

    if zero_args:
        cond1 = lt(var("y"), num(4))
        cond2 = lt(var("y"), num(5))
        arg1, arg2 = [], []
    else:
        cond1 = lt(arg("a"), num(limit))
        cond2 = lt(arg("a"), num(2))
        if two_args:
            arg1 = [add(arg("a"), num(1)), add(arg("b"), arg("a"))]
            arg2 = [add(arg("a"), num(2)), add(arg("b"), num(3))]
        else:
            arg1 = [var("x")] if bool_next else [
                add(arg("a"), num(1))
            ]
            arg2 = [add(arg("a"), num(2))]

    prefix = [
        S_repeat(num(8), [S_change("x", num(1))]) if overflow else
        S_change("x", num(1)),
        S_change("y", num(1)),
    ]
    mid = [S_change("x", num(2))]
    suffix = [
        S_change("z", arg("a")) if not zero_args else
        S_change("z", var("x")),
        S_add("items", num(1)) if list_effect else
        S_change("z", num(3)),
    ]
    if two_args:
        suffix.append(S_change("z", arg("b")))
    if three:
        body = prefix + [
            S_if(cond1, [S_call(signature, arg1)]),
            *mid,
            S_if(cond2, [S_call(signature, arg2)]),
            S_change("x", num(5)),
            S_if(lt(arg("a"), num(1)), [S_call(signature, arg1)]),
            *suffix,
        ]
    else:
        body = prefix + [
            S_if(cond1, [S_call(signature, arg1)]),
            *mid,
            *([] if one else [S_if(cond2, [S_call(signature, arg2)])]),
            *suffix,
        ]
    gen.define(signature, names, body, warp=not nonwarp)
    first_arguments = (
        [] if zero_args else
        [num(0), num(1)] if two_args else
        [text("0") if string_initial else num(0)]
    )
    gen.define("worker", [], [
        S_set("x", num(0)),
        S_set("y", num(0)),
        S_set("z", num(0)),
        S_repeat(num(1), [S_call(signature, first_arguments)]),
    ], warp=True)
    calls = 80 if overflow else 210
    hat = gen.script([S_repeat(num(calls), [S_call("worker")])])
    first = gen.blocks[hat]["next"]
    wait = gen.new("control_wait", parent=hat)
    gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
    gen.blocks[hat]["next"] = wait
    gen.blocks[wait]["next"] = first
    gen.blocks[first]["parent"] = wait

    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"{kind}.sb3"
    write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
    return path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--addon", type=Path, required=True)
    parser.add_argument("--llvm", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    for kind in CASES:
        path = make_case(args.out, kind)
        reference, _ = run(args.wasm, path, "off")
        disabled, _ = run(
            args.wasm, path, "hotspot", addon=args.addon, nested=False
        )
        builtin, builtin_diag = run(args.wasm, path, "hotspot")
        binaryen, binaryen_diag = run(
            args.wasm, path, "hotspot", addon=args.addon
        )
        llvm, _ = run(args.llvm, path, "hotspot", native=True)
        if len({reference, disabled, builtin, binaryen, llvm}) != 1:
            raise AssertionError(
                f"{kind}: output divergence\n"
                f"reference={reference}\ndisabled={disabled}\n"
                f"builtin={builtin}\nbinaryen={binaryen}\nllvm={llvm}"
            )
        for backend, diag in (("builtin", builtin_diag),
                              ("binaryen", binaryen_diag)):
            match = re.search(
                r"pc_compiled=(\d+) pc_runs=(\d+) pc_bails=(\d+)",
                diag,
            )
            if not match:
                raise AssertionError(f"{kind}: {backend} lacks PC statistics")
            compiled, executed, bailed = map(int, match.groups())
            rejected = kind in (
                "two_sites_list_effect", "two_sites_nonwarp",
                "three_sites_rejected", "two_sites_string_initial",
                "two_sites_dynamic_next",
            )
            if rejected:
                if compiled != 0 or executed != 0:
                    raise AssertionError(
                        f"{kind}: unsafe PC-stack compiled: {match.group(0)}"
                    )
            elif kind == "two_sites_fuel_overflow":
                if compiled < 1 or executed != 0 or bailed < 2:
                    raise AssertionError(
                        f"{kind}: shared-fuel rollback failed: "
                        f"{match.group(0)}"
                    )
            else:
                if compiled < 1 or executed < 100 or bailed != 0:
                    raise AssertionError(
                        f"{kind}: compiled return-PC stack unused: "
                        f"{match.group(0)}"
                    )
        print(f"PASS {kind}: 5-way exact output, verified PC stack", flush=True)
    print(f"PASS {len(CASES)} return-PC/argument-frame fixtures", flush=True)


if __name__ == "__main__":
    main()
