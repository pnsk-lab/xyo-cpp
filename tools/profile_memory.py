#!/usr/bin/env python3
"""Measure peak process RSS and Linux memory-map breakdown for xyo-cpp.

Uses /proc/<pid>/smaps, without modifying the target process. Categories describe
OS mappings, not exact ownership by LLVM, GC, Scratch, or SDL.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def classify(name: str) -> str:
    if name == "[heap]":
        return "heap"
    if name.startswith("[stack"):
        return "stack"
    if name.startswith("/") and ("libLLVM" in name or "/llvm" in name.lower()):
        return "llvm-file"
    if name.startswith("/") and any(x in name.lower() for x in ("libgl", "dri/", "vulkan", "libegl")):
        return "graphics-file"
    if name.startswith("/") and any(x in name.lower() for x in ("libSDL".lower(), "libwayland", "libx11")):
        return "window-file"
    if name.startswith("/") or name.startswith("/memfd:"):
        return "other-file"
    return "anonymous"


def snapshot(pid: int) -> dict[str, object]:
    groups: dict[str, dict[str, int]] = {}
    current = None
    vmas = []
    # Each VMA header starts with start-end addresses; only Rss/Private_Dirty/
    # Pss and Size are aggregated, never sum maps and smaps_rollup together.
    with open(f"/proc/{pid}/smaps", encoding="utf-8") as handle:
        for line in handle:
            first = line.split(" ", 1)[0]
            if "-" in first and all(c in "0123456789abcdef" for c in first.replace("-", "")):
                name = line.strip().split(maxsplit=5)
                category = classify(name[5] if len(name) > 5 else "")
                current = groups.setdefault(category, {"rss_kib": 0, "pss_kib": 0, "private_dirty_kib": 0})
                vmas.append({"category": category, "name": name[5] if len(name) > 5 else "", "range": name[0], "rss_kib": 0})
            elif current is not None:
                field, _, value = line.partition(":")
                key = {"Rss": "rss_kib", "Pss": "pss_kib", "Private_Dirty": "private_dirty_kib"}.get(field)
                if key:
                    amount = int(value.strip().split()[0])
                    current[key] += amount
                    if key == "rss_kib":
                        vmas[-1]["rss_kib"] = amount
    return groups, sorted(vmas, key=lambda x: x["rss_kib"], reverse=True)[:12]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("--binary", type=Path, default=Path("build/xyo"))
    parser.add_argument("--frames", type=int, default=150)
    parser.add_argument("--interval", type=float, default=0.025)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--jit", choices=["off", "hotspot", "eager"], default="hotspot")
    parser.add_argument("--lazy", choices=["auto", "on", "off"], default="auto")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--stderr-log", type=Path)
    parser.add_argument("--list-limit", type=int, default=0)
    parser.add_argument("--rss-limit-mib", type=int, default=0, help="Kill child once observed RSS reaches this threshold")
    args = parser.parse_args()
    if args.frames < 1 or args.interval <= 0 or args.timeout <= 0:
        parser.error("frames, interval and timeout must be positive")

    binary = args.binary.resolve()
    project = args.project.resolve()
    command = [str(binary), "run", str(project), "--frames", str(args.frames),
               "--jit", args.jit, "--jit-lazy", args.lazy]
    if args.list_limit:
        command += ["--list-limit", str(args.list_limit)]
    env = os.environ.copy()
    start = time.monotonic()
    with subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                          env=env) as proc:
        max_snapshot: dict[str, object] = {}
        max_rss = 0
        max_vmas = []
        samples = 0
        timed_out = False
        rss_limited = False
        milestones = []
        previous_band = -1
        while proc.poll() is None:
            try:
                groups, top_vmas = snapshot(proc.pid)
                rss = sum(group["rss_kib"] for group in groups.values())
                if rss > max_rss:
                    max_rss, max_snapshot, max_vmas = rss, groups, top_vmas
                samples += 1
                band = rss // (64 * 1024)
                if band > previous_band:
                    milestones.append({"elapsed_s": round(time.monotonic() - start, 2), "rss_kib": rss, "anonymous_kib": groups.get("anonymous", {}).get("rss_kib", 0), "heap_kib": groups.get("heap", {}).get("rss_kib", 0)})
                    previous_band = band
                if args.rss_limit_mib and rss >= args.rss_limit_mib * 1024:
                    rss_limited = True
                    proc.kill()
                    break
            except (FileNotFoundError, ProcessLookupError):
                break
            if time.monotonic() - start > args.timeout:
                timed_out = True
                proc.kill()
                break
            time.sleep(args.interval)
        _, stderr = proc.communicate()
    if args.stderr_log:
        args.stderr_log.parent.mkdir(parents=True, exist_ok=True)
        args.stderr_log.write_bytes(stderr)
    result = {
        "command": command, "returncode": proc.returncode, "frame_limit_reached": proc.returncode == 2, "timed_out": timed_out, "rss_limited": rss_limited, "milestones": milestones,
        "elapsed_s": round(time.monotonic() - start, 3), "samples": samples,
        "sampled_peak_rss_kib": max_rss, "at_sampled_peak": max_snapshot, "largest_mappings": max_vmas,
        "stderr_tail": stderr.decode("utf-8", errors="replace")[-1500:],
        "note": "Sampling can miss brief peaks. Categories are OS mappings, not subsystem allocations.",
    }
    payload = json.dumps(result, indent=2, ensure_ascii=False)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload + "\n", encoding="utf-8")
    print(payload)
    return 0 if proc.returncode in (0, 2) and not timed_out and not rss_limited else 1


if __name__ == "__main__":
    raise SystemExit(main())
