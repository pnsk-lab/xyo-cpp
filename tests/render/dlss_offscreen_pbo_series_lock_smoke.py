#!/usr/bin/env python3
"""Per-prefix cross-process lock rejects competing writers and survives SIGKILL."""
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


def invoke(binary: Path, scene: Path, prefix: Path, *, direct: bool,
           recover: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command(binary, scene),
        env=environment(prefix, direct=direct, recover=recover,
                        lock_series=True),
        capture_output=True, text=True, timeout=45,
    )


def check(binary: Path, scene: Path, root: Path, *, direct: bool) -> None:
    root.mkdir()
    prefix = root / "frame"
    args = command(binary, scene)
    holder = subprocess.Popen(
        args, env=environment(prefix, direct=direct, recover=False,
                              stage_pause_ms=9000, lock_series=True),
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    orphan: Path | None = None
    try:
        deadline = time.monotonic() + 20
        expected_min = len(b"P6\n960 720\n255\n") + 4*960*3
        while time.monotonic() < deadline:
            candidates = list(root.glob("frame-000001.ppm.part.*"))
            if candidates and candidates[0].stat().st_size >= expected_min:
                orphan = candidates[0]
                break
            if holder.poll() is not None:
                break
            time.sleep(0.025)
        if orphan is None:
            raise RuntimeError("lock owner did not reach partial-file checkpoint")
        if not (root / ".frame.xyo-capture.lock").is_file():
            raise RuntimeError("capture-series lock file not present")

        second = invoke(binary, scene, prefix, direct=direct, recover=True)
        if (second.returncode != 2 or
                "[async PPM] capture series already active" not in second.stderr or
                (root / "frame-000001.ppm").exists()):
            raise RuntimeError(
                f"concurrent same-series writer not rejected:\n{second.stderr[-2400:]}"
            )
        if not orphan.exists():
            raise RuntimeError("contender deleted active writer's tempfile")

        # Lock scope is precisely the series prefix, not the directory.
        other_prefix = root / "independent"
        other = invoke(binary, scene, other_prefix, direct=direct)
        if other.returncode:
            raise RuntimeError(
                f"unrelated capture series unexpectedly blocked:\n{other.stderr[-2200:]}"
            )
        assert_saved(other_prefix, other.stderr)
        if "series_lock enabled=1 acquired=1" not in other.stderr:
            raise RuntimeError("second independent capture did not get a series lock")
        if not (root / ".independent.xyo-capture.lock").exists():
            raise RuntimeError("independent capture lock missing")

        # SIGKILL must release flock, without removing/replacing the lock
        # inode (which could allow a competing writer to bypass it).
        holder.kill()
        holder.communicate(timeout=6)
        if holder.returncode != -signal.SIGKILL:
            raise RuntimeError(f"SIGKILL exit was {holder.returncode}")
        if not (root / ".frame.xyo-capture.lock").is_file():
            raise RuntimeError("SIGKILL removed the lockfile")

        # Verify lock truly became available, and stale recovery can now
        # proceed in a new owner after a simulated two-day age increase.
        assert orphan is not None
        os.utime(orphan, (time.time() - 2*86400,)*2)
        restarted = invoke(binary, scene, prefix, direct=direct, recover=True)
        if restarted.returncode:
            raise RuntimeError(
                f"post-SIGKILL owner cannot acquire lock:\n{restarted.stderr[-2300:]}"
            )
        if "series_lock enabled=1 acquired=1" not in restarted.stderr:
            raise RuntimeError("restarted writer did not own capture-series lock")
        checked, removed, recent, locked, unsafe, errors, _ = recovery_stats(restarted)
        if (checked, removed, recent, locked, unsafe, errors) != (1, 1, 0, 0, 0, 0):
            raise RuntimeError(
                "post-SIGKILL stale recovery failed: "
                f"{(checked, removed, recent, locked, unsafe, errors)}"
            )
        assert_saved(prefix, restarted.stderr)
        if orphan.exists():
            raise RuntimeError("old orphan remains after lock release/restart")
        if not (root / ".frame.xyo-capture.lock").is_file():
            raise RuntimeError("lockfile should remain in place after owner closes")

        # Prevent a symlink from being used as a fake lock target.
        sentinel = root / "sentinel.txt"
        sentinel.write_bytes(b"safe-content")
        (root / ".unsafe.xyo-capture.lock").symlink_to(sentinel)
        unsafe = invoke(binary, scene, root / "unsafe", direct=direct)
        if (unsafe.returncode != 2 or
                "cannot open capture-series lock" not in unsafe.stderr or
                sentinel.read_bytes() != b"safe-content"):
            raise RuntimeError(
                f"symlink lockfile was not rejected safely:\n{unsafe.stderr[-2200:]}"
            )

        print(
            f"{'direct' if direct else 'copied'}: active prefix rejected; "
            "different prefix allowed; SIGKILL released persistent lock; "
            "stale file recovered; symlink lock rejected",
            flush=True,
        )
    finally:
        if holder.poll() is None:
            holder.kill()
        holder.communicate(timeout=6)


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_offscreen_pbo_series_lock_smoke.py <xyo>")
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xyo-ngxsdk-series-lock-") as temp:
        root = Path(temp)
        scene = root / "scene.sb3"
        make_case(scene, "replay")
        for direct in (False, True):
            check(binary, scene, root / ("direct" if direct else "copied"),
                  direct=direct)
    print("PASS: cooperative per-prefix capture lock with actual concurrent processes")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
