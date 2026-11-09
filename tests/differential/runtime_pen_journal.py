#!/usr/bin/env python3
"""Pen stamp live journal route, with shared interpreter/LLVM/WASM semantics.

The C++ platform smoke additionally checks a real RendererAPI callback and
exactly two penStamp calls; this fixture tests the actual Scratch opcode
and proves Binaryen uses interpreter regions only at the effect barriers.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import os

from runtime_looks_journal import execute, ROOT, Gen
from runtime_looks_journal import S_change, S_set, num
from runtime_looks_journal import make_project, write_sb3


def fixture(path: Path) -> Path:
    g = Gen(["x", "done"], [])
    hat = g.script([S_set("x", num(4)), S_set("done", num(0))])
    init = g.blocks[hat]["next"]
    tail = g.blocks[init]["next"]
    for _ in range(2):
        stamp = g.new("pen_stamp", parent=tail)
        g.blocks[tail]["next"] = stamp
        changed = g.emit_stmt(S_change("x", num(1)), stamp)
        g.blocks[stamp]["next"] = changed
        tail = changed
    changed = g.emit_stmt(S_change("x", num(1)), tail)
    g.blocks[tail]["next"] = changed
    finished = g.emit_stmt(S_set("done", num(1)), changed)
    g.blocks[changed]["next"] = finished
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(["x", "done"], [], g))
    return path


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--native", required=True, type=Path)
    p.add_argument("--wasm", type=Path)
    p.add_argument("--addon", type=Path)
    p.add_argument("--out", required=True, type=Path)
    args = p.parse_args()
    source = fixture(args.out / "runtime-pen-stamp-journal.sb3")
    native = str(args.native.resolve())
    expected = {"x": "7", "done": "1"}
    for name, jit, journal in (
        ("Interpreter", "off", False),
        ("LLVM", "eager", False),
        ("Interpreter+journal", "off", True),
        ("LLVM+journal", "eager", True),
    ):
        state, stats, _ = execute(
            [native, "run", str(source.resolve()),
             "--jit", jit, "--frames", "100"], name, journal)
        if state != expected or (journal and stats != (2, 0)):
            raise AssertionError(f"{name}: state={state} stats={stats}")
        print(f"PASS {name}: {state}, committedEffects={stats}")

    if args.wasm:
        if not args.addon:
            p.error("--wasm requires --addon")
        for emitter in ("builtin", "binaryen"):
            cmd = [
                "node", str(ROOT / "web/xyo-node.mjs"),
                str(args.wasm.resolve()), "run", str(source.resolve()),
                "--jit", "hotspot", "--frames", "100",
            ]
            previous = os.environ.get("XYO_BINARYEN_NODE_ADDON")
            os.environ["XYO_BINARYEN_NODE_ADDON"] = str(args.addon.resolve())
            try:
                state, stats, stderr = execute(cmd, emitter, True)
            finally:
                if previous is None:
                    os.environ.pop("XYO_BINARYEN_NODE_ADDON", None)
                else:
                    os.environ["XYO_BINARYEN_NODE_ADDON"] = previous
            if state != expected or stats != (2, 0):
                raise AssertionError(
                    f"WASM {emitter}: state={state} stats={stats}")
            if emitter == "binaryen":
                if ("emitter=binaryen-host" not in stderr or
                    "xyo-wasm-hot: hybrid script=" not in stderr):
                    raise AssertionError(
                        "Pen effect barrier was not interpreted between "
                        "real Binaryen-optimized WASM regions")
            print(f"PASS WASM {emitter}+journal: {state} effects={stats}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
