#!/usr/bin/env python3
"""Actual SIGKILL between durable recovery batches, followed by restart."""
from __future__ import annotations

import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time

from dlss_pen_primitive_smoke import make_case
from dlss_offscreen_pbo_sigkill_recovery_smoke import command, environment


def run_case(binary: Path, scene: Path, root: Path, *, direct: bool) -> None:
    root.mkdir(parents=True)
    prefix = root / "frame"
    stamp = time.time() - 2 * 86400
    for n in range(130):
        file = root / f"frame-{n+1000:06d}.ppm.part.{n:06d}"
        file.write_bytes(b"abandoned-p6")
        os.utime(file, (stamp, stamp))

    # Exact fixtures are 001000..001129; match by strict numeric range
    # rather than glob patterns that could also catch new .part files.
    def old_remaining() -> list[Path]:
        return [
            root / f"frame-{n+1000:06d}.ppm.part.{n:06d}"
            for n in range(130)
            if (root / f"frame-{n+1000:06d}.ppm.part.{n:06d}").exists()
        ]

    env = environment(prefix, direct=direct, recover=True,
                      lock_series=True)
    for key in (
        "XYO_SHOT_ASYNC_TEST_AFTER_RECOVERY_BATCH_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_POST_UNLINK_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_PRE_UNLINK_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_FAIL_RECOVERY_SYNC",
        "XYO_SHOT_ASYNC_RECOVERY_THREAD",
    ):
        env.pop(key, None)
    env["XYO_SHOT_ASYNC_RECOVERY_THREAD"] = "1"
    env["XYO_SHOT_ASYNC_TEST_AFTER_RECOVERY_BATCH_PAUSE_MS"] = "8000"
    log_path = root / "killed-writer.stderr"
    with log_path.open("wb", buffering=0) as log:
        proc = subprocess.Popen(
            command(binary, scene), env=env, stdout=subprocess.DEVNULL,
            stderr=log,
        )
        try:
            deadline = time.monotonic() + 25
            marker = "[async PPM] test first recovery batch fsynced removed=64"
            while time.monotonic() < deadline:
                if marker in log_path.read_text(errors="replace"):
                    break
                if proc.poll() is not None:
                    break
                time.sleep(0.025)
            else:
                raise RuntimeError("writer did not reach first fsynced 64-file batch")

            if marker not in log_path.read_text(errors="replace"):
                raise RuntimeError(
                    "writer exited before fsync checkpoint: "
                    + log_path.read_text(errors="replace")[-2200:]
                )
            if proc.poll() is not None:
                raise RuntimeError("writer already exited before SIGKILL")
            if len(old_remaining()) != 66:
                raise RuntimeError(
                    f"unexpected surviving old-file count at checkpoint: "
                    f"{len(old_remaining())}"
                )
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait(timeout=6)

    if proc.returncode != -signal.SIGKILL:
        raise RuntimeError(f"expected actual SIGKILL, got {proc.returncode}")
    if len(old_remaining()) != 66:
        raise RuntimeError("SIGKILL did not preserve exactly 64 committed unlinks")
    if not (root / ".frame.xyo-capture.lock").is_file():
        raise RuntimeError("series lockfile disappeared after SIGKILL")

    retry_env = env.copy()
    retry_env.pop("XYO_SHOT_ASYNC_TEST_AFTER_RECOVERY_BATCH_PAUSE_MS")
    retry_env["XYO_EXIT_FRAMES"] = "24"
    recovered = subprocess.run(
        command(binary, scene), env=retry_env,
        capture_output=True, text=True, check=False, timeout=70,
    )
    if recovered.returncode != 0:
        raise RuntimeError(
            f"restart after SIGKILL failed ({recovered.returncode}):\n"
            f"{recovered.stderr[-2800:]}"
        )
    metrics = re.search(
        r"\[async PPM\] recovery_durability enabled=1 "
        r"dir_fsyncs=(\d+) synced_removals=(\d+) "
        r"sync_failures=(\d+) sync_batch=(\d+) max_unsynced=(\d+)",
        recovered.stderr,
    )
    if not metrics or tuple(map(int, metrics.groups())) != (2, 66, 0, 64, 64):
        raise RuntimeError(
            "recovery after SIGKILL did not fsync 64+2: "
            + recovered.stderr[-2800:]
        )
    if old_remaining():
        raise RuntimeError(f"{len(old_remaining())} aged tempfiles still remain")
    if "series_lock enabled=1 acquired=1" not in recovered.stderr:
        raise RuntimeError("restarted writer did not reacquire series lock")
    if "recovery_progress complete=1" not in recovered.stderr:
        raise RuntimeError("restart did not complete its scan")
    if "durability enabled=1 file_fsyncs=24 dir_fsyncs=24 sync_failures=0" not in recovered.stderr:
        raise RuntimeError("durable image output regressed")
    frames = list(root.glob("frame-??????.ppm"))
    if len(frames) != 24:
        raise RuntimeError(f"expected 24 saved PPMs, found {len(frames)}")
    expected_size = len(b"P6\n960 720\n255\n") + 960 * 720 * 3
    if any(frame.stat().st_size != expected_size for frame in frames):
        raise RuntimeError("invalid image size after forced-kill recovery")
    print(
        f"{'direct' if direct else 'copied'}: actual SIGKILL after "
        "first directory fsync (64 files); remaining=66; "
        "restart fsynced 64+2=66; 24/24 durable PPM",
        flush=True,
    )


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_offscreen_pbo_recovery_batch_sigkill_smoke.py <xyo>")
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xyo-recovery-batch-sigkill-") as td:
        root = Path(td)
        scene = root / "project.sb3"
        make_case(scene, "replay")
        for direct in (False, True):
            run_case(binary, scene,
                     root / ("direct" if direct else "copied"),
                     direct=direct)
    print("PASS SIGKILL at durable 64-unlink recovery boundary + restart")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
