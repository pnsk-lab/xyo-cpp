#!/usr/bin/env python3
"""IF false-edge typed SSA forwarding: interpreter parity and IR proof.

A mixed-storage variable is overwritten on the true edge; on the false
edge it retains a value assigned just before the IF. Both incoming numeric
facts are valid but neither implies the variable's global type.
"""
import json
import os
import subprocess
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_repeat, S_set,
    add, join, lt, num, text, var, make_project, write_sb3,
)


def make_fixture(path, warp):
    outputs = [f"out_{i}" for i in range(5)]
    gen = Gen(["temp", "sum", "dead", "textonly", "label", "same",
               "done", *outputs], [])
    body = [
        # This string write ensures the variable is genuinely mixed-type.
        S_set("temp", text(" 12.5 ")),
        S_change("sum", var("temp")),
        S_set("temp", add(var("sum"), num(1))),
        S_if(lt(num(1), num(2)),
             [S_set("temp", add(var("sum"), num(5)))]),
        S_change("sum", var("temp")),
        S_change("sum", var("temp")),
        S_set("temp", add(var("sum"), num(-1))),
        S_if(lt(num(2), num(1)),
             [S_set("temp", add(var("sum"), num(-5)))]),
        S_change("sum", var("temp")),
        # A variable with earlier numeric uses but NO read before the
        # overwrite after this IF. Demand analysis should drop both PHIs.
        S_set("dead", text(" 11 ")),
        S_change("sum", var("dead")),
        S_set("dead", add(var("sum"), num(1))),
        S_if(lt(num(1), num(2)),
             [S_set("dead", add(var("sum"), num(2)))]),
        S_set("dead", text("overwritten")),
        S_change("sum", var("dead")),
        # This variable is read as STRING, not numerically, after the join:
        # demand analysis must keep the boxed PHI but drop its numeric PHI.
        S_set("textonly", text("7")),
        S_change("sum", var("textonly")),
        S_set("textonly", add(var("sum"), num(3))),
        S_if(lt(num(2), num(1)),
             [S_set("textonly", add(var("sum"), num(4)))]),
        S_set("label", join(text("p"), var("textonly"))),
        S_set("textonly", text("updated")),
        # Identical-looking Scratch literals need not be identical boxed
        # values: separately allocated string shadows can have different
        # pointer bits, so exact identity is NOT assumed here.
        S_set("same", text("mixed")),
        S_change("sum", var("same")),
        S_set("same", num(7)),
        S_if(lt(num(1), num(2)),
             [S_set("same", num(7))]),
        S_change("sum", var("same")),
        # Dynamic conditional keeps the general guarded fallback.
        S_set("temp", text("NaN")),
        S_if(lt(var("sum"), num(1000)),
             [S_set("temp", num(2))]),
        S_change("sum", var("temp")),
    ]
    statements = [S_set("sum", num(0))]
    for i, count in enumerate((0, 1, 2, 65, 129)):
        statements.extend([S_repeat(num(count), body),
                           S_set(outputs[i], var("sum"))])
    statements.append(S_set("done", num(1)))
    gen.define("work", [], statements, warp=warp)
    gen.script([S_call("work")])
    write_sb3(path, make_project(gen.vars.keys(), gen.lists.keys(), gen))
    return outputs + ["temp", "sum", "dead", "textonly",
                      "label", "same", "done"]


