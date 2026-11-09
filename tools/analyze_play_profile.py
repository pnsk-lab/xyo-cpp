#!/usr/bin/env python3
"""Summarize opt-in xyo play frame CSV; no external Python dependencies."""
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path

TIMERS = (
    "frame_interval_ms", "step_ms", "vm_ms", "draw_ms", "sprite_ms",
    "backdrop_ms", "pen_ms", "monitor_ms", "ui_ms", "present_ms",
    "costume_decode_ms", "effect_build_ms", "collision_ms",
)
COUNTS = (
    "sprite_copies", "backdrop_copies", "costume_decodes", "effect_builds",
    "effect_hits", "texture_uploads", "collision_calls",
)


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0
    xs = sorted(values)
    i = (len(xs) - 1) * fraction
    lo = int(i)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (i - lo)


def describe(rows: list[dict[str, float]], name: str) -> dict[str, float]:
    a = [r[name] for r in rows]
    return {
        "mean": round(statistics.mean(a), 3),
        "p50": round(percentile(a, .5), 3),
        "p95": round(percentile(a, .95), 3),
        "p99": round(percentile(a, .99), 3),
        "max": round(max(a), 3),
        "sum": round(sum(a), 2),
    }


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("csv", type=Path)
    p.add_argument("--json", type=Path, help="Save machine-readable summary")
    p.add_argument("--fps", type=float, default=30, help="Target frame rate")
    args = p.parse_args()
    with args.csv.open(newline="", encoding="utf-8") as f:
        source = csv.DictReader(f)
        required = {"frame", "elapsed_s", "rss_kib", "threads", *TIMERS, *COUNTS}
        if not required.issubset(source.fieldnames or []):
            p.error("CSV is missing profile columns")
        # Skip truncated final records (e.g. process killed mid-write).
        rows = []
        for record in source:
            try:
                row = {name: float(record[name]) for name in required}
            except (TypeError, ValueError):
                continue
            if all(math.isfinite(n) for n in row.values()):
                rows.append(row)
    if not rows:
        p.error("CSV contains no complete frame samples")

    # First interval has no previous frame timestamp and is not an FPS sample.
    valid_interval = [r["frame_interval_ms"] for r in rows
                      if r["frame"] != 0 and r["frame_interval_ms"] > 0]
    interval = {
        "p50_ms": round(percentile(valid_interval, .5), 3),
        "p95_ms": round(percentile(valid_interval, .95), 3),
        "p99_ms": round(percentile(valid_interval, .99), 3),
        "effective_fps_median": (
            round(1000 / percentile(valid_interval, .5), 2)
            if valid_interval else None
        ),
    }

    budget = 1000 / args.fps
    slow = [r for r in rows if r["step_ms"] > budget]
    summaries = {name: describe(rows, name) for name in TIMERS}
    # draw_ms includes sprite/pen/present. sprite_ms includes costume effects.
    contributors = [
        ("vm_ms", "Scratch execution (includes collision queries)"),
        ("draw_ms", "whole renderer (includes present/vsync)"),
        ("sprite_ms", "sprite rendering and skin/effects"),
        ("pen_ms", "pen rendering"),
        ("present_ms", "SDL_RenderPresent (may include vsync/GPU wait)"),
        ("collision_ms", "sensing/touching queries during VM"),
        ("effect_build_ms", "costume effect cache misses"),
        ("costume_decode_ms", "initial costume rasterization/upload"),
        ("monitor_ms", "variable/list monitor rendering"),
    ]
    totals = sorted(
        [{"name": label, "total_ms": round(sum(r[key] for r in rows), 2),
          "mean_ms": summaries[key]["mean"]}
         for key, label in contributors],
        key=lambda item: item["total_ms"], reverse=True,
    )

    windows: dict[int, list[dict[str, float]]] = defaultdict(list)
    for r in rows:
        windows[int(r["elapsed_s"] // 5)].append(r)
    hot_windows = sorted(
        [{"begin_s": k * 5, "end_s": k * 5 + 5,
          "frames": len(w),
          "mean_step_ms": round(statistics.mean(r["step_ms"] for r in w), 2),
          "p95_step_ms": round(percentile([r["step_ms"] for r in w], .95), 2),
          "mean_sprite_ms": round(statistics.mean(r["sprite_ms"] for r in w), 2),
          "mean_pen_ms": round(statistics.mean(r["pen_ms"] for r in w), 2),
          "mean_vm_ms": round(statistics.mean(r["vm_ms"] for r in w), 2)}
         for k, w in windows.items()],
        key=lambda w: w["p95_step_ms"], reverse=True,
    )[:8]
    top_frames = sorted(
        [{"frame": int(r["frame"]), "time_s": round(r["elapsed_s"], 2),
          "step_ms": round(r["step_ms"], 2),
          "vm_ms": round(r["vm_ms"], 2),
          "draw_ms": round(r["draw_ms"], 2),
          "sprite_ms": round(r["sprite_ms"], 2),
          "pen_ms": round(r["pen_ms"], 2),
          "effects": int(r["effect_builds"]),
          "costumes": int(r["costume_decodes"])}
         for r in rows],
        key=lambda r: r["step_ms"], reverse=True,
    )[:15]

    sprite_rows = []
    sprite_csv = Path(str(args.csv) + ".sprites.csv")
    if sprite_csv.is_file():
        with sprite_csv.open(newline="", encoding="utf-8") as f:
            for entry in csv.DictReader(f):
                try:
                    sprite_rows.append({
                        "sprite": entry["sprite"],
                        "draw_ms": round(float(entry["draw_ms"]), 2),
                        "copies": int(entry["copies"]),
                    })
                except (ValueError, KeyError):
                    continue
        sprite_rows.sort(key=lambda r: r["draw_ms"], reverse=True)

    result = {
        "source": str(args.csv),
        "frames": len(rows),
        "duration_s": round(rows[-1]["elapsed_s"] - rows[0]["elapsed_s"], 2),
        "target_fps": args.fps,
        "budget_ms": round(budget, 2),
        "slow_step_frames": len(slow),
        "slow_step_fraction": round(len(slow) / len(rows), 4),
        "observed_fps": interval,
        "timings": summaries,
        "counts": {k: int(sum(r[k] for r in rows)) for k in COUNTS},
        "rss_mib": {
            "initial": round(rows[0]["rss_kib"] / 1024, 1),
            "max": round(max(r["rss_kib"] for r in rows) / 1024, 1),
            "last": round(rows[-1]["rss_kib"] / 1024, 1),
        },
        "threads": {
            "max": int(max(r["threads"] for r in rows)),
            "last": int(rows[-1]["threads"]),
        },
        "contributors_with_overlaps": totals,
        "worst_5s_windows": hot_windows,
        "slowest_frames": top_frames,
        "per_sprite": sprite_rows,
        "per_sprite_file": str(sprite_csv) if sprite_csv.is_file() else None,
        "interpretation_note": (
            "Nested timers are not additive: draw includes sprite/pen/present; "
            "sprite includes costume decode/effect builds; vm includes collision. "
            "Present includes possible vsync and GPU backpressure, not pure GPU time. "
            "Frame interval includes requested FPS sleep."
        ),
    }
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(result, ensure_ascii=False, indent=2)
                             + "\n", encoding="utf-8")

    print(f"Samples: {result['frames']} frames / {result['duration_s']} s")
    print(f"Frame interval: median {interval['p50_ms']:.2f} ms, "
          f"p95 {interval['p95_ms']:.2f} ms, "
          f"effective median {interval['effective_fps_median']} fps")
    print(f"VM+draw step over {budget:.2f} ms: {len(slow)}/{len(rows)} frames")
    print("Timings:                  mean / p95 / p99 (ms)")
    for name in TIMERS[1:]:
        v = summaries[name]
        print(f"  {name:<24} {v['mean']:>8.2f} / "
              f"{v['p95']:>8.2f} / {v['p99']:>8.2f}")
    print("Cumulative events:")
    for name, amount in result["counts"].items():
        print(f"  {name:<24} {amount:>9}")
    print(f"RSS (MiB): {result['rss_mib']}")
    print("Worst 5-second windows:")
    for w in hot_windows[:5]:
        print(f"  {w['begin_s']:>5}-{w['end_s']:<5}s "
              f"p95 step={w['p95_step_ms']:.2f} ms "
              f"sprites={w['mean_sprite_ms']:.2f} ms "
              f"pen={w['mean_pen_ms']:.2f} ms "
              f"vm={w['mean_vm_ms']:.2f} ms")
    if sprite_rows:
        print("Sprite drawing totals (clones grouped by original sprite):")
        for entry in sprite_rows[:12]:
            print(f"  {entry['draw_ms']:>10.1f} ms  "
                  f"{entry['copies']:>8} copies  {entry['sprite']}")
    print("Note: timing subcategories overlap (see JSON interpretation_note).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
