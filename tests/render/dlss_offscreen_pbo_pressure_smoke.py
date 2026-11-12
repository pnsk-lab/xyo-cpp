#!/usr/bin/env python3
"""Native DLSS PBO queue-pressure and final-drain regression (no display server)."""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from dlss_pen_primitive_smoke import make_case


def execute(binary: Path, project: Path, root: Path, name: str,
            frames: int, scale: int, *, asynchronous: bool,
            every: int = 1, writer_delay_ms: int = 0,
            rgb_readback: bool = False, cpu_pool: bool = False,
            direct_map: bool = False, prefix_override: Path | None = None,
            expect_failure: bool = False
            ) -> tuple[subprocess.CompletedProcess[str], list[Path]]:
    prefix = prefix_override if prefix_override is not None else root / name
    env = os.environ.copy()
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER",
                "EGL_PLATFORM", "XYO_SHOT", "XYO_SHOT_SERIES_ASYNC",
                "XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS", "XYO_SHOT_ASYNC_RGB",
                "XYO_SHOT_ASYNC_BUFFER_POOL",
                "XYO_SHOT_ASYNC_DIRECT_MAP", "XYO_SHOT_ASYNC_STAGE_PROFILE"):
        env.pop(key, None)
    if writer_delay_ms:
        env["XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS"] = str(writer_delay_ms)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_EXIT_FRAMES": str(frames),
        "XYO_SHOT_SERIES": str(prefix),
        "XYO_SHOT_EVERY": str(every),
        "XYO_SHOT_SERIES_ASYNC": "1" if asynchronous else "0",
        "XYO_SHOT_ASYNC_STATS": "1",
        "XYO_SHOT_ASYNC_RGB": "1" if rgb_readback else "0",
        "XYO_SHOT_ASYNC_BUFFER_POOL": "1" if cpu_pool else "0",
        "XYO_SHOT_ASYNC_DIRECT_MAP": "1" if direct_map else "0",
        "XYO_SHOT_ASYNC_STAGE_PROFILE": "1",
    })
    result = subprocess.run(
        [str(binary), "play", str(project), "--scale", str(scale),
         "--fps", "1000"],
        capture_output=True, text=True, check=False, env=env, timeout=150,
    )
    if ((not expect_failure and result.returncode) or
            "NVIDIA NGX initialized" not in result.stderr):
        raise RuntimeError(f"{name}: NGX failure:\n{result.stderr[-3000:]}")
    if expect_failure:
        return result, []
    if "SDL video driver=offscreen" not in result.stderr:
        raise RuntimeError(f"{name}: missing SDL offscreen")
    paths = sorted(root.glob(name + "-??????.ppm"))
    expected = [root / f"{name}-{i:06d}.ppm"
                for i in range(every, frames + 1, every)]
    if paths != expected:
        raise RuntimeError(
            f"{name}: missing/duplicate frames: {len(paths)} vs {len(expected)}"
        )
    return result, paths


