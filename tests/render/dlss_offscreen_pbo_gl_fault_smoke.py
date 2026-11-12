#!/usr/bin/env python3
"""Test GL readback/fence/mapping failures and SDL context rebinding."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from dlss_pen_primitive_smoke import make_case


def run(binary: str, scene: Path, root: Path, name: str, *,
        rgb: bool, failure: str = "", unbind: bool = False,
        foreign: bool = False, skip_restore: bool = False
        ) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER",
                "EGL_PLATFORM", "XYO_SHOT_ASYNC_TEST_FAIL_STAGE",
                "XYO_SHOT_ASYNC_TEST_UNBIND_CONTEXT",
                "XYO_SHOT_ASYNC_TEST_FOREIGN_CONTEXT",
                "XYO_SHOT_ASYNC_TEST_SKIP_RESTORE",
                "XYO_SHOT_ASYNC_TEST_FAIL_AFTER",
                "XYO_SHOT_ASYNC_TEST_ENQUEUE_DELAY_MS",
                "XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS"):
        env.pop(key, None)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_EXIT_FRAMES": "8",
        "XYO_SHOT_SERIES": str(root / name),
        "XYO_SHOT_SERIES_ASYNC": "1",
        "XYO_SHOT_ASYNC_DIRECT_MAP": "1",
        "XYO_SHOT_ASYNC_RGB": "1" if rgb else "0",
        "XYO_SHOT_ASYNC_STATS": "1",
        "XYO_SHOT_ASYNC_STAGE_PROFILE": "1",
    })
    if failure:
        env["XYO_SHOT_ASYNC_TEST_FAIL_STAGE"] = failure
    if unbind:
        env["XYO_SHOT_ASYNC_TEST_UNBIND_CONTEXT"] = "1"
    if foreign:
        env["XYO_SHOT_ASYNC_TEST_FOREIGN_CONTEXT"] = "1"
    if skip_restore:
        env["XYO_SHOT_ASYNC_TEST_SKIP_RESTORE"] = "1"
    return subprocess.run(
        [binary, "play", str(scene), "--scale", "2", "--fps", "1000"],
        capture_output=True, text=True, check=False, env=env, timeout=40,
    )


def check_lifecycle(result: subprocess.CompletedProcess[str]) -> None:
    match = re.search(
        r"\[async PPM\] lifecycle created=(\d+) deleted=(\d+) "
        r"borrowed_remaining=(\d+) completed_remaining=(\d+) "
        r"aborted_before_enqueue=(\d+)",
        result.stderr,
    )
    if not match:
        raise RuntimeError(f"missing GL lifecycle log:\n{result.stderr[-2200:]}")
    created, deleted, borrowed, completed, _ = map(int, match.groups())
    if created < 1 or created != deleted or borrowed != 0 or completed != 0:
        raise RuntimeError(f"GL PBO leak: {match.group()}")
    if "GL buffer lifecycle mismatch" in result.stderr:
        raise RuntimeError("GL lifecycle mismatch at shutdown")


def image_hashes(root: Path, prefix: str) -> list[str]:
    paths = sorted(root.glob(prefix + "-??????.ppm"))
    expected = [root / f"{prefix}-{i:06d}.ppm" for i in range(1, 9)]
    if paths != expected:
        raise RuntimeError(f"missing output: {prefix}, {len(paths)} of 8")
    output = []
    for path in paths:
        value = path.read_bytes()
        if not value.startswith(b"P6\n960 720\n255\n"):
            raise RuntimeError(f"PPM header malformed: {path}")
        if len(value) != 960*720*3 + len(b"P6\n960 720\n255\n"):
            raise RuntimeError(f"PPM truncated: {path}")
        output.append(hashlib.sha256(value).hexdigest())
    return output


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_offscreen_pbo_gl_fault_smoke.py <xyo>")
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="xyo-ngxsdk-gl-fault-") as temp:
        root = Path(temp)
        scene = root / "project.sb3"
        make_case(scene, "replay")
        for rgb in (False, True):
            label = "rgb" if rgb else "rgba"
            normal = run(binary, scene, root, f"{label}-normal", rgb=rgb)
            if normal.returncode or "NVIDIA NGX initialized" not in normal.stderr:
                raise RuntimeError(f"{label} baseline failed:\n{normal.stderr[-2200:]}")
            check_lifecycle(normal)
            expected = image_hashes(root, f"{label}-normal")

            rebound = run(binary, scene, root, f"{label}-rebound",
                          rgb=rgb, unbind=True)
            if rebound.returncode != 0:
                raise RuntimeError(f"{label} GL rebind failed:\n{rebound.stderr[-2200:]}")
            check_lifecycle(rebound)
            if image_hashes(root, f"{label}-rebound") != expected:
                raise RuntimeError(f"{label}: context-rebound images differ")
            print(f"{label}: GL context unbind/restore 8/8 byte-exact", flush=True)

            switched = run(binary, scene, root, f"{label}-switched",
                           rgb=rgb, foreign=True)
            if (switched.returncode or
                    "test foreign GL context made current" not in switched.stderr):
                raise RuntimeError(
                    f"{label}: foreign GL context restore failed:\n"
                    f"{switched.stderr[-2200:]}"
                )
            check_lifecycle(switched)
            if image_hashes(root, f"{label}-switched") != expected:
                raise RuntimeError(f"{label}: foreign-context image mismatch")
            print(f"{label}: foreign GL context restore 8/8 byte-exact",
                  flush=True)

            # Without the owning context, GL calls must NOT run against the
            # unrelated GL Pen context. Failure is reported, never hidden.
            refused = run(binary, scene, root, f"{label}-refused",
                          rgb=rgb, foreign=True, skip_restore=True)
            if (refused.returncode != 2 or
                    "test foreign GL context made current" not in refused.stderr or
                    "owning GL context unavailable at final flush"
                    not in refused.stderr or
                    "asynchronous GPU readback or write failed"
                    not in refused.stderr):
                raise RuntimeError(
                    f"{label}: foreign context not rejected:\n"
                    f"{refused.stderr[-2200:]}"
                )
            print(f"{label}: foreign GL context cleanup safely rejected",
                  flush=True)

            no_context = run(binary, scene, root, f"{label}-contextless",
                             rgb=rgb, unbind=True, skip_restore=True)
            if (no_context.returncode != 2 or
                    "owning GL context unavailable at final flush"
                    not in no_context.stderr):
                raise RuntimeError(
                    f"{label}: no-context cleanup not rejected:\n"
                    f"{no_context.stderr[-2200:]}"
                )
            print(f"{label}: contextless GL cleanup safely rejected",
                  flush=True)

            for stage, diagnostic in (
                ("read", "GL readback failed"),
                ("fence", "GL fence wait failed"),
                ("map", "GL PBO mapping failed"),
            ):
                result = run(binary, scene, root, f"{label}-{stage}",
                             rgb=rgb, failure=stage)
                if (result.returncode != 2 or
                        diagnostic not in result.stderr or
                        "asynchronous GPU readback or write failed" not in result.stderr):
                    raise RuntimeError(
                        f"{label}/{stage}: failure not propagated "
                        f"(exit {result.returncode}):\n{result.stderr[-2200:]}"
                    )
                check_lifecycle(result)
                print(
                    f"{label}/{stage}: exit=2, GL buffers created=deleted, "
                    "no borrowed/mapped PBOs",
                    flush=True,
                )
        print("PASS RGB and RGBA GL failures, context rebind, foreign-context "
              "recovery and fail-closed rejection")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
