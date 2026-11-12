#!/usr/bin/env python3
"""Kill a real NVIDIA NGX PBO writer mid-PPM; verify safe restart cleanup."""
from __future__ import annotations

import fcntl
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time

from dlss_pen_primitive_smoke import make_case


def environment(prefix: Path, *, direct: bool,
                recover: bool, stage_pause_ms: int = 0,
                lock_series: bool = False) -> dict[str, str]:
    env = os.environ.copy()
    for key in (
        "DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER", "EGL_PLATFORM",
        "XYO_SHOT_ASYNC_RECOVER_STALE", "XYO_SHOT_ASYNC_TEST_STAGE_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_FAIL_AFTER", "XYO_SHOT_ASYNC_TEST_FAIL_STAGE",
        "XYO_SHOT_ASYNC_TEST_FAIL_WRITE_ROW",
        "XYO_SHOT_ASYNC_TEST_FAIL_SYNC",
        "XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS",
        "XYO_SHOT_ASYNC_TEST_ENQUEUE_DELAY_MS",
        "XYO_SHOT_ASYNC_TEST_ABORT_BEFORE_ENQUEUE",
        "XYO_SHOT_ASYNC_TEST_UNBIND_CONTEXT",
        "XYO_SHOT_ASYNC_TEST_FOREIGN_CONTEXT",
        "XYO_SHOT_ASYNC_TEST_SKIP_RESTORE",
        "XYO_SHOT_ASYNC_BUFFER_POOL",
        "XYO_SHOT_ASYNC_ATOMIC",
        "XYO_SHOT_ASYNC_LOCK_SERIES",
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
        "XYO_SHOT_ASYNC_RECOVER_STALE": "1" if recover else "0",
        "XYO_SHOT_ASYNC_LOCK_SERIES": "1" if lock_series else "0",
        "XYO_SHOT_ASYNC_STATS": "1",
        "XYO_EXIT_FRAMES": "8",
    })
    if stage_pause_ms:
        env["XYO_SHOT_ASYNC_TEST_STAGE_PAUSE_MS"] = str(stage_pause_ms)
    return env


def command(binary: Path, project: Path) -> list[str]:
    return [str(binary), "play", str(project),
            "--scale", "2", "--fps", "1000"]


def recovery_stats(result: subprocess.CompletedProcess[str]) -> tuple[int, ...]:
    match = re.search(
        r"\[async PPM\] recovery checked=(\d+) removed=(\d+) "
        r"recent=(\d+) locked=(\d+) unsafe=(\d+) errors=(\d+) "
        r"dir_entries=(\d+) scan_ms=([0-9.]+)", result.stderr,
    )
    if not match:
        raise RuntimeError(f"missing recovery stats:\n{result.stderr[-2500:]}")
    return tuple(int(x) for x in match.groups()[:7])


def assert_saved(prefix: Path, stderr: str) -> None:
    header = b"P6\n960 720\n255\n"
    for frame in range(1, 9):
        path = prefix.parent / f"{prefix.name}-{frame:06d}.ppm"
        with path.open("rb") as file:
            if file.read(len(header)) != header:
                raise RuntimeError(f"invalid PPM header: {path}")
        if path.stat().st_size != len(header) + 960*720*3:
            raise RuntimeError(f"incomplete final screenshot: {path}")
    if "atomic_files enabled=1 staged=8 published=8 discarded=0" not in stderr:
        raise RuntimeError("expected exactly 8 atomic PPM publications")
    if "durability enabled=1 file_fsyncs=8 dir_fsyncs=8 sync_failures=0" not in stderr:
        raise RuntimeError("expected exactly 8 durable fsync pairs")


