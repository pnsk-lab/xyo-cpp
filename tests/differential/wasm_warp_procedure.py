#!/usr/bin/env python3
"""Guarded warp-procedure inlining parity, captured args and interpreter fallback."""
from __future__ import annotations
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))
from gen_algo_bench import (Gen, S_set, S_change, S_repeat, S_call, S_if,
                            S_add, S_delall, length,
                            num, var, arg, add, sub, gt, eq, join, text,
                            make_project, write_sb3)
from wasm_hotspot_integration import run, run_native


def fixture(path: Path, kind: str) -> None:
    lists = ["items"] if kind == "list_rollback" else []
    g = Gen(["x", "y", "z"], lists)
    if kind == "warp_inline":
        g.define("step %s", ["n"], [
            S_set("x", add(var("x"), arg("n"))),
            S_change("y", arg("n")),
        ], warp=True)
        g.define("tick", [], [S_change("z", num(1))], warp=True)
        g.script([
            S_set("x", num(2)), S_set("y", num(0)),
            S_call("step %s", [var("x")]),
            S_repeat(num(3), [
                S_call("step %s", [var("x")]), S_call("tick")]),
            S_set("z", add(var("z"), add(var("x"), var("y")))),
        ])
    elif kind == "warp_nested_repeat":
        g.define("nested %s", ["n"], [
            S_repeat(num(3), [
                S_repeat(num(2), [S_change("x", arg("n"))]),
            ]),
            S_set("y", var("x")),
        ], warp=True)
        g.script([S_set("x", num(1)), S_call("nested %s", [num(2)]),
                  S_set("z", add(var("x"), var("y")))])
    elif kind == "dynamic_warp_repeat":
        g.define("nested %s", ["n"], [
            S_repeat(arg("n"), [S_change("x", num(1))]),
        ], warp=True)
        g.script([S_set("x", num(0)), S_call("nested %s", [num(4)])])
    elif kind == "list_rollback":
        g.define("fill %s", ["n"], [
            S_delall("items"),
            S_set("x", num(0)),
            S_repeat(arg("n"), [
                S_add("items", var("x")),
                S_change("x", num(1)),
            ]),
            S_set("y", length("items")),
        ], warp=True)
        g.script([S_add("items", num(99)),
                  S_call("fill %s", [num(1050)]),
                  S_set("z", length("items"))])
    elif kind == "oversized_warp_repeat":
        g.define("nested %s", ["n"], [
            S_repeat(arg("n"), [S_change("x", num(1))]),
        ], warp=True)
        g.script([S_set("x", num(0)), S_call("nested %s", [num(1050)]),
                  S_set("y", var("x"))])
    elif kind == "nonwarp_loop_fallback":
        g.define("step %s", ["n"], [
            S_repeat(arg("n"), [S_change("x", num(1))]),
        ], warp=False)
        g.script([S_set("x", num(1)), S_call("step %s", [num(3)])])
    elif kind == "nonwarp_leaf":
        g.define("step %s", ["n"], [
            S_set("x", add(var("x"), arg("n"))),
        ], warp=False)
        g.script([S_set("x", num(1)), S_call("step %s", [num(3)])])
    elif kind in ("recursive_tail", "recursive_deep", "recursive_min"):
        depth = 2000 if kind == "recursive_deep" else 3
        if kind == "recursive_min":
            # The whole body is the conditional self call: no prefix.
            body = [S_if(gt(arg("n"), num(0)), [
                S_call("recurse %s", [sub(arg("n"), num(1))]),
            ])]
        else:
            body = [
                S_change("x", num(1)),
                S_if(gt(arg("n"), num(0)), [
                    S_call("recurse %s", [sub(arg("n"), num(1))]),
                ]),
            ]
        g.define("recurse %s", ["n"], body, warp=True)
        g.script([S_set("x", num(0)), S_call("recurse %s", [num(depth)])])
    elif kind == "recursive_suffix":
        # A suffix after the recursive IF forces the multi-return-PC frame
        # stack (WarpReturnPcStack), which reserves host frames beyond the
        # scalar slots.
        g.define("recurse %s", ["n"], [
            S_change("x", num(1)),
            S_if(gt(arg("n"), num(0)), [
                S_call("recurse %s", [sub(arg("n"), num(1))]),
            ]),
            S_change("y", num(1)),
        ], warp=True)
        g.script([S_set("x", num(0)), S_set("y", num(0)),
                  S_call("recurse %s", [num(3)])])
    elif kind == "recursive_two_sites":
        # Two conditional self calls with distinct return PCs and a middle
        # statement between the sites.
        g.define("recurse %s", ["n"], [
            S_change("x", num(1)),
            S_if(gt(arg("n"), num(1)), [
                S_call("recurse %s", [sub(arg("n"), num(2))]),
            ]),
            S_change("y", num(1)),
            S_if(eq(arg("n"), num(1)), [
                S_call("recurse %s", [sub(arg("n"), num(1))]),
            ]),
            S_change("z", num(1)),
        ], warp=True)
        g.script([S_set("x", num(0)), S_set("y", num(0)), S_set("z", num(0)),
                  S_call("recurse %s", [num(3)])])
    elif kind == "recursive_three_args":
        # Three numeric arguments require a wider per-frame stride; the host
        # reservation follows the actual argument count.
        g.define("recurse %s", ["n", "b", "c"], [
            S_change("x", num(1)),
            S_if(gt(arg("n"), num(0)), [
                S_call("recurse %s", [sub(arg("n"), num(1)),
                                      add(arg("b"), num(1)),
                                      add(arg("c"), num(2))]),
            ]),
            S_change("y", arg("b")),
            S_change("z", arg("c")),
        ], warp=True)
        g.script([S_set("x", num(0)), S_set("y", num(0)), S_set("z", num(0)),
                  S_call("recurse %s", [num(3), num(10), num(20)])])
    elif kind in ("nonwarp_recursive_tail", "nonwarp_recursive_suffix",
                  "nonwarp_recursive_loop"):
        if kind == "nonwarp_recursive_tail":
            body = [
                S_change("x", num(1)),
                S_if(gt(arg("n"), num(0)), [
                    S_call("recurse %s", [sub(arg("n"), num(1))]),
                ]),
            ]
        elif kind == "nonwarp_recursive_suffix":
            body = [
                S_change("x", num(1)),
                S_if(gt(arg("n"), num(0)), [
                    S_call("recurse %s", [sub(arg("n"), num(1))]),
                ]),
                S_change("y", num(1)),
            ]
        else:
            # A loop prefix makes the procedure yield in Scratch, so the
            # whole-script synchronous form must not be used.
            body = [
                S_repeat(num(2), [S_change("y", num(1))]),
                S_if(gt(arg("n"), num(0)), [
                    S_call("recurse %s", [sub(arg("n"), num(1))]),
                ]),
            ]
        g.define("recurse %s", ["n"], body, warp=False)
        g.script([S_set("x", num(0)), S_set("y", num(0)),
                  S_call("recurse %s", [num(3)])])
    elif kind in ("recursive_five_args_tail", "recursive_five_args_pc",
                  "recursive_five_sites"):
        if kind == "recursive_five_args_tail":
            g.define("recurse %s", ["n", "b", "c", "d", "e"], [
                S_change("x", num(1)),
                S_if(gt(arg("n"), num(0)), [
                    S_call("recurse %s", [
                        sub(arg("n"), num(1)), add(arg("b"), num(1)),
                        add(arg("c"), num(1)), add(arg("d"), num(1)),
                        add(arg("e"), num(1))]),
                ]),
            ], warp=True)
            g.script([S_set("x", num(0)),
                      S_call("recurse %s", [num(3), num(0), num(0),
                                            num(0), num(0)])])
        elif kind == "recursive_five_args_pc":
            g.define("recurse %s", ["n", "b", "c", "d", "e"], [
                S_change("x", num(1)),
                S_if(gt(arg("n"), num(0)), [
                    S_call("recurse %s", [
                        sub(arg("n"), num(1)), add(arg("b"), num(1)),
                        add(arg("c"), num(1)), add(arg("d"), num(1)),
                        add(arg("e"), num(1))]),
                ]),
                S_change("y", num(1)),
            ], warp=True)
            g.script([S_set("x", num(0)), S_set("y", num(0)),
                      S_call("recurse %s", [num(3), num(0), num(0),
                                            num(0), num(0)])])
        else:
            # Five distinct conditional self-call return sites.
            g.define("recurse %s", ["n"], [
                S_change("x", num(1)),
                S_if(gt(arg("n"), num(4)), [
                    S_call("recurse %s", [sub(arg("n"), num(5))])]),
                S_change("y", num(1)),
                S_if(gt(arg("n"), num(3)), [
                    S_call("recurse %s", [sub(arg("n"), num(4))])]),
                S_change("y", num(1)),
                S_if(gt(arg("n"), num(2)), [
                    S_call("recurse %s", [sub(arg("n"), num(3))])]),
                S_change("y", num(1)),
                S_if(gt(arg("n"), num(1)), [
                    S_call("recurse %s", [sub(arg("n"), num(2))])]),
                S_change("y", num(1)),
                S_if(gt(arg("n"), num(0)), [
                    S_call("recurse %s", [sub(arg("n"), num(1))])]),
                S_change("y", num(1)),
            ], warp=True)
            g.script([S_set("x", num(0)), S_set("y", num(0)),
                      S_call("recurse %s", [num(5)])])
    elif kind == "recursive_string_arg":
        # The initial argument is numeric (frontend accepts), but the
        # recursive argument is a heap string. The emitter's numeric tag
        # guard must bail before storing it in a frame/local.
        g.define("recurse %s", ["n"], [
            S_change("x", num(1)),
            # "ab" == 1 is false, so the recursion terminates after one
            # non-numeric call; only the tag guard path is exercised.
            S_if(eq(arg("n"), num(1)), [
                S_call("recurse %s", [join(text("a"), text("b"))]),
            ]),
        ], warp=True)
        g.script([S_set("x", num(0)), S_call("recurse %s", [num(1)])])
    else:
        raise ValueError(kind)
    write_sb3(str(path), make_project(["x", "y", "z"], lists, g))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--llvm", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for kind in ("warp_inline", "warp_nested_repeat",
                 "dynamic_warp_repeat", "oversized_warp_repeat",
                 "list_rollback", "nonwarp_leaf", "nonwarp_loop_fallback",
                 "recursive_tail", "recursive_deep", "recursive_min",
                 "recursive_suffix", "recursive_two_sites",
                 "recursive_three_args", "recursive_five_args_tail",
                 "recursive_five_args_pc", "recursive_five_sites",
                 "nonwarp_recursive_tail",
                 "nonwarp_recursive_suffix",
                 "nonwarp_recursive_loop", "recursive_string_arg"):
        sb3 = args.out / (kind + ".sb3")
        fixture(sb3, kind)
        native = run_native(args.llvm, sb3, "hotspot")
        interpreter, _ = run("node", ROOT / "web/xyo-node.mjs",
                             args.wasm, sb3, "off")
        builtin, _ = run("node", ROOT / "web/xyo-node.mjs",
                         args.wasm, sb3, "hotspot",
                         initial={"XYO_WASM_HOT_EMITTER": "builtin"})
        binaryen, log = run("node", ROOT / "web/xyo-node.mjs",
                            args.wasm, sb3, "hotspot",
                            initial={"XYO_WASM_HOT_EMITTER": "binaryen",
                                     "XYO_BINARYEN_NODE_ADDON":
                                         str(args.addon.resolve())})
        if len({native, interpreter, builtin, binaryen}) != 1:
            raise AssertionError(f"{kind}: LLVM={native}\ninterpreter={interpreter}"
                                 f"\nbuiltin={builtin}\nbinaryen={binaryen}")
        # "selecting emitter=" is printed before lowering is attempted, so it
        # is NOT proof of compilation. Only a whole-script/hybrid compile
        # marker means the Binaryen path actually ran.
        compiled = ("xyo-wasm-hot: compiled script=" in log or
                    "xyo-wasm-hot: hybrid script=" in log)
        # LLVM compiles every one of these shapes. The WASM backend must EITHER
        # compile them or fall back safely to the interpreter with identical
        # results; a silent partial execution is the only failure mode.
        # A failed whole-script plan must not execute. The backend may
        # explicitly retry with the hybrid planner, which is a different,
        # independently validated plan; that is the intended fallback.
        whole_failed = "xyo-wasm-hot: compile failed" in log
        hybrid_retry = ("retrying hybrid regions" in log)
        if whole_failed and not hybrid_retry and (
                "xyo-wasm-hot: committed" in log or
                "xyo-wasm-hot: run handle=" in log):
            raise AssertionError(
                f"{kind}: failed lowering still executed a WASM region")
        if kind == "nonwarp_recursive_loop" and "compiled script=" in log:
            raise AssertionError(
                f"{kind}: yielding procedure must not whole-compile: "
                f"{log[-1800:]}")
        if not compiled:
            raise AssertionError(
                f"{kind}: numeric top-level prefix should compile: {log[-1800:]}")
        # The dependent-recursion procedure itself stays interpreter-owned
        # (LLVM compiles it), but since 2026-11-16 the hybrid planner still
        # compiles the numeric regions before/after the call. A future
        # recursion implementation may legitimately compile the whole script.
        if kind == "nonwarp_loop_fallback" and (
                "xyo-wasm-hot: hybrid script=" not in log):
            raise AssertionError(
                f"{kind}: procedure must execute in interpreted hybrid region")
        if kind in ("oversized_warp_repeat", "list_rollback",
                    "recursive_deep", "recursive_string_arg") and (
                        "wasm return=1" not in log):
            raise AssertionError("fuel exhaustion did not deopt")
        print(f"PASS {kind}: LLVM/interpreter/builtin/Binaryen parity and "
              + ("actual Binaryen compilation" if compiled
                 else "safe interpreter fallback"))
        if compiled:
            print(binaryen if kind != "list_rollback" else
                  f"list_rollback: 1050 elements; x=y=z=1050")
    return 0


if __name__ == "__main__":
    sys.exit(main())
