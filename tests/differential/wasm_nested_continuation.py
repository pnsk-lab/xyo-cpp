#!/usr/bin/env python3
"""Yield-preserving nested WASM body acceleration; 5-way Scratch parity.

Tests one interpreted non-warp repeat iteration per scheduler tick while
compiling only the straight-line body at the existing StackFrame checkpoint.
Unlike a whole-script output comparison, requires actual nested WASM runs.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_set, S_change, S_repeat, S_if, S_ifelse, S_add,
    S_call, S_until, num, var, arg, text, add, lt, eq, op,
    make_project, write_sb3,
)


def create(directory: Path, kind: str) -> Path:
    gen = Gen(["x", "y", "z", "i"], ["items"])
    if kind == "nonwarp_repeat":
        gen.define("worker %s", ["n"], [
            S_repeat(num(240), [
                S_change("x", num(1)),
                S_set("y", add(var("x"), num(2))),
            ]),
            S_set("z", var("x")),
        ], warp=False)
        gen.script([S_set("x", num(0)), S_call("worker %s", [num(240)])])
    elif kind == "nonwarp_if":
        gen.define("worker", [], [
            S_repeat(num(230), [
                S_change("x", num(1)),
                S_if(lt(var("x"), num(20)), [
                    S_change("y", num(3)),
                ]),
                S_change("z", num(2)),
            ]),
        ], warp=False)
        gen.script([S_call("worker")])
    elif kind in ("nonwarp_ifelse", "nonwarp_ifelse_interleaved"):
        gen.define("worker", [], [
            S_repeat(num(230), [
                S_change("x", num(1)),
                S_ifelse(lt(var("x"), num(160)),
                         [S_change("y", num(2))],
                         [S_change("z", num(3))]),
            ]),
        ], warp=False)
        gen.script([S_call("worker")])
        if kind == "nonwarp_ifelse_interleaved":
            # Another thread changes the predicate's source between
            # nonwarp iterations; a resumed JIT must reload live slots.
            gen.script([S_repeat(num(230), [
                S_change("x", num(1)),
            ])])
    elif kind == "warp_repeat":
        # Exceeds full-script JIT's 1024-iteration fuel, forcing an
        # interpreter-owned continuation and nested statement compilation.
        gen.define("worker", [], [
            S_repeat(num(1300), [
                S_change("x", num(1)),
                S_set("y", add(var("x"), num(4))),
            ]),
            S_set("z", var("x")),
        ], warp=True)
        gen.script([S_call("worker")])
    elif kind == "warp_batched":
        # The root non-warp repeat may not be fused: it invokes a warp
        # procedure and yields after every call. Inside the warp procedure,
        # one pure numeric repeat can execute transactionally in a single
        # WebAssembly call after 64 warmup invocations.
        gen.define("worker", [], [
            S_repeat(num(320), [
                S_change("x", num(1)),
                S_set("y", add(var("x"), num(7))),
            ]),
        ], warp=True)
        hat = gen.script([
            S_repeat(num(210), [S_call("worker")]),
            S_set("z", var("x")),
        ])
        first = gen.blocks[hat]["next"]
        wait = gen.new("control_wait", parent=hat)
        gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
        gen.blocks[hat]["next"] = wait
        gen.blocks[wait]["next"] = first
        gen.blocks[first]["parent"] = wait
    elif kind in ("warp_until", "warp_while", "warp_while_overflow",
                  "warp_until_empty"):
        limit = 1100 if kind == "warp_while_overflow" else 180
        predicate = (eq(var("x"), num(0)) if kind == "warp_until_empty"
                     else eq(var("x"), num(limit)) if kind == "warp_until"
                     else lt(var("x"), num(limit)))
        gen.define("worker", [], [
            S_set("x", num(0)),
            S_until(predicate, [
                S_change("x", num(1)),
                S_change("y", num(2)),
            ]),
            S_set("z", var("x")),
        ], warp=True)
        if kind in ("warp_while", "warp_while_overflow"):
            for block in gen.blocks.values():
                if block["opcode"] == "control_repeat_until":
                    block["opcode"] = "control_while"
                    break
        calls = 80 if kind == "warp_while_overflow" else 210
        hat = gen.script([
            S_repeat(num(calls), [S_call("worker")]),
            S_set("z", var("y")),
        ])
        first = gen.blocks[hat]["next"]
        wait = gen.new("control_wait", parent=hat)
        gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
        gen.blocks[hat]["next"] = wait
        gen.blocks[wait]["next"] = first
        gen.blocks[first]["parent"] = wait
    elif kind in ("warp_foreach", "warp_foreach_dynamic",
                  "warp_foreach_overflow", "warp_foreach_empty",
                  "warp_foreach_fractional", "warp_foreach_mutated_index",
                  "warp_foreach_string_bound"):
        limit = (1100 if kind == "warp_foreach_overflow" else
                 0 if kind == "warp_foreach_empty" else
                 2.5 if kind == "warp_foreach_fractional" else
                 3.5 if kind == "warp_foreach_string_bound" else 180)
        dynamic = kind == "warp_foreach_dynamic"
        stringBound = kind == "warp_foreach_string_bound"
        body = [
            S_change("x", var("i")),
            S_change("y", num(1)),
        ]
        if dynamic:
            body.append(S_change("z", num(-1)))
        if kind == "warp_foreach_mutated_index":
            body.append(S_set("i", num(999)))
        gen.define("worker", [], [
            S_set("z", text("3.5") if stringBound else num(limit)),
            S_repeat(num(limit), body),
        ], warp=True)
        candidate = [(bid, block) for bid, block in gen.blocks.items()
                     if block["opcode"] == "control_repeat"]
        if len(candidate) != 1:
            raise AssertionError("expected one warp procedure for-each loop")
        bid, block = candidate[0]
        block["opcode"] = "control_for_each"
        block["fields"]["VARIABLE"] = gen.field("VARIABLE", "i")
        block["inputs"].pop("TIMES")
        block["inputs"]["VALUE"] = gen.emit_value(
            var("z") if (dynamic or stringBound) else num(limit), bid)
        calls = 80 if kind == "warp_foreach_overflow" else 210
        hat = gen.script([
            S_repeat(num(calls), [S_call("worker")]),
        ])
        first = gen.blocks[hat]["next"]
        wait = gen.new("control_wait", parent=hat)
        gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
        gen.blocks[hat]["next"] = wait
        gen.blocks[wait]["next"] = first
        gen.blocks[first]["parent"] = wait
    elif kind in ("warp_nested_repeat", "warp_nested_conditional",
                  "warp_nested_if", "warp_nested_overflow",
                  "warp_nested_list_effect"):
        # A single warp procedure call owns the entire nested loop tree.
        # The outer Scratch repeat is non-warp and the explicit initial
        # wait forces the interpreter to retain all original frames.
        if kind == "warp_nested_repeat":
            outer = S_repeat(num(20), [
                S_repeat(num(15), [
                    S_change("x", num(1)),
                    S_change("y", num(2)),
                ]),
                S_change("z", num(1)),
            ])
        elif kind == "warp_nested_conditional":
            outer = S_repeat(num(12), [
                S_set("i", num(0)),
                S_until(lt(num(16), var("i")), [
                    S_change("i", num(1)),
                    S_change("x", num(1)),
                ]),
                S_if(lt(var("x"), num(120)), [
                    S_change("y", num(3)),
                ]),
            ])
        elif kind == "warp_nested_if":
            outer = S_repeat(num(210), [
                S_change("x", num(1)),
                S_if(lt(var("x"), num(60)), [
                    S_change("y", num(2)),
                ]),
            ])
        elif kind == "warp_nested_overflow":
            # Each bound is individually below 1024, but total nested
            # iterations exceed the shared fuel (30 + 30*40).
            outer = S_repeat(num(30), [
                S_repeat(num(40), [
                    S_change("x", num(1)),
                ]),
            ])
        else:
            # List mutation is an observable effect. Reject the complete
            # speculative warp tree even when inner arithmetic is pure.
            outer = S_repeat(num(12), [
                S_repeat(num(15), [
                    S_change("x", num(1)),
                    S_add("items", num(1)),
                ]),
            ])
        gen.define("nested-worker", [], [
            outer,
            S_set("z", var("x")),
        ], warp=True)
        calls = 80 if kind in ("warp_nested_overflow",
                                "warp_nested_list_effect") else 210
        hat = gen.script([
            S_repeat(num(calls), [S_call("nested-worker")]),
            S_set("y", add(var("y"), var("z"))),
        ])
        first = gen.blocks[hat]["next"]
        wait = gen.new("control_wait", parent=hat)
        gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
        gen.blocks[hat]["next"] = wait
        gen.blocks[wait]["next"] = first
        gen.blocks[first]["parent"] = wait
    elif kind in ("warp_leaf_inline", "warp_nested_leaf_loop",
                  "warp_nonwarp_leaf_blocked",
                  "warp_recursive_leaf_blocked"):
        # The source frontend captures referenced procedure definitions.
        # Only zero-arg, nonrecursive warp helpers are safe to collapse
        # without recreating separate argument/return StackFrames.
        if kind == "warp_nested_leaf_loop":
            leaf_body = [S_repeat(num(5), [S_change("x", num(1))])]
        elif kind == "warp_recursive_leaf_blocked":
            leaf_body = [
                S_change("x", num(1)),
                # A noncanonical recursive IF body cannot use the one-PC
                # return stack: work remains *inside* the recursive arm.
                S_if(lt(var("x"), num(3)), [
                    S_call("leaf"),
                    S_change("z", num(2)),
                ]),
                S_change("z", num(1)),
            ]
        else:
            leaf_body = [
                S_change("x", num(1)),
                S_if(lt(var("x"), num(40)), [
                    S_change("y", num(2)),
                ]),
            ]
        gen.define(
            "leaf", [], leaf_body,
            warp=kind != "warp_nonwarp_leaf_blocked")
        repeat = 35 if kind in ("warp_nested_leaf_loop",
                                "warp_recursive_leaf_blocked") else 210
        body = []
        if kind == "warp_recursive_leaf_blocked":
            body.append(S_set("x", num(0)))
        body.extend([S_call("leaf"), S_change("z", num(1))])
        gen.define("worker", [], [
            S_repeat(num(repeat), body),
        ], warp=True)
        calls = 80 if "blocked" in kind else 210
        hat = gen.script([S_repeat(num(calls), [S_call("worker")])])
        first = gen.blocks[hat]["next"]
        wait = gen.new("control_wait", parent=hat)
        gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
        gen.blocks[hat]["next"] = wait
        gen.blocks[wait]["next"] = first
        gen.blocks[first]["parent"] = wait
    elif kind in ("warp_tail_recursive", "warp_tail_recursive_nested",
                  "warp_tail_recursive_overflow",
                  "warp_non_tail_recursive", "nonwarp_tail_recursive",
                  "warp_tail_recursive_arguments",
                  "warp_tail_recursive_arguments_dynamic",
                  "warp_tail_recursive_arguments_string",
                  "warp_tail_recursive_arguments_overflow"):
        # Canonical self-tail call: numeric prefix followed by the final
        # if-condition/recursive-call block. Every recursive entry consumes
        # the shared 1024-step transaction budget.
        nested = kind == "warp_tail_recursive_nested"
        overflow = kind in ("warp_tail_recursive_overflow",
                            "warp_tail_recursive_arguments_overflow")
        non_tail = kind == "warp_non_tail_recursive"
        non_warp = kind == "nonwarp_tail_recursive"
        with_args = kind.startswith("warp_tail_recursive_arguments")
        dynamic_arg = kind == "warp_tail_recursive_arguments_dynamic"
        string_arg = kind == "warp_tail_recursive_arguments_string"
        name = "leaf %s" if with_args else "leaf"
        depth = 180 if overflow else 8
        step = (S_repeat(num(6), [S_change("x", num(1))])
                if overflow else
                S_repeat(num(4), [S_change("x", num(1))])
                if nested else S_change("x", num(1)))
        prefix = [step, S_change("y", num(1))]
        if with_args:
            prefix.append(S_change("x", arg("n")))
            condition = lt(arg("n"), num(depth))
            recurse = S_call(name, [add(arg("n"), num(1))])
        else:
            condition = lt(var("y"), num(depth))
            recurse = S_call(name)
        body = prefix + [S_if(condition, [recurse])]
        if non_tail:
            # Executed during stack unwinding: not eligible for do/while.
            body.append(S_change("z", num(1)))
        gen.define(name, ["n"] if with_args else [], body,
                   warp=not non_warp)
        gen.define("worker", [], [
            S_set("x", num(0)),
            S_set("y", num(0)),
            S_set("i", num(0)),
            S_repeat(num(1), [
                S_call(name, [text("0") if string_arg else
                              var("i") if dynamic_arg else num(0)]
                       if with_args else []),
            ]),
            S_change("z", num(3)),
        ], warp=True)
        calls = 80 if overflow else 210
        hat = gen.script([
            S_repeat(num(calls), [S_call("worker")]),
        ])
        first = gen.blocks[hat]["next"]
        wait = gen.new("control_wait", parent=hat)
        gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
        gen.blocks[hat]["next"] = wait
        gen.blocks[wait]["next"] = first
        gen.blocks[first]["parent"] = wait
    elif kind in ("warp_non_tail_nested", "warp_non_tail_overflow",
                  "warp_non_tail_unwind_overflow",
                  "warp_non_tail_effect", "warp_non_tail_broken"):
        overflow = kind == "warp_non_tail_overflow"
        unwind_overflow = kind == "warp_non_tail_unwind_overflow"
        nested = kind == "warp_non_tail_nested"
        effect = kind == "warp_non_tail_effect"
        broken = kind == "warp_non_tail_broken"
        depth = 180 if overflow else 25 if unwind_overflow else 9
        prefix = [
            S_repeat(num(6 if overflow else 3), [
                S_change("x", num(1)),
            ]) if (overflow or nested) else S_change("x", num(1)),
            S_change("y", num(1)),
        ]
        arm = [S_call("leaf")]
        if broken:
            arm.append(S_change("z", num(8)))
        suffix = (
            [S_add("items", num(1))] if effect else
            [S_repeat(num(50), [S_change("z", num(1))])]
            if unwind_overflow else
            [S_repeat(num(4), [S_change("z", num(1))])]
            if nested else [S_change("z", num(1))]
        )
        gen.define("leaf", [], prefix + [
            S_if(lt(var("y"), num(depth)), arm),
            *suffix,
        ], warp=True)
        gen.define("worker", [], [
            S_set("x", num(0)),
            S_set("y", num(0)),
            S_set("z", num(0)),
            S_repeat(num(1), [S_call("leaf")]),
        ], warp=True)
        calls = 80 if (overflow or unwind_overflow) else 210
        hat = gen.script([S_repeat(num(calls), [S_call("worker")])])
        first = gen.blocks[hat]["next"]
        wait = gen.new("control_wait", parent=hat)
        gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
        gen.blocks[hat]["next"] = wait
        gen.blocks[wait]["next"] = first
        gen.blocks[first]["parent"] = wait
    elif kind in ("warp_tail_recursive_two_arguments",
                  "warp_tail_recursive_two_arguments_swap",
                  "warp_tail_recursive_two_arguments_string"):
        # Two independently captured numeric parameters. In particular,
        # the Fibonacci-style swap requires both next-frame expressions
        # to read OLD argument values (simultaneous eager binding).
        swap = kind == "warp_tail_recursive_two_arguments_swap"
        invalid = kind == "warp_tail_recursive_two_arguments_string"
        signature = "tail %s %s"
        body = [
            S_change("i", num(1)),
            S_change("x", arg("a")),
            S_change("y", arg("b")),
        ]
        if swap:
            condition = lt(var("i"), num(7))
            recursiveArgs = [arg("b"), add(arg("a"), arg("b"))]
            initialArgs = [num(1), num(1)]
        else:
            condition = lt(num(1), arg("a"))
            recursiveArgs = [add(arg("a"), num(-1)),
                             op("operator_multiply", NUM1=arg("b"),
                                NUM2=arg("a"))]
            initialArgs = [text("6") if invalid else num(6), num(1)]
        body.append(S_if(condition, [S_call(signature, recursiveArgs)]))
        gen.define(signature, ["a", "b"], body, warp=True)
        gen.define("worker", [], [
            S_set("x", num(0)),
            S_set("y", num(0)),
            S_set("i", num(0)),
            S_repeat(num(1), [S_call(signature, initialArgs)]),
        ], warp=True)
        hat = gen.script([
            S_repeat(num(210), [S_call("worker")]),
        ])
        first = gen.blocks[hat]["next"]
        wait = gen.new("control_wait", parent=hat)
        gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
        gen.blocks[hat]["next"] = wait
        gen.blocks[wait]["next"] = first
        gen.blocks[first]["parent"] = wait
    elif kind in ("warp_tail_recursive_four_arguments",
                  "warp_tail_recursive_five_arguments"):
        count = 5 if kind == "warp_tail_recursive_five_arguments" else 4
        names = ["a", "b", "c", "d", "e"][:count]
        signature = "tail " + " ".join("%s" for _ in names)
        prefix = [
            S_change("x", arg("a")),
            S_change("y", arg("b")),
            S_change("z", add(arg("c"), arg("d"))),
        ]
        recursiveArgs = [
            add(arg("a"), num(1)),
            add(arg("b"), arg("a")),
            add(arg("c"), arg("b")),
            add(arg("d"), arg("c")),
        ]
        if count == 5:
            recursiveArgs.append(add(arg("e"), arg("d")))
        gen.define(signature, names, prefix + [
            S_if(lt(arg("a"), num(7)),
                 [S_call(signature, recursiveArgs)]),
        ], warp=True)
        gen.define("worker", [], [
            S_set("x", num(0)),
            S_set("y", num(0)),
            S_set("z", num(0)),
            S_repeat(num(1), [
                S_call(signature, [num(i) for i in range(count)]),
            ]),
        ], warp=True)
        hat = gen.script([S_repeat(num(210), [S_call("worker")])])
        first = gen.blocks[hat]["next"]
        wait = gen.new("control_wait", parent=hat)
        gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
        gen.blocks[hat]["next"] = wait
        gen.blocks[wait]["next"] = first
        gen.blocks[first]["parent"] = wait
    elif kind == "warp_overflow":
        # A static repeat > the emitter's fuel guard must bail *before*
        # changing variables, then execute the whole block in Interpreter.
        gen.define("worker", [], [
            S_repeat(num(1100), [
                S_change("x", num(1)),
                S_set("y", add(var("x"), num(3))),
            ]),
        ], warp=True)
        hat = gen.script([
            S_repeat(num(80), [S_call("worker")]),
            S_set("z", var("x")),
        ])
        first = gen.blocks[hat]["next"]
        wait = gen.new("control_wait", parent=hat)
        gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
        gen.blocks[hat]["next"] = wait
        gen.blocks[wait]["next"] = first
        gen.blocks[first]["parent"] = wait
    elif kind == "concurrent_writes":
        # The first thread's cached Variable* must load fresh values from
        # the second thread on *each* non-warp scheduler pass. Two hats
        # intentionally compete for the same scalar.
        gen.define("worker", [], [
            S_repeat(num(220), [
                S_change("x", num(1)),
                S_set("y", add(var("x"), num(2))),
            ]),
            S_set("z", var("y")),
        ], warp=False)
        gen.script([S_call("worker")])
        gen.script([S_repeat(num(220), [S_change("x", num(2))])])
    elif kind == "wait_inside_loop":
        gen.define("worker", [], [
            S_repeat(num(150), [
                S_change("x", num(1)),
                S_change("y", num(3)),
            ]),
            S_set("z", var("x")),
        ], warp=False)
        gen.script([S_call("worker")])
        loop = next(
            block for block in gen.blocks.values()
            if block["opcode"] == "control_repeat"
        )
        first = loop["inputs"]["SUBSTACK"][1]
        second = gen.blocks[first]["next"]
        wait = gen.new("control_wait", parent=first)
        gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(
            num(0), wait)
        gen.blocks[first]["next"] = wait
        gen.blocks[wait]["next"] = second
        gen.blocks[second]["parent"] = wait
    elif kind in ("nonwarp_until", "nonwarp_while", "nonwarp_foreach"):
        gen.define("worker", [], [
            S_until(lt(num(179), var("x")), [
                S_change("x", num(1)),
                S_change("y", num(1)),
            ]),
        ], warp=False)
        if kind == "nonwarp_while":
            for block in gen.blocks.values():
                if block["opcode"] == "control_repeat_until":
                    block["opcode"] = "control_while"
                    block["inputs"]["CONDITION"] = gen.emit_value(
                        lt(var("x"), num(180)),
                        next(bid for bid, b in gen.blocks.items()
                             if b is block), bool_slot=True)
                    break
        elif kind == "nonwarp_foreach":
            for bid, block in gen.blocks.items():
                if block["opcode"] != "control_repeat_until":
                    continue
                block["opcode"] = "control_for_each"
                block["fields"]["VARIABLE"] = gen.field("VARIABLE", "i")
                block["inputs"].pop("CONDITION")
                block["inputs"]["VALUE"] = gen.emit_value(num(180), bid)
                break
        gen.script([S_call("worker")])
        gen.script([S_repeat(num(180), [S_change("z", num(1))])])
    elif kind == "mixed_list":
        gen.define("worker", [], [
            S_repeat(num(240), [
                S_change("x", num(1)),
                S_add("items", text("a")),
            ]),
            S_set("z", var("x")),
        ], warp=False)
        gen.script([S_call("worker")])
    else:
        raise ValueError(kind)
    path = directory / f"{kind}.sb3"
    write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
    return path


def run(wasm: Path, sb3: Path, mode: str, *, addon: Path | None = None,
        native: bool = False, nested: bool = True) -> tuple[str, str]:
    env = dict(
        os.environ,
        XYO_DUMPVARS="json",
        XYO_EXIT_ON_DONE="1",
        XYO_PAR="0",
        XYO_HOT_INVOCATIONS="1",
        XYO_JIT_WARP_CLOCK="0",
        XYO_NESTED_WASM_STATS="1",
        XYO_NESTED_WASM_ALL="1",
        XYO_NESTED_WASM="1" if nested else "0",
    )
    if not native:
        env["XYO_WASM_HOT_EMITTER"] = "binaryen" if addon else "builtin"
        if addon:
            env["XYO_BINARYEN_NODE_ADDON"] = str(addon.resolve())
        command = [
            "node", str(ROOT / "web/xyo-node.mjs"), str(wasm.resolve()),
            "run", str(sb3), "--jit", mode, "--frames", "500", "--fps", "0",
        ]
    else:
        command = [
            str(wasm.resolve()), "run", str(sb3), "--jit", mode,
            "--frames", "500", "--fps", "0",
        ]
        env["XYO_JIT_ASYNC"] = "0"
    result = subprocess.run(
        command, env=env, cwd=ROOT, capture_output=True, text=True,
        timeout=100,
    )
    if result.returncode:
        raise AssertionError(
            f"{sb3.name} {mode}: exit={result.returncode}: "
            f"{result.stdout[-1000:]}\n{result.stderr[-1500:]}")
    start = result.stdout.find("{")
    if start < 0:
        raise AssertionError(f"missing dump for {sb3.name}: {result.stdout}")
    values = json.JSONDecoder().raw_decode(result.stdout[start:])[0]
    return json.dumps(values["Bench"], sort_keys=True), result.stderr


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--addon", type=Path, required=True)
    parser.add_argument("--llvm", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for kind in ("nonwarp_repeat", "nonwarp_if",
                 "nonwarp_ifelse", "nonwarp_ifelse_interleaved",
                 "warp_repeat",
                 "mixed_list", "concurrent_writes", "wait_inside_loop",
                 "warp_batched", "warp_overflow", "warp_until",
                 "warp_while", "warp_while_overflow",
                 "warp_until_empty", "warp_foreach",
                 "warp_foreach_dynamic", "warp_foreach_overflow",
                 "warp_foreach_empty", "warp_foreach_fractional",
                 "warp_foreach_mutated_index", "warp_foreach_string_bound",
                 "nonwarp_until", "nonwarp_while", "nonwarp_foreach",
                 "warp_nested_repeat", "warp_nested_conditional",
                 "warp_nested_if", "warp_nested_overflow",
                 "warp_nested_list_effect", "warp_leaf_inline",
                 "warp_nested_leaf_loop", "warp_nonwarp_leaf_blocked",
                 "warp_recursive_leaf_blocked",
                 "warp_tail_recursive", "warp_tail_recursive_nested",
                 "warp_tail_recursive_overflow",
                 "warp_non_tail_recursive", "nonwarp_tail_recursive",
                 "warp_tail_recursive_arguments",
                 "warp_tail_recursive_arguments_dynamic",
                 "warp_tail_recursive_arguments_string",
                 "warp_tail_recursive_arguments_overflow",
                 "warp_tail_recursive_two_arguments",
                 "warp_tail_recursive_two_arguments_swap",
                 "warp_tail_recursive_two_arguments_string",
                 "warp_tail_recursive_four_arguments",
                 "warp_tail_recursive_five_arguments",
                 "warp_non_tail_nested", "warp_non_tail_overflow",
                 "warp_non_tail_unwind_overflow",
                 "warp_non_tail_effect", "warp_non_tail_broken"):
        sb3 = create(args.out, kind)
        interpreter, _ = run(args.wasm, sb3, "off")
        without_nested, _ = run(
            args.wasm, sb3, "hotspot", addon=args.addon, nested=False)
        builtin, _ = run(args.wasm, sb3, "hotspot")
        binaryen, diag = run(
            args.wasm, sb3, "hotspot", addon=args.addon)
        llvm, _ = run(args.llvm, sb3, "hotspot", native=True)
        if len({interpreter, without_nested, builtin, binaryen, llvm}) != 1:
            raise AssertionError(
                f"{kind} output mismatch\ninterp={interpreter}\n"
                f"without_nested={without_nested}\nbuiltin={builtin}\n"
                f"binaryen={binaryen}\nllvm={llvm}\n"
                f"diagnostics={diag[-1800:]}")
        stats = re.search(
            r"\[nested-wasm\] attempted=(\d+) compiled=(\d+) "
            r"executed=(\d+) fast=(\d+) bailed=(\d+)", diag)
        if not stats:
            raise AssertionError(f"{kind}: missing nested JIT statistics")
        attempted, compiled, executed, fast, bailed = map(
            int, stats.groups())
        warp = re.search(
            r"warp_compiled=(\d+) warp_runs=(\d+) warp_bails=(\d+)",
            diag)
        if not warp:
            raise AssertionError(f"{kind}: missing warp statistics")
        warp_compiled, warp_runs, warp_bails = map(int, warp.groups())
        tail = re.search(
            r"tail_compiled=(\d+) tail_runs=(\d+) tail_bails=(\d+)",
            diag)
        if not tail:
            raise AssertionError(f"{kind}: missing tail-recursion counters")
        tail_compiled, tail_runs, tail_bails = map(int, tail.groups())
        returns = re.search(
            r"return_compiled=(\d+) return_runs=(\d+) return_bails=(\d+)",
            diag,
        )
        if not returns:
            raise AssertionError(f"{kind}: missing return-stack counters")
        return_compiled, return_runs, return_bails = map(int, returns.groups())
        if kind in ("warp_non_tail_recursive", "warp_non_tail_nested"):
            if not (return_compiled >= 1 and return_runs > 100 and
                    return_bails == 0):
                raise AssertionError(
                    f"{kind}: non-tail return-stack path not executed: "
                    f"{returns.group(0)}"
                )
        if kind in ("warp_non_tail_overflow",
                    "warp_non_tail_unwind_overflow"):
            if not (return_compiled >= 1 and return_runs == 0 and
                    return_bails >= 2):
                raise AssertionError(
                    f"{kind}: non-tail rollback guard failed: "
                    f"{returns.group(0)}"
                )
        if kind in ("warp_non_tail_effect", "warp_non_tail_broken",
                    "warp_recursive_leaf_blocked"):
            if return_compiled != 0 or return_runs != 0:
                raise AssertionError(
                    f"{kind}: unsafe non-tail recursion compiled: "
                    f"{returns.group(0)}"
                )
        if kind in ("warp_tail_recursive", "warp_tail_recursive_nested",
                    "warp_tail_recursive_arguments",
                    "warp_tail_recursive_two_arguments",
                    "warp_tail_recursive_two_arguments_swap",
                    "warp_tail_recursive_four_arguments"):
            if not (tail_compiled >= 1 and tail_runs > 100 and
                    tail_bails == 0):
                raise AssertionError(
                    f"{kind}: tail recursion did not execute: {tail.group(0)}")
        if kind in ("warp_tail_recursive_overflow",
                    "warp_tail_recursive_arguments_overflow"):
            if not (tail_compiled >= 1 and tail_runs == 0 and
                    tail_bails >= 2):
                raise AssertionError(
                    f"{kind}: transactional tail recursion guard failed: "
                    f"{tail.group(0)}")
        if kind in ("warp_non_tail_recursive",
                    "nonwarp_tail_recursive",
                    "warp_tail_recursive_arguments_dynamic",
                    "warp_tail_recursive_arguments_string",
                    "warp_tail_recursive_two_arguments_string",
                    "warp_tail_recursive_five_arguments"):
            if tail_compiled != 0 or tail_runs != 0:
                raise AssertionError(
                    f"{kind}: illegal recursive transformation: "
                    f"{tail.group(0)}")
        tree = re.search(
            r"tree_compiled=(\d+) tree_runs=(\d+) tree_bails=(\d+)",
            diag)
        if not tree:
            raise AssertionError(f"{kind}: missing nested tree counters")
        tree_compiled, tree_runs, tree_bails = map(int, tree.groups())
        if kind in ("warp_nested_repeat", "warp_nested_conditional",
                    "warp_nested_leaf_loop"):
            if tree_compiled < 1 or tree_runs <= 100:
                raise AssertionError(
                    f"{kind}: whole-tree WASM not exercised: {tree.group(0)}")
        if kind in ("warp_nested_if", "warp_leaf_inline"):
            if tree_compiled < 1 or tree_runs <= 100:
                # A single loop containing a conditional is not a
                # multiloop tree, but it must exercise the warp root.
                if not (warp_compiled >= 1 and warp_runs > 100):
                    raise AssertionError(
                        f"{kind}: conditional warp root not executed")
        if kind in ("nonwarp_if", "nonwarp_ifelse",
                    "nonwarp_ifelse_interleaved") and fast <= 100:
            raise AssertionError(
                f"{kind}: numeric conditional bypass did not run: "
                f"{stats.group(0)}")
        if kind in ("warp_batched", "warp_while", "warp_until",
                    "warp_until_empty", "warp_foreach",
                    "warp_foreach_dynamic", "warp_foreach_empty",
                    "warp_foreach_fractional", "warp_foreach_mutated_index",
                    "warp_foreach_string_bound",
                    "warp_nested_repeat", "warp_nested_conditional",
                    "warp_nested_if") and not (
                warp_compiled >= 1 and warp_runs > 100):
            raise AssertionError(
                f"{kind}: expected actual whole-loop WASM calls: "
                f"{warp.group(0)}")
        if kind in ("warp_nonwarp_leaf_blocked",
                    "warp_recursive_leaf_blocked"):
            if warp_compiled != 0 or warp_runs != 0:
                raise AssertionError(
                    f"{kind}: nonwarp/recursive helper illegally inlined: "
                    f"{warp.group(0)}")
        if kind == "warp_nested_list_effect":
            if tree_compiled != 0 or tree_runs != 0:
                raise AssertionError(
                    f"{kind}: effectful nested tree incorrectly compiled: "
                    f"{tree.group(0)}")
        if kind == "warp_nested_overflow":
            if not (tree_compiled >= 1 and tree_runs == 0 and
                    tree_bails >= 2):
                raise AssertionError(
                    f"{kind}: nested aggregate fuel guard failed: "
                    f"{tree.group(0)}")
        if kind in ("warp_overflow", "warp_while_overflow",
                    "warp_foreach_overflow"):
            if not (warp_compiled >= 1 and warp_bails >= 2 and
                    warp_runs == 0):
                raise AssertionError(
                    f"{kind}: expected transactional overflow bailout: "
                    f"{warp.group(0)}")
        elif kind.startswith("nonwarp_"):
            if warp_runs != 0:
                raise AssertionError(
                    f"{kind}: nonwarp loop illegally batched: {warp.group(0)}")
        elif kind == "warp_nested_overflow":
            # The enclosing transaction bails; the unchanged interpreter
            # may still compile individual inner loop statements.
            pass
        elif kind in ("warp_nested_list_effect",
                    "warp_nonwarp_leaf_blocked",
                    "warp_recursive_leaf_blocked",
                    "warp_non_tail_overflow",
                    "warp_non_tail_unwind_overflow",
                    "warp_non_tail_effect",
                    "warp_non_tail_broken",
                    "warp_tail_recursive_overflow",
                    "warp_tail_recursive_arguments_overflow",
                    "warp_non_tail_recursive",
                    "nonwarp_tail_recursive",
                    "warp_tail_recursive_arguments_dynamic",
                    "warp_tail_recursive_arguments_string",
                    "warp_tail_recursive_two_arguments_string",
                    "warp_tail_recursive_five_arguments"):
            pass
        elif not (compiled > 0 and executed > 100):
            raise AssertionError(
                f"{kind}: nested code not executed enough: {stats.group(0)}")
        print(f"PASS {kind}: 5-way parity, {stats.group(0)}")


if __name__ == "__main__":
    main()
