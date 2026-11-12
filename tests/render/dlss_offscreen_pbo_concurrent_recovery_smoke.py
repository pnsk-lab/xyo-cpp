#!/usr/bin/env python3
"""A real concurrent NGX writer must not have its old active .part file recovered."""
from __future__ import annotations

import fcntl
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

from dlss_pen_primitive_smoke import make_case
from dlss_offscreen_pbo_sigkill_recovery_smoke import (
    assert_saved, command, environment, recovery_stats,
)


def verify(binary: Path, scene: Path, root: Path, *, direct: bool) -> None:
    root.mkdir()
    prefix = root / "frame"
    args = command(binary, scene)
    holder = subprocess.Popen(
        args,
        env=environment(prefix, direct=direct, recover=False,
                        stage_pause_ms=9000),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    orphan: Path | None = None
    try:
        deadline = time.monotonic() + 20
        expected_min = len(b"P6\n960 720\n255\n") + 4*960*3
        while time.monotonic() < deadline:
            files = list(root.glob("frame-000001.ppm.part.*"))
            if files and files[0].stat().st_size >= expected_min:
                orphan = files[0]
                break
            if holder.poll() is not None:
                break
            time.sleep(0.025)
        if orphan is None:
            if holder.poll() is None:
                holder.kill()
            _, err = holder.communicate(timeout=6)
            raise RuntimeError(
                "live writer did not reach partial-file checkpoint: "
                f"returncode={holder.returncode} "
                f"stderr={err.decode(errors='replace')[-2300:]} "
                f"files={list(root.iterdir())}"
            )
        if holder.poll() is not None:
            raise RuntimeError(f"writer exited before concurrency test: {holder.returncode}")

        # Artificially age an *active* file while another process retains
        # its flock. The age predicate alone must not be enough to unlink.
        old_stamp = time.time() - 2*24*3600
        os.utime(orphan, (old_stamp, old_stamp))
        with orphan.open("rb") as handle:
            try:
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                pass
            else:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
                raise RuntimeError("writer did not retain its exclusive lock")

        second = subprocess.run(
            args,
            env=environment(prefix, direct=direct, recover=True),
            capture_output=True, text=True, timeout=45,
        )
        if second.returncode:
            raise RuntimeError(f"concurrent capture failed:\n{second.stderr[-2500:]}")
        checked, removed, recent, locked, unsafe, errors, _ = recovery_stats(second)
        if (checked, removed, recent, locked, unsafe, errors) != (1, 0, 0, 1, 0, 0):
            raise RuntimeError(
                "concurrent recovery did not protect locked old writer: "
                f"{(checked, removed, recent, locked, unsafe, errors)}"
            )
        if not orphan.exists():
            raise RuntimeError("concurrent recovery deleted active .part file")
        assert_saved(prefix, second.stderr)

        holder.kill()
        holder.communicate(timeout=6)
        if holder.returncode != -signal.SIGKILL:
            raise RuntimeError(f"expected actual SIGKILL, got {holder.returncode}")

        aged = subprocess.run(
            args,
            env=environment(prefix, direct=direct, recover=True),
            capture_output=True, text=True, timeout=45,
        )
        if aged.returncode:
            raise RuntimeError(f"post-kill capture failed:\n{aged.stderr[-2500:]}")
        checked, removed, recent, locked, unsafe, errors, _ = recovery_stats(aged)
        if (checked, removed, recent, locked, unsafe, errors) != (1, 1, 0, 0, 0, 0):
            raise RuntimeError(
                "released lock was not recoverable: "
                f"{(checked, removed, recent, locked, unsafe, errors)}"
            )
        if orphan.exists():
            raise RuntimeError("post-SIGKILL orphan was not cleaned up")
        assert_saved(prefix, aged.stderr)
        print(
            f"{'direct' if direct else 'copied'}: active aged file locked=1, "
            "removed=0 while live; SIGKILL releases lock, removed=1 "
            "on next capture; 8/8 durable PPM each run",
            flush=True,
        )
    finally:
        if holder.poll() is None:
            holder.kill()
        holder.communicate(timeout=6)


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_offscreen_pbo_concurrent_recovery_smoke.py <xyo>")
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xyo-ngxsdk-concurrent-recovery-") as temp:
        root = Path(temp)
        scene = root / "scene.sb3"
        make_case(scene, "replay")
        for direct in (False, True):
            verify(binary, scene, root / ("direct" if direct else "copied"),
                   direct=direct)
    print("PASS: active old PBO stage file protected from other process, recovered after SIGKILL")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
