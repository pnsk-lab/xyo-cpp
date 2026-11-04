#!/usr/bin/env python3
"""JIT integer-fact benchmark using independent Scratch arithmetic expressions.

All projects are ordinary Scratch 3 projects built with the existing generator.
They are not used to decide when the optimizer activates. The optimizer sees
only generic producer/consumer properties in its LLVM SSA graph.

Each process performs one warmup followed by three independently timed runs.
Use a Release binary and pin all variants to the same core.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import statistics
import subprocess
import tempfile
from pathlib import Path

from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set, add, arg, div, driver,
    floor_, make_project, mod, num, round_, var, write_sb3,
)

ROOT = Path(__file__).resolve().parents[3]


def expression(kind: str):
    x = var("x")
    if kind == "round_mod":
        return mod(round_(x), num(7))
    if kind == "round_add_mod":
        return mod(add(round_(x), num(3)), num(5))
    if kind == "floor_mod":
        return mod(floor_(x), num(7))
    if kind == "nested_mod":
        return mod(add(round_(x), floor_(div(x, num(3)))), num(11))
    raise ValueError(kind)


def project(kind: str, repetitions: int):
    g = Gen(["x", "sum", "t0", "t1", "check"], ["times"])
    procedure = f"bench {kind} %s"
    g.define(procedure, ["n"], [
        S_set("sum", num(0)),
        S_repeat(arg("n"), [
            S_set("x", num(-25.5)),
            S_repeat(num(15000), [
                S_change("x", num(1.25)),
                S_change("sum", expression(kind)),
            ]),
        ]),
    ], warp=True)
    g.script(driver(
        [], [S_call(procedure, [num(1)])],
        S_call(procedure, [num(repetitions)]),
        S_set("check", var("sum")),
    ))
    return make_project(g.vars.keys(), g.lists.keys(), g)


def execute(binary: Path, sb3: Path, flags: dict[str, str], cpu: int) -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(flags)
    env["XYO_DUMPVARS"] = "json"
    proc = subprocess.run(
        ["taskset", "-c", str(cpu), str(binary), "run", str(sb3),
         "--jit", "hotspot"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=120,
    )
    if proc.returncode:
        raise RuntimeError(f"runtime returned {proc.returncode}: {proc.stderr[-1500:]}")
    output = proc.stdout
    try:
        payload = json.loads(output[output.index("{"):output.rindex("}") + 1])
        bench = payload["Bench"]
        times = [float(x) for x in bench["times"]]
        check = str(bench["check"])
    except (KeyError, ValueError, IndexError) as exc:
        raise RuntimeError(f"cannot parse results: {output[-1000:]}") from exc
    if len(times) != 3 or any(x <= 0 for x in times):
        raise RuntimeError(f"invalid measured times {times}")
    return {"median_ms":statistics.median(times), "times_ms":times, "check":check}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--cpu", type=int, default=12)
    parser.add_argument("--blocks", type=int, default=6)
    parser.add_argument("--repetitions", type=int, default=60)
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "jit-intfacts-ab.json")
    parser.add_argument("--projects", type=Path,
                        default=ROOT / "build" / "jit-intfacts-projects")
    parser.add_argument("--seed", type=int, default=20261104)
    opts = parser.parse_args()
    binary = opts.binary.resolve()
    if opts.cpu not in os.sched_getaffinity(0):
        parser.error("specified CPU is not in allowed affinity mask")
    if opts.blocks < 1 or opts.repetitions < 1:
        parser.error("blocks and repetitions must be positive")
    opts.projects.mkdir(parents=True, exist_ok=True)
    opts.out.parent.mkdir(parents=True, exist_ok=True)
    kinds = ["round_mod", "round_add_mod", "floor_mod", "nested_mod"]
    for kind in kinds:
        write_sb3(opts.projects / f"{kind}.sb3", project(kind, opts.repetitions))
    combinations = {
        "baseline": {"XYO_JIT_ROUND_INLINE": "0", "XYO_JIT_INTEGRAL_FACTS": "0"},
        "round": {"XYO_JIT_ROUND_INLINE": "1", "XYO_JIT_INTEGRAL_FACTS": "0"},
        "both": {"XYO_JIT_ROUND_INLINE": "1", "XYO_JIT_INTEGRAL_FACTS": "1"},
    }
    rng = random.Random(opts.seed)
    rows: list[dict] = []
    for block in range(opts.blocks):
        order = [(kind, v) for kind in kinds for v in combinations]
        rng.shuffle(order)
        for kind, variant in order:
            metrics = execute(binary, opts.projects / f"{kind}.sb3",
                              combinations[variant], opts.cpu)
            rows.append({"block": block, "case": kind, "variant": variant,
                         **metrics})
            opts.out.write_text(json.dumps(rows, indent=2))
            print(f"{block} {kind} {variant}: {metrics['median_ms']:.3f}ms "
                  f"check={metrics['check']}", flush=True)
    for kind in kinds:
        cases = [r for r in rows if r["case"] == kind]
        if len({r["check"] for r in cases}) != 1:
            raise AssertionError(f"mismatched program output for {kind}: {cases}")
        durations = {
            name: statistics.median(r["median_ms"] for r in cases
                                    if r["variant"] == name)
            for name in combinations
        }
        print("RESULT", kind, " ".join(
            f"{name}={time:.3f}ms" for name, time in durations.items()
        ), f"both_speedup={durations['baseline']/durations['both']:.4f}", flush=True)


if __name__ == "__main__":
    main()
