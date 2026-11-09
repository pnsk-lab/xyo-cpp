#!/usr/bin/env python3
"""Actual Scratch create-clone side effects across LLVM and WASM fallback.

Three clones execute control_start_as_clone and increment a Stage variable.
The Stage counter proves that the effects are not duplicated by compiled
hybrid rollback, while the committed queue count proves actual integration.
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
    Gen, S_change, S_set, num, make_project, write_sb3,
)


def fixture(path: Path) -> Path:
    g = Gen(["count", "done"], [])
    hat = g.script([S_set("count", num(0)),
                    S_set("done", num(0))])
    first = g.blocks[hat]["next"]
    cursor = g.blocks[first]["next"]
    for _ in range(3):
        clone = g.new("control_create_clone_of", parent=cursor)
        menu = g.new("control_create_clone_of_menu", shadow=True,
                     parent=clone,
                     fields={"CLONE_OPTION": ["_myself_", None]})
        g.blocks[clone]["inputs"]["CLONE_OPTION"] = [1, menu]
        g.blocks[cursor]["next"] = clone
        cursor = clone
    done = g.emit_stmt(S_set("done", num(4)), cursor)
    g.blocks[cursor]["next"] = done

    started = g.new("control_start_as_clone", top=True, x=500)
    update = g.emit_stmt(S_change("count", num(1)), started)
    g.blocks[started]["next"] = update
    remove = g.new("control_delete_this_clone", parent=update)
    g.blocks[update]["next"] = remove

    project = make_project(["count", "done"], [], g)
    stage, sprite = project["targets"]
    # A Stage variable is shared by every clone, unlike an ordinary sprite
    # variable which gets copied independently to each spawned clone.
    stage["variables"][g.vars["count"]] = ["count", 0]
    del sprite["variables"][g.vars["count"]]
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), project)
    return path


def run(cmd: list[str], emitter: str, journal: bool, addon: Path | None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(
        XYO_DUMPVARS="json", XYO_EXIT_ON_DONE="1",
        XYO_PAR="0", XYO_JIT_ASYNC="0",
        XYO_JIT_VERBOSE="1", XYO_HOT_INVOCATIONS="1",
        XYO_WASM_HOT_EMITTER=emitter,
        XYO_EFFECT_JOURNAL="1" if journal else "0",
        XYO_EFFECT_JOURNAL_STATS="1" if journal else "",
    )
    if addon:
        env["XYO_BINARYEN_NODE_ADDON"] = str(addon.resolve())
    result = subprocess.run(cmd, cwd=ROOT, env=env,
                            capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise AssertionError(
            f"{emitter}/{journal}: exit={result.returncode} "
            f"stdout={result.stdout[-1200:]} "
            f"stderr={result.stderr[-1200:]}")
    start = result.stdout.find("{")
    if start < 0:
        raise AssertionError(f"no output: {result.stdout[-1000:]}")
    data, _ = json.JSONDecoder().raw_decode(result.stdout[start:])
    state = {
        "count": data["Stage"]["count"],
        "done": data["Bench"]["done"],
    }
    stats = re.search(
        r"\[effect-journal\].*committed_effects=(\d+) "
        r"immediate_effects=(\d+)", result.stderr)
    if journal and not stats:
        raise AssertionError(
            "No journal statistics: " + result.stderr[-1000:])
    return state, (
        tuple(map(int, stats.groups())) if stats else None), result.stderr


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--native", required=True, type=Path)
    p.add_argument("--wasm", type=Path)
    p.add_argument("--addon", type=Path)
    p.add_argument("--out", required=True, type=Path)
    args = p.parse_args()
    source = fixture(args.out / "runtime-clone-journal.sb3")
    native = str(args.native.resolve())
    expected = {"count": "3", "done": "4"}
    for label, jit, journal in (
        ("Interpreter", "off", False),
        ("LLVM", "eager", False),
        ("Interpreter+journal", "off", True),
        ("LLVM+journal", "eager", True),
    ):
        state, stats, _ = run(
            [native, "run", str(source.resolve()),
             "--jit", jit, "--frames", "150"],
            label, journal, None)
        if state != expected or (journal and stats != (3, 0)):
            raise AssertionError(f"{label}: state={state}, stats={stats}")
        print(f"PASS {label}: {state}, committedEffects={stats}")
    if args.wasm:
        if not args.addon:
            p.error("--wasm requires --addon")
        for emitter in ("builtin", "binaryen"):
            cmd = [
                "node", str(ROOT / "web/xyo-node.mjs"),
                str(args.wasm.resolve()), "run", str(source.resolve()),
                "--jit", "hotspot", "--frames", "150",
            ]
            state, stats, log = run(cmd, emitter, True, args.addon)
            if state != expected or stats != (3, 0):
                raise AssertionError(
                    f"WASM {emitter}: state={state}, stats={stats}")
            if emitter == "binaryen" and not (
                    "emitter=binaryen-host" in log and
                    "xyo-wasm-hot: hybrid script=" in log):
                raise AssertionError(
                    "Binaryen must compile numeric regions around "
                    "interpreter-only clone effect")
            print(f"PASS WASM {emitter}+journal: {state} effects={stats}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
