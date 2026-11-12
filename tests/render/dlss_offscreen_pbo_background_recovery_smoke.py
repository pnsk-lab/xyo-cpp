#!/usr/bin/env python3
"""Background PPM orphan recovery: no renderer-thread I/O, safe join, old-file policy."""
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
from dlss_offscreen_pbo_stale_recovery_smoke import make_fixture


def run(
    binary: Path, scene: Path, prefix: Path, *,
    direct: bool, background: bool, frames: int,
    pause_ms: int = 0, series_lock: bool = False,
    fail_stage: str = "",
    entry_delay_ms: int = 0,
    write_delay_ms: int = 0,
    pre_unlink_pause_ms: int = 0,
    post_unlink_pause_ms: int = 0,
    fail_recovery_sync: bool = False,
    durable: bool = True,
) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in (
        "DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER", "EGL_PLATFORM",
        "XYO_SHOT_ASYNC_RECOVERY_THREAD",
        "XYO_SHOT_ASYNC_TEST_RECOVERY_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_RECOVERY_ENTRY_DELAY_MS",
        "XYO_SHOT_ASYNC_TEST_PRE_UNLINK_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_POST_UNLINK_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_FAIL_RECOVERY_SYNC",
        "XYO_SHOT_ASYNC_TEST_STAGE_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_FAIL_AFTER",
        "XYO_SHOT_ASYNC_TEST_FAIL_SYNC",
        "XYO_SHOT_ASYNC_TEST_FAIL_STAGE",
        "XYO_SHOT_ASYNC_TEST_FAIL_WRITE_ROW",
        "XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS",
        "XYO_SHOT_ASYNC_ATOMIC", "XYO_SHOT_ASYNC_BUFFER_POOL",
        "XYO_SHOT_ASYNC_DURABLE", "XYO_SHOT_ASYNC_LOCK_SERIES",
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
        "XYO_SHOT_SERIES": str(prefix),
        "XYO_SHOT_SERIES_ASYNC": "1",
        "XYO_SHOT_ASYNC_RGB": "1",
        "XYO_SHOT_ASYNC_DIRECT_MAP": "1" if direct else "0",
        "XYO_SHOT_ASYNC_DURABLE": "1" if durable else "0",
        "XYO_SHOT_ASYNC_ATOMIC": "1",
        "XYO_SHOT_ASYNC_RECOVER_STALE": "1",
        "XYO_SHOT_ASYNC_RECOVERY_THREAD": "1" if background else "0",
        "XYO_SHOT_ASYNC_LOCK_SERIES": "1" if series_lock else "0",
        "XYO_SHOT_ASYNC_STATS": "1",
    })
    if pause_ms:
        env["XYO_SHOT_ASYNC_TEST_RECOVERY_PAUSE_MS"] = str(pause_ms)
    if fail_stage:
        env["XYO_SHOT_ASYNC_TEST_FAIL_STAGE"] = fail_stage
    if entry_delay_ms:
        env["XYO_SHOT_ASYNC_TEST_RECOVERY_ENTRY_DELAY_MS"] = str(entry_delay_ms)
    if write_delay_ms:
        env["XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS"] = str(write_delay_ms)
    if pre_unlink_pause_ms:
        env["XYO_SHOT_ASYNC_TEST_PRE_UNLINK_PAUSE_MS"] = str(pre_unlink_pause_ms)
    if post_unlink_pause_ms:
        env["XYO_SHOT_ASYNC_TEST_POST_UNLINK_PAUSE_MS"] = str(post_unlink_pause_ms)
    if fail_recovery_sync:
        env["XYO_SHOT_ASYNC_TEST_FAIL_RECOVERY_SYNC"] = "1"
    return subprocess.run(
        # Isolate SDL/NGX/PBO tests from concurrent JIT implementation
        # changes; the runtime's HotSpot teardown currently crashes in
        # JitEngine::~JitEngine(), unrelated to PPM capture.
        [str(binary), "play", str(scene), "--scale", "2",
         "--fps", "1000", "--jit", "off"],
        capture_output=True, text=True, env=env, check=False, timeout=70,
    )


