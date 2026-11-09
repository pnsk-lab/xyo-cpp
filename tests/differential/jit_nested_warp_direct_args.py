#!/usr/bin/env python3
"""Read-only GC-rooted argument ABI for an in-flight native warp loop.

The script must be long enough for the asynchronous LLVM compiler to finish
before the interpreter reaches the end of the selected repeat. Distinguish
"same output" from actual OSR promotion and restored caller continuation.
"""
from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set,
    arg, make_project, num, text, write_sb3,
)
from jit_nested_warp_args_preserved import fixture
from jit_nested_warp_argroots_gc import (
    fixture as gc_fixture, MESSAGE as GC_MESSAGE, ITERATIONS as GC_ITERATIONS,
)

MESSAGE = "GC-rooted 日本語 🌠🏔️"


def mixed_fixture(path: Path, iterations: int = 8_000_000) -> Path:
    g = Gen(["prefix", "sum", "message", "suffix", "done"], [])
    g.define(
        "worker %s %s", ["表示🌠", "増分"],
        [
            S_change("prefix", arg("増分")),
            S_repeat(num(iterations), [
                S_repeat(num(3), [
                    S_change("sum", arg("増分")),
                    S_set("message", arg("表示🌠")),
                ]),
            ]),
            S_set("suffix", arg("表示🌠")),
        ],
        warp=True,
    )
    g.script([
        S_set("prefix", num(0)),
        S_set("sum", num(0)),
        S_set("message", text("")),
        S_set("suffix", text("")),
        S_set("done", num(0)),
        S_call("worker %s %s", [text(MESSAGE), num(2)]),
        S_set("done", num(1)),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(g.vars.keys(), g.lists.keys(), g))
    return path


def invalid_reporter_fixture(source: Path, destination: Path) -> Path:
    """Break only the loop's argument reporter, not the declared signature."""
    with zipfile.ZipFile(source) as original:
        files = {name: original.read(name) for name in original.namelist()}
    project = json.loads(files["project.json"])
    blocks = project["targets"][1]["blocks"]
    changed = 0
    for block in blocks.values():
        if block["opcode"] != "argument_reporter_string_number":
            continue
        parent = blocks.get(block.get("parent"))
        if not parent or parent["opcode"] != "data_changevariableby":
            continue
        if parent["fields"].get("VARIABLE", [None])[0] != "sum":
            continue
        block["fields"]["VALUE"][0] = "missing-argument"
        changed += 1
    assert changed == 1, f"expected exactly one loop reporter, got {changed}"
    files["project.json"] = json.dumps(project, ensure_ascii=False).encode()
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as result:
        for name, data in files.items():
            result.writestr(name, data)
    return destination


def execute(
    binary: Path, source: Path, mode: str, direct: bool,
    *, hoist: bool = False, block_count: bool = False,
    gc_stress: bool = False,
):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_JIT_MODE": mode,
        "XYO_JIT_ASYNC": "1",
        "XYO_JIT_OSR": "1",
        "XYO_JIT_OSR_NESTED": "1",
        "XYO_JIT_OSR_NESTED_ARG_READS": "1",
        "XYO_JIT_OSR_NESTED_DIRECT_ARGS": "1" if direct else "0",
        "XYO_JIT_OSR_NESTED_HOIST_ARGS": "1" if hoist else "0",
        "XYO_JIT_OSR_TIMING": "1",
        "XYO_JIT_OSR_NESTED_TRACE": "1",
        "XYO_EXIT_ON_DONE": "1",
        "XYO_DUMPVARS": "json",
        "XYO_PAR": "0",
        "XYO_PARFOR": "0",
        "XYO_GPU": "0",
        "XYO_BLOCK_COUNT": "1" if block_count else "0",
        "XYO_GC_THRESHOLD": "1" if gc_stress else "0",
        "XYO_GC_DEBUG": "1" if gc_stress else "0",
    })
    run = subprocess.run(
        [str(binary.resolve()), "play", str(source.resolve())],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=180,
    )
    if run.returncode:
        raise AssertionError(
            f"{source.name} {mode=} {direct=} exited {run.returncode}\n"
            + run.stderr[-2500:]
        )
    pos = run.stdout.find("{")
    if pos < 0:
        raise AssertionError("No Scratch state: " + run.stdout[-1000:])
    state, _ = json.JSONDecoder().raw_decode(run.stdout[pos:])
    return state["Bench"], run.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    numeric = args.out / "arg-numeric-native.sb3"
    fixture(numeric, use_argument_in_loop=True, iterations_override=8_000_000)
    mixed = mixed_fixture(args.out / "arg-unicode-nested-native.sb3")
    negative = invalid_reporter_fixture(
        numeric, args.out / "invalid-arg-reporter-no-osr.sb3"
    )

    for project, should_promote in (
        (numeric, True), (mixed, True), (negative, False)
    ):
        expected, _ = execute(args.native, project, "off", False)
        if project == mixed:
            assert expected["sum"] == str(8_000_000 * 3 * 2), expected
            assert expected["message"] == MESSAGE, expected
            assert expected["suffix"] == MESSAGE, expected
            assert expected["prefix"] == "2", expected
            assert expected["done"] == "1", expected
        for direct, hoist in ((False, False), (True, False), (True, True)):
            # OSR is asynchronous: on a shared CI host, LLVM may finish
            # compiling after the interpreter exits its counted loop.
            # Check semantics on each attempt and require a real promotion.
            for attempt in range(3 if should_promote else 1):
                actual, stderr = execute(
                    args.native, project, "hotspot", direct, hoist=hoist
                )
                assert actual == expected, (
                    project.name, direct, hoist, actual, expected,
                    stderr[-2500:],
                )
                promoted = "[jit-osr-nested] promoted " in stderr
                restored = "[jit-osr-nested] restored caller" in stderr
                if not should_promote or promoted or restored:
                    break
                print(
                    f"RETRY {project.name} direct={direct} hoist={hoist}: "
                    f"no asynchronous OSR (attempt {attempt + 1}/3)",
                    flush=True,
                )
            assert promoted == should_promote, (
                project.name, direct, hoist, promoted, stderr[-2500:]
            )
            assert restored == should_promote, (
                project.name, direct, hoist, restored, stderr[-2500:]
            )
            if hoist and should_promote:
                match = re.search(
                    r"\[jit-osr-nested\] hoisted-args=(\d+)", stderr
                )
                assert match and int(match.group(1)) >= 1, (
                    project.name, "no LLVM SSA argument preload", stderr[-2500:]
                )
            print(
                f"PASS {project.name} direct={direct} hoist={hoist} "
                f"promoted={promoted}: GC-rooted args, nested repeat, "
                "exact caller continuation",
                flush=True,
            )

    # A direct native argument read must count precisely one reporter just
    # like an interpreter read. It must not count again on slow fallback.
    counts = []
    for mode, direct, hoist in (
        ("off", False, False),
        ("hotspot", False, False),
        ("hotspot", True, False),
        ("hotspot", True, True),
    ):
        state, diagnostics = execute(
            args.native, numeric, mode, direct,
            hoist=hoist, block_count=True,
        )
        assert state["sum"] == "16000000", (mode, direct, state)
        match = re.search(r"\[block-count\] executed=(\d+)", diagnostics)
        assert match, (mode, direct, diagnostics[-1000:])
        counts.append(int(match.group(1)))
        print(
            f"PASS block-count {mode} direct={direct} hoist={hoist}: "
            f"{counts[-1]}", flush=True
        )
    assert len(set(counts)) == 1, counts

    # The argument string must remain rooted even while the native body
    # allocates new strings and forces GC collections.
    gc_project = args.out / "arg-direct-gc-string-join.sb3"
    gc_fixture(gc_project)
    baseline, _ = execute(
        args.native, gc_project, "off", False, gc_stress=True
    )
    assert baseline["count"] == str(GC_ITERATIONS), baseline
    assert baseline["message"] == GC_MESSAGE, baseline
    for hoist in (False, True):
        native, diagnostics = execute(
            args.native, gc_project, "hotspot", True,
            hoist=hoist, gc_stress=True,
        )
        assert native == baseline, (native, baseline, diagnostics[-1500:])
        assert "[jit-osr-nested] promoted " in diagnostics, diagnostics[-1500:]
        assert "[jit-osr-nested] restored caller" in diagnostics
        collections = len(re.findall(r"\[gc\] collect #", diagnostics))
        assert collections > 0, diagnostics[-1500:]
        if hoist:
            assert re.search(
                r"\[jit-osr-nested\] hoisted-args=[1-9]\d*", diagnostics
            ), diagnostics[-1500:]
        print(
            f"PASS native direct args hoist={hoist} GC: "
            f"collections={collections}",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
