#!/usr/bin/env python3
"""A/B SDL/EGL offscreen DLSS capture: synchronous vs PBO + disk worker."""
from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time


def run(binary: Path, project: Path, root: Path, index: int, mode: str,
        frames: int, fps: int, scale: int = 2
        ) -> tuple[float, float, int, int]:
    env = os.environ.copy()
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER",
                "EGL_PLATFORM", "XYO_SHOT", "XYO_SHOT_SERIES",
                "XYO_SHOT_EVERY", "XYO_SHOT_SERIES_ASYNC",
                "XYO_SHOT_ASYNC_RGB", "XYO_SHOT_ASYNC_BUFFER_POOL",
                "XYO_SHOT_ASYNC_DIRECT_MAP", "XYO_SHOT_ASYNC_STAGE_PROFILE",
                "XYO_SHOT_ASYNC_ATOMIC", "XYO_SHOT_ASYNC_DURABLE",
                "XYO_SHOT_ASYNC_TEST_FAIL_SYNC",
                "XYO_SHOT_ASYNC_TEST_FAIL_WRITE_ROW"):
        env.pop(key, None)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_EXIT_FRAMES": str(frames),
        "XYO_PLAY_PROFILE": str(root / f"profile-{index}.csv"),
    })
    if mode != "none":
        env.update({
            "XYO_SHOT_SERIES": str(root / f"frame-{index}"),
            "XYO_SHOT_SERIES_ASYNC": "1" if mode.startswith("pbo") else "0",
            "XYO_SHOT_ASYNC_RGB": "1" if mode.startswith("pbo-rgb") else "0",
            "XYO_SHOT_ASYNC_BUFFER_POOL": "1" if mode.endswith("-pool") else "0",
            "XYO_SHOT_ASYNC_DIRECT_MAP": "1" if "-direct" in mode else "0",
            "XYO_SHOT_ASYNC_ATOMIC": "1" if mode.endswith("-atomic") else "0",
            "XYO_SHOT_ASYNC_DURABLE": "1" if mode.endswith("-durable") else "0",
        })
    before = time.perf_counter()
    result = subprocess.run(
        [str(binary), "play", str(project), "--scale", str(scale),
         "--fps", str(fps)],
        env=env, capture_output=True, text=True, timeout=150, check=False,
    )
    wall = time.perf_counter() - before
    if result.returncode or "NVIDIA NGX initialized" not in result.stderr:
        raise RuntimeError(
            f"{mode} returned {result.returncode}:\n{result.stderr[-2000:]}"
        )
    with (root / f"profile-{index}.csv").open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != frames:
        raise RuntimeError(f"expected {frames} profile rows, got {len(rows)}")
    count = len(list(root.glob(f"frame-{index}-*.ppm")))
    if count != (0 if mode == "none" else frames):
        raise RuntimeError(f"{mode}: missing PPM output ({count}/{frames})")
    peak_rss_kib = max(int(row["rss_kib"]) for row in rows)
    return (statistics.median(float(row["draw_ms"])
                              for row in rows[2:]), wall, count, peak_rss_kib)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument(
        "--project", type=Path,
        default=Path(__file__).resolve().parents[2] / "debug/projects/3D2.sb3"
    )
    parser.add_argument("--frames", type=int, default=40)
    parser.add_argument("--fps", type=int, default=1000)
    parser.add_argument("--blocks", type=int, default=1)
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument("--compare-rgb", action="store_true",
                        help="interleaved RGBA-PBO vs RGB-PBO A/B")
    parser.add_argument("--compare-pool", action="store_true",
                        help="interleaved RGB-PBO with/without CPU buffer recycling")
    parser.add_argument("--compare-direct", action="store_true",
                        help="interleaved RGB-PBO copy vs direct PBO writer")
    parser.add_argument("--compare-atomic", action="store_true",
                        help="interleaved direct RGB PBO with/without atomic file publishing")
    parser.add_argument("--compare-durable", action="store_true",
                        help="interleaved atomic RGB direct PBO vs fsync-durable mode")
    args = parser.parse_args()
    if sum((args.compare_rgb, args.compare_pool,
            args.compare_direct, args.compare_atomic,
            args.compare_durable)) > 1:
        parser.error("choose only one --compare-* option")
    if (args.blocks < 1 or args.frames < 4 or args.fps < 1 or
            not 1 <= args.scale <= 4):
        parser.error("blocks>=1, frames>=4, fps>=1, scale in 1..4")

    if args.compare_durable:
        order = ("pbo-rgb-direct-atomic", "pbo-rgb-direct-durable",
                 "pbo-rgb-direct-durable", "pbo-rgb-direct-atomic")
    elif args.compare_atomic:
        order = ("pbo-rgb-direct", "pbo-rgb-direct-atomic",
                 "pbo-rgb-direct-atomic", "pbo-rgb-direct")
    elif args.compare_direct:
        order = ("pbo-rgb", "pbo-rgb-direct", "pbo-rgb-direct", "pbo-rgb")
    elif args.compare_pool:
        order = ("pbo-rgb", "pbo-rgb-pool", "pbo-rgb-pool", "pbo-rgb")
    elif args.compare_rgb:
        order = ("pbo", "pbo-rgb", "pbo-rgb", "pbo")
    else:
        order = ("none", "sync", "pbo", "pbo", "sync", "none")
    samples: dict[str, list[tuple[float, float, int]]] = {
        mode: [] for mode in order
    }
    with tempfile.TemporaryDirectory(prefix="xyo-dlss-pbo-benchmark-") as temp:
        root = Path(temp)
        for block in range(args.blocks):
            if block % 2 == 0:
                run_order = order
            elif args.compare_durable:
                run_order = ("pbo-rgb-direct-durable", "pbo-rgb-direct-atomic",
                             "pbo-rgb-direct-atomic", "pbo-rgb-direct-durable")
            elif args.compare_atomic:
                run_order = ("pbo-rgb-direct-atomic", "pbo-rgb-direct",
                             "pbo-rgb-direct", "pbo-rgb-direct-atomic")
            elif args.compare_direct:
                run_order = ("pbo-rgb-direct", "pbo-rgb",
                             "pbo-rgb", "pbo-rgb-direct")
            elif args.compare_pool:
                run_order = ("pbo-rgb-pool", "pbo-rgb",
                             "pbo-rgb", "pbo-rgb-pool")
            elif args.compare_rgb:
                run_order = ("pbo-rgb", "pbo", "pbo", "pbo-rgb")
            else:
                run_order = ("none", "pbo", "sync", "sync", "pbo", "none")
            for slot, mode in enumerate(run_order):
                index = block * len(order) + slot
                draw, wall, _, peak_rss = run(
                    args.binary.resolve(), args.project.resolve(),
                    root, index, mode, args.frames, args.fps, args.scale
                )
                samples[mode].append((draw, wall, peak_rss))
                print(f"block={block + 1} trial={slot + 1} {mode} "
                      f"median_draw_ms={draw:.4f} wall_s={wall:.4f} "
                      f"peak_rss_kib={peak_rss}",
                      flush=True)
    modes = (("pbo-rgb-direct-atomic", "pbo-rgb-direct-durable")
             if args.compare_durable
             else ("pbo-rgb-direct", "pbo-rgb-direct-atomic")
             if args.compare_atomic
             else ("pbo-rgb", "pbo-rgb-direct") if args.compare_direct
             else ("pbo-rgb", "pbo-rgb-pool") if args.compare_pool
             else ("pbo", "pbo-rgb") if args.compare_rgb
             else ("none", "sync", "pbo"))
    for mode in modes:
        sample = samples[mode]
        print(f"{mode}: mean run-median draw_ms="
              f"{statistics.mean(value[0] for value in sample):.4f}, "
              f"mean process wall_s="
              f"{statistics.mean(value[1] for value in sample):.4f}, "
              f"mean sampled peak_rss_kib="
              f"{statistics.mean(value[2] for value in sample):.0f}")
    if args.compare_durable:
        plain = samples["pbo-rgb-direct-atomic"]
        sync = samples["pbo-rgb-direct-durable"]
        plain_wall = statistics.mean(x[1] for x in plain)
        sync_wall = statistics.mean(x[1] for x in sync)
        print(f"fsync durability overhead vs atomic: mean wall "
              f"{(sync_wall / plain_wall - 1) * 100:.2f}%; "
              f"draw change {(statistics.mean(x[0] for x in sync) / statistics.mean(x[0] for x in plain) - 1) * 100:.2f}%")
    elif args.compare_atomic:
        plain = samples["pbo-rgb-direct"]
        atom = samples["pbo-rgb-direct-atomic"]
        plain_wall = statistics.mean(x[1] for x in plain)
        atomic_wall = statistics.mean(x[1] for x in atom)
        print(f"Atomic publication: mean wall overhead "
              f"{(atomic_wall / plain_wall - 1) * 100:.2f}%; "
              f"draw change {(statistics.mean(x[0] for x in atom) / statistics.mean(x[0] for x in plain) - 1) * 100:.2f}%")
    elif args.compare_direct:
        copied = statistics.mean(x[0] for x in samples["pbo-rgb"])
        direct = statistics.mean(x[0] for x in samples["pbo-rgb-direct"])
        copied_wall = statistics.mean(x[1] for x in samples["pbo-rgb"])
        direct_wall = statistics.mean(x[1] for x in samples["pbo-rgb-direct"])
        print(f"Direct mapped PBO vs CPU copy: draw reduction "
              f"{(1 - direct / copied) * 100:.2f}%; "
              f"wall reduction {(1 - direct_wall / copied_wall) * 100:.2f}%")
    elif args.compare_pool:
        no_pool = statistics.mean(x[0] for x in samples["pbo-rgb"])
        pool = statistics.mean(x[0] for x in samples["pbo-rgb-pool"])
        print(f"CPU buffer reuse vs allocation: draw reduction "
              f"{(1 - pool / no_pool) * 100:.2f}%")
    elif args.compare_rgb:
        rgba = statistics.mean(x[0] for x in samples["pbo"])
        rgb = statistics.mean(x[0] for x in samples["pbo-rgb"])
        print(f"RGB vs RGBA PBO: draw reduction "
              f"{(1 - rgb / rgba) * 100:.2f}%")
    else:
        sync = statistics.mean(x[0] for x in samples["sync"])
        pbo = statistics.mean(x[0] for x in samples["pbo"])
        print(f"PBO vs synchronous: draw reduction "
              f"{(1 - pbo / sync) * 100:.2f}%")


if __name__ == "__main__":
    main()
