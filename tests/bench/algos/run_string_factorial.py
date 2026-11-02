#!/usr/bin/env python3
"""Staged randomized 2^3 xyo string-factorial runner.

This does not build variants. Each xyo path must come from a separate CMake
build configured with the same compiler/build type, differing only in the three
experiment options. One subprocess per project per configuration performs the
existing in-project warmup and three timed runs. No diagnostics are enabled.
"""
import argparse
import hashlib
import json
import os
import platform
import random
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
VARIANTS = [f"V{i}" for i in range(8)]
FACTORS = {f"V{i}": {"A": bool(i & 1), "B": bool(i & 2), "C": bool(i & 4)} for i in range(8)}


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_variant(binary, factors):
    cache = binary.parent / "CMakeCache.txt"
    if not cache.is_file():
        raise ValueError(f"missing CMakeCache.txt beside {binary}")
    values, common = {}, {}
    for line in cache.read_text(errors="replace").splitlines():
        key, sep, value = line.partition("=")
        if not sep:
            continue
        if key in ("XYO_STRING_CONS_POOL:BOOL", "XYO_STRING_JOIN_SPECIALIZED:BOOL",
                   "XYO_DISABLE_RT_PROF:BOOL"):
            values[{"XYO_STRING_CONS_POOL:BOOL": "A",
                    "XYO_STRING_JOIN_SPECIALIZED:BOOL": "B",
                    "XYO_DISABLE_RT_PROF:BOOL": "C"}[key]] = value == "ON"
        elif key in ("CMAKE_BUILD_TYPE:STRING", "CMAKE_CXX_COMPILER:FILEPATH",
                     "XYO_JIT:BOOL", "XYO_BUILD_SHELL:BOOL"):
            common[key] = value
    if values != factors:
        raise ValueError(f"{binary}: configured factors {values} != expected {factors}")
    return common


def set_affinity(cpu):
    if cpu is not None and cpu >= 0 and hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, {cpu})


def generate(kind, scale, out):
    sys.path.insert(0, str(ROOT / "tests/bench/algos"))
    from bench_strings import generate as gen
    from gen_algo_bench import write_sb3
    project, expected = gen(kind, scale)
    dest = out / "projects" / f"{kind}-scale{scale}.sb3"
    dest.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(dest), project)
    return dest, expected


def run_one(binary, project, cpu):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env["XYO_DUMPVARS"] = "json"
    env["XYO_JIT_MODE"] = "hotspot"
    command = [str(binary), "run", str(project), "--jit", "hotspot"]
    start = time.perf_counter_ns()
    p = subprocess.run(command, cwd=ROOT, env=env, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300,
                       preexec_fn=(lambda: set_affinity(cpu)) if cpu is not None and cpu >= 0 else None)
    elapsed = (time.perf_counter_ns() - start) / 1e6
    if p.returncode != 0:
        raise RuntimeError(f"{binary}: exit={p.returncode}\n{p.stderr[-4000:]}")
    # Use the established benchmark parser, retaining raw stdout/stderr below.
    sys.path.insert(0, str(ROOT / "tests/bench/algos"))
    from bench import parse_xyo
    parsed = parse_xyo(p.stdout.encode())
    return {"command": command, "wall_ms": elapsed, **parsed,
            "median_ms": statistics.median(parsed["times"]),
            "stdout": p.stdout, "stderr": p.stderr}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binaries", nargs=8, required=True,
                    help="V0...V7 xyo executable paths, in configuration order")
    ap.add_argument("--out", required=True)
    ap.add_argument("--scale", type=int, default=80)
    ap.add_argument("--kinds", default="join,join_number,join_observe,join_observe_last,short_join,short_dynamic,prefix_fraction,cast,letter,contains")
    ap.add_argument("--blocks", type=int, default=1,
                    help="randomized complete blocks (1=smoke, 3=pilot, 15=planned main run)")
    ap.add_argument("--cpu", type=int, default=12,
                    help="pin subprocesses here; use --cpu -1 to leave affinity unchanged")
    ap.add_argument("--seed", type=int, default=20260618)
    ap.add_argument("--existing-out", action="store_true",
                    help="append blocks to a pre-created empty output directory")
    args = ap.parse_args()
    if args.blocks < 1 or args.scale < 1:
        ap.error("blocks and scale must be positive")
    if args.cpu >= 0:
        if not hasattr(os, "sched_getaffinity") or not hasattr(os, "sched_setaffinity"):
            ap.error("CPU affinity is unavailable on this platform; pass --cpu -1")
        if args.cpu not in os.sched_getaffinity(0):
            ap.error(f"CPU {args.cpu} is outside the process affinity mask; pass --cpu -1 or choose an allowed CPU")
    if args.cpu < -1:
        ap.error("--cpu must be -1 or a nonnegative CPU id")
    out = Path(args.out).resolve()
    if args.existing_out:
        if not out.is_dir() or any(out.iterdir()): ap.error("--existing-out requires an empty existing directory")
    else:
        out.mkdir(parents=True, exist_ok=False)
    binaries = [Path(p).resolve() for p in args.binaries]
    signatures = []
    for i, p in enumerate(binaries):
        if not p.is_file(): ap.error(f"binary not found: {p}")
        try:
            signatures.append(verify_variant(p, FACTORS[VARIANTS[i]]))
        except ValueError as e:
            ap.error(str(e))
    if any(signature != signatures[0] for signature in signatures[1:]):
        ap.error(f"variants must share build type/compiler/JIT settings: {signatures}")
    expected_flags = [FACTORS[v] for v in VARIANTS]
    provenance = {
        "schema": 1, "platform": platform.platform(), "cpu": args.cpu,
        "seed": args.seed, "blocks": args.blocks, "scale": args.scale,
        "kinds": args.kinds.split(","), "mode": "hotspot", "diagnostics": "disabled",
        "variants": [{"name": v, "factors": expected_flags[i], "binary": str(binaries[i]),
                      "binary_sha256": sha(binaries[i])} for i, v in enumerate(VARIANTS)],
        "rows": [],
    }
    rng = random.Random(args.seed)
    runs_dir = out / "runs"
    runs_dir.mkdir()
    for kind in provenance["kinds"]:
        project, expected = generate(kind, args.scale, out)
        project_hash = sha(project)
        for block in range(args.blocks):
            order = list(range(8))
            rng.shuffle(order)
            for index in order:
                variant = VARIANTS[index]
                record = run_one(binaries[index], project, args.cpu)
                if str(record["check"]) != expected:
                    raise RuntimeError(f"{kind}/{variant}/block{block}: {record['check']} != {expected}")
                rawbase = runs_dir / f"{kind}-b{block:02d}-{variant}"
                (rawbase.with_suffix(".stdout.txt")).write_text(record.pop("stdout"))
                (rawbase.with_suffix(".stderr.txt")).write_text(record.pop("stderr"))
                provenance["rows"].append({"kind": kind, "block": block,
                    "order_in_block": order.index(index), "variant": variant,
                    "factors": FACTORS[variant], "project": str(project),
                    "project_sha256": project_hash, "expected": expected, **record})
                print(f"{kind} block={block} {variant} {record['median_ms']:.3f}ms", flush=True)
                (out / "raw.json").write_text(json.dumps(provenance, indent=2) + "\n")
    print(out)

if __name__ == "__main__":
    main()
