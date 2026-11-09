#!/usr/bin/env python3
"""Independent storage-type transition parity cases for JIT type flow.

Runs the same SB3 in the interpreter, eager LLVM JIT and HotSpot. The fixture
intentionally mixes numeric strings, booleans, colors, NaN-like spellings,
whitespace, branch joins, repeated numeric reads and non-warp scheduler yields.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_ifelse, S_repeat, S_set,
    add, gt, join, lt, num, op, text, var, make_project, write_sb3,
)

STRINGS = ["", " ", "\u3000", "NaN", "Infinity", "-Infinity",
           "-0", "0x10", " 10 ", "1e3", "abc", "1.23e-6"]


def fixture(path: Path, warp: bool):
    outputs = [f"out_{i}" for i in range(7)]
    names = ["value", "copy", "sum", "flag", "color", "acc", "done", *outputs]
    gen = Gen(names, [])
    program = [S_set("sum", num(0)), S_set("acc", text(""))]
    for i, size in enumerate([0, 1, 2, 63, 64, 65, 129]):
        body = [
            S_set("value", var("flag")),
            S_change("sum", var("value")),
            S_set("value", text(STRINGS[i])),
            S_change("sum", var("value")),
            S_set("value", op("colour_picker", {"COLOUR": "#a0ff20"})),
            S_set("color", var("value")),
            S_set("value", text(STRINGS[(i + 5) % len(STRINGS)])),
            S_ifelse(gt(var("sum"), num(10)),
                     [S_set("value", num(i + 1))],
                     [S_set("value", text(STRINGS[(i + 9) % len(STRINGS)]))]),
            S_set("copy", var("value")),
            S_change("sum", var("copy")),
            S_change("sum", var("copy")),
            S_set("acc", join(var("acc"), text("x"))),
            S_set("value", add(var("sum"), num(1))),
            S_change("sum", var("value")),
        ]
        program.extend([S_repeat(num(size), body), S_set(outputs[i], var("sum"))])
    program.append(S_set("done", num(1)))
    gen.define("types", [], program, warp=warp)
    gen.script([S_call("types")])
    project = make_project(gen.vars.keys(), gen.lists.keys(), gen)
    # Scratch project variables can have literal Boolean initial values.
    project["targets"][1]["variables"][gen.vars["flag"]] = ["flag", True]
    write_sb3(path, project)
    return outputs + ["value", "copy", "sum", "color", "acc", "done"]


def run(binary: Path, path: Path, keys, jit: str, flags: dict):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_PAR": "0", "XYO_GPU": "0",
                "XYO_JIT_CONDITIONAL_SSA": "0",
                "XYO_JIT_INLINE_EMPTY_DIRECT": "0"})
    env.update(flags)
    process = subprocess.run(
        [str(binary), "run", str(path), "--jit", jit, "--frames", "1200"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=110)
    if process.returncode:
        raise RuntimeError((path.name, jit, flags, process.returncode,
                            process.stderr[-1200:]))
    output = process.stdout
    variables = json.loads(output[output.index("{"):output.rindex("}") + 1])["Bench"]
    return {key: variables[key] for key in keys}


def main():
    binary = (Path(sys.argv[1]).resolve() if len(sys.argv) > 1
              else (ROOT / "build/xyo"))
    target = ROOT / "build/type-flow-safety"
    target.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        path = target / ("safety-warp.sb3" if warp else "safety-nonwarp.sb3")
        keys = fixture(path, warp)
        expected = run(binary, path, keys, "off", {})
        assert expected["done"] == "1", (warp, expected)
        for jit, flags in [
            ("eager", {}),
            ("hotspot", {}),
            ("eager", {"XYO_JIT_TYPE_PROFILE": "0"}),
            ("eager", {"XYO_JIT_LAZY_TYPED_SSA": "1"}),
            ("eager", {"XYO_JIT_TYPE_FLOW": "0",
                       "XYO_JIT_LOCAL_VAR_FORWARD": "0"}),
            ("eager", {"XYO_JIT_O2": "1"}),
        ]:
            actual = run(binary, path, keys, jit, flags)
            mismatches = {name: (expected[name], actual[name])
                          for name in keys if expected[name] != actual[name]}
            if mismatches:
                raise AssertionError((warp, jit, flags, mismatches))
            print("PASS", "warp" if warp else "nonwarp", jit, flags, flush=True)


if __name__ == "__main__":
    main()
