#!/usr/bin/env python3
"""Real NVIDIA GPU pen replay vs CPU replay and unsafe-history fallback."""
from __future__ import annotations
import os
from pathlib import Path
import sys
import tempfile
from dlss_pen_primitive_smoke import make_case, run

def main() -> None:
    if len(sys.argv)!=2:
        raise ValueError("usage: dlss_pen_gpu_replay_smoke.py <xyo>")
    with tempfile.TemporaryDirectory(prefix="xyo-pen-gpu-replay-") as temp:
        root=Path(temp)
        for case in ("replay", "stamp", "uncleared"):
            project=root/f"{case}.sb3"
            make_case(project,case)
            os.environ["XYO_DLSS_PEN_REPLAY_GPU"]="0"
            baseline,baseLog=run(sys.argv[1],project,root/f"{case}-cpu.ppm",True)
            os.environ["XYO_DLSS_PEN_REPLAY_GPU"]="1"
            actual,gpuLog=run(sys.argv[1],project,root/f"{case}-gpu.ppm",True)
            gpuUsed="GPU pen primitive rerasterization active" in gpuLog
            cpuUsed="pen primitive rerasterization active" in baseLog
            changed=sum(a!=b for a,b in zip(baseline,actual))
            print(f"{case}: GPU={gpuUsed} CPU={cpuUsed} differing_RGB_bytes={changed}")
            if case=="replay":
                if not gpuUsed or not cpuUsed:
                    raise RuntimeError("eligible pen replay did not execute both paths")
                if len(actual)!=len(baseline) or changed>40:
                    raise RuntimeError("GPU raster differs excessively from CPU reference")
            else:
                if gpuUsed or cpuUsed or changed:
                    raise RuntimeError("unsafe or unknown Pen history did not fall back")
    print("PASS: NVIDIA GPU Pen replay closely matches CPU and honors fallback rules")

if __name__=="__main__":
    try:main()
    except (OSError,RuntimeError,ValueError) as error:
        print("FAIL:",error,file=sys.stderr);sys.exit(1)
