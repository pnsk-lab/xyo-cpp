#!/usr/bin/env python3
"""Real mixed LLVM/flat-interpreter write before the native loop checkpoint.

One HotSpot root script is compiled because it has a loop. Its
broadcast receiver is a cold, single-shot interpreted script. A wait
after broadcast yields to the receiver BEFORE each native loop
checkpoint. Therefore all 3 checkpoints must fail closed solely due
to an interpreted data_changevariableby, without dropping the journal
delivery or changing Scratch-visible values.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))
from gen_algo_bench import (
    Gen, S_set, S_change, S_repeat, num, op, make_project, write_sb3,
)
from ir001_effect_source_trace import LINE, CHECKPOINT
from runtime_broadcast_journal import execute

INTERP = re.compile(r"\[ir001-interp-vm-write\] opcode=(\S+)")
REASON = re.compile(
    r"\[ir001-effect-checkpoint\][^\n]* "
    r"foreign_vm_write=(\d) interp_vm_write=(\d)"
)


def fixture(path: Path, *, visual_receiver: bool = False) -> Path:
    variables = ["x", "done", "position"] if visual_receiver else ["x", "done"]
    gen = Gen(variables, [])
    gen.script([
        S_set("x", num(0)),
        S_repeat(num(3), [S_change("x", num(1))]),
        S_set("done", num(1)),
    ])
    root = next(ident for ident, block in gen.blocks.items()
                if block["opcode"] == "control_repeat")
    body = gen.blocks[root]["inputs"]["SUBSTACK"][1]
    broadcast = gen.new("event_broadcast", parent=body)
    gen.blocks[body]["next"] = broadcast
    gen.blocks[broadcast]["inputs"]["BROADCAST_INPUT"] = [
        1, [11, "go", "id_go"]]
    wait = gen.new("control_wait", parent=broadcast)
    gen.blocks[broadcast]["next"] = wait
    gen.blocks[wait]["inputs"]["DURATION"] = [1, [4, "0"]]

    receiver = gen.script([S_change("x", num(10))], x=300)
    gen.blocks[receiver]["opcode"] = "event_whenbroadcastreceived"
    gen.blocks[receiver]["fields"]["BROADCAST_OPTION"] = [
        "go", "id_go"]

    if visual_receiver:
        # This cold interpreted receiver writes target geometry,
        # then stores the real x-position reporter as observable proof,
        # changes sprite visibility, and modifies the pen state.
        # No one of these may be accepted as a deopt-safe native
        # checkpoint, even though the host broadcast was exact.
        first = gen.blocks[receiver]["next"]
        gen.blocks[first]["opcode"] = "motion_setx"
        gen.blocks[first]["fields"] = {}
        gen.blocks[first]["inputs"] = {"X": [1, [4, "42"]]}
        sampled = gen.emit_stmt(
            S_set("position", op("motion_xposition")), first)
        hidden = gen.new("looks_hide", parent=sampled)
        pen_down = gen.new("pen_penDown", parent=hidden)
        gen.blocks[first]["next"] = sampled
        gen.blocks[sampled]["next"] = hidden
        gen.blocks[hidden]["next"] = pen_down

    project = make_project(variables, [], gen)
    project["targets"][1]["broadcasts"]["id_go"] = "go"
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), project)
    return path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    project = fixture(args.out / "ir001-hotspot-interpreted-receiver.sb3")

    configured = {
        "XYO_IR001_TRACE_EFFECT_PC": "1",
        "XYO_IR001_TRACE_INTERP_WRITE": "1",
        "XYO_JIT_CALLFRAME": "1",
        "XYO_RT_ROOT_ONLY_DEOPT": "1",
        "XYO_HOT_INVOCATIONS": "5000",
        "XYO_HOT_LOOP": "1",
    }
    previous = {key: os.environ.get(key) for key in configured}
    try:
        os.environ.update(configured)
        state, stats, log = execute(args.native, project, "hotspot", True)
        assert state.get("x") == "33" and state.get("done") == "1", state
        assert stats == (3, 0, 0, 0), stats
        assert log.count("xyo-jit: compile top=") >= 1, log[-2300:]
        assert log.count("xyo-jit: interp top=") >= 1, log[-2300:]
        assert INTERP.findall(log) == ["data_changevariableby"] * 3, (
            INTERP.findall(log), log[-2300:])

        deliveries = LINE.findall(log)
        assert len(deliveries) == 3, log[-2400:]
        assert all(event[13] == "1" for event in deliveries), deliveries
        checkpoints = CHECKPOINT.findall(log)
        assert len(checkpoints) == 3, log[-2500:]
        assert all(record[3] == "0" and record[7] == "1" and
                   record[9] == "1" for record in checkpoints), checkpoints
        assert REASON.findall(log) == [("1", "1")] * 3, log[-2500:]

        visual_project = fixture(
            args.out / "ir001-hotspot-interpreted-visual-receiver.sb3",
            visual_receiver=True)
        visual_state, visual_stats, visual_log = execute(
            args.native, visual_project, "hotspot", True)
        assert visual_state.get("x") == "3", visual_state
        assert visual_state.get("done") == "1", visual_state
        assert visual_state.get("position") == "42", visual_state
        assert visual_stats == (3, 0, 0, 0), visual_stats
        visual_writes = INTERP.findall(visual_log)
        assert visual_writes == [
            "motion_setx", "data_setvariableto",
            "looks_hide", "pen_penDown"] * 3, (
            visual_writes, visual_log[-2700:])
        visual_checkpoints = CHECKPOINT.findall(visual_log)
        assert len(visual_checkpoints) == 3, visual_log[-2500:]
        assert all(row[3] == "0" and row[7] == "1" and
                   row[9] == "1" for row in visual_checkpoints), (
            visual_checkpoints, visual_log[-2500:])
        assert REASON.findall(visual_log) == [("1", "1")] * 3
        assert len(LINE.findall(visual_log)) == 3, visual_log[-2500:]

        os.environ.pop("XYO_IR001_TRACE_EFFECT_PC")
        os.environ.pop("XYO_IR001_TRACE_INTERP_WRITE")
        baseline, baseline_stats, baseline_log = execute(
            args.native, project, "hotspot", True)
        assert state == baseline and stats == baseline_stats
        assert not INTERP.findall(baseline_log)
        visual_baseline, visual_baseline_stats, visual_baseline_log = execute(
            args.native, visual_project, "hotspot", True)
        assert visual_baseline == visual_state
        assert visual_baseline_stats == visual_stats
        assert not INTERP.findall(visual_baseline_log)
        print(
            "PASS IR-001 mixed HotSpot LLVM sender / interpreted receiver: "
            "3 data writes and 12 data/motion/looks/pen writes, "
            "6 denied checkpoints; journal + Scratch parity retained")
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


if __name__ == "__main__":
    main()
