#!/usr/bin/env python3
"""Exactly one broadcast-and-wait launch per yielding Scratch continuation.

The sender issues two broadcast-and-wait commands. Each of two receivers
changes a variable before/after yielding with control_wait; the sender
must wait until both receivers finish before proceeding to the next.
All four execution paths must match, with precisely two journal commits.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from runtime_broadcast_journal import execute, execute_wasm, ROOT, Gen
from runtime_broadcast_journal import (
    S_change, S_set, num, make_project, write_sb3,
)


def append_wait(g: Gen, after: str, name: str, duration: float) -> str:
    waiting = g.new("control_wait", parent=after)
    g.blocks[waiting]["inputs"]["DURATION"] = g.emit_value(
        num(duration), waiting)
    g.blocks[after]["next"] = waiting
    return waiting


def fixture(path: Path) -> Path:
    g = Gen(["events", "done", "checkpoint"], [])
    sender = g.script([S_set("events", num(0)),
                       S_set("done", num(0)),
                       S_set("checkpoint", num(0))], x=25)
    tail = sender
    for _ in range(3):
        tail = g.blocks[tail]["next"]
    for i in range(2):
        op = g.new("event_broadcastandwait", parent=tail)
        g.blocks[op]["inputs"]["BROADCAST_INPUT"] = [
            1, [11, "go", "go-id"]]
        g.blocks[tail]["next"] = op
        tail = op
        if i == 0:
            # The sender must observe both post-wait receiver effects
            # BEFORE it can issue the second message.
            observed = g.emit_stmt(S_set("checkpoint", ("var", "events")), tail)
            g.blocks[tail]["next"] = observed
            tail = observed
    finished = g.emit_stmt(S_set("done", num(1)), tail)
    g.blocks[tail]["next"] = finished

    for i, (before, after, duration) in enumerate((
        (1, 10, 0.03), (100, 1000, 0.05)
    )):
        receiver = g.script([S_change("events", num(before))],
                            x=300+i*300)
        g.blocks[receiver]["opcode"] = "event_whenbroadcastreceived"
        g.blocks[receiver]["fields"]["BROADCAST_OPTION"] = ["go", "go-id"]
        start = g.blocks[receiver]["next"]
        wait = append_wait(g, start, "go", duration)
        end = g.emit_stmt(S_change("events", num(after)), wait)
        g.blocks[wait]["next"] = end

    project = make_project(["events", "done", "checkpoint"], [], g)
    project["targets"][0]["broadcasts"]["go-id"] = "go"
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), project)
    return path


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--native", required=True, type=Path)
    p.add_argument("--wasm", type=Path)
    p.add_argument("--addon", type=Path)
    p.add_argument("--out", required=True, type=Path)
    args = p.parse_args()
    source = fixture(args.out / "live-broadcast-and-wait-journal.sb3")
    baseline, _, _ = execute(args.native, source, "off", False)
    llvm, _, _ = execute(args.native, source, "eager", False)
    for backend in ("off", "eager"):
        state, stats, diagnostic = execute(args.native, source, backend, True)
        if state != baseline or stats is None or stats[0] != 2 or (
                stats[2:] != (0, 0)):
            raise AssertionError(
                f"native {backend}: {state} != {baseline}; stats={stats}\n"
                + diagnostic[-1500:])
        print(f"PASS native {backend}+journal: committed=2 "
              "pending=0 roots=0")
    if baseline != llvm or baseline != {"events": "2222", "done": "1", "checkpoint": "1111"}:
        raise AssertionError(
            f"native results: interpreter={baseline}, LLVM={llvm}")
    if args.wasm:
        if not args.addon:
            p.error("--wasm requires --addon")
        for emitter in ("builtin", "binaryen"):
            state, stats = execute_wasm(args.wasm, args.addon, source, emitter)
            if state != baseline or stats[0] != 2 or stats[2:] != (0, 0):
                raise AssertionError(
                    f"WASM {emitter}: {state} stats={stats}")
            print(f"PASS WASM {emitter}+journal: committed=2 "
                  "pending=0 roots=0")
    print(f"PASS broadcast-and-wait yields: {baseline}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
