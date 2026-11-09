#!/usr/bin/env python3
"""Check async native compilation while a Scratch interpreter is running.

No thread is promoted from the middle of a script: a repeated broadcast
receiver is compiled in the background and uses native code on re-entry.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_change, S_call, S_repeat, S_set, make_project, num, write_sb3
)

def create(path):
    g = Gen(["sum", "done"], [])
    g.define("work", [], [S_repeat(num(2000), [S_change("sum", num(1))])],
             warp=True)
    receiver = g.new(
        "event_whenbroadcastreceived",
        fields={"BROADCAST_OPTION": ["go", "go-id"]},
        top=True, x=400)
    g.blocks[receiver]["next"] = g.chain([S_call("work")], receiver)

    start = g.script([S_set("sum", num(0))])
    current = g.blocks[start]["next"]
    for i in range(8):
        broadcast = g.new("event_broadcastandwait", parent=current)
        g.blocks[broadcast]["inputs"]["BROADCAST_INPUT"] = [
            1, [11, "go", "go-id"]]
        g.blocks[current]["next"] = broadcast
        wait = g.new("control_wait", parent=broadcast)
        g.blocks[wait]["inputs"]["DURATION"] = g.emit_value(num(0.04), wait)
        g.blocks[broadcast]["next"] = wait
        current = wait
    done = g.emit_stmt(S_set("done", num(1)), current)
    g.blocks[current]["next"] = done
    project = make_project(g.vars.keys(), g.lists.keys(), g)
    project["targets"][0]["broadcasts"]["go-id"] = "go"
    write_sb3(path, project)

def run(binary, project, jit, async_mode, frames=1500):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("XYO_")}
    env.update(XYO_JIT_ASYNC=str(int(async_mode)), XYO_DUMPVARS="json",
               XYO_HOT_STATS="1", XYO_JIT_ASYNC_TIMING="1",
               XYO_PAR="0", XYO_GPU="0", XYO_PARFOR="0")
    proc = subprocess.run(
        [str(binary), "run", str(project), "--jit", jit,
         "--frames", str(frames)], cwd=ROOT, capture_output=True,
        text=True, env=env, timeout=60)
    if proc.returncode:
        raise RuntimeError((jit, async_mode, proc.returncode,
                            proc.stdout[-500:], proc.stderr[-800:]))
    text = proc.stdout
    state = json.loads(text[text.index("{"):text.rindex("}")+1])["Bench"]
    return state, proc.stderr

def main():
    binary = (Path(sys.argv[1]) if len(sys.argv) > 1
              else ROOT / "build/xyo").resolve()
    out = ROOT / "build/async-jit"
    out.mkdir(parents=True, exist_ok=True)
    path = out/"broadcast-upgrade.sb3"
    create(path)
    baseline, _ = run(binary, path, "off", False)
    synchronous, _ = run(binary, path, "hotspot", False)
    asynchronous, log = run(binary, path, "hotspot", True)
    assert baseline["sum"] == "16000", baseline
    assert baseline["done"] == "1", baseline
    assert baseline == synchronous == asynchronous, (
        baseline, synchronous, asynchronous)
    assert "[jit-async] published" in log, log[-1500:]
    # The backend must publish native code for subsequent invocations.
    assert re.search(r"\[hot\].*jit.*", log) and re.search(
        r"\[hot\].*\byes\b", log), log[-1500:]
    print("PASS: interpreter executes while native compilation completes",
          flush=True)
    print("PASS: native code published, repeated broadcast parity", flush=True)

if __name__ == "__main__":
    main()
