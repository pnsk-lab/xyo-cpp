#!/usr/bin/env python3
"""Paired ScratchBench-derived SB3 comparison, feature OFF/ON.

Individual benchmark copies retain the original Scratch logic and checksum.
The numeric fast path is guarded on both boxed value types at runtime.
"""
import argparse
import hashlib
import json
import os
import random
import re
import statistics
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PROJECTS = ROOT / "build/scratchbench-cause-probes/projects"
BINARY = ROOT / "build/xyo"
CASES = (
    "algo_sort", "math_compare", "math_strcompare", "algo_matmul",
    "app_raycast", "list_read", "algo_primes", "app_brainfuck",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=BINARY)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=CASES)
    parser.add_argument("--trials", type=int, default=12)
    parser.add_argument("--out", type=Path,
                        default=ROOT / "build/plain-number-cmp-paired.json")
    args = parser.parse_args()
    binary = args.binary.resolve()
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    available = os.sched_getaffinity(0)
    cpu = 12 if 12 in available else min(available)
    print(f"binary={binary} sha256={binary_hash} cpu={cpu}", flush=True)
    results = []
    for case in args.cases:
        path = PROJECTS / f"{case}.sb3"
        if not path.is_file():
            raise FileNotFoundError(path)
        ratios = []
        for block in range(args.trials):
            order = ["off", "on"]
            random.Random(1618 + block * 101 + len(case)).shuffle(order)
            pair = {}
            for mode in order:
                env = {key: value for key, value in os.environ.items()
                       if not key.startswith("XYO_")}
                env.update({
                    "XYO_GPU": "0", "XYO_PAR": "0", "XYO_PARFOR": "0",
                })
                if mode == "off":
                    env["XYO_JIT_PLAIN_NUMBER_CMP"] = "0"
                # On = default: only structurally direct list-item comparisons.
                p = subprocess.run([
                    "taskset", "-c", str(cpu), str(binary), "run", str(path),
                    "--jit", "hotspot", "--fps", "30", "--print-list", "output",
                ], cwd=ROOT, env=env, text=True, capture_output=True,
                   timeout=45)
                match = re.search(
                    rf"^{re.escape(case)} \| [^\n|]+ \| [^\n|]+ "
                    rf"\| ([\d.]+) \| [^\n|]+ \| (\S+)",
                    p.stdout, re.M,
                )
                if p.returncode != 0 or not match:
                    raise RuntimeError(
                        f"{case}/{mode} rc={p.returncode}: "
                        f"{p.stderr[-500:]} {p.stdout[-500:]}")
                pair[mode] = (float(match[1]), match[2])
            if pair["off"][1] != pair["on"][1]:
                raise AssertionError((case, block, pair))
            ratio = pair["on"][0] / pair["off"][0]
            ratios.append(ratio)
            results.append({
                "case": case, "block": block, "checksum": pair["off"][1],
                "binary_sha256": binary_hash, "cpu": cpu,
                "off": pair["off"][0], "on": pair["on"][0], "ratio": ratio,
            })
            print(f"{case:19} {block} {ratio:.4f}", flush=True)
        print(f"RESULT {case} median={statistics.median(ratios):.4f} "
              f"range=[{min(ratios):.4f}, {max(ratios):.4f}]", flush=True)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
