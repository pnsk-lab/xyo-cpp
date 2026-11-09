#!/usr/bin/env python3
"""Real LLVM Fiber CallFrame mirror parity against the interpreter.

Scratch's raw math_number literal is often a STRING Value. For tests of
numeric raw-SSA arguments use operator_add as the producer. Heap
string arguments are retained by the GC-rooted sidecar; the numeric arena
contains no raw GC pointers.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_ifelse, S_repeat, S_set,
    add, arg, lt, make_project, num, text, var, write_sb3,
)

CASES = (
    "one_arg", "two_args", "nested", "nonwarp_recursive",
    "nonwarp_yield", "nonwarp_nested", "nonwarp_triple", "dynamic",
    "non_numeric",
    "nonwarp_string", "string_recursive", "loop_recursive",
    "empty_string_arg", "nonwarp_wait", "branch_return", "loop_return",
    "procedure_return", "distinct_return_sites", "script_return",
    "branch_arms", "nested_branch_arms", "post_loop_call",
    "post_loop_wait", "root_nested_counted", "root_dynamic_counted",
)


def fixture(directory: Path, kind: str) -> Path:
    gen = Gen(["x", "y", "z"], [])
    # Force an actual numeric (not raw string literal) JIT argument.
    numeric = lambda value: add(num(value), num(0))
    args = [numeric(1)]
    signature = "worker %s"
    names = ["a"]
    body = [
        S_change("x", arg("a")),
        S_change("y", arg("a")),
    ]
    warp = True

    if kind == "two_args":
        signature, names = "worker %s %s", ["a", "b"]
        args = [numeric(1), numeric(2)]
        body = [
            S_change("x", arg("a")),
            S_change("y", arg("b")),
            S_change("z", arg("a")),
        ]
    elif kind == "nested":
        gen.define("inner %s", ["v"], [
            S_change("z", arg("v")),
            S_change("x", arg("v")),
        ], warp=True)
        body = [
            S_call("inner %s", [add(arg("a"), num(1))]),
            S_change("y", arg("a")),
        ]
    elif kind == "nonwarp_recursive":
        warp = False
        args = [numeric(0)]
        body = [
            S_change("x", arg("a")),
            S_if(lt(arg("a"), num(3)), [
                S_call(signature, [add(arg("a"), num(1))])
            ]),
            S_change("z", arg("a")),
        ]
    elif kind == "nonwarp_yield":
        warp = False
        body = [
            S_repeat(num(3), [S_change("x", arg("a"))]),
            S_change("z", arg("a")),
        ]
    elif kind in ("root_nested_counted", "root_dynamic_counted"):
        warp = False
        body = [
            S_repeat(num(3), [S_change("x", arg("a"))]),
            S_change("z", num(1)),
        ]
    elif kind == "nonwarp_nested":
        warp = False
        body = [
            S_repeat(num(2), [
                S_repeat(num(3), [S_change("x", arg("a"))]),
                S_change("y", arg("a")),
            ]),
            S_change("z", arg("a")),
        ]
    elif kind == "nonwarp_triple":
        warp = False
        body = [
            S_repeat(num(2), [
                S_repeat(num(2), [
                    S_repeat(num(2), [
                        S_change("x", arg("a")),
                    ]),
                    S_change("y", arg("a")),
                ]),
                S_change("z", arg("a")),
            ]),
        ]
    elif kind == "dynamic":
        body = [
            S_change("x", arg("a")),
            S_change("y", arg("a")),
        ]
        args = [var("x")]
    elif kind == "non_numeric":
        args = [text("hello")]
        body = [
            S_set("z", arg("a")),
            S_change("y", num(1)),
        ]
    elif kind == "nonwarp_wait":
        warp = False
        body = [
            S_change("x", arg("a")),
            S_change("y", arg("a")),
        ]
    elif kind == "nonwarp_string":
        warp = False
        args = [text("GC rooted string")]
        body = [
            S_repeat(num(3), [
                S_set("z", arg("a")),
                S_change("y", num(1)),
            ]),
        ]
    elif kind == "empty_string_arg":
        warp = False
        args = [text("")]
        body = [
            S_repeat(num(2), [
                S_set("z", arg("a")),
                S_change("y", num(1)),
            ]),
        ]
    elif kind == "loop_recursive":
        warp = False
        args = [numeric(0)]
        body = [
            S_repeat(num(2), [
                S_change("x", arg("a")),
                S_if(lt(arg("a"), num(2)), [
                    S_call(signature, [add(arg("a"), num(1))]),
                ]),
            ]),
            S_change("z", arg("a")),
        ]
    elif kind in ("branch_return", "loop_return", "procedure_return",
                  "distinct_return_sites"):
        warp = False
        gen.define("inner %s", ["n"], [
            S_repeat(num(2), [
                S_change("x", arg("n")),
            ]),
        ], warp=False)
        if kind == "branch_return":
            body = [
                S_if(lt(arg("a"), num(3)), [
                    S_call("inner %s", [arg("a")]),
                ]),
                S_change("z", arg("a")),
            ]
        elif kind == "loop_return":
            body = [
                S_repeat(num(2), [
                    S_call("inner %s", [arg("a")]),
                ]),
                S_change("z", arg("a")),
            ]
        elif kind == "procedure_return":
            body = [
                S_change("z", arg("a")),
                S_call("inner %s", [arg("a")]),
            ]
        else:
            body = [
                S_if(lt(arg("a"), num(3)), [
                    S_call("inner %s", [arg("a")]),
                ]),
                S_if(lt(arg("a"), num(4)), [
                    S_call("inner %s", [arg("a")]),
                ]),
                S_repeat(num(2), [
                    S_call("inner %s", [arg("a")]),
                ]),
                S_change("z", arg("a")),
            ]
    elif kind == "post_loop_wait":
        warp = False
        body = [
            S_repeat(num(2), [S_change("x", arg("a"))]),
            S_change("y", arg("a")),
            S_repeat(num(2), [S_change("z", arg("a"))]),
        ]
    elif kind == "post_loop_call":
        warp = False
        gen.define("inner %s", ["n"], [
            S_repeat(num(2), [S_change("y", arg("n"))]),
        ], warp=False)
        body = [
            S_repeat(num(2), [S_change("x", arg("a"))]),
            S_call("inner %s", [arg("a")]),
            S_repeat(num(2), [S_change("z", arg("a"))]),
            S_call("inner %s", [arg("a")]),
        ]
    elif kind in ("branch_arms", "nested_branch_arms"):
        warp = False
        gen.define("inner %s", ["n"], [
            S_repeat(num(2), [
                S_change("y", arg("n")),
            ]),
        ], warp=False)
        true_body = [S_call("inner %s", [arg("a")])]
        if kind == "nested_branch_arms":
            true_body = [
                S_if(lt(arg("a"), num(4)), true_body),
                S_change("z", num(1)),
            ]
        body = [
            S_ifelse(lt(arg("a"), num(6)), true_body, [
                S_call("inner %s", [add(arg("a"), num(1))]),
            ]),
            S_change("z", arg("a")),
        ]
    elif kind == "script_return":
        warp = False
        body = [
            S_repeat(num(2), [
                S_change("z", arg("a")),
            ]),
        ]
    elif kind == "string_recursive":
        warp = False
        signature, names = "worker %s %s", ["a", "n"]
        args = [text("GC recursive string"), numeric(0)]
        body = [
            S_set("z", arg("a")),
            S_if(lt(arg("n"), num(2)), [
                S_call(signature, [arg("a"), add(arg("n"), num(1))]),
            ]),
            S_repeat(num(2), [S_change("y", num(1))]),
        ]

    definition = gen.define(signature, names, body, warp=warp)
    if kind in ("nonwarp_wait", "post_loop_wait"):
        # The post-loop variant inserts a wait after the first completed
        # repeat. Any yield during the wait must NOT reuse the previous
        # clean AfterLoopBody checkpoint.
        # Insert a wait between two flat source statements. The base
        # statement generator has no built-in wait DSL expression.
        first = gen.blocks[definition]["next"]
        after = gen.blocks[first]["next"]
        assert after
        wait = gen.new("control_wait", parent=first)
        gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(
            num(0.005), wait)
        gen.blocks[wait]["next"] = after
        gen.blocks[first]["next"] = wait
        gen.blocks[after]["parent"] = wait
    if kind in ("branch_arms", "nested_branch_arms"):
        gen.script([
            S_set("x", num(0)),
            S_set("y", num(0)),
            S_set("z", num(0)),
            S_repeat(num(12), [
                S_change("x", num(1)),
                S_call(signature, [var("x")]),
            ]),
        ])
    elif kind == "root_nested_counted":
        gen.script([
            S_set("x", num(0)),
            S_set("y", num(0)),
            S_set("z", num(0)),
            S_repeat(num(3), [
                S_repeat(num(4), [S_call(signature, args)]),
            ]),
        ])
    elif kind == "root_dynamic_counted":
        gen.script([
            S_set("x", num(0)),
            S_set("y", num(0)),
            S_set("z", num(12)),
            # The root count is captured once. Callees mutate z, so a
            # re-evaluated TIMES reporter would diverge from Scratch.
            S_repeat(var("z"), [S_call(signature, args)]),
        ])
    elif kind == "script_return":
        gen.script([
            S_set("x", num(0)),
            S_set("y", num(0)),
            S_set("z", num(0)),
            S_repeat(num(11), [
                S_call(signature, args),
            ]),
            S_call(signature, args),
        ])
    else:
        gen.script([
            S_set("x", num(0)),
            S_set("y", num(0)),
            S_set("z", num(0)),
            S_repeat(num(12), [
                S_change("x", num(1)) if kind == "dynamic"
                else S_change("x", num(0)),
                S_call(signature, args),
            ]),
        ])
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"native-callframe-{kind}.sb3"
    write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
    return path


def execute(native: Path, project: Path, jit: str, mirror: bool,
            export_probe: bool = False):
    env = dict(
        os.environ,
        XYO_JIT_COMMON_CFG="0",
        XYO_JIT_CALLFRAME="1" if mirror else "0",
        XYO_JIT_CALLFRAME_STATS="1" if mirror else "",
        XYO_JIT_CALLFRAME_EXPORT_PROBE="1" if export_probe else "0",
        XYO_JIT_VERBOSE="1",
        XYO_DUMPVARS="json",
        XYO_EXIT_ON_DONE="1",
        XYO_PAR="0",
    )
    proc = subprocess.run(
        [str(native), "run", str(project), "--jit", jit,
         "--fps", "30", "--frames", "200"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=120,
    )
    if proc.returncode:
        raise AssertionError(
            f"{project.name} jit={jit} mirror={mirror}: "
            f"{proc.returncode}\n{proc.stderr[-1200:]}"
        )
    match = re.search(r'(?s)(\{\s*"Bench".*\})', proc.stdout)
    if not match:
        raise AssertionError(
            f"missing final variables: {project.name}\n{proc.stdout[-400:]}"
        )
    stats = [
        (tuple(map(int, m.groups()[:6])), m.group(7),
         int(m.group(8)), int(m.group(9)),
         int(m.group(10)), int(m.group(11)), int(m.group(12)),
         int(m.group(13)), int(m.group(14)), int(m.group(15)),
         int(m.group(16)), int(m.group(17)), int(m.group(18)),
         int(m.group(19)), int(m.group(20)),
         int(m.group(21)), int(m.group(22)),
         int(m.group(23)), int(m.group(24)), int(m.group(25)),
         int(m.group(26)), int(m.group(27)),
         int(m.group(28)), int(m.group(29)), int(m.group(30)),
         int(m.group(31)), int(m.group(32)), int(m.group(33)),
         int(m.group(34)), int(m.group(35)), int(m.group(36)))
        for m in re.finditer(
            r'\[jit-callframe\] script=\S+ calls=(\d+) '
            r'returns=(\d+) yields=(\d+) max_depth=(\d+) '
            r'enabled=(\d+) remaining=(\d+) reason=(\S+) '
            r'checkpoints=(\d+) last_pc=(\d+) '
            r'counters=(\d+) counter_sum=(\d+) '
            r'max_counter_slots=(\d+) heap_roots=(\d+) '
            r'rooted_frames=(\d+) peak_heap_roots=(\d+) '
            r'path_depth=(\d+) parent_counters=(\d+) '
            r'parent_counter_sum=(\d+) exports=(\d+) '
            r'max_export_roots=(\d+) max_export_depth=(\d+) '
            r'ancestor_loop_frames=(\d+) return_sites=(\d+) '
            r'loop_returns=(\d+) branch_returns=(\d+) '
            r'proc_returns=(\d+) script_returns=(\d+) '
            r'phase_entry=(\d+) phase_after_body=(\d+) '
            r'phase_after_callee=(\d+) awaiting_frames=(\d+) '
            r'awaiting_loop_frames=(\d+) phase_unknown=(\d+) '
            r'branch_then=(\d+) branch_else=(\d+) nested_branch=(\d+)', 
            proc.stderr,
        )
    ]
    return match.group(1), stats, proc.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--export-probe", action="store_true",
                        help="clone all live CallFrames and GC roots at each native yield")
    args = parser.parse_args()
    native = args.native.resolve()

    for kind in CASES:
        sb3 = fixture(args.out, kind)
        expected, _, _ = execute(native, sb3, "off", False)
        regular, _, _ = execute(native, sb3, "eager", False)
        mirrored, stats, diagnostics = execute(
            native, sb3, "eager", True, args.export_probe)
        if not (expected == regular == mirrored):
            raise AssertionError(
                f"{kind}: result divergence\n"
                f"interpreter={expected}\nnormal={regular}\nmirrored={mirrored}"
            )
        if not stats:
            raise AssertionError(
                f"{kind}: LLVM did not publish CallFrame diagnostics\n"
                f"{diagnostics[-1300:]}"
            )
        calls = sum(row[0][0] for row in stats)
        returns = sum(row[0][1] for row in stats)
        yields = sum(row[0][2] for row in stats)
        max_depth = max(row[0][3] for row in stats)
        enabled = all(row[0][4] == 1 for row in stats)
        if calls < 12 or returns != calls or max_depth < 2 or not enabled:
            raise AssertionError(
                f"{kind}: wrong frame accounting: {stats}\n"
                f"{diagnostics[-1500:]}"
            )
        if any(row[7] != 0 or row[8] != 0 for row in stats):
            raise AssertionError(f"{kind}: GC roots or frames leaked: {stats}")
        if kind in ("non_numeric", "nonwarp_string", "string_recursive"):
            if max(row[9] for row in stats) < 1:
                raise AssertionError(f"{kind}: no heap string rooted: {stats}")
        if args.export_probe:
            exports = sum(row[13] for row in stats)
            if kind in ("nonwarp_yield", "nonwarp_nested",
                        "loop_recursive"):
                # The very first procedure call occurs inside the root's
                # repeat. The root frame must preserve the live numeric
                # loop counter, not just the child procedure's loop PCs.
                root_counted = [
                    int(v) for v in re.findall(
                        r"root_counted_frames=(\d+)", diagnostics
                    )
                ]
                if not root_counted or max(root_counted) == 0:
                    raise AssertionError(
                        f"{kind}: missing live root repeat counter in "
                        f"exported LLVM image: {diagnostics[-1200:]}"
                    )
            if kind in ("nonwarp_yield", "root_nested_counted",
                        "root_dynamic_counted"):
                exact_sum = {
                    "nonwarp_yield": 234,
                    "root_nested_counted": 162,
                    "root_dynamic_counted": 234,
                }[kind]
                expected_depth = 2 if kind == "root_nested_counted" else 1
                actual_sums = [int(value) for value in re.findall(
                    r"root_counter_sum=(\d+)", diagnostics
                )]
                actual_depths = [int(value) for value in re.findall(
                    r"root_max_depth=(\d+)", diagnostics
                )]
                if (sum(actual_sums) != exact_sum or
                    max(actual_depths, default=0) != expected_depth):
                    raise AssertionError(
                        f"{kind}: live root counters incorrectly saved: "
                        f"sum={actual_sums} (expected {exact_sum}), "
                        f"depth={actual_depths} (expected {expected_depth}) "
                        f"{diagnostics[-1000:]}"
                    )
            if exports != yields:
                raise AssertionError(
                    f"{kind}: exported={exports}, observed yields={yields}")
            if kind in ("nonwarp_string", "string_recursive") and yields and (
                max(row[14] for row in stats) == 0
            ):
                raise AssertionError(f"{kind}: detached image lost GC roots")
            phase_entry = sum(row[22] for row in stats)
            phase_after_body = sum(row[23] for row in stats)
            phase_after_callee = sum(row[24] for row in stats)
            awaiting_frames = sum(row[25] for row in stats)
            awaiting_loops = sum(row[26] for row in stats)
            phase_unknown = sum(row[27] for row in stats)
            if (phase_entry + phase_after_body +
                phase_after_callee + phase_unknown != yields):
                raise AssertionError(
                    f"{kind}: phase accounting lost a native yield: {stats}")
            if kind in ("nonwarp_yield", "nonwarp_nested", "nonwarp_triple",
                        "empty_string_arg") and phase_after_body != yields:
                raise AssertionError(f"{kind}: expected only after-body yields: {stats}")
            if kind in ("nonwarp_recursive", "string_recursive",
                        "loop_recursive") and phase_entry == 0:
                raise AssertionError(f"{kind}: missing pre-entry recursive yield: {stats}")
            if kind in ("nonwarp_wait", "post_loop_wait") and phase_unknown == 0:
                raise AssertionError(
                    f"{kind}: wait after statement start must have UnknownBody "
                    f"phase, not an old loop/entry checkpoint: {stats}")
            if kind == "post_loop_wait" and phase_after_body == 0:
                raise AssertionError(
                    f"{kind}: expected real loop checkpoint yields too: {stats}")
            if kind in ("branch_arms", "nested_branch_arms"):
                then_arms = sum(row[28] for row in stats)
                else_arms = sum(row[29] for row in stats)
                nested_arms = sum(row[30] for row in stats)
                if then_arms == 0 or else_arms == 0 or (
                    kind == "nested_branch_arms" and nested_arms == 0
                ):
                    raise AssertionError(
                        f"{kind}: actual LLVM branch source paths not "
                        f"exported: then={then_arms} else={else_arms} "
                        f"nested={nested_arms}")
            if kind == "post_loop_call" and (
                awaiting_frames == 0 or awaiting_loops != 0
            ):
                raise AssertionError(
                    f"{kind}: completed source repeat must not remain "
                    f"in an awaiting caller path: {stats}")
            if kind == "loop_recursive":
                if max(row[15] for row in stats) < 4 or (
                    sum(row[16] for row in stats) < 1
                ) or awaiting_frames == 0 or awaiting_loops == 0:
                    raise AssertionError(
                        f"{kind}: missing recursive parent loop phase: {stats}")
        if kind in ("branch_return", "loop_return",
                    "procedure_return", "distinct_return_sites",
                    "script_return"):
            sites = sum(row[17] for row in stats)
            loop_sites = sum(row[18] for row in stats)
            branch_sites = sum(row[19] for row in stats)
            procedure_sites = sum(row[20] for row in stats)
            expected = {
                "branch_return": (2, 1, 1, 0),
                "loop_return": (2, 2, 0, 0),
                "procedure_return": (2, 1, 0, 1),
                "distinct_return_sites": (4, 2, 2, 0),
                "script_return": (2, 1, 0, 0, 1),
            }[kind]
            if kind != "script_return":
                expected = (*expected, 0)
            script_sites = sum(row[21] for row in stats)
            if (sites, loop_sites, branch_sites,
                procedure_sites, script_sites) != expected:
                raise AssertionError(
                    f"{kind}: bad unique return-site routing: "
                    f"{(sites, loop_sites, branch_sites, procedure_sites, script_sites)}, "
                    f"expected={expected}, stats={stats}")
        if kind == "nested" and max_depth < 3:
            raise AssertionError(f"{kind}: nested calls not tracked: {stats}")
        if kind in ("nonwarp_recursive", "string_recursive", "loop_recursive") and yields < 1:
            raise AssertionError(f"{kind}: recursive Fiber did not yield: {stats}")
        if kind in ("nonwarp_yield", "nonwarp_nested", "nonwarp_triple"):
            checkpoints = sum(row[2] for row in stats)
            counters = sum(row[4] for row in stats)
            counter_sum = sum(row[5] for row in stats)
            expected_yields = {
                "nonwarp_yield": 36, "nonwarp_nested": 96,
                "nonwarp_triple": 168,
            }[kind]
            expected_sum = {
                "nonwarp_yield": 72, "nonwarp_nested": 180,
                "nonwarp_triple": 252,
            }[kind]
            max_slots = max(row[6] for row in stats)
            expected_slots = {
                "nonwarp_yield": 1, "nonwarp_nested": 2,
                "nonwarp_triple": 3,
            }[kind]
            max_path_depth = max(row[10] for row in stats)
            parent_counts = sum(row[11] for row in stats)
            parent_sum = sum(row[12] for row in stats)
            expected_parent_count = {
                "nonwarp_yield": 0, "nonwarp_nested": 72,
                "nonwarp_triple": 240,
            }[kind]
            expected_parent_sum = {
                "nonwarp_yield": 0, "nonwarp_nested": 108,
                "nonwarp_triple": 360,
            }[kind]
            if (yields != expected_yields or checkpoints < yields or
                counters != expected_yields or counter_sum != expected_sum or
                max_slots != expected_slots or
                max_path_depth != expected_slots or
                parent_counts != expected_parent_count or
                parent_sum != expected_parent_sum or
                any(row[3] == 0 for row in stats)):
                raise AssertionError(
                    f"{kind}: missing source-level resume checkpoints "
                    f"at fast native loop yields: {stats}"
                )
        print(
            f"PASS {kind}: Interpreter/regular LLVM/mirrored LLVM parity; "
            f"calls={calls} returns={returns} yields={yields} "
            f"max_depth={max_depth}, enabled={enabled}",
            flush=True,
        )
    print(f"PASS {len(CASES)} native LLVM CallFrame integration programs")


if __name__ == "__main__":
    main()
