#!/usr/bin/env python3
"""Pure conditional repeat lowering: interpreter / guarded SSA parity.

Exercising both sides of branches, nested expressions, signed remainders,
zero and 64-tick yield boundaries, type/range deoptimization, and non-warp
execution. The SB3 is generated independently of ScratchBench.
"""
import json
import os
import random
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_ifelse, S_repeat, S_set,
    add, eq, gt, le, lt, mod, num, op, sub, text, var,
    make_project, write_sb3,
)


def conditional_body(kind: int):
    if kind == 0:
        return [
            S_change("index", num(1)),
            S_ifelse(eq(mod(var("index"), num(3)), num(0)),
                [S_change("acc", num(1))],
                [S_ifelse(eq(mod(var("index"), num(3)), num(1)),
                    [S_change("acc", num(2))],
                    [S_change("acc", num(3))])]),
        ]
    if kind == 1:
        return [
            S_change("index", num(-2)),
            S_ifelse(lt(mod(var("index"), num(7)), num(3)),
                [S_change("acc", num(-5))],
                [S_if(gt(add(var("index"), num(2)), num(0)),
                    [S_change("acc", num(4))]),
                 S_change("acc", num(1))]),
        ]
    if kind == 3:
        # Set-to-own-affine form is eligible in both branch outcomes.
        return [
            S_set("index", add(var("index"), num(2))),
            S_ifelse(eq(mod(var("index"), num(4)), num(0)),
                [S_set("acc", add(var("acc"), num(3)))],
                [S_set("acc", sub(var("acc"), num(2)))]),
        ]
    if kind == 8:
        # MATH-019: actual selected i32 path. Two independent,
        # positive-remainder predicates with flat if bodies.
        pred = op("operator_or",
            OPERAND1=eq(mod(var("index"),num(5)),num(2)),
            OPERAND2=eq(mod(var("index"),num(7)),num(0)))
        return [
            S_change("index",num(1)),
            S_if(eq(mod(var("index"),num(3)),num(0)),
                 [S_change("acc",num(1))]),
            S_if(pred,[S_change("acc",num(2))]),
        ]
    if kind == 7:
        # All updates are monotone, but the modulo numerator can be
        # negative; this must remain on the proven-safe i64 path.
        return [
            S_change("index",num(1)),
            S_if(lt(mod(sub(var("index"),num(7)),num(11)),num(3)),
                 [S_change("acc",num(2))]),
        ]
    if kind in (5, 6):
        # MATH-019: the 22-leaf condition has an absolute bound exceeding
        # INT32_MAX under the 64-tick entry proof, and must retain i64
        # conditional SSA. The 10-leaf variant fits and may use i32.
        values = [var("index") for _ in range(22 if kind == 5 else 10)]
        while len(values) > 1:
            combined=[]
            for i in range(0,len(values),2):
                if i+1 < len(values):
                    combined.append(add(values[i],values[i+1]))
                else:
                    combined.append(values[i])
            values=combined
        return [
            S_change("index",num(1)),
            S_if(gt(values[0],num(0)),
                 [S_change("acc",num(1))]),
        ]
    if kind == 4:
        # A different source variable may feed a set expression. The
        # branch input and set source must use the current SSA values.
        return [
            S_change("index", num(1)),
            S_ifelse(eq(mod(var("index"), num(5)), num(2)),
                [S_set("acc", add(num(-2), var("index")))],
                [S_set("acc", sub(var("acc"), num(3)))]),
        ]
    return [
        S_change("index", num(1)),
        S_if(op("operator_and",
                OPERAND1=le(mod(sub(var("index"), num(1)), num(5)), num(1)),
                OPERAND2=gt(var("acc"), num(-200))),
            [S_change("acc", num(3)),
             S_ifelse(gt(var("index"), num(32)),
                [S_change("acc", num(-1))],
                [S_change("acc", num(2))])]),
    ]


