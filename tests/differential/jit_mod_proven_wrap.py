#!/usr/bin/env python3
"""Bounded exact-integer modulo: fsub/select vs conventional i32 remainder.

The optimization is allowed only if the dividend is a *proved* positive
integer in [1, 2*m] for a fixed integral m in [2, 65536]. It must preserve
Scratch's modulo result, including the upper endpoint and out-of-range
fallback. Compare interpreter, eager, hotspot and optimized LLVM IR.
"""
from __future__ import annotations
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tests/differential"),
                str(ROOT / "tests/bench/algos")]
from gen_algo_bench import (
    S_add, S_change, S_set, add, make_project, mod, num,
    sub, var, write_sb3,
)
from jit_list_proven_i32_index import ForEachGen


def fixture(n: int, divisor: int, variant: str = "direct"):
    g = ForEachGen(["i", "sum"], ["output"])
    value = var("i")
    if variant == "negative":
        value = sub(num(1), value)
    elif variant == "shift":
        value = add(value, num(1))
    elif variant != "direct":
        raise ValueError(variant)
    g.script([
        S_set("sum", num(0)),
        ("foreach", "i", num(n), [
            S_change("sum", mod(value, num(divisor)))
        ]),
        S_add("output", var("sum")),
        S_add("output", var("i")),
    ])
    return make_project(g.vars.keys(), g.lists.keys(), g)


def run(binary: Path, project: Path, tier: str, mode: int, dump=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(
        XYO_JIT_LAZY_MODE="off",
        XYO_JIT_MOD_REAL_WRAP="1",
        XYO_JIT_MOD_SMALL_WRAP="1",
        XYO_GPU="0", XYO_PARFOR="0",
    )
    if mode != 3:
        env["XYO_JIT_MOD_PROVEN_SMALL_WRAP"] = str(int(mode != 0))
        env["XYO_JIT_MOD_PROVEN_I32_WRAP"] = str(int(mode == 2))
    if dump:
        env["XYO_JIT_DUMP2"] = "1"
    result = subprocess.run(
        [str(binary), "run", str(project), "--jit", tier,
         "--print-list", "output"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=90,
    )
    if result.returncode:
        raise RuntimeError((project.name, tier, mode,
                            result.returncode, result.stderr[-1500:]))
    return tuple(line.strip() for line in result.stdout.splitlines()
                 if line.strip()), result.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--out", type=Path,
                    default=ROOT / "build/multi-bench-20261111/proven-small-mod")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    cases = [
        ("m2", 4, 2, "direct"),
        ("m3", 6, 3, "direct"),
        ("m7", 14, 7, "direct"),
        ("m12", 24, 12, "direct"),
        ("m30", 60, 30, "direct"),
        ("m31", 62, 31, "direct"),
        ("m32", 64, 32, "direct"),
        ("m63", 126, 63, "direct"),
        ("m127", 254, 127, "direct"),
        ("m254", 508, 254, "direct"),
        ("m255", 510, 255, "direct"),
        ("m257", 514, 257, "direct"),
        ("m511", 1022, 511, "direct"),
        ("m512", 1024, 512, "direct"),
        ("m1000", 2000, 1000, "direct"),
        ("m1023", 2046, 1023, "direct"),
        ("m2047", 4094, 2047, "direct"),
        ("m4095", 8190, 4095, "direct"),
        ("m8191", 16382, 8191, "direct"),
        ("m32767", 65534, 32767, "direct"),
        ("m256", 512, 256, "direct"),
        ("m65535", 131070, 65535, "direct"),
        ("m65536", 131072, 65536, "direct"),
        ("m12_small", 10, 12, "direct"),
        ("m12_outside", 25, 12, "direct"),
        ("m255_outside", 511, 255, "direct"),
        ("m12_shift", 24, 12, "shift"),
        ("m12_negative", 24, 12, "negative"),
        ("m1", 2, 1, "direct"),
        ("m65537", 20, 65537, "direct"),
    ]
    for name, n, m, variant in cases:
        path = args.out / f"{name}.sb3"
        write_sb3(path, fixture(n, m, variant))
        expected, _ = run(binary, path, "off", False)
        for tier in ("eager", "hotspot"):
            for mode in (0, 1, 2, 3):
                result, _ = run(binary, path, tier, mode)
                assert result == expected, (name, tier, mode, result, expected)
        counts = {}
        for mode in (0, 1, 2, 3):
            _, ir = run(binary, path, "eager", mode, dump=True)
            (args.out / f"{name}-{mode}.ll").write_text(ir)
            counts[mode] = {
                "urem": ir.count("urem "),
                "srem": ir.count("srem "),
                "fsub": ir.count("fsub double"),
                "fptoui": ir.count("fptoui double"),
                "i32_sel": ir.count("select i1"),
                "ir_bytes": len(ir),
            }
        assert counts[3]["urem"] == counts[2]["urem"], (name, counts)
        assert counts[3]["ir_bytes"] == counts[2]["ir_bytes"], (name, counts)
        if (31 <= m <= 65536 and n == 2*m and variant == "direct"
                and m & (m - 1)):
            assert counts[0]["urem"] > 0, (name, counts)
            assert counts[1]["urem"] == counts[2]["urem"] == 0, (name, counts)
        else:
            assert (counts[0]["urem"] == counts[1]["urem"] ==
                    counts[2]["urem"] == counts[3]["urem"]), (name, counts)
        print("PASS", name, "result", expected, "IR", counts, flush=True)
    print("PASS all proven bounded integer modulo checks", flush=True)


if __name__ == "__main__":
    main()