def statistics(result: subprocess.CompletedProcess[str]) -> dict[str, int | float]:
    if result.returncode != 0 or "NVIDIA NGX initialized" not in result.stderr:
        raise RuntimeError(
            f"NGX capture failed (exit {result.returncode}):\n"
            f"{result.stderr[-3200:]}"
        )
    recovery = re.search(
        r"\[async PPM\] recovery checked=(\d+) removed=(\d+) "
        r"recent=(\d+) locked=(\d+) unsafe=(\d+) errors=(\d+) "
        r"dir_entries=(\d+) scan_ms=([0-9.]+)", result.stderr,
    )
    thread = re.search(
        r"\[async PPM\] recovery_thread enabled=(\d+) started=(\d+) "
        r"joined=(\d+) foreground_ms=([0-9.]+)", result.stderr,
    )
    progress = re.search(
        r"\[async PPM\] recovery_progress complete=(\d+) steps=(\d+) "
        r"max_step_entries=(\d+) max_step_ms=([0-9.]+) pending=(\d+)",
        result.stderr,
    )
    if not recovery or not thread or not progress:
        raise RuntimeError(f"missing recovery diagnostics:\n{result.stderr[-3000:]}")

    fields = dict(zip(
        ("checked", "removed", "recent", "locked", "unsafe", "errors",
         "entries", "scan_ms", "thread_enabled", "thread_started",
         "thread_joined", "foreground_ms", "complete", "steps",
         "max_entries", "max_step_ms", "pending"),
        (*[int(x) for x in recovery.groups()[:7]],
         float(recovery.group(8)),
         *[int(x) for x in thread.groups()[:3]],
         float(thread.group(4)),
         *[int(x) for x in progress.groups()[:3]],
         float(progress.group(4)),
         int(progress.group(5))),
    ))
    if fields["errors"] or fields["max_entries"] > 128:
        raise RuntimeError(f"recovery invariants invalid: {fields}")
    return fields


def ensure_frames(prefix: Path, frames: int, stderr: str) -> None:
    header = b"P6\n960 720\n255\n"
    for i in (1, frames):
        path = prefix.parent / f"{prefix.name}-{i:06}.ppm"
        with path.open("rb") as image:
            if image.read(len(header)) != header:
                raise RuntimeError(f"invalid PPM header: {path}")
        if path.stat().st_size != len(header) + 960*720*3:
            raise RuntimeError(f"PPM missing image bytes: {path}")
    if (f"atomic_files enabled=1 staged={frames} "
            f"published={frames} discarded=0") not in stderr:
        raise RuntimeError(f"atomic publication count differs:\n{stderr[-1700:]}")
    if (f"durability enabled=1 file_fsyncs={frames} "
            f"dir_fsyncs={frames} sync_failures=0") not in stderr:
        raise RuntimeError("durable write counters differ")


def test_complete(binary: Path, scene: Path, root: Path, direct: bool) -> None:
    root.mkdir(parents=True)
    prefix, files, lock_holder = make_fixture(root)
    try:
        for i in range(3200):
            (root / f"unrelated-{i:05}.txt").write_bytes(b"x")
        result = run(
            binary, scene, prefix, direct=direct, background=True,
            frames=48, series_lock=True,
        )
        m = statistics(result)
        if (
            (m["thread_enabled"], m["thread_started"], m["thread_joined"]) !=
            (1, 1, 1)
            or not m["complete"] or m["pending"] or
            m["entries"] < 3200 or
            (m["checked"], m["removed"], m["recent"],
             m["locked"], m["unsafe"]) != (7, 2, 1, 1, 3)
        ):
            raise RuntimeError(f"background recovery incomplete/unsafe: {m}")
        if "series_lock enabled=1 acquired=1" not in result.stderr:
            raise RuntimeError("series lock was not retained while recovering")
        if "recovery_durability enabled=1 dir_fsyncs=1 synced_removals=2 sync_failures=0" not in result.stderr:
            raise RuntimeError("recovered deletions not synced once at scan completion")
        for name in ("old_a", "old_b"):
            if files[name].exists():
                raise RuntimeError(f"stale file was not removed: {name}")
        for name in ("recent", "locked", "other_prefix", "invalid_token",
                     "old_final", "symlink_target", "symlink",
                     "hardlink", "hardlink_target", "directory"):
            if not files[name].exists() and not files[name].is_symlink():
                raise RuntimeError(f"protected file was removed: {name}")
        ensure_frames(prefix, 48, result.stderr)
        print(
            f"{'direct' if direct else 'copied'} background: entries={m['entries']} "
            f"steps={m['steps']} complete=1 removed=2 locked=1 "
            f"max_step_entries={m['max_entries']} "
            f"renderer_recovery_ms={m['foreground_ms']:.3f}",
            flush=True,
        )
    finally:
        lock_holder.close()


