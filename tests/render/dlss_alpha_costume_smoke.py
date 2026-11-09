#!/usr/bin/env python3
"""End-to-end opt-in DLSS costume alpha classification on an NVIDIA GPU.

Synthesizes PNG/SVG Scratch projects from examples/fx_demo.sb3. No images
or SDK binaries are committed, and no third-party Python packages required.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[2]
LOG_TRIGGER = "[real DLSS] alpha-safe: visible partial opacity"
LOG_NGX = "[real DLSS] NVIDIA NGX initialized"


def png_image(alpha: int, edge_only: bool = False) -> bytes:
    width, height = 48, 48
    if edge_only:
        # Opaque center with a 1px intermediate-alpha antialias border.
        rows = []
        for y in range(height):
            row = bytearray(b"\x00")
            for x in range(width):
                a = 255 if 16 <= x < 32 and 16 <= y < 32 else (
                    128 if 15 <= x <= 32 and 15 <= y <= 32 else 0)
                row.extend((230, 30, 36, a))
            rows.append(bytes(row))
        pixels = b"".join(rows)
    else:
        pixels = (b"\x00" + bytes([230, 30, 36, alpha]) * width) * height
    def chunk(tag: bytes, content: bytes) -> bytes:
        return (struct.pack(">I", len(content)) + tag + content +
                struct.pack(">I", zlib.crc32(tag + content) & 0xFFFFFFFF))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                       8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(pixels, 9)) +
            chunk(b"IEND", b""))


def svg_image(alpha: int, rounded: bool = False) -> bytes:
    opacity = "0.5" if alpha == 128 else "1"
    shape = ('<circle cx="24" cy="24" r="17" fill="#e61e24"/>' if rounded else
             f'<rect width="48" height="48" fill="#e61e24" '
             f'fill-opacity="{opacity}"/>')
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" '
        'viewBox="0 0 48 48">' + shape + '</svg>'
    ).encode()


def project(template: ZipFile, output: Path, fmt: str, alpha: int,
            variant: str = "flat") -> None:
    spec = json.loads(template.read("project.json"))
    stage = next(t for t in spec["targets"] if t["isStage"])
    sprite = next(t for t in spec["targets"] if t["name"] == "fx-ghost")
    sprite["blocks"] = {}     # no ghost-effect: costume alpha alone
    sprite["name"] = f"alpha-{fmt}-{alpha}"
    sprite["visible"] = variant != "hidden"
    sprite["x"] = 1000 if variant == "offscreen" else 0
    sprite["y"] = 0
    sprite["size"] = 220
    image = (png_image(alpha, edge_only=variant == "aa-edge")
             if fmt == "png" else
             svg_image(alpha, rounded=variant == "circle"))
    asset_id = hashlib.md5(image).hexdigest()
    asset_name = f"{asset_id}.{fmt}"
    costume = sprite["costumes"][0]
    costume["dataFormat"] = fmt
    costume["assetId"] = asset_id
    costume["md5ext"] = asset_name
    costume["bitmapResolution"] = 1
    costume["rotationCenterX"] = 24
    costume["rotationCenterY"] = 24
    sprite["costumes"] = [costume]
    sprite["currentCostume"] = 0
    spec["targets"] = [stage, sprite]
    spec["monitors"] = []
    with ZipFile(output, "w", compression=ZIP_DEFLATED) as archive:
        archive.writestr("project.json", json.dumps(spec))
        for stage_costume in stage["costumes"]:
            name = stage_costume["md5ext"]
            archive.writestr(name, template.read(name))
        archive.writestr(asset_name, image)


def verify(binary: str, sb3: Path, fmt: str,
           alpha: int, variant: str = "flat") -> None:
    env = os.environ.copy()
    env.update({
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_ALPHA_SAFE": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_EXIT_FRAMES": "9",
        "XYO_PEN_FSR": "0",
    })
    result = subprocess.run(
        [binary, "play", str(sb3), "--scale", "2"],
        env=env, capture_output=True, text=True, timeout=25, check=False,
    )
    output = result.stdout + result.stderr
    expected = alpha != 255 and variant == "flat"
    observed = LOG_TRIGGER in output
    ngx = LOG_NGX in output
    print(f"{fmt} alpha={alpha} variant={variant}: trigger={observed}, "
          f"expected={expected}, NGX={ngx}, exit={result.returncode}")
    if result.returncode or not ngx or observed != expected:
        raise RuntimeError(f"GPU costume alpha classification failed:\n{output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", help="built xyo-cli executable")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="xyo-alpha-costume-") as temp:
        with ZipFile(ROOT / "examples" / "fx_demo.sb3") as template:
            cases = (
                ("png", 255, "flat"), ("png", 128, "flat"),
                ("png", 255, "aa-edge"), ("png", 128, "offscreen"),
                ("svg", 255, "flat"), ("svg", 128, "flat"),
                ("svg", 255, "circle"), ("svg", 128, "hidden"),
            )
            for fmt, alpha, variant in cases:
                sb3 = Path(temp) / f"alpha-{fmt}-{alpha}-{variant}.sb3"
                project(template, sb3, fmt, alpha, variant)
                verify(args.binary, sb3, fmt, alpha, variant)
    print("PASS: PNG/SVG opaque, alpha-filled and antialias-only native NGX")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
