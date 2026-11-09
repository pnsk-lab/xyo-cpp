#!/usr/bin/env python3
"""Interpreter root/procedure repeat around a warp call with LLVM inner-loop OSR.

Parent repeat counter belongs to the original StackFrame and MUST be reused
across the native handoff, even if its TIMES variable mutates in the body.
The native code executes only the remaining *inner* repeat; all parent latches
are processed by the interpreter. Real promotion/return is mandatory.
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
    Gen, S_call, S_change, S_if, S_repeat, S_set, add, arg,
    join, make_project, num, op, text, var, write_sb3,
)

MESSAGE = "外側repeat GC付き 🌠🍀"
INNER_CALL = "inner %s %s"
OUTER_CALL = "outer %s %s"


def fixture(
    path: Path, *,
    parent_count: int = 3,
    inner_iterations: int = 8_000_000,
    nested_parents: int = 1,
    inside_warp: bool = False,
    branch: bool = False,
    mutate_times: bool = True,
    inner_warp: bool = True,
    gc: bool = False,
):
    assert nested_parents in (1, 2)
    vars_ = [
        "times", "sum", "entered", "afterInner", "outerSuffix",
        "innerSuffix", "message", "scratch", "done", "branchCount",
    ]
    g = Gen(vars_, [])

    inner_body = [S_change("sum", arg("increment"))]
    if gc:
        inner_body.append(
            S_set("scratch", join(arg("内容🌠"), text("!")))
        )
    else:
        inner_body.append(S_set("message", arg("内容🌠")))
    g.define(INNER_CALL, ["内容🌠", "increment"], [
        S_repeat(num(inner_iterations), inner_body),
        S_set("innerSuffix", arg("内容🌠")),
    ], warp=inner_warp)

    parent_actions = [
        S_change("entered", num(1)),
        S_set("times", num(17)) if mutate_times
        else S_set("times", num(parent_count)),
        S_call(INNER_CALL, [arg("元文🌸"), arg("外値")])
        if inside_warp else S_call(
            INNER_CALL, [text(MESSAGE), num(2)]
        ),
        S_change("afterInner", num(1)),
    ]
    if branch:
        condition = op(
            "operator_lt", OPERAND1=num(1), OPERAND2=num(2)
        )
        parent_actions = [S_if(condition, parent_actions)]
        parent_actions.append(S_change("branchCount", num(1)))

    body = parent_actions
    for i in range(nested_parents):
        amount = var("times") if i == nested_parents - 1 else num(2)
        body = [S_repeat(amount, body)]
    if inside_warp:
        g.define(OUTER_CALL, ["元文🌸", "外値"], [
            *body, S_set("outerSuffix", arg("元文🌸")),
        ], warp=True)
        outer = [S_call(OUTER_CALL, [text(MESSAGE), num(2)])]
    else:
        outer = body
    g.script([
        S_set("times", num(parent_count)),
        S_set("sum", num(0)),
        S_set("entered", num(0)),
        S_set("afterInner", num(0)),
        S_set("outerSuffix", text("")),
        S_set("innerSuffix", text("")),
        S_set("message", text("")),
        S_set("scratch", text("")),
        S_set("done", num(0)),
        S_set("branchCount", num(0)),
        *outer,
        S_set("done", num(1)),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(g.vars.keys(), g.lists.keys(), g))
    return path


def execute(binary: Path, project: Path, *, mode: str = "off",
            parent: bool = True, direct: bool = True,
            hoist: bool = True, branch: bool = False,
            gc: bool = False, count: bool = False):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen", "SDL_AUDIODRIVER": "dummy",
        "XYO_JIT_MODE": mode, "XYO_JIT_ASYNC": "1",
        "XYO_JIT_OSR": "1", "XYO_JIT_OSR_NESTED": "1",
        "XYO_JIT_OSR_NESTED_DEEP": "1",
        "XYO_JIT_OSR_NESTED_BRANCH": "1" if branch else "0",
        "XYO_JIT_OSR_NESTED_PARENT_REPEAT": "1" if parent else "0",
        "XYO_JIT_OSR_NESTED_ARG_READS": "1",
        "XYO_JIT_OSR_NESTED_DIRECT_ARGS": "1" if direct else "0",
        "XYO_JIT_OSR_NESTED_HOIST_ARGS": "1" if hoist else "0",
        "XYO_JIT_OSR_TIMING": "1", "XYO_JIT_OSR_NESTED_TRACE": "1",
        "XYO_EXIT_ON_DONE": "1", "XYO_DUMPVARS": "json",
        "XYO_PAR": "0", "XYO_PARFOR": "0", "XYO_GPU": "0",
        "XYO_BLOCK_COUNT": "1" if count else "0",
        "XYO_GC_THRESHOLD": "1" if gc else "0",
        "XYO_GC_DEBUG": "1" if gc else "0",
    })
    result = subprocess.run(
        [str(binary.resolve()), "play", str(project.resolve())],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=210,
    )
    assert result.returncode == 0, (
        project.name, mode, result.returncode,
        result.stderr[-2800:], result.stdout[-350:],
    )
    offset = result.stdout.find("{")
    assert offset >= 0, result.stdout[-700:]
    full, _ = json.JSONDecoder().raw_decode(result.stdout[offset:])
    return full["Bench"], result.stderr


def verify(state: dict, parent_count: int,
           inner_iterations: int, *, nested_parents: int = 1,
           gc: bool = False, inside_warp: bool = False, branch: bool = False):
    executions = parent_count * (2 if nested_parents == 2 else 1)
    assert state["done"] == "1", state
    assert state["sum"] == str(
        executions * inner_iterations * 2
    ), state
    assert state["entered"] == str(executions), state
    assert state["afterInner"] == str(executions), state
    assert state["innerSuffix"] == MESSAGE, state
    assert state["outerSuffix"] == (MESSAGE if inside_warp else ""), state
    if branch:
        assert state["branchCount"] == str(executions), state
    if gc:
        assert state["scratch"] == MESSAGE + "!", state
    else:
        assert state["message"] == MESSAGE, state


def compare(binary: Path, project: Path, *,
            parent_count: int = 3, inner_iterations: int = 8_000_000,
            nested_parents: int = 1,
            branch: bool = False, gc: bool = False,
            inside_warp: bool = False,
            expect_promotion: bool = True,
            modes=((True, True),)):
    baseline, _ = execute(binary, project, mode="off", gc=gc)
    verify(baseline, parent_count, inner_iterations,
           nested_parents=nested_parents, branch=branch,
           inside_warp=inside_warp, gc=gc)
    for direct, hoist in modes:
        current, trace = execute(
            binary, project, mode="hotspot",
            parent=True, direct=direct, hoist=hoist,
            branch=branch, gc=gc,
        )
        assert current == baseline, (
            project.name, direct, hoist, current, baseline,
            trace[-2500:],
        )
        promoted = re.findall(
            r"\[jit-osr-nested\] promoted loop=.+ remaining=.+ depth=(\d+)",
            trace,
        )
        assert bool(promoted) == expect_promotion, (
            project.name, direct, hoist, trace[-2200:]
        )
        if promoted:
            assert all(int(x) >= 3 for x in promoted), trace[-2200:]
            assert len(re.findall(
                r"\[jit-osr-nested\] restored caller", trace
            )) == len(promoted), trace[-2200:]
        collections = len(re.findall(r"\[gc\] collect #", trace))
        if gc:
            assert collections > 0, trace[-2200:]
        print(
            f"PASS {project.name} direct={direct} hoist={hoist}"
            f" promotions={len(promoted)} GC={collections}",
            flush=True,
        )
    return baseline


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=8_000_000)
    a = parser.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)

    root = fixture(
        a.out / "root-dynamic-times.sb3",
        inner_iterations=a.iterations,
    )
    compare(
        a.native, root, inner_iterations=a.iterations,
        modes=((False, False), (True, False), (True, True)),
    )
    base, _ = execute(a.native, root)
    disabled, trace = execute(
        a.native, root, mode="hotspot", parent=False
    )
    assert disabled == base and "[jit-osr-nested] promoted " not in trace
    print("PASS parent repeat opt-in disabled", flush=True)

    nested = fixture(
        a.out / "root-two-parent-repeats.sb3",
        nested_parents=2, parent_count=2,
        inner_iterations=a.iterations,
    )
    compare(
        a.native, nested, parent_count=2,
        nested_parents=2, inner_iterations=a.iterations,
    )

    warp = fixture(
        a.out / "warp-parent-repeat.sb3",
        inside_warp=True, inner_iterations=a.iterations,
    )
    compare(
        a.native, warp, inside_warp=True,
        inner_iterations=a.iterations,
    )

    with_if = fixture(
        a.out / "parent-repeat-with-if.sb3", branch=True,
        inner_iterations=a.iterations,
    )
    compare(
        a.native, with_if, branch=True, inner_iterations=a.iterations,
    )

    counts = []
    for mode, direct, hoist in (
        ("off", False, False), ("hotspot", False, False),
        ("hotspot", True, False), ("hotspot", True, True),
    ):
        current, trace = execute(
            a.native, root, mode=mode,
            direct=direct, hoist=hoist, count=True,
        )
        verify(current, 3, a.iterations)
        match = re.search(r"\[block-count\] executed=(\d+)", trace)
        assert match, trace[-1600:]
        counts.append(int(match.group(1)))
    assert len(set(counts)) == 1, counts
    print("PASS exact parent loop block counts:", counts, flush=True)

    gc_proj = fixture(
        a.out / "parent-loop-gc-utf16.sb3",
        inner_iterations=5_000_000, gc=True,
    )
    compare(
        a.native, gc_proj, inner_iterations=5_000_000,
        gc=True, modes=((True, False), (True, True)),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