def test_case(binary: Path, project: Path, root: Path, *, direct: bool) -> None:
    root.mkdir(parents=True)
    prefix = root / "frame"
    sentinel = root / "frame-000099.ppm"
    sentinel.write_bytes(b"previous-complete-PPM")
    args = command(binary, project)

    proc = subprocess.Popen(
        args, env=environment(prefix, direct=direct, recover=False,
                              stage_pause_ms=8000),
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    orphan: Path | None = None
    try:
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            candidates = list(root.glob("frame-000001.ppm.part.*"))
            if candidates:
                path = candidates[0]
                if path.stat().st_size >= len(b"P6\n960 720\n255\n") + 4*960*3:
                    orphan = path
                    break
            if proc.poll() is not None:
                break
            time.sleep(0.025)
        if orphan is None:
            proc.kill()
            _, stderr = proc.communicate(timeout=5)
            raise RuntimeError(
                "did not observe a flushed partial PPM from live process: "
                + stderr.decode(errors="replace")[-2000:]
            )
        if proc.poll() is not None:
            raise RuntimeError("writer exited before SIGKILL")
        # The writer must own its cooperative lock while the tempfile
        # contains only the first four rows.
        with orphan.open("rb") as file:
            try:
                fcntl.flock(file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                pass
            else:
                fcntl.flock(file.fileno(), fcntl.LOCK_UN)
                raise RuntimeError("active writer did not lock partial PPM")
        incomplete_bytes = orphan.stat().st_size
        full_bytes = len(b"P6\n960 720\n255\n") + 960*720*3
        if not 0 < incomplete_bytes < full_bytes:
            raise RuntimeError(f"not a partial file: size={incomplete_bytes}")
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.communicate(timeout=6)
    if proc.returncode != -signal.SIGKILL:
        raise RuntimeError(f"test process not SIGKILLed: {proc.returncode}")

    assert orphan is not None
    if not orphan.exists() or sentinel.read_bytes() != b"previous-complete-PPM":
        raise RuntimeError("SIGKILL did not preserve orphan and existing image")
    if (root / "frame-000001.ppm").exists():
        raise RuntimeError("incomplete first frame was prematurely published")
    with orphan.open("rb") as partial:
        if partial.read(len(b"P6\n960 720\n255\n")) != b"P6\n960 720\n255\n":
            raise RuntimeError("orphan lacks valid PPM header")

    # The SIGKILL released the writer's lock, but a new run must still
    # refuse to remove this RECENT file (fixed 24-hour minimum age).
    with orphan.open("rb") as file:
        fcntl.flock(file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(file.fileno(), fcntl.LOCK_UN)
    recent = subprocess.run(
        args, env=environment(prefix, direct=direct, recover=True),
        capture_output=True, text=True, timeout=45,
    )
    if recent.returncode:
        raise RuntimeError(f"restarted capture failed:\n{recent.stderr[-2000:]}")
    recent_stats = recovery_stats(recent)
    if recent_stats[:6] != (1, 0, 1, 0, 0, 0) or not orphan.exists():
        raise RuntimeError(f"newly orphaned file incorrectly removed: {recent_stats}")
    assert_saved(prefix, recent.stderr)

    # Age simulation instead of an actual 24h sleep: emulate an orphan
    # left behind from yesterday, then start a fresh real NGX process.
    old_time = time.time() - 2*24*60*60
    os.utime(orphan, (old_time, old_time))
    aged = subprocess.run(
        args, env=environment(prefix, direct=direct, recover=True),
        capture_output=True, text=True, timeout=45,
    )
    if aged.returncode:
        raise RuntimeError(f"aged-orphan recovery failed:\n{aged.stderr[-2000:]}")
    aged_stats = recovery_stats(aged)
    if aged_stats[:6] != (1, 1, 0, 0, 0, 0) or orphan.exists():
        raise RuntimeError(f"aged SIGKILL orphan not recovered: {aged_stats}")
    assert_saved(prefix, aged.stderr)
    if sentinel.read_bytes() != b"previous-complete-PPM":
        raise RuntimeError("recovery damaged unrelated completed image")
    if list(root.glob("frame-*.ppm.part.*")):
        raise RuntimeError("unexpected staging files remained after cleanup")
    print(
        f"{'direct' if direct else 'copied'}: actual SIGKILL -9, "
        f"flushed partial={incomplete_bytes} bytes, live flock released; "
        "recent orphan kept, aged orphan removed=1, 8/8 durable PPM",
        flush=True,
    )


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_offscreen_pbo_sigkill_recovery_smoke.py <xyo>")
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xyo-dlss-real-sigkill-") as td:
        root = Path(td)
        scene = root / "project.sb3"
        make_case(scene, "replay")
        for direct in (False, True):
            test_case(binary, scene,
                      root / ("direct" if direct else "copied"),
                      direct=direct)
    print("PASS: real SIGKILL left partial PPM; restarted writer recovered after age threshold")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
