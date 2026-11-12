#!/usr/bin/env python3
"""Native NVIDIA NGX: durable atomic PPM fsync and rename failure semantics."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from dlss_pen_primitive_smoke import make_case


def run(binary: Path, scene: Path, prefix: Path, *, rgb: bool,
        direct: bool, durable: bool, sync_failure: str = ""
        ) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in (
        "DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER", "EGL_PLATFORM",
        "XYO_SHOT_ASYNC_ATOMIC", "XYO_SHOT_ASYNC_DURABLE",
        "XYO_SHOT_ASYNC_TEST_FAIL_SYNC", "XYO_SHOT_ASYNC_TEST_FAIL_WRITE_ROW",
        "XYO_SHOT_ASYNC_TEST_FAIL_AFTER", "XYO_SHOT_ASYNC_TEST_FAIL_STAGE",
        "XYO_SHOT_ASYNC_TEST_ABORT_BEFORE_ENQUEUE",
    ):
        env.pop(key, None)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_EXIT_FRAMES": "8",
        "XYO_SHOT_SERIES": str(prefix),
        "XYO_SHOT_SERIES_ASYNC": "1",
        "XYO_SHOT_ASYNC_RGB": "1" if rgb else "0",
        "XYO_SHOT_ASYNC_DIRECT_MAP": "1" if direct else "0",
        "XYO_SHOT_ASYNC_DURABLE": "1" if durable else "0",
        "XYO_SHOT_ASYNC_STATS": "1",
    })
    if sync_failure:
        env["XYO_SHOT_ASYNC_TEST_FAIL_SYNC"] = sync_failure
    return subprocess.run(
        [str(binary), "play", str(scene), "--scale", "2", "--fps", "1000"],
        check=False, capture_output=True, text=True, env=env, timeout=60,
    )


def stats(result: subprocess.CompletedProcess[str]) -> dict[str, int]:
    atomic = re.search(
        r"\[async PPM\] atomic_files enabled=(\d+) staged=(\d+) "
        r"published=(\d+) discarded=(\d+)", result.stderr,
    )
    durable = re.search(
        r"\[async PPM\] durability enabled=(\d+) file_fsyncs=(\d+) "
        r"dir_fsyncs=(\d+) sync_failures=(\d+)", result.stderr,
    )
    lifetime = re.search(
        r"\[async PPM\] lifecycle created=(\d+) deleted=(\d+) "
        r"borrowed_remaining=(\d+) completed_remaining=(\d+)",
        result.stderr,
    )
    if not atomic or not durable or not lifetime:
        raise RuntimeError(f"missing counters:\n{result.stderr[-2400:]}")
    a = list(map(int, atomic.groups()))
    d = list(map(int, durable.groups()))
    created, deleted, borrowed, completed = map(int, lifetime.groups())
    if created < 1 or created != deleted or borrowed or completed:
        raise RuntimeError(f"GL object not reclaimed: {lifetime.group()}")
    return dict(zip(("atomic", "staged", "published", "discarded",
                     "durable", "file_fsyncs", "dir_fsyncs",
                     "sync_failures"), a + d))


def ppm_hash(path: Path) -> str:
    data = path.read_bytes()
    header = b"P6\n960 720\n255\n"
    if not data.startswith(header) or len(data) != len(header) + 960*720*3:
        raise RuntimeError(f"corrupted PPM: {path}")
    return hashlib.sha256(data).hexdigest()


def no_partials(prefix: Path) -> None:
    parts = sorted(prefix.parent.glob(prefix.name + "-*.ppm.part.*"))
    if parts:
        raise RuntimeError(f"orphaned temp files: {parts}")


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_offscreen_pbo_durable_smoke.py <xyo>")
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xyo-dlss-durable-") as td:
        root = Path(td)
        scene = root / "project.sb3"
        make_case(scene, "replay")
        for direct in (False, True):
            for rgb in (False, True):
                label = f"{'direct' if direct else 'copy'}-{'rgb' if rgb else 'rgba'}"
                plain_prefix = root / f"{label}-plain"
                plain = run(binary, scene, plain_prefix, rgb=rgb,
                            direct=direct, durable=False)
                if plain.returncode or "NVIDIA NGX initialized" not in plain.stderr:
                    raise RuntimeError(f"{label} plain failed:\n{plain.stderr[-2400:]}")
                expected = [ppm_hash(root / f"{label}-plain-{i:06}.ppm")
                            for i in range(1, 9)]

                durable_prefix = root / f"{label}-durable"
                success = run(binary, scene, durable_prefix, rgb=rgb,
                              direct=direct, durable=True)
                if success.returncode:
                    raise RuntimeError(f"{label} durable failed:\n{success.stderr[-2400:]}")
                values = stats(success)
                if (values["atomic"] != 1 or values["durable"] != 1 or
                        values["staged"] != 8 or values["published"] != 8 or
                        values["discarded"] or values["file_fsyncs"] != 8 or
                        values["dir_fsyncs"] != 8 or values["sync_failures"]):
                    raise RuntimeError(f"{label} unexpected sync stats: {values}")
                if [ppm_hash(root / f"{label}-durable-{i:06}.ppm")
                        for i in range(1, 9)] != expected:
                    raise RuntimeError(f"{label} durable screenshot hashes differ")
                no_partials(durable_prefix)

                # Before rename: failure must leave an existing destination
                # unchanged and clean up all newly written .part files.
                file_prefix = root / f"{label}-filefail"
                original = root / f"{label}-filefail-000001.ppm"
                original.write_bytes(b"older-valid-image")
                file_fail = run(binary, scene, file_prefix, rgb=rgb,
                                direct=direct, durable=True,
                                sync_failure="file")
                if (file_fail.returncode != 2 or
                        "file fsync failed before rename" not in file_fail.stderr):
                    raise RuntimeError(f"{label} file sync error not propagated:\n{file_fail.stderr[-2300:]}")
                values = stats(file_fail)
                if (values["file_fsyncs"] or values["published"] or
                        values["sync_failures"] < 1 or values["discarded"] < 1 or
                        original.read_bytes() != b"older-valid-image"):
                    raise RuntimeError(f"{label} file sync failure semantics: {values}")
                no_partials(file_prefix)

                # After rename: directory fsync failure must be reported,
                # but the complete new PPM can ALREADY be visible. Never
                # falsely claim the old destination is preserved here.
                dir_prefix = root / f"{label}-dirfail"
                directory_fail = run(binary, scene, dir_prefix, rgb=rgb,
                                     direct=direct, durable=True,
                                     sync_failure="dir")
                if (directory_fail.returncode != 2 or
                        "directory fsync failed AFTER rename"
                        not in directory_fail.stderr):
                    raise RuntimeError(f"{label} dir sync error not propagated:\n{directory_fail.stderr[-2300:]}")
                values = stats(directory_fail)
                if (values["file_fsyncs"] < 1 or
                        values["published"] < 1 or
                        values["dir_fsyncs"] != 0 or
                        values["sync_failures"] < 1):
                    raise RuntimeError(f"{label} dir sync failure semantics: {values}")
                files = list(root.glob(dir_prefix.name + "-??????.ppm"))
                if not files or any(ppm_hash(p) not in expected for p in files):
                    raise RuntimeError(f"{label} dir fsync failure published partial PPM")
                no_partials(dir_prefix)
                print(f"{label}: 8/8 durable byte-exact; file fsync failure "
                      "preserves final; directory fsync failure reports "
                      "already-published complete image", flush=True)
    print("PASS: durable PBO RGBA/RGB and copied/direct, failure semantics and GL cleanup")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
