#!/usr/bin/env python3
"""Recover only stale orphaned PBO .part files for the active capture prefix."""
from __future__ import annotations

import fcntl
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

from dlss_pen_primitive_smoke import make_case


def make_fixture(root: Path) -> tuple[Path, dict[str, Path], object]:
    prefix = root / "frame"
    stamp = time.time() - 2 * 86400

    def old(name: str) -> Path:
        path = root / name
        path.write_bytes(b"stale-incomplete-PPM")
        os.utime(path, (stamp, stamp))
        return path

    files = {
        "old_a": old("frame-000099.ppm.part.Abc123"),
        "old_b": old("frame-000010.ppm.part.aB1Cd3"),
        "recent": root / "frame-000011.ppm.part.Zz0123",
        "locked": old("frame-000012.ppm.part.LocK12"),
        "other_prefix": old("foreign-000099.ppm.part.abc456"),
        "invalid_token": old("frame-000099.ppm.part.$$Z999"),
        "old_final": root / "frame-000099.ppm",
        "symlink_target": old("target-data.txt"),
        "symlink": root / "frame-000013.ppm.part.Aa1234",
        "hardlink": root / "frame-000014.ppm.part.AB12Cd",
        "hardlink_target": old("hardlink-source.txt"),
        "directory": root / "frame-000015.ppm.part.DiR123",
    }
    files["recent"].write_bytes(b"actively-recent-data")
    files["old_final"].write_bytes(b"existing-final-should-remain")
    files["symlink"].symlink_to(files["symlink_target"])
    os.link(files["hardlink_target"], files["hardlink"])
    files["directory"].mkdir()
    os.utime(files["directory"], (stamp, stamp))
    # This is the same advisory lock used by the C++ atomic writer.
    holder = files["locked"].open("rb")
    fcntl.flock(holder.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    return prefix, files, holder


def run(binary: Path, project: Path, prefix: Path, *, recover: bool,
        direct: bool) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in (
        "DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER", "EGL_PLATFORM",
        "XYO_SHOT_ASYNC_RECOVER_STALE", "XYO_SHOT_ASYNC_TEST_FAIL_AFTER",
        "XYO_SHOT_ASYNC_TEST_FAIL_STAGE", "XYO_SHOT_ASYNC_TEST_FAIL_SYNC",
        "XYO_SHOT_ASYNC_TEST_FAIL_WRITE_ROW",
    ):
        env.pop(key, None)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_SHOT_SERIES": str(prefix),
        "XYO_SHOT_SERIES_ASYNC": "1",
        "XYO_SHOT_ASYNC_RGB": "1",
        "XYO_SHOT_ASYNC_DIRECT_MAP": "1" if direct else "0",
        "XYO_SHOT_ASYNC_DURABLE": "1",
        "XYO_SHOT_ASYNC_STATS": "1",
        "XYO_SHOT_ASYNC_RECOVER_STALE": "1" if recover else "0",
        "XYO_EXIT_FRAMES": "8",
    })
    return subprocess.run(
        [str(binary), "play", str(project), "--scale", "2", "--fps", "1000"],
        env=env, capture_output=True, text=True, check=False, timeout=45,
    )


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_offscreen_pbo_stale_recovery_smoke.py <xyo>")
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xyo-dlss-stale-recovery-") as td:
        root = Path(td)
        project = root / "scene.sb3"
        make_case(project, "replay")
        for direct in (False, True):
            sub = root / ("direct" if direct else "copied")
            sub.mkdir()
            prefix, files, lock_holder = make_fixture(sub)
            try:
                disabled = run(binary, project, prefix, recover=False,
                               direct=direct)
                if disabled.returncode:
                    raise RuntimeError(
                        f"{direct=}: baseline NGX failed:\n{disabled.stderr[-2000:]}"
                    )
                if not files["old_a"].exists() or not files["old_b"].exists():
                    raise RuntimeError("disabled recovery unexpectedly removed stale data")
                if "[async PPM] recovery" in disabled.stderr:
                    raise RuntimeError("recovery ran when disabled")

                enabled = run(binary, project, prefix, recover=True,
                              direct=direct)
                if enabled.returncode or "NVIDIA NGX initialized" not in enabled.stderr:
                    raise RuntimeError(
                        f"{direct=}: enabled recovery failed:\n{enabled.stderr[-2000:]}"
                    )
                recovery = re.search(
                    r"\[async PPM\] recovery checked=(\d+) removed=(\d+) "
                    r"recent=(\d+) locked=(\d+) unsafe=(\d+) errors=(\d+) "
                    r"dir_entries=(\d+) scan_ms=([0-9.]+)",
                    enabled.stderr,
                )
                if not recovery:
                    raise RuntimeError(f"missing recovery stats:\n{enabled.stderr[-2000:]}")
                checked, removed, recent, locked, unsafe, errors, entries = map(
                    int, recovery.groups()[:7]
                )
                scan_ms = float(recovery.group(8))
                if (checked != 7 or removed != 2 or recent != 1 or
                        locked != 1 or unsafe != 3 or errors != 0 or
                        entries < checked or scan_ms < 0):
                    raise RuntimeError(f"incorrect recovery counters: {recovery.group()}")
                for key in ("old_a", "old_b"):
                    if files[key].exists():
                        raise RuntimeError(f"stale orphan not deleted: {key}")
                for key in ("recent", "locked", "other_prefix", "invalid_token",
                            "old_final", "symlink_target", "symlink",
                            "hardlink", "hardlink_target", "directory"):
                    if not files[key].exists() and not files[key].is_symlink():
                        raise RuntimeError(f"protected file removed: {key}")
                if files["old_final"].read_bytes() != b"existing-final-should-remain":
                    raise RuntimeError("existing completed PPM modified")
                for i in range(1, 9):
                    image = sub / f"frame-{i:06d}.ppm"
                    header = b"P6\n960 720\n255\n"
                    if not image.exists() or image.stat().st_size != len(header) + 960*720*3:
                        raise RuntimeError(f"missing or incomplete frame: {image}")
                if "atomic_files enabled=1 staged=8 published=8 discarded=0" not in enabled.stderr:
                    raise RuntimeError("atomic image publishing failed after recovery")
                if "durability enabled=1 file_fsyncs=8 dir_fsyncs=8 sync_failures=0" not in enabled.stderr:
                    raise RuntimeError("durable image publishing failed after recovery")
                if ("recovery_durability enabled=1 dir_fsyncs=1 "
                        "synced_removals=2 sync_failures=0") not in enabled.stderr:
                    raise RuntimeError("foreground deletion fsync missing or not batched")

                # The same output directory has no eligible stale files
                # after cleanup. A fresh foreground scan must not fsync it
                # again merely for observing recent/locked candidates.
                empty = run(binary, project, prefix, recover=True,
                            direct=direct)
                if empty.returncode or (
                        "recovery_durability enabled=1 dir_fsyncs=0 "
                        "synced_removals=0 sync_failures=0"
                        not in empty.stderr):
                    raise RuntimeError(
                        f"unneeded deletion fsync on empty scan:\n"
                        f"{empty.stderr[-2000:]}"
                    )
                print(
                    f"{'direct' if direct else 'copied'}: removed=2, "
                    "recent=1, active_locked=1, unsafe=3, errors=0; "
                    f"entries={entries} scan_ms={scan_ms:.3f}; "
                    "8/8 durable RGB frames intact",
                    flush=True,
                )
            finally:
                lock_holder.close()
    print("PASS: prefix-scoped stale recovery, safe exclusions and live file locks")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
