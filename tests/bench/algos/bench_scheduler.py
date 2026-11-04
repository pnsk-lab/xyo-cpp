#!/usr/bin/env python3
"""Paired scratch-bench scheduler measurements, with unchanged kernel bodies.

Example:
  python3 tests/bench/algos/bench_scheduler.py --before build/old-xyo \
      --after build/fast-release/xyo --project performance.sb3 --runs 5
Both binaries run with the same JIT mode and 30 fps; no danger/parallel overrides.
Only the startup calls to unrelated suites are removed from a temporary archive.
Use --full to measure the original, unmodified performance suite instead.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import zipfile


def scheduler_project(source, destination):
    with zipfile.ZipFile(source) as archive:
        project = json.loads(archive.read("project.json"))
        blocks = project["targets"][0]["blocks"]
        skipped = {key for key, block in blocks.items()
                   if block.get("opcode") == "procedures_call"
                   and block.get("mutation", {}).get("proccode") in {
                       "suite_loops", "suite_vars", "suite_math", "suite_strings",
                       "suite_lists", "suite_algos", "suite_apps"}}
        assert len(skipped) == 7, "expected the seven non-scheduler suite calls"
        for block in blocks.values():
            while block.get("next") in skipped:
                block["next"] = blocks[block["next"]].get("next")
        with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as out:
            for entry in archive.infolist():
                data = (json.dumps(project).encode() if entry.filename == "project.json"
                        else archive.read(entry.filename))
                out.writestr(entry, data)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--before", type=Path, required=True)
    ap.add_argument("--after", type=Path, required=True)
    ap.add_argument("--project", type=Path, default=Path("performance.sb3"))
    ap.add_argument("--out", type=Path, default=Path("build/scheduler-ab"))
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--jit", choices=["hotspot", "eager", "off"], default="hotspot")
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--cpu", type=int)
    args = ap.parse_args()
    if args.runs < 1:
        ap.error("--runs must be positive")
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    args.out.mkdir(parents=True, exist_ok=True)
    project = args.project.resolve()
    if not args.full:
        project = args.out.resolve() / "scheduler.sb3"
        scheduler_project(args.project, project)
    env = dict(os.environ)
    for key in list(env):
        if key.startswith("XYO_"):
            del env[key]
    env["XYO_JIT_MODE"] = args.jit
    results = []
    for run in range(args.runs):
        for label in (["before", "after"] if run % 2 == 0 else ["after", "before"]):
            command = [str(getattr(args, label).resolve()), "run", str(project),
                       "--fps", "30", "--print-list", "output"]
            proc = subprocess.run(command, env=env, capture_output=True, text=True, timeout=180)
            stem = args.out / f"{run + 1}-{label}"
            stem.with_suffix(".out").write_text(proc.stdout)
            stem.with_suffix(".err").write_text(proc.stderr)
            if proc.returncode or "DONE" not in proc.stdout.splitlines():
                raise RuntimeError(f"{label} failed: exit={proc.returncode}: {proc.stderr[-2000:]}")
            rows = {}
            for line in proc.stdout.splitlines():
                fields = [s.strip() for s in line.split("|")]
                if len(fields) == 6 and fields[0] != "id":
                    rows[fields[0]] = {"rate": float(fields[3]), "checksum": fields[5]}
            assert len([key for key in rows if key.startswith("sched_")]) == 8
            results.append({"run": run + 1, "binary": label, "rows": rows})
            print(run + 1, label, {key: value["rate"] for key, value in rows.items()
                                  if key.startswith("sched_")}, flush=True)
    summary = {}
    for key in results[0]["rows"]:
        medians = {label: statistics.median(r["rows"][key]["rate"] for r in results
                                          if r["binary"] == label)
                   for label in ["before", "after"]}
        summary[key] = {**medians, "ratio": medians["after"] / medians["before"]}
        if not key.startswith("sched_"):
            checksums = {r["rows"][key]["checksum"] for r in results}
            assert len(checksums) == 1, (key, checksums)
    ratios = [v["ratio"] for k, v in summary.items() if k.startswith("sched_")]
    report = {"jit": args.jit, "fps": 30, "runs": args.runs,
              "source_sha256": hashlib.sha256(args.project.read_bytes()).hexdigest(),
              "binaries": {label: {"path": str(getattr(args, label).resolve()),
                                    "sha256": hashlib.sha256(getattr(args, label).read_bytes()).hexdigest()}
                           for label in ["before", "after"]},
              "scheduler_geomean_ratio": math.exp(sum(map(math.log, ratios)) / len(ratios)),
              "summary": summary, "results": results}
    (args.out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"scheduler_geomean_ratio": report["scheduler_geomean_ratio"],
                      "summary": summary}, indent=2))


if __name__ == "__main__":
    main()
