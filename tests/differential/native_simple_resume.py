#!/usr/bin/env python3
"""Opt-in real native LLVM Fiber -> Scratch interpreter deopt parity.

A nonwarp procedure chain (including bounded recursion and verified
nested IF/IF-ELSE arms), with repeat/while/until loops, may transfer at a
completed native body checkpoint. Tests check exact interpreter variables,
GC-rooted arguments, source fingerprints and fail-closed unsupported paths.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_ifelse, S_repeat, S_set, S_until, add, arg,
    div, floor_, gt, join, lt, make_project, num, sub, text, var, write_sb3,
)
from native_callframe_mirror import fixture as mirror_fixture

SUPPORTED = (
    "heap_string", "numeric", "empty_string", "argument_snapshot",
    "no_args", "terminal_call", "nested_repeat", "triple_repeat",
    "sibling_repeat", "prefixed_body", "nested_prefix",
    "while", "until", "while_nested", "until_nested",
    "repeat_one", "count_arg", "count_dynamic",
    "nested_call", "nested_call_string", "nested_call_terminal",
    "nested_call_terminal_script", "triple_call",
    "triple_call_terminal", "parent_repeat_terminal_chain", "parent_repeat", "parent_repeat_terminal",
    "parent_repeat_string", "parent_while", "parent_until",
    "bounded_recursive", "bounded_recursive_string",
    "unicode_gc_string", "bounded_recursive_unicode_string",
    "bounded_recursive_multi", "bounded_recursive_deep_string",
    "bounded_recursive_twelve_string", "bounded_recursive_twenty_string",
    "bounded_recursive_thirtytwo_string", "bounded_recursive_if", "bounded_recursive_if_string",
    "bounded_recursive_ifelse", "bounded_recursive_ifelse_arm2",
    "bounded_recursive_if_suffix", "parent_branch_call",
    "completed_branch_prefix", "nested_if", "nested_ifelse",
    "nested_three_if", "nested_four_if", "nested_five_if",
    "nested_eight_if", "nested_sixteen_if", "nested_thirtytwo_if",
    "nested_changed_condition", "nested_mixed_else_arms",
    "parent_nested_branch_call", "parent_if_repeat",
    "parent_if_repeat_if", "parent_repeat_if_repeat", "parent_if_until",
    "parent_if_terminal", "parent_nested_if_terminal", "parent_ifelse_terminal",
    "parent_if_terminal_changed", "parent_repeat_if_terminal",
    "parent_repeat_ifelse_terminal",
    "parent_ifelse_repeat_string", "parent_ifelse_until",
    "leaf_if_repeat", "leaf_repeat_if_repeat", "leaf_ifelse_repeat",
    "leaf_repeat_ifelse_repeat",
    "bounded_recursive_nested_if", "bounded_recursive_nested_if_string",
    "root_if_call", "root_until_call", "root_while_call",
    "root_if_until_call", "root_if_terminal_chain",
    "root_ifelse_terminal_chain", "root_until_terminal_chain",
    "root_repeat_one_call", "root_repeat_one_terminal_chain",
    "root_nested_repeat_one_call", "root_if_repeat_one_call",
    "root_repeat_three_call", "root_repeat_dynamic_one_call",
    "root_repeat_expr_one_call", "root_repeat_nineteen_call",
    "root_nested_repeat_nineteen_seven",
    "root_if_repeat_three_call", "root_repeat_dynamic_three_call",
    "root_terminal_if", "root_terminal_ifelse_else",
    "root_terminal_nested_if", "root_terminal_nested_ifelse_then",
    "root_terminal_nested_ifelse_else", "root_terminal_eight_if",
    "root_loop_terminal_if", "root_loop_terminal_ifelse_else",
    "root_loop_terminal_nested_ifelse", "root_loop_terminal_double_repeat",
    "procedure_loop_terminal_if", "procedure_loop_terminal_ifelse_else",
    "procedure_loop_terminal_nested_if", "procedure_loop_terminal_double_repeat",
    "procedure_loop_terminal_chain",
    "recursive",
)
UNSUPPORTED = (
    "repeat_zero", "nested_thirtythree_if",
)
# Multi-hat projects exercise independent native CallFrame lifetimes while
# both green-flag scripts mutate shared Scratch target variables.
INTERLEAVED = (
    "interleaved_shared_variables",
    "interleaved_nested_unicode",
    "interleaved_terminal_unicode",
    "interleaved_two_native_callframes",
    "interleaved_two_root_counted_callframes",
)


def fixture(directory: Path, case: str) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    if case == "varslot_guard_editor_mutation":
        # A real native Fiber yields between repeat iterations.
        # The host editor erases and re-creates v_x while the Fiber
        # is suspended, without editing this script's source graph.
        # y is an independent iteration counter for the exact
        # remaining-work oracle.
        gen = Gen(["x", "y"], [])
        gen.script([
            S_set("x", num(0)),
            S_set("y", num(0)),
            S_repeat(num(2400), [
                S_change("x", num(1)),
                S_change("y", num(1)),
            ]),
        ])
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    if case.startswith("root_nocall_"):
        # No custom procedure exists anywhere in the source project.
        # The experimental root-only JIT producer must initialize the
        # verified source CallFrame before its first loop checkpoint.
        gen = Gen(["x", "y", "z"], [])
        loopBody = [S_change("x", num(1)), S_change("y", num(2))]
        if case == "root_nocall_dynamic":
            loopBody.append(S_change("z", num(-1)))
        inner = S_repeat(
            var("z") if case == "root_nocall_dynamic" else num(2200),
            loopBody,
        )
        if case == "root_nocall_nested":
            controls = [S_repeat(num(3), [
                inner, S_change("x", num(3)),
            ])]
        elif case == "root_nocall_terminal_if":
            controls = [S_if(lt(var("y"), num(5000)), [inner])]
        elif case in (
            "root_nocall_simple", "root_nocall_dynamic",
            "root_nocall_two_hats",
        ):
            controls = [inner]
        else:
            raise AssertionError(f"unknown fixture {case}")
        gen.script([
            S_set("x", num(0)), S_set("y", num(0)),
            S_set("z", num(2200)), *controls,
            *([] if case == "root_nocall_terminal_if"
              else [S_change("x", num(7))]),
        ])
        if case == "root_nocall_two_hats":
            gen.script([S_repeat(num(1700), [
                S_change("x", num(1)), S_change("y", num(3)),
            ])])
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    if case.startswith("root_afterloop_"):
        # A completed nonwarp procedure precedes a long ROOT loop.
        # The loop's native AfterLoopBody checkpoint has depth 1.
        # Restoring no interpreter frames used to silently drop the
        # entire remaining root loop and the following statement.
        count = (20000 if case == "root_afterloop_20000"
                 else 2200 if case == "root_afterloop_dynamic"
                 else 2000)
        gen = Gen(["x", "y", "z"], [])
        gen.define("seed %s", ["unused"], [
            S_repeat(num(2), [S_change("x", num(1))]),
        ], warp=False)
        body = [S_change("x", num(1)), S_change("y", num(2))]
        dynamic_mutates = case in (
            "root_afterloop_dynamic_mutated",
            "root_afterloop_nested_dynamic_mutated",
            "root_afterloop_two_hats_dynamic",
        )
        if dynamic_mutates:
            # The original TIMES result is snapshotted once. Re-reading
            # z on transfer would drop the remaining committed iterations.
            body.append(S_change("z", num(-1)))
        loop = S_repeat(
            var("z") if (
                case == "root_afterloop_dynamic" or dynamic_mutates
            ) else num(count),
            body,
        )
        suffix = S_change("x", num(7))
        if case == "root_afterloop_nested_if":
            controls = [S_if(lt(var("y"), num(1)), [loop])]
        elif case == "root_afterloop_nested_repeat":
            controls = [S_repeat(num(4), [
                S_repeat(num(1700), body), S_change("x", num(3)),
            ])]
        elif case == "root_afterloop_repeat_if":
            controls = [S_repeat(num(4), [
                S_if(lt(var("y"), num(100000)), [
                    S_repeat(num(1700), body),
                ]),
                S_change("x", num(3)),
            ])]
        elif case == "root_afterloop_if_over":
            controls = [S_if(lt(var("y"), num(1)), [
                S_repeat(num(5000), body), S_change("x", num(3)),
            ])]
        elif case == "root_afterloop_sibling":
            controls = [
                S_repeat(num(2), [S_change("x", num(11))]),
                S_repeat(num(2400), [S_change("x", num(1))]),
            ]
        elif case == "root_afterloop_until":
            controls = [S_until(gt(var("x"), num(5000)), body)]
        elif case == "root_afterloop_ifelse":
            controls = [S_ifelse(gt(var("y"), num(1)), [
                S_change("x", num(30000)),
            ], [
                S_repeat(num(5000), body),
            ])]
        elif case == "root_afterloop_triple":
            controls = [S_repeat(num(2), [
                S_repeat(num(3), [S_repeat(num(1200), body)]),
                S_change("y", num(7)),
            ])]
        elif case == "root_afterloop_nested_dynamic_mutated":
            controls = [S_repeat(num(3), [
                loop, S_change("x", num(7)),
            ])]
        elif case == "root_afterloop_terminal_if":
            controls = [S_if(lt(var("y"), num(5000)), [
                S_repeat(num(2000), body),
            ])]
        elif case == "root_afterloop_terminal_ifelse":
            controls = [S_ifelse(gt(var("y"), num(5000)), [
                S_change("x", num(999)),
            ], [S_repeat(num(2000), body)])]
        elif case == "root_afterloop_terminal_nested_if":
            inner = S_repeat(num(2000), body)
            for _ in range(3):
                inner = S_if(lt(var("y"), num(5000)), [inner])
            controls = [inner]
        elif case == "root_afterloop_terminal_repeat_if":
            controls = [S_repeat(num(3), [
                S_if(lt(var("y"), num(5000)), [
                    S_repeat(num(2000), body),
                ]),
            ])]
        elif case == "root_afterloop_terminal_repeat_ifelse":
            controls = [S_repeat(num(3), [
                S_ifelse(gt(var("y"), num(5000)), [
                    S_change("x", num(3)),
                ], [S_repeat(num(2000), body)]),
            ])]
        elif case in (
            "root_afterloop_2000", "root_afterloop_20000",
            "root_afterloop_dynamic", "root_afterloop_dynamic_mutated",
            "root_afterloop_two_hats",
            "root_afterloop_two_hats_dynamic",
        ):
            controls = [S_repeat(num(2000), [
                S_change("x", num(1)),
                S_change("y", num(2)),
            ])] if case == "root_afterloop_two_hats" else [loop]
        else:
            raise AssertionError(f"unknown fixture {case}")
        gen.script([
            S_set("x", num(0)), S_set("y", num(0)),
            S_set("z", num(count)),
            S_call("seed %s", [text("GC-root-before-loop𠮷" * 2)]),
            *controls,
            *([] if case.startswith("root_afterloop_terminal_") else [suffix]),
        ])
        if case.startswith("root_afterloop_two_hats"):
            # Shared-variable concurrent hat: a different scheduler
            # thread must continue to contribute effects across deopt.
            gen.script([S_repeat(num(1700), [
                S_change("x", num(1)), S_change("y", num(3)),
            ])])
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    if case.startswith("procedure_loop_terminal_"):
        gen = Gen(["x", "y", "z"], [])
        gen.define("inner %s", ["v"], [
            S_repeat(num(2), [
                S_change("x", num(1)),
                S_change("y", num(1)),
                S_set("z", arg("v")),
            ]),
        ], warp=False)
        call = S_call("inner %s", [arg("v")])
        if case == "procedure_loop_terminal_chain":
            gen.define("middle %s", ["v"], [
                S_change("x", num(5)), call,
            ], warp=False)
            call = S_call("middle %s", [arg("v")])
        if case in ("procedure_loop_terminal_if",
                    "procedure_loop_terminal_chain"):
            body = [S_if(lt(var("y"), num(1)), [
                S_change("x", num(3)), call,
            ])]
        elif case == "procedure_loop_terminal_ifelse_else":
            body = [S_ifelse(gt(var("y"), num(1)), [
                S_change("x", num(99)),
            ], [
                S_change("x", num(4)), call,
            ])]
        elif case == "procedure_loop_terminal_nested_if":
            body = [S_if(lt(var("y"), num(1)), [
                S_if(lt(var("y"), num(1)), [
                    S_change("x", num(6)), call,
                ]),
            ])]
        elif case == "procedure_loop_terminal_double_repeat":
            body = [S_repeat(num(2), [
                S_if(lt(var("y"), num(1)), [
                    S_change("x", num(7)), call,
                ]),
            ])]
        else:
            raise AssertionError(f"unknown fixture {case}")
        gen.define("outer %s", ["v"], [
            S_repeat(num(3), body), S_change("x", num(11)),
        ], warp=False)
        gen.script([
            S_set("x", num(0)), S_set("y", num(0)),
            S_call("outer %s", [
                text("non-root continuation𠮷🧪" * 2),
            ]),
            S_change("x", num(13)),
        ])
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    if case.startswith("root_loop_terminal_"):
        gen = Gen(["x", "y", "z"], [])
        gen.define("worker %s", ["a"], [
            S_repeat(num(2), [
                S_change("x", num(1)),
                S_change("y", num(1)),
                S_set("z", arg("a")),
            ]),
        ], warp=False)
        invoke = S_call("worker %s", [
            text("terminal loop branch GC argument𠮷" * 2),
        ])
        if case == "root_loop_terminal_if":
            body = [S_if(lt(var("y"), num(1)), [
                S_change("x", num(5)), invoke,
            ])]
        elif case == "root_loop_terminal_ifelse_else":
            body = [S_ifelse(gt(var("y"), num(1)), [
                S_change("x", num(9)),
            ], [
                S_change("x", num(4)), invoke,
            ])]
        elif case == "root_loop_terminal_nested_ifelse":
            body = [S_if(lt(var("y"), num(1)), [
                S_ifelse(gt(var("y"), num(1)), [
                    S_change("x", num(90)),
                ], [
                    S_change("x", num(6)), invoke,
                ]),
            ])]
        elif case == "root_loop_terminal_double_repeat":
            body = [S_repeat(num(2), [
                S_if(lt(var("y"), num(1)), [
                    S_change("x", num(7)), invoke,
                ]),
            ])]
        else:
            raise AssertionError(f"unknown fixture {case}")
        # Each call ends an IF arm at a repeat latch. After returning,
        # a fresh IF evaluation would see the callee-mutated variable.
        gen.script([
            S_set("x", num(0)), S_set("y", num(0)),
            S_repeat(num(3), body), S_change("x", num(11)),
        ])
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    if case.startswith("root_terminal_"):
        gen = Gen(["x", "y", "z"], [])
        gen.define("worker %s", ["a"], [
            S_repeat(num(2), [
                S_change("x", num(1)),
                S_change("y", num(1)),
                S_set("z", arg("a")),
            ]),
        ], warp=False)
        invoke = S_call("worker %s", [
            text("root script終端のUnicode・GCを検証𠮷🧪" * 2),
        ])
        if case == "root_terminal_if":
            control = S_if(lt(var("y"), num(1)), [
                S_change("x", num(5)), invoke,
            ])
        elif case == "root_terminal_ifelse_else":
            control = S_ifelse(gt(var("y"), num(1)), [
                S_change("x", num(900)),
            ], [
                S_change("x", num(7)), invoke,
            ])
        elif case == "root_terminal_nested_if":
            control = S_if(lt(var("y"), num(1)), [
                S_change("x", num(2)),
                S_if(lt(var("y"), num(1)), [
                    S_change("x", num(3)), invoke,
                ]),
            ])
        elif case == "root_terminal_nested_ifelse_then":
            control = S_if(lt(var("y"), num(1)), [
                S_ifelse(lt(var("y"), num(1)), [
                    S_change("x", num(4)), invoke,
                ], [
                    S_change("x", num(901)),
                ]),
            ])
        elif case == "root_terminal_nested_ifelse_else":
            control = S_if(lt(var("y"), num(1)), [
                S_ifelse(gt(var("y"), num(1)), [
                    S_change("x", num(902)),
                ], [
                    S_change("x", num(8)), invoke,
                ]),
            ])
        elif case == "root_terminal_eight_if":
            control = invoke
            for n in range(8):
                control = S_if(lt(var("y"), num(1)), [
                    S_change("x", num(n + 1)), control,
                ])
        else:
            raise AssertionError(f"unknown fixture {case}")
        # No block follows this conditional, so an authentic source
        # return must be ScriptExit, not AfterBranch/NextBlock.
        gen.script([S_set("x", num(0)), S_set("y", num(0)), control])
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    if case in INTERLEAVED:
        gen = Gen(["x", "y", "z"], [])
        if case in ("interleaved_two_native_callframes",
                    "interleaved_two_root_counted_callframes"):
            gen.define("worker %s", ["a"], [
                S_repeat(num(3), [
                    S_change("x", arg("a")),
                    S_change("y", num(1)),
                ]),
            ], warp=False)
            if case == "interleaved_two_root_counted_callframes":
                # Each root hat has its OWN lexical repeat counter. The
                # same procedure definition is shared between both hats.
                gen.script([
                    S_repeat(num(5), [
                        S_call("worker %s", [num(7)]),
                    ]),
                    S_change("x", num(3)),
                ])
                gen.script([
                    S_repeat(num(3), [
                        S_call("worker %s", [num(11)]),
                    ]),
                    S_change("x", num(4)),
                ], x=80)
            else:
                gen.script([
                    S_call("worker %s", [num(7)]),
                    S_change("x", num(3)),
                ])
                gen.script([
                    S_call("worker %s", [num(11)]),
                    S_change("x", num(4)),
                ], x=80)
        else:
            gen.define("inner %s", ["a"], [
                S_repeat(num(3), [
                    S_change("x", num(2)),
                    S_change("y", num(1)),
                    S_set("z", arg("a")),
                ]),
            ], warp=False)
            if case == "interleaved_nested_unicode":
                body = [
                    S_change("x", num(3)),
                    S_call("inner %s", [arg("a")]),
                    S_change("x", num(5)),
                ]
            else:
                body = [S_call("inner %s", [arg("a")])]
                if case == "interleaved_shared_variables":
                    body.append(S_change("x", num(5)))
            gen.define("worker %s", ["a"], body, warp=False)
            gen.script([
                S_call("worker %s", [
                    text("並列 GC Unicode 🧪 𠮷" * 3),
                ]),
                S_change("x", num(11)),
            ])
            gen.script([
                S_repeat(num(7), [S_change("x", num(7))]),
            ], x=80)
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    if case == "recursive":
        return mirror_fixture(directory, "loop_recursive")
    if case in ("branch_return", "nested_branch_arms",
                "branch_arms", "loop_return", "procedure_return"):
        return mirror_fixture(directory, case)
    if case in ("root_if_call", "root_until_call", "root_while_call",
                "root_if_until_call", "root_repeat_three_call",
                "root_repeat_one_call", "root_repeat_one_terminal_chain",
                "root_nested_repeat_one_call", "root_if_repeat_one_call",
                "root_repeat_dynamic_one_call",
                "root_repeat_expr_one_call", "root_repeat_nineteen_call",
                "root_nested_repeat_nineteen_seven",
                "root_if_repeat_three_call", "root_repeat_dynamic_three_call",
                "root_if_terminal_chain", "root_ifelse_terminal_chain",
                "root_until_terminal_chain"):
        gen = Gen(["x", "y", "z"], [])
        body = [
            S_repeat(num(2), [
                S_change("x", num(1)),
                S_set("z", arg("a")),
                S_change("y", num(1)),
            ]),
        ]
        if case.endswith("_terminal_chain"):
            gen.define("inner %s", ["a"], body, warp=False)
            body = [
                S_change("x", num(1)),
                S_call("inner %s", [arg("a")]),
            ]
        gen.define("worker %s", ["a"], body, warp=False)
        invoke = S_call("worker %s", [
            text("RootのIFとUnicode GCを保持𠮷🧪" * 2),
        ])
        if case in ("root_if_call", "root_if_terminal_chain"):
            control = S_if(lt(var("y"), num(1)), [invoke])
        elif case in ("root_repeat_one_call",
                       "root_repeat_one_terminal_chain"):
            control = S_repeat(num(1), [invoke])
        elif case == "root_nested_repeat_one_call":
            control = S_repeat(num(1), [
                S_repeat(num(1), [invoke]),
                S_change("x", num(2)),
            ])
        elif case == "root_if_repeat_one_call":
            control = S_if(lt(var("y"), num(1)), [
                S_repeat(num(1), [invoke]),
                S_change("x", num(2)),
            ])
        elif case in ("root_repeat_dynamic_one_call",
                       "root_repeat_dynamic_three_call"):
            control = S_repeat(var("y"), [invoke])
        elif case == "root_repeat_expr_one_call":
            control = S_repeat(add(num(0), num(1)), [invoke])
        elif case == "root_repeat_nineteen_call":
            control = S_repeat(num(19), [invoke])
        elif case == "root_nested_repeat_nineteen_seven":
            control = S_repeat(num(19), [
                S_repeat(num(7), [invoke]),
                S_change("x", num(2)),
            ])
        elif case == "root_if_repeat_three_call":
            control = S_if(lt(var("y"), num(10)), [
                S_repeat(num(3), [invoke]),
                S_change("x", num(2)),
            ])
        elif case == "root_ifelse_terminal_chain":
            control = S_ifelse(gt(var("y"), num(10)), [
                S_change("x", num(900)),
            ], [invoke])
        elif case == "root_repeat_three_call":
            control = S_repeat(num(3), [invoke])
        elif case == "root_if_until_call":
            control = S_if(lt(var("y"), num(1)), [
                S_until(gt(var("x"), num(4)), [invoke]),
                S_change("y", num(3)),
            ])
        elif case == "root_while_call":
            # Replaced with control_while on the generated source below.
            control = S_until(lt(var("x"), num(5)), [invoke])
        else:
            control = S_until(gt(var("x"), num(4)), [invoke])
        gen.script([
            S_set("x", num(0)),
            S_set("y", num(1) if case ==
                  "root_repeat_dynamic_one_call" else
                  num(3) if case == "root_repeat_dynamic_three_call"
                  else num(0)),
            control,
            S_change("x", num(7)),
        ])
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        if case == "root_while_call":
            with zipfile.ZipFile(path) as z:
                content = {name: z.read(name) for name in z.namelist()}
            project = json.loads(content["project.json"])
            changed = 0
            for target in project["targets"]:
                for block in target["blocks"].values():
                    if block["opcode"] == "control_repeat_until":
                        block["opcode"] = "control_while"
                        changed += 1
            if changed != 1:
                raise AssertionError(f"{case}: expected one while")
            content["project.json"] = json.dumps(project).encode()
            with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as dst:
                for name, data in content.items():
                    dst.writestr(name, data)
        return path
    if case == "parent_repeat_terminal_chain":
        gen = Gen(["x", "y", "z"], [])
        gen.define("inner %s", ["v"], [
            S_repeat(num(2), [
                S_set("z", arg("v")),
                S_change("y", num(1)),
            ]),
        ], warp=False)
        gen.define("middle %s", ["v"], [
            S_change("x", num(1)),
            S_call("inner %s", [arg("v")]),
        ], warp=False)
        gen.define("worker %s", ["v"], [
            S_repeat(num(2), [
                S_change("x", num(3)),
                S_call("middle %s", [arg("v")]),
            ]),
            S_change("x", num(5)),
        ], warp=False)
        gen.script([
            S_call("worker %s", [text("根をGCで保持🧪" * 3)]),
            S_change("x", num(7)),
        ])
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    if case.startswith("bounded_recursive"):
        gen = Gen(["x", "y", "z"], [])
        is_string = "string" in case
        is_multi = "multi" in case
        limit = (32 if "thirtytwo" in case else
                 20 if "twenty" in case else
                 12 if "twelve" in case else
                 5 if "deep" in case else 3)
        code = "recursive %s %s"
        # count=1-floor(n/limit) gives one call at each n<limit
        # and zero at n=limit. The multi variant adds a second
        # call at the initial n=0, requiring the parent to resume its
        # first counted repeat with an unconsumed second iteration.
        count = sub(num(1), floor_(div(arg("n"), num(limit))))
        if is_multi:
            count = add(count, floor_(div(num(1), add(arg("n"), num(1)))))
        recursive_arg = (
            join(arg("a"), arg("n")) if is_string else arg("a")
        )
        leaf_value = (
            join(var("z"), arg("a")) if is_string else arg("a")
        )
        inner_call = S_call(code, [recursive_arg, add(arg("n"), num(1))])
        if "_if" in case:
            branch = (
                S_if(lt(arg("n"), num(limit)), [
                    S_if(lt(arg("n"), num(limit)), [
                        inner_call,
                        S_change("x", num(1)),
                    ]),
                    S_change("x", num(2)),
                ])
                if "nested_if" in case else
                S_ifelse(gt(arg("n"), num(limit - 1)),
                         [S_change("x", num(0))], [inner_call])
                if case == "bounded_recursive_ifelse_arm2"
                else S_ifelse(lt(arg("n"), num(limit)),
                             [inner_call], [S_change("x", num(0))])
                if case == "bounded_recursive_ifelse"
                else S_if(lt(arg("n"), num(limit)),
                          [inner_call, S_change("x", num(1))]
                          if case == "bounded_recursive_if_suffix"
                          else [inner_call])
            )
        else:
            branch = S_repeat(count, [inner_call])
        gen.define(code, ["a", "n"], [
            branch,
            S_repeat(num(2), [
                S_set("z", leaf_value),
                S_change("y", num(1)),
            ]),
            S_change("x", arg("n")),
        ], warp=False)
        value = (
            text("再帰🧪サロゲート対𠮷 UTF-16 GC root検証" * 3)
            if "unicode" in case else
            text("long GC-rooted recursive string value")
            if is_string else add(num(41), num(0))
        )
        gen.script([S_call(code, [value, add(num(0), num(0))])])
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    if case == "ssa_budget_cross_fiber_two_hats":
        # Two REAL suspended LLVM Fibers, both in nonwarp loops.
        # The first memory-budget handoff frees only one Fiber. The
        # other Fiber still holds compiler-live generated Unicode
        # string SSA, so the global collector must defer its sweep.
        # The second handoff retires the last Fiber and may collect.
        gen = Gen(["x", "y", "z"], [])
        gen.define("budgetjoin %s", ["a"], [
            S_repeat(num(24000), [
                S_ifelse(
                    lt(var("y"), num(12000)),
                    [S_set("x", join(arg("a"), text("LEFT!")))],
                    [S_set("x", join(arg("a"), text("RIGHT!")))]),
                S_set("z", var("x")),
                S_change("y", num(1)),
            ]),
        ], warp=False)
        gen.script([
            S_set("y", num(0)),
            S_call("budgetjoin %s", [text("cross-fiber-A-𠮷" * 480)]),
            S_change("y", num(2)),
        ])
        gen.script([
            S_call("budgetjoin %s", [text("cross-fiber-B-🧪" * 480)]),
            S_change("y", num(3)),
        ], x=80)
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    if case in ("ssa_budget_warp_two_hats_observer",
                "ssa_budget_warp_two_hats_writer"):
        # A certified 1-MiB native budget handoff is NOT a Scratch
        # scheduling yield. The observing second green-flag hat should
        # run only after the first hat's warp script completes (within
        # its normal warp deadline), not between native and Interpreter.
        #
        # 17k joins exceed the 1-MiB soft budget but complete well
        # within the ordinary 500-ms warp time on test hardware.
        gen = Gen(["x", "y", "z"], [])
        gen.define("budgetwarp %s", ["a"], [
            S_repeat(num(17000), [
                S_set("z", join(arg("a"), text("!"))),
                S_change("x", num(1)),
            ]),
        ], warp=True)
        gen.script([
            S_set("x", num(0)),
            S_set("y", num(-1)),
            S_call("budgetwarp %s", [text("interleave-GC-root-𠮷" * 480)]),
        ])
        observer = [S_set("y", var("x"))]
        if case == "ssa_budget_warp_two_hats_writer":
            # A competing shared-variable write must not be overtaken
            # by the rest of the handed-off warp procedure.
            observer.append(S_set("x", num(777)))
        gen.script(observer, x=80)
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    if case in (
        "ssa_budget_join_string", "ssa_budget_join_warp",
        "ssa_budget_join_warp_nested",
        "ssa_budget_join_warp_nested_loop",
        "ssa_budget_join_warp_nested_if_loop",
        "ssa_budget_join_warp_recursive",
        "ssa_budget_join_warp_depth_overflow",
    ):
        # Repeated genuine native xyo_join operations accumulate
        # GC-managed string cons cells (the small-object pool batches
        # accounting). The 1-MiB soft budget is eventually crossed
        # inside a GC-deferred native JIT activation.
        # Real nonargument generated-string PHI liveness is retained
        # across the call to the source-certified budget handoff.
        gen = Gen(["x", "y", "z"], [])
        initial = "large-GC-junction-𠮷" * 480
        if case == "ssa_budget_join_warp_depth_overflow":
            # Native may execute a deeper recursion stack, but a
            # transfer exceeding the explicit 32-CallFrame safety cap
            # must fail closed and defer GC until Fiber completion.
            gen.define("budgetdeep %s %s", ["a", "n"], [
                S_if(lt(arg("n"), num(33)), [
                    S_call("budgetdeep %s %s", [
                        arg("a"), add(arg("n"), num(1)),
                    ]),
                ]),
                S_if(gt(arg("n"), num(32)), [
                    S_repeat(num(24000), [
                        S_set("x", join(arg("a"), text("!"))),
                        S_set("z", var("x")),
                        S_change("y", num(1)),
                    ]),
                ]),
            ], warp=True)
            gen.script([
                S_set("y", num(0)),
                S_call("budgetdeep %s %s", [text(initial), num(0)]),
                S_set("z", var("x")),
            ])
            path = directory / f"native-simple-resume-{case}.sb3"
            write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
            return path
        if case == "ssa_budget_join_warp_recursive":
            # Four declared-warp recursive activations, with three
            # proven active IF arms and a leaf native counted loop.
            # Only the innermost loop crosses the 1-MiB budget before
            # the first handoff. Recover all eight inherited warp frames
            # without re-running a recursive call or branch reporter.
            gen.define("budgetrec %s %s", ["a", "n"], [
                S_if(lt(arg("n"), num(3)), [
                    S_call("budgetrec %s %s", [
                        arg("a"), add(arg("n"), num(1)),
                    ]),
                ]),
                S_repeat(num(24000), [
                    S_ifelse(lt(var("y"), num(12000)),
                             [S_set("x", join(arg("a"), text("LEFT!")))],
                             [S_set("x", join(arg("a"), text("RIGHT!")))]),
                    S_set("z", var("x")),
                    S_change("y", num(1)),
                ]),
            ], warp=True)
            gen.script([
                S_set("y", num(0)),
                S_call("budgetrec %s %s", [text(initial), num(0)]),
                S_set("z", var("x")),
            ])
            path = directory / f"native-simple-resume-{case}.sb3"
            write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
            return path
        gen.define("budgetjoin %s", ["a"], [
            S_repeat(num(24000), [
                S_ifelse(
                    lt(var("y"), num(12000)),
                    [S_set("x", join(arg("a"), text("LEFT!")))],
                    [S_set("x", join(arg("a"), text("RIGHT!")))]),
                S_set("z", var("x")),
                S_change("y", num(1)),
            ]),
        ], warp=case == "ssa_budget_join_warp")
        entry = "budgetjoin %s"
        if case.startswith("ssa_budget_join_warp_nested"):
            # The declared-warp parent must propagate its atomic mode
            # through a nonwarp child and every live lexical scope.
            # For the two-loop cases, the original TIMES=2 is evaluated
            # ONCE. The child overwrites z with a string while it runs,
            # so re-evaluating TIMES after native -> Interpreter transfer
            # would silently lose the remaining parent iteration.
            nested_loop = case != "ssa_budget_join_warp_nested"
            call = S_call("budgetjoin %s", [arg("b")])
            if case == "ssa_budget_join_warp_nested_if_loop":
                call_body = [S_if(lt(var("y"), num(100000)), [call])]
            else:
                call_body = [call]
            body = (
                [S_repeat(var("z"), [*call_body, S_change("y", num(2))])]
                if nested_loop else [call, S_change("y", num(2))]
            )
            gen.define("budgetouter %s", ["b"], body, warp=True)
            entry = "budgetouter %s"
        gen.script([
            S_set("y", num(0)),
            *([S_set("z", num(2))]
              if case in ("ssa_budget_join_warp_nested_loop",
                          "ssa_budget_join_warp_nested_if_loop") else []),
            S_call(entry, [text(initial)]),
            S_set("z", var("x")),
        ])
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    if case == "ssa_new_join_branch_string":
        # An actual LLVM i64 PHI whose two inputs are results of
        # two independently allocated host string-join helpers.
        # Unlike ssa_alias_branch_string there is NO relation to the
        # original argument's boxed bits and therefore no source
        # argument owner proof for the resulting PHI.
        gen = Gen(["x", "y", "z"], [])
        initial = "GC-source-junction \U00020bb7\U0001f9ea " * 4
        gen.define("joinbranch %s", ["a"], [
            S_repeat(num(2400), [
                S_ifelse(
                    lt(var("y"), num(1200)),
                    [S_set("x", join(arg("a"),
                        text("LEFT!\U00020bb7")))],
                    [S_set("x", join(arg("a"),
                        text("RIGHT!\U0001f9ea")))]),
                S_set("z", var("x")),
                S_change("y", num(1)),
            ]),
        ], warp=False)
        gen.script([
            S_set("y", num(0)),
            S_call("joinbranch %s", [text(initial)]),
            S_set("z", var("x")),
        ])
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    if case == "ssa_alias_branch_string":
        # Source-derived boxed string Value is copied through BOTH
        # branches without a conversion. The active LLVM loop should
        # carry a live i64 PHI from argument("a") at its checkpoint.
        # The branch condition changes while the source argument is
        # stable and rooted; the following variable read consumes the
        # phi and prevents an unobserved dead branch value.
        gen = Gen(["x", "y", "z"], [])
        original = "GC-rooted PHI branch 🧪𠮷 " * 5
        gen.define("branch %s", ["a"], [
            S_repeat(num(2400), [
                S_ifelse(lt(var("y"), num(1200)),
                         [S_set("x", arg("a"))],
                         [S_set("x", arg("a"))]),
                S_set("z", var("x")),
                S_change("y", num(1)),
            ]),
        ], warp=False)
        gen.script([
            S_set("y", num(0)),
            S_call("branch %s", [text(original)]),
            S_set("z", var("x")),
        ])
        path = directory / f"native-simple-resume-{case}.sb3"
        write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
        return path
    gen = Gen(["x", "y", "z"], [])
    value = (
        text("GC string kept across native Fiber release")
        if case in ("heap_string", "nested_call_string",
                    "parent_repeat_string", "parent_ifelse_repeat_string") else
        text("辞書𠮷🥼絵文字💻・日本語の文字列をGC後も保持" * 3)
        if case == "unicode_gc_string" else
        text("") if case == "empty_string" else
        var("x") if case == "argument_snapshot" else
        add(num(9), num(0))
    )
    noargs = case == "no_args"
    signature = "worker" if noargs else "worker %s"
    body = [
        S_set("z", text("no arguments") if noargs else arg("a")),
        S_change("y", num(1)),
    ]
    if case == "argument_snapshot":
        # x changes every iteration, while the argument keeps the value
        # captured at the callsite before the first native checkpoint.
        body.insert(0, S_change("x", num(1)))
    conditional = case.startswith(("while", "until"))
    body_loop = (
        S_until(
            lt(var("y"), num(4)) if case.startswith("while")
            else gt(var("y"), num(3)),
            body,
        ) if conditional else S_repeat(
            num(0) if case == "repeat_zero" else
            num(1) if case == "repeat_one" else
            var("y") if case == "count_dynamic" else
            arg("a") if case == "count_arg" else num(3),
            body,
        )
    )
    if case in ("while_nested", "until_nested"):
        body_loop = S_repeat(num(2), [
            S_set("y", num(0)),
            body_loop,
            S_change("x", num(2)),
        ])
    if case in ("nested_repeat", "triple_repeat"):
        body_loop = S_repeat(num(2), [
            body_loop,
            S_change("x", num(2)),
        ])
    if case == "triple_repeat":
        body_loop = S_repeat(num(2), [
            body_loop,
            S_change("y", num(2)),
        ])
    if case == "nested_prefix":
        body_loop = S_repeat(num(2), [
            S_change("x", num(3)),
            body_loop,
            S_change("y", num(2)),
        ])
    definition = [body_loop, S_change("x", num(2))]
    if case == "sibling_repeat":
        definition.insert(0, S_repeat(num(2), [
            S_change("x", num(4)),
        ]))
    if case == "prefixed_body":
        definition.insert(0, S_set("x", num(4)))
    if case == "completed_branch_prefix":
        definition.insert(0, S_if(lt(var("y"), num(10)), [
            S_change("x", num(4)),
        ]))
    if case in ("nested_call", "nested_call_string",
                "nested_call_terminal", "nested_call_terminal_script",
                "triple_call", "triple_call_terminal"):
        # Caller is suspended by native LLVM as AwaitingCallee while
        # the leaf has a committed AfterLoopBody checkpoint. Both the
        # outer and inner argument maps must survive Fiber release.
        gen.define("inner %s", ["b"], [
            S_repeat(num(3), [
                S_set("z", arg("b")),
                S_change("y", num(1)),
            ]),
            S_change("x", num(2)),
        ], warp=False)
        callee = "inner %s"
        if case in ("triple_call", "triple_call_terminal"):
            middle_body = [
                S_change("x", num(1)),
                S_call("inner %s", [arg("c")]),
            ]
            if case == "triple_call":
                middle_body.append(S_change("y", num(2)))
            gen.define("middle %s", ["c"], middle_body, warp=False)
            callee = "middle %s"
        definition = [
            S_change("x", num(1)),
            S_call(callee, [arg("a")]),
        ]
        if case not in ("nested_call_terminal",
                        "nested_call_terminal_script",
                        "triple_call_terminal"):
            definition.append(S_change("x", num(2)))
    if case.startswith("parent_") or case in (
            "nested_if", "nested_ifelse", "nested_three_if",
            "nested_four_if", "nested_five_if", "nested_eight_if",
            "nested_sixteen_if", "nested_thirtytwo_if",
            "nested_thirtythree_if", "nested_changed_condition",
            "nested_mixed_else_arms"):
        gen.define("inner %s", ["b"], [
            S_repeat(num(3), [
                S_set("z", arg("b")),
                S_change("y", num(1)),
            ]),
            S_change("x", num(2)),
        ], warp=False)
        child_body = [S_call("inner %s", [arg("a")])]
        if case != "parent_repeat_terminal":
            child_body.append(S_change("x", num(2)))
        if case in ("parent_if_terminal", "parent_if_terminal_changed"):
            # The child changes y. With threshold=1, repeating the IF
            # reporter after the return would take a different branch.
            outer_loop = S_if(lt(var("y"), num(
                1 if case == "parent_if_terminal_changed" else 10)), [
                S_call("inner %s", [arg("a")]),
            ])
        elif case == "parent_repeat_if_terminal":
            outer_loop = S_repeat(num(2), [
                S_if(lt(var("y"), num(10)), [
                    S_call("inner %s", [arg("a")]),
                ]),
                S_change("x", num(3)),
            ])
        elif case == "parent_repeat_ifelse_terminal":
            outer_loop = S_repeat(num(2), [
                S_ifelse(gt(var("y"), num(10)), [
                    S_change("x", num(900)),
                ], [
                    S_call("inner %s", [arg("a")]),
                ]),
                S_change("x", num(3)),
            ])
        elif case == "parent_nested_if_terminal":
            outer_loop = S_if(lt(var("y"), num(10)), [
                S_if(lt(var("y"), num(10)), [
                    S_call("inner %s", [arg("a")]),
                ]),
            ])
        elif case == "parent_ifelse_terminal":
            outer_loop = S_ifelse(gt(var("y"), num(10)), [
                S_change("x", num(900)),
            ], [
                S_call("inner %s", [arg("a")]),
            ])
        elif case == "parent_while":
            outer_loop = S_until(lt(var("y"), num(6)), child_body)
        elif case == "parent_until":
            outer_loop = S_until(gt(var("y"), num(5)), child_body)
        elif case == "parent_ifelse_repeat_string":
            outer_loop = S_ifelse(gt(var("y"), num(50)), [
                S_change("x", num(99)),
            ], [
                S_repeat(num(2), [*child_body, S_change("x", num(1))]),
                S_change("x", num(3)),
            ])
        elif case == "parent_ifelse_until":
            outer_loop = S_ifelse(gt(var("y"), num(50)), [
                S_change("x", num(99)),
            ], [
                S_until(gt(var("y"), num(5)), child_body),
                S_change("x", num(3)),
            ])
        elif case == "parent_if_repeat":
            outer_loop = S_if(lt(var("y"), num(12)), [
                S_repeat(num(2), [*child_body, S_change("x", num(1))]),
                S_change("x", num(3)),
            ])
        elif case == "parent_if_repeat_if":
            outer_loop = S_if(lt(var("y"), num(12)), [
                S_repeat(num(2), [
                    S_if(lt(var("y"), num(12)), child_body),
                    S_change("x", num(3)),
                ]),
                S_change("x", num(4)),
            ])
        elif case == "parent_repeat_if_repeat":
            outer_loop = S_repeat(num(2), [
                S_if(lt(var("y"), num(12)), [
                    S_repeat(num(2), child_body),
                    S_change("x", num(2)),
                ]),
                S_change("x", num(3)),
            ])
        elif case == "parent_if_until":
            outer_loop = S_if(lt(var("y"), num(12)), [
                S_until(gt(var("y"), num(5)), child_body),
                S_change("x", num(3)),
            ])
        elif case in ("parent_branch_call", "parent_nested_branch_call"):
            current = child_body
            if case == "parent_nested_branch_call":
                current = [
                    S_if(lt(var("y"), num(6)), current),
                    S_change("x", num(2)),
                ]
            outer_loop = S_repeat(num(2), [
                S_if(lt(var("y"), num(6)), current),
                S_change("x", num(2)),
            ])
        elif case in ("nested_if", "nested_ifelse", "nested_three_if",
                      "nested_four_if", "nested_five_if", "nested_eight_if",
                      "nested_sixteen_if", "nested_thirtytwo_if",
                      "nested_thirtythree_if", "nested_changed_condition",
                      "nested_mixed_else_arms"):
            if case == "nested_mixed_else_arms":
                outer_loop = S_ifelse(lt(var("y"), num(1)), [
                    S_ifelse(gt(var("y"), num(1)), [
                        S_change("x", num(99)),
                    ], child_body),
                    S_change("x", num(5)),
                ], [
                    S_change("x", num(999)),
                ])
            elif case == "nested_ifelse":
                outer_loop = S_ifelse(gt(var("y"), num(20)), [
                    S_change("x", num(1)),
                ], [
                    S_ifelse(gt(var("y"), num(10)), [
                        S_change("x", num(1)),
                    ], child_body),
                    S_change("x", num(2)),
                ])
            else:
                conditional_depth = {
                    "nested_three_if": 3,
                    "nested_four_if": 4,
                    "nested_five_if": 5,
                    "nested_eight_if": 8,
                    "nested_sixteen_if": 16,
                    "nested_thirtytwo_if": 32,
                    "nested_thirtythree_if": 33,
                }
                count = conditional_depth.get(case, 2)
                limit = 1 if case == "nested_changed_condition" else 20
                inner = child_body
                for level in range(count):
                    inner = [
                        S_if(lt(var("y"), num(limit)), inner),
                        S_change("x", num(level + 1)),
                    ]
                outer_loop = inner[0]
        else:
            outer_loop = S_repeat(num(2), child_body)
        definition = [outer_loop, S_change("x", num(3))]
    if case == "leaf_ifelse_repeat":
        definition = [
            S_ifelse(gt(var("y"), num(50)), [
                S_change("x", num(99)),
            ], [
                S_repeat(num(3), body),
                S_change("x", num(2)),
            ]),
            S_change("x", num(4)),
        ]
    elif case == "leaf_repeat_ifelse_repeat":
        definition = [
            S_repeat(num(2), [
                S_ifelse(gt(var("y"), num(50)), [
                    S_change("x", num(99)),
                ], [
                    S_repeat(num(2), body),
                    S_change("x", num(3)),
                ]),
                S_change("x", num(4)),
            ]),
            S_change("x", num(5)),
        ]
    elif case == "leaf_if_repeat":
        definition = [
            S_if(lt(var("y"), num(12)), [
                S_repeat(num(3), body),
                S_change("x", num(3)),
            ]),
            S_change("x", num(4)),
        ]
    elif case == "leaf_repeat_if_repeat":
        definition = [
            S_repeat(num(2), [
                S_if(lt(var("y"), num(12)), [
                    S_repeat(num(2), body),
                    S_change("x", num(3)),
                ]),
                S_change("x", num(4)),
            ]),
            S_change("x", num(5)),
        ]
    gen.define(signature, [] if noargs else ["a"], definition, warp=False)
    statements = [S_call(signature, [] if noargs else [value])]
    if case == "prefixed_body":
        statements.insert(0, S_change("x", num(5)))
    if case == "count_dynamic":
        statements.insert(0, S_set("y", num(3)))
    if case not in ("terminal_call", "nested_call_terminal_script"):
        statements.append(S_change("x", num(5)))
    gen.script(statements)
    path = directory / f"native-simple-resume-{case}.sb3"
    write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
    if case.startswith("while") or case == "parent_while":
        # The generator models the reporter with S_until, and a single
        # opcode change gives its Scratch while counterpart.
        with zipfile.ZipFile(path) as src:
            content = {name: src.read(name) for name in src.namelist()}
        project = json.loads(content["project.json"])
        replaced = 0
        for target in project["targets"]:
            for block in target["blocks"].values():
                if block["opcode"] == "control_repeat_until":
                    block["opcode"] = "control_while"
                    replaced += 1
        if replaced != 1:
            raise AssertionError(f"{case}: expected one conditional loop")
        content["project.json"] = json.dumps(project).encode()
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as dst:
            for name, data in content.items():
                dst.writestr(name, data)
    return path


def execute(native: Path, project: Path, jit: str,
            adopt_after: int | None,
            bump_source_revision: bool = False,
            real_cache_reset: bool = False,
            edit_source_without_revision: bool = False,
            edit_parent_without_revision: bool = False,
            probe_source: bool = False,
            gc_after_adopt: bool = False,
            yield_after_callee: bool = False,
            only_after_callee: bool = False,
            only_root_after_loop: bool = False,
            empty_resume_plan: bool = False,
            drop_resume_loop: bool = False,
            root_only_producer: bool = False,
            trace_root_yield_count: bool = False,
            loop_ssa_diagnostic: bool = False,
            require_loop_ssa: bool = False,
            corrupt_loop_ssa: str = "",
            llvm_ssa_liveness: bool = False,
            detached_llvm_ssa: bool = False,
            check_detached_llvm_ssa: bool = False,
            trace_detached_llvm_ssa: bool = False,
            corrupt_detached_llvm_ssa: str = "",
            require_complete_llvm_ssa: bool = False,
            require_typed_ssa_manifest: bool = False,
            require_rooted_ssa_args: bool = False,
            lazy_typed_ssa: bool = False,
            require_generated_ssa_roots: bool = False,
            gc_during_ssa_transfer: bool = False,
            trace_native_gc_deferral: bool = False,
            inject_explicit_gc_before_ssa_root: bool = False,
            gc_threshold_mb: int | None = None,
            soft_gc_budget_mb: int | None = None,
            guard_native_var_slots: bool = False,
            guard_native_var_slots_fast: bool = False) -> tuple[dict, str]:
    env = dict(
        os.environ,
        XYO_JIT_COMMON_CFG="0",
        XYO_JIT_CALLFRAME="1" if jit != "off" else "0",
        XYO_RT_NATIVE_DEOPT_ONCE="1" if adopt_after is not None else "0",
        XYO_RT_NATIVE_DEOPT_AFTER=str(adopt_after or 1),
        XYO_RT_SOURCE_HANDOFF_PROBE="1" if probe_source else "0",
        XYO_RT_TEST_BUMP_SOURCE_REVISION="1" if bump_source_revision else "0",
        XYO_RT_TEST_REAL_CACHE_RESET="1" if real_cache_reset else "0",
        XYO_RT_TEST_DIRECT_SOURCE_EDIT="1" if edit_source_without_revision else "0",
        XYO_RT_TEST_DIRECT_PARENT_EDIT="1" if edit_parent_without_revision else "0",
        XYO_RT_TEST_GC_AFTER_ADOPT="1" if gc_after_adopt else "0",
        XYO_RT_TEST_YIELD_AFTER_CALLEE="1" if yield_after_callee else "0",
        XYO_RT_TEST_DEOPT_AFTER_CALLEE_ONLY="1" if only_after_callee else "0",
        XYO_RT_TEST_DEOPT_ROOT_AFTER_LOOP_ONLY=(
            "1" if only_root_after_loop else "0"
        ),
        XYO_RT_TEST_EMPTY_RESUME_PLAN="1" if empty_resume_plan else "0",
        XYO_RT_TEST_DROP_RESUME_LOOP="1" if drop_resume_loop else "0",
        XYO_RT_ROOT_ONLY_DEOPT="1" if root_only_producer else "0",
        XYO_RT_TEST_ROOT_YIELD_COUNT=(
            "1" if trace_root_yield_count else "0"
        ),
        XYO_JIT_CALLFRAME_LOOP_SSA_DIAGNOSTIC=(
            "1" if loop_ssa_diagnostic else "0"
        ),
        XYO_RT_REQUIRE_LOOP_SSA_DIAGNOSTIC=(
            "1" if require_loop_ssa else "0"
        ),
        XYO_RT_TEST_CORRUPT_LOOP_SSA=corrupt_loop_ssa,
        XYO_JIT_SSA_LIVENESS="1" if llvm_ssa_liveness else "0",
        XYO_RT_CAPTURE_LLVM_SSA="1" if detached_llvm_ssa else "0",
        XYO_RT_CHECK_LLVM_SSA_CAPTURE=(
            "1" if check_detached_llvm_ssa else "0"
        ),
        XYO_RT_TRACE_LLVM_SSA_CAPTURE=(
            "1" if trace_detached_llvm_ssa else "0"
        ),
        XYO_RT_TEST_CORRUPT_LLVM_SSA=corrupt_detached_llvm_ssa,
        XYO_RT_REQUIRE_COMPLETE_LLVM_SSA=(
            "1" if require_complete_llvm_ssa else "0"
        ),
        XYO_RT_REQUIRE_TYPED_SSA_MANIFEST=(
            "1" if require_typed_ssa_manifest else "0"
        ),
        XYO_RT_REQUIRE_ROOTED_SSA_ARGS=(
            "1" if require_rooted_ssa_args else "0"
        ),
        XYO_JIT_LAZY_TYPED_SSA="1" if lazy_typed_ssa else "0",
        XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=(
            "1" if require_generated_ssa_roots else "0"
        ),
        XYO_RT_GUARD_NATIVE_VAR_SLOTS=(
            "1" if guard_native_var_slots else "0"
        ),
        XYO_RT_GUARD_NATIVE_VAR_SLOTS_FAST=(
            "1" if guard_native_var_slots_fast else "0"
        ),
        XYO_RT_TEST_GC_DURING_SSA_TRANSFER=(
            "1" if gc_during_ssa_transfer else "0"
        ),
        XYO_RT_TRACE_NATIVE_GC_DEFERRAL=(
            "1" if trace_native_gc_deferral else "0"
        ),
        XYO_RT_TEST_EXPLICIT_GC_BEFORE_SSA_ROOT=(
            "1" if inject_explicit_gc_before_ssa_root else "0"
        ),
        XYO_PAR="0",
        XYO_DUMPVARS="json",
        XYO_EXIT_ON_DONE="1",
    )
    if gc_threshold_mb is not None:
        if not 1 <= gc_threshold_mb <= 256:
            raise ValueError("GC threshold must be 1..256 MiB")
        env["XYO_GC_THRESHOLD"] = str(gc_threshold_mb)
    if soft_gc_budget_mb is not None:
        if not 1 <= soft_gc_budget_mb <= 1024:
            raise ValueError("soft GC budget must be 1..1024 MiB")
        env["XYO_RT_GC_SOFT_BUDGET_MB"] = str(soft_gc_budget_mb)
    else:
        env.pop("XYO_RT_GC_SOFT_BUDGET_MB", None)
    proc = subprocess.run(
        [str(native), "run", str(project), "--jit", jit,
         "--fps", "30", "--frames", "100"],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=120,
    )
    if proc.returncode:
        raise AssertionError(
            f"{project.name}/{jit}/adopt={adopt_after} exited {proc.returncode}: "
            f"{proc.stderr[-1500:]}"
        )
    match = re.search(r'(?s)(\{\s*"Bench".*\})', proc.stdout)
    if not match:
        raise AssertionError(
            f"{project.name}: missing JSON: {proc.stdout[-500:]}"
        )
    return json.loads(match.group(1)), proc.stderr


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    native = args.native.resolve()
    passed_runs = 0
    for case in (*SUPPORTED, *UNSUPPORTED):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        native_state, _ = execute(native, source, "eager", None)
        if expected != native_state:
            raise AssertionError(f"{case}: native baseline mismatch")
        checkpoint_set = (
            # Terminal branch and loop-arm fixtures contain exactly two
            # native body yields; post-return is tested separately.
            (1, 2) if case.startswith((
                "root_terminal_", "root_loop_terminal_",
                "procedure_loop_terminal_"))
            else (1, 2, 3, 4, 6, 8, 12) if case == "recursive"
            else (1, 2) if case in (
                "root_if_call", "root_if_terminal_chain",
                "root_ifelse_terminal_chain", "root_repeat_one_call",
                "root_repeat_one_terminal_chain",
                "root_nested_repeat_one_call", "root_if_repeat_one_call")
            else (1, 3, 5, 8, 12, 20, 40, 100, 200) if case ==
                "root_nested_repeat_nineteen_seven"
            else (1, 3, 5, 8, 12, 20, 35) if case ==
                "root_repeat_nineteen_call"
            else (1, 2, 3, 4, 5) if case in (
                "root_repeat_three_call", "root_if_repeat_three_call",
                "root_repeat_dynamic_three_call")
            else (1, 2) if case in (
                "root_repeat_dynamic_one_call", "root_repeat_expr_one_call")
            else (1, 2, 3) if case in (
                "parent_repeat_terminal_chain",
                "parent_if_terminal", "parent_nested_if_terminal",
                "parent_ifelse_terminal", "parent_if_terminal_changed",
                "parent_repeat_if_terminal", "parent_repeat_ifelse_terminal")
            else (1,) if case == "bounded_recursive_thirtytwo_string"
            else (1, 2, 3, 4, 10, 20, 32, 40)
            if case == "bounded_recursive_twenty_string"
            else (1, 2, 3, 6, 12, 20)
            if case == "bounded_recursive_twelve_string"
            else (4, 5, 6, 7, 8)
            if case.startswith("bounded_recursive_if") or
               case.startswith("bounded_recursive_nested_if")
            else (1, 2, 3, 4, 5, 6, 8, 10, 12, 14)
            if case == "bounded_recursive_multi"
            else (1, 2, 3, 4, 5, 6, 8, 10, 12)
            if case == "bounded_recursive_deep_string"
            else (1, 2, 3, 4, 5, 6) if case.startswith("bounded_recursive")
            else (1, 3, 4, 5, 7, 8) if case.startswith("parent_")
            else (1, 5, 6) if case in ("while_nested", "until_nested")
            else (1, 3, 5) if case in ("nested_repeat", "triple_repeat", "nested_prefix")
            else (1, 5, 9) if case == "count_arg"
            else (1,) if case == "repeat_one" or case in UNSUPPORTED
            else (1, 2, 3)
        )
        for checkpoint in checkpoint_set:
            transferred, log = execute(native, source, "eager", checkpoint)
            if expected != transferred:
                raise AssertionError(
                    f"{case} at yield {checkpoint}: interpreter/native/transferred "
                    f"mismatch\ninterp={expected}\nnative={native_state}\n"
                    f"transferred={transferred}\n{log[-1700:]}"
                )
            success = len(re.findall(
                r"\[native-simple-deopt\].*adopted=1", log
            ))
            if case in SUPPORTED and success != 1:
                raise AssertionError(
                    f"{case}: expected one actual adoption at yield "
                    f"{checkpoint}: {log[-1400:]}"
                )
            if case in UNSUPPORTED and success:
                raise AssertionError(f"{case}: unsupported control flow was adopted")
            passed_runs += 1
        if case == "bounded_recursive_if":
            for early in (1, 2, 3):
                result, log = execute(native, source, "eager", early)
                if result != expected:
                    raise AssertionError(
                        f"{case} at ProcedureEntry yield {early}: variable mismatch"
                    )
                attempts = re.findall(
                    r"\[native-simple-deopt\].*adopted=(\d)", log
                )
                # AFTER=N is a lower bound: a rejected ProcedureEntry
                # may legitimately be followed by a later committed
                # AfterLoopBody adoption in the SAME run. Verify the
                # first attempt was rejected instead of requiring all
                # subsequent checkpoints to be rejected as well.
                if not attempts or attempts[0] != "0":
                    raise AssertionError(
                        f"{case}: unsafe ProcedureEntry first attempt was "
                        f"not rejected: {log[-1200:]}"
                    )
                passed_runs += 1
        if case in ("bounded_recursive_twelve_string",
                    "bounded_recursive_twenty_string",
                    "bounded_recursive_thirtytwo_string"):
            # The deep call must be a *real* suspended native CallFrame:
            # first yield occurs in the innermost recursive procedure.
            # <=32 frames can be adopted; 34 frames must fail closed,
            # but later shallower checkpoints are independently safe.
            target_depth = (
                34 if case == "bounded_recursive_thirtytwo_string" else
                22 if case == "bounded_recursive_twenty_string" else 14
            )
            value, trace = execute(
                native, source, "eager", 1, probe_source=True,
            )
            if value != expected:
                raise AssertionError(
                    f"{case}: deep native continuation output mismatch"
                )
            # A native procedure may yield at ProcedureEntry BEFORE
            # reaching its leaf loop. Match every source image depth to
            # the adoption decision at that SAME scheduler suspension.
            snapshots = [
                int(d) for d in re.findall(
                    r"\[source-handoff\].*valid=1 depth=(\d+)", trace
                )
            ]
            attempts = [
                int(a) for a in re.findall(
                    r"\[native-simple-deopt\].*adopted=(\d+)", trace
                )
            ]
            if len(snapshots) != len(attempts) or not snapshots or (
                max(snapshots) != target_depth
            ):
                raise AssertionError(
                    f"{case}: invalid depth/adoption trace: "
                    f"{trace[-1800:]}"
                )
            pairs = list(zip(snapshots, attempts))
            if any(depth > 32 and adopted for depth, adopted in pairs):
                raise AssertionError(
                    f"{case}: adopted an over-limit native source stack"
                )
            if target_depth <= 32 and not any(
                depth == target_depth and adopted for depth, adopted in pairs
            ):
                raise AssertionError(
                    f"{case}: no actual innermost deep CallFrame adoption: "
                    f"{trace[-1800:]}"
                )
            if not any(adopted for _depth, adopted in pairs):
                raise AssertionError(
                    f"{case}: never safely adopted a real native frame"
                )
            passed_runs += 1
        if case in (
            "heap_string", "nested_call_string", "parent_ifelse_repeat_string",
            "bounded_recursive_deep_string", "bounded_recursive_twelve_string",
            "bounded_recursive_nested_if_string", "unicode_gc_string",
            "bounded_recursive_unicode_string",
            "root_if_call", "root_until_call", "root_while_call",
            "root_if_until_call", "root_repeat_one_call",
            "root_repeat_one_terminal_chain",
            "root_nested_repeat_one_call", "root_if_repeat_one_call",
            "root_repeat_three_call", "root_repeat_dynamic_one_call",
            "root_repeat_expr_one_call", "root_repeat_nineteen_call",
            "root_nested_repeat_nineteen_seven",
            "root_if_repeat_three_call", "root_repeat_dynamic_three_call",
            "parent_repeat_terminal_chain",
            "nested_call_terminal_script", "root_if_terminal_chain",
            "root_ifelse_terminal_chain", "root_until_terminal_chain",
        ):
            # Force a full GC only AFTER the scheduler has released the
            # suspended native Fiber and rooted source call stack. The
            # restored interpreter parameter maps must now own every
            # string independently, including nested recursive arguments.
            checkpoint = (
                5 if case == "bounded_recursive_nested_if_string" else 1
            )
            actual, diagnostics = execute(
                native, source, "eager", checkpoint,
                gc_after_adopt=True,
            )
            if actual != expected:
                raise AssertionError(
                    f"{case}: forced post-Fiber GC corrupted Scratch values\n"
                    f"expected={expected}\nactual={actual}"
                )
            adopts = re.findall(
                r"\[native-simple-deopt\].*adopted=(\d+)", diagnostics
            )
            collections = re.findall(
                r"\[native-gc-after-adopt\].*collected=1"
                r" objects_before=(\d+) objects_after=(\d+)",
                diagnostics,
            )
            if adopts.count("1") != 1 or len(collections) != 1:
                raise AssertionError(
                    f"{case}: no verified native adoption + full GC: "
                    f"{diagnostics[-1500:]}"
                )
            passed_runs += 1
        if case in ("heap_string", "nested_repeat", "triple_repeat",
                    "while", "triple_call", "parent_repeat_string"):
            # A source revision bump occurs AFTER a real LLVM Fiber yield
            # and BEFORE the scheduler attempts a native-to-interpreter
            # handoff. Even when all block IDs remain unchanged, the
            # exported image was compiled from the old source version.
            at_yield = 3 if case == "triple_repeat" else 1
            refused, diagnostics = execute(
                native, source, "eager", at_yield,
                bump_source_revision=True,
            )
            if refused != expected:
                raise AssertionError(
                    f"{case}: source-change rejection changed Scratch variables"
                    f"\nexpected={expected}\nactual={refused}"
                )
            bumps = re.findall(
                r"\[native-source-revision-bump\] compiled=(\d+) live=(\d+)",
                diagnostics,
            )
            if len(bumps) != 1 or int(bumps[0][0]) == int(bumps[0][1]):
                raise AssertionError(
                    f"{case}: missing real post-yield revision change: "
                    f"{diagnostics[-1500:]}"
                )
            if re.search(r"\[native-simple-deopt\].*adopted=1", diagnostics):
                raise AssertionError(
                    f"{case}: stale compiled image was adopted: {diagnostics[-1500:]}"
                )
            passed_runs += 1
            if case == "heap_string":
                no_gc, no_gc_log = execute(
                    native, source, "eager", 1,
                    bump_source_revision=True, gc_after_adopt=True,
                )
                if no_gc != expected or "[native-gc-after-adopt]" in no_gc_log:
                    raise AssertionError(
                        f"{case}: refused deopt nevertheless ran post-adopt GC"
                    )
                passed_runs += 1
        if case in ("heap_string", "bounded_recursive_string",
                    "bounded_recursive_if_string", "parent_branch_call"):
            # A raw Block edit does NOT call Blocks::resetCache() and
            # deliberately keeps revision identical. The immutable
            # dual-word source digest must still reject this stale image.
            at_yield = 4 if case == "bounded_recursive_if_string" else 1
            refused, diagnostics = execute(
                native, source, "eager", at_yield,
                edit_source_without_revision=True,
            )
            if refused != expected:
                raise AssertionError(
                    f"{case}: same-revision direct edit changed Scratch variables"
                    f"\nexpected={expected}\nactual={refused}"
                )
            edits = re.findall(
                r"\[native-source-content-edit\] revision=(\d+) mutated=1",
                diagnostics,
            )
            if len(edits) != 1 or int(edits[0]) <= 0:
                raise AssertionError(
                    f"{case}: missing real same-revision source edit: "
                    f"{diagnostics[-1500:]}"
                )
            if re.search(r"\[native-simple-deopt\].*adopted=1", diagnostics):
                raise AssertionError(
                    f"{case}: source digest mismatch was adopted: "
                    f"{diagnostics[-1500:]}"
                )
            passed_runs += 1
        if case in ("parent_ifelse_repeat_string", "leaf_ifelse_repeat",
                    "parent_repeat_if_repeat"):
            # Mutate the lexical parent of the suspended native callsite
            # without changing Blocks::revision(). This parent is critical
            # to the restored IF/LOOP stack topology and is included in
            # the immutable compiled source fingerprint.
            result, diagnostics = execute(
                native, source, "eager", 1,
                edit_parent_without_revision=True,
            )
            if result != expected:
                raise AssertionError(
                    f"{case}: parent-link edit altered native output\n"
                    f"expected={expected}\nactual={result}"
                )
            edits = re.findall(
                r"\[native-source-parent-edit\] revision=(\d+) mutated=1",
                diagnostics,
            )
            if len(edits) != 1:
                raise AssertionError(
                    f"{case}: parent-link test never mutated source: "
                    f"{diagnostics[-1200:]}"
                )
            if re.search(r"\[native-simple-deopt\].*adopted=1", diagnostics):
                raise AssertionError(
                    f"{case}: stale parent-link source image was adopted"
                )
            passed_runs += 1
        print(
            f"PASS {case}: {len(checkpoint_set)} "
            f"{'actual handoff checkpoints' if case in SUPPORTED else 'safe rejections'}",
            flush=True,
        )
    # AfterCallee is observable only at an explicit post-return suspension.
    # Unlike the AfterLoopBody cases above, the completed child procedure
    # must NOT be replayed, and exactly one source post-call successor is
    # reconstructed from the original immutable return-site graph.
    postcall_supported = (
        "nested_call", "nested_call_string", "triple_call",
        "parent_repeat", "parent_repeat_terminal",
        "parent_if_repeat", "parent_ifelse_repeat_string",
        "parent_nested_branch_call",
        "parent_if_terminal", "parent_nested_if_terminal",
        "parent_ifelse_terminal", "parent_if_terminal_changed",
        "parent_repeat_if_terminal", "parent_repeat_ifelse_terminal",
        "bounded_recursive_unicode_string",
        "nested_call_terminal", "nested_call_terminal_script",
        "triple_call_terminal", "parent_repeat_terminal_chain",
        "root_if_terminal_chain", "root_ifelse_terminal_chain",
        "root_until_terminal_chain", "root_repeat_one_terminal_chain",
        # IR-001's root counted-loop CallFrame now preserves the exact
        # root lexical path; previously rejected mirror fixtures can
        # finish an IF/ELSE arm or counted root repeat without replay.
        "procedure_return", "branch_return", "branch_arms",
        "nested_branch_arms", "loop_return",
    )
    postcall_rejected = ()
    for case in (*postcall_supported, *postcall_rejected):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        # Multiple successive post-call boundaries catch duplicated
        # prefix effects across different nonwarp invocations.
        for checkpoint in ((1, 5) if case in (
                "parent_repeat", "parent_repeat_terminal",
                "loop_return", "bounded_recursive_unicode_string")
                else (1,)):
            actual, trace = execute(
                native, source, "eager", checkpoint,
                yield_after_callee=True, only_after_callee=True,
                probe_source=True,
                gc_after_adopt=("string" in case or case in (
                    "root_if_terminal_chain", "root_ifelse_terminal_chain",
                    "root_until_terminal_chain",
                    "parent_repeat_terminal_chain",
                    "nested_call_terminal_script")),
            )
            if actual != expected:
                raise AssertionError(
                    f"{case}: post-return native deopt replayed/omitted effects"
                    f"\ninterp={expected}\nactual={actual}\n{trace[-1600:]}"
                )
            attempts = re.findall(
                r"\[native-simple-deopt\].*adopted=(\d+)", trace
            )
            postcall_sites = [
                (int(kind), int(valid))
                for kind, valid in re.findall(
                    r"\[native-postcall-site\] kind=(-?\d+)"
                    r" target=\d+ call=\d+ valid=(\d+)", trace
                )
            ]
            if len(postcall_sites) != len(attempts):
                raise AssertionError(
                    f"{case}: post-call successor/adoption log misaligned"
                )
            expected_kind = (
                3 if case in (
                    "nested_call_terminal", "nested_call_terminal_script",
                    "triple_call_terminal", "parent_repeat_terminal_chain",
                    "root_if_terminal_chain", "root_ifelse_terminal_chain",
                    "root_until_terminal_chain",
                    "root_repeat_one_terminal_chain",
                    "procedure_return")
                else 1 if case.startswith("parent_if_terminal") or
                     case in ("branch_return", "branch_arms",
                              "nested_branch_arms",
                              "parent_nested_if_terminal",
                              "parent_ifelse_terminal",
                              "parent_repeat_if_terminal",
                              "parent_repeat_ifelse_terminal")
                else 2 if case in ("parent_repeat_terminal",
                                  "bounded_recursive_unicode_string",
                                  "loop_return")
                else 0
            )
            if case in postcall_supported and not any(
                adopted == "1" and valid and kind == expected_kind
                for (kind, valid), adopted in zip(postcall_sites, attempts)
            ):
                raise AssertionError(
                    f"{case}: wrong source return kind (expected "
                    f"{expected_kind}) or no actual adoption: {trace[-1700:]}"
                )
            if case in postcall_supported and attempts.count("1") != 1:
                raise AssertionError(
                    f"{case}: expected exactly one AfterCallee adoption"
                )
            if case in postcall_rejected and any(v == "1" for v in attempts):
                raise AssertionError(
                    f"{case}: unimplemented ProcedureExit was adopted"
                )
            passed_runs += 1
        if case == "parent_if_terminal_changed":
            # Corrupt the immutable source revision at the *post-call*
            # scheduler boundary: even a valid structural AfterBranch
            # edge must not permit adoption from a stale compiled image.
            result, diagnostic = execute(
                native, source, "eager", 1,
                yield_after_callee=True, only_after_callee=True,
                bump_source_revision=True, gc_after_adopt=True,
            )
            if result != expected or (
                "[native-source-revision-bump]" not in diagnostic
            ) or re.search(
                r"\[native-simple-deopt\].*adopted=1", diagnostic
            ) or "[native-gc-after-adopt]" in diagnostic:
                raise AssertionError(
                    f"{case}: stale AfterBranch image was adopted: "
                    f"{diagnostic[-1600:]}"
                )
            passed_runs += 1
        print(
            f"PASS {case}: post-callee "
            f"{'adopted' if case in postcall_supported else 'refused'}",
            flush=True,
        )
    # A loop's final IF arm can finish inside a *procedure* frame,
    # not only at the root hat. ProcedureExit may propagate to an
    # ancestor with a LoopLatch; no branch reporter may be re-evaluated.
    procedure_latch = {
        "procedure_loop_terminal_if": (2, 2),
        "procedure_loop_terminal_ifelse_else": (2, 2),
        "procedure_loop_terminal_nested_if": (2, 2),
        "procedure_loop_terminal_double_repeat": (2, 2),
        "procedure_loop_terminal_chain": (3, 3),
    }
    for case, (kind, depth) in procedure_latch.items():
        project = fixture(args.out, case)
        expected, _ = execute(native, project, "off", None)
        for checkpoint in (1, 2):
            actual, trace = execute(
                native, project, "eager", checkpoint,
                yield_after_callee=True, only_after_callee=True,
                probe_source=True, gc_after_adopt=True,
            )
            sites = re.findall(
                r"\[native-postcall-site\] kind=(-?\d+)"
                r" target=\d+ call=\d+ valid=(\d+) depth=(\d+)",
                trace,
            )
            attempts = re.findall(
                r"\[native-simple-deopt\].*adopted=(\d+)", trace,
            )
            if actual != expected or attempts.count("1") != 1 or (
                (str(kind), "1", str(depth)) not in sites
            ) or trace.count("[native-gc-after-adopt]") != 1:
                raise AssertionError(
                    f"{case}: nonroot terminal IF/LoopLatch checkpoint "
                    f"{checkpoint} was replayed or refused: {trace[-1800:]}"
                )
            passed_runs += 1
        for mode in ("revision", "digest", "real_cache_reset"):
            actual, trace = execute(
                native, project, "eager", 1,
                yield_after_callee=True, only_after_callee=True,
                bump_source_revision=mode in ("revision", "real_cache_reset"),
                real_cache_reset=mode == "real_cache_reset",
                edit_source_without_revision=mode == "digest",
                gc_after_adopt=True,
            )
            marker = (
                "[native-source-content-edit]" if mode == "digest"
                else "[native-real-cache-reset]" if mode == "real_cache_reset"
                else "[native-source-revision-bump]"
            )
            if actual != expected or marker not in trace or (
                "adopted=1" in trace or "[native-gc-after-adopt]" in trace
            ):
                raise AssertionError(
                    f"{case}: stale nonroot loop {mode} was adopted: "
                    f"{trace[-1800:]}"
                )
            passed_runs += 1
        print(f"PASS {case}: 2 nonroot postreturns + 3 stale-source refusals",
              flush=True)

    for case in (
        "root_repeat_three_call", "root_repeat_dynamic_three_call",
        "root_nested_repeat_nineteen_seven", "recursive",
    ):
        # Fingerprint and revision are independent safeguards; neither
        # may authorize stale root counters. The opt-in revision probe
        # deliberately preserves all live native cache allocations.
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        for mode in ("digest", "revision"):
            # Test independent guards; a revision bump must not free
            # cached BlockCached objects held by the suspended Fiber.
            actual, trace = execute(
                native, source, "eager", 1,
                edit_source_without_revision=(mode == "digest"),
                bump_source_revision=(mode == "revision"),
                gc_after_adopt=True,
            )
            marker = ("[native-source-content-edit]"
                      if mode == "digest" else
                      "[native-source-revision-bump]")
            if actual != expected or marker not in trace or re.search(
                r"\[native-simple-deopt\].*adopted=1", trace
            ) or "[native-gc-after-adopt]" in trace:
                raise AssertionError(
                    f"{case}: stale {mode} root loop image was adopted:"
                    f" {trace[-1800:]}"
                )
            passed_runs += 1
        print(f"PASS {case}: stale source digest/revision rejected", flush=True)
    # The depth-one root AfterLoopBody is distinct from AfterCallee.
    # Without rebuilding its active source loop stack the old adopter
    # reported success but silently discarded the remaining 2000/20000
    # native iterations and the following Scratch instruction.
    for case in (
        "root_afterloop_2000", "root_afterloop_20000",
        "root_afterloop_dynamic", "root_afterloop_nested_if",
        "root_afterloop_nested_repeat", "root_afterloop_repeat_if",
        "root_afterloop_if_over", "root_afterloop_sibling",
        "root_afterloop_until", "root_afterloop_ifelse",
        "root_afterloop_triple",
        "root_afterloop_dynamic_mutated",
        "root_afterloop_nested_dynamic_mutated",
        "root_afterloop_two_hats", "root_afterloop_two_hats_dynamic",
        "root_afterloop_terminal_if", "root_afterloop_terminal_ifelse",
        "root_afterloop_terminal_nested_if",
        "root_afterloop_terminal_repeat_if",
        "root_afterloop_terminal_repeat_ifelse",
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        baseline, _ = execute(native, source, "eager", None)
        actual, trace = execute(
            native, source, "eager", 1,
            only_root_after_loop=True, gc_after_adopt=True,
        )
        if expected != baseline or expected != actual or (
            len(re.findall(
                r"\[native-simple-deopt\].*adopted=1", trace,
            )) != 1 or
            trace.count("[native-gc-after-adopt]") != 1
        ):
            raise AssertionError(
                f"{case}: root AfterLoopBody dropped remaining effects: "
                f"interp={expected}, native={baseline}, restored={actual}"
                f" trace={trace[-1700:]}"
            )
        passed_runs += 1
        for mode in ("revision", "digest", "real_cache_reset"):
            rejected, log = execute(
                native, source, "eager", 1,
                only_root_after_loop=True, gc_after_adopt=True,
                bump_source_revision=mode in ("revision", "real_cache_reset"),
                real_cache_reset=mode == "real_cache_reset",
                edit_source_without_revision=mode == "digest",
            )
            marker = (
                "[native-source-content-edit]" if mode == "digest"
                else "[native-real-cache-reset]" if mode == "real_cache_reset"
                else "[native-source-revision-bump]"
            )
            if rejected != expected or marker not in log or (
                "adopted=1" in log or
                "[native-gc-after-adopt]" in log
            ):
                raise AssertionError(
                    f"{case}: stale root AfterLoopBody {mode} was adopted: "
                    f"{log[-1700:]}"
                )
            passed_runs += 1
        print(f"PASS {case}: root AfterLoopBody + 3 invalidation guards",
              flush=True)

    # Guard the adopter itself against a future regression that returns
    # a validated but EMPTY execution plan. A test-only hook replaces
    # the computed plan with an empty one before Thread state mutation.
    # A real terminal ScriptExit must still be accepted when not forced.
    for case, root_loop in (
        ("root_afterloop_2000", True),
        ("root_afterloop_triple", True),
        ("root_repeat_three_call", False),
        ("root_terminal_if", False),
        ("terminal_call", False),
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        actual, log = execute(
            native, source, "eager", 1,
            only_root_after_loop=root_loop,
            only_after_callee=not root_loop,
            yield_after_callee=not root_loop,
            empty_resume_plan=True,
            gc_after_adopt=True,
        )
        if actual != expected or not re.search(
            r"\[native-simple-deopt\].*adopted=0", log,
        ) or re.search(
            r"\[native-simple-deopt\].*adopted=1", log,
        ) or "[native-gc-after-adopt]" in log:
            raise AssertionError(
                f"{case}: empty restore plan retired native execution: "
                f"{log[-1700:]}"
            )
        passed_runs += 1
        print(f"PASS {case}: empty restore plan refused before Fiber release",
              flush=True)

    # Unlike an empty plan, a corrupted nonempty IF -> repeat plan
    # satisfies the basic executable-frame check but loses the exact
    # saved loop-latch PC. Deliberately drop only its innermost loop:
    # the adopter must independently compare it with the native leaf.
    for case, root_only in (
        ("root_afterloop_nested_if", True),
        ("root_afterloop_terminal_repeat_if", True),
        ("root_afterloop_nested_dynamic_mutated", True),
        ("nested_repeat", False),
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 1,
            only_root_after_loop=root_only,
            drop_resume_loop=True,
            gc_after_adopt=True,
        )
        if actual != expected or not re.search(
            r"\[native-simple-deopt\].*adopted=0", trace,
        ) or re.search(
            r"\[native-simple-deopt\].*adopted=1", trace,
        ) or "[native-gc-after-adopt]" in trace:
            raise AssertionError(
                f"{case}: missing native loop frame was adopted: "
                f"{trace[-1700:]}"
            )
        passed_runs += 1
        print(f"PASS {case}: missing source loop-latch frame refused",
              flush=True)

    # Root-only scripts with NO procedures_call previously had no
    # native CallFrame session and therefore no LLVM->Interpreter handoff.
    # Verify an opt-in compile-verified root session with real completed
    # loop PCs, post-Fiber GC and three independent stale-source guards.
    for case in (
        "root_nocall_simple", "root_nocall_dynamic",
        "root_nocall_nested", "root_nocall_terminal_if",
        "root_nocall_two_hats",
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        ordinary, _ = execute(native, source, "eager", None)
        resumed, trace = execute(
            native, source, "eager", 1,
            only_root_after_loop=True, root_only_producer=True,
            gc_after_adopt=True,
        )
        # Two independent native flag hats each legitimately transfer.
        # Neither is allowed to borrow the other script's source digest.
        expected_handoffs = 2 if case == "root_nocall_two_hats" else 1
        if ordinary != expected or resumed != expected or (
            len(re.findall(r"\[native-simple-deopt\].*adopted=1", trace))
            != expected_handoffs or
            trace.count("[native-gc-after-adopt]") != expected_handoffs
        ):
            raise AssertionError(
                f"{case}: source-only root loop handoff failed: "
                f"expected={expected}, ordinary={ordinary}, "
                f"resumed={resumed}, trace={trace[-1800:]}"
            )
        passed_runs += 1
        for mode in ("revision", "digest", "real_cache_reset"):
            rejected, log = execute(
                native, source, "eager", 1,
                only_root_after_loop=True, root_only_producer=True,
                gc_after_adopt=True,
                bump_source_revision=mode in ("revision", "real_cache_reset"),
                real_cache_reset=mode == "real_cache_reset",
                edit_source_without_revision=mode == "digest",
            )
            marker = (
                "[native-source-content-edit]" if mode == "digest"
                else "[native-real-cache-reset]" if mode == "real_cache_reset"
                else "[native-source-revision-bump]"
            )
            if rejected != expected or marker not in log or (
                "adopted=1" in log or "[native-gc-after-adopt]" in log
            ):
                raise AssertionError(
                    f"{case}: root-only stale {mode} was adopted: "
                    f"{log[-1800:]}"
                )
            passed_runs += 1
        print(f"PASS {case}: real root-only no-callee transfer and "
              "three independent source refusals", flush=True)

    # The standalone root producer itself is the production opt-in:
    # no strict test-only phase gate should be needed to adopt the
    # first genuine completed body from a no-callee root script.
    for case in ("root_nocall_simple", "root_nocall_nested"):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 1,
            root_only_producer=True, gc_after_adopt=True,
        )
        if actual != expected or trace.count("adopted=1") != 1 or (
            trace.count("[native-gc-after-adopt]") != 1
        ):
            raise AssertionError(
                f"{case}: root-only producer without test phase gate "
                f"failed: {trace[-1500:]}"
            )
        passed_runs += 1
        print(f"PASS {case}: production opt-in root deopt without test gate",
              flush=True)

    no_producer = fixture(args.out, "root_nocall_simple")
    no_producer_expected, _ = execute(native, no_producer, "off", None)
    no_producer_actual, no_producer_trace = execute(
        native, no_producer, "eager", 1,
        root_only_producer=False, gc_after_adopt=True,
    )
    if no_producer_actual != no_producer_expected or (
        "adopted=1" in no_producer_trace or
        "[native-gc-after-adopt]" in no_producer_trace
    ):
        raise AssertionError(
            "no-procedure-call root must remain native unless the "
            "source-root producer is explicitly enabled"
        )
    passed_runs += 1
    print("PASS root_nocall_simple: root producer disabled is failclosed",
          flush=True)

    # IR-001's source SSA diagnostic currently captures only emitted
    # lexical numeric loop counters. RT may require the captured raw
    # slot words to agree exactly with its independently decoded
    # CallFrame at a committed loop latch. This is NOT a generic SSA
    # live-value or effect-continuity proof.
    for case, root_producer, strict_root_loop in (
        ("root_nocall_simple", True, True),
        ("root_nocall_nested", True, True),
        ("root_afterloop_nested_if", False, True),
        ("nested_repeat", False, False),
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        good, good_log = execute(
            native, source, "eager", 1,
            only_root_after_loop=strict_root_loop,
            root_only_producer=root_producer,
            loop_ssa_diagnostic=True,
            require_loop_ssa=True,
            gc_after_adopt=True,
        )
        if good != expected or len(re.findall(
            r"\[native-simple-deopt\].*adopted=1", good_log
        )) != 1 or good_log.count("[native-gc-after-adopt]") != 1:
            raise AssertionError(
                f"{case}: valid LLVM lexical-counter sidecar rejected: "
                f"{good_log[-1800:]}"
            )
        passed_runs += 1
        for mode in ("value", "slot", "drop", "absent"):
            result, trace = execute(
                native, source, "eager", 1,
                only_root_after_loop=strict_root_loop,
                root_only_producer=root_producer,
                loop_ssa_diagnostic=mode != "absent",
                require_loop_ssa=True,
                corrupt_loop_ssa="" if mode == "absent" else mode,
                gc_after_adopt=True,
            )
            if result != expected or not re.search(
                r"\[native-simple-deopt\].*adopted=0", trace
            ) or re.search(
                r"\[native-simple-deopt\].*adopted=1", trace
            ) or "[native-gc-after-adopt]" in trace:
                raise AssertionError(
                    f"{case}: {mode} SSA-sidecar image was adopted: "
                    f"{trace[-1800:]}"
                )
            passed_runs += 1
        print(
            f"PASS {case}: verified lexical-loop SSA bits + 4 "
            "fail-closed sidecar corruption/missing cases", flush=True,
        )

    # A repeat-until loop has NO numeric loop counter. Its explicitly
    # captured EMPTY lexical SSA manifest is valid and must not be
    # mistaken for an ABSENT snapshot, which fails closed.
    ssa_empty = fixture(args.out, "root_afterloop_until")
    expected_ssa_empty, _ = execute(native, ssa_empty, "off", None)
    for mode in ("empty", "drop", "absent"):
        restored, trace = execute(
            native, ssa_empty, "eager", 1,
            only_root_after_loop=True, require_loop_ssa=True,
            loop_ssa_diagnostic=mode != "absent",
            corrupt_loop_ssa="drop" if mode == "drop" else "",
            gc_after_adopt=True,
        )
        valid = mode == "empty"
        if restored != expected_ssa_empty or (
            trace.count("adopted=1") != (1 if valid else 0)
        ) or (
            trace.count("[native-gc-after-adopt]") !=
            (1 if valid else 0)
        ) or (not valid and "adopted=0" not in trace):
            raise AssertionError(
                f"root_afterloop_until: {mode} empty SSA certificate "
                f"handled incorrectly: {trace[-1600:]}"
            )
        passed_runs += 1
    print("PASS root_afterloop_until: explicit empty SSA != missing SSA",
          flush=True)

    # IR-001's procedure yield metric is intentionally zero for a
    # root-only script. Verify that RT's scheduler counts actual
    # completed root latches independently for each Thread, including
    # deferred handoffs after >1 native yield.
    for case, checkpoints in (
        ("root_nocall_simple", (2, 17, 233, 2199)),
        ("root_nocall_dynamic", (17, 233)),
        ("root_nocall_nested", (2, 17, 233, 6599)),
        ("root_nocall_two_hats", (2, 17, 233, 1699)),
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        for nth in checkpoints:
            result, trace = execute(
                native, source, "eager", nth,
                root_only_producer=True, trace_root_yield_count=True,
                gc_after_adopt=True,
            )
            hats = 2 if case == "root_nocall_two_hats" else 1
            markers = re.findall(
                r"\[native-root-yield\] script=([^ ]+) "
                r"count=(\d+) target=(\d+)", trace,
            )
            if (result != expected or
                trace.count("adopted=1") != hats or
                trace.count("[native-gc-after-adopt]") != hats or
                len(markers) != hats or
                any(int(count) != nth or int(target) != nth
                    for _, count, target in markers) or
                len({script for script, _, _ in markers}) != hats):
                raise AssertionError(
                    f"{case}: failed at native root latch {nth} "
                    f"(expected {hats} independent hats): {trace[-1700:]}"
                )
            passed_runs += 1
        print(f"PASS {case}: late root-only checkpoints {checkpoints}",
              flush=True)

    # A changed source cannot be adopted at a LATER checkpoint either.
    # The original suspended LLVM Fiber must continue and survive cache
    # retirement / collection with no second interpreter execution.
    for mode in ("revision", "digest", "real_cache_reset"):
        source = fixture(args.out, "root_nocall_nested")
        expected, _ = execute(native, source, "off", None)
        result, trace = execute(
            native, source, "eager", 17,
            root_only_producer=True, trace_root_yield_count=True,
            gc_after_adopt=True,
            bump_source_revision=mode in ("revision", "real_cache_reset"),
            edit_source_without_revision=mode == "digest",
            real_cache_reset=mode == "real_cache_reset",
        )
        marker = (
            "[native-source-content-edit]" if mode == "digest"
            else "[native-real-cache-reset]" if mode == "real_cache_reset"
            else "[native-source-revision-bump]"
        )
        if (result != expected or marker not in trace or
            "[native-root-yield]" not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace):
            raise AssertionError(
                f"root_nocall_nested late-yield stale {mode} adopted: "
                f"{trace[-1700:]}"
            )
        passed_runs += 1
    print("PASS root_nocall_nested: late-yield 3 stale-source refusals",
          flush=True)

    # A requested checkpoint beyond the script's natural 2200 yields is
    # NOT a license to deopt at the final ScriptExit or invent a latch.
    source = fixture(args.out, "root_nocall_simple")
    expected, _ = execute(native, source, "off", None)
    result, trace = execute(
        native, source, "eager", 2201,
        root_only_producer=True, trace_root_yield_count=True,
        gc_after_adopt=True,
    )
    if result != expected or "adopted=1" in trace or (
        "[native-root-yield]" in trace or
        "[native-gc-after-adopt]" in trace
    ):
        raise AssertionError(
            "root_nocall_simple: nonexistent 2201st latch was adopted"
        )
    passed_runs += 1
    print("PASS root_nocall_simple: beyond-final checkpoint refused",
          flush=True)

    # The new host-side yield count must not bypass IR-001's separate
    # lexical numeric SSA sidecar consistency check.
    source = fixture(args.out, "root_nocall_nested")
    expected, _ = execute(native, source, "off", None)
    for mode in ("correct", "corrupt"):
        result, trace = execute(
            native, source, "eager", 17,
            root_only_producer=True, trace_root_yield_count=True,
            loop_ssa_diagnostic=True, require_loop_ssa=True,
            corrupt_loop_ssa="value" if mode == "corrupt" else "",
            gc_after_adopt=True,
        )
        succeeds = mode == "correct"
        if (result != expected or
            trace.count("adopted=1") != int(succeeds) or
            trace.count("[native-gc-after-adopt]") != int(succeeds) or
            not re.search(
                r"\[native-root-yield\].*count=17 target=17", trace
            ) or (not succeeds and "adopted=0" not in trace)):
            raise AssertionError(
                f"root_nocall_nested late-yield SSA {mode} mismatch: "
                f"{trace[-1600:]}"
            )
        passed_runs += 1
    print("PASS root_nocall_nested: late-yield SSA sidecar enforced",
          flush=True)

    # RT-001: LLVM integers previously only spilled to the native
    # activation, which disappears on real Fiber release. Copy the
    # pre-opt typed raw values and metadata into the Scratch Thread
    # BEFORE yield, validate source provenance and its detached
    # fingerprint in RT, and read the copy AFTER the Fiber is released.
    # Compiler liveness still reports unsupported pointers / arguments:
    # none of this authorizes arbitrary SSA/effect deoptimization.
    detached_cases = (
        ("root_nocall_simple", True, True, 1),
        ("root_nocall_nested", True, True, 1),
        ("root_afterloop_nested_if", False, True, 1),
        ("nested_repeat", False, False, 1),
        ("root_nocall_two_hats", True, True, 2),
    )
    for case, root_producer, strict_root_loop, hats in detached_cases:
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        common = dict(
            root_only_producer=root_producer,
            only_root_after_loop=strict_root_loop,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True,
            check_detached_llvm_ssa=True,
            trace_detached_llvm_ssa=True,
            gc_after_adopt=True,
        )
        actual, trace = execute(native, source, "eager", 1, **common)
        sidecars = re.findall(
            r"\[native-llvm-ssa-detached\] script=([^ ]+) site=(\d+) "
            r"pc=(\d+) words=(\d+) unsupported=(\d+) "
            r"complete=(\d+) checked=1", trace,
        )
        retained = re.findall(
            r"\[native-llvm-ssa-post-release\] script=([^ ]+) "
            r"words=(\d+) intact=1", trace,
        )
        if (actual != expected or trace.count("adopted=1") != hats or
            trace.count("[native-gc-after-adopt]") != hats or
            len(sidecars) != hats or len(retained) != hats or
            len({row[0] for row in sidecars}) != hats or
            {row[0] for row in sidecars} !=
                {row[0] for row in retained} or
            any(int(pc) == 0 or int(count) == 0 or
                int(unsupported) == 0 or int(complete) != 0
                for _, _, pc, count, unsupported, complete in sidecars)):
            raise AssertionError(
                f"{case}: detached LLVM SSA was not retained safely "
                f"across Fiber release: {trace[-1800:]}"
            )
        passed_runs += 1
        print(f"PASS {case}: {hats} detached LLVM SSA snapshot(s) "
              "verified after Fiber release + forced GC", flush=True)

        # Independent negative runs: make the host-owned bytes invalid,
        # omit capture altogether, and demand proof of full numerical
        # SSA coverage (unavailable for these real LLVM checkpoints).
        for mode in ("value", "source", "absent", "require_complete"):
            changed = dict(common)
            changed["trace_detached_llvm_ssa"] = False
            changed["corrupt_detached_llvm_ssa"] = (
                mode if mode in ("value", "source") else ""
            )
            changed["detached_llvm_ssa"] = mode != "absent"
            changed["require_complete_llvm_ssa"] = (
                mode == "require_complete"
            )
            refused, log = execute(
                native, source, "eager", 1, **changed,
            )
            if (refused != expected or
                "adopted=0" not in log or "adopted=1" in log or
                "[native-gc-after-adopt]" in log):
                raise AssertionError(
                    f"{case}: invalid detached LLVM SSA {mode} adopted: "
                    f"{log[-1800:]}"
                )
            passed_runs += 1
        print(f"PASS {case}: corrupt/missing/incomplete LLVM SSA "
              "fail-closed (4 modes)", flush=True)

    # Capture cannot be silently invented by the host when the
    # compiler-side LLVM SSA analyzer was disabled at codegen time.
    source = fixture(args.out, "root_nocall_simple")
    expected, _ = execute(native, source, "off", None)
    refused, log = execute(
        native, source, "eager", 1,
        root_only_producer=True, detached_llvm_ssa=True,
        check_detached_llvm_ssa=True, gc_after_adopt=True,
    )
    if refused != expected or "adopted=0" not in log or (
        "adopted=1" in log or "[native-gc-after-adopt]" in log
    ):
        raise AssertionError(
            "root_nocall_simple: missing compiler LLVM SSA analysis "
            "was accepted"
        )
    passed_runs += 1
    print("PASS root_nocall_simple: compiler SSA analyzer absent refused",
          flush=True)

    # Existing late root Nth-yield accounting must work with these
    # detached values. Corrupting host-owned bytes at N=17 refuses
    # independently of the already-proven lexical CallFrame counters.
    source = fixture(args.out, "root_nocall_nested")
    expected, _ = execute(native, source, "off", None)
    for corrupted in (False, True):
        actual, trace = execute(
            native, source, "eager", 17,
            root_only_producer=True, trace_root_yield_count=True,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True, check_detached_llvm_ssa=True,
            trace_detached_llvm_ssa=True,
            corrupt_detached_llvm_ssa="value" if corrupted else "",
            gc_after_adopt=True,
        )
        if (actual != expected or
            trace.count("adopted=1") != int(not corrupted) or
            trace.count("[native-gc-after-adopt]") !=
                int(not corrupted) or
            not re.search(
                r"\[native-root-yield\].*count=17 target=17", trace
            ) or (corrupted and "adopted=0" not in trace)):
            raise AssertionError(
                f"root_nocall_nested: detached SSA N=17 "
                f"corrupted={corrupted}: {trace[-1800:]}"
            )
        passed_runs += 1
    print("PASS root_nocall_nested: N=17 detached numeric SSA parity",
          flush=True)

    # RT-001 typed SSA expansion: native double/float intermediate
    # results and integer native procedure arguments now join the raw
    # numeric snapshot. Pointer values and other unrepresentable types
    # stay OUT of the copied words and retain explicit unsupported
    # counts. The detached image is still non-executable diagnostic.
    for case, needs_root, strict_root, hats, needs_arg in (
        ("root_nocall_simple", True, True, 1, False),
        ("root_nocall_nested", True, True, 1, False),
        ("root_nocall_two_hats", True, True, 2, False),
        ("root_afterloop_nested_if", False, True, 1, False),
        ("nested_repeat", False, False, 1, True),
        ("heap_string", False, False, 1, True),
        ("argument_snapshot", False, False, 1, True),
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        result, trace = execute(
            native, source, "eager", 1,
            root_only_producer=needs_root,
            only_root_after_loop=strict_root,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            check_detached_llvm_ssa=True,
            trace_detached_llvm_ssa=True,
            gc_after_adopt=True,
        )
        typed = re.findall(
            r"\[native-llvm-ssa-detached\] script=([^ ]+) site=(\d+) "
            r"pc=(\d+) words=(\d+) unsupported=(\d+) "
            r"complete=(\d+) checked=1 "
            r"floats=(\d+) intargs=(\d+) floatargs=(\d+) "
            r"pointers=(\d+) other=(\d+)", trace,
        )
        if (result != expected or len(typed) != hats or
            trace.count("adopted=1") != hats or
            trace.count("[native-gc-after-adopt]") != hats or
            trace.count("[native-llvm-ssa-post-release]") != hats or
            len({row[0] for row in typed}) != hats or
            "[rt001-ssa-extra] unsupported-mismatch" in trace or
            any(int(row[6]) <= 0 or int(row[9]) <= 0 or
                int(row[4]) != int(row[9]) + int(row[10]) or
                int(row[5]) != 0 or
                (needs_arg and int(row[7]) <= 0)
                for row in typed)):
            raise AssertionError(
                f"{case}: typed FP/argument/pointer SSA metadata "
                f"and real GC handoff mismatch: {trace[-2000:]}"
            )
        passed_runs += 1
        print(f"PASS {case}: {hats} typed LLVM FP/argument captures, "
              "pointer counts explicit, copied across Fiber GC",
              flush=True)

    for case, root in (
        ("root_nocall_simple", True),
        ("argument_snapshot", False),
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        for corruption in ("kind", "unresolved"):
            result, trace = execute(
                native, source, "eager", 1,
                root_only_producer=root,
                only_root_after_loop=root,
                llvm_ssa_liveness=True, detached_llvm_ssa=True,
                check_detached_llvm_ssa=True,
                corrupt_detached_llvm_ssa=corruption,
                gc_after_adopt=True,
            )
            if (result != expected or
                "adopted=0" not in trace or "adopted=1" in trace or
                "[native-gc-after-adopt]" in trace):
                raise AssertionError(
                    f"{case}: forged {corruption} typed SSA metadata "
                    f"was adopted: {trace[-1800:]}"
                )
            passed_runs += 1
        print(f"PASS {case}: forged SSA kind/pointer counts refused",
              flush=True)

    # RT-001 conservative pointer-origin census: the compiler counts
    # source-live LLVM pointers by their immediate origin, but NEVER
    # serializes native pointer addresses/GC references. The fixed
    # seven buckets must sum to unsupported pointers, survive Fiber
    # release as host metadata and remain source-bound.
    for case, root, strict, hats in (
        ("root_nocall_simple", True, True, 1),
        ("root_nocall_nested", True, True, 1),
        ("root_nocall_two_hats", True, True, 2),
        ("nested_repeat", False, False, 1),
        ("heap_string", False, False, 1),
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        result, trace = execute(
            native, source, "eager", 1,
            root_only_producer=root, only_root_after_loop=strict,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            check_detached_llvm_ssa=True, trace_detached_llvm_ssa=True,
            gc_after_adopt=True,
        )
        counts = re.findall(
            r"\[native-llvm-ssa-detached\] script=([^ ]+) .*"
            r"pointers=(\d+) other=(\d+) "
            r"ptr_origins=(\d+),(\d+),(\d+),(\d+),"
            r"(\d+),(\d+),(\d+)", trace,
        )
        if (
            result != expected or len(counts) != hats or
            len({values[0] for values in counts}) != hats or
            trace.count("adopted=1") != hats or
            trace.count("[native-gc-after-adopt]") != hats or
            trace.count("[native-llvm-ssa-post-release]") != hats or
            any(
                sum(int(x) for x in vals[3:]) != int(vals[1]) or
                int(vals[1]) == 0 for vals in counts
            )
        ):
            raise AssertionError(
                f"{case}: pointer-origin distribution was not "
                f"preserved by native Fiber suspension: {trace[-1750:]}"
            )
        passed_runs += 1
        print(f"PASS {case}: {hats} source-bound pointer origin "
              "histogram(s), no raw pointers copied", flush=True)

    for case, root in (
        ("root_nocall_simple", True),
        ("heap_string", False),
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        for mode in ("origin", "origin-swap"):
            result, trace = execute(
                native, source, "eager", 1,
                root_only_producer=root,
                only_root_after_loop=root,
                llvm_ssa_liveness=True,
                detached_llvm_ssa=True,
                check_detached_llvm_ssa=True,
                corrupt_detached_llvm_ssa=mode,
                gc_after_adopt=True,
            )
            if (
                result != expected or "adopted=0" not in trace or
                "adopted=1" in trace or
                "[native-gc-after-adopt]" in trace
            ):
                raise AssertionError(
                    f"{case}: forged pointer origin {mode} "
                    f"was adopted: {trace[-1750:]}"
                )
            passed_runs += 1
        print(f"PASS {case}: pointer histogram sum and sum-preserving "
              "forgery refused", flush=True)

    source = fixture(args.out, "root_nocall_nested")
    expected, _ = execute(native, source, "off", None)
    for mode in ("intact", "origin-swap"):
        result, trace = execute(
            native, source, "eager", 17,
            root_only_producer=True,
            trace_root_yield_count=True,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            check_detached_llvm_ssa=True,
            corrupt_detached_llvm_ssa=(
                "origin-swap" if mode != "intact" else ""),
            gc_after_adopt=True,
        )
        expected_adopt = int(mode == "intact")
        if (
            result != expected or
            trace.count("adopted=1") != expected_adopt or
            trace.count("[native-gc-after-adopt]") != expected_adopt or
            not re.search(
                r"\[native-root-yield\].*count=17 target=17", trace
            ) or (mode != "intact" and "adopted=0" not in trace)
        ):
            raise AssertionError(
                f"root_nocall_nested N=17 pointer histogram "
                f"{mode} failed: {trace[-1750:]}"
            )
        passed_runs += 1
    print("PASS root_nocall_nested: pointer histogram survives N=17 "
          "and corruption refuses transfer", flush=True)

    # RT-001: source-bound static stack-ALLoca lineage. Compiler-live
    # pointers with an exact local alloca root are frame-local and
    # become INVALID on native Fiber release; they are NEVER restored
    # or used as GC roots. Unknown/mixed roots remain unsupported.
    # The host receives only stack-derived counts and unique root count.
    for case, root, strict, hats in (
        ("root_nocall_simple", True, True, 1),
        ("root_nocall_nested", True, True, 1),
        ("root_nocall_two_hats", True, True, 2),
        ("nested_repeat", False, False, 1),
        ("heap_string", False, False, 1),
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 1,
            root_only_producer=root, only_root_after_loop=strict,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            check_detached_llvm_ssa=True,
            trace_detached_llvm_ssa=True, gc_after_adopt=True,
        )
        lineage = re.findall(
            r"\[native-llvm-ssa-detached\] script=([^ ]+) .*"
            r"pointers=(\d+) other=(\d+) "
            r"ptr_origins=(\d+),(\d+),(\d+),(\d+),"
            r"(\d+),(\d+),(\d+) "
            r"stack_derived=(\d+) stack_roots=(\d+)", trace,
        )
        if (
            actual != expected or len(lineage) != hats or
            len({record[0] for record in lineage}) != hats or
            trace.count("adopted=1") != hats or
            trace.count("[native-gc-after-adopt]") != hats or
            trace.count("[native-llvm-ssa-post-release]") != hats or
            any(
                sum(int(v) for v in record[3:10]) != int(record[1]) or
                not 0 < int(record[11]) <= int(record[10]) <=
                    int(record[1]) or
                int(record[10]) < int(record[4])
                for record in lineage
            )
        ):
            raise AssertionError(
                f"{case}: native stack root lineage failed to survive "
                f"Fiber release safely: {trace[-1800:]}"
            )
        passed_runs += 1
        print(f"PASS {case}: native stack lineage counts "
              "survive release; no pointer addresses reused", flush=True)

    for case, root in (
        ("root_nocall_simple", True),
        ("heap_string", False),
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        for fault in ("stack-count", "stack-roots"):
            actual, trace = execute(
                native, source, "eager", 1,
                root_only_producer=root,
                only_root_after_loop=root,
                llvm_ssa_liveness=True, detached_llvm_ssa=True,
                check_detached_llvm_ssa=True,
                corrupt_detached_llvm_ssa=fault,
                gc_after_adopt=True,
            )
            if (
                actual != expected or "adopted=0" not in trace or
                "adopted=1" in trace or
                "[native-gc-after-adopt]" in trace
            ):
                raise AssertionError(
                    f"{case}: forged stack lineage {fault} "
                    f"was adopted: {trace[-1800:]}"
                )
            passed_runs += 1
        print(f"PASS {case}: forged native stack/alloca counts refused",
              flush=True)

    source = fixture(args.out, "root_nocall_nested")
    expected, _ = execute(native, source, "off", None)
    for mode in ("intact", "stack-count"):
        actual, trace = execute(
            native, source, "eager", 17,
            root_only_producer=True,
            trace_root_yield_count=True,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            check_detached_llvm_ssa=True,
            corrupt_detached_llvm_ssa=(
                "stack-count" if mode != "intact" else ""),
            gc_after_adopt=True,
        )
        allowed = int(mode == "intact")
        if (
            actual != expected or trace.count("adopted=1") != allowed or
            trace.count("[native-gc-after-adopt]") != allowed or
            not re.search(
                r"\[native-root-yield\].*count=17 target=17", trace
            ) or (mode != "intact" and "adopted=0" not in trace)
        ):
            raise AssertionError(
                f"root_nocall_nested N=17 stack-lineage "
                f"{mode} failed: {trace[-1750:]}"
            )
        passed_runs += 1
    print("PASS root_nocall_nested: 17th native source latch stack "
          "lineage consistent and forged count refused", flush=True)

    # A stale source cannot borrow otherwise valid stack-lineage facts
    # from the same native checkpoint: the original Fiber must continue.
    for mode in ("revision", "digest", "real_cache_reset"):
        source = fixture(args.out, "root_nocall_nested")
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 17,
            root_only_producer=True,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            check_detached_llvm_ssa=True,
            bump_source_revision=mode in ("revision", "real_cache_reset"),
            edit_source_without_revision=mode == "digest",
            real_cache_reset=mode == "real_cache_reset",
            gc_after_adopt=True,
        )
        marker = (
            "[native-source-content-edit]" if mode == "digest"
            else "[native-real-cache-reset]" if mode == "real_cache_reset"
            else "[native-source-revision-bump]"
        )
        if (
            actual != expected or marker not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"root_nocall_nested: stale {mode} stack-lineage "
                f"borrowed: {trace[-1700:]}"
            )
        passed_runs += 1
    print("PASS root_nocall_nested: 3 stale source versions deny "
          "stack-lineage adoption", flush=True)

    # IR-001/RT-001 typed-manifest convergence. The compiler's original
    # typed SSA site descriptors and source identity are copied into a
    # separate manifest, NOT derived from the mutable detached SSA
    # diagnostic. IR's verifier must validate exact site/proc/source,
    # slots and raw type widths before native Fiber release, and the
    # deep-copied snapshot must survive forced GC after release.
    typed_cases = (
        ("root_nocall_simple", True, True, 1),
        ("root_nocall_nested", True, True, 1),
        ("root_nocall_two_hats", True, True, 2),
        ("root_afterloop_nested_if", False, True, 1),
        ("nested_repeat", False, False, 1),
        ("heap_string", False, False, 1),
    )
    for case, root, strict, hats in typed_cases:
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 1,
            root_only_producer=root,
            only_root_after_loop=strict,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True,
            require_typed_ssa_manifest=True,
            trace_detached_llvm_ssa=True,
            gc_after_adopt=True,
        )
        originals = re.findall(
            r"\[native-typed-ssa-manifest\] script=([^ ]+) "
            r"site=(\d+) pc=(\d+) words=(\d+) independent=1",
            trace,
        )
        released = re.findall(
            r"\[native-typed-ssa-post-release\] "
            r"script=([^ ]+) words=(\d+) intact=1", trace,
        )
        if (
            actual != expected or
            trace.count("adopted=1") != hats or
            trace.count("[native-gc-after-adopt]") != hats or
            len(originals) != hats or len(released) != hats or
            len({row[0] for row in originals}) != hats or
            {row[0] for row in originals} !=
                {row[0] for row in released} or
            any(int(site) == 0 or int(pc) == 0 or int(count) == 0
                for _, site, pc, count in originals)
        ):
            raise AssertionError(
                f"{case}: compiler-anchored IR-001 typed manifest "
                f"not verified after real Fiber release/GC: "
                f"{trace[-1750:]}"
            )
        passed_runs += 1
        print(f"PASS {case}: {hats} independently checked IR typed "
              "SSA manifests survive native Fiber release", flush=True)

    # A mutated runtime descriptor may retain a VALID width/tag and
    # its FNV integrity hash may be recomputed. The SEPARATE original
    # compiler manifest must still reject the forged kind/site.
    # Likewise a missing or independently corrupted manifest is not
    # silently inferred from the runtime copy.
    for case, root in (
        ("root_nocall_simple", True),
        ("heap_string", False),
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        for mode in (
            "site-rehash", "kind-rehash",
            "manifest-drop", "manifest-site", "manifest-kind"
        ):
            actual, trace = execute(
                native, source, "eager", 1,
                root_only_producer=root,
                only_root_after_loop=root,
                llvm_ssa_liveness=True,
                detached_llvm_ssa=True,
                require_typed_ssa_manifest=True,
                corrupt_detached_llvm_ssa=mode,
                gc_after_adopt=True,
            )
            if (
                actual != expected or
                "adopted=0" not in trace or
                "adopted=1" in trace or
                "[native-gc-after-adopt]" in trace
            ):
                raise AssertionError(
                    f"{case}: forged typed SSA manifest "
                    f"{mode} was adopted: {trace[-1600:]}"
                )
            passed_runs += 1
        print(f"PASS {case}: 5 type/site manifest forgeries "
              "fail closed", flush=True)

    # The producer and compiler's SSA analysis are both mandatory
    # when requesting this independent verification gate.
    source = fixture(args.out, "root_nocall_simple")
    expected, _ = execute(native, source, "off", None)
    for mode in ("missing-producer", "missing-analysis"):
        actual, trace = execute(
            native, source, "eager", 1,
            root_only_producer=True,
            llvm_ssa_liveness=mode != "missing-analysis",
            detached_llvm_ssa=mode != "missing-producer",
            require_typed_ssa_manifest=True,
            gc_after_adopt=True,
        )
        if (
            actual != expected or
            "adopted=0" not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"root_nocall_simple: typed manifest {mode} "
                f"was adopted: {trace[-1600:]}"
            )
        passed_runs += 1
    print("PASS root_nocall_simple: compiler analysis/producer "
          "absent fail closed", flush=True)

    # Late Nth-yield transitions, including a tampered descriptor
    # with recomputed integrity hash, must use the correct original
    # manifest for the ACTUAL suspended LLVM checkpoint.
    source = fixture(args.out, "root_nocall_nested")
    expected, _ = execute(native, source, "off", None)
    for mode in ("valid", "kind-rehash"):
        actual, trace = execute(
            native, source, "eager", 17,
            root_only_producer=True,
            trace_root_yield_count=True,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True,
            require_typed_ssa_manifest=True,
            corrupt_detached_llvm_ssa=(
                mode if mode != "valid" else ""),
            trace_detached_llvm_ssa=True,
            gc_after_adopt=True,
        )
        adopted = int(mode == "valid")
        if (
            actual != expected or trace.count("adopted=1") != adopted or
            trace.count("[native-gc-after-adopt]") != adopted or
            trace.count("[native-typed-ssa-post-release]") != adopted or
            not re.search(
                r"\[native-root-yield\].*count=17 target=17", trace
            ) or (mode != "valid" and "adopted=0" not in trace)
        ):
            raise AssertionError(
                f"root_nocall_nested N=17 typed manifest {mode} "
                f"failed: {trace[-1700:]}"
            )
        passed_runs += 1
    print("PASS root_nocall_nested: N=17 independent typed "
          "manifest verified/recomputed-hash forgery refused",
          flush=True)

    # A stale source must not borrow a legitimate compiled snapshot,
    # even when source-PC and typed descriptor shapes are unchanged.
    for mode in ("revision", "digest", "real_cache_reset"):
        source = fixture(args.out, "root_nocall_nested")
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 17,
            root_only_producer=True,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True,
            require_typed_ssa_manifest=True,
            bump_source_revision=mode in ("revision", "real_cache_reset"),
            edit_source_without_revision=mode == "digest",
            real_cache_reset=mode == "real_cache_reset",
            gc_after_adopt=True,
        )
        marker = (
            "[native-source-content-edit]" if mode == "digest"
            else "[native-real-cache-reset]" if mode == "real_cache_reset"
            else "[native-source-revision-bump]"
        )
        if (
            actual != expected or marker not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"root_nocall_nested: typed manifest stale {mode} "
                f"adopted: {trace[-1700:]}"
            )
        passed_runs += 1
    print("PASS root_nocall_nested: independent typed SSA source "
          "revision/digest/cache reset refused (3)", flush=True)

    # RT-001: JIT's native procedure ABI (JitCtx*, i64 ScratchValue...)
    # must match the *already GC-rooted* source CallFrame argument
    # sidecar exactly. This is ONLY a partial proof for observed live
    # argument SSA words; LLVM temporaries and pointer SSA stay unsafe.
    # Require the independently copied IR typed source manifest too.
    rooted_args_cases = (
        ("root_nocall_simple", True, True, 1, 0, 0),
        ("root_nocall_nested", True, True, 1, 0, 0),
        ("root_nocall_two_hats", True, True, 2, 0, 0),
        ("nested_repeat", False, False, 1, 1, 0),
        ("heap_string", False, False, 1, 1, 1),
        ("argument_snapshot", False, False, 1, 1, 0),
        ("nested_call_string", False, False, 1, 1, 1),
    )
    for case, root, strict, hats, matched, strings in rooted_args_cases:
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        result, trace = execute(
            native, source, "eager", 1,
            root_only_producer=root,
            only_root_after_loop=strict,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True,
            require_rooted_ssa_args=True,
            trace_detached_llvm_ssa=True,
            gc_after_adopt=True,
        )
        before = re.findall(
            r"\[native-ssa-rooted-args\] script=([^ ]+) "
            r"matched=(\d+) heap_strings=(\d+) "
            r"nonargument_words=(\d+) checked=1", trace,
        )
        after = re.findall(
            r"\[native-ssa-rooted-args-post-release\] script=([^ ]+) "
            r"matched=(\d+) heap_strings=(\d+) intact=1", trace,
        )
        if (
            result != expected or
            trace.count("adopted=1") != hats or
            trace.count("[native-gc-after-adopt]") != hats or
            len(before) != hats or len(after) != hats or
            len({row[0] for row in before}) != hats or
            {row[0] for row in before} !=
                {row[0] for row in after} or
            any(
                int(m) != matched or int(st) != strings or
                int(skipped) == 0
                for _, m, st, skipped in before
            ) or
            any(
                int(m) != matched or int(st) != strings
                for _, m, st in after
            )
        ):
            raise AssertionError(
                f"{case}: GC-rooted Scratch CallFrame argument SSA "
                f"not matched across native Fiber release/GC: "
                f"{trace[-1800:]}"
            )
        passed_runs += 1
        print(f"PASS {case}: {hats} rooted source CallFrame(s) "
              f"match {matched} native arg SSA(s), "
              f"{strings} rooted string(s)", flush=True)

    # A forged live argument word with its FNV fingerprint RECOMPUTED
    # satisfies the typed-descriptor manifest alone (raw values are
    # runtime data), but MUST fail against the independent GC-rooted
    # source owner. The original native machine execution continues.
    for case in (
        "nested_repeat", "heap_string",
        "argument_snapshot", "nested_call_string"
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 1,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            require_rooted_ssa_args=True,
            corrupt_detached_llvm_ssa="rooted-arg-rehash",
            gc_after_adopt=True,
        )
        if (
            actual != expected or "adopted=0" not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"{case}: forged live source argument escaped "
                f"rooted SSA validation: {trace[-1650:]}"
            )
        passed_runs += 1
        print(f"PASS {case}: forged i64 argument with repaired "
              "fingerprint refused", flush=True)

    # A control observation establishes what the earlier IR-only typed
    # manifest does NOT certify: word values are not compiler-known,
    # so a rehashed, incorrectly observed boxed string still passes
    # typed descriptor validation. With the rooted gate enabled,
    # the same fault cannot be adopted.
    source = fixture(args.out, "heap_string")
    expected, _ = execute(native, source, "off", None)
    for strict in (False, True):
        actual, trace = execute(
            native, source, "eager", 1,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            require_typed_ssa_manifest=not strict,
            require_rooted_ssa_args=strict,
            corrupt_detached_llvm_ssa="rooted-arg-rehash",
            gc_after_adopt=True,
        )
        if (
            actual != expected or
            trace.count("adopted=1") != int(not strict) or
            trace.count("[native-gc-after-adopt]") !=
                int(not strict) or
            (strict and "adopted=0" not in trace)
        ):
            raise AssertionError(
                f"heap_string: typed-only vs rooted owner "
                f"strict={strict}: {trace[-1550:]}"
            )
        passed_runs += 1
    print("PASS heap_string: typed-only misses changed Value bits; "
          "rooted owner rejects", flush=True)

    # Multiple actual nonroot native yields continue to observe the
    # same original rooted string, with no native SSA raw pointer
    # resurrected by GC and no late checkpoint borrow.
    for nth in (2, 3):
        source = fixture(args.out, "heap_string")
        expected, _ = execute(native, source, "off", None)
        for mode in ("valid", "rooted-arg-rehash"):
            actual, trace = execute(
                native, source, "eager", nth,
                llvm_ssa_liveness=True, detached_llvm_ssa=True,
                require_rooted_ssa_args=True,
                trace_detached_llvm_ssa=True,
                corrupt_detached_llvm_ssa=(
                    "" if mode == "valid" else mode),
                gc_after_adopt=True,
            )
            adopted = int(mode == "valid")
            if (
                actual != expected or
                trace.count("adopted=1") != adopted or
                trace.count("[native-gc-after-adopt]") != adopted or
                trace.count(
                    "[native-ssa-rooted-args-post-release]"
                ) != adopted or
                (mode != "valid" and "adopted=0" not in trace)
            ):
                raise AssertionError(
                    f"heap_string: native yield {nth} rooted "
                    f"args {mode}: {trace[-1700:]}"
                )
            passed_runs += 1
    print("PASS heap_string: N=2/3 rooted arguments, "
          "source-safe adopt/corrupt refusal", flush=True)

    # Missing compiler-side liveness or the native typed-SSA
    # producer can never be filled in from only GC-rooted source
    # values. This is still NOT generic executable deoptimization.
    source = fixture(args.out, "heap_string")
    expected, _ = execute(native, source, "off", None)
    for mode in ("no-producer", "no-analyzer"):
        actual, trace = execute(
            native, source, "eager", 1,
            llvm_ssa_liveness=mode != "no-analyzer",
            detached_llvm_ssa=mode != "no-producer",
            require_rooted_ssa_args=True,
            gc_after_adopt=True,
        )
        if (
            actual != expected or
            "adopted=0" not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"heap_string: {mode} missing SSA provenance "
                f"was adopted: {trace[-1700:]}"
            )
        passed_runs += 1
    print("PASS heap_string: missing LLVM producer/analyzer denied",
          flush=True)

    # Stale source invalidation must prevail even if every observed
    # native argument is bitwise identical to a still-rooted Value.
    for mode in ("revision", "digest", "real_cache_reset"):
        source = fixture(args.out, "heap_string")
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 2,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            require_rooted_ssa_args=True,
            bump_source_revision=mode in ("revision", "real_cache_reset"),
            edit_source_without_revision=mode == "digest",
            real_cache_reset=mode == "real_cache_reset",
            gc_after_adopt=True,
        )
        marker = (
            "[native-source-content-edit]" if mode == "digest"
            else "[native-real-cache-reset]" if mode == "real_cache_reset"
            else "[native-source-revision-bump]"
        )
        if (
            actual != expected or marker not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"heap_string: rooted ABI args borrowed stale "
                f"{mode} source: {trace[-1600:]}"
            )
        passed_runs += 1
    print("PASS heap_string: 3 stale source cases refused "
          "even with matching GC handles", flush=True)

    # RT-001: the compiler-only same-argument SSA ancestry map can
    # certify NONARG i64 PHI/SELECT results only when EVERY incoming
    # path preserves exactly the same rooted Scratch argument bits.
    # Roots remain owned by the original source CallFrame. Ordinary
    # i64 arithmetic, loads, strings not in that frame, etc stay unsafe.
    # In the current fixtures no nonargument SSA PHI/SELECT is proven:
    # positive/negative *real* checks cover original direct arg mapping
    # and the correctness of the new unchanged-root metadata pipeline;
    # synthetic LLVM23 smoke directly tests nonarg PHI/SELECT ancestry.
    for case, root, strict, hats in (
        ("root_nocall_simple", True, True, 1),
        ("root_nocall_nested", True, True, 1),
        ("root_nocall_two_hats", True, True, 2),
        ("nested_repeat", False, False, 1),
        ("heap_string", False, False, 1),
        ("nested_call_string", False, False, 1),
        ("argument_snapshot", False, False, 1),
        ("parent_ifelse_repeat_string", False, False, 1),
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 1,
            root_only_producer=root,
            only_root_after_loop=strict,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True,
            require_rooted_ssa_args=True,
            trace_detached_llvm_ssa=True,
            gc_after_adopt=True,
        )
        found = re.findall(
            r"\[native-ssa-rooted-args\] script=([^ ]+) "
            r"matched=(\d+) heap_strings=(\d+) "
            r"nonargument_words=(\d+) checked=1 "
            r"aliased=(\d+) aliased_strings=(\d+)", trace,
        )
        after = re.findall(
            r"\[native-ssa-rooted-args-post-release\] "
            r"script=([^ ]+) matched=(\d+) "
            r"heap_strings=(\d+) intact=1 "
            r"aliased=(\d+) aliased_strings=(\d+)", trace,
        )
        expected_args = int(not root)
        if (
            actual != expected or len(found) != hats or
            len(after) != hats or
            trace.count("adopted=1") != hats or
            trace.count("[native-gc-after-adopt]") != hats or
            len({row[0] for row in found}) != hats or
            {row[0] for row in found} !=
                {row[0] for row in after} or
            any(
                int(row[1]) != expected_args or
                int(row[4]) != 0 or int(row[5]) != 0
                for row in found
            )
        ):
            raise AssertionError(
                f"{case}: compiler SSA->rooted i64 arg mapping "
                f"not validated across Fiber release: {trace[-1800:]}"
            )
        passed_runs += 1
        print(f"PASS {case}: compiler SSA source alias mapping "
              f"intact across {hats} native Fiber(s), "
              "nonargument aliases currently 0", flush=True)

    # Forging a runtime slot->argument owner and RECOMPUTING the
    # detached FNV must NOT bypass the original read-only compiler
    # mapping, even when the typed SSA site/widths remain consistent.
    for case in (
        "nested_repeat", "heap_string", "nested_call_string",
        "argument_snapshot", "parent_ifelse_repeat_string",
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 1,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True,
            require_rooted_ssa_args=True,
            corrupt_detached_llvm_ssa="alias-map-rehash",
            gc_after_adopt=True,
        )
        if (
            actual != expected or "adopted=0" not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"{case}: forged alias-to-root source map with "
                f"recomputed hash adopted: {trace[-1700:]}"
            )
        passed_runs += 1
    print("PASS rooted SSA alias map: 5 rehashed compiler-root "
          "forgeries rejected", flush=True)

    source = fixture(args.out, "heap_string")
    expected, _ = execute(native, source, "off", None)
    for strict in (False, True):
        actual, trace = execute(
            native, source, "eager", 1,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            require_typed_ssa_manifest=not strict,
            require_rooted_ssa_args=strict,
            corrupt_detached_llvm_ssa="alias-map-rehash",
            gc_after_adopt=True,
        )
        if (
            actual != expected or
            trace.count("adopted=1") != int(not strict) or
            trace.count("[native-gc-after-adopt]") !=
                int(not strict) or
            (strict and "adopted=0" not in trace)
        ):
            raise AssertionError(
                f"heap_string: typed-only vs alias source "
                f"strict={strict}: {trace[-1550:]}"
            )
        passed_runs += 1
    print("PASS heap_string: typed manifest alone cannot detect "
          "forged alias map, rooted-source verifier rejects",
          flush=True)

    # Multi-yield source checkpoints and independent missing compiler
    # producer/analyzer refusal remain fail-closed.
    for nth in (2, 3):
        source = fixture(args.out, "heap_string")
        expected, _ = execute(native, source, "off", None)
        for mode in ("valid", "alias-map-rehash"):
            actual, trace = execute(
                native, source, "eager", nth,
                llvm_ssa_liveness=True,
                detached_llvm_ssa=True,
                require_rooted_ssa_args=True,
                corrupt_detached_llvm_ssa=(
                    mode if mode != "valid" else ""),
                gc_after_adopt=True,
            )
            should_adopt = int(mode == "valid")
            if (
                actual != expected or
                trace.count("adopted=1") != should_adopt or
                trace.count("[native-gc-after-adopt]") !=
                    should_adopt or
                (mode != "valid" and "adopted=0" not in trace)
            ):
                raise AssertionError(
                    f"heap_string: N={nth} same-argument source "
                    f"{mode} failed: {trace[-1600:]}"
                )
            passed_runs += 1
    print("PASS heap_string: N=2/3 rooted SSA ancestry map "
          "remains bound to actual native source checkpoint",
          flush=True)

    for mode in ("no-producer", "no-analyzer"):
        source = fixture(args.out, "heap_string")
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 1,
            llvm_ssa_liveness=mode != "no-analyzer",
            detached_llvm_ssa=mode != "no-producer",
            require_rooted_ssa_args=True,
            gc_after_adopt=True,
        )
        if (
            actual != expected or "adopted=0" not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"heap_string: missing SSA ancestry {mode} "
                f"was adopted: {trace[-1650:]}"
            )
        passed_runs += 1
    print("PASS heap_string: missing ancestry producer/analyzer "
          "refused (2)", flush=True)

    for mode in ("revision", "digest", "real_cache_reset"):
        source = fixture(args.out, "heap_string")
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 2,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            require_rooted_ssa_args=True,
            bump_source_revision=mode in ("revision", "real_cache_reset"),
            edit_source_without_revision=mode == "digest",
            real_cache_reset=mode == "real_cache_reset",
            gc_after_adopt=True,
        )
        marker = (
            "[native-source-content-edit]" if mode == "digest"
            else "[native-real-cache-reset]" if mode == "real_cache_reset"
            else "[native-source-revision-bump]"
        )
        if (
            actual != expected or marker not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"heap_string: stale compiled SSA ancestry "
                f"{mode} borrowed: {trace[-1650:]}"
            )
        passed_runs += 1
    print("PASS heap_string: 3 stale sources never authorize "
          "SSA alias ancestry", flush=True)

    # RT-001 real Scratch PHI from a GC-managed Unicode string.
    # BOTH branches store the SAME native i64 argument into a promoted
    # mixed-type Scratch variable. With lazy typed SSA enabled, the
    # code generator merges those identical arguments in an LLVM
    # i64 'typed.merge.bits' PHI and uses it in the following variable
    # reporter. The live PHI reaches a real AfterLoopBody checkpoint.
    # This closes the former synthetic-only nonargument GC-owner gap:
    # the original source CallFrame already roots the exact SStr Value,
    # and the compiler SSA alias proof ties the PHI to that same owner.
    source = fixture(args.out, "ssa_alias_branch_string")
    expected, _ = execute(native, source, "off", None)
    for nth in (1, 2, 5, 17):
        actual, trace = execute(
            native, source, "eager", nth,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True,
            require_rooted_ssa_args=True,
            lazy_typed_ssa=True,
            trace_detached_llvm_ssa=True,
            gc_after_adopt=True,
        )
        before = re.findall(
            r"\[native-ssa-rooted-args\] script=([^ ]+) "
            r"matched=(\d+) heap_strings=(\d+) "
            r"nonargument_words=(\d+) checked=1 "
            r"aliased=(\d+) aliased_strings=(\d+)", trace,
        )
        after = re.findall(
            r"\[native-ssa-rooted-args-post-release\] "
            r"script=([^ ]+) matched=(\d+) "
            r"heap_strings=(\d+) intact=1 "
            r"aliased=(\d+) aliased_strings=(\d+)", trace,
        )
        if (
            actual != expected or trace.count("adopted=1") != 1 or
            trace.count("[native-gc-after-adopt]") != 1 or
            len(before) != 1 or len(after) != 1 or
            [int(before[0][i]) for i in (1, 2, 4, 5)] !=
                [1, 1, 1, 1] or
            [int(after[0][i]) for i in (1, 2, 3, 4)] !=
                [1, 1, 1, 1] or
            not re.search(r"\[rt001-ssa-extra\].*aliasable=1", trace)
        ):
            raise AssertionError(
                f"ssa_alias_branch_string: n={nth} real live "
                f"PHI SSA->GC-root owner proof not observed: "
                f"{trace[-1750:]}"
            )
        passed_runs += 1
    print("PASS ssa_alias_branch_string: 4 real LLVM i64 PHI "
          "GC-rooted Unicode string handoffs (N=1/2/5/17)",
          flush=True)

    # Changing the PHI's *runtime value* is not a source owner
    # proof, even if the typed SSA descriptors and recomputed FNV
    # stay valid. Likewise its original compiler index must match.
    for mode in (
        "alias-word-rehash", "alias-map-rehash",
        "alias-both-drop", "rooted-arg-rehash"
    ):
        actual, trace = execute(
            native, source, "eager", 1,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True,
            require_rooted_ssa_args=True,
            lazy_typed_ssa=True,
            corrupt_detached_llvm_ssa=mode,
            gc_after_adopt=True,
        )
        if (
            actual != expected or "adopted=0" not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"ssa_alias_branch_string: forged live PHI owner "
                f"{mode} adopted: {trace[-1700:]}"
            )
        passed_runs += 1
    print("PASS ssa_alias_branch_string: 4 forged PHI word/map/"
          "missing-map/direct-arg proofs refused", flush=True)

    # The compiler-only type manifest cannot establish the runtime
    # ownership of a PHI word. A negative control proves that the new
    # extra source owner check, NOT the shape-only typed manifest,
    # provides the veto. The diagnostic bit flip never gets executed.
    for mode in ("alias-word-rehash", "alias-map-rehash"):
        actual, trace = execute(
            native, source, "eager", 1,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True,
            require_typed_ssa_manifest=True,
            lazy_typed_ssa=True,
            corrupt_detached_llvm_ssa=mode,
            gc_after_adopt=True,
        )
        if (
            actual != expected or trace.count("adopted=1") != 1 or
            trace.count("[native-gc-after-adopt]") != 1
        ):
            raise AssertionError(
                f"ssa_alias_branch_string: typed-only control for "
                f"{mode} unexpectedly refused: {trace[-1600:]}"
            )
        passed_runs += 1
    print("PASS ssa_alias_branch_string: 2 type-manifest-only "
          "controls show the missing runtime SSA owner proof",
          flush=True)

    # The same original Scratch project without the experimental
    # lazy mixed-type SSA has no PHI; do not claim a fabricated
    # nonargument GC owner.
    actual, trace = execute(
        native, source, "eager", 1,
        llvm_ssa_liveness=True,
        detached_llvm_ssa=True,
        require_rooted_ssa_args=True,
        lazy_typed_ssa=False,
        trace_detached_llvm_ssa=True,
        gc_after_adopt=True,
    )
    if (
        actual != expected or trace.count("adopted=1") != 1 or
        not re.search(
            r"\[native-ssa-rooted-args\].*"
            r"aliased=0 aliased_strings=0", trace
        ) or
        not re.search(r"\[rt001-ssa-extra\].*aliasable=0", trace)
    ):
        raise AssertionError(
            f"ssa_alias_branch_string: lazy-SSA-disabled control "
            f"misclassified PHI: {trace[-1700:]}"
        )
    passed_runs += 1
    print("PASS ssa_alias_branch_string: lazy typed SSA OFF "
          "does not invent a PHI owner", flush=True)

    for mode in ("no-producer", "no-analyzer"):
        actual, trace = execute(
            native, source, "eager", 1,
            llvm_ssa_liveness=mode != "no-analyzer",
            detached_llvm_ssa=mode != "no-producer",
            require_rooted_ssa_args=True,
            lazy_typed_ssa=True,
            gc_after_adopt=True,
        )
        if (
            actual != expected or "adopted=0" not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"ssa_alias_branch_string: missing {mode} "
                f"was adopted: {trace[-1700:]}"
            )
        passed_runs += 1

    # No old compiled PHI/CallFrame owner may be reused after a
    # revision bump, unchanged-revision content digest edit, or the
    # real production resetCache + full GC path.
    for mode in ("revision", "digest", "real_cache_reset"):
        actual, trace = execute(
            native, source, "eager", 2,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True,
            require_rooted_ssa_args=True,
            lazy_typed_ssa=True,
            bump_source_revision=mode in ("revision", "real_cache_reset"),
            edit_source_without_revision=mode == "digest",
            real_cache_reset=mode == "real_cache_reset",
            gc_after_adopt=True,
        )
        marker = (
            "[native-source-content-edit]" if mode == "digest"
            else "[native-real-cache-reset]" if mode == "real_cache_reset"
            else "[native-source-revision-bump]"
        )
        if (
            actual != expected or marker not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"ssa_alias_branch_string: stale {mode} "
                f"PHI GC-root owner adopted: {trace[-1700:]}"
            )
        passed_runs += 1
    print("PASS ssa_alias_branch_string: missing producer/analysis "
          "and 3 stale source GC-owner checks refused", flush=True)

    # RT-001: REAL JIT-created GC string, *not* source argument-alias
    # ancestry. Two independent operator_join helpers return boxed
    # heap SStr Values; lazy typed SSA merges both results into a
    # genuine LLVM i64 PHI used after IF/ELSE. Only an independent
    # exact GcValueHandles registration BEFORE Fiber retirement can
    # keep this particular new native SSA value protected afterward.
    source = fixture(args.out, "ssa_new_join_branch_string")
    expected, _ = execute(native, source, "off", None)
    for nth in (1, 2, 5, 17):
        actual, trace = execute(
            native, source, "eager", nth,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True,
            lazy_typed_ssa=True,
            require_generated_ssa_roots=True,
            gc_during_ssa_transfer=True,
            gc_after_adopt=True,
            trace_detached_llvm_ssa=True,
        )
        before = re.findall(
            r"\[native-ssa-generated-roots\] script=([^ ]+) "
            r"certified=(\d+) rooted=(\d+) immediate=(\d+) "
            r"checked=1", trace,
        )
        after = re.findall(
            r"\[native-ssa-generated-post-release\] "
            r"script=([^ ]+) certified=(\d+) rooted=(\d+) "
            r"immediate=(\d+) intact=1", trace,
        )
        if (
            actual != expected or trace.count("adopted=1") != 1 or
            trace.count("[native-gc-after-adopt]") != 1 or
            trace.count("[native-ssa-generated-gc]") != 1 or
            len(before) != 1 or len(after) != 1 or
            [int(v) for v in before[0][1:]] != [1, 1, 0] or
            [int(v) for v in after[0][1:]] != [1, 1, 0] or
            not re.search(
                r"\[rt001-ssa-extra\].*aliasable=0 generated=1",
                trace
            )
        ):
            raise AssertionError(
                f"ssa_new_join_branch_string: n={nth} native-created "
                f"PHI SStr exact GC root failed: {trace[-2000:]}"
            )
        passed_runs += 1
    print("PASS ssa_new_join_branch_string: 4 real JIT-created "
          "SStr roots across actual Fiber release + 2 full GCs",
          flush=True)

    for mode in (
        "generated-word-rehash", "generated-mask-rehash",
        "generated-both-drop", "generated-root-drop",
    ):
        actual, trace = execute(
            native, source, "eager", 1,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=True,
            corrupt_detached_llvm_ssa=mode,
            gc_after_adopt=True,
        )
        if (
            actual != expected or "adopted=0" not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"ssa_new_join_branch_string: forged native "
                f"GC-provenance {mode} adopted: {trace[-1800:]}"
            )
        passed_runs += 1
    print("PASS ssa_new_join_branch_string: 4 forged i64 "
          "SSA/provenance/GC handle defects fail closed", flush=True)

    # The typed manifest proves shape and source, not the identity
    # or retention of a newly produced native GC string. These
    # independent negative controls demonstrate the missing proof.
    for mode in ("generated-word-rehash", "generated-mask-rehash"):
        actual, trace = execute(
            native, source, "eager", 1,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True,
            require_typed_ssa_manifest=True,
            corrupt_detached_llvm_ssa=mode,
            gc_after_adopt=True,
        )
        if (
            actual != expected or trace.count("adopted=1") != 1 or
            trace.count("[native-gc-after-adopt]") != 1
        ):
            raise AssertionError(
                f"ssa_new_join_branch_string: typed-only negative "
                f"control {mode} unexpectedly rejected: {trace[-1800:]}"
            )
        passed_runs += 1
    print("PASS ssa_new_join_branch_string: typed manifest alone "
          "does not prove generated string GC ownership (2)", flush=True)

    actual, trace = execute(
        native, source, "eager", 1,
        llvm_ssa_liveness=True, detached_llvm_ssa=True,
        lazy_typed_ssa=False,
        require_generated_ssa_roots=True,
        gc_during_ssa_transfer=True, gc_after_adopt=True,
        trace_detached_llvm_ssa=True,
    )
    if (
        actual != expected or trace.count("adopted=1") != 1 or
        not re.search(
            r"\[native-ssa-generated-roots\].*certified=0 "
            r"rooted=0 immediate=0 checked=1", trace
        ) or
        not re.search(
            r"\[rt001-ssa-extra\].*generated=0", trace
        )
    ):
        raise AssertionError(
            f"ssa_new_join_branch_string: lazy OFF falsely "
            f"roots a generated SSA value: {trace[-1800:]}"
        )
    passed_runs += 1
    print("PASS ssa_new_join_branch_string: no generated PHI "
          "with lazy SSA disabled", flush=True)

    for mode in ("no-producer", "no-analyzer"):
        actual, trace = execute(
            native, source, "eager", 1,
            llvm_ssa_liveness=mode != "no-analyzer",
            detached_llvm_ssa=mode != "no-producer",
            lazy_typed_ssa=True,
            require_generated_ssa_roots=True,
            gc_after_adopt=True,
        )
        if (
            actual != expected or "adopted=0" not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"ssa_new_join_branch_string: missing {mode} "
                f"adopted without GC certificate: {trace[-1800:]}"
            )
        passed_runs += 1

    for mode in ("revision", "digest", "real_cache_reset"):
        actual, trace = execute(
            native, source, "eager", 2,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=True,
            bump_source_revision=mode in ("revision", "real_cache_reset"),
            edit_source_without_revision=mode == "digest",
            real_cache_reset=mode == "real_cache_reset",
            gc_after_adopt=True,
        )
        marker = (
            "[native-source-content-edit]" if mode == "digest"
            else "[native-real-cache-reset]" if mode == "real_cache_reset"
            else "[native-source-revision-bump]"
        )
        if (
            actual != expected or marker not in trace or
            "adopted=1" in trace or
            "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"ssa_new_join_branch_string: stale source "
                f"{mode} adopted: {trace[-1800:]}"
            )
        passed_runs += 1
    print("PASS ssa_new_join_branch_string: 2 absent producer/"
          "analyzer + 3 stale source owner guards", flush=True)

    # An ordinary old GC-rooted argument and a source-owned argument
    # PHI must NOT be misclassified as a fresh join-created GC root.
    for control in ("heap_string", "ssa_alias_branch_string"):
        project = fixture(args.out, control)
        reference, _ = execute(native, project, "off", None)
        actual, trace = execute(
            native, project, "eager", 1,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=control == "ssa_alias_branch_string",
            require_generated_ssa_roots=True,
            gc_during_ssa_transfer=True, gc_after_adopt=True,
            trace_detached_llvm_ssa=True,
        )
        if (
            actual != reference or trace.count("adopted=1") != 1 or
            trace.count("[native-ssa-generated-gc]") != 1 or
            not re.search(
                r"\[native-ssa-generated-roots\].*certified=0 "
                r"rooted=0 immediate=0 checked=1", trace
            )
        ):
            raise AssertionError(
                f"{control}: original argument was misclassified "
                f"as generated SStr: {trace[-1800:]}"
            )
        passed_runs += 1
    print("PASS native SSA generated-root scope: 2 original "
          "CallFrame string cases remain unclassified", flush=True)

    # RT-001: when generated-string source rooting is opted in, the
    # native Fiber may be holding a newly created SStr only in LLVM SSA
    # registers BEFORE its next source-certified yield. Automatic GC
    # during host string allocation must therefore be deferred until
    # that fiber returns to the scheduler (where exact source snapshot
    # handles are already installed). Forced/manual full collection
    # AFTER Fiber release remains allowed and is checked separately.
    source = fixture(args.out, "ssa_new_join_branch_string")
    expected, _ = execute(native, source, "off", None,
                          gc_threshold_mb=1)
    for nth in (1, 2, 5, 17):
        actual, trace = execute(
            native, source, "eager", nth,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=True,
            gc_during_ssa_transfer=True, gc_after_adopt=True,
            trace_native_gc_deferral=True,
            trace_detached_llvm_ssa=True, gc_threshold_mb=1,
        )
        logs = re.findall(
            r"\[native-gc-deferred\] script=([^ ]+) "
            r"before=(\d+) after=(\d+) unchanged=(\d+)", trace
        )
        if (
            actual != expected or
            trace.count("adopted=1") != 1 or
            trace.count("[native-gc-after-adopt]") != 1 or
            trace.count("[native-ssa-generated-gc]") != 1 or
            not re.search(
                r"\[native-ssa-generated-post-release\].*"
                r"certified=1 rooted=1 immediate=0 intact=1", trace
            ) or not logs or
            any(
                script != logs[0][0] or before != after or stable != "1"
                for script, before, after, stable in logs
            )
        ):
            raise AssertionError(
                f"ssa_new_join_branch_string: native Fiber auto-GC "
                f"not deferred at N={nth}: {trace[-1700:]}"
            )
        passed_runs += 1
    print("PASS ssa_new_join_branch_string: 4 low-1MiB "
          "threshold tests defer automatic native GC but keep explicit "
          "post-Fiber fullGC and source parity", flush=True)

    # Opt-in ownership is the strict boundary: with the producer gate
    # disabled the original LLVM execution should be unaffected, and
    # the native GC deferral trace must NOT appear.
    source = fixture(args.out, "heap_string")
    expected, _ = execute(native, source, "off", None)
    actual, trace = execute(
        native, source, "eager", 1,
        llvm_ssa_liveness=True, detached_llvm_ssa=True,
        require_generated_ssa_roots=False,
        trace_native_gc_deferral=True, gc_threshold_mb=1,
        gc_after_adopt=True,
    )
    if (
        actual != expected or
        trace.count("adopted=1") != 1 or
        "[native-gc-deferred]" in trace
    ):
        raise AssertionError(
            f"heap_string: native GC should not be deferred when "
            f"generated source rooting OFF: {trace[-1600:]}"
        )
    passed_runs += 1
    print("PASS heap_string: default native execution has no "
          "automatic GC deferral", flush=True)

    # RT-001: make an ACTUAL direct Gc::collect() call from the
    # native Fiber BEFORE the generated-string SSA snapshot has any
    # GC roots. A one-shot host callback injected at the authentic
    # boxed-PHI checkpoint must defer the explicit request without
    # sweeping, then allow the already-owned post-Fiber full GC.
    source = fixture(args.out, "ssa_new_join_branch_string")
    expected, _ = execute(native, source, "off", None)
    for nth in (1, 2, 5, 17):
        actual, trace = execute(
            native, source, "eager", nth,
            llvm_ssa_liveness=True,
            detached_llvm_ssa=True,
            lazy_typed_ssa=True,
            require_generated_ssa_roots=True,
            inject_explicit_gc_before_ssa_root=True,
            trace_native_gc_deferral=True,
            trace_detached_llvm_ssa=True,
            gc_during_ssa_transfer=True,
            gc_after_adopt=True,
            gc_threshold_mb=1,
        )
        injection = re.findall(
            r"\[native-ssa-explicit-gc-guard\] before=(\d+) "
            r"after=(\d+) unchanged=(\d+) pending=(\d+)", trace
        )
        if (
            actual != expected or
            trace.count("adopted=1") != 1 or
            len(injection) != 1 or
            injection[0][0] != injection[0][1] or
            injection[0][2:] != ("1", "1") or
            trace.count("[native-ssa-generated-gc]") != 1 or
            trace.count("[native-gc-after-adopt]") != 1 or
            not re.search(
                r"\[native-ssa-generated-post-release\].*"
                r"certified=1 rooted=1 immediate=0 intact=1", trace
            )
        ):
            raise AssertionError(
                f"ssa_new_join_branch_string: actual explicit "
                f"GC from active native Fiber before SSA rooting "
                f"was not deferred at N={nth}: {trace[-2100:]}"
            )
        passed_runs += 1
    print("PASS ssa_new_join_branch_string: explicit full GC "
          "requested by real native Fiber before roots, deferred "
          "at N=1/2/5/17; post-Fiber actual GC and parity PASS",
          flush=True)

    # This deliberately dangerous fault injection MUST be inert when
    # generated-root ownership is not opted in. Default behavior and
    # the legacy typed-manifest-only diagnostic are unaffected.
    source = fixture(args.out, "heap_string")
    expected, _ = execute(native, source, "off", None)
    actual, trace = execute(
        native, source, "eager", 1,
        llvm_ssa_liveness=True,
        detached_llvm_ssa=True,
        require_generated_ssa_roots=False,
        inject_explicit_gc_before_ssa_root=True,
        gc_after_adopt=True,
    )
    if (
        actual != expected or "adopted=1" not in trace or
        "[native-ssa-explicit-gc-guard]" in trace
    ):
        raise AssertionError(
            f"heap_string: generated SSA direct-GC injection "
            f"must be inert when opt-in OFF: {trace[-1650:]}"
        )
    passed_runs += 1
    print("PASS heap_string: native explicit GC injection is "
          "completely disabled with feature opt-in OFF", flush=True)

    # An explicit collection request must not be forgotten: when the
    # native Fiber is genuinely RETIRED by an authenticated handoff,
    # the fully rooted Interpreter has become the GC authority and
    # the pending request must be fulfilled exactly once even without
    # the separate test-only "GC after adopt" mechanism.
    source = fixture(args.out, "ssa_new_join_branch_string")
    expected, _ = execute(native, source, "off", None)
    actual, trace = execute(
        native, source, "eager", 2,
        llvm_ssa_liveness=True, detached_llvm_ssa=True,
        lazy_typed_ssa=True, require_generated_ssa_roots=True,
        inject_explicit_gc_before_ssa_root=True,
        gc_after_adopt=False, gc_during_ssa_transfer=False,
    )
    drain = re.findall(
        r"\[native-ssa-explicit-gc-drained\] script=([^ ]+) "
        r"before=(\d+) after=(\d+) pending=(\d+)", trace
    )
    if (
        actual != expected or trace.count("adopted=1") != 1 or
        len(drain) != 1 or
        int(drain[0][2]) != int(drain[0][1]) + 1 or
        drain[0][3] != "0" or
        "[native-gc-after-adopt]" in trace
    ):
        raise AssertionError(
            f"ssa_new_join_branch_string: authenticated handoff "
            f"did not drain pending direct GC safely: {trace[-1850:]}"
        )
    passed_runs += 1

    # A rejected deopt leaves the original native Fiber runnable.
    # No collector can service the deferred explicit request while
    # that Fiber is suspended at an unproven checkpoint. It is only
    # drained after the native script FINISHES and the Fiber is retired.
    source = fixture(args.out, "ssa_new_join_branch_string")
    actual, trace = execute(
        native, source, "eager", 1,
        llvm_ssa_liveness=True, detached_llvm_ssa=True,
        lazy_typed_ssa=True, require_generated_ssa_roots=True,
        inject_explicit_gc_before_ssa_root=True,
        corrupt_detached_llvm_ssa="generated-word-rehash",
        trace_native_gc_deferral=True,
        gc_after_adopt=False,
    )
    finished = re.findall(
        r"\[native-ssa-explicit-gc-finished\] script=([^ ]+) "
        r"before=(\d+) after=(\d+) pending=(\d+)", trace
    )
    if (
        actual != expected or "adopted=0" not in trace or
        "adopted=1" in trace or
        "[native-ssa-explicit-gc-drained]" in trace or
        len(finished) != 1 or
        int(finished[0][2]) != int(finished[0][1]) + 1 or
        finished[0][3] != "0"
    ):
        raise AssertionError(
            f"ssa_new_join_branch_string: rejected transfer must "
            f"finish native Fiber before delayed explicit collection: "
            f"{trace[-1950:]}"
        )
    passed_runs += 1
    print("PASS ssa_new_join_branch_string: pending direct GC "
          "drained exactly once after successful interpreter handoff "
          "or failed handoff followed by native finish (2)",
          flush=True)

    # RT-001 opt-in GC SOFT budget: real Scratch join allocation
    # exceeds 1 MiB while automatic and direct GC remain deferred.
    # The normal nonwarp loop yields a source-certified AfterLoopBody
    # with genuine live JIT-generated heap string PHI. It must
    # automatically adopt a verified interpreter continuation WITHOUT
    # enabling the test-only Nth-yield deopt mechanism.
    source = fixture(args.out, "ssa_budget_join_string")
    expected, _ = execute(native, source, "off", None)
    actual, trace = execute(
        native, source, "eager", None,
        llvm_ssa_liveness=True, detached_llvm_ssa=True,
        lazy_typed_ssa=True, require_generated_ssa_roots=True,
        soft_gc_budget_mb=1,
    )
    moved = re.findall(
        r"\[native-gc-soft-budget\] script=([^ ]+) "
        r"requested=1 adopted=1 collected=1 "
        r"bytes_before=(\d+) bytes_after=(\d+)", trace
    )
    if (
        actual != expected or len(moved) != 1 or
        int(moved[0][1]) < 1048576 or
        int(moved[0][2]) >= int(moved[0][1]) or
        trace.count("[native-simple-deopt]") != 1
    ):
        raise AssertionError(
            "ssa_budget_join_string: budget auto source retirement "
            f"did not reclaim at verified Interpreter state: {trace[-1650:]}"
        )
    passed_runs += 1
    print("PASS ssa_budget_join_string: real 1-MiB soft budget "
          "automatically retires native Fiber and collects only "
          "after exact source adoption", flush=True)

    # An available larger budget, or a disabled root-ownership
    # experiment, must not introduce a silent or invented deopt.
    for mb, enabled in ((2, True), (1, False)):
        actual, trace = execute(
            native, source, "eager", None,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True,
            require_generated_ssa_roots=enabled,
            soft_gc_budget_mb=mb,
        )
        if actual != expected or "[native-gc-soft-budget]" in trace:
            raise AssertionError(
                f"ssa_budget_join_string: soft budget {mb} MiB, "
                f"producer={enabled} erroneously triggered: "
                f"{trace[-1550:]}"
            )
        passed_runs += 1
    print("PASS ssa_budget_join_string: 2 quiet controls "
          "(below budget and disabled exact-root producer)",
          flush=True)

    # A forged SSA image cannot authorize a memory-pressure GC,
    # even after the live native Fiber has accumulated >1MiB.
    # It may only be collected AFTER normal script completion.
    actual, trace = execute(
        native, source, "eager", None,
        llvm_ssa_liveness=True, detached_llvm_ssa=True,
        lazy_typed_ssa=True, require_generated_ssa_roots=True,
        soft_gc_budget_mb=1,
        corrupt_detached_llvm_ssa="generated-word-rehash",
    )
    refused = re.findall(
        r"\[native-gc-soft-budget\] script=([^ ]+) "
        r"requested=1 adopted=0 collected=0 "
        r"reason=source-refused", trace
    )
    finished = re.findall(
        r"\[native-gc-soft-budget-finished\] script=([^ ]+) "
        r"collected=1 bytes_before=(\d+) bytes_after=(\d+)", trace
    )
    if (
        actual != expected or len(refused) != 1 or
        len(finished) != 1 or
        "adopted=1" in trace or
        int(finished[0][2]) >= int(finished[0][1])
    ):
        raise AssertionError(
            "ssa_budget_join_string: corrupt image collected "
            f"while native Fiber was runnable: {trace[-1950:]}"
        )
    passed_runs += 1

    # A source-proven declared-warp procedure is now recoverable at
    # the committed AfterLoopBody checkpoint. Reconstruct the source
    # procedure's original warpMode before continuing in Interpreter.
    # Nested cases additionally test captured parent TIMES counters,
    # an interposed IF arm, and warp inheritance by nonwarp children.
    # These must not re-evaluate TIMES (z changes inside the inner loop)
    # or silently drop lexical frames after native Fiber retirement.
    for warp_case, expected_warp_frames in (
        ("ssa_budget_join_warp", 2),
        ("ssa_budget_join_warp_nested", 3),
        ("ssa_budget_join_warp_nested_loop", 4),
        ("ssa_budget_join_warp_nested_if_loop", 5),
        ("ssa_budget_join_warp_recursive", 8),
    ):
        source_warp = fixture(args.out, warp_case)
        expected_warp, _ = execute(native, source_warp, "off", None)
        actual, trace = execute(
            native, source_warp, "eager", None,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=True,
            soft_gc_budget_mb=1,
        )
        accepted = re.findall(
            r"\[native-gc-soft-budget\] script=([^ ]+) "
            r"requested=1 adopted=1 collected=1 "
            r"bytes_before=(\d+) bytes_after=(\d+) "
            r"warp_frames=(\d+)", trace,
        )
        if (
            actual != expected_warp or len(accepted) != 1 or
            int(accepted[0][1]) < 1048576 or
            int(accepted[0][2]) >= int(accepted[0][1]) or
            int(accepted[0][3]) != expected_warp_frames or
            trace.count("[native-simple-deopt]") != 1 or
            "[native-gc-soft-budget-finished]" in trace
        ):
            raise AssertionError(
                f"{warp_case}: proven warp handoff did not restore "
                f"the exact source warp stack and post-Fiber GC: "
                f"{trace[-1850:]}"
            )
        passed_runs += 1

        # Changing an SSA word without disturbing its local fingerprint
        # must STILL refuse the handoff and leave native code running.
        # No GC until the original warp Fiber actually finishes.
        actual, trace = execute(
            native, source_warp, "eager", None,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=True,
            soft_gc_budget_mb=1,
            corrupt_detached_llvm_ssa="generated-word-rehash",
        )
        if (
            actual != expected_warp or
            trace.count("requested=1 adopted=0 collected=0") != 1 or
            trace.count("[native-gc-soft-budget-finished]") != 1 or
            "adopted=1" in trace
        ):
            raise AssertionError(
                f"{warp_case}: forged SSA must refuse warp handoff "
                f"and collect only after Fiber completion: "
                f"{trace[-1750:]}"
            )
        passed_runs += 1

        # An 8-MiB budget exceeds these fixtures' native GC pressure,
        # including all four recursive invocations. A configured
        # budget alone must not permit a speculative warp handoff.
        actual, trace = execute(
            native, source_warp, "eager", None,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=True,
            soft_gc_budget_mb=8,
        )
        if actual != expected_warp or "[native-gc-soft-budget]" in trace:
            raise AssertionError(
                f"{warp_case}: warp source resumed despite having "
                f"no pending memory budget request: {trace[-1600:]}"
            )
        passed_runs += 1

        # Even with a 1-MiB threshold, the optimizer may not enter the
        # warp recovery path without compiler-proven generated SSA roots.
        # This must remain a quiet, nontransferring native execution.
        if warp_case.endswith(("_loop", "_recursive")):
            actual, trace = execute(
                native, source_warp, "eager", None,
                llvm_ssa_liveness=True, detached_llvm_ssa=True,
                lazy_typed_ssa=True, require_generated_ssa_roots=False,
                soft_gc_budget_mb=1,
            )
            if (actual != expected_warp or
                "[native-gc-soft-budget]" in trace or
                "[native-simple-deopt]" in trace):
                raise AssertionError(
                    f"{warp_case}: no exact SSA-root owner yet resumed "
                    f"warp code or changed its result: {trace[-1600:]}"
                )
            passed_runs += 1
    print("PASS GC soft-budget: source-certified warp and "
          "nested-inherited-warp transfers (5), forged SSA "
          "fail-closed (5), no-pressure controls (5), "
          "missing-root controls (3)",
          flush=True)

    # Keep the explicit 32-frame guard effective for warp recursion:
    # deeper native stacks are not materializable in this source subset.
    # The budget MUST refuse to retire their Fiber, then run GC only
    # after native completion, preserving exact interpreter semantics.
    source_deep = fixture(args.out, "ssa_budget_join_warp_depth_overflow")
    expected_deep, _ = execute(native, source_deep, "off", None)
    actual, trace = execute(
        native, source_deep, "eager", None,
        llvm_ssa_liveness=True, detached_llvm_ssa=True,
        lazy_typed_ssa=True, require_generated_ssa_roots=True,
        soft_gc_budget_mb=1,
    )
    if (
        actual != expected_deep or
        trace.count("requested=1 adopted=0 collected=0") != 1 or
        trace.count("[native-gc-soft-budget-finished]") != 1 or
        "adopted=1" in trace
    ):
        raise AssertionError(
            "ssa_budget_join_warp_depth_overflow: over-32 depth "
            f"must refuse and collect only after Fiber exit: {trace[-1800:]}"
        )
    passed_runs += 1
    print("PASS GC soft-budget: 34-deep warp recursion cannot exceed "
          "32-frame handoff limit; Fiber finishes before GC", flush=True)

    # GC-triggered JIT -> Interpreter warp transfer must not introduce
    # a synthetic scheduler yield. In particular, another green-flag
    # script must not observe a partially updated shared variable or
    # overwrite it before the remainder of this warp script completes.
    for case, final_x in (
        ("ssa_budget_warp_two_hats_observer", "17000"),
        ("ssa_budget_warp_two_hats_writer", "777"),
    ):
        source_warp = fixture(args.out, case)
        expected, _ = execute(native, source_warp, "off", None)
        eager, _ = execute(native, source_warp, "eager", None)
        got, trace = execute(
            native, source_warp, "eager", None,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=True,
            soft_gc_budget_mb=1,
        )
        accepted = re.findall(
            r"\[native-gc-soft-budget\] script=([^ ]+) "
            r"requested=1 adopted=1 collected=1 "
            r"bytes_before=(\d+) bytes_after=(\d+) "
            r"warp_frames=(\d+)", trace,
        )
        if (
            expected != eager or got != expected or
            got["Bench"]["x"] != final_x or
            got["Bench"]["y"] != "17000" or
            len(accepted) != 1 or int(accepted[0][3]) != 2 or
            int(accepted[0][1]) < 1048576 or
            int(accepted[0][2]) >= int(accepted[0][1]) or
            trace.count("[native-simple-deopt]") != 1
        ):
            raise AssertionError(
                f"{case}: native soft budget leaked a warp scheduling "
                f"yield to the other green-flag hat: "
                f"observed_x={got['Bench']['x']} "
                f"observed_y={got['Bench']['y']} "
                f"trace={trace[-1400:]}"
            )
        passed_runs += 1

        # No artificial memory pressure => no native handoff and no
        # change to inter-thread scheduling or final effect ordering.
        quiet, trace = execute(
            native, source_warp, "eager", None,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=True,
            soft_gc_budget_mb=8,
        )
        if quiet != expected or "[native-gc-soft-budget]" in trace:
            raise AssertionError(
                f"{case}: 8-MiB no-pressure scheduler control failed"
            )
        passed_runs += 1

        # The same artificial budget checkpoint must not expose a
        # scheduling yield even when the independent source/SSA proof
        # FAILS: continue the ORIGINAL native warp Fiber within its
        # existing scheduler turn. GC still waits until Fiber exit.
        forged, trace = execute(
            native, source_warp, "eager", None,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=True,
            soft_gc_budget_mb=1,
            corrupt_detached_llvm_ssa="generated-word-rehash",
        )
        if (
            forged != expected or
            trace.count("requested=1 adopted=0 collected=0") != 1 or
            trace.count("[native-gc-soft-budget-finished]") != 1 or
            "adopted=1" in trace
        ):
            raise AssertionError(
                f"{case}: source/SSA refusal leaked a scheduler yield "
                f"or collected suspended native roots: {trace[-1600:]}"
            )
        passed_runs += 1

        # Without the exact GC-generated SSA owner, there must not
        # be a budget request or a speculative interpreter handoff.
        unrooted, trace = execute(
            native, source_warp, "eager", None,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=False,
            soft_gc_budget_mb=1,
        )
        if (
            unrooted != expected or
            "[native-gc-soft-budget]" in trace or
            "[native-simple-deopt]" in trace
        ):
            raise AssertionError(
                f"{case}: missing SSA root owner changed scheduling"
            )
        passed_runs += 1
    print("PASS GC soft-budget: two concurrent green-flag hats "
          "preserve warp atomicity and shared-variable write order, "
          "including forged SSA fail-closed (8)",
          flush=True)

    # One GC cannot sweep while ANOTHER suspended native LLVM Fiber
    # may still own compiler-live SSA that has never been exported as
    # GC roots. Two distinct green-flag hats run the SAME 24k-join
    # nonwarp procedure with independent CallFrame/SSA snapshots.
    # Their shared managed-GC allocation crosses 1 MiB while BOTH
    # Fibers are live. The first handoff must defer actual GC, and
    # only the handoff that retires the last Fiber may sweep.
    # Guarded native variable binding checks resolve Variable* from
    # Target on every use. These executions validate the new LLVM
    # external resolver symbol and prevent scalar or list promotions
    # from hoisting a stale cached node across scheduler yields.
    for guarded_case in ("varslot_guard_editor_mutation",
                         "ssa_budget_cross_fiber_two_hats"):
        guarded_project = fixture(args.out, guarded_case)
        control, _ = execute(native, guarded_project, "off", None)
        for stamped_fast in (False, True):
            guarded, trace = execute(
                native, guarded_project, "eager", None,
                guard_native_var_slots=True,
                guard_native_var_slots_fast=stamped_fast,
            )
            if control != guarded or "JIT session error" in trace:
                raise AssertionError(
                    f"{guarded_case}: guarded stamp_fast={stamped_fast} "
                    f"JIT mismatches Interpreter: {trace[-3000:]}"
                )
            passed_runs += 1
    print("PASS RT-001 guarded native varslot rebinding: "
          "full-relookup vs generation-stamped; scalar and "
          "concurrent two-Fiber Unicode parity (4)", flush=True)

    cross_case = fixture(args.out, "ssa_budget_cross_fiber_two_hats")
    cross_expected, _ = execute(native, cross_case, "off", None)
    cross_actual, cross_trace = execute(
        native, cross_case, "eager", None,
        llvm_ssa_liveness=True, detached_llvm_ssa=True,
        lazy_typed_ssa=True, require_generated_ssa_roots=True,
        soft_gc_budget_mb=1,
    )
    cross_events = re.findall(
        r"\[native-gc-soft-budget\] script=(\S+) requested=1 "
        r"adopted=1 collected=(\d+) bytes_before=(\d+) "
        r"bytes_after=(\d+) warp_frames=(\d+) pending=(\d+) "
        r"active_fibers=(\d+)", cross_trace,
    )
    if (
        cross_actual != cross_expected or
        cross_actual["Bench"]["y"] != "48005" or
        len(cross_events) != 2 or
        len({event[0] for event in cross_events}) != 2 or
        tuple(int(x) for x in cross_events[0][1:])[:1] != (0,) or
        tuple(int(x) for x in cross_events[0][4:]) != (0, 1, 1) or
        tuple(int(x) for x in cross_events[1][4:]) != (0, 0, 0) or
        cross_events[1][1] != "1" or
        int(cross_events[0][2]) < 1048576 or
        int(cross_events[0][3]) != int(cross_events[0][2]) or
        int(cross_events[1][3]) >= int(cross_events[1][2]) or
        cross_trace.count("[native-simple-deopt]") != 2
    ):
        raise AssertionError(
            "ssa_budget_cross_fiber_two_hats: unsafe collector sweep "
            "while the second LLVM SSA activation was live, or final "
            f"effects mismatch: {cross_trace[-2800:]}"
        )
    passed_runs += 1
    # Forged SSA forces BOTH handoffs to fail. Their Fibers must remain
    # runnable, both GC requests must be postponed, and only the last
    # completed Fiber may authorize a real collection.
    cross_actual, cross_trace = execute(
        native, cross_case, "eager", None,
        llvm_ssa_liveness=True, detached_llvm_ssa=True,
        lazy_typed_ssa=True, require_generated_ssa_roots=True,
        soft_gc_budget_mb=1,
        corrupt_detached_llvm_ssa="generated-word-rehash",
    )
    finished = re.findall(
        r"\[native-gc-soft-budget-finished\] script=(\S+) "
        r"collected=(\d+) bytes_before=(\d+) bytes_after=(\d+) "
        r"pending=(\d+) active_fibers=(\d+)", cross_trace,
    )
    if (
        cross_actual != cross_expected or len(finished) != 2 or
        cross_trace.count("requested=1 adopted=0 collected=0") != 2 or
        "adopted=1" in cross_trace or
        tuple(int(x) for x in finished[0][1:][0:1]) != (0,) or
        tuple(int(x) for x in finished[0][4:]) != (1, 1) or
        tuple(int(x) for x in finished[1][4:]) != (0, 0) or
        finished[1][1] != "1"
    ):
        raise AssertionError(
            "ssa_budget_cross_fiber_two_hats: forged SSA must keep "
            f"both Fibers alive and GC deferred: {cross_trace[-2600:]}"
        )
    passed_runs += 1

    # The feature-disabled and above-budget cases must not invent
    # post-Fiber GC or introduce a second source handoff.
    for roots, budget in ((False, 1), (True, 8)):
        actual, trace = execute(
            native, cross_case, "eager", None,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=roots,
            soft_gc_budget_mb=budget,
        )
        if actual != cross_expected or "[native-gc-soft-budget]" in trace:
            raise AssertionError(
                "ssa_budget_cross_fiber_two_hats: quiet GC owner/budget "
                f"control failed: {trace[-1500:]}"
            )
        passed_runs += 1
    print("PASS GC soft-budget: 2 simultaneous native Fibers; first "
          "sweep deferred, last Fiber retirement collects; both "
          "forged-SSA refusals defer too (4)", flush=True)

    # Compiler-only pointer provenance audit on the same REAL two-Fiber
    # Scratch workload. Region counts must partition the existing
    # conservative unrepresentable pointer census exactly. Host
    # context origins never certify pointers loaded from JitCtx, and
    # NO region result may allow collection of a live LLVM Fiber.
    observed, trace = execute(
        native, cross_case, "eager", None,
        llvm_ssa_liveness=True, detached_llvm_ssa=True,
        trace_detached_llvm_ssa=True, lazy_typed_ssa=True,
        require_generated_ssa_roots=True, soft_gc_budget_mb=1,
    )
    raw_reports = re.findall(
        r"\[rt001-ssa-extra\] fn=(\S+) .*"
        r"unresolved_ptrs=(\d+) other=\d+ "
        r"stack_derived=(\d+) stack_roots=(\d+)", trace,
    )
    region_reports = re.findall(
        r"\[rt001-ssa-pointer-regions\] fn=(\S+) "
        r"native_stack=(\d+) host_ctx=(\d+) "
        r"static_global=(\d+) unknown=(\d+) "
        r"unresolved_total=(\d+) gc_safe=(\d+)", trace,
    )
    unknown_reports = re.findall(
        r"\[rt001-ssa-pointer-unknown\] fn=(\S+) "
        r"load_stack=(\d+) load_host=(\d+) "
        r"load_static=(\d+) load_unknown=(\d+) "
        r"call=(\d+) cast=(\d+) foreign_arg=(\d+) "
        r"phi_select=(\d+) other=(\d+) total=(\d+) "
        r"gc_safe=(\d+)", trace,
    )
    unknown_ids = re.findall(
        r"\[rt001-ssa-pointer-unknown-id\] "
        r"fn=(\S+) id=(\d+) cause=(\w+)", trace,
    )
    load_chains = re.findall(
        r"\[rt001-ssa-pointer-load-chain\] "
        r"fn=(\S+) id=(\d+) loads=(\d+) derivations=(\d+) "
        r"terminal=(\w+) ctx_field=(\w+) "
        r"ctx_offset=(\d+) gc_safe=(\d+)", trace,
    )
    from collections import Counter
    # Each script has TWO source checkpoints, so compiler instruction
    # IDs can repeat across records. Counters deliberately preserve
    # those repetitions instead of silently dropping collisions.
    expected_load_ids = Counter(
        (fn, iid) for fn, iid, cause in unknown_ids
        if cause.startswith("load_")
    )
    actual_load_ids = Counter(
        (fn, iid) for fn, iid, *_ in load_chains
    )
    id_counts = Counter((fn, cause) for fn, _, cause in unknown_ids)
    report_counts = Counter()
    cause_names = (
        "load_stack", "load_host", "load_static", "load_unknown",
        "call_return", "int_or_addr_cast", "foreign_argument",
        "phi_select", "other",
    )
    for record in unknown_reports:
        for i, cause in enumerate(cause_names):
            report_counts[(record[0], cause)] += int(record[i + 1])
    if (
        observed != cross_expected or
        len(unknown_reports) != len(region_reports) or
        any(
            unknown[0] != region[0] or
            sum(int(n) for n in unknown[1:10]) != int(region[4]) or
            int(unknown[10]) != int(region[4]) or
            unknown[11] != "0"
            for unknown, region in zip(unknown_reports, region_reports)
        ) or not any(int(record[2]) > 0 for record in unknown_reports) or (
            id_counts != report_counts
        ) or (
            actual_load_ids != expected_load_ids
        ) or any(
            # A source pointer produced by an earlier Load remains
            # UNKNOWN, even when its address chain ultimately starts
            # from the ABI-proven JitCtx allocation. This is a hazard
            # diagnostic, never a GC root certificate.
            terminal != "host" or safe != "0" or
            int(loads) not in (1, 2) or
            int(derivations) < 1 or
            (int(loads) == 1 and (
                int(derivations) != 1 or
                field != "sampledWarpClockUs")) or
            (int(loads) == 2 and field != "varSlotData") or
            int(offset) == 0
            for _, _, loads, derivations, terminal, field, offset, safe
            in load_chains
        ) or (
            not any(int(loads) == 2 for _, _, loads, *_ in load_chains)
        ) or (
            not any(int(loads) == 1 for _, _, loads, *_ in load_chains)
        ) or (
            len({offset for _, _, _, _, _, field, offset, _ in load_chains
                 if field == "varSlotData"}) != 1
        ) or (
            len({offset for _, _, _, _, _, field, offset, _ in load_chains
                 if field == "sampledWarpClockUs"}) != 1
        ) or (
            {field for _, _, _, _, _, field, _, _ in load_chains} !=
            {"varSlotData", "sampledWarpClockUs"}
        ) or
        len(raw_reports) < 2 or
        len(raw_reports) != len(region_reports) or
        len({r[0] for r in region_reports}) < 2 or
        any(
            raw[0] != region[0] or
            int(raw[1]) != int(region[5]) or
            int(raw[2]) != int(region[1]) or
            sum(int(n) for n in region[1:5]) != int(region[5]) or
            int(region[2]) == 0 or int(region[4]) == 0 or
            region[6] != "0"
            for raw, region in zip(raw_reports, region_reports)
        ) or
        not re.search(
            r"\[native-gc-soft-budget\].*"
            r"collected=0 .*active_fibers=1", trace,
        )
    ):
        raise AssertionError(
            "ssa_budget_cross_fiber_two_hats: compiler pointer regions "
            "must partition the original unsupported values without "
            f"authorizing GC: {trace[-3600:]}"
        )
    passed_runs += 1
    print("PASS RT-001 compiler-only GC pointer address provenance: "
          "nested variable pointers map to JitCtx::varSlotData and "
          "single timer pointer maps to sampledWarpClockUs; no "
          "live-Fiber GC authorization (1)", flush=True)

    # The final callee can return with only the root CallFrame left.
    # IR-001's dedicated rootPostCallSuccessor() provides the *source*
    # return edge; RT-001 must additionally recover the exact root lexical
    # scopes and committed effects before it may discard the native Fiber.
    root_only = {
        "terminal_call": 4,  # ScriptExit: direct terminal call
        "numeric": 0,  # NextBlock: no lexical controls
        "root_repeat_three_call": 2,  # LoopLatch
        "root_repeat_dynamic_three_call": 2,
        "root_nested_repeat_nineteen_seven": 2,
        "root_if_call": 1,  # AfterBranch: do not replay IF reporter
        "root_if_repeat_three_call": 2,
        # ScriptExit with a committed, terminal lexical IF/IF-ELSE
        # ancestry (including eight nested IF arms).
        "root_terminal_if": 4,
        "root_terminal_ifelse_else": 4,
        "root_terminal_nested_if": 4,
        "root_terminal_nested_ifelse_then": 4,
        "root_terminal_nested_ifelse_else": 4,
        "root_terminal_eight_if": 4,
        # ScriptExit is not the same as LoopLatch: after the call has
        # completed one or more terminal IF arms, retain the real loop
        # counters and jump back to the source-proven latch.
        "root_loop_terminal_if": 2,
        "root_loop_terminal_ifelse_else": 2,
        "root_loop_terminal_nested_ifelse": 2,
        "root_loop_terminal_double_repeat": 2,
    }
    for case, expected_kind in root_only.items():
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        checkpoints = (
            (1, 200) if case == "root_nested_repeat_nineteen_seven"
            else (1, 3) if "repeat_three" in case or "dynamic_three" in case
            else (1,)
        )
        for checkpoint in checkpoints:
            actual, trace = execute(
                native, source, "eager", checkpoint,
                yield_after_callee=True, only_after_callee=True,
                probe_source=True, gc_after_adopt=True,
            )
            attempts = re.findall(
                r"\[native-simple-deopt\].*adopted=(\d+)", trace
            )
            site = re.findall(
                r"\[native-postcall-site\] kind=(-?\d+)"
                r" target=\d+ call=\d+ valid=(\d+) depth=(\d+)",
                trace,
            )
            if actual != expected or attempts.count("1") != 1 or (
                (str(expected_kind), "1", "1") not in site
            ) or trace.count("[native-gc-after-adopt]") != 1:
                raise AssertionError(
                    f"{case}: root-only AfterCallee checkpoint "
                    f"{checkpoint} did not transfer exactly once, "
                    f"or saved root controls were corrupted: {trace[-1800:]}"
                )
            passed_runs += 1
        print(f"PASS {case}: root-only AfterCallee {checkpoints}", flush=True)

    # Stale root-only images must never authorize a synthetic return.
    for mode in ("revision", "digest", "real_cache_reset"):
        case = "root_repeat_three_call"
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 1,
            yield_after_callee=True, only_after_callee=True,
            bump_source_revision=mode in ("revision", "real_cache_reset"),
            real_cache_reset=mode == "real_cache_reset",
            edit_source_without_revision=mode == "digest",
            probe_source=True, gc_after_adopt=True,
        )
        marker = (
            "[native-source-content-edit]" if mode == "digest"
            else "[native-real-cache-reset]" if mode == "real_cache_reset"
            else "[native-source-revision-bump]"
        )
        if actual != expected or marker not in trace or (
            "adopted=1" in trace or "[native-gc-after-adopt]" in trace
        ):
            raise AssertionError(
                f"{case}: stale root AfterCallee {mode} adopted or crashed "
                f"{trace[-1800:]}"
            )
        passed_runs += 1
        print(f"PASS {case}: stale root-only {mode} refused", flush=True)

    # Source edits must also be rejected at a terminal IF ScriptExit:
    # the IF condition changed during the native callee, so re-running
    # its body would silently lose parity if an old image were adopted.
    for case in (
        "root_terminal_if", "root_terminal_nested_ifelse_else",
        "root_loop_terminal_if", "root_loop_terminal_double_repeat",
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        for mode in ("revision", "digest", "real_cache_reset"):
            actual, trace = execute(
                native, source, "eager", 1,
                yield_after_callee=True, only_after_callee=True,
                bump_source_revision=mode in ("revision", "real_cache_reset"),
                real_cache_reset=mode == "real_cache_reset",
                edit_source_without_revision=mode == "digest",
                gc_after_adopt=True,
            )
            marker = (
                "[native-source-content-edit]" if mode == "digest"
                else "[native-real-cache-reset]" if mode == "real_cache_reset"
                else "[native-source-revision-bump]"
            )
            if actual != expected or marker not in trace or (
                "adopted=1" in trace or "[native-gc-after-adopt]" in trace
            ):
                raise AssertionError(
                    f"{case}: stale terminal branch {mode} was adopted "
                    f"or crashed: {trace[-1800:]}"
                )
            passed_runs += 1
        print(f"PASS {case}: terminal conditional invalidation (3 modes)", flush=True)

    for case in INTERLEAVED:
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        baseline, _ = execute(native, source, "eager", None)
        if expected != baseline:
            raise AssertionError(
                f"{case}: two-hat baseline scheduler order mismatch"
                f"\ninterpreter={expected}\nnative={baseline}"
            )
        checkpoints = (
            (1, 2, 3, 9, 12, 17)
            if case == "interleaved_two_root_counted_callframes"
            else (1, 2, 3)
        )
        for checkpoint in checkpoints:
            result, log = execute(
                native, source, "eager", checkpoint,
                probe_source=True, gc_after_adopt=True,
                yield_after_callee=True,
            )
            if result != expected:
                raise AssertionError(
                    f"{case}: interleaved native handoff dropped/replayed"
                    f" cross-hat effects at checkpoint {checkpoint}:"
                    f"\ninterpreter={expected}\nactual={result}"
                )
            adopted = re.findall(
                r"\[native-simple-deopt\].*adopted=(\d+)", log
            )
            collections = re.findall(
                r"\[native-gc-after-adopt\].*collected=1", log
            )
            expected_count = (
                # A 5×3-yield script and a 3×3-yield script.
                # At count 10–15 only the first remains live;
                # after 15 neither can contribute a native checkpoint.
                (2 if checkpoint <= 9 else
                 1 if checkpoint <= 15 else 0)
                if case == "interleaved_two_root_counted_callframes"
                else 2 if case == "interleaved_two_native_callframes"
                else 1
            )
            if adopted.count("1") != expected_count or (
                len(collections) != expected_count
            ):
                raise AssertionError(
                    f"{case}: expected {expected_count} independent"
                    f" native-to-interpreter transfers and full GCs:"
                    f" {log[-1500:]}"
                )
            passed_runs += 1
        # Require a real source-level post-return yield, rather than an
        # unrelated repeat-body checkpoint. Both independent hats may
        # now transfer from a root-only AfterCallee using the verified
        # source return edge and each hat's own saved root counters.
        result, trace = execute(
            native, source, "eager", 1,
            probe_source=True, gc_after_adopt=True,
            yield_after_callee=True, only_after_callee=True,
        )
        actual_adopts = re.findall(
            r"\[native-simple-deopt\].*adopted=(\d+)", trace
        )
        collections = re.findall(
            r"\[native-gc-after-adopt\].*collected=1", trace
        )
        expected_adopts = (
            2 if case in (
                "interleaved_two_native_callframes",
                "interleaved_two_root_counted_callframes",
            ) else 1
        )
        if result != expected or (
            actual_adopts.count("1") != expected_adopts
        ) or len(collections) != expected_adopts:
            raise AssertionError(
                f"{case}: cross-hat strict AfterCallee adoption or"
                f" rejection inconsistent: {trace[-1700:]}"
            )
        passed_runs += 1
        print(
            f"PASS {case}: {len(checkpoints)} cross-hat checkpoints "
            f"+ strict AfterCallee "
            f"{expected_adopts} handoff(s)",
            flush=True,
        )
    # Production cache invalidation used to free BlockCached* embedded in
    # suspended native compiled code and yielded StackFrames. Exercise the
    # *real* Blocks::resetCache() while fibers remain alive, then force GC
    # before those original fibers resume. Only the code's lookup index
    # should be invalidated; older cache storage must remain rooted.
    for case in (
        "recursive", "root_nested_repeat_nineteen_seven",
        "nested_call_string", "interleaved_two_root_counted_callframes",
    ):
        source = fixture(args.out, case)
        expected, _ = execute(native, source, "off", None)
        actual, trace = execute(
            native, source, "eager", 1,
            bump_source_revision=True, real_cache_reset=True,
            probe_source=True,
        )
        if actual != expected or (
            "[native-real-cache-reset]" not in trace
        ) or (
            "[native-source-revision-bump]" not in trace
        ) or re.search(
            r"\[native-simple-deopt\].*adopted=1", trace
        ):
            raise AssertionError(
                f"{case}: real cache invalidation used stale native"
                f" pointer or adopted old code: {trace[-1900:]}"
            )
        passed_runs += 1
        print(
            f"PASS {case}: live Native Fiber survived actual"
            f" resetCache + full GC",
            flush=True,
        )
    print(f"PASS {passed_runs} native resume/fail-closed parity runs", flush=True)


if __name__ == "__main__":
    main()
