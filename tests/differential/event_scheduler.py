#!/usr/bin/env python3
"""Event-driven wait-until: interpreter/JIT/HotSpot differential fixtures.

Every ON run is compared to an OFF run for the same Scratch project.
Uses the ordinary SB3 runtime rather than synthetic C++ scheduler objects.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set,
    gt, num, op, var, make_project, write_sb3,
)


def append_wait_script(g, expression, counter="done", x=0):
    hat = g.script([], x=x)
    wait = g.new("control_wait_until", parent=hat)
    g.blocks[hat]["next"] = wait
    g.blocks[wait]["inputs"]["CONDITION"] = g.emit_value(expression, wait, True)
    finish = g.emit_stmt(S_change(counter, num(1)), wait)
    g.blocks[wait]["next"] = finish


def project(kind):
    g = Gen(["ready", "progress", "done"], [])
    if kind == "wait_no_writer":
        append_wait_script(g, gt(var("ready"), num(0)))
    elif kind == "wait_with_writer":
        append_wait_script(g, gt(var("ready"), num(0)), x=0)
        g.script([S_repeat(num(80), [S_change("progress", num(1))]),
                  S_set("ready", num(1))], x=300)
    elif kind == "two_waiters":
        append_wait_script(g, gt(var("ready"), num(0)), x=0)
        append_wait_script(g, gt(var("ready"), num(0)), x=250)
        g.script([S_repeat(num(100), [S_change("progress", num(1))]),
                  S_set("ready", num(1))], x=500)
    elif kind == "warp_reentry":
        # A yielded warp procedure leaves wait until, writes to shared state,
        # and re-enters the SAME wait block before the next scheduler pass.
        # Site identity alone must not suppress waking other waiters.
        g.vars["observed"] = "var_observed"
        definition = g.define("consumer", [], [], x=0, y=300, warp=True)
        repeat = g.new("control_repeat", parent=definition)
        g.blocks[definition]["next"] = repeat
        g.blocks[repeat]["inputs"]["TIMES"] = g.emit_value(num(20), repeat)
        waiting = g.new("control_wait_until", parent=repeat)
        g.blocks[waiting]["inputs"]["CONDITION"] = g.emit_value(
            gt(var("ready"), num(0)), waiting, True)
        change = g.emit_stmt(S_change("done", num(1)), waiting)
        reset = g.emit_stmt(S_set("ready", num(0)), change)
        g.blocks[waiting]["next"] = change
        g.blocks[change]["next"] = reset
        g.blocks[repeat]["inputs"]["SUBSTACK"] = [2, waiting]
        g.script([S_call("consumer", [])], x=0)
        g.script([S_repeat(num(200), [
            S_set("ready", num(1)), S_change("progress", num(1))
        ])], x=350)
        append_wait_script(g, gt(var("done"), num(19)), "observed", x=600)
    elif kind == "timer":
        # Time advances even when no Scratch script writes a variable.
        append_wait_script(g, gt(op("sensing_timer"), num(0.05)))
    elif kind == "random":
        append_wait_script(g, gt(op("operator_random",
                                   FROM=num(0), TO=num(1)), num(-1)))
    else:
        raise ValueError(kind)
    return make_project(g.vars.keys(), g.lists.keys(), g)


def run(binary, sb3, enabled, tier, parallel=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_EVENT_SCHED": "1" if enabled else "0",
                "XYO_DUMPVARS": "json", "XYO_GPU": "0",
                "XYO_PARFOR": "0", "XYO_PAR": "1" if parallel else "0"})
    p = subprocess.run(
        [str(binary), "run", str(sb3), "--jit", tier, "--frames", "300"],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=45)
    if p.returncode not in (0, 2):
        raise RuntimeError(f"{sb3.name} {tier} {enabled}: {p.stderr[-1200:]}")
    try:
        text = p.stdout
        return json.loads(text[text.index("{"):text.rindex("}") + 1])["Bench"]
    except (KeyError, ValueError) as e:
        raise RuntimeError(f"Unexpected project output: {p.stdout[-1200:]}") from e


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 \
        else ROOT / "build/xyo"
    out = ROOT / "build/event-scheduler-fixtures"
    out.mkdir(parents=True, exist_ok=True)
    for kind in ("wait_no_writer", "wait_with_writer", "two_waiters",
                 "warp_reentry", "timer", "random"):
        project_path = out / f"{kind}.sb3"
        write_sb3(project_path, project(kind))
        for tier in ("off", "eager", "hotspot"):
            for parallel in (False, True):
                old = run(binary, project_path, False, tier, parallel)
                new = run(binary, project_path, True, tier, parallel)
                assert old == new, (
                    f"{kind}/{tier}/parallel={parallel}: {old!r} != {new!r}")
                expected = {"wait_no_writer": "0", "two_waiters": "2",
                            "warp_reentry": "20"}
                done = expected.get(kind, "1")
                assert new["done"] == done, (
                    f"{kind}/{tier}/parallel={parallel} wrong done={new!r}")
                if kind in ("wait_with_writer", "two_waiters"):
                    assert new["progress"] == (
                        "80" if kind == "wait_with_writer" else "100")
                if kind == "warp_reentry":
                    assert new["done"] == "20" and new["observed"] == "1"
                print(f"{tier:7s} par={int(parallel)} {kind:17s} "
                      f"PASS done={new['done']}", flush=True)


if __name__ == "__main__":
    main()
