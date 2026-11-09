#!/usr/bin/env python3
"""Differential regression of transactional, tagged mixed Scratch list values."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))
from gen_algo_bench import (
    Gen, S_set, S_change, S_add, S_replace, S_del, S_delall, S_if,
    S_call, S_repeat, num, text, var, arg, add, eq, item, length, op,
    make_project, write_sb3,
)
from wasm_hotspot_integration import run, run_native


def join(a, b):
    return op("operator_join", STRING1=a, STRING2=b)


def contains(a):
    return op("data_listcontainsitem", {"LIST": "items"}, ITEM=a)


def indexof(a):
    return op("data_itemnumoflist", {"LIST": "items"}, ITEM=a)


def contents():
    return op("data_listcontents", {"LIST": "items"})


def fixture(directory: Path, kind: str) -> Path:
    gen = Gen(["x", "y", "z", "flag"], ["items"])
    if kind == "mixed_append_read":
        gen.script([
            S_delall("items"),
            S_add("items", text("Apple")),
            S_add("items", num(5)),
            S_add("items", text("世界")),
            S_set("x", item("items", num(1))),
            S_set("y", join(item("items", num(3)), text("!"))),
            S_set("z", length("items")),
        ])
    elif kind == "insert_replace":
        gen.script([
            S_delall("items"),
            S_add("items", text("first")),
            S_add("items", num(19)),
            S_replace("items", num(2), text("middle")),
            S_replace("items", num(1), join(text("new"), num(1))),
            S_set("x", item("items", num(1))),
            S_set("y", item("items", num(3))),
            S_set("z", length("items")),
            S_del("items", num(2)),
            S_set("flag", item("items", num(2))),
        ])
        insertion = [
            b for b in gen.blocks.values()
            if b["opcode"] == "data_replaceitemoflist"
            and b["inputs"]["ITEM"] == [1, [10, "middle"]]
        ]
        if len(insertion) != 1:
            raise AssertionError("expected one insertion placeholder")
        insertion[0]["opcode"] = "data_insertatlist"
    elif kind == "mixed_search":
        gen.script([
            S_delall("items"),
            S_add("items", text("alpha")),
            S_add("items", num(15)),
            S_add("items", text("BETA")),
            S_add("items", text("15")),
            S_set("x", indexof(text("beta"))),
            S_set("y", indexof(num(15))),
            S_if(contains(text("ALPHA")), [
                S_change("z", num(1)),
            ]),
            S_if(contains(num(15)), [
                S_change("z", num(10)),
            ]),
            S_if(contains(text("missing")), [
                S_change("z", num(1000)),
            ]),
        ])
    elif kind == "boolean_item":
        gen.script([
            S_delall("items"),
            S_add("items", num(0)),
            S_add("items", text("true")),
            S_set("x", item("items", num(1))),
            S_set("y", join(var("x"), text("!"))),
            S_set("z", indexof(text("true"))),
            S_if(contains(var("x")), [S_change("z", num(10))]),
        ])
        target = [
            (id_, b) for id_, b in gen.blocks.items()
            if b["opcode"] == "data_addtolist"
            and b["inputs"]["ITEM"] == [1, [4, "0"]]
        ]
        if len(target) != 1:
            raise AssertionError("expected boolean list append placeholder")
        id_, block = target[0]
        block["inputs"]["ITEM"] = gen.emit_value(
            eq(num(1), num(1)), id_, bool_slot=True)
    elif kind == "staged_order":
        gen.script([
            S_delall("items"),
            S_add("items", text("A")),
            S_add("items", text("B")),
            S_replace("items", num(1), join(item("items", num(2)), text("!"))),
            S_set("x", item("items", num(1))),
            S_del("items", num(2)),
            S_set("y", item("items", num(1))),
            S_set("z", length("items")),
        ])
    elif kind == "rollback_mixed":
        gen.define("grow %s", ["n"], [
            S_repeat(arg("n"), [
                S_add("items", join(text("s"), var("x"))),
                S_change("x", num(1)),
            ]),
            S_set("y", length("items")),
        ], warp=True)
        gen.script([
            S_delall("items"),
            S_set("x", num(0)),
            S_call("grow %s", [num(1050)]),
            S_set("z", length("items")),
        ])
    elif kind == "procedure_string_args":
        gen.define("pass %s", ["value"], [
            S_set("x", arg("value")),
            S_add("items", arg("value")),
            S_set("y", join(arg("value"), text("!"))),
        ], warp=True)
        gen.script([
            S_delall("items"),
            S_call("pass %s", [text("大阪")]),
            S_set("z", length("items")),
            S_set("flag", item("items", num(1))),
        ])
    elif kind == "procedure_nested_args":
        gen.define("inner %s", ["v"], [
            S_add("items", arg("v")),
            S_set("y", join(arg("v"), text("B"))),
        ], warp=True)
        gen.define("outer %s", ["v"], [
            S_call("inner %s", [arg("v")]),
            S_set("x", arg("v")),
        ], warp=True)
        gen.script([
            S_delall("items"),
            S_call("outer %s", [text("A")]),
            S_set("z", contents()),
        ])
    elif kind == "procedure_boolean_args":
        gen.define("pass %s", ["value"], [
            S_set("x", arg("value")),
            S_add("items", arg("value")),
            S_set("y", join(arg("value"), text("!"))),
        ], warp=True)
        gen.script([
            S_delall("items"),
            S_call("pass %s", [num(1)]),
            S_set("z", length("items")),
        ])
        candidates = [(id_, block) for id_, block in gen.blocks.items()
                      if block["opcode"] == "procedures_call"]
        if len(candidates) != 1:
            raise AssertionError("unexpected procedure call count")
        bid, block = candidates[0]
        block["inputs"]["arg0"] = gen.emit_value(
            eq(num(1), num(2)), bid, bool_slot=True)
    elif kind == "procedure_string_truth":
        gen.define("guard %s", ["value"], [
            S_if(arg("value"), [S_change("z", num(1))]),
        ], warp=True)
        gen.script([
            S_set("z", num(0)),
            S_call("guard %s", [text("hello")]),
            S_call("guard %s", [text("false")]),
            S_set("x", var("z")),
        ])
    elif kind == "procedure_string_numeric":
        gen.define("change %s", ["value"], [
            S_change("x", arg("value")),
            S_set("y", arg("value")),
        ], warp=True)
        gen.script([
            S_set("x", num(3)),
            S_call("change %s", [text("12")]),
            S_set("z", join(var("y"), text("!"))),
        ])
    elif kind == "contents_characters":
        gen.script([
            S_delall("items"),
            S_add("items", text("a")),
            S_add("items", text("あ")),
            S_add("items", text("β")),
            S_set("x", contents()),
            S_set("y", join(contents(), text("!"))),
            S_set("z", length("items")),
        ])
    elif kind == "contents_spaced":
        gen.script([
            S_delall("items"),
            S_add("items", text("a")),
            S_add("items", num(12)),
            S_add("items", text("日本語")),
            S_set("x", contents()),
            S_replace("items", num(2), text("new")),
            S_set("y", contents()),
            S_del("items", num(1)),
            S_set("z", contents()),
        ])
    elif kind == "special_indices":
        gen.script([
            S_delall("items"),
            S_add("items", text("first")),
            S_add("items", text("middle")),
            S_add("items", text("last")),
            S_set("x", item("items", text("last"))),
            S_replace("items", text("2"), text("replace")),
            S_set("y", item("items", text("2"))),
            S_del("items", text("last")),
            S_set("z", length("items")),
            S_del("items", text("all")),
            S_set("flag", length("items")),
        ])
    elif kind == "random_index_fallback":
        # A speculative index must not consume the global RNG. With a
        # singleton list, final output is deterministic, so the bailout
        # can still be compared across LLVM/interpreter/WASM/Binaryen.
        gen.script([
            S_delall("items"),
            S_add("items", text("only")),
            S_set("x", item("items", text("random"))),
            S_set("y", length("items")),
        ])
    elif kind == "invalid_index":
        gen.script([
            S_delall("items"),
            S_add("items", text("a")),
            S_add("items", text("b")),
            S_set("x", item("items", num(999))),
            S_set("y", length("items")),
        ])
    else:
        raise ValueError(kind)

    path = directory / f"{kind}.sb3"
    write_sb3(str(path), make_project(list(gen.vars), ["items"], gen))
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--addon", type=Path, required=True)
    parser.add_argument("--llvm", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    cases = (
        "mixed_append_read", "insert_replace", "mixed_search",
        "boolean_item", "staged_order", "rollback_mixed",
        "special_indices", "contents_characters", "contents_spaced",
        "procedure_string_args", "procedure_nested_args",
        "procedure_boolean_args", "procedure_string_truth",
        "procedure_string_numeric",
        "random_index_fallback", "invalid_index",
    )
    for kind in cases:
        path = fixture(args.out, kind)
        interp, _ = run("node", ROOT / "web/xyo-node.mjs", args.wasm, path,
                        "off")
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
                f"{kind}: interpreter={interp}\n"
                f"builtin={builtin}\nbinaryen={binaryen}\n"
                f"{log[-1500:]}")
        if args.llvm:
            native = run_native(args.llvm, path, "hotspot")
            if native != interp:
                raise AssertionError(
                    f"{kind}: native LLVM={native}, interp={interp}")
        if ("emitter=binaryen-host" not in log or
                "xyo-wasm-hot: run handle=" not in log):
            raise AssertionError(f"{kind}: actual Binaryen was not used")
        if kind not in ("rollback_mixed", "invalid_index",
                        "random_index_fallback") and (
                "xyo-wasm-hot: committed" not in log):
            raise AssertionError(f"{kind}: no staged list changes committed")
        if kind in ("rollback_mixed", "invalid_index",
                     "random_index_fallback") and (
                "wasm return=1" not in log):
            raise AssertionError(f"{kind}: late bailout was not exercised")
        print(f"PASS {kind}: LLVM/Interpreter/WASM/Binaryen exact parity")
        if kind != "rollback_mixed":
            print(f"  {binaryen}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