def test_nonblocking(
    binary: Path, scene: Path, root: Path, direct: bool,
) -> None:
    root.mkdir(parents=True)
    for i in range(400):
        (root / f"other-{i:05}.txt").write_bytes(b"x")
    front = run(
        binary, scene, root / "front", direct=direct,
        background=False, frames=1, pause_ms=250,
    )
    back = run(
        binary, scene, root / "back", direct=direct,
        background=True, frames=1, pause_ms=250,
    )
    foreground = statistics(front)
    asynchronous = statistics(back)
    if (
        foreground["thread_started"] or foreground["thread_joined"]
        or (asynchronous["thread_enabled"], asynchronous["thread_started"],
            asynchronous["thread_joined"]) != (1, 1, 1)
    ):
        raise RuntimeError(f"recovery worker lifetime mismatch: {asynchronous}")
    if foreground["foreground_ms"] < 200:
        raise RuntimeError(f"foreground I/O delay injection failed: {foreground}")
    if (asynchronous["foreground_ms"] > 100 or
            asynchronous["foreground_ms"] * 2 >= foreground["foreground_ms"]):
        raise RuntimeError(
            "background recovery blocked the render thread: "
            f"front={foreground}, background={asynchronous}"
        )
    if asynchronous["complete"]:
        raise RuntimeError("short capture unexpectedly finished the slow scan")
    ensure_frames(root / "front", 1, front.stderr)
    ensure_frames(root / "back", 1, back.stderr)
    print(
        f"{'direct' if direct else 'copied'} slow-storage injection: "
        f"render_thread_recovery front={foreground['foreground_ms']:.1f} ms, "
        f"background={asynchronous['foreground_ms']:.3f} ms, "
        "background worker joined=1, incomplete scan safely stopped",
        flush=True,
    )

    # A GPU readback failure while the scanner might still be in its
    # test-only slow directory operation must also join safely.
    failed = run(
        binary, scene, root / "failed", direct=direct,
        background=True, frames=1, pause_ms=250, fail_stage="read",
    )
    if (
        failed.returncode != 2
        or "[async PPM] GL readback failed" not in failed.stderr
        or "recovery_thread enabled=1 started=1 joined=1"
        not in failed.stderr
        or "GL buffer lifecycle mismatch" in failed.stderr
    ):
        raise RuntimeError(
            f"failed GL readback did not stop scanner safely: "
            f"exit={failed.returncode}\n{failed.stderr[-2600:]}"
        )


def test_cancel_batch(
    binary: Path, scene: Path, root: Path, direct: bool,
) -> None:
    root.mkdir(parents=True)
    # Every unrelated filename costs 20 ms in this test-only scenario.
    # Without an in-batch cancellation check, an already-started scan
    # continues at least 16 iterations (~320 ms) after the stop request,
    # because the soft deadline is checked only once per 16 entries.
    for index in range(500):
        (root / f"other-{index:05}.txt").write_bytes(b"x")
    result = run(
        binary, scene, root / "slow-entries",
        direct=direct, background=True, frames=8,
        entry_delay_ms=20,
    )
    parsed = statistics(result)
    shutdown = re.search(
        r"\[async PPM\] recovery_shutdown joined_ms=([0-9.]+) "
        r"cancel_observed=(\d+)", result.stderr,
    )
    if not shutdown:
        raise RuntimeError(
            f"missing recovery shutdown telemetry:\n{result.stderr[-2200:]}"
        )
    joined_ms = float(shutdown.group(1))
    cancelled = int(shutdown.group(2))
    if (
        (parsed["thread_enabled"], parsed["thread_started"],
         parsed["thread_joined"]) != (1, 1, 1)
        or parsed["complete"] or not parsed["pending"]
        # Stop may arrive while the worker is waiting on the first
        # simulated I/O call, correctly aborting with zero readdir entries.
        or parsed["steps"] < 1 or parsed["scan_ms"] < 15
        or not cancelled or joined_ms >= 150
    ):
        raise RuntimeError(
            f"slow-IO cancellation did not stop within batch: "
            f"join={joined_ms:.2f} ms cancel={cancelled} stats={parsed}"
        )
    ensure_frames(root / "slow-entries", 8, result.stderr)
    print(
        f"{'direct' if direct else 'copied'} cancellation: "
        f"entries={parsed['entries']} join={joined_ms:.2f}ms "
        "cancel_observed=1; 8/8 durable PPM",
        flush=True,
    )


