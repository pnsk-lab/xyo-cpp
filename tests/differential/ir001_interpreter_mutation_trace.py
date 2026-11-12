#!/usr/bin/env python3
"""IR-001 native flat-interpreter VM-write diagnostic and Scratch parity.

All seven Scratch data mutation opcodes must enter the central
interpreter write barrier before the actual primitive executes. The
barrier cannot authorize source deoptimization and must not change
Scratch-visible values with diagnostics on/off.
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
    Gen, S_add, S_change, S_del, S_delall, S_replace, S_set,
    length, make_project, num, write_sb3,
)
from runtime_broadcast_journal import execute

OPCODES = {
    "data_setvariableto": 2,
    "data_changevariableby": 1,
    "data_addtolist": 2,
    "data_replaceitemoflist": 1,
    "data_deleteoflist": 1,
    "data_deletealloflist": 1,
    "data_insertatlist": 1,
}
WRITE = re.compile(r"\[ir001-interp-vm-write\] opcode=(\S+)")


def fixture(path: Path) -> Path:
    gen = Gen(["x", "done"], ["history"])
    hat = gen.script([
        S_set("x", num(0)),
        S_change("x", num(2)),
        S_add("history", num(5)),
        S_replace("history", num(1), num(6)),
        S_del("history", num(1)),
        S_add("history", num(2)),
        S_delall("history"),
    ])
    last = hat
    while gen.blocks[last]["next"]:
        last = gen.blocks[last]["next"]
    inserted = gen.new(
        "data_insertatlist", parent=last,
        fields={"LIST": gen.field("LIST", "history")})
    gen.blocks[inserted]["inputs"]["INDEX"] = gen.emit_value(num(1), inserted)
    gen.blocks[inserted]["inputs"]["ITEM"] = gen.emit_value(num(7), inserted)
    gen.blocks[last]["next"] = inserted
    done = gen.emit_stmt(S_set("done", length("history")), inserted)
    gen.blocks[inserted]["next"] = done

    project = make_project(["x", "done"], ["history"], gen)
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), project)
    return path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    project = fixture(args.out / "ir001-interpreter-writes.sb3")

    flags = ("XYO_IR001_TRACE_EFFECT_PC",
             "XYO_IR001_TRACE_INTERP_WRITE")
    old = {flag: os.environ.get(flag) for flag in flags}
    try:
        os.environ.pop("XYO_IR001_TRACE_EFFECT_PC", None)
        os.environ["XYO_IR001_TRACE_INTERP_WRITE"] = "1"
        baseline, _, off_log = execute(args.native, project, "off", False)
        assert not WRITE.findall(off_log), off_log[-2500:]

        os.environ["XYO_IR001_TRACE_EFFECT_PC"] = "1"
        traced, _, on_log = execute(args.native, project, "off", False)
        assert traced == baseline, (traced, baseline)
        assert traced.get("x") == "2" and traced.get("done") == "1", traced

        emitted = WRITE.findall(on_log)
        actual = {opcode: emitted.count(opcode) for opcode in set(emitted)}
        assert actual == OPCODES, (actual, OPCODES, on_log[-2500:])
        print("PASS IR-001 interpreter data mutation barrier: "
              "7 opcodes / 9 dispatches, Scratch parity retained")
    finally:
        for key, value in old.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


if __name__ == "__main__":
    main()
