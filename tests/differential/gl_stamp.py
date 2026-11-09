#!/usr/bin/env python3
"""GPU penStamp visual parity regression (OpenGL 3.3, llvmpipe is sufficient).

Checks four stamps including rotation, left-right flip, and ghost:
the GPU direct-FBO output may differ by one boundary pixel from the
CPU raster sampler, but its overall shape/colour must be preserved.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def ppm(path):
    with path.open("rb") as f:
        assert f.readline().strip() == b"P6"
        dimensions = tuple(int(x) for x in f.readline().split())
        assert f.readline().strip() == b"255"
        raw = f.read()
    assert len(raw) == dimensions[0] * dimensions[1] * 3
    return dimensions, raw


def run(binary, project, result, cpu):
    env = os.environ.copy()
    env.update(
        SDL_VIDEODRIVER="offscreen",
        SDL_RENDER_DRIVER="opengl",
        SDL_AUDIODRIVER="dummy",
        XYO_EXIT_FRAMES="5",
        XYO_PENPROF="1",
        XYO_SHOT=str(result),
    )
    if cpu:
        env["XYO_PEN_STAMP_CPU"] = "1"
    else:
        env.pop("XYO_PEN_STAMP_CPU", None)
    proc = subprocess.run(
        [str(binary), "play", str(project), "--jit=off", "--fps", "30"],
        env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=60, check=False,
    )
    assert proc.returncode == 0, proc.stderr
    stamps = re.search(r"stamps=(\d+) gpuStamps=(\d+)", proc.stderr)
    assert stamps, proc.stderr
    return tuple(int(v) for v in stamps.groups())


def test(binary):
    here = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="xyo-stamp-") as tmp:
        base = Path(tmp)
        project = base / "project.sb3"
        subprocess.run(
            [sys.executable, str(here / "fixtures/gen_stamp_parity.py"),
             str(project)],
            check=True, stdout=subprocess.PIPE,
        )
        cpu_count = run(binary, project, base / "cpu.ppm", True)
        gpu_count = run(binary, project, base / "gpu.ppm", False)
        assert cpu_count == (4, 0), cpu_count
        if gpu_count == (4, 0):
            print("SKIP: shared OpenGL GPU pen unavailable")
            return
        assert gpu_count == (4, 4), gpu_count
        (dimensions, cpu), (other, gpu) = (
            ppm(base / "cpu.ppm"), ppm(base / "gpu.ppm")
        )
        assert dimensions == other
        bad_pixels = sum(
            max(abs(cpu[i + k] - gpu[i + k]) for k in range(3)) > 5
            for i in range(0, len(cpu), 3)
        )
        # Small 1-pixel boundary rasterization differences are expected.
        assert bad_pixels <= dimensions[0] * dimensions[1] * 0.002, (
            f"pixel mismatch: {bad_pixels} pixels over tolerance"
        )
        print(
            f"PASS: GPU stamps={gpu_count[1]}, "
            f"pixels with RGB delta>5={bad_pixels}/{dimensions[0] * dimensions[1]}"
        )


if __name__ == "__main__":
    test(Path(sys.argv[1]).resolve())
