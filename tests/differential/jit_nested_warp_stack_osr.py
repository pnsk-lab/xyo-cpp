#!/usr/bin/env python3
"""Bounded LLVM OSR through 2..8 simultaneously live warp callers.

Each procedure has distinct UTF-16 argument names and a different numeric
argument value, so selecting an incorrect lexical argument frame is observable.
Checks actual promoted depth, every caller suffix, exact counts, forced GC
and strict rejection for over-limit or non-warp paths.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set, add, arg,
    join, make_project, num, text, write_sb3,
)
sys.path.insert(0, str(ROOT / "tests/differential"))
from jit_nested_warp_deep_osr import (
    execute, invalid_reporter_fixture,
)

MESSAGE = "階層別 GC rooted 日本語 🌠🍀"


def fixture(
    path: Path, *, callers: int, iterations: int = 8_000_000,
    gc: bool = False, nonwarp_layer: int | None = None,
) -> Path:
    assert 1 <= callers <= 10
    names = [f"文章{i}🌠" for i in range(callers)]
    deltas = [f"増分{i}" for i in range(callers)]
    code = [f"layer{i} %s %s" for i in range(callers)]
    variables = (["sum", "message", "scratch", "done"] +
                 [f"prefix{i}" for i in range(callers)] +
                 [f"suffix{i}" for i in range(callers)])
    g = Gen(variables, [])

    for i in reversed(range(callers)):
        if i == callers - 1:
            body = [
                S_change(f"prefix{i}", arg(deltas[i])),
                S_repeat(num(iterations), [
                    S_change("sum", arg(deltas[i])),
                    S_set(
                        "scratch", join(arg(names[i]), text("!"))
                    ) if gc else S_set("message", arg(names[i])),
                ]),
                S_set(f"suffix{i}", arg(names[i])),
            ]
        else:
            body = [
                S_change(f"prefix{i}", arg(deltas[i])),
                S_call(code[i + 1], [
                    arg(names[i]), add(arg(deltas[i]), num(1)),
                ]),
                S_set(f"suffix{i}", arg(names[i])),
            ]
        g.define(code[i], [names[i], deltas[i]], body,
                 warp=(nonwarp_layer != i))

    g.script([
        *[S_set(v, num(0)) for v in variables if v.startswith("prefix")],
        *[S_set(v, text("")) for v in variables if v.startswith("suffix")],
        S_set("sum", num(0)),
        S_set("message", text("")),
        S_set("scratch", text("")),
        S_set("done", num(0)),
        S_call(code[0], [text(MESSAGE), num(2)]),
        S_set("done", num(1)),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(g.vars.keys(), g.lists.keys(), g))
    return path


def verify_state(state: dict, callers: int, iterations: int):
    assert state["done"] == "1", state
    assert state["sum"] == str(iterations * (callers + 1)), state
    for i in range(callers):
        assert state[f"prefix{i}"] == str(i + 2), (i, state)
        assert state[f"suffix{i}"] == MESSAGE, (i, state)


def compare(
    binary: Path, project: Path, *, callers: int,
    expect_promotion: bool, modes: tuple[tuple[bool, bool], ...],
    gc: bool = False,
) -> dict:
    baseline, _ = execute(binary, project, mode="off", gc=gc)
    for direct, hoist in modes:
        state, trace = execute(
            binary, project, mode="hotspot", deep=True,
            direct=direct, hoist=hoist, gc=gc,
        )
        assert state == baseline, (
            project.name, callers, direct, hoist, baseline, state,
            trace[-2500:],
        )
        match = re.search(
            r"\[jit-osr-nested\] promoted loop=.+ depth=(\d+)",
            trace,
        )
        assert bool(match) == expect_promotion, (
            project.name, callers, direct, hoist,
            "incorrect OSR adoption", trace[-2500:],
        )
        if match:
            assert int(match.group(1)) == callers + 1, trace[-2500:]
            assert "[jit-osr-nested] restored caller" in trace, trace[-2500:]
            if hoist:
                assert re.search(
                    r"\[jit-osr-nested\] hoisted-args=[1-9]\d*", trace
                ), trace[-2500:]
        if gc:
            collections = len(re.findall(r"\[gc\] collect #", trace))
            assert collections > 0, trace[-2500:]
            print(f"  GC={collections}", flush=True)
        print(
            f"PASS caller-depth={callers} {project.name} "
            f"direct={direct} hoist={hoist} "
            f"promoted={bool(match)}",
            flush=True,
        )
    return baseline


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=8_000_000)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    # Deep nested warps, with different names/values in each lexical frame.
    # Depth 9 is the current upper bound: eight procedure calls plus loop.
    for callers in (3, 5, 7, 8):
        project = fixture(args.out / f"stack-{callers}.sb3",
                          callers=callers, iterations=args.iterations)
        state = compare(
            args.native, project, callers=callers,
            expect_promotion=True, modes=((False, False),
                                          (True, False), (True, True)),
        )
        verify_state(state, callers, args.iterations)
        if callers == 3:
            off, trace = execute(
                args.native, project, mode="hotspot", deep=False
            )
            assert off == state
            assert "[jit-osr-nested] promoted " not in trace
            print("PASS deep flag disabled: no 4-frame promotion", flush=True)

    # Distinct non-warp caller midway and a stack deeper than the
    # configured eight-call bound must never be admitted.
    for label, n, nonwarp in (
        ("nonwarp-middle", 4, 2),
        ("over-eight", 9, None),
    ):
        project = fixture(
            args.out / f"{label}.sb3", callers=n, iterations=250_000,
            nonwarp_layer=nonwarp,
        )
        state = compare(
            args.native, project, callers=n,
            expect_promotion=False,
            modes=((False, False), (True, True)),
        )
        verify_state(state, n, 250_000)

    negative_root = fixture(
        args.out / "invalid-inner-reporter-source.sb3",
        callers=5, iterations=250_000,
    )
    negative = invalid_reporter_fixture(
        negative_root, args.out / "invalid-inner-reporter.sb3"
    )
    compare(args.native, negative, callers=5,
            expect_promotion=False, modes=((False, False), (True, True)))

    # Executed block accounting must not depend on reporter fallback,
    # native direct read or SSA hoisting.
    counter_proj = args.out / "stack-5.sb3"
    counts = []
    for mode, direct, hoist in (
        ("off", False, False), ("hotspot", False, False),
        ("hotspot", True, False), ("hotspot", True, True),
    ):
        state, trace = execute(
            args.native, counter_proj, mode=mode, deep=True,
            direct=direct, hoist=hoist, block_count=True,
        )
        verify_state(state, 5, args.iterations)
        match = re.search(r"\[block-count\] executed=(\d+)", trace)
        assert match, trace[-1400:]
        counts.append(int(match.group(1)))
    assert len(set(counts)) == 1, counts
    print("PASS five-caller exact block counts:", counts, flush=True)

    gc_proj = fixture(
        args.out / "stack-8-gc-utf16.sb3",
        callers=8, iterations=5_000_000, gc=True,
    )
    baseline = compare(
        args.native, gc_proj, callers=8,
        expect_promotion=True, modes=((True, False), (True, True)),
        gc=True,
    )
    verify_state(baseline, 8, 5_000_000)
    assert baseline["scratch"] == MESSAGE + "!", baseline
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
