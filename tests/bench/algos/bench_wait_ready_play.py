#!/usr/bin/env python3
"""CPU-pinned, alternating OFF/ON SDL play benchmark for wait-ready sleeping.

No changes to the Scratch project, VM implementation or renderer.
Profiles are produced by XYO_PLAY_PROFILE; final screenshots are compared
as a *diagnostic*, not as definitive semantic parity for nondeterministic games.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentile(samples: list[float], q: float) -> float:
    vals = sorted(samples)
    pos = (len(vals) - 1) * q
    index = int(pos)
    fraction = pos - index
    return vals[index] * (1 - fraction) + vals[min(index + 1, len(vals) - 1)] * fraction


def profile_stats(path: Path, frames: int, warmup: int) -> dict[str, float | int]:
    with path.open(newline="") as file:
        rows = list(csv.DictReader(file))
    if len(rows) != frames or any(int(r["frame"]) != i for i, r in enumerate(rows)):
        raise RuntimeError(f"{path}: wrong frame sequence: {len(rows)} instead of {frames}")
    measured = rows[warmup:]
    step = [float(r["step_ms"]) for r in measured]
    vm = [float(r["vm_ms"]) for r in measured]
    draw = [float(r["draw_ms"]) for r in measured]
    interval = [float(r["frame_interval_ms"]) for r in measured]
    return {
        "frames": len(measured),
        "step_p50_ms": percentile(step, 0.5),
        "step_p95_ms": percentile(step, 0.95),
        "step_p99_ms": percentile(step, 0.99),
        "step_max_ms": max(step),
        "vm_p95_ms": percentile(vm, 0.95),
        "draw_p95_ms": percentile(draw, 0.95),
        "interval_p95_ms": percentile(interval, 0.95),
        "over_16_7ms": sum(x > 1000 / 60 for x in step),
        "over_33_3ms": sum(x > 1000 / 30 for x in step),
        "avg_threads": statistics.mean(int(r["threads"]) for r in measured),
        "drawn_frames": sum(float(r["draw_ms"]) > 0 for r in measured),
    }


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", required=True, type=Path)
    ap.add_argument("--project", action="append", required=True, metavar="NAME=FILE")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--frames", type=int, default=70)
    ap.add_argument("--warmup", type=int, default=10)
    ap.add_argument("--cpu", type=int, default=16)
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--require-parity", action="store_true",
                    help="require ON/OFF final image and full-variable JSON equality")
    ap.add_argument("--expect-stage-var", action="append", default=[],
                    metavar="NAME=VALUE",
                    help="check final Stage variable, requires --require-parity")
    ap.add_argument(
        "--inject",
        default="12:mm:210,140|13:md:210,140|14:mu:210,140|"
                "30:key:left|31:ku:left|45:key:right|46:ku:right",
        help="Same deterministic SDL mouse/keyboard injection for each variant",
    )
    args = ap.parse_args()
    if not (args.runs > 0 and args.frames > args.warmup >= 1):
        ap.error("require runs>0, frames>warmup>=1")
    expected_stage = {}
    for spec in args.expect_stage_var:
        if "=" not in spec:
            ap.error("--expect-stage-var requires NAME=VALUE")
        name, value = spec.split("=", 1)
        expected_stage[name] = value
    if expected_stage and not args.require_parity:
        ap.error("--expect-stage-var requires --require-parity")
    projects = {}
    for spec in args.project:
        if "=" not in spec:
            ap.error("--project requires NAME=FILE")
        name, file = spec.split("=", 1)
        if not name or name in projects or not Path(file).is_file():
            ap.error(f"invalid or duplicated project: {spec}")
        projects[name] = Path(file).resolve()
    if hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, {args.cpu})
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = out / "frozen-xyo"
    shutil.copy2(args.binary.resolve(), binary)
    binary.chmod(binary.stat().st_mode | 0o111)
    binary_hash = sha(binary)
    frozen_projects = {}
    for name, source in projects.items():
        dest = out / f"source-{name}.sb3"
        shutil.copy2(source, dest)
        frozen_projects[name] = (dest, sha(dest))

    base_env = {key: val for key, val in os.environ.items()
                if not key.startswith("XYO_") and not key.startswith("SDL_")}
    base_env.update({
        "SDL_VIDEODRIVER": "dummy",
        "SDL_AUDIODRIVER": "dummy",
        "SDL_RENDER_DRIVER": "software",
        "XYO_JIT_MODE": "eager",
        "XYO_YIELD_QMODE": "off",
        "XYO_WAIT_WAKE_REGISTRY": "1",
        "XYO_SCHED_READY_INDEX": "1",
        "XYO_EXIT_FRAMES": str(args.frames),
        "XYO_INJECT": args.inject,
    })
    if args.require_parity:
        base_env["XYO_DUMPVARS"] = "json"
    records = []
    for name, (project, project_hash) in frozen_projects.items():
        for run in range(1, args.runs + 1):
            for enabled in ([False, True] if run % 2 else [True, False]):
                state = "on" if enabled else "off"
                stem = out / f"{name}-{run:02d}-{state}"
                profile = Path(str(stem) + ".csv")
                shot = Path(str(stem) + ".ppm")
                env = dict(base_env)
                env["XYO_WAIT_READY_SLEEP"] = "1" if enabled else "0"
                env["XYO_PLAY_PROFILE"] = str(profile)
                env["XYO_SHOT"] = str(shot)
                command = [str(binary), "play", str(project),
                           "--scale", "1", "--fps", str(args.fps), "--turbo"]
                proc = subprocess.run(command, env=env, cwd=out,
                                      capture_output=True, text=True,
                                      timeout=args.timeout)
                Path(str(stem) + ".stdout").write_text(proc.stdout)
                Path(str(stem) + ".stderr").write_text(proc.stderr)
                if proc.returncode:
                    raise RuntimeError(
                        f"{name} run={run} {state}: exit={proc.returncode}: "
                        f"{proc.stderr[-1500:]}")
                if not profile.is_file() or not shot.is_file():
                    raise RuntimeError(f"{name} run={run} {state}: missing CSV or screenshot")
                stats = profile_stats(profile, args.frames, args.warmup)
                if stats["drawn_frames"] == 0:
                    raise RuntimeError(f"{name} run={run} {state}: no drawing occurred")
                variable_hash = None
                if args.require_parity:
                    try:
                        variables = json.loads(proc.stdout)
                    except json.JSONDecodeError as error:
                        raise RuntimeError(f"{name} run={run} {state}: no variable JSON") from error
                    stage = variables.get("Stage", {})
                    for key, expected in expected_stage.items():
                        if str(stage.get(key)) != expected:
                            raise RuntimeError(f"{name} run={run} {state}: Stage/{key}="
                                               f"{stage.get(key)!r}, expected {expected!r}")
                    canonical = json.dumps(variables, sort_keys=True, ensure_ascii=False)
                    variable_hash = hashlib.sha256(canonical.encode()).hexdigest()
                record = dict(project=name, run=run, variant=state,
                              stats=stats, shot_sha256=sha(shot),
                              vars_sha256=variable_hash)
                records.append(record)
                print(f"{name} {run} {state} "
                      f"step-p95={stats['step_p95_ms']:.3f}ms "
                      f"vm-p95={stats['vm_p95_ms']:.3f}ms "
                      f"draw-p95={stats['draw_p95_ms']:.3f}ms",
                      flush=True)

    if sha(binary) != binary_hash or any(
            sha(project) != digest for project, digest in frozen_projects.values()):
        raise RuntimeError("binary or source project changed during benchmark")

    summaries = {}
    for name in projects:
        rows = [r for r in records if r["project"] == name]
        pairs = [
            {state: next(r for r in rows if r["run"] == run and r["variant"] == state)
             for state in ("off", "on")}
            for run in range(1, args.runs + 1)
        ]
        summary = {}
        for field in ("step_p50_ms", "step_p95_ms", "step_p99_ms",
                      "vm_p95_ms", "draw_p95_ms", "interval_p95_ms"):
            off = [p["off"]["stats"][field] for p in pairs]
            on = [p["on"]["stats"][field] for p in pairs]
            summary[field] = {
                "off_median": statistics.median(off),
                "on_median": statistics.median(on),
                "paired_off_over_on": math.exp(
                    statistics.mean(math.log(a / b) for a, b in zip(off, on))
                ) if all(a > 0 and b > 0 for a, b in zip(off, on)) else None,
                "on_wins": sum(b < a for a, b in zip(off, on)),
            }
        summary["shot_equal_pairs"] = sum(
            p["off"]["shot_sha256"] == p["on"]["shot_sha256"] for p in pairs
        )
        summary["shot_total_pairs"] = len(pairs)
        if args.require_parity:
            summary["state_equal_pairs"] = sum(
                p["off"]["vars_sha256"] == p["on"]["vars_sha256"] for p in pairs)
            if (summary["shot_equal_pairs"] != len(pairs) or
                    summary["state_equal_pairs"] != len(pairs)):
                raise RuntimeError(
                    f"{name}: strict parity failure: image {summary['shot_equal_pairs']}"
                    f"/{len(pairs)}, state {summary['state_equal_pairs']}/{len(pairs)}")
        summaries[name] = summary

    report = {
        "binary": str(binary), "binary_sha256": binary_hash,
        "project_hashes": {name: digest for name, (_, digest) in frozen_projects.items()},
        "flags_fixed": {k: v for k, v in base_env.items() if k.startswith("XYO_")},
        "flag_toggled": "XYO_WAIT_READY_SLEEP",
        "environment": {k: v for k, v in base_env.items() if k.startswith("SDL_")},
        "cpu": args.cpu, "runs": args.runs, "frames": args.frames,
        "warmup": args.warmup, "fps": args.fps, "turbo": True,
        "strict_parity": args.require_parity, "expected_stage_variables": expected_stage,
        "input_injection": args.inject,
        "summary": summaries, "records": records,
    }
    (out / "results.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(summaries, indent=2), flush=True)


if __name__ == "__main__":
    main()