def test_cancel_before_writer_drain(
    binary: Path, scene: Path, root: Path, direct: bool,
) -> None:
    root.mkdir(parents=True)
    for i in range(1000):
        (root / f"other-{i:05}.txt").write_bytes(b"x")

    # The disk writer stays busy with delayed, durable output. The scanner
    # itself is deliberately slowed to keep it active at final flush.
    # The shutdown request should be sent while some PPM frames are still
    # pending, not only after the disk writer has completed every image.
    result = run(
        binary, scene, root / "frame",
        direct=direct, background=True, frames=16,
        entry_delay_ms=5, write_delay_ms=80,
    )
    m = statistics(result)
    shutdown = re.search(
        r"\[async PPM\] recovery_shutdown joined_ms=([0-9.]+) "
        r"cancel_observed=(\d+) pending_images_at_stop=(\d+)",
        result.stderr,
    )
    if not shutdown:
        raise RuntimeError(
            f"missing early stop diagnostics:\n{result.stderr[-2400:]}"
        )
    join_ms, cancelled, pending = (
        float(shutdown.group(1)), int(shutdown.group(2)),
        int(shutdown.group(3)),
    )
    if (
        (m["thread_enabled"], m["thread_started"], m["thread_joined"]) !=
        (1, 1, 1)
        or m["complete"] or not m["pending"] or not cancelled
        or pending <= 0 or pending > 16 or join_ms > 250
    ):
        raise RuntimeError(
            f"early scanner cancellation failed: pending={pending} "
            f"join={join_ms:.3f} cancel={cancelled} stats={m}"
        )
    ensure_frames(root / "frame", 16, result.stderr)
    print(
        f"{'direct' if direct else 'copied'} early stop: "
        f"{pending}/16 PPM frames not yet saved when scanner stop requested; "
        f"joined={join_ms:.2f}ms, cancel=1; 16/16 durable PPM",
        flush=True,
    )


def test_cancel_before_unlink(
    binary: Path, scene: Path, root: Path, direct: bool,
) -> None:
    root.mkdir(parents=True)
    orphan = root / "frame-000099.ppm.part.Abc123"
    orphan.write_bytes(b"old incomplete image should remain")
    age = time.time() - 2 * 86400
    os.utime(orphan, (age, age))
    failed_to_delete = run(
        binary, scene, root / "frame", direct=direct,
        background=True, frames=2, pre_unlink_pause_ms=2500,
    )
    first = statistics(failed_to_delete)
    m = re.search(
        r"\[async PPM\] recovery_shutdown joined_ms=([0-9.]+) "
        r"cancel_observed=(\d+) pending_images_at_stop=(\d+) "
        r"preunlink_skipped=(\d+)",
        failed_to_delete.stderr,
    )
    if not m or int(m.group(2)) != 1 or int(m.group(4)) < 1:
        raise RuntimeError(
            f"worker didn't stop at pre-unlink boundary: "
            f"{failed_to_delete.stderr[-2500:]}"
        )
    if first["removed"] != 0 or not orphan.exists() or first["complete"]:
        raise RuntimeError(
            f"cancelled worker still unlinked staged file: {first}"
        )
    ensure_frames(root / "frame", 2, failed_to_delete.stderr)
    # The old candidate was intentionally kept when cancellation arrived;
    # a new process with recovery enabled must still be able to delete it.
    restart = run(
        binary, scene, root / "frame", direct=direct,
        background=True, frames=12,
    )
    second = statistics(restart)
    if (second["removed"] != 1 or orphan.exists() or
            not second["complete"]):
        raise RuntimeError(f"orphan was not recovered on restart: {second}")
    ensure_frames(root / "frame", 12, restart.stderr)
    print(
        f"{'direct' if direct else 'copied'} pre-unlink race: "
        f"cancel=1 skipped={m.group(4)} old_file_retained=1 "
        "restart_removed=1; 2/2 + 12/12 durable frames", flush=True,
    )


