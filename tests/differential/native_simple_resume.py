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
            root_only_producer: bool = False) -> tuple[dict, str]:
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
        XYO_PAR="0",
        XYO_DUMPVARS="json",
        XYO_EXIT_ON_DONE="1",
    )
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
