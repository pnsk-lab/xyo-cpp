#!/usr/bin/env python3
"""Differential Scratch test: freshly allocated numeric strings parsed in JIT.

String joins create a new (unparsed) SStr repeatedly, so this exercises the
cold xyo_to_num path instead of only cached numeric literals.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set, join, mod, num,
    text, var, make_project, write_sb3,
)


def fixture(path: Path, warp: bool, iterations: int = 350) -> None:
    g = Gen(["i", "s", "sum", "done"], [])
    variants = (
        text("000"),
        text("0."),
        text("1e-"),
        text("-"),
        text("0x"),
        text("not-a-number"),
        text("\u3000"),
    )
    body = [S_change("i", num(1))]
    for prefix in variants:
        body.extend((
            S_set("s", join(prefix, mod(var("i"), num(100)))),
            S_change("sum", var("s")),
        ))
    code = [
        S_set("i", num(0)),
        S_set("sum", num(0)),
        S_repeat(num(iterations), body),
        S_set("done", num(1)),
    ]
    g.define("convert", [], code, warp=warp)
    g.script([S_call("convert")])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))


def execute(binary: Path, project: Path, tier: str, cold_helper: bool = False,
            dump_ir: bool = False) -> tuple[dict, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(XYO_PAR="0", XYO_GPU="0", XYO_DUMPVARS="json",
               XYO_JIT_TO_NUM_COLD_HELPER="1" if cold_helper else "0")
    if dump_ir:
        env["XYO_JIT_DUMP2"] = "1"
    p = subprocess.run(
        [str(binary), "run", str(project), "--jit", tier,
         "--frames", "2000"],
        env=env, cwd=ROOT, capture_output=True, text=True, timeout=110,
    )
    if p.returncode != 0:
        raise RuntimeError((project.name, tier, p.returncode,
                            p.stderr[-1200:]))
    raw = p.stdout
    return json.loads(raw[raw.index("{"):raw.rindex("}") + 1])["Bench"], p.stderr


def main() -> None:
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xyo"
    target = ROOT / "build/to-num-fixtures"
    target.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        project = target / ("warp.sb3" if warp else "nonwarp.sb3")
        fixture(project, warp)
        expected, _ = execute(binary, project, "off")
        assert expected["done"] == "1", (warp, expected)
        for tier in ("eager", "hotspot"):
            for cold_helper in (False, True):
                dump_ir = tier == "eager" and cold_helper
                actual, ir = execute(binary, project, tier,
                                     cold_helper=cold_helper, dump_ir=dump_ir)
                if actual != expected:
                    raise AssertionError(
                        (warp, tier, cold_helper, expected, actual))
                if dump_ir and "@xyo_to_num_uncached" not in ir:
                    raise AssertionError("cold helper is missing in LLVM IR")
                print("PASS", "warp" if warp else "nonwarp", tier,
                      "cold_helper", cold_helper, flush=True)


if __name__ == "__main__":
    main()