def test_recovery_sync_on_partial_stop(
    binary: Path, scene: Path, root: Path, direct: bool,
) -> None:
    root.mkdir(parents=True)
    orphan = root / "frame-000099.ppm.part.Abc123"
    orphan.write_bytes(b"abandoned-PPM")
    stamp = time.time() - 2 * 86400
    os.utime(orphan, (stamp, stamp))
    for index in range(200):
        (root / f"unrelated-{index:05}.txt").write_bytes(b"x")

    cancelled = run(
        binary, scene, root / "frame",
        direct=direct, background=True, frames=2,
        post_unlink_pause_ms=2500,
    )
    metrics = statistics(cancelled)
    expected = (
        "recovery_durability enabled=1 dir_fsyncs=1 "
        "synced_removals=1 sync_failures=0"
    )
    if (metrics["removed"] != 1 or not metrics["pending"] or
            not metrics["thread_joined"] or orphan.exists() or
            expected not in cancelled.stderr):
        raise RuntimeError(
            f"cancellation did not sync completed orphan unlink: "
            f"{metrics}\n{cancelled.stderr[-2300:]}"
        )
    ensure_frames(root / "frame", 2, cancelled.stderr)

    # A recovered deletion is already visible before a directory fsync.
    # If the fsync fails, retain the complete output frames, report an
    # explicit recovery error and DO NOT count removals as durable.
    bad = root / "failed"
    bad.mkdir()
    stale = bad / "frame-000099.ppm.part.Xyz123"
    stale.write_bytes(b"old-staging")
    os.utime(stale, (stamp, stamp))
    failed = run(
        binary, scene, bad / "frame", direct=direct,
        background=True, frames=16, fail_recovery_sync=True,
    )
    if failed.returncode or "NVIDIA NGX initialized" not in failed.stderr:
        raise RuntimeError(f"fault injection stopped valid capture:\n{failed.stderr[-2600:]}")
    if (stale.exists() or
            "recovery_durability enabled=1 dir_fsyncs=0 "
            "synced_removals=0 sync_failures=1" not in failed.stderr or
            "recovered PPM unlink directory fsync failed (injected)" not in failed.stderr):
        raise RuntimeError(
            "directory fsync failure not reported as non-durable deletion: "
            f"{failed.stderr[-2600:]}"
        )
    recovery = re.search(
        r"\[async PPM\] recovery checked=(\d+) removed=(\d+) "
        r"recent=(\d+) locked=(\d+) unsafe=(\d+) errors=(\d+)",
        failed.stderr,
    )
    if not recovery or int(recovery.group(2)) != 1 or int(recovery.group(6)) != 1:
        raise RuntimeError(f"recovery sync failure not tracked: {recovery}")
    ensure_frames(bad / "frame", 16, failed.stderr)
    print(
        f"{'direct' if direct else 'copied'} durable removal: "
        "partial scan cancellation fsynced 1 unlink; "
        "injected directory-sync error reported without data loss",
        flush=True,
    )


