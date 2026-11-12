#!/usr/bin/env python3
"""Atomic PPM publication: no partial final files after mid-image I/O failure."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from dlss_pen_primitive_smoke import make_case


def run(
    binary: str, scene: Path, prefix: Path, *,
    rgb: bool, direct: bool, atomic: bool, fail_row: int = 0
) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in (
        "DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER", "EGL_PLATFORM",
        "XYO_SHOT_ASYNC_ATOMIC", "XYO_SHOT_ASYNC_TEST_FAIL_WRITE_ROW",
        "XYO_SHOT_ASYNC_TEST_FAIL_AFTER",
        "XYO_SHOT_ASYNC_TEST_FAIL_STAGE",
        "XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS",
        "XYO_SHOT_ASYNC_TEST_ABORT_BEFORE_ENQUEUE",
    ):
        env.pop(key, None)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_EXIT_FRAMES": "8",
        "XYO_SHOT_SERIES": str(prefix),
        "XYO_SHOT_SERIES_ASYNC": "1",
        "XYO_SHOT_ASYNC_RGB": "1" if rgb else "0",
        "XYO_SHOT_ASYNC_DIRECT_MAP": "1" if direct else "0",
        "XYO_SHOT_ASYNC_ATOMIC": "1" if atomic else "0",
        "XYO_SHOT_ASYNC_STATS": "1",
    })
    if fail_row:
        env["XYO_SHOT_ASYNC_TEST_FAIL_WRITE_ROW"] = str(fail_row)
    return subprocess.run(
        [binary, "play", str(scene), "--scale", "2", "--fps", "1000"],
        capture_output=True, text=True, check=False, env=env, timeout=40,
    )


def check_no_parts(prefix: Path) -> None:
    parts = sorted(prefix.parent.glob(prefix.name + "-*.ppm.part.*"))
    if parts:
        raise RuntimeError(f"orphaned partial files: {parts}")


def check_stats(proc: subprocess.CompletedProcess[str],
                *, staged: int, published: int, discarded: int) -> None:
    match = re.search(
        r"\[async PPM\] atomic_files enabled=(\d+) staged=(\d+) "
        r"published=(\d+) discarded=(\d+)", proc.stderr
    )
    if not match:
        raise RuntimeError(f"missing atomic counters:\n{proc.stderr[-2300:]}")
    actual = tuple(map(int, match.groups()))
    expected = (1, staged, published, discarded)
    if actual != expected:
        raise RuntimeError(f"atomic counters {actual}, expected {expected}")
    lifecycle = re.search(
        r"\[async PPM\] lifecycle created=(\d+) deleted=(\d+) "
        r"borrowed_remaining=(\d+) completed_remaining=(\d+) ",
        proc.stderr
    )
    if not lifecycle:
        raise RuntimeError("missing GL PBO lifecycle log")
    created, deleted, borrowed, completed = map(int, lifecycle.groups())
    if created == 0 or created != deleted or borrowed or completed:
        raise RuntimeError(f"GL PBO leak: {lifecycle.group()}")


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_offscreen_pbo_atomic_smoke.py <xyo>")
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="xyo-dlss-atomic-") as tmp:
        root = Path(tmp)
        scene = root / "project.sb3"
        make_case(scene, "replay")
        for direct in (False, True):
            for rgb in (False, True):
                label = f"{'direct' if direct else 'copied'}-{'rgb' if rgb else 'rgba'}"
                prefix = root / label
                baseline = run(
                    binary, scene, prefix, rgb=rgb, direct=direct, atomic=False
                )
                if baseline.returncode != 0:
                    raise RuntimeError(
                        f"{label}: baseline failed:\n{baseline.stderr[-2500:]}"
                    )
                images = [root / f"{label}-{i:06d}.ppm" for i in range(1, 9)]
                hashes = []
                for path in images:
                    data = path.read_bytes()
                    if len(data) != len(b"P6\n960 720\n255\n") + 960*720*3:
                        raise RuntimeError(f"invalid baseline PPM: {path}")
                    hashes.append(hashlib.sha256(data).hexdigest())

                # The atomic writer should safely replace existing images
                # using a sibling tempfile, never changing the pixel content.
                atomic = run(
                    binary, scene, prefix, rgb=rgb, direct=direct, atomic=True
                )
                if atomic.returncode:
                    raise RuntimeError(
                        f"{label}: atomic capture failed:\n{atomic.stderr[-2500:]}"
                    )
                for path, expected in zip(images, hashes):
                    if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                        raise RuntimeError(f"{label}: atomic image differs: {path}")
                check_stats(atomic, staged=8, published=8, discarded=0)
                check_no_parts(prefix)

                # A pre-existing valid frame must survive a partial write.
                # The other destination frames must never be published.
                sentinel = images[0]
                original_bytes = b"previous-complete-image-placeholder\n"
                sentinel.write_bytes(original_bytes)
                for path in images[1:]:
                    path.unlink()
                failed = run(
                    binary, scene, prefix, rgb=rgb, direct=direct,
                    atomic=True, fail_row=2
                )
                if (failed.returncode != 2 or
                        "injected write failure at row 2" not in failed.stderr or
                        "[async PPM] cannot save" not in failed.stderr):
                    raise RuntimeError(
                        f"{label}: write error not reported:\n{failed.stderr[-2500:]}"
                    )
                if sentinel.read_bytes() != original_bytes:
                    raise RuntimeError(f"{label}: prior complete image overwritten")
                if any(path.exists() for path in images[1:]):
                    raise RuntimeError(f"{label}: incomplete final PPM published")
                check_no_parts(prefix)
                # One or more queued frames may be completed before error
                # becomes visible; every attempted partial write must be
                # discarded, never published.
                counter = re.search(
                    r"\[async PPM\] atomic_files enabled=1 staged=(\d+) "
                    r"published=(\d+) discarded=(\d+)", failed.stderr
                )
                if not counter or int(counter.group(2)) or not int(counter.group(3)):
                    raise RuntimeError(
                        f"{label}: partial write was not discarded:\n"
                        f"{failed.stderr[-2500:]}"
                    )
                check_stats(
                    failed, staged=int(counter.group(1)),
                    published=0, discarded=int(counter.group(3))
                )

                # A successful temporary write can still fail when the final
                # destination is a directory. The complete temporary PPM
                # must be unlinked and this must still return status 2.
                rename_prefix = root / f"{label}-rename-error"
                directory_destination = root / f"{label}-rename-error-000001.ppm"
                directory_destination.mkdir()
                rename_failed = run(
                    binary, scene, rename_prefix,
                    rgb=rgb, direct=direct, atomic=True
                )
                if (rename_failed.returncode != 2 or
                        "[async PPM] cannot save" not in rename_failed.stderr):
                    raise RuntimeError(
                        f"{label}: failed rename not propagated:\n"
                        f"{rename_failed.stderr[-2500:]}"
                    )
                if not directory_destination.is_dir():
                    raise RuntimeError(f"{label}: destination directory damaged")
                check_no_parts(rename_prefix)
                rename_stats = re.search(
                    r"\[async PPM\] atomic_files enabled=1 staged=(\d+) "
                    r"published=(\d+) discarded=(\d+)",
                    rename_failed.stderr
                )
                if not rename_stats or int(rename_stats.group(3)) < 1:
                    raise RuntimeError(
                        f"{label}: failed rename did not discard tempfile"
                    )
                print(f"{label}: 8/8 atomic PPM byte-exact; interrupted "
                      "and failed-rename files discarded; existing image "
                      "preserved", flush=True)

        print("PASS: four PBO modes, all-or-nothing PPM publication, "
              "fault cleanup, and GL lifecycle")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(1)
