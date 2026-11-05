#!/usr/bin/env python3
"""Prove guarded warp induction matches Scratch across values and loop forms.

Runs both accelerated and generic JIT, plus interpreter and HotSpot.
The accelerated path may only aggregate safe integral +/-1 scalar loops,
and must preserve the checkpoint phase and return to the generic loop
for non-warp, fractional, nonnumeric, and out-of-range inputs.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_set, S_until,
    add, sub, gt, lt, make_project, num, op, text, var, write_sb3,
)


class LoopGen(Gen):
    def emit_stmt(self, stmt, parent=None):
        if stmt[0] == "while":
            bid = super().emit_stmt(("until", stmt[1], stmt[2]), parent)
            self.blocks[bid]["opcode"] = "control_while"
            return bid
        return super().emit_stmt(stmt, parent)


def create_fixture(path: Path, warp: bool) -> list[str]:
    # (initial, bound, increment, while/until). The latter negates the
    # relational condition, so the loop remains equivalent to while.
    scenarios = [
        (num(0), 20, 1, "while"),
        (text("0"), 20, 1, "until"),
        (text("000"), 20, 1, "while"),
        (text("-5"), 13, 1, "until"),
        (num(12), 5, 1, "while"),
        (num(-2), 0, 1, "until"),
        (text("2.5"), 7, 1, "while"),
        (text(" "), 5, 1, "until"),
        (text("abc"), 5, 1, "while"),
        (text("0x10"), 18, 1, "until"),
        (num(20), 3, -1, "while"),
        (text("20"), 3, -1, "until"),
        (num(-1), -10, -1, "while"),
        (num(-13), -13, -1, "until"),
        (text("-1.5"), -5, -1, "while"),
        (num(-4), 10, -1, "until"),
        (num(281474976710653), 281474976710656, 1, "while"),
        (num(-281474976710653), -281474976710656, -1, "until"),
        (num(281474976710656), 281474976710659, 1, "while"),
        (num(-281474976710656), -281474976710659, -1, "until"),
        (text("NaN"), 0, 1, "while"),
        (text("🍣"), 5, 1, "until"),
        (text("Infinity"), 10, 1, "while"),
        (text("-Infinity"), -10, -1, "until"),
        # Illegal numeric-shadow strings are possible in hand-crafted SB3.
        # Their Scratch comparisons must remain lexical, not coerce to zero.
        (text("z"), "NaN", 1, "while"),
        (text("z"), " ", 1, "until"),
    ]
    keys = [f"result_{i}" for i in range(len(scenarios))]
    g = LoopGen(["i", *keys, "done"], [])
    body = []
    for index, (initial, bound, step, loop_kind) in enumerate(scenarios):
        condition = (lt(var("i"), num(bound)) if step > 0
                     else gt(var("i"), num(bound)))
        if loop_kind == "until":
            condition = op("operator_not", OPERAND=condition)
        loop = (("while", condition, [S_change("i", num(step))])
                if loop_kind == "while"
                else S_until(condition, [S_change("i", num(step))]))
        body.extend([S_set("i", initial), loop, S_set(keys[index], var("i"))])
    g.define("runloop", [], body, warp=warp)
    g.script([S_call("runloop"), S_set("done", num(1))])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return [*keys, "done"]



def create_extended_fixture(path: Path, warp: bool) -> list[str]:
    # Bounds are either literal or independent variable reporters. All cases
    # terminate and all body effects are deterministic (also when fallback
    # is selected). Record secondary variable results as well as induction.
    # (start, bound, step, relation, reversed, dynamic, secondary steps, initial)
    cases = [
        (num(0), num(21), 2, "lt", False, False, (0, 0), (num(3), num(4))),
        (text("0"), num(22), 3, "lt", False, False, (2, -3), (text("5"), num(60))),
        (num(-10), num(10), 7, "lt", False, False, (-5, 10), (num(0), num(0))),
        (num(27), num(-5), -4, "gt", False, False, (2, -2), (num(0), num(0))),
        (num(3), num(10), 2, "le", False, False, (1, 3), (num(2), num(-2))),
        (num(12), num(-9), -5, "ge", False, False, (0, 2), (num(-5), num(3))),
        (num(13), num(-10), -3, "lt", True, False, (1, 2), (num(1), num(3))),
        (num(-11), num(10), 6, "gt", True, False, (2, 4), (num(0), num(0))),
        (num(0), num(10), 3, "le", True, False, (0, -3), (num(0), num(0))),
        (num(12), num(-10), -3, "ge", True, False, (2, 0), (num(5), num(5))),
        (text("0"), num(10), 1, "lt", False, True, (2, 5), (text("9"), text("10"))),
        (num(10), text("-10"), -2, "gt", False, True, (3, -3), (num(0), num(0))),
        (num(1), num(6), 1, "le", True, True, (4, 1), (num(0), num(4))),
        (num(1), num(9), 1, "lt", False, True, (2, 0), (text("abc"), num(0))),
        (num(2), num(14), 2, "lt", False, True, (0, 3), (text("1.25"), num(0))),
        (num(0), text("13.5"), 1, "lt", False, True, (1, 0), (num(1), num(0))),
        (text("z"), text("a"), 1, "lt", False, True, (2, 0), (num(0), num(0))),
        (num(0), num(20), 100, "lt", False, False, (3, -2), (num(2), num(0))),
        (num(20), num(0), -7, "ge", False, False, (2, -2), (num(0), num(0))),
        (num(281474976710650), num(281474976710655), 2, "lt", False, False,
         (2, -1), (num(2), num(3))),
        (num(-281474976710650), num(-281474976710655), -2, "gt", False, False,
         (2, 0), (num(-1), num(-4))),
        (num(0), num(70), 65536, "lt", False, False,
         (0, 1), (num(1), num(2))),
        (num(0), num(70), 65537, "lt", False, False,
         (0, 1), (num(1), num(2))),
        (num(0), num(6), 0, "lt", False, False,
         (0, 0), (num(1), num(2))), # deliberately overridden: zero trips
    ]
    # Last case must terminate: testing zero updates against a completed loop.
    cases[-1] = (num(7), num(6), 0, "lt", False, False,
                 (0, 0), (num(1), num(2)))
    keys = [f"v_{i}_{j}" for i in range(len(cases)) for j in range(3)]
    g = LoopGen(["i", "limit", "acc1", "acc2", "done", *keys], [])
    body = []
    for n, (initial, limit, step, relation, reverse, dynamic, adds, starts) in enumerate(cases):
        v = var("i")
        other = var("limit") if dynamic else limit
        left, right = (other, v) if reverse else (v, other)
        if relation in ("lt", "le"):
            comp = lt(left, right)
        else:
            comp = gt(left, right)
        if relation in ("le", "ge"):
            comp = op("operator_not", OPERAND=comp)
        # Alternate a while and an inverted repeat-until to ensure both forms
        # and nested logical negation are analyzed.
        loop_kind = "while" if n % 2 == 0 else "until"
        condition = comp if loop_kind == "while" else op("operator_not", OPERAND=comp)
        if n % 5 == 0:
            index_stmt = S_set("i", add(var("i"), num(step)))
        elif n % 5 == 1:
            index_stmt = S_set("i", sub(var("i"), num(-step)))
        elif n % 5 == 2:
            index_stmt = S_set("i", add(num(step), var("i")))
        else:
            index_stmt = S_change("i", num(step))
        statements = [index_stmt]
        if adds[0]:
            statements.append(
                S_set("acc1", add(var("acc1"), num(adds[0])))
                if n % 4 == 0 else S_change("acc1", num(adds[0])))
        if adds[1]: statements.append(S_change("acc2", num(adds[1])))
        if n % 3 == 1: statements.reverse()
        loop = (("while", condition, statements) if loop_kind == "while"
                else S_until(condition, statements))
        body.extend([
            S_set("i", initial), S_set("limit", limit),
            S_set("acc1", starts[0]), S_set("acc2", starts[1]), loop,
            S_set(keys[n*3], var("i")),
            S_set(keys[n*3+1], var("acc1")),
            S_set(keys[n*3+2], var("acc2")),
        ])
    g.define("extendedloops", [], body, warp=warp)
    g.script([S_call("extendedloops"), S_set("done", num(1))])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return [*keys, "done"]


def run(binary: Path, sb3: Path, mode: str, enabled: bool,
        fields: list[str], frames: int,
        extra_env: dict[str, str] | None = None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS": "json", "XYO_PARFOR": "0", "XYO_GPU": "0",
        "XYO_JIT_WARP_INDUCTION": "1" if enabled else "0",
    })
    env.update(extra_env or {})
    process = subprocess.run(
        [str(binary), "run", str(sb3), "--jit", mode, "--frames", str(frames)],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=120,
    )
    if process.returncode:
        raise RuntimeError(f"{sb3.name}/{mode}/{enabled}: {process.stderr[-1500:]}")
    output = process.stdout
    try:
        data = json.loads(output[output.index("{"):output.rindex("}") + 1])["Bench"]
    except (ValueError, KeyError) as exc:
        raise AssertionError((sb3.name, mode, enabled, output[-1200:])) from exc
    return {key: data[key] for key in fields}


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xyo"
    output = ROOT / "build/loop-investigation-20261105"
    output.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        path = output / ("warp-induction.sb3" if warp else "nonwarp-induction.sb3")
        keys = create_fixture(path, warp)
        baseline = run(binary, path, "off", False, keys, 1000)
        assert baseline["done"] == "1", baseline
        for mode, enabled in [
            ("eager", True), ("eager", False),
            ("hotspot", True), ("hotspot", False),
        ]:
            result = run(binary, path, mode, enabled, keys, 1000)
            assert result == baseline, (warp, mode, enabled, baseline, result)
            print("PASS", "warp" if warp else "nonwarp", mode,
                  "induction" if enabled else "fallback", flush=True)

        extended = output / ("warp-induction-extended.sb3" if warp
                             else "nonwarp-induction-extended.sb3")
        extended_keys = create_extended_fixture(extended, warp)
        extended_baseline = run(binary, extended, "off", False, extended_keys, 1000)
        assert extended_baseline["done"] == "1", extended_baseline
        for mode, enabled in [
            ("eager", True), ("eager", False),
            ("hotspot", True), ("hotspot", False),
        ]:
            result = run(binary, extended, mode, enabled, extended_keys, 1000)
            if result != extended_baseline:
                differing = {key: (extended_baseline[key], result[key])
                             for key in extended_keys if result[key] != extended_baseline[key]}
                raise AssertionError((warp, mode, enabled, differing))
            print("PASS extended", "warp" if warp else "nonwarp", mode,
                  "induction" if enabled else "fallback", flush=True)

        # LLVM's stock O2/O3 pipelines must optimize a canonical SSA loop
        # without relying on a bespoke ScalarEvolution rewrite. Compare
        # against the direct arithmetic lowering and Scratch interpreter.
        if warp:
            for label, options in [
                ("llvm-O2", {"XYO_JIT_OPT_LEVEL": "2", "XYO_JIT_WARP_SCEV": "1"}),
                ("llvm-O3", {"XYO_JIT_OPT_LEVEL": "3", "XYO_JIT_WARP_SCEV": "1"}),
                ("direct", {"XYO_JIT_WARP_SCEV": "0"}),
            ]:
                result = run(binary, extended, "eager", True,
                             extended_keys, 1000, options)
                assert result == extended_baseline, (label, result, extended_baseline)
                print("PASS LLVM affine", label, flush=True)


if __name__ == "__main__":
    main()
