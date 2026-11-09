#!/usr/bin/env python3
"""Exactly-three-frame interpreter->LLVM warp OSR, with real promotion/return.

root script -> outer warp procedure -> inner warp procedure -> counted repeat.
Both original interpreter call frames must remain live, GC-rooted and resume
their suffix blocks exactly once after the standalone native loop finishes.
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
    Gen, S_call, S_change, S_repeat, S_set, arg, join,
    make_project, num, text, write_sb3,
)

MESSAGE = "二段階 warp のGCルート 🌌🚀"


def fixture(path: Path, *, iterations: int = 8_000_000,
            gc: bool = False, inner_warp: bool = True) -> Path:
    g = Gen([
        "outerPrefix", "innerPrefix", "sum", "message", "innerSuffix",
        "outerSuffix", "done", "scratch"
    ], [])
    body = [
        S_change("innerPrefix", arg("increment")),
        S_repeat(num(iterations), [
            S_change("sum", arg("increment")),
            S_set(
                "scratch", join(arg("内容🌠"), text("!"))
            ) if gc else S_set("message", arg("内容🌠")),
        ]),
        S_set("innerSuffix", arg("内容🌠")),
    ]
    g.define(
        "inner %s %s", ["内容🌠", "increment"],
        body, warp=inner_warp,
    )
    g.define(
        "outer %s %s", ["元文🌸", "scale"],
        [
            S_change("outerPrefix", arg("scale")),
            S_call("inner %s %s", [arg("元文🌸"), arg("scale")]),
            S_set("outerSuffix", arg("元文🌸")),
        ], warp=True,
    )
    g.script([
        S_set("outerPrefix", num(0)),
        S_set("innerPrefix", num(0)),
        S_set("sum", num(0)),
        S_set("message", text("")),
        S_set("innerSuffix", text("")),
        S_set("outerSuffix", text("")),
        S_set("scratch", text("")),
        S_set("done", num(0)),
        S_call("outer %s %s", [text(MESSAGE), num(2)]),
        S_set("done", num(1)),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(g.vars.keys(), g.lists.keys(), g))
    return path


def invalid_reporter_fixture(src: Path, destination: Path) -> Path:
    with zipfile.ZipFile(src) as original:
        files = {n: original.read(n) for n in original.namelist()}
    project = json.loads(files["project.json"])
    blocks = project["targets"][1]["blocks"]
    updated = 0
    for block in blocks.values():
        if block["opcode"] != "argument_reporter_string_number":
            continue
        parent = blocks.get(block.get("parent"))
        if parent and parent["opcode"] == "data_changevariableby":
            field = parent["fields"].get("VARIABLE")
            if field and field[0] == "sum":
                block["fields"]["VALUE"][0] = "missing-inner-argument"
                updated += 1
    assert updated == 1, updated
    files["project.json"] = json.dumps(project, ensure_ascii=False).encode()
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as target:
        for name, data in files.items():
            target.writestr(name, data)
    return destination


def execute(binary: Path, project: Path, *, mode: str = "off",
            deep: bool = False, direct: bool = False, hoist: bool = False,
            block_count: bool = False, gc: bool = False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_JIT_MODE": mode,
        "XYO_JIT_ASYNC": "1",
        "XYO_JIT_OSR": "1",
        "XYO_JIT_OSR_NESTED": "1",
        "XYO_JIT_OSR_NESTED_DEEP": "1" if deep else "0",
        "XYO_JIT_OSR_NESTED_ARG_READS": "1",
        "XYO_JIT_OSR_NESTED_DIRECT_ARGS": "1" if direct else "0",
        "XYO_JIT_OSR_NESTED_HOIST_ARGS": "1" if hoist else "0",
        "XYO_JIT_OSR_TIMING": "1",
        "XYO_JIT_OSR_NESTED_TRACE": "1",
        "XYO_EXIT_ON_DONE": "1",
        "XYO_DUMPVARS": "json",
        "XYO_PAR": "0", "XYO_PARFOR": "0", "XYO_GPU": "0",
        "XYO_BLOCK_COUNT": "1" if block_count else "0",
        "XYO_GC_THRESHOLD": "1" if gc else "0",
        "XYO_GC_DEBUG": "1" if gc else "0",
    })
    proc = subprocess.run(
        [str(binary.resolve()), "play", str(project.resolve())],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=180,
    )
    assert proc.returncode == 0, (
        project.name, mode, deep, direct, hoist,
        proc.returncode, proc.stderr[-2200:]
    )
    pos = proc.stdout.find("{")
    assert pos >= 0, (proc.stdout[-400:], proc.stderr[-1500:])
    full, _ = json.JSONDecoder().raw_decode(proc.stdout[pos:])
    return full["Bench"], proc.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=8_000_000)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    normal = fixture(args.out / "deep-two-callers.sb3",
                     iterations=args.iterations)
    rejected = invalid_reporter_fixture(
        normal, args.out / "deep-unknown-inner-arg.sb3")
    no_warp = fixture(
        args.out / "deep-inner-nonwarp.sb3",
        iterations=200_000, inner_warp=False
    )

    for project, should_promote in (
        (normal, True), (rejected, False), (no_warp, False),
    ):
        baseline, _ = execute(args.native, project)
        assert baseline["done"] == "1", baseline
        assert baseline["outerPrefix"] == "2", baseline
        assert baseline["outerSuffix"] == MESSAGE, baseline
        for direct, hoist in (
            (False, False), (True, False), (True, True)
        ):
            actual, trace = execute(
                args.native, project, mode="hotspot",
                deep=True, direct=direct, hoist=hoist,
            )
            assert actual == baseline, (
                project.name, direct, hoist, actual, baseline, trace[-2300:]
            )
            match = re.search(
                r"\[jit-osr-nested\] promoted loop=.+ depth=(\d+)", trace
            )
            promoted = bool(match)
            assert promoted == should_promote, (
                project.name, direct, hoist, trace[-3000:]
            )
            if promoted:
                assert match.group(1) == "3", trace[-3000:]
                assert "[jit-osr-nested] restored caller" in trace, trace
                if hoist:
                    assert re.search(
                        r"\[jit-osr-nested\] hoisted-args=[1-9]\d*",
                        trace
                    ), trace[-2000:]
            print(
                f"PASS {project.name} direct={direct} hoist={hoist}"
                f" promoted={promoted} (3-frame source+GC+suffix)",
                flush=True,
            )
        if project == normal:
            actual, trace = execute(
                args.native, normal, mode="hotspot", deep=False
            )
            assert actual == baseline
            assert "[jit-osr-nested] promoted " not in trace, trace[-1200:]
            print("PASS opt-in gate rejects deeper stacks", flush=True)

    # Block counting is an observable Scratch behavior. The reporter bypass
    # in both direct/SSA paths must still count exactly once at each site.
    counts = []
    for mode, direct, hoist in (
        ("off", False, False), ("hotspot", False, False),
        ("hotspot", True, False), ("hotspot", True, True),
    ):
        actual, trace = execute(
            args.native, normal, mode=mode, deep=True,
            direct=direct, hoist=hoist, block_count=True,
        )
        assert actual["sum"] == str(2 * args.iterations), actual
        count = re.search(r"\[block-count\] executed=(\d+)", trace)
        assert count, trace[-1500:]
        counts.append(int(count.group(1)))
    assert len(set(counts)) == 1, counts
    print(f"PASS deep block count: {counts}", flush=True)

    gc_proj = fixture(
        args.out / "deep-rooted-strings-gc.sb3", iterations=5_000_000, gc=True
    )
    baseline, _ = execute(args.native, gc_proj, gc=True)
    assert baseline["innerSuffix"] == MESSAGE, baseline
    for hoist in (False, True):
        actual, trace = execute(
            args.native, gc_proj, mode="hotspot",
            deep=True, direct=True, hoist=hoist, gc=True,
        )
        assert actual == baseline, (actual, baseline, trace[-1500:])
        assert re.search(
            r"\[jit-osr-nested\] promoted loop=.+ depth=3", trace
        ), trace[-1500:]
        assert "[jit-osr-nested] restored caller" in trace, trace
        collections = len(re.findall(r"\[gc\] collect #", trace))
        assert collections > 0, trace[-1200:]
        print(f"PASS GC deep hoist={hoist} collections={collections}",
              flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
