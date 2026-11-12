#!/usr/bin/env python3
"""RTX NGX recovery contention: interleaved off/foreground/background A/B."""
from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import time

MODES = ("off", "foreground", "background")


def percentile(data: list[float], p: float) -> float:
    values = sorted(data)
    position = (len(values) - 1) * p
    lo = int(position)
    hi = min(lo + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (position - lo)


def setup(root: Path, irrelevant: int, stale: int) -> None:
    root.mkdir()
    for i in range(irrelevant):
        fd = os.open(root / f"other-{i:05}.txt",
                     os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.close(fd)
    stamp = time.time() - 2*86400
    for i in range(stale):
        # Exact six-digit frame and six-character suffix format.
        name = root / f"frame-{i+1000:06}.ppm.part.{i:06}"
        name.write_bytes(b"abandoned-PPM")
        os.utime(name, (stamp, stamp))


def measure(binary: Path, project: Path, root: Path, *,
            mode: str, frames: int, fps: int, scale: int,
            durable: bool, irrelevant: int, stale: int
            ) -> dict[str, float | int | str]:
    setup(root, irrelevant, stale)
    profile = root / "profile.csv"
    env = os.environ.copy()
    for key in (
        "DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER", "EGL_PLATFORM",
        "XYO_SHOT", "XYO_SHOT_SERIES", "XYO_SHOT_SERIES_ASYNC",
        "XYO_SHOT_ASYNC_ATOMIC", "XYO_SHOT_ASYNC_DURABLE",
        "XYO_SHOT_ASYNC_BUFFER_POOL", "XYO_SHOT_ASYNC_DIRECT_MAP",
        "XYO_SHOT_ASYNC_RGB", "XYO_SHOT_ASYNC_RECOVER_STALE",
        "XYO_SHOT_ASYNC_RECOVERY_THREAD", "XYO_SHOT_ASYNC_LOCK_SERIES",
        "XYO_SHOT_ASYNC_TEST_RECOVERY_PAUSE_MS", "XYO_SHOT_ASYNC_TEST_FAIL_STAGE",
        "XYO_SHOT_ASYNC_TEST_FAIL_AFTER", "XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS",
    ):
        env.pop(key, None)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_EXIT_FRAMES": str(frames),
        "XYO_PLAY_PROFILE": str(profile),
        "XYO_SHOT_SERIES": str(root / "frame"),
        "XYO_SHOT_SERIES_ASYNC": "1",
        "XYO_SHOT_ASYNC_RGB": "1",
        "XYO_SHOT_ASYNC_DIRECT_MAP": "1",
        "XYO_SHOT_ASYNC_ATOMIC": "1",
        "XYO_SHOT_ASYNC_DURABLE": "1" if durable else "0",
        "XYO_SHOT_ASYNC_LOCK_SERIES": "1",
        "XYO_SHOT_ASYNC_RECOVER_STALE": "0" if mode == "off" else "1",
        "XYO_SHOT_ASYNC_RECOVERY_THREAD": "1" if mode == "background" else "0",
        "XYO_SHOT_ASYNC_STATS": "1",
    })
    start = time.perf_counter()
    proc = subprocess.run(
        [str(binary), "play", str(project), "--scale", str(scale),
         "--fps", str(fps)],
        env=env, capture_output=True, text=True, check=False, timeout=100,
    )
    wall = time.perf_counter() - start
    if proc.returncode or "NVIDIA NGX initialized" not in proc.stderr:
        raise RuntimeError(
            f"{mode}: process exit={proc.returncode}\n{proc.stderr[-2600:]}"
        )
    with profile.open(newline="") as file:
        rows = list(csv.DictReader(file))
    if len(rows) != frames:
        raise RuntimeError(f"{mode}: profile rows {len(rows)}/{frames}")
    images = list(root.glob("frame-??????.ppm"))
    if len(images) != frames:
        raise RuntimeError(f"{mode}: saved images {len(images)}/{frames}")
    if f"atomic_files enabled=1 staged={frames} published={frames} discarded=0" not in proc.stderr:
        raise RuntimeError(f"{mode}: atomic output incomplete")
    progress = re.search(
        r"\[async PPM\] recovery_progress complete=(\d+) steps=(\d+) "
        r"max_step_entries=(\d+) max_step_ms=([0-9.]+) pending=(\d+)",
        proc.stderr,
    )
    thread = re.search(
        r"\[async PPM\] recovery_thread enabled=(\d+) started=(\d+) "
        r"joined=(\d+) foreground_ms=([0-9.]+)", proc.stderr,
    )
    tally = re.search(
        r"\[async PPM\] recovery checked=(\d+) removed=(\d+) "
        r"recent=(\d+) locked=(\d+) unsafe=(\d+) errors=(\d+) "
        r"dir_entries=(\d+) scan_ms=([0-9.]+)", proc.stderr,
    )
    recovered = -1
    if mode != "off":
        if not progress or not thread or not tally:
            raise RuntimeError(f"{mode}: missing recovery diagnostics")
        if (int(progress.group(1)) != 1 or int(progress.group(5)) != 0 or
                int(tally.group(2)) != stale or int(tally.group(6)) != 0):
            raise RuntimeError(f"{mode}: recovery incomplete:\n{proc.stderr[-2300:]}")
        if mode == "background" and tuple(map(int, thread.groups()[:3])) != (1,1,1):
            raise RuntimeError("background worker was not joined")
        recovered = int(tally.group(2))
    values = [float(r["draw_ms"]) for r in rows[2:]]
    step = [float(r["step_ms"]) for r in rows[2:]]
    start_step = [float(r["step_ms"]) for r in rows[2:18]]
    return {
        "mode": mode, "wall_s": wall,
        "draw_p50_ms": percentile(values, .5),
        "draw_p95_ms": percentile(values, .95),
        "draw_p99_ms": percentile(values, .99),
        "step_p50_ms": percentile(step, .5),
        "step_p95_ms": percentile(step, .95),
        "step_p99_ms": percentile(step, .99),
        "step_max_ms": max(step),
        "early_step_p95_ms": percentile(start_step, .95),
        "recovered": recovered,
        "scan_ms": float(tally.group(8)) if tally else 0.,
        "render_recovery_ms": float(thread.group(4)) if thread else 0.,
        "peak_rss_kib": max(int(r["rss_kib"]) for r in rows),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--project", type=Path,
                        default=Path(__file__).resolve().parents[2] / "debug/projects/3D2.sb3")
    parser.add_argument("--frames", type=int, default=64)
    parser.add_argument("--fps", type=int, default=1000)
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument("--blocks", type=int, default=2)
    parser.add_argument("--irrelevant", type=int, default=3200)
    parser.add_argument("--stale", type=int, default=600)
    parser.add_argument("--durable", action="store_true")
    parser.add_argument("--output-csv", type=Path,
                        help="persist raw per-trial profile statistics")
    args = parser.parse_args()
    if (args.frames < 20 or args.blocks < 1 or args.irrelevant < 0 or
            args.stale < 1 or args.scale not in (1,2,3,4)):
        parser.error("frames>=20, blocks>=1, stale>=1, scale in 1..4")
    if args.frames <= 16:
        parser.error("frames must cover the first 16 samples")

    order = (
        ("off", "foreground", "background", "background", "foreground", "off"),
        ("background", "off", "foreground", "foreground", "off", "background"),
    )
    samples: dict[str, list[dict[str, float | int | str]]] = {
        m: [] for m in MODES
    }
    trials: list[dict[str, float | int | str]] = []
    with tempfile.TemporaryDirectory(prefix="xyo-ngxsdk-recovery-contention-") as td:
        root = Path(td)
        for block in range(args.blocks):
            for idx, mode in enumerate(order[block % 2]):
                result = measure(
                    args.binary.resolve(), args.project.resolve(),
                    root / f"run-{block}-{idx}", mode=mode,
                    frames=args.frames, fps=args.fps, scale=args.scale,
                    durable=args.durable, irrelevant=args.irrelevant,
                    stale=args.stale,
                )
                samples[mode].append(result)
                trials.append({"block": block, "trial": idx, **result})
                print(
                    f"block={block} trial={idx} mode={mode:<10} "
                    f"step_p95={result['step_p95_ms']:.3f} ms "
                    f"step_p99={result['step_p99_ms']:.3f} ms "
                    f"draw_p95={result['draw_p95_ms']:.3f} ms "
                    f"early_step_p95={result['early_step_p95_ms']:.3f} ms "
                    f"wall={result['wall_s']:.3f} s "
                    f"recovered={result['recovered']}",
                    flush=True,
                )
    if args.output_csv:
        args.output_csv.parent.mkdir(parents=True, exist_ok=True)
        with args.output_csv.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(trials[0]))
            writer.writeheader()
            writer.writerows(trials)
        print(f"saved {len(trials)} raw trial metrics to {args.output_csv}",
              flush=True)
    print("SUMMARY mean trial metrics", flush=True)
    for mode in MODES:
        results = samples[mode]
        fields = ("step_p50_ms", "step_p95_ms", "step_p99_ms",
                  "draw_p50_ms", "draw_p95_ms",
                  "early_step_p95_ms", "wall_s", "scan_ms",
                  "render_recovery_ms")
        report = " ".join(
            f"{key}={statistics.mean(float(row[key]) for row in results):.3f}"
            for key in fields
        )
        print(f"{mode:10} {report}", flush=True)
    print("NOTE: GPU contention, OS page cache and fsync/join can affect wall and tail times.")


if __name__ == "__main__":
    main()
