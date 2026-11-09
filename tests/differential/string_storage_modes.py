#!/usr/bin/env python3
"""Check interpreter/JIT/CLI parity for each Scratch string storage policy."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (Gen, S_call, S_if, S_repeat, S_set, eq, make_project,
                            num, op, text, var, write_sb3)

PREFIX = "abcdefghijklmnop"
UNIT = "日本x"
REPEATS = 120


def fixture(path):
    names = ["s", "old", "fork", "size", "matched", "last", "numeric"]
    g = Gen(names, [])
    g.define("build", [], [
        S_repeat(num(REPEATS), [
            S_set("s", op("operator_join", STRING1=var("s"),
                          STRING2=text(UNIT))),
        ]),
    ], warp=True)
    expected = PREFIX + UNIT * REPEATS + "42"
    g.script([
        S_set("s", text(PREFIX)),
        S_call("build"),
        S_set("old", var("s")),
        S_set("fork", op("operator_join", STRING1=var("old"),
                         STRING2=text("★"))),
        S_set("s", op("operator_join", STRING1=var("old"),
                      STRING2=num(42))),
        S_set("size", op("operator_length", STRING=var("s"))),
        S_set("matched", num(0)),
        S_if(eq(var("s"), text(expected)), [S_set("matched", num(1))]),
        S_set("last", op("operator_letter_of", LETTER=num(len(expected)),
                          STRING=var("s"))),
        S_set("numeric", op("operator_join", STRING1=num(-0.0),
                             STRING2=text("abc"))),
    ])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return {
        "s": expected,
        "old": expected[:-2],
        "fork": expected[:-2] + "★",
        "size": str(len(expected)),
        "matched": "1",
        "last": "2",
        "numeric": "0abc",
    }


def run(binary, sb3, mode, jit, use_cli=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_PAR": "0", "XYO_GPU": "0",
                "XYO_STRING_STORAGE": "rope" if use_cli else mode})
    command = [str(binary), "run", str(sb3), "--jit", jit,
               "--frames", "10"]
    if use_cli:
        command.append(f"--string-storage={mode}")
    result = subprocess.run(command, cwd=ROOT, env=env, text=True,
                            capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(
            f"{mode}/{jit}: {result.returncode}\n{result.stderr[-1500:]}"
        )
    payload = result.stdout[result.stdout.index("{"):
                            result.stdout.rindex("}") + 1]
    return json.loads(payload)["Bench"]


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xyo-string-storage-") as work:
        sb3 = Path(work) / "join.sb3"
        expected = fixture(sb3)
        for mode in ("rope", "flat", "append-log"):
            for jit in ("off", "eager", "hotspot"):
                output = run(binary, sb3, mode, jit)
                actual = {k: output[k] for k in expected}
                assert actual == expected, (mode, jit, actual, expected)
                print(f"PASS {mode} {jit}", flush=True)
            cli = run(binary, sb3, mode, "eager", use_cli=True)
            actual = {k: cli[k] for k in expected}
            assert actual == expected, (mode, "CLI override", actual, expected)
            print(f"PASS {mode} CLI override", flush=True)


if __name__ == "__main__":
    main()
