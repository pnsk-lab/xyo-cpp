#!/usr/bin/env python3
"""Randomized bounded nested-warp control-flow parity with real Binaryen calls."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import random
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_ifelse, S_repeat, S_set,
    lt, num, var, make_project, write_sb3,
)


def make_case(seed: int, folder: Path) -> Path:
    rng = random.Random(seed)
    gen = Gen(["x", "y", "z"], ["items"])
    leaf = rng.choice([True, False])
    if leaf:
        gen.define("leaf", [], [
            S_change("y", num(rng.choice((-3, -1, 1, 2, 4)))),
        ], warp=True)

    body = [S_change("x", num(rng.choice((-3, -1, 1, 2, 4))))]
    if rng.choice([True, False]):
        body.append(S_ifelse(
            lt(var("x"), num(rng.randint(-10, 90))),
            [S_change("y", num(rng.choice((-2, 1, 3))))],
            [S_change("z", num(rng.choice((-3, 2, 4))))],
        ))
    if leaf:
        body.append(S_call("leaf"))

    # 2–4 loop levels, each at most 5 iterations. Aggregate fuel for
    # even four levels is 5+25+125+625=780 < 1024.
    for _ in range(rng.randint(2, 4)):
        body = [S_repeat(num(rng.randint(1, 5)), body)]

    gen.define("worker", [], [
        S_set("x", num(rng.randint(-4, 4))),
        *body,
        S_change("z", num(1)),
    ], warp=True)

    hat = gen.script([S_repeat(num(72), [S_call("worker")])])
    first = gen.blocks[hat]["next"]
    wait = gen.new("control_wait", parent=hat)
    gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
    gen.blocks[hat]["next"] = wait
    gen.blocks[wait]["next"] = first
    gen.blocks[first]["parent"] = wait

    folder.mkdir(parents=True, exist_ok=True)
    sb3 = folder / f"tree_{seed:04d}.sb3"
    write_sb3(str(sb3), make_project(list(gen.vars), list(gen.lists), gen))
    return sb3


def run_variant(
    sb3: Path, wasm: Path, llvm: Path, addon: Path, variant: str,
) -> tuple[str, str]:
    env = dict(
        os.environ,
        XYO_EXIT_ON_DONE="1",
        XYO_DUMPVARS="json",
        XYO_PAR="0",
        XYO_HOT_INVOCATIONS="1",
        XYO_JIT_WARP_CLOCK="0",
        XYO_NESTED_WASM_STATS="1",
        XYO_NESTED_WASM_WARP_ONLY="1",
        XYO_NESTED_WASM="1",
        XYO_WASM_HOT_EMITTER="binaryen",
        XYO_BINARYEN_NODE_ADDON=str(addon.resolve()),
    )
    if variant == "llvm":
        argv = [str(llvm.resolve()), "run", str(sb3), "--jit", "hotspot"]
        env["XYO_NESTED_WASM"] = "0"
        env["XYO_JIT_ASYNC"] = "0"
    else:
        argv = [
            "node", str(ROOT / "web/xyo-node.mjs"), str(wasm.resolve()),
            "run", str(sb3), "--jit", "off" if variant == "interp"
            else "hotspot",
        ]
        env["XYO_WASM_HOT_EMITTER"] = (
            "builtin" if variant == "builtin" else "binaryen"
        )
        if variant == "interp":
            env["XYO_NESTED_WASM"] = "0"
    argv += ["--frames", "350", "--fps", "0"]
    result = subprocess.run(
        argv, cwd=ROOT, env=env, capture_output=True, text=True,
        timeout=65,
    )
    if result.returncode:
        raise AssertionError(
            f"{sb3.name} {variant}: exit {result.returncode}\n"
            f"{result.stdout[-800:]}\n{result.stderr[-1200:]}"
        )
    pos = result.stdout.find("{")
    if pos == -1:
        raise AssertionError(f"{sb3.name} {variant}: no JSON output")
    parsed = json.JSONDecoder().raw_decode(result.stdout[pos:])[0]["Bench"]
    return json.dumps(parsed, sort_keys=True), result.stderr


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("wasm", type=Path)
    p.add_argument("--addon", type=Path, required=True)
    p.add_argument("--llvm", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--seeds", type=int, default=48)
    args = p.parse_args()
    for seed in range(args.seeds):
        sb3 = make_case(seed, args.out)
        results = {}
        for variant in ("interp", "builtin", "binaryen", "llvm"):
            results[variant] = run_variant(
                sb3, args.wasm, args.llvm, args.addon, variant
            )
        outputs = {value[0] for value in results.values()}
        if len(outputs) != 1:
            raise AssertionError(
                f"Seed {seed}: cross-backend divergence\n" +
                "\n".join(f"{k}: {v[0]}" for k, v in results.items())
            )
        for backend in ("builtin", "binaryen"):
            diag = results[backend][1]
            stats = re.search(
                r"tree_compiled=(\d+) tree_runs=(\d+) tree_bails=(\d+)",
                diag,
            )
            if not stats:
                raise AssertionError(
                    f"Seed {seed} {backend}: no tree execution counters"
                )
            compiled, runs, bails = map(int, stats.groups())
            if not (compiled >= 1 and runs >= 1 and bails == 0):
                raise AssertionError(
                    f"Seed {seed} {backend}: expected compiled tree "
                    f"execution: {stats.group(0)}"
                )
        print(
            f"PASS seed {seed:03d}: 4-way numeric nested warp tree "
            f"parity with real builtin/Binaryen compiled execution",
            flush=True,
        )
    print(f"PASS: {args.seeds} randomized nested warp trees", flush=True)


if __name__ == "__main__":
    main()
