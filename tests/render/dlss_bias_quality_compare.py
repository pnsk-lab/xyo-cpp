#!/usr/bin/env python3
"""Compare NGX current-color-bias masks against temporal and safety modes.

Runs the same 90-frame synthetic alpha-occlusion scene on real NVIDIA GPU.
This is a diagnostic regression; it must not claim that DLSS uses the bias.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

SCORE = re.compile(
    r"DLSS moving alpha\+occlusion: trailing red ([0-9]+(?:\.[0-9]+)?)"
)

def run(executable: str, output: Path, mode: str):
    env = os.environ.copy()
    env.update({
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_SCENE_CUT_SYNC": "1",
        "XYO_DLSS_MOTION_COST_CLIP": "0",
        "XYO_DLSS_MODE": "performance",
        "XYO_DLSS_PRESET": "M",
        "XYO_DLSS_STRESS_FRAMES": "90",
        "XYO_DLSS_STRESS_ALPHA_OCCLUSION": "1",
        "XYO_DLSS_STRESS_ALPHA_SAFE": "1" if mode == "safe" else "0",
        "XYO_DLSS_ALPHA_BIAS": "1" if mode.startswith("bias") else "0",
        "XYO_DLSS_STRESS_ALPHA_BIAS":
            "1" if mode.startswith("bias") else "0",
        "XYO_DLSS_STRESS_ALPHA_BIAS_FULL":
            "1" if mode == "bias-full" else "0",
        "XYO_DLSS_ALPHA_BIAS_STRENGTH": "1",
        "XYO_DLSS_STRESS_CAPTURE_BMP": str(output),
    })
    result = subprocess.run(
        [executable], env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=40, check=False,
    )
    if result.returncode:
        raise RuntimeError(
            f"{mode}: exit={result.returncode}\n"
            f"{result.stdout}\n{result.stderr}"
        )
    score = SCORE.search(result.stdout)
    if not score or not output.is_file():
        raise RuntimeError(f"{mode}: missing score or BMP file\n{result.stdout}")
    return float(score.group(1)), output.read_bytes()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    args = parser.parse_args()
    modes = ("temporal", "bias-local", "bias-full", "safe")
    with tempfile.TemporaryDirectory(prefix="xyo-ngx-bias-") as temp:
        results = {
            mode: run(args.binary, Path(temp) / f"{mode}.bmp", mode)
            for mode in modes
        }
    normal_score, normal_image = results["temporal"]
    print(f"NGX temporal baseline: trailing red={normal_score:.3f}")
    for mode in modes[1:]:
        score, pixels = results[mode]
        if len(pixels) != len(normal_image):
            raise RuntimeError("BMP dimensions differ")
        # Ignore the 54-byte BMP header, which may hold metadata.
        changed = sum(a != b for a, b in
                      zip(normal_image[54:], pixels[54:]))
        print(f"{mode}: trailing red={score:.3f}; changed BMP bytes={changed}")
    if results["safe"][0] > normal_score + 0.01:
        raise RuntimeError("conservative alpha-safe path regressed")
    print("PASS: bias mask evaluated, output differences reported without "
          "assuming efficacy")
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
