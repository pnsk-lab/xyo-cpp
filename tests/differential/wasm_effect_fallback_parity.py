#!/usr/bin/env python3
"""Observable Scratch effects must never be replayed by a speculative JIT.

Exercises a broadcast receiver with a numeric WASM/Binaryen hot region,
broadcast-and-wait completion, a scheduler wait, and a looks effect between
numeric regions. Compares actual output against the interpreter, LLVM and
both WASM backends. This is NOT an in-process CommitOnceEffects integration:
the existing compiler must still bridge/fallback at these boundaries.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "differential"))
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))

from wasm_hotspot_integration import fixture as known_fixture, run, run_native
from gen_algo_bench import (
    Gen, S_set, S_change, S_if, S_repeat, num, var, add, lt, op,
    make_project, write_sb3,
)


def looks_fixture(output: Path) -> Path:
    g = Gen(["x", "y"], [])
    hat = g.script([
        S_set("x", num(4)),
        S_change("x", num(3)),
        S_set("y", add(var("x"), num(1))),
        S_change("x", num(2)),
    ])
    # Insert two observable blocks into a genuine Scratch block chain,
    # preserving order and both parent / next links.
    first = g.blocks[hat]["next"]
    second = g.blocks[first]["next"]
    third = g.blocks[second]["next"]
    say = g.new("looks_say", parent=second)
    g.blocks[say]["inputs"]["MESSAGE"] = g.emit_value(var("x"), say)
    g.blocks[second]["next"] = say
    g.blocks[say]["next"] = third
    g.blocks[third]["parent"] = say

    project = make_project(["x", "y"], [], g)
    sb3 = output / "effect-looks-bridge.sb3"
    write_sb3(str(sb3), project)
    return sb3


def opaque_extension_fixture(output: Path) -> Path:
    gen = Gen(["x", "y"], [])
    hat = gen.script([
        S_set("x", num(4)),
        S_change("x", num(3)),
        S_set("y", add(var("x"), num(1))),
        S_change("x", num(2)),
    ])
    first = gen.blocks[hat]["next"]
    second = gen.blocks[first]["next"]
    third = gen.blocks[second]["next"]
    # No primitive or codegen backend knows this extension opcode. An
    # interpreter treats it as a no-op; no backend may silently compile
    # across the opaque effect or discard its source checkpoint.
    opaque = gen.new("extension_private_unknown_side_effect", parent=second)
    gen.blocks[second]["next"] = opaque
    gen.blocks[opaque]["next"] = third
    gen.blocks[third]["parent"] = opaque
    source = output / "opaque-effect-must-fallback.sb3"
    write_sb3(str(source),
              make_project(["x", "y"], [], gen))
    return source


def nested_opaque_fixture(output: Path, kind: str) -> Path:
    """Unknown opcode in a conditional, repeat body, or value expression."""
    g = Gen(["x", "y"], [])
    if kind == "unknown_reporter":
        g.script([
            S_set("x", num(4)),
            S_set("y", num(3)),
            S_change("x", add(
                num(5), op("extension_private_unknown_reporter"))),
            S_set("y", add(var("x"), num(2))),
        ])
    else:
        nested = [
            S_change("x", num(3)), S_change("x", num(2)),
        ]
        g.script([
            S_set("x", num(4)),
            (S_if(lt(var("x"), num(5)), nested)
             if kind == "unknown_if" else
             S_repeat(num(3), nested)),
            S_set("y", add(var("x"), num(2))),
        ])
        control = next(
            block for block in g.blocks.values()
            if block["opcode"] in ("control_if", "control_repeat"))
        first_id = control["inputs"]["SUBSTACK"][1]
        second_id = g.blocks[first_id]["next"]
        opaque = g.new("extension_private_unknown_side_effect",
                       parent=first_id)
        g.blocks[first_id]["next"] = opaque
        g.blocks[opaque]["next"] = second_id
        g.blocks[second_id]["parent"] = opaque
    source = output / f"opaque-effect-{kind}.sb3"
    write_sb3(str(source), make_project(["x", "y"], [], g))
    return source


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--wasm", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for kind in ("broadcast", "broadcast_wait", "unsupported_wait",
                 "looks_effect", "unknown_extension", "unknown_if",
                 "unknown_repeat", "unknown_reporter"):
        source = (looks_fixture(args.out) if kind == "looks_effect"
                  else opaque_extension_fixture(args.out)
                  if kind == "unknown_extension"
                  else nested_opaque_fixture(args.out, kind)
                  if kind.startswith("unknown_")
                  else known_fixture(args.out, kind))
        off, _ = run("node", ROOT / "web/xyo-node.mjs",
                     args.wasm, source, "off")
        builtin, _ = run(
            "node", ROOT / "web/xyo-node.mjs", args.wasm, source,
            "hotspot", initial={"XYO_WASM_HOT_EMITTER": "builtin"})
        binaryen, log = run(
            "node", ROOT / "web/xyo-node.mjs", args.wasm, source,
            "hotspot", initial={
                "XYO_WASM_HOT_EMITTER": "binaryen",
                "XYO_BINARYEN_NODE_ADDON": str(args.addon.resolve())})
        llvm = run_native(args.native, source, "hotspot")
        if len({off, builtin, binaryen, llvm}) != 1:
            raise AssertionError(
                f"{kind} mismatch: interpreter={off}, LLVM={llvm}, "
                f"builtin={builtin}, Binaryen={binaryen}")
        # The receiver/between-effects regions must use actual Binaryen,
        # but an unsupported Scratch block is never itself elided or
        # guessed into a generic numeric opcode.
        if kind in ("broadcast", "broadcast_wait") and (
            "emitter=binaryen-host" not in log or
            "xyo-wasm-hot: committed" not in log):
            raise AssertionError(f"{kind} did not execute compiled Binaryen")
        if kind.startswith("unknown_"):
            if ("xyo-wasm-hot: hybrid script=" not in log or
                "emitter=binaryen-host" not in log):
                raise AssertionError(
                    f"{kind}: no interpreter/compiled hybrid boundary")
        if kind in ("broadcast", "broadcast_wait"):
            # One broadcast starts a receiver with exactly 3 increments.
            if '"x": "3"' not in binaryen and '"x": "3"' not in off:
                raise AssertionError(f"{kind} should execute receiver once")
        print(f"PASS {kind}: Interpreter/LLVM/Builtin/Binaryen match; {off}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