def test_batched_recovery_sync(
    binary: Path, scene: Path, root: Path, direct: bool,
) -> None:
    root.mkdir(parents=True)
    prefix = root / "frame"
    stamp = time.time() - 2 * 86400
    for idx in range(130):
        path = root / f"frame-{idx + 1000:06d}.ppm.part.{idx:06d}"
        path.write_bytes(b"stale PPM")
        os.utime(path, (stamp, stamp))
    result = run(
        binary, scene, prefix, direct=direct,
        background=True, frames=48, series_lock=True,
    )
    m = statistics(result)
    match = re.search(
        r"\[async PPM\] recovery_durability enabled=1 "
        r"dir_fsyncs=(\d+) synced_removals=(\d+) "
        r"sync_failures=(\d+) sync_batch=(\d+) max_unsynced=(\d+)",
        result.stderr,
    )
    if (
        not match or tuple(map(int, match.groups())) != (3, 130, 0, 64, 64)
        or not m["complete"] or m["removed"] != 130
        or m["thread_joined"] != 1
    ):
        raise RuntimeError(
            f"130 recovered unlinks did not sync in 64+64+2 batches: "
            f"stats={m} durable={match and match.group(0)}"
        )
    if list(root.glob("frame-*.ppm.part.*")):
        raise RuntimeError("old partfiles remain after complete batched scan")
    ensure_frames(prefix, 48, result.stderr)
    print(
        f"{'direct' if direct else 'copied'} sync batching: "
        "130 removals, fsync 64+64+2=3, max unsynced=64, "
        "48/48 PPM durable",
        flush=True,
    )

    # The same recovery in non-durable mode must not sync old deletions.
    no_sync = root / "non-durable"
    no_sync.mkdir()
    path = no_sync / "frame-000099.ppm.part.Xy1234"
    path.write_bytes(b"stale PPM")
    os.utime(path, (stamp, stamp))
    plain = run(
        binary, scene, no_sync / "frame", direct=direct,
        background=True, frames=16, durable=False,
    )
    if plain.returncode or path.exists():
        raise RuntimeError(f"non-durable cleanup failed:\n{plain.stderr[-2000:]}")
    if ("recovery_durability enabled=0 dir_fsyncs=0 "
            "synced_removals=0 sync_failures=0" not in plain.stderr):
        raise RuntimeError(
            f"non-durable mode unexpectedly synced stale removals: "
            f"{plain.stderr[-2200:]}"
        )
    if "atomic_files enabled=1 staged=16 published=16 discarded=0" not in plain.stderr:
        raise RuntimeError("non-durable PPM atomic publishing regression")

    # A directory-sync error in the *first* 64-removal batch is sticky.
    # It must not generate another filesystem sync/error for the second
    # batch or the partial final batch, and must not break image output.
    bad = root / "sync-error"
    bad.mkdir()
    for idx in range(130):
        orphan = bad / f"frame-{idx + 1000:06d}.ppm.part.{idx:06d}"
        orphan.write_bytes(b"old")
        os.utime(orphan, (stamp, stamp))
    failed = run(
        binary, scene, bad / "frame", direct=direct,
        background=True, frames=32, fail_recovery_sync=True,
    )
    if failed.returncode:
        raise RuntimeError(f"recovery fsync injection prevented capture: {failed.stderr[-2000:]}")
    if ("recovery_durability enabled=1 dir_fsyncs=0 "
            "synced_removals=0 sync_failures=1 sync_batch=64 max_unsynced=130"
            not in failed.stderr):
        raise RuntimeError(
            f"failed first sync was retried or marked durable:\n"
            f"{failed.stderr[-2200:]}"
        )
    if "recovery checked=130 removed=130 recent=0 locked=0 unsafe=0 errors=1" not in failed.stderr:
        raise RuntimeError("failed directory sync not counted once")
    if list(bad.glob("frame-*.ppm.part.*")):
        raise RuntimeError("failed recovery sync left a staging file")
    ensure_frames(bad / "frame", 32, failed.stderr)


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_offscreen_pbo_background_recovery_smoke.py <xyo>")
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xyo-ngxsdk-background-PPM-") as tmp:
        root = Path(tmp)
        scene = root / "scene.sb3"
        make_case(scene, "replay")
        for direct in (False, True):
            target = root / ("direct" if direct else "copied")
            test_complete(binary, scene, target / "complete", direct)
            test_nonblocking(binary, scene, target / "slow", direct)
            test_cancel_batch(binary, scene, target / "cancel", direct)
            test_cancel_before_writer_drain(binary, scene, target / "early", direct)
            test_cancel_before_unlink(binary, scene, target / "unlink", direct)
            test_recovery_sync_on_partial_stop(binary, scene, target / "sync", direct)
            test_batched_recovery_sync(binary, scene, target / "batch", direct)
    print("PASS background recovery, safe files and locks, nonblocking render submit")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
