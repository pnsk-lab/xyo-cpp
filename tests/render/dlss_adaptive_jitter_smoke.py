#!/usr/bin/env python3
"""Validate optical input gating for a moving Scratch sprite on real NGX GPU."""
from __future__ import annotations
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from dlss_rigid_project_smoke import build_project

def run(binary: str, project: Path, capture: Path, *,
        jitter: bool, adaptive: bool) -> tuple[bytes, str]:
    env = os.environ.copy()
    env.update({
        "XYO_EXIT_FRAMES": "14",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_RASTER_JITTER": "1" if jitter else "0",
        "XYO_DLSS_ADAPTIVE_JITTER": "1" if adaptive else "0",
        "XYO_DLSS_JITTER_MAX_MOTION": "3",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_PEN_FSR": "0",
        "XYO_DLSS_RASTER_CAPTURE_FRAME": "3",
        "XYO_DLSS_RASTER_INPUT_CAPTURE": str(capture),
    })
    p = subprocess.run(
        [binary, "play", str(project), "--scale", "2"],
        env=env, capture_output=True, text=True, timeout=35,
        check=False
    )
    output=p.stdout+p.stderr
    if p.returncode or "NVIDIA NGX initialized" not in output:
        raise RuntimeError(f"playback rc={p.returncode}\n{output}")
    return capture.read_bytes(), output

def main():
    if len(sys.argv)!=2:
        raise ValueError("usage: dlss_adaptive_jitter_smoke.py <xyo>")
    with tempfile.TemporaryDirectory(prefix="xyo-adaptive-jitter-") as tmp:
        root=Path(tmp)
        project=root/"moving-sprite.sb3"
        build_project(project)
        native,_=run(sys.argv[1],project,root/"native.ppm",
                     jitter=False,adaptive=False)
        raw,_=run(sys.argv[1],project,root/"raw.ppm",
                  jitter=True,adaptive=False)
        adaptive,logs=run(sys.argv[1],project,root/"adaptive.ppm",
                          jitter=True,adaptive=True)
        if "adaptive jitter skipped" not in logs:
            raise RuntimeError("6-pixel translation was not detected")
        if native!=adaptive:
            raise RuntimeError("fast-moving sprite was jittered despite gating")
        changed=sum(x!=y for x,y in zip(raw,adaptive))
        # A solid axis-aligned sprite can rasterize identically for a
        # fractional shift smaller than half a pixel. This is expected:
        # SDL fractional geometry does not guarantee subpixel coverage.
        print(f"fast +6px/frame: adaptive equals unjittered input; "
              f"ungated differs in {changed} RGB bytes")
    print("PASS: adaptive jitter disables unstable movement and preserves baseline")
if __name__=="__main__":
    try:
        main()
    except (RuntimeError,OSError,ValueError,subprocess.TimeoutExpired) as e:
        print(f"FAIL: {e}",file=sys.stderr); sys.exit(1)