def make_fixture(path: Path, warp: bool):
    cases = [
        (0, num(0), num(0), 0),
        (1, num(0), num(0), 0),
        (2, num(1), num(-5), 0),
        (63, num(0), num(0), 0),
        (64, num(0), num(0), 0),
        (65, num(0), num(0), 0),
        (127, num(0), num(0), 0),
        (128, num(0), num(0), 0),
        (129, num(0), num(0), 0),
        (130, num(-40), num(-100), 1),
        (130, num(40), num(2), 1),
        (130, num(10), num(-300), 2),
        (130, num(0), text("abc"), 0),
        (130, num(0), text("2.5"), 0),
        (130, num(0), num(1.5), 0),
        (65, num(0), text("stale"), 0),
        (65, text("NaN"), num(0), 0),
        (65, text("Infinity"), num(0), 0),
        (65, num(2**48), num(0), 0),
        (65, num(-2**48), num(0), 0),
        (65, num(1), num(2**48), 1),
        (130, num(-0.0), num(-0.0), 2),
        (130, num(1.25), num(0), 0),
        (140, num(1), num(-10), 1),
        (131, num(-10), num(300), 3),
        (129, num(100), num(-300), 3),
        (129, num(-200), num(10), 4),
        (129, num(20), num(10), 4),
        (130, num(3.5), num(0), 3),
        (130, text("99"), num(5), 4),
        (130, num(-100), text("hello"), 3),
        # i32 guard crossing at 2^20, including a positive variable
        # that starts inside the i32 domain and exits it during repeat.
        (130, num(1048575), num(0), 0),
        (130, num(1048576), num(0), 0),
        (130, num(1048577), num(0), 0),
        (130, num(-1048576), num(0), 0),
        # i64 optimized SSA remains valid outside the tighter i32 guard.
        (130, num(2**40), num(0), 0),
        (130, num(-2**40), num(0), 0),
        # Bound analysis must reject 22-leaf expressions but can admit
        # 10-leaf expressions.
        (65, num(100000), num(3), 5),
        (65, num(100000), num(3), 6),
        (130, num(0), num(3), 7),
        (130, num(5), num(3), 7),
        (0, num(0), num(0), 8),
        (1, num(0), num(0), 8),
        (63, num(0), num(0), 8),
        (64, num(0), num(0), 8),
        (65, num(0), num(0), 8),
        (127, num(0), num(0), 8),
        (128, num(0), num(0), 8),
        (129, num(0), num(0), 8),
        (130, num(1048570), num(5), 8),
        (130, num(1048576), num(5), 8),
        (130, num(1048577), num(5), 8),
        (130, num(-10), num(5), 8),
        (130, num(0), num(-100), 8),
        (130, num(0), num(1.25), 8),
        (130, text("hello"), num(0), 8),
        (130, num(2**40), num(0), 8),
    ]
    # MATH-019: systematic guard and 64-tick boundary cross-product.
    # Cover a whole native chunk and the first iteration of the next
    # chunk, including initial nonnegative-state rejection and i32
    # entry-guard rejection. Keep these on the flat unsigned-mod path.
    for count in (63, 64, 65, 129):
        for initial in (-1, 0, 1, 1048575, 1048576, 1048577):
            cases.append((count, num(initial), num(0), 8))
    # Deterministic adversarial input corpus. Exercise short/long native
    # chunks, the runtime type guard and both selected and rejected i32
    # plans. Use one fixed seed to keep failures reproducible.
    rng = random.Random(0x0192026)
    input_pool = (
        num(-1048577), num(-1048576), num(-1), num(-0.0),
        num(0), num(1), num(2), num(3.5),
        num(1048575), num(1048576), num(1048577),
        num(2**40), num(2**48),
        text("0"), text("1"), text("2.5"),
        text("NaN"), text("Infinity"), text("abc"),
    )
    for _ in range(48):
        count = rng.choice((0, 1, 2, 31, 63, 64, 65, 127, 128, 129, 130, 191))
        index = rng.choice(input_pool)
        acc = rng.choice(input_pool)
        kind = rng.choice((0, 1, 2, 3, 4, 6, 7, 8))
        cases.append((count, index, acc, kind))
    # Force the narrow path: nonnegative integer state, flat positive
    # modulo predicates, and both sides of several if statements.
    for _ in range(32):
        count = rng.choice((1, 2, 31, 63, 64, 65, 127, 128, 129, 191))
        index = rng.choice((0, 1, 2, 3, 5, 6, 7, 31, 63, 127,
                            1024, 65535, 1048575, 1048576))
        acc = rng.choice((0, 1, 2, 7, 1024, 1048575))
        cases.append((count, num(index), num(acc), 8))
    if warp:
        cases.append((80000, num(0), num(0), 0))
    outputs = [f"case_{i}_{suffix}" for i in range(len(cases))
               for suffix in ("index", "acc")]
    g = Gen(["index", "acc", "done", "limit", *outputs], [])
    statements = []
    for i, (count, index, acc, kind) in enumerate(cases):
        statements += [
            S_set("index", index),
            S_set("acc", acc),
            S_repeat(num(count), conditional_body(kind)),
            S_set(outputs[2 * i], var("index")),
            S_set(outputs[2 * i + 1], var("acc")),
        ]
    statements += [
        S_set("index", num(0)),
        S_set("acc", num(0)),
        S_set("limit", num(131)),
        S_repeat(var("limit"), conditional_body(0)),  # dynamic count fallback
        S_set("done", num(1)),
    ]
    g.define("run", [], statements, warp=warp)
    g.script([S_call("run")])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return [*outputs, "done", "index", "acc"]


def run(binary: Path, project: Path, keys, jit: str, flag: str, extra=None):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("XYO_")}
    env.update(XYO_JIT_CONDITIONAL_SSA=flag,
               XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0",
               XYO_PARFOR="0")
    env.update(extra or {})
    p = subprocess.run(
        [str(binary), "run", str(project), "--jit", jit, "--frames", "1000"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=100)
    if p.returncode:
        raise AssertionError((project.name, jit, flag, p.stderr[-1000:]))
    data = json.loads(p.stdout[p.stdout.index("{"):p.stdout.rindex("}") + 1])["Bench"]
    return {key: data[key] for key in keys}


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xyo"
    out = ROOT / "build/conditional-ssa-fixtures"
    out.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        project = out / ("conditional-warp.sb3" if warp else "conditional-nonwarp.sb3")
        keys = make_fixture(project, warp)
        expected = run(binary, project, keys, "off", "0")
        assert expected["done"] == "1", expected
        for jit, flag, extra in [
            ("eager", "0", {}),
            ("eager", "1", {"XYO_JIT_CONDITIONAL_I32": "0"}),
            ("eager", "1", {"XYO_JIT_CONDITIONAL_I32": "1"}),
            ("hotspot", "1", {"XYO_JIT_CONDITIONAL_I32": "1"}),
            ("eager", "1", {"XYO_JIT_OPT_LEVEL": "2",
                            "XYO_JIT_CONDITIONAL_I32": "1"}),
            ("eager", "1", {"XYO_JIT_OPT_LEVEL": "3",
                            "XYO_JIT_CONDITIONAL_I32": "1"}),
        ]:
            result = run(binary, project, keys, jit, flag, extra)
            diff = {key: (expected[key], result[key])
                    for key in keys if result[key] != expected[key]}
            if diff:
                raise AssertionError((warp, jit, flag, extra, diff))
            print("PASS", "warp" if warp else "nonwarp",
                  jit, flag, extra, flush=True)


if __name__ == "__main__":
    main()
