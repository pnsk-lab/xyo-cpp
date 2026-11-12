#!/usr/bin/env python3
"""ABBA renderer-draw comparison for CPU-quantized GPU Pen tile masks.

Measures end-to-end draw_ms (including NGX) rather than isolated shader time.
sparse: 61 disjoint strokes; untiled exceeds the work budget and uses CPU.
contiguous: 64 nearby strokes; both paths use the GPU shader.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import statistics
import tempfile

from dlss_pen_gpu_quantized_benchmark import measure
from dlss_pen_gpu_quantized_stress import make_sparse_strokes, make_strokes


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--scene", choices=("sparse", "contiguous"),
                        default="sparse")
    parser.add_argument("--blocks", type=int, default=2)
    parser.add_argument("--frames", type=int, default=32)
    args = parser.parse_args()
    if args.blocks < 1 or args.frames < 4:
        parser.error("blocks >= 1 and frames >= 4 required")

    times: dict[bool, list[float]] = {False: [], True: []}
    with tempfile.TemporaryDirectory(prefix="xyo-pen-tiled-bench-") as directory:
        root = Path(directory)
        project = root / "pen.sb3"
        if args.scene == "sparse":
            make_sparse_strokes(project)
        else:
            make_strokes(project, 63, wide=False)

        for block in range(args.blocks):
            sequence = ((False, True, True, False) if block % 2 == 0
                        else (True, False, False, True))
            for trial, tiled in enumerate(sequence):
                profile = root / f"{block}-{trial}.csv"
                draw_ms = measure(
                    args.binary.resolve(), project, profile,
                    scissor=True, frames=args.frames, tiled=tiled,
                    require_gpu=(args.scene != "sparse" or tiled),
                )
                times[tiled].append(draw_ms)
                print(f"block={block+1} trial={trial+1} tiled={int(tiled)} "
                      f"median_draw_ms={draw_ms:.4f}", flush=True)

    off = statistics.mean(times[False])
    on = statistics.mean(times[True])
    baseline = "CPU fallback" if args.scene == "sparse" else "untiled GPU"
    print(f"{args.scene}: {baseline}={off:.4f}ms, tiled GPU={on:.4f}ms, "
          f"draw reduction={(1-on/off)*100:.2f}%, "
          f"ratio={off/on:.3f}x", flush=True)


if __name__ == "__main__":
    main()
