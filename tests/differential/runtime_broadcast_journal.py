#!/usr/bin/env python3
"""Real LLVM JIT -> Runtime::startBroadcast -> CommitOnceEffects parity.

A sender starts the 'go' receiver, which in turn emits 'nested'. Every
receiver must run exactly once. The opt-in journal must preserve Scratch
observable ordering and keep all GC payload handles released at completion.
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
from gen_algo_bench import Gen, S_change, S_set, num, make_project, write_sb3


def append_broadcast(g: Gen, after: str, message: str) -> str:
    block = g.new("event_broadcast", parent=after)
    g.blocks[block]["inputs"]["BROADCAST_INPUT"] = [
        1, [11, message, "id_" + message]]
    g.blocks[after]["next"] = block
    return block


def fixture(path: Path) -> Path:
    gen = Gen(["events", "done"], [])
    sender = gen.script([S_set("events", num(0)),
                         S_set("done", num(0))])
    init = gen.blocks[sender]["next"]
    second_init = gen.blocks[init]["next"]
    send = append_broadcast(gen, second_init, "go")
    done = gen.emit_stmt(S_set("done", num(1)), send)
    gen.blocks[send]["next"] = done

    first = gen.script([S_change("events", num(1))], x=300)
    gen.blocks[first]["opcode"] = "event_whenbroadcastreceived"
    gen.blocks[first]["fields"]["BROADCAST_OPTION"] = ["go", "id_go"]
    append_broadcast(gen, gen.blocks[first]["next"], "nested")

    second = gen.script([S_change("events", num(10))], x=600)
    gen.blocks[second]["opcode"] = "event_whenbroadcastreceived"
    gen.blocks[second]["fields"]["BROADCAST_OPTION"] = [
        "nested", "id_nested"]

    project = make_project(["events", "done"], [], gen)
    project["targets"][0]["broadcasts"].update({
        "id_go": "go", "id_nested": "nested"})
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), project)
    return path


def execute(binary: Path, source: Path, jit: str, journal: bool):
    env = dict(os.environ)
    env.update(XYO_DUMPVARS="json", XYO_EXIT_ON_DONE="1",
               XYO_PAR="0", XYO_JIT_ASYNC="0",
               XYO_EFFECT_JOURNAL="1" if journal else "0",
               XYO_EFFECT_JOURNAL_STATS="1" if journal else "",
               XYO_JIT_VERBOSE="1")
    proc = subprocess.run(
        [str(binary.resolve()), "run", str(source.resolve()), "--jit", jit,
         "--frames", "100"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=90)
    if proc.returncode:
        raise AssertionError(f"jit={jit} journal={journal}: "
                             f"{proc.returncode} {proc.stderr[-1600:]}")
    start = proc.stdout.find("{")
    if start < 0:
        raise AssertionError("No dumped Bench state")
    data, _ = json.JSONDecoder().raw_decode(proc.stdout[start:])
    state = data["Bench"]
    match = re.search(
        r"\[effect-journal\] committed_broadcasts=(\d+) "
        r"immediate_broadcasts=(\d+) pending=(\d+) rooted=(\d+)",
        proc.stderr)
    return state, tuple(map(int, match.groups())) if match else None, proc.stderr


def execute_wasm(runtime: Path, addon: Path, source: Path, emitter: str):
    env = dict(os.environ)
    env.update(XYO_DUMPVARS="json", XYO_EXIT_ON_DONE="1",
               XYO_PAR="0", XYO_JIT_ASYNC="0", XYO_HOT_INVOCATIONS="1",
               XYO_EFFECT_JOURNAL="1", XYO_EFFECT_JOURNAL_STATS="1",
               XYO_JIT_VERBOSE="1", XYO_WASM_HOT_EMITTER=emitter,
               XYO_BINARYEN_NODE_ADDON=str(addon.resolve()))
    proc = subprocess.run(
        ["node", str(ROOT / "web/xyo-node.mjs"), str(runtime.resolve()),
         "run", str(source.resolve()), "--jit", "hotspot",
         "--frames", "100"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=90)
    if proc.returncode:
        raise AssertionError(
            f"WASM {emitter}: {proc.returncode} {proc.stderr[-1600:]}")
    start = proc.stdout.find("{")
    if start < 0:
        raise AssertionError(f"WASM {emitter}: missing JSON")
    data, _ = json.JSONDecoder().raw_decode(proc.stdout[start:])
    stats = re.search(
        r"\[effect-journal\] committed_broadcasts=(\d+) "
        r"immediate_broadcasts=(\d+) pending=(\d+) rooted=(\d+)",
        proc.stderr)
    if not stats:
        raise AssertionError(f"WASM {emitter}: missing journal stats")
    if emitter == "binaryen" and "emitter=binaryen-host" not in proc.stderr:
        raise AssertionError("Binaryen route did not actually compile")
    return data["Bench"], tuple(map(int, stats.groups()))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--wasm", type=Path)
    parser.add_argument("--addon", type=Path)
    args = parser.parse_args()
    source = fixture(args.out / "live-broadcast-journal.sb3")
    baseline, _, _ = execute(args.native, source, "off", False)
    interpreted, interp_stats, interp_log = execute(
        args.native, source, "off", True)
    llvm, _, _ = execute(args.native, source, "eager", False)
    journaled, stats, diagnostics = execute(args.native, source, "eager", True)
    if baseline != interpreted or baseline != llvm or llvm != journaled:
        raise AssertionError(
            f"interpreter={baseline}, LLVM={llvm}, journal={journaled}")
    if baseline != {"events": "11", "done": "1"}:
        raise AssertionError(f"Unexpected broadcast outcome: {baseline}")
    if not interp_stats or interp_stats[0] != 2 or interp_stats[2:] != (0, 0):
        raise AssertionError(
            f"Interpreter did not journal both broadcasts: "
            f"{interp_stats}\n{interp_log[-1400:]}")
    if not stats or stats[0] != 2 or stats[2:] != (0, 0):
        raise AssertionError(f"Did not reach live journal: {stats}\n"
                             f"{diagnostics[-1400:]}")
    if args.wasm:
        if not args.addon:
            parser.error("--wasm requires --addon")
        for emitter in ("builtin", "binaryen"):
            state, wasm_stats = execute_wasm(
                args.wasm, args.addon, source, emitter)
            if state != baseline or wasm_stats[0] != 2 or (
                    wasm_stats[2:] != (0, 0)):
                raise AssertionError(
                    f"{emitter}: state={state}, journal={wasm_stats}")
            print(f"PASS {emitter} WASM + commit-once journal: "
                  f"committed={wasm_stats[0]} pending=0 roots=0")
    print("PASS native interpreter/LLVM/LLVM+commit-once journal: "
          f"{journaled} committed={stats[0]} immediate={stats[1]} "
          "pending=0 roots=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