def execute(binary, project, keys, jit, overrides):
    env = {key: val for key, val in os.environ.items()
           if not key.startswith("XYO_")}
    env.update(XYO_DUMPVARS="json", XYO_PAR="0", XYO_GPU="0",
               XYO_JIT_TYPE_FLOW="1", XYO_JIT_LAZY_TYPED_SSA="1",
               XYO_JIT_CONDITIONAL_SSA="0")
    env.update(overrides)
    process = subprocess.run(
        [str(binary), "run", str(project), "--jit", jit, "--frames", "600"],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=100)
    if process.returncode:
        raise RuntimeError((project.name, jit, overrides, process.returncode,
                            process.stderr[-1200:]))
    output = process.stdout
    obj = json.loads(output[output.index("{"):output.rindex("}") + 1])["Bench"]
    return {key: obj[key] for key in keys}, process.stderr


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xyo"
    target = ROOT / "build/if-phi-fixtures"
    target.mkdir(parents=True, exist_ok=True)
    irFound = False
    irCounts = {}
    for warp in (True, False):
        fixture = target / ("warp.sb3" if warp else "nonwarp.sb3")
        keys = make_fixture(fixture, warp)
        expected, _ = execute(binary, fixture, keys, "off", {})
        assert expected["done"] == "1"
        for tier, flags in [
            ("eager", {}),  # default OFF
            ("eager", {"XYO_JIT_IF_PHI": "0"}),
            ("eager", {"XYO_JIT_IF_PHI": "1"}),
            ("eager", {"XYO_JIT_IF_PHI": "1",
                       "XYO_JIT_PHI_DEMAND": "1"}),
            ("hotspot", {"XYO_JIT_IF_PHI": "1",
                         "XYO_JIT_PHI_DEMAND": "1"}),
            ("eager", {"XYO_JIT_IF_PHI": "1", "XYO_JIT_O2": "1"}),
            ("eager", {"XYO_JIT_IF_PHI": "1", "XYO_JIT_DUMP": "1",
                       "XYO_JIT_PHI_STATS": "1",
                       "XYO_JIT_O2": "0", "XYO_JIT_PHI_DEMAND": "0"}),
            ("eager", {"XYO_JIT_IF_PHI": "1", "XYO_JIT_DUMP": "1",
                       "XYO_JIT_PHI_STATS": "1",
                       "XYO_JIT_O2": "0", "XYO_JIT_PHI_DEMAND": "1"}),
        ]:
            actual, log = execute(binary, fixture, keys, tier, flags)
            if actual != expected:
                differences = {key: (expected[key], actual[key]) for key in keys
                               if expected[key] != actual[key]}
                raise AssertionError((warp, tier, flags, differences))
            if flags.get("XYO_JIT_DUMP") == "1":
                def phi_count(name):
                    return sum(
                        line.lstrip().startswith("%typed.if." + name)
                        and " = phi " in line
                        for line in log.splitlines())
                counts = (phi_count("bits"), phi_count("number"))
                irCounts[(warp, flags["XYO_JIT_PHI_DEMAND"])] = counts
                # Unlike a raw IR count, these statistics tell us whether
                # the demand pass actually ran and pruned opportunities.
                stats_lines = [line for line in log.splitlines()
                               if line.startswith("[jit-phi] ")]
                if not stats_lines:
                    raise AssertionError("missing JIT PHI stats")
                info = stats_lines[0].split()
                fields = dict(part.split("=", 1) for part in info[2:]
                              if "=" in part)
                if int(fields["candidates"]) < counts[0]:
                    raise AssertionError(("inconsistent PHI counters", fields))
                if flags["XYO_JIT_PHI_DEMAND"] == "1":
                    if (int(fields["boxed_skipped"]) == 0 or
                            int(fields["numeric_skipped"]) == 0 or
                            int(fields["demand_scans"]) == 0):
                        raise AssertionError(("PHI pruning not observed", fields))
                irFound |= counts[0] > 0 and counts[1] > 0
            print("PASS", "warp" if warp else "nonwarp",
                  tier, flags, flush=True)
        unfiltered = irCounts[(warp, "0")]
        filtered = irCounts[(warp, "1")]
        if not (filtered[0] < unfiltered[0] and
                filtered[1] < unfiltered[1] and filtered[1] > 0):
            raise AssertionError(
                ("PHI demand did not filter unused/nonnumeric consumers",
                 warp, unfiltered, filtered))
        print("IR PHI counts", warp, unfiltered, filtered, flush=True)
    if not irFound:
        raise AssertionError(
            "IF false-edge boxed/numeric SSA PHIs were not generated")


if __name__ == "__main__":
    main()
