#!/usr/bin/env python3
"""Scratch UTF-16 string/Value ABI differential tests across LLVM and WASM."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))
from gen_algo_bench import (
    Gen, S_set, S_change, S_repeat, S_call, S_if, num, text, var, arg,
    op, eq, lt, S_add, S_delall, length as list_length,
    make_project, write_sb3,
)
from wasm_hotspot_integration import run, run_native


def join(a, b):
    return op("operator_join", STRING1=a, STRING2=b)


def length(a):
    return op("operator_length", STRING=a)


def contains(a, b):
    return op("operator_contains", STRING1=a, STRING2=b)


def letter(index, value):
    return op("operator_letter_of", LETTER=index, STRING=value)


def fixture(directory: Path, kind: str) -> Path:
    lists = ["items"] if kind == "list_mixed_guard" else []
    g = Gen(["x", "y", "z", "flag"], lists)
    boolean_updates = []
    if kind == "simple_join":
        g.script([
            S_set("x", text("Hi")),
            S_set("y", join(var("x"), text(" 世界"))),
            S_set("z", join(var("y"), num(42))),
        ])
    elif kind == "nested_unicode_join":
        g.script([
            S_set("x", text("🔥")),
            S_set("y", join(join(var("x"), text("あ")),
                            join(text("β"), num(-8)))),
            S_set("z", length(var("y"))),
        ])
    elif kind == "contains":
        g.script([
            S_set("x", text("Hello世界")),
            S_set("y", num(0)),
            S_set("z", num(0)),
            S_set("flag", num(0)),
        ])
        boolean_updates.extend([
            ("y", contains(var("x"), text("HELLO"))),
            ("z", contains(var("x"), text("界"))),
            ("flag", contains(var("x"), text("missing"))),
        ])
    elif kind == "letter":
        g.script([
            S_set("x", text("あいう")),
            S_set("y", letter(num(2), var("x"))),
            S_set("z", length(var("y"))),
            S_set("flag", join(var("y"), text("Z"))),
        ])
    elif kind == "compare":
        g.script([
            S_set("x", text("cat")),
            S_set("y", text("DOG")),
            S_set("z", num(0)),
            S_set("flag", num(0)),
        ])
        boolean_updates.extend([
            ("z", eq(var("x"), var("y"))),
            ("flag", lt(var("x"), var("y"))),
        ])
    elif kind == "string_boolean":
        g.script([
            S_set("x", text("hello")),
            S_set("y", text("false")),
            S_set("z", num(0)),
            S_if(var("x"), [S_change("z", num(1))]),
            S_if(var("y"), [S_change("z", num(100))]),
            S_set("flag", num(0)),
        ])
        boolean_updates.append((
            "flag", op("operator_not", OPERAND=var("y"))))
    elif kind == "join_mixed_bool":
        g.script([
            S_set("x", num(0)),
            S_set("y", join(var("x"), text("!"))),
            S_set("z", join(num(123), var("y"))),
        ])
        source = next(
            key for key, item in g.blocks.items()
            if item["opcode"] == "data_setvariableto"
            and item["fields"]["VARIABLE"][1] == g.vars["x"]
        )
        g.blocks[source]["inputs"]["VALUE"] = g.emit_value(
            eq(num(1), num(2)), source, bool_slot=True)
    elif kind == "list_mixed_guard":
        # Roll back the earlier numeric append if the second append reads a
        # tagged string. The interpreter must append both *once*.
        g.script([
            S_delall("items"),
            S_set("x", num(5)),
            S_add("items", var("x")),
            S_set("x", text("hello")),
            S_add("items", var("x")),
            S_set("z", list_length("items")),
            S_set("y", var("x")),
        ])
    elif kind == "join_rollback":
        g.define("grow %s", ["n"], [
            S_repeat(arg("n"), [
                S_set("x", join(var("x"), text("x"))),
            ]),
            S_set("y", length(var("x"))),
        ], warp=True)
        g.script([
            S_set("x", text("base")),
            S_call("grow %s", [num(1030)]),
            S_set("z", length(var("x"))),
        ])
    else:
        raise ValueError(kind)

    for variable, expr in boolean_updates:
        key = next(
            key for key, block in g.blocks.items()
            if block["opcode"] == "data_setvariableto"
            and block["fields"]["VARIABLE"][1] == g.vars[variable]
        )
        g.blocks[key]["inputs"]["VALUE"] = g.emit_value(
            expr, key, bool_slot=True)
    project = make_project(list(g.vars.keys()), lists, g)
    path = directory / (kind + ".sb3")
    write_sb3(str(path), project)
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--addon", type=Path, required=True)
    parser.add_argument("--llvm", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    for kind in (
        "simple_join", "nested_unicode_join", "contains", "letter",
        "compare", "string_boolean", "join_mixed_bool",
        "list_mixed_guard", "join_rollback",
    ):
        path = fixture(args.out, kind)
        interp, _ = run("node", ROOT / "web/xyo-node.mjs", args.wasm,
                        path, "off")
        builtin, _ = run(
            "node", ROOT / "web/xyo-node.mjs", args.wasm, path, "hotspot",
            initial={"XYO_WASM_HOT_EMITTER": "builtin"})
        binaryen, log = run(
            "node", ROOT / "web/xyo-node.mjs", args.wasm, path, "hotspot",
            initial={
                "XYO_WASM_HOT_EMITTER": "binaryen",
                "XYO_BINARYEN_NODE_ADDON": str(args.addon.resolve()),
            })
        if not (interp == builtin == binaryen):
            raise AssertionError(
                f"{kind}: interp={interp}; builtin={builtin}; "
                f"binaryen={binaryen}\n{log[-1800:]}")
        if args.llvm:
            native = run_native(args.llvm, path, "hotspot")
            if native != interp:
                raise AssertionError(
                    f"{kind}: LLVM={native}; interp={interp}")
        if "emitter=binaryen-host" not in log or (
            "xyo-wasm-hot: committed" not in log
            and kind != "join_rollback"
        ):
            raise AssertionError(f"{kind}: expected actual Binaryen commit")
        if kind == "join_rollback" and (
            "wasm return=1" not in log or "y\": \"1034" not in binaryen
        ):
            raise AssertionError(
                f"{kind}: transactional UTF-16 rollback not demonstrated")
        if kind == "list_mixed_guard" and (
                "wasm return=0" not in log or
                "\"items\": [\"5\", \"hello\"]" not in binaryen):
            raise AssertionError(
                "mixed tagged list write should stage/commit exactly")
        print(f"PASS {kind}: interpreter/builtin/Binaryen/LLVM parity; "
              f"Binaryen executed")
        if kind != "join_rollback":
            print("  " + binaryen)
    return 0


if __name__ == "__main__":
    sys.exit(main())
