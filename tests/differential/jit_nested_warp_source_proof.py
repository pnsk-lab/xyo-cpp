#!/usr/bin/env python3
"""OSR source-PC/CallFrame interoperability against actual live Scratch.

The optional source proof does NOT enable arbitrary deopt. Its independent
canonical IR-001 dictionary must accept exactly the path actually executed
by the interpreter: calls + return PCs, branch arms and counted outer loops.
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
sys.path.insert(0, str(Path(__file__).parent))
from jit_nested_warp_parent_repeat_osr import fixture as parent_fixture
from jit_nested_warp_branch_osr import fixture as branch_fixture
from jit_nested_warp_stack_osr import fixture as stack_fixture
from jit_nested_warp_direct_args import fixture as legacy_fixture

PROOF = re.compile(
    r"\[jit-osr-source-proof\] calls=(\d+) parent-loops=(\d+) "
    r"branches=(\d+) leaf-pc=(\d+) revision=(\d+)"
)
PROMOTION = re.compile(
    r"\[jit-osr-nested\] promoted loop=.+ remaining=.+ depth=(\d+)"
)


def execute(native: Path, project: Path, *, mode: str,
            proof: bool = True, deep: bool = True,
            parent: bool = False, branch: bool = False,
            hoist: bool = True, gc: bool = False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_JIT_MODE": mode,
        "XYO_JIT_ASYNC": "1",
        "XYO_JIT_OSR": "1",
        "XYO_JIT_OSR_NESTED": "1",
        "XYO_JIT_OSR_NESTED_DEEP": "1" if deep else "0",
        "XYO_JIT_OSR_NESTED_PARENT_REPEAT": "1" if parent else "0",
        "XYO_JIT_OSR_NESTED_BRANCH": "1" if branch else "0",
        "XYO_JIT_OSR_NESTED_ARG_READS": "1",
        "XYO_JIT_OSR_NESTED_DIRECT_ARGS": "1",
        "XYO_JIT_OSR_NESTED_HOIST_ARGS": "1" if hoist else "0",
        "XYO_JIT_OSR_NESTED_SOURCE_PROOF": "1" if proof else "0",
        "XYO_JIT_OSR_TIMING": "1",
        "XYO_JIT_OSR_NESTED_TRACE": "1",
        "XYO_EXIT_ON_DONE": "1",
        "XYO_DUMPVARS": "json",
        "XYO_PAR": "0",
        "XYO_PARFOR": "0",
        "XYO_GPU": "0",
        "XYO_GC_THRESHOLD": "1" if gc else "0",
        "XYO_GC_DEBUG": "1" if gc else "0",
    })
    proc = subprocess.run(
        [str(native.resolve()), "play", str(project.resolve())],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=240,
    )
    assert proc.returncode == 0, (
        project.name, mode, proc.returncode, proc.stderr[-2800:]
    )
    pos = proc.stdout.find("{")
    assert pos >= 0, proc.stdout[-600:]
    state, _ = json.JSONDecoder().raw_decode(proc.stdout[pos:])
    return state["Bench"], proc.stderr


def verify(
    native: Path, project: Path, *,
    calls: int, parents: int, branches: int,
    promotions: int, deep: bool = True,
    parent: bool = False, branch: bool = False,
    gc: bool = False,
):
    baseline, _ = execute(
        native, project, mode="off", proof=False,
        deep=deep, parent=parent, branch=branch, gc=gc
    )
    actual, trace = execute(
        native, project, mode="hotspot", proof=True,
        deep=deep, parent=parent, branch=branch, gc=gc
    )
    assert actual == baseline, (
        project.name, actual, baseline, trace[-2000:]
    )
    native_promotions = PROMOTION.findall(trace)
    proofs = PROOF.findall(trace)
    assert len(native_promotions) == len(proofs) == promotions, (
        project.name, "native/proof count mismatch",
        len(native_promotions), len(proofs), trace[-3500:]
    )
    for matched in proofs:
        assert (int(matched[0]), int(matched[1]), int(matched[2])) == (
            calls, parents, branches
        ), (project.name, matched, trace[-2000:])
        assert int(matched[3]) >= 1 and int(matched[4]) >= 1
    assert len(re.findall(
        r"\[jit-osr-nested\] restored caller", trace
    )) == promotions, trace[-2500:]
    if gc:
        assert "[gc] collect #" in trace, trace[-1500:]
    print(
        f"PASS {project.name} canonical calls={calls} "
        f"parent-loops={parents} branches={branches} "
        f"real-promotions={promotions} gc={gc}",
        flush=True,
    )


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--native", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--iterations", type=int, default=8_000_000)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)

    root = parent_fixture(
        a.out / "proof-root-dynamic-times.sb3",
        inner_iterations=a.iterations,
    )
    verify(
        a.native, root, calls=1, parents=1, branches=0,
        promotions=3, parent=True,
    )
    nested = parent_fixture(
        a.out / "proof-root-double-repeat.sb3",
        parent_count=2, nested_parents=2,
        inner_iterations=a.iterations,
    )
    verify(
        a.native, nested, calls=1, parents=2, branches=0,
        promotions=4, parent=True,
    )
    warp_parent = parent_fixture(
        a.out / "proof-warp-parent-repeat.sb3",
        inside_warp=True, inner_iterations=a.iterations,
    )
    verify(
        a.native, warp_parent, calls=2, parents=1, branches=0,
        promotions=3, parent=True,
    )
    root_branch = parent_fixture(
        a.out / "proof-repeat-with-if.sb3",
        branch=True, inner_iterations=a.iterations,
    )
    verify(
        a.native, root_branch, calls=1, parents=1, branches=1,
        promotions=3, parent=True, branch=True,
    )
    branches = branch_fixture(
        a.out / "proof-if-else-2-2.sb3",
        outer_arm=2, inner_arm=2, iterations=a.iterations,
    )
    verify(
        a.native, branches, calls=2, parents=0, branches=2,
        promotions=1, branch=True,
    )
    stack = stack_fixture(
        a.out / "proof-eight-warp-callers.sb3",
        callers=8, iterations=a.iterations,
    )
    verify(
        a.native, stack, calls=8, parents=0, branches=0,
        promotions=1,
    )
    legacy = a.out / "proof-legacy-root-warp.sb3"
    legacy_fixture(
        legacy, use_argument_in_loop=True,
        iterations_override=a.iterations,
    )
    verify(
        a.native, legacy, calls=1, parents=0, branches=0,
        promotions=1, deep=False,
    )
    rooted = parent_fixture(
        a.out / "proof-rooted-utf16-gc.sb3",
        inner_iterations=5_000_000, gc=True,
    )
    verify(
        a.native, rooted, calls=1, parents=1, branches=0,
        promotions=3, parent=True, gc=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
