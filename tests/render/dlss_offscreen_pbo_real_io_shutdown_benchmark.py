#!/usr/bin/env python3
"""Measure real Btrfs/NVMe contention on the async PPM recovery shutdown.

Uses actual concurrent buffered writes + fdatasync and directory metadata
create/unlink on the *same* filesystem. No fake sleep is used by default.
Separate output directories are created per trial; only those are deleted.
"""
from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import threading
import time

from dlss_pen_primitive_smoke import make_case

MiB = 1024 * 1024
BLOCK = b"0123456789abcdef" * 65536  # 1 MiB


def prepare(path: Path, stale: int, unrelated: int) -> None:
    path.mkdir(parents=True)
    age = time.time() - 2 * 86400
    for i in range(stale):
        p = path / f"frame-{1000+i:06d}.ppm.part.{i:06d}"
        with p.open("xb") as f:
            f.write(b"old unfinished PPM")
        os.utime(p, (age, age))
    for i in range(unrelated):
        (path / f"unrelated-{i:05d}.txt").touch()


def create_env(prefix: Path, frames: int, direct: bool, delay_ms: int) -> dict[str, str]:
    env = os.environ.copy()
    for key in (
        "DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER", "EGL_PLATFORM",
        "XYO_SHOT_ASYNC_TEST_RECOVERY_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_RECOVERY_ENTRY_DELAY_MS",
        "XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS",
        "XYO_SHOT_ASYNC_TEST_FAIL_AFTER", "XYO_SHOT_ASYNC_TEST_FAIL_STAGE",
        "XYO_SHOT_ASYNC_TEST_FAIL_SYNC", "XYO_SHOT_ASYNC_TEST_FAIL_WRITE_ROW",
    ):
        env.pop(key, None)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_EXIT_FRAMES": str(frames),
        "XYO_SHOT_SERIES": str(prefix),
        "XYO_SHOT_SERIES_ASYNC": "1",
        "XYO_SHOT_ASYNC_DIRECT_MAP": "1" if direct else "0",
        "XYO_SHOT_ASYNC_RGB": "1",
        "XYO_SHOT_ASYNC_DURABLE": "1",
        "XYO_SHOT_ASYNC_RECOVER_STALE": "1",
        "XYO_SHOT_ASYNC_RECOVERY_THREAD": "1",
        "XYO_SHOT_ASYNC_LOCK_SERIES": "1",
        "XYO_SHOT_ASYNC_STATS": "1",
    })
    if delay_ms:
        env["XYO_SHOT_ASYNC_TEST_RECOVERY_ENTRY_DELAY_MS"] = str(delay_ms)
    return env


class StorageLoad:
    def __init__(self, directory: Path, write_threads: int,
                 max_mib_per_writer: int, pause_ms: int) -> None:
        self.directory = directory
        self.write_threads = write_threads
        self.max_bytes_per_writer = max_mib_per_writer * MiB
        self.pause_s = pause_ms / 1000
        self.stop = threading.Event()
        self.workers: list[threading.Thread] = []
        self.synchronized_bytes = 0
        self.syncs = 0
        self.metadata_changes = 0
        self.errors: list[str] = []
        self.guard = threading.Lock()

    def start(self) -> None:
        for idx in range(self.write_threads):
            t = threading.Thread(target=self.write, args=(idx,), daemon=False)
            self.workers.append(t)
            t.start()
        t = threading.Thread(target=self.metadata, daemon=False)
        self.workers.append(t)
        t.start()

    def write(self, idx: int) -> None:
        path = self.directory / f"stress-data-{idx}.bin"
        count = 0
        total = 0
        try:
            # Cap total logical writes as well as the 16-MiB file length:
            # Btrfs CoW can allocate new extents even for overwrites.
            with path.open("wb", buffering=0) as file:
                while (not self.stop.is_set() and
                       total < self.max_bytes_per_writer):
                    for _ in range(8):
                        if self.stop.is_set() or total >= self.max_bytes_per_writer:
                            break
                        file.write(BLOCK)
                        count += len(BLOCK)
                        total += len(BLOCK)
                    if not count:
                        break
                    os.fdatasync(file.fileno())
                    with self.guard:
                        self.synchronized_bytes += count
                        self.syncs += 1
                    count = 0
                    if file.tell() >= 16 * MiB:
                        file.seek(0)
                    if self.stop.wait(self.pause_s):
                        break
        except OSError as exc:
            with self.guard:
                self.errors.append(f"data writer {idx}: {exc}")

    def metadata(self) -> None:
        index = 0
        try:
            while not self.stop.is_set():
                path = self.directory / f"stress-meta-{index%512:03d}.tmp"
                with path.open("wb") as file:
                    file.write(b"metadata write")
                path.unlink()
                index += 1
                with self.guard:
                    self.metadata_changes = index
        except OSError as exc:
            with self.guard:
                self.errors.append(f"metadata writer: {exc}")

    def finish(self) -> None:
        self.stop.set()
        for t in self.workers:
            t.join(timeout=10)
            if t.is_alive():
                raise RuntimeError("storage-load thread did not exit")
        if self.errors:
            raise RuntimeError("; ".join(self.errors))


