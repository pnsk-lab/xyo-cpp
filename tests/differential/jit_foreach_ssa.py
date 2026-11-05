#!/usr/bin/env python3
"""Parity for LLVM SSA for-each reductions across warp polling boundaries.

In a pure warp for-each, Scratch's index is overwritten at each iteration,
while accumulators are observable at yield. The optimized SSA path must
preserve both, even if it deoptimizes after an earlier batch.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_set, add, make_project, mul,
    num, text, var, write_sb3,
)


class ForEachGen(Gen):
    def emit_stmt(self, stmt, parent=None):
        if stmt[0] == "foreach":
            _, index, bound, body = stmt
            bid = self.new("control_for_each", parent=parent,
                           fields={"VARIABLE": self.field("VARIABLE", index)})
            self.blocks[bid]["inputs"]["VALUE"] = self.emit_value(bound, bid)
            first = self.chain(body, bid)
            if first:
                self.blocks[bid]["inputs"]["SUBSTACK"] = [2, first]
            return bid
        return super().emit_stmt(stmt, parent)


def fixture(path: Path, warp: bool):
    cases = [
        # Bound, accumulator initial value, multiplicative scale, expression,
        # additional action. The count 63/64/65 exercises warp poll phases.
        (num(0), num(5), 1, "index", False),
        (num(1), num(0), 1, "index", False),
        (num(2.5), num(3), 1, "index", False),
        (num(63), num(0), 1, "index", False),
        (num(64), num(0), 1, "index", False),
        (num(65), num(0), 1, "index", False),
        (num(129), num(0), 1, "index", False),
        (num(130), num(-5), 2, "mul_lhs", False),
        (num(127), text("10"), -3, "mul_rhs", False),
        (num(256), num(0), 5, "mul_lhs", False),
        (num(500), num(-100), 1, "index", False),
        (num(1000), text("abc"), 1, "index", False),
        (num(128), text("1.25"), 1, "index", False),
        (num(128), text(" "), 1, "index", False),
        (num(130), num(281474976710654), 2, "mul_rhs", False),
        (num(130), num(-281474976710654), -3, "mul_lhs", False),
        (num(125), num(20), 1024, "mul_rhs", False),
        (num(125), num(20), 1025, "mul_lhs", False),
        (num(125), num(20), 0, "mul_lhs", False),
        (num(125), num(20), 0.5, "mul_rhs", False),
        (num(129), num(20), 1, "index", True),
        (num(10000), num(0), 1, "index", False),
        (num(0), text("Infinity"), 1, "index", False),
        (text("7"), text("-1"), 1, "index", False),
    ]
    names = [f"o_{i}_{suffix}" for i in range(len(cases))
             for suffix in ("acc", "idx")]
    g = ForEachGen(["acc", "idx", "limit", "done", *names], [])
    statements = []
    for i, (bound, init, scale, expr, extra) in enumerate(cases):
        if expr == "index":
            delta = var("idx")
        elif expr == "mul_lhs":
            delta = mul(var("idx"), num(scale))
        else:
            delta = mul(num(scale), var("idx"))
        body = [S_set("acc", add(var("acc"), delta)) if i % 4 == 0
                else S_set("acc", add(delta, var("acc"))) if i % 4 == 1
                else S_change("acc", delta)]
        if extra:
            body.append(S_change("acc", num(1)))
        statements.extend([
            S_set("idx", text("stale")),
            S_set("acc", init),
            ("foreach", "idx", bound, body),
            S_set(names[2 * i], var("acc")),
            S_set(names[2 * i + 1], var("idx")),
        ])
    # Dynamic bound must preserve header reevaluation and reject SSA plan.
    statements.extend([
        S_set("idx", num(888)), S_set("acc", num(5)),
        S_set("limit", num(128)),
        ("foreach", "idx", var("limit"), [S_change("acc", var("idx"))]),
        S_set("done", num(1)),
    ])
    g.define("run", [], statements, warp=warp)
    g.script([S_call("run")])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return [*names, "idx", "acc", "done"]


def run(binary: Path, fixture_path: Path, jit: str, fast: bool, keys: list[str],
        extra_env: dict[str, str] | None = None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_GPU": "0", "XYO_PAR": "0",
                "XYO_JIT_FOREACH_SSA": "1" if fast else "0"})
    env.update(extra_env or {})
    process = subprocess.run(
        [str(binary), "run", str(fixture_path), "--jit", jit,
         "--frames", "1000"],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=100,
    )
    if process.returncode:
        raise RuntimeError((jit, fast, process.returncode, process.stderr[-1000:]))
    output = process.stdout
    data = json.loads(output[output.index("{"):output.rindex("}") + 1])["Bench"]
    return {key: data[key] for key in keys}


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xyo"
    out = ROOT / "build/llvm-scev-loop"
    out.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        project = out / ("foreach-ssa-warp.sb3" if warp else "foreach-ssa-nonwarp.sb3")
        keys = fixture(project, warp)
        baseline = run(binary, project, "off", False, keys)
        assert baseline["done"] == "1", baseline
        for jit, fast, extra in [
            ("eager", True, {}), ("eager", False, {}),
            ("hotspot", True, {}), ("hotspot", False, {}),
            ("eager", True, {"XYO_JIT_OPT_LEVEL": "2"}),
        ]:
            result = run(binary, project, jit, fast, keys, extra)
            if baseline != result:
                diff = {k: (baseline[k], result[k]) for k in keys
                        if result[k] != baseline[k]}
                raise AssertionError((warp, jit, fast, diff))
            print("PASS", "warp" if warp else "nonwarp", jit,
                  "ssa" if fast else "fallback", extra, flush=True)


if __name__ == "__main__":
    main()
