#!/usr/bin/env python3
"""Real NGX: PBO+writer captures must exactly match synchronous screenshots."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from dlss_pen_primitive_smoke import make_case


def play(binary: str, scene: Path, directory: Path, name: str,
         *, async_capture: bool, frames: int = 32, every: int = 1,
         prefix: Path | None = None, rgb_readback: bool = False
         ) -> tuple[subprocess.CompletedProcess[str], Path]:
    prefix = prefix if prefix is not None else directory / name
    final = directory / f"{name}-final.ppm"
    env = os.environ.copy()
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER",
                "EGL_PLATFORM"):
        env.pop(key, None)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_RASTER_JITTER": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_EXIT_FRAMES": str(frames),
        "XYO_SHOT_SERIES": str(prefix),
        "XYO_SHOT_SERIES_ASYNC": "1" if async_capture else "0",
        "XYO_SHOT_ASYNC_RGB": "1" if rgb_readback else "0",
        "XYO_SHOT_ASYNC_STATS": "1" if async_capture else "0",
        "XYO_SHOT_EVERY": str(every),
        "XYO_SHOT": str(final),
    })
    result = subprocess.run(
        [binary, "play", str(scene), "--scale", "2"],
        env=env, capture_output=True, text=True, timeout=70, check=False,
    )
    return result, final


def images(prefix: Path, frames: int, every: int) -> list[Path]:
    return [prefix.with_name(f"{prefix.name}-{frame:06}.ppm")
            for frame in range(every, frames + 1, every)]


def verified_ppm(path: Path) -> bytes:
    value = path.read_bytes()
    header = b"P6\n960 720\n255\n"
    if not value.startswith(header) or len(value) != len(header) + 960 * 720 * 3:
        raise RuntimeError(f"bad PPM bytes: {path}")
    return value


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_offscreen_pbo_smoke.py <xyo>")
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="xyo-ngxsdk-pbo-") as temp:
        root = Path(temp)
        scene = root / "pen.sb3"
        make_case(scene, "replay")
        baseline, base_final = play(
            binary, scene, root, "sync", async_capture=False,
        )
        pbo, pbo_final = play(
            binary, scene, root, "pbo", async_capture=True,
        )
        rgb, rgb_final = play(
            binary, scene, root, "pbo-rgb",
            async_capture=True, rgb_readback=True,
        )
        for name, result in (("sync", baseline), ("pbo", pbo),
                             ("pbo-rgb", rgb)):
            if result.returncode != 0:
                raise RuntimeError(
                    f"{name} failed ({result.returncode}): {result.stderr[-3000:]}"
                )
            if ("SDL video driver=offscreen" not in result.stderr or
                    "NVIDIA NGX initialized" not in result.stderr):
                raise RuntimeError(f"{name}: offscreen NGX not active")

        reference = images(root / "sync", 32, 1)
        asynchronous = images(root / "pbo", 32, 1)
        if sorted(root.glob("sync-??????.ppm")) != reference:
            raise RuntimeError("sync capture count incorrect")
        if sorted(root.glob("pbo-??????.ppm")) != asynchronous:
            raise RuntimeError("PBO capture lost or duplicated frames")
        for frame, (sync_path, pbo_path) in enumerate(
            zip(reference, asynchronous), 1
        ):
            if verified_ppm(sync_path) != verified_ppm(pbo_path):
                raise RuntimeError(f"PBO pixel mismatch at frame {frame}")

        if (verified_ppm(base_final) != verified_ppm(pbo_final) or
                verified_ppm(pbo_final) != verified_ppm(asynchronous[-1])):
            raise RuntimeError("PBO final image mismatch")

        rgb_files = images(root / "pbo-rgb", 32, 1)
        if sorted(root.glob("pbo-rgb-??????.ppm")) != rgb_files:
            raise RuntimeError("RGB PBO capture lost or duplicated frames")
        for frame, (baseline_path, rgb_path) in enumerate(
                zip(reference, rgb_files), 1):
            if verified_ppm(baseline_path) != verified_ppm(rgb_path):
                raise RuntimeError(f"RGB PBO mismatch at frame {frame}")
        if verified_ppm(rgb_final) != verified_ppm(base_final):
            raise RuntimeError("RGB PBO final image mismatch")
        if ("[async PPM] transfer format=RGB bytes_per_pixel=3 "
                "submitted_bytes=66355200" not in rgb.stderr):
            raise RuntimeError("RGB PBO telemetry bytes mismatch")
        if ("[async PPM] transfer format=RGBA bytes_per_pixel=4 "
                "submitted_bytes=88473600" not in pbo.stderr):
            raise RuntimeError("RGBA PBO telemetry bytes mismatch")

        sparse, sparse_final = play(
            binary, scene, root, "sparse", async_capture=True,
            frames=32, every=4,
        )
        if sparse.returncode:
            raise RuntimeError(f"PBO stride-4 failed: {sparse.stderr[-3000:]}")
        expected_sparse = images(root / "sparse", 32, 4)
        if sorted(root.glob("sparse-??????.ppm")) != expected_sparse:
            raise RuntimeError("PBO stride-4 lost frames")
        for idx, path in enumerate(expected_sparse, 4):
            frame_number = int(path.stem.rsplit("-", 1)[-1])
            if verified_ppm(path) != verified_ppm(reference[frame_number - 1]):
                raise RuntimeError(f"PBO stride-4 differs at {frame_number}")
        if verified_ppm(sparse_final) != verified_ppm(asynchronous[-1]):
            raise RuntimeError("PBO stride-4 final image mismatch")

        bad, _ = play(
            binary, scene, root, "bad",
            async_capture=True, frames=4,
            prefix=root / "missing-directory" / "bad",
        )
        if (bad.returncode != 2 or
                "[async PPM] cannot save" not in bad.stderr or
                "XYO_SHOT_SERIES: asynchronous" not in bad.stderr):
            raise RuntimeError(
                f"async I/O error not propagated to exit code: {bad.returncode}\n"
                f"{bad.stderr[-3000:]}"
            )

        print(
            "PASS: no DISPLAY/Wayland, real NGX RGBA and RGB PBO each "
            "captured 32/32 byte-exact frames; RGB transfer -25%; "
            "stride-4 8/8; final PPM identical; "
            "async background write failure returns 2",
            flush=True,
        )


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