def trial(binary: Path, scene: Path, root: Path, *,
          stress: bool, direct: bool, frames: int,
          stale: int, unrelated: int, delay_ms: int,
          threads: int, max_mib_per_writer: int,
          io_pause_ms: int) -> dict[str, object]:
    capture_root = root / "shots"
    prepare(capture_root, stale, unrelated)
    storage = StorageLoad(root, threads, max_mib_per_writer, io_pause_ms)
    if stress:
        storage.start()
    try:
        started = time.perf_counter()
        run = subprocess.run(
            [str(binary), "play", str(scene), "--scale", "2", "--fps", "1000"],
            env=create_env(capture_root/"frame", frames, direct, delay_ms),
            text=True, capture_output=True, check=False, timeout=70,
        )
        wall = time.perf_counter() - started
    finally:
        if stress:
            storage.finish()

    if stress and (not storage.syncs or not storage.metadata_changes):
        raise RuntimeError("storage pressure failed to produce disk or metadata I/O")
    if run.returncode or "NVIDIA NGX initialized" not in run.stderr:
        raise RuntimeError(f"xyo failed (exit {run.returncode}):\n{run.stderr[-2700:]}")
    m = re.search(
        r"\[async PPM\] recovery_shutdown joined_ms=([0-9.]+) cancel_observed=(\d+)",
        run.stderr,
    )
    progress = re.search(
        r"\[async PPM\] recovery_progress complete=(\d+) steps=(\d+) "
        r"max_step_entries=(\d+) max_step_ms=([0-9.]+) pending=(\d+)",
        run.stderr,
    )
    recovery = re.search(
        r"\[async PPM\] recovery checked=(\d+) removed=(\d+) "
        r"recent=(\d+) locked=(\d+) unsafe=(\d+) errors=(\d+) "
        r"dir_entries=(\d+) scan_ms=([0-9.]+)", run.stderr,
    )
    if not m or not progress or not recovery:
        raise RuntimeError(f"missing scanner statistics:\n{run.stderr[-2800:]}")
    if int(recovery.group(6)) or int(progress.group(3)) > 128:
        raise RuntimeError(f"recovery invariant violated: {run.stderr[-2300:]}")
    output = sorted(capture_root.glob("frame-??????.ppm"))
    if len(output) != frames:
        raise RuntimeError(f"saved {len(output)} of {frames} PPM files")
    for path in output:
        expected_size = len(b"P6\n960 720\n255\n") + 960*720*3
        if path.stat().st_size != expected_size:
            raise RuntimeError(f"invalid PPM: {path}")
    if (f"atomic_files enabled=1 staged={frames} published={frames} discarded=0"
        not in run.stderr or
        f"durability enabled=1 file_fsyncs={frames} dir_fsyncs={frames} sync_failures=0"
        not in run.stderr):
        raise RuntimeError("PPM atomic/durability counts differ")

    return {
        "stress": int(stress),
        "direct": int(direct),
        "frames": frames,
        "stale": stale,
        "unrelated": unrelated,
        "test_entry_delay_ms": delay_ms,
        "join_ms": float(m.group(1)),
        "cancel_observed": int(m.group(2)),
        "scan_complete": int(progress.group(1)),
        "scan_steps": int(progress.group(2)),
        "scanned_entries": int(recovery.group(7)),
        "removed": int(recovery.group(2)),
        "max_step_ms": float(progress.group(4)),
        "scan_ms": float(recovery.group(8)),
        "wall_s": wall,
        "io_synced_mib": round(storage.synchronized_bytes/MiB, 1),
        "io_fsyncs": storage.syncs,
        "metadata_ops": storage.metadata_changes,
    }


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("binary", type=Path)
    p.add_argument("--trials", type=int, default=2)
    p.add_argument("--frames", type=int, default=4)
    p.add_argument("--stale", type=int, default=3000)
    p.add_argument("--unrelated", type=int, default=1000)
    p.add_argument("--write-threads", type=int, default=2)
    p.add_argument("--max-mib-per-writer", type=int, default=512,
                   help="strict cap on logical MB written by each stress thread")
    p.add_argument("--io-pause-ms", type=int, default=25,
                   help="pause after each real 8 MiB write+fdatasync batch")
    p.add_argument("--entry-delay-ms", type=int, default=0,
                   help="test-only delay; 0 means genuine filesystem latency only")
    p.add_argument("--output-csv", type=Path)
    args = p.parse_args()
    if not (1 <= args.trials <= 20 and 1 <= args.frames <= 256 and
            1 <= args.stale <= 50_000 and 0 <= args.unrelated <= 50_000 and
            1 <= args.write_threads <= 4 and 0 <= args.entry_delay_ms <= 100 and
            8 <= args.max_mib_per_writer <= 1024 and 1 <= args.io_pause_ms <= 100):
        p.error("unreasonable fixture/workload settings")
    with tempfile.TemporaryDirectory(prefix="xyo-ppm-real-disk-contention-") as temp:
        root = Path(temp)
        scene = root / "scene.sb3"
        make_case(scene, "replay")
        rows: list[dict[str, object]] = []
        for trial_idx in range(args.trials):
            for direct in (False, True):
                for stress in ((False, True) if trial_idx % 2 == 0 else (True, False)):
                    out = trial(
                        args.binary.resolve(), scene,
                        root / f"case-{trial_idx}-{int(direct)}-{int(stress)}",
                        stress=stress, direct=direct,
                        frames=args.frames,
                        stale=args.stale, unrelated=args.unrelated,
                        delay_ms=args.entry_delay_ms, threads=args.write_threads,
                        max_mib_per_writer=args.max_mib_per_writer,
                        io_pause_ms=args.io_pause_ms,
                    )
                    rows.append({"trial": trial_idx, **out})
                    print(
                        f"run={trial_idx} direct={int(direct)} stress={int(stress)} "
                        f"joined={out['join_ms']:.3f}ms "
                        f"entries={out['scanned_entries']} "
                        f"complete={out['scan_complete']} "
                        f"wall={out['wall_s']:.3f}s "
                        f"sync_MiB={out['io_synced_mib']} "
                        f"metadata_ops={out['metadata_ops']}",
                        flush=True,
                    )
    if args.output_csv:
        args.output_csv.parent.mkdir(parents=True, exist_ok=True)
        with args.output_csv.open("w", newline="") as file:
            writer = csv.DictWriter(file, list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        print(f"saved {len(rows)} trial rows to {args.output_csv}")
    for direct in (False, True):
        for stressed in (False, True):
            subset = [r for r in rows if r["direct"] == int(direct)
                      and r["stress"] == int(stressed)]
            values = [float(r["join_ms"]) for r in subset]
            print(
                f"SUMMARY direct={int(direct)} stress={int(stressed)} "
                f"join_mean={statistics.mean(values):.3f}ms "
                f"join_max={max(values):.3f}ms "
                f"n={len(values)}",
                flush=True,
            )


if __name__ == "__main__":
    main()
