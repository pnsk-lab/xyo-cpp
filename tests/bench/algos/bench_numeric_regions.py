#!/usr/bin/env python3
"""Compare source structures through one common typed numeric CFG backend."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess

from bench_structure_cliffs import ROOT, PROFILES, invoke, validate
from gen_algo_bench import write_sb3
from numeric_region_cases import KINDS, oracle, project


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--count", type=int, default=12000000)
    ap.add_argument("--blocks", type=int, default=6)
    ap.add_argument("--samples", type=int, default=3)
    ap.add_argument("--cpu", type=int, default=12)
    ap.add_argument("--profiles", default="default,composition_all,numeric_region")
    ap.add_argument("--kinds", default=",".join(KINDS))
    ap.add_argument("--diagnostics", action="store_true")
    ap.add_argument("--source-revision", default="unattested")
    args = ap.parse_args()
    profiles, kinds = args.profiles.split(","), args.kinds.split(",")
    if (not 1 <= args.count <= 2147483647 or args.blocks < 1 or args.samples < 1 or
            len(set(profiles)) != len(profiles) or len(set(kinds)) != len(kinds) or
            not set(profiles) <= set(PROFILES) or not set(kinds) <= set(KINDS)):
        ap.error("invalid cases, profiles or counts")
    if args.cpu not in os.sched_getaffinity(0):
        ap.error("CPU not available")
    binary, out = args.binary.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / "projects").mkdir()
    (out / "logs").mkdir()
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    hashes = {}
    for name in ("bench_numeric_regions.py", "numeric_region_cases.py", "bench_structure_cliffs.py", "gen_algo_bench.py"):
        path = Path(__file__).with_name(name)
        shutil.copyfile(path, out / name)
        hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    manifest = {"binary": str(binary), "binary_sha256": digest,
                "source_revision": args.source_revision,
                "workspace_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "platform": platform.platform(), "cpu": args.cpu, "count": args.count,
                "blocks": args.blocks, "samples": args.samples, "profiles": profiles,
                "kinds": kinds, "harness_sha256": hashes, "seed": 404,
                "notes": "eager, 2 warmups + samples, no profiling/block count/danger, "
                         "GPU/PAR/PARFOR off; kernel timing excludes compilation"}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2))
    if (binary.parent / "CMakeCache.txt").exists():
        shutil.copyfile(binary.parent / "CMakeCache.txt", out / "CMakeCache.txt")
    expected = oracle(args.count)
    for kind in kinds:
        write_sb3(out / "projects" / f"{kind}.sb3", project(args.count, kind, args.samples))
    rows = []
    rng = random.Random(404)
    jobs = [(kind, profile) for kind in kinds for profile in profiles]
    for block in range(args.blocks):
        if block % 2 == 0:
            rng.shuffle(jobs)
        else:
            jobs.reverse()
        for kind, profile in jobs:
            result = invoke(binary, out / "projects" / f"{kind}.sb3", "eager", profile,
                            args.cpu, 120, out / "logs" / f"b{block}-{kind}-{profile}")
            validate(result, expected, args.samples, timed=True)
            rows.append({"block": block, "kind": kind, "profile": profile, **result})
            (out / "results.json").write_text(json.dumps(rows, indent=2))
            print(block, kind, profile, f"{statistics.median(result['timings_ms']):.3f}ms", flush=True)
    summary = {}
    for kind in kinds:
        baseline = {r["block"]: statistics.median(r["timings_ms"]) for r in rows
                    if r["kind"] == kind and r["profile"] == "default"}
        for profile in profiles:
            selected = [r for r in rows if r["kind"] == kind and r["profile"] == profile]
            medians = [statistics.median(r["timings_ms"]) for r in selected]
            ratios = [baseline[r["block"]] / statistics.median(r["timings_ms"])
                      for r in selected] if baseline else []
            summary[f"{profile}/{kind}"] = {
                "median_ms": statistics.median(medians), "process_medians_ms": medians,
                "paired_speedup": statistics.median(ratios) if ratios else None,
                "paired_ratios": ratios,
            }
    (out / "summary.json").write_text(json.dumps(summary, indent=2))
    if args.diagnostics:
        for kind, profile in [(k, p) for k in kinds for p in profiles]:
            result = invoke(binary, out / "projects" / f"{kind}.sb3", "eager", profile,
                            args.cpu, 120, out / "logs" / f"ir-{kind}-{profile}", True)
            validate(result, expected, args.samples)
    if hashlib.sha256(binary.read_bytes()).hexdigest() != digest:
        raise RuntimeError("binary changed during experiment")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
