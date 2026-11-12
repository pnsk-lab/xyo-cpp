#!/usr/bin/env python3
"""Scratch music block semantics in headless Interpreter and LLVM JIT."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import Gen, S_set, make_project, num, op, write_sb3


def fixture(path: Path):
    g = Gen(["tempo", "done"], [])
    hat = g.script([
        S_set("tempo", op("music_getTempo")),
        S_set("done", num(1)),
    ])
    original = g.blocks[hat]["next"]
    assert original
    chain = [
        ("music_setTempo", {"TEMPO": 120}),
        ("music_changeTempo", {"TEMPO": 30}),
        ("music_setInstrument", {"INSTRUMENT": 12}),
        ("music_midiSetInstrument", {"INSTRUMENT": 74}),
        ("music_playNoteForBeats", {"NOTE": 60, "BEATS": 0.2}),
        ("music_playDrumForBeats", {"DRUM": 2, "BEATS": 0.1}),
        ("music_midiPlayDrumForBeats", {"DRUM": 38, "BEATS": 0.1}),
        ("music_restForBeats", {"BEATS": 0.2}),
    ]
    prev = hat
    for opcode, inputs in chain:
        block = g.new(opcode, parent=prev)
        for name, value in inputs.items():
            g.blocks[block]["inputs"][name] = g.emit_value(num(value), block)
        g.blocks[prev]["next"] = block
        prev = block
    g.blocks[prev]["next"] = original
    g.blocks[original]["parent"] = prev
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))


def main():
    binary = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    project = output / "music-blocks.sb3"
    fixture(project)
    for mode in ("off", "eager", "hotspot"):
        env = dict(os.environ)
        env.update(XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0")
        proc = subprocess.run(
            [str(binary), "run", str(project), "--jit", mode,
             "--frames", "2000"],
            env=env, cwd=ROOT, capture_output=True, text=True, timeout=90)
        assert proc.returncode == 0, (mode, proc.stderr[-2000:])
        value = json.loads(proc.stdout[proc.stdout.index("{"):
                                      proc.stdout.rindex("}")+1])
        state = value["Bench"]
        assert state["done"] == "1", (mode, state)
        assert state["tempo"] == "150", (mode, state)
        short = subprocess.run(
            [str(binary), "run", str(project), "--jit", mode,
             "--frames", "1"],
            env=env, cwd=ROOT, capture_output=True, text=True, timeout=20)
        # xyo run returns a nonzero status when it reaches the explicit
        # frame limit. The checkpoint JSON is still valid.
        assert "{" in short.stdout and "}" in short.stdout, (
            mode, short.returncode, short.stderr[-1000:])
        early = json.loads(short.stdout[short.stdout.index("{"):
                                       short.stdout.rindex("}")+1])["Bench"]
        assert early["done"] == "0", (mode, early)
        print(f"PASS music mode={mode} tempo={state['tempo']} "
              f"done={state['done']} early={early['done']}", flush=True)


if __name__ == "__main__":
    main()
