#!/usr/bin/env python3
"""Large-directory stale PPM cleanup: bounded incremental work per frame."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

from dlss_pen_primitive_smoke import make_case

UNRELATED_FILES = 3200
FRAME_BUDGET = 128
FULL_FRAMES = 48


def capture(binary: Path, project: Path, prefix: Path,
            *, frames: int, direct: bool) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in (
        "DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER", "EGL_PLATFORM",
        "XYO_SHOT_ASYNC_TEST_STAGE_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_FAIL_STAGE",
        "XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS",
        "XYO_SHOT_ASYNC_TEST_ABORT_BEFORE_ENQUEUE",
        "XYO_SHOT_ASYNC_TEST_FAIL_AFTER",
        "XYO_SHOT_ASYNC_TEST_FAIL_WRITE_ROW",
        "XYO_SHOT_ASYNC_TEST_FAIL_SYNC",
        "XYO_SHOT_ASYNC_LOCK_SERIES",
    ):
        env.pop(key, None)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_SHOT_SERIES": str(prefix),
        "XYO_SHOT_SERIES_ASYNC": "1",
        "XYO_SHOT_ASYNC_ATOMIC": "1",
        "XYO_SHOT_ASYNC_RGB": "1",
        "XYO_SHOT_ASYNC_DIRECT_MAP": "1" if direct else "0",
        "XYO_SHOT_ASYNC_RECOVER_STALE": "1",
        "XYO_SHOT_ASYNC_STATS": "1",
        "XYO_EXIT_FRAMES": str(frames),
    })
    return subprocess.run(
        [str(binary), "play", str(project), "--scale", "2", "--fps", "1000"],
        capture_output=True, text=True, check=False, env=env, timeout=80,
    )


def metrics(result: subprocess.CompletedProcess[str]) -> dict[str, int | float]:
    if result.returncode or "NVIDIA NGX initialized" not in result.stderr:
        raise RuntimeError(f"NGX capture failed:\n{result.stderr[-2400:]}")
    old = re.search(
        r"\[async PPM\] recovery checked=(\d+) removed=(\d+) "
        r"recent=(\d+) locked=(\d+) unsafe=(\d+) errors=(\d+) "
        r"dir_entries=(\d+) scan_ms=([0-9.]+)",
        result.stderr,
    )
    new = re.search(
        r"\[async PPM\] recovery_progress complete=(\d+) steps=(\d+) "
        r"max_step_entries=(\d+) max_step_ms=([0-9.]+) pending=(\d+)",
        result.stderr,
    )
    if not old or not new:
        raise RuntimeError(f"missing recovery metrics:\n{result.stderr[-2800:]}")
    values = [int(x) for x in old.groups()[:7]] + [float(old.group(8))]
    values += [int(x) for x in new.groups()[:3]]
    values += [float(new.group(4)), int(new.group(5))]
    names = (
        "checked", "removed", "recent", "locked", "unsafe", "errors",
        "entries", "scan_ms", "complete", "steps", "max_step_entries",
        "max_step_ms", "pending",
    )
    data = dict(zip(names, values))
    if not 0 < data["max_step_entries"] <= FRAME_BUDGET:
        raise RuntimeError(f"recovery exceeded per-frame entry cap: {data}")
    if data["entries"] > FRAME_BUDGET * data["steps"]:
        raise RuntimeError(f"inconsistent step counts: {data}")
    if data["errors"]:
        raise RuntimeError(f"recovery reported errors: {data}")
    return data


def verify(binary: Path, project: Path, root: Path, *, direct: bool) -> None:
    root.mkdir()
    prefix = root / "frame"
    for i in range(UNRELATED_FILES):
        (root / f"unrelated-{i:05}.txt").write_bytes(b"x")
    old = []
    timestamp = time.time() - 2*86400
    for i in (99, 100, 101):
        path = root / f"frame-{i:06d}.ppm.part.Aa{i:04d}"
        path.write_bytes(b"abandoned-PPM")
        os.utime(path, (timestamp, timestamp))
        old.append(path)

    # After a single frame, the previous implementation scanned all ~3200
    # entries. The incremental code must stop with work still pending.
    first = capture(binary, project, prefix, frames=1, direct=direct)
    sample = metrics(first)
    if (sample["complete"] != 0 or sample["pending"] != 1 or
            sample["steps"] != 1 or sample["entries"] > FRAME_BUDGET):
        raise RuntimeError(f"single-frame scan was not bounded: {sample}")

    # Directory enumeration order is not guaranteed: even after creating
    # unrelated files first, one early scan may encounter and remove old
    # candidates. Restore the same stale fixtures for the full-scan phase.
    for path in old:
        path.write_bytes(b"abandoned-PPM")
        os.utime(path, (timestamp, timestamp))

    # Over successive real rendered frames the same directory cursor
    # advances, eventually reaches EOF and removes old matching PPM parts.
    all_frames = capture(binary, project, prefix,
                         frames=FULL_FRAMES, direct=direct)
    finished = metrics(all_frames)
    if (finished["complete"] != 1 or finished["pending"] != 0 or
            finished["entries"] < UNRELATED_FILES or
            finished["removed"] < 1 or
            finished["steps"] <= 1):
        raise RuntimeError(f"long-running capture did not complete scan: {finished}")
    if any(file.exists() for file in old):
        raise RuntimeError("eligible old PPM parts remain after completed scan")
    for i in (1, FULL_FRAMES):
        frame = root / f"frame-{i:06d}.ppm"
        expected_size = len(b"P6\n960 720\n255\n") + 960 * 720 * 3
        if not frame.is_file() or frame.stat().st_size != expected_size:
            raise RuntimeError(f"invalid saved PPM frame: {frame}")
    if len(list(root.glob("frame-??????.ppm"))) != FULL_FRAMES:
        raise RuntimeError("some completed screenshots are missing")
    if "atomic_files enabled=1 staged=48 published=48 discarded=0" not in all_frames.stderr:
        raise RuntimeError("atomic publication did not complete 48 PPMs")
    if (root / "unrelated-00000.txt").read_bytes() != b"x":
        raise RuntimeError("unrelated file was modified")
    print(
        f"{'direct' if direct else 'copied'}: single-frame entries={sample['entries']}, "
        f"complete=0; full entries={finished['entries']}, "
        f"steps={finished['steps']}, max_step_entries={finished['max_step_entries']}, "
        f"max_step_ms={finished['max_step_ms']:.3f}, "
        f"old_part_removed={finished['removed']}, "
        "saved=48/48",
        flush=True,
    )


def verify_large_directory(binary: Path, project: Path, root: Path) -> None:
    root.mkdir()
    # An additional, opt-in large-dir case; the normal CTest stays quick.
    for i in range(25_000):
        fd = os.open(root / f"other-{i:05}.txt",
                     os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.close(fd)
    for direct in (False, True):
        prefix = root / ("direct" if direct else "copied")
        sample = metrics(capture(binary, project, prefix,
                                 frames=1, direct=direct))
        if (sample["steps"] != 1 or sample["entries"] > FRAME_BUDGET or
                sample["complete"] or not sample["pending"]):
            raise RuntimeError(f"25k files exceeded first-frame budget: {sample}")
        print(
            f"{'direct' if direct else 'copied'}: 25000 filenames, "
            f"first frame scanned {sample['entries']}, "
            f"step={sample['max_step_ms']:.3f}ms, pending=1",
            flush=True,
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="xyo CLI binary")
    parser.add_argument("--large-only", action="store_true",
                        help="25,000 unrelated files and single-frame scan")
    args = parser.parse_args()
    binary = args.binary.resolve()
    with tempfile.TemporaryDirectory(prefix="xyo-ngxsdk-incremental-recovery-") as temp:
        root = Path(temp)
        project = root / "scene.sb3"
        make_case(project, "replay")
        if args.large_only:
            verify_large_directory(binary, project, root / "large")
        else:
            for direct in (False, True):
                verify(binary, project, root / ("direct" if direct else "copied"),
                       direct=direct)
    print("PASS incremental bounded stale PPM recovery in large directories")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as err:
        print(f"FAIL: {err}", file=sys.stderr)
        sys.exit(1)
