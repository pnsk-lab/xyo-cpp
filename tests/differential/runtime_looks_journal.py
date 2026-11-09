#!/usr/bin/env python3
"""Journaled, GC-rooted say/think effects on interpreter/LLVM/WASM fallback."""
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
    Gen, S_change, S_set, join, num, text, var,
    make_project, write_sb3,
)


def fixture(path: Path) -> Path:
    g = Gen(["x", "done"], [])
    hat = g.script([S_set("x", num(4)),
                    S_set("done", num(0))])
    init = g.blocks[hat]["next"]
    current = g.blocks[init]["next"]

    say = g.new("looks_say", parent=current)
    g.blocks[say]["inputs"]["MESSAGE"] = g.emit_value(
        join(text("Hello 🌟 "), var("x")), say)
    g.blocks[current]["next"] = say
    current = say

    changed = g.emit_stmt(S_change("x", num(3)), current)
    g.blocks[current]["next"] = changed
    current = changed

    think = g.new("looks_think", parent=current)
    g.blocks[think]["inputs"]["MESSAGE"] = g.emit_value(
        join(text("数値 "), var("x")), think)
    g.blocks[current]["next"] = think
    current = think

    timed = g.new("looks_sayforsecs", parent=current)
    g.blocks[timed]["inputs"]["MESSAGE"] = g.emit_value(
        text("タイマー 🌏"), timed)
    g.blocks[timed]["inputs"]["SECS"] = g.emit_value(
        num(0.01), timed)
    g.blocks[current]["next"] = timed
    current = timed

    finished = g.emit_stmt(S_set("done", num(1)), current)
    g.blocks[current]["next"] = finished
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(["x", "done"], [], g))
    return path


def execute(cmd: list[str], emitter: str, journal: bool):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(XYO_DUMPVARS="json", XYO_PAR="0",
               XYO_EXIT_ON_DONE="1", XYO_JIT_ASYNC="0",
               XYO_WASM_HOT_EMITTER=emitter,
               XYO_JIT_VERBOSE="1", XYO_HOT_INVOCATIONS="1",
               XYO_EFFECT_JOURNAL="1" if journal else "0",
               XYO_EFFECT_JOURNAL_STATS="1" if journal else "")
    if "XYO_BINARYEN_NODE_ADDON" in os.environ:
        env["XYO_BINARYEN_NODE_ADDON"] = os.environ[
            "XYO_BINARYEN_NODE_ADDON"]
    result = subprocess.run(cmd, cwd=ROOT, env=env,
                            capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise AssertionError(
            f"{emitter}/{journal}: exit={result.returncode} "
            f"{result.stderr[-1800:]}")
    start = result.stdout.find("{")
    if start < 0:
        raise AssertionError(f"Missing JSON output: {result.stdout[-1600:]}")
    state, _ = json.JSONDecoder().raw_decode(result.stdout[start:])
    journal_match = re.search(
        r"\[effect-journal\].*committed_effects=(\d+) "
        r"immediate_effects=(\d+)", result.stderr)
    if journal and not journal_match:
        raise AssertionError("No journal stats: " + result.stderr[-800:])
    return state["Bench"], (
        tuple(map(int, journal_match.groups())) if journal_match else None
    ), result.stderr


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--native", required=True, type=Path)
    p.add_argument("--wasm", type=Path)
    p.add_argument("--addon", type=Path)
    p.add_argument("--out", required=True, type=Path)
    args = p.parse_args()
    sb3 = fixture(args.out / "runtime-looks-say-journal.sb3")
    native = str(args.native.resolve())
    expected = {"x": "7", "done": "1"}
    for name, jit, journal in (
        ("Interpreter", "off", False),
        ("LLVM", "eager", False),
        ("Interpreter+journal", "off", True),
        ("LLVM+journal", "eager", True),
    ):
        state, stat, _ = execute(
            [native, "run", str(sb3.resolve()),
             "--jit", jit, "--frames", "200"], name, journal)
        if state != expected or (
            journal and stat != (3, 0)):
            raise AssertionError(
                f"{name}: state={state} stat={stat}, expected={expected}")
        print(f"PASS {name}: {state}, effects={stat}")

    if args.wasm:
        if not args.addon:
            p.error("--wasm requires --addon")
        for emitter in ("builtin", "binaryen"):
            cmd = [
                "node", str(ROOT / "web/xyo-node.mjs"),
                str(args.wasm.resolve()), "run", str(sb3.resolve()),
                "--jit", "hotspot", "--frames", "200",
            ]
            # The WASM optimizer resolves its optional native addon via env.
            env_prev = os.environ.get("XYO_BINARYEN_NODE_ADDON")
            os.environ["XYO_BINARYEN_NODE_ADDON"] = str(args.addon.resolve())
            try:
                state, stat, stderr = execute(cmd, emitter, True)
            finally:
                if env_prev is None:
                    os.environ.pop("XYO_BINARYEN_NODE_ADDON", None)
                else:
                    os.environ["XYO_BINARYEN_NODE_ADDON"] = env_prev
            if state != expected or stat != (3, 0):
                raise AssertionError(
                    f"WASM {emitter}: state={state}, stats={stat}")
            if emitter == "binaryen" and "emitter=binaryen-host" not in stderr:
                raise AssertionError("Binaryen emitter not executed")
            print(f"PASS WASM {emitter}+journal: {state} effects={stat}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