def sha256_ppm(path: Path, scale: int) -> str:
    w, h = 480 * scale, 360 * scale
    header = f"P6\n{w} {h}\n255\n".encode()
    if path.stat().st_size != len(header) + 3 * w * h:
        raise RuntimeError(f"truncated or oversized image: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as file:
        if file.read(len(header)) != header:
            raise RuntimeError(f"invalid image header: {path}")
        digest.update(header)
        while data := file.read(1 << 20):
            digest.update(data)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--frames", type=int, default=96)
    parser.add_argument("--scale", type=int, default=4)
    parser.add_argument("--every", type=int, default=16)
    parser.add_argument("--rgb", action="store_true",
                        help="test GL_RGB 3-byte PBO readback")
    parser.add_argument("--pool", action="store_true",
                        help="test recycling the CPU mapped image buffers")
    parser.add_argument("--direct-map", action="store_true",
                        help="write directly from PBO mapping without CPU memcpy")
    args = parser.parse_args()
    if args.pool and args.direct_map:
        parser.error("--direct-map and --pool cannot be combined")
    if (args.frames < 2 or args.scale < 1 or args.scale > 4 or
            args.every < 1 or args.frames % args.every):
        parser.error("frames >= 2, scale in [1,4], frames divisible by every")

    with tempfile.TemporaryDirectory(prefix="xyo-ngxsdk-pbo-pressure-") as t:
        root = Path(t)
        project = root / "pen.sb3"
        make_case(project, "replay")
        pbo_result, pbo_files = execute(
            args.binary.resolve(), project, root, "pbo",
            args.frames, args.scale, asynchronous=True,
            rgb_readback=args.rgb, cpu_pool=args.pool,
            direct_map=args.direct_map,
        )
        match = re.search(
            r"\[async PPM\] stats submitted=(\d+) saved=(\d+) "
            r"max_pbos=(\d+) max_cpu_queue=(\d+) "
            r"gpu_waits=(\d+) gpu_wait_ms=([0-9.]+) "
            r"queue_waits=(\d+) queue_wait_ms=([0-9.]+) error=(\d+)",
            pbo_result.stderr,
        )
        if not match:
            raise RuntimeError("missing PBO queue telemetry")
        expected_bytes = (480 * args.scale) * (360 * args.scale) * (
            3 if args.rgb else 4
        ) * args.frames
        format_label = "RGB" if args.rgb else "RGBA"
        transfer_marker = (
            f"[async PPM] transfer format={format_label} "
            f"bytes_per_pixel={3 if args.rgb else 4} "
            f"submitted_bytes={expected_bytes}"
        )
        if transfer_marker not in pbo_result.stderr:
            raise RuntimeError("PBO transfer format/byte count incorrect")
        pool_match = re.search(
            r"\[async PPM\] cpu_buffers reuse=(\d+) allocations=(\d+) "
            r"reused=(\d+) peak_pool=(\d+)",
            pbo_result.stderr,
        )
        if not pool_match:
            raise RuntimeError("missing CPU buffer allocation telemetry")
        pool_enabled, allocations, reuses, max_pool = map(
            int, pool_match.groups()
        )
        if pool_enabled != int(args.pool) or not 0 <= max_pool <= 10:
            raise RuntimeError(f"CPU buffer pool mode/bounds incorrect: {pool_match.group()}")
        if args.pool:
            if not 0 < reuses < args.frames or not 0 < allocations < args.frames:
                raise RuntimeError(f"CPU buffer reuse not exercised: {pool_match.group()}")
        elif args.direct_map:
            if reuses or allocations or max_pool:
                raise RuntimeError(f"direct PBO unexpectedly allocated CPU image buffers: {pool_match.group()}")
        elif reuses or allocations != args.frames or max_pool:
            raise RuntimeError(f"disabled buffer pool changed baseline: {pool_match.group()}")
        direct_match = re.search(
            r"\[async PPM\] direct_map enabled=(\d+) mapped_bytes=(\d+) "
            r"max_borrowed_pbos=(\d+) pbo_worker_waits=(\d+) "
            r"pbo_worker_wait_ms=([0-9.]+)",
            pbo_result.stderr,
        )
        if not direct_match:
            raise RuntimeError("missing direct PBO telemetry")
        enabled, direct_bytes, max_borrowed, waits, wait_ms = direct_match.groups()
        if (int(enabled) != int(args.direct_map) or
                int(direct_bytes) != (expected_bytes if args.direct_map else 0) or
                not 0 <= int(max_borrowed) <= 4):
            raise RuntimeError(f"invalid direct-PBO telemetry: {direct_match.group()}")
        stage_match = re.search(
            r"\[async PPM\] stages capture_ms=([0-9.]+) "
            r"gl_read_call_ms=([0-9.]+) pbo_maps=(\d+) "
            r"pbo_map_ms=([0-9.]+) cpu_copy_ms=([0-9.]+) "
            r"cpu_copy_bytes=(\d+) writer_ms=([0-9.]+) writer_calls=(\d+)",
            pbo_result.stderr,
        )
        if (not stage_match or int(stage_match.group(3)) != args.frames
                or int(stage_match.group(8)) != args.frames
                or int(stage_match.group(6)) != (0 if args.direct_map else expected_bytes)):
            raise RuntimeError(f"PBO mapping/copy counters invalid: {stage_match.group() if stage_match else ''}")
        submitted, saved, peak_pbos, peak_queue, gpu_waits, gpu_ms, \
            queue_waits, queue_ms, error = match.groups()
        if (int(submitted) != args.frames or int(saved) != args.frames or
                not 1 <= int(peak_pbos) <= 4 or
                not 0 <= int(peak_queue) <= 8 or int(error) != 0):
            raise RuntimeError(f"PBO queue bounds/count violated: {match.group()}")

        sync_result, sync_files = execute(
            args.binary.resolve(), project, root, "sync",
            args.frames, args.scale, asynchronous=False, every=args.every,
        )
        for index, sync_path in enumerate(sync_files):
            frame_number = (index + 1) * args.every
            pbo_path = pbo_files[frame_number - 1]
            if sha256_ppm(sync_path, args.scale) != sha256_ppm(
                    pbo_path, args.scale):
                raise RuntimeError(
                    f"stressed PBO differs from sync at frame {frame_number}"
                )
        # One-frame exit must drain an unsignaled PBO and write its image.
        single_result, single_files = execute(
            args.binary.resolve(), project, root, "single",
            1, args.scale, asynchronous=True,
            rgb_readback=args.rgb, cpu_pool=args.pool,
            direct_map=args.direct_map,
        )
        if len(single_files) != 1:
            raise RuntimeError("single-frame PBO not drained")
        if sha256_ppm(single_files[0], args.scale) != sha256_ppm(
                pbo_files[0], args.scale):
            raise RuntimeError("single-frame final capture differs")

        # Deliberately slow only the disk writer, not the GPU.
        # The queue must stop accepting new images rather than dropping them
        # or growing beyond kMaxQueuedImages=8.
        pressure, pressure_files = execute(
            args.binary.resolve(), project, root, "backpressure",
            48, 2, asynchronous=True, writer_delay_ms=25,
            rgb_readback=args.rgb, cpu_pool=args.pool,
            direct_map=args.direct_map,
        )
        pressure_match = re.search(
            r"\[async PPM\] stats submitted=(\d+) saved=(\d+) "
            r"max_pbos=(\d+) max_cpu_queue=(\d+) "
            r"gpu_waits=(\d+) gpu_wait_ms=([0-9.]+) "
            r"queue_waits=(\d+) queue_wait_ms=([0-9.]+) error=(\d+)",
            pressure.stderr,
        )
        if not pressure_match:
            raise RuntimeError("missing backpressure telemetry")
        direct_pressure = re.search(
            r"\[async PPM\] direct_map enabled=(\d+) mapped_bytes=(\d+) "
            r"max_borrowed_pbos=(\d+) pbo_worker_waits=(\d+) "
            r"pbo_worker_wait_ms=([0-9.]+)",
            pressure.stderr,
        )
        if (len(pressure_files) != 48 or
                int(pressure_match.group(1)) != 48 or
                int(pressure_match.group(2)) != 48 or
                int(pressure_match.group(3)) > 4 or
                int(pressure_match.group(4)) > (4 if args.direct_map else 8) or
                int(pressure_match.group(9)) != 0 or
                (args.direct_map and (not direct_pressure or
                                      int(direct_pressure.group(4)) < 1)) or
                (not args.direct_map and (int(pressure_match.group(4)) != 8 or
                                         int(pressure_match.group(7)) < 1))):
            raise RuntimeError(
                f"bounded writer backpressure not verified: "
                f"{pressure_match.group()}, direct={direct_pressure.group() if direct_pressure else None}"
            )
        for path in pressure_files:
            w, h = 960, 720
            if path.stat().st_size != len(f"P6\n{w} {h}\n255\n") + w*h*3:
                raise RuntimeError(f"truncated backpressure frame: {path}")

        # A blocked writer must preserve the *correct* frame association,
        # not merely produce the right number of valid-sized images.
        _, pressure_sync_files = execute(
            args.binary.resolve(), project, root, "backpressure-sync",
            48, 2, asynchronous=False, every=8,
        )
        for i, sync_frame in enumerate(pressure_sync_files, 1):
            frame_number = i * 8
            if sha256_ppm(sync_frame, 2) != sha256_ppm(
                    pressure_files[frame_number - 1], 2):
                raise RuntimeError(
                    f"backpressure frame identity corrupted: {frame_number}"
                )

        if args.direct_map:
            # A writer error while a GL PBO is still borrowed must not
            # deadlock the GL thread or attempt to reuse a mapped buffer.
            failed_result, _ = execute(
                args.binary.resolve(), project, root, "fail-direct",
                8, 2, asynchronous=True, rgb_readback=args.rgb,
                direct_map=True,
                prefix_override=root / "nonexistent-parent" / "frame",
                expect_failure=True,
            )
            if (failed_result.returncode != 2 or
                    "[async PPM] cannot save" not in failed_result.stderr or
                    "XYO_SHOT_SERIES: asynchronous" not in failed_result.stderr):
                raise RuntimeError(
                    "direct mapped PBO writer error not propagated: "
                    + failed_result.stderr[-2000:]
                )

        print(
            f"PASS: {args.frames}/{args.frames} {480*args.scale}x{360*args.scale} "
            f"{format_label} PBO frames; {expected_bytes} transfer bytes; "
            f"{len(sync_files)} sync comparisons; "
            f"one-frame drain; peak_pbos={peak_pbos}/4, "
            f"peak_queue={peak_queue}/8, queue_waits={queue_waits}, "
            f"queue_wait_ms={queue_ms}, GPU_waits={gpu_waits}, "
            f"GPU_wait_ms={gpu_ms}; "
            f"CPU buffers alloc={allocations} reused={reuses} "
            f"peak_pool={max_pool}; "
            f"direct_map={enabled} copied_bytes={stage_match.group(6)}; "
            f"48-frame forced writer backpressure: 6 sync hashes match; "
            f"queue_waits={pressure_match.group(7)}, "
            f"queue_wait_ms={pressure_match.group(8)} "
            f"pbo_worker_waits={direct_pressure.group(4) if direct_pressure else '?'}",
            flush=True,
        )


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
