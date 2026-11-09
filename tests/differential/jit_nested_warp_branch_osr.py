#!/usr/bin/env python3
"""LLVM OSR with verified IF/IF-ELSE interpreter control frames.

Two retained warp callers, with IF/ELSE frames before a call and before the
selected inner repeat. The real interpreter stack, lexical branch arm and
innermost GC-rooted argument frame must match the compiled native region.
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
    Gen, S_call, S_change, S_if, S_ifelse, S_repeat, S_set, add, arg,
    join, make_project, num, op, text, write_sb3,
)

MESSAGE = "条件分岐とGCの引数 🌌🍀"


def conditional(then: list, other: list, arm: int):
    assert arm in (1, 2)
    cond = op(
        "operator_lt", OPERAND1=num(1 if arm == 1 else 2),
        OPERAND2=num(2 if arm == 1 else 1)
    )
    return S_ifelse(cond, then, other)


def fixture(path: Path, *, outer_arm: int = 1, inner_arm: int = 1,
            iterations: int = 8_000_000,
            gc: bool = False, middle_warp: bool = True,
            branch_inside: bool = True,
            inner_if_only: bool = False) -> Path:
    g = Gen(["sum", "prefix0", "prefix1", "suffix0", "suffix1",
             "message", "scratch", "done", "untaken"], [])

    body = [S_change("sum", arg("加算🌠")),
            S_set("scratch", join(arg("本文🍀"), text("!")))
            if gc else S_set("message", arg("本文🍀"))]
    repeat = S_repeat(num(iterations), body)
    inner_statements = [
        S_change("prefix1", arg("加算🌠")),
        S_if(
            op("operator_lt", OPERAND1=num(1), OPERAND2=num(2)),
            [repeat]
        ) if inner_if_only else conditional(
            [repeat], [S_set("untaken", num(1))], 1
        ) if (branch_inside and inner_arm == 1) else (
            conditional(
                [S_set("untaken", num(1))], [repeat], 2
            ) if branch_inside else repeat
        ),
        S_set("suffix1", arg("本文🍀")),
    ]
    g.define(
        "inner %s %s", ["本文🍀", "加算🌠"], inner_statements,
        warp=middle_warp,
    )
    child = S_call("inner %s %s", [
        arg("外文🌸"), add(arg("外値"), num(1))
    ])
    outer_statements = [
        S_change("prefix0", arg("外値")),
        conditional(
            [child], [S_set("untaken", num(1))], 1
        ) if outer_arm == 1 else conditional(
            [S_set("untaken", num(1))], [child], 2
        ),
        S_set("suffix0", arg("外文🌸")),
    ]
    g.define("outer %s %s", ["外文🌸", "外値"],
             outer_statements, warp=True)

    g.script([
        S_set("sum", num(0)),
        S_set("prefix0", num(0)),
        S_set("prefix1", num(0)),
        S_set("suffix0", text("")),
        S_set("suffix1", text("")),
        S_set("message", text("")),
        S_set("scratch", text("")),
        S_set("untaken", num(0)),
        S_set("done", num(0)),
        S_call("outer %s %s", [text(MESSAGE), num(2)]),
        S_set("done", num(1)),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(g.vars.keys(), g.lists.keys(), g))
    return path


def invalid_reporter_fixture(source: Path, destination: Path) -> Path:
    with zipfile.ZipFile(source) as src:
        files = {name: src.read(name) for name in src.namelist()}
    project = json.loads(files["project.json"])
    blocks = project["targets"][1]["blocks"]
    changed = 0
    for block in blocks.values():
        if block["opcode"] != "argument_reporter_string_number":
            continue
        parent = blocks.get(block.get("parent"))
        if not parent or parent["opcode"] != "data_changevariableby":
            continue
        if parent["fields"].get("VARIABLE", [None])[0] == "sum":
            block["fields"]["VALUE"][0] = "invalid-inner-arg"
            changed += 1
    assert changed == 1, changed
    files["project.json"] = json.dumps(
        project, ensure_ascii=False
    ).encode("utf-8")
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as out:
        for name, content in files.items():
            out.writestr(name, content)
    return destination


def execute(binary: Path, project: Path, *,
            mode: str = "off", branch: bool = False,
            direct: bool = False, hoist: bool = False,
            gc: bool = False, count: bool = False):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen", "SDL_AUDIODRIVER": "dummy",
        "XYO_JIT_MODE": mode,
        "XYO_JIT_ASYNC": "1", "XYO_JIT_OSR": "1",
        "XYO_JIT_OSR_NESTED": "1",
        "XYO_JIT_OSR_NESTED_DEEP": "1",
        "XYO_JIT_OSR_NESTED_BRANCH": "1" if branch else "0",
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
    proc = subprocess.run(
        [str(binary.resolve()), "play", str(project.resolve())],
        cwd=ROOT, capture_output=True, text=True, env=env, timeout=240,
    )
    assert proc.returncode == 0, (project, proc.returncode,
                                  proc.stderr[-2000:])
    offset = proc.stdout.find("{")
    assert offset >= 0, proc.stdout[-1000:]
    whole, _ = json.JSONDecoder().raw_decode(proc.stdout[offset:])
    return whole["Bench"], proc.stderr


def verify(state: dict, iterations: int, *, gc: bool = False):
    assert state["sum"] == str(iterations * 3), state
    assert state["prefix0"] == "2", state
    assert state["prefix1"] == "3", state
    assert state["suffix0"] == state["suffix1"] == MESSAGE, state
    assert state["done"] == "1" and state["untaken"] == "0", state
    if gc:
        assert state["scratch"] == MESSAGE + "!", state
    else:
        assert state["message"] == MESSAGE, state


def compare(binary: Path, project: Path, iterations: int, *,
            promote: bool, modes: tuple[tuple[bool, bool], ...],
            gc: bool = False, depth: int = 5,
            invalid_argument: bool = False) -> dict:
    baseline, _ = execute(binary, project, gc=gc)
    if invalid_argument:
        # Scratch's missing numeric argument evaluates to 0. The expected
        # result changes; refusal still compares the entire interpreter
        # state with the JIT path, rather than assuming valid-input output.
        assert baseline["sum"] == "0", baseline
        assert baseline["done"] == "1" and baseline["untaken"] == "0"
        assert baseline["suffix0"] == baseline["suffix1"] == MESSAGE
    else:
        verify(baseline, iterations, gc=gc)
    for direct, hoist in modes:
        actual, stderr = execute(
            binary, project, mode="hotspot",
            branch=True, direct=direct, hoist=hoist, gc=gc,
        )
        assert actual == baseline, (
            project.name, direct, hoist, actual, baseline, stderr[-2200:]
        )
        promoted = re.search(
            r"\[jit-osr-nested\] promoted loop=.+ depth=(\d+)",
            stderr,
        )
        assert bool(promoted) == promote, (
            project.name, direct, hoist, stderr[-3000:]
        )
        if promote:
            assert int(promoted.group(1)) == depth, stderr[-3000:]
            assert "[jit-osr-nested] restored caller" in stderr, stderr
            if hoist:
                assert re.search(
                    r"\[jit-osr-nested\] hoisted-args=[1-9]\d*",
                    stderr,
                ), stderr[-2500:]
        collections = len(re.findall(r"\[gc\] collect #", stderr))
        if gc:
            assert collections > 0, stderr[-1500:]
        print(
            f"PASS {project.name} direct={direct} hoist={hoist}"
            f" promoted={bool(promoted)} gc={collections}", flush=True,
        )
    return baseline


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=8_000_000)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    for outer_arm, inner_arm in ((1, 1), (2, 1), (1, 2), (2, 2)):
        project = fixture(
            args.out / f"branch-{outer_arm}-{inner_arm}.sb3",
            outer_arm=outer_arm, inner_arm=inner_arm,
            iterations=args.iterations,
        )
        compare(
            args.native, project, args.iterations,
            promote=True,
            modes=((False, False), (True, False), (True, True))
            if (outer_arm, inner_arm) == (1, 1)
            else ((True, True),),
        )
        if (outer_arm, inner_arm) == (1, 1):
            baseline, _ = execute(args.native, project)
            state, stderr = execute(
                args.native, project, mode="hotspot", branch=False
            )
            assert state == baseline
            assert "[jit-osr-nested] promoted " not in stderr
            print("PASS branch gate disabled", flush=True)

    # Single-arm control_if uses the same source frame / return semantics.
    single_if = fixture(
        args.out / "control-if-then.sb3",
        iterations=args.iterations, inner_if_only=True,
    )
    compare(
        args.native, single_if, args.iterations,
        promote=True, modes=((True, False), (True, True)),
    )

    # Safety refusal: an unknown procedure argument and a non-warp child.
    good = fixture(
        args.out / "negative-source.sb3", iterations=300_000
    )
    invalid = invalid_reporter_fixture(
        good, args.out / "invalid-argument.sb3"
    )
    no_warp = fixture(
        args.out / "nonwarp-child.sb3",
        iterations=300_000, middle_warp=False,
    )
    for negative in (invalid, no_warp):
        compare(
            args.native, negative, 300_000, promote=False,
            modes=((False, False), (True, True)),
            invalid_argument=(negative == invalid),
        )

    # No branch after the innermost call: 4 live frames, still root args.
    without_inner = fixture(
        args.out / "branch-only-at-outer.sb3",
        iterations=args.iterations, branch_inside=False
    )
    compare(
        args.native, without_inner, args.iterations,
        promote=True, modes=((True, False), (True, True)), depth=4,
    )

    counter = args.out / "branch-1-1.sb3"
    counts = []
    for mode, direct, hoist in (
        ("off", False, False), ("hotspot", False, False),
        ("hotspot", True, False), ("hotspot", True, True),
    ):
        state, stderr = execute(
            args.native, counter, mode=mode, branch=True,
            direct=direct, hoist=hoist, count=True,
        )
        verify(state, args.iterations)
        match = re.search(r"\[block-count\] executed=(\d+)", stderr)
        assert match, stderr[-1500:]
        counts.append(int(match.group(1)))
    assert len(set(counts)) == 1, counts
    print("PASS branch block count:", counts, flush=True)

    gc_project = fixture(
        args.out / "branch-gc-string.sb3",
        iterations=5_000_000, gc=True, outer_arm=2, inner_arm=2,
    )
    compare(
        args.native, gc_project, 5_000_000, promote=True,
        modes=((True, False), (True, True)), gc=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
