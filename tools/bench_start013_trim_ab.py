#!/usr/bin/env python3
"""Paired startup/full comparison of optional AOT runtime settings.

Both cases use the exact same native binary, project and AOT object.
Only the selected environment variable differs. An AOT object must never be
reused with a concurrently regenerated Scratch parity project.
"""
from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import statistics

from bench_start012_startup_ab import first_output_ms
from bench_start012_full_ab import run as full_run


def measure(binary: Path, project: Path, obj: Path, cpu: int,
            full: bool, enabled: bool,
            switch: str = "XYO_AOT_LAZY_MALLOC_TRIM") -> tuple[float, int | None]:
    previous = os.environ.get(switch)
    os.environ[switch] = "1" if enabled else "0"
    try:
        if full:
            elapsed, rss = full_run(binary, project, obj, cpu, parallel="0")
            return elapsed, rss
        return first_output_ms(binary, project, obj, cpu), None
    finally:
        if previous is None:
            os.environ.pop(switch, None)
        else:
            os.environ[switch] = previous


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--aot", required=True, type=Path)
    parser.add_argument("--cpu", type=int, default=10)
    parser.add_argument("--pairs", type=int, default=20)
    parser.add_argument("--full", action="store_true",
                        help="Run all 2656 parity tests and sample VmHWM; otherwise first output")
    parser.add_argument("--switch", default="XYO_AOT_LAZY_MALLOC_TRIM",
                        choices=("XYO_AOT_LAZY_MALLOC_TRIM", "XYO_AOT_FAST_META"),
                        help="Which opt-in AOT experiment to compare (default: trim)")
    options = parser.parse_args()
    binary, project, obj = (p.resolve()
                            for p in (options.bin, options.project, options.aot))
    for path in (binary, project, obj):
        if not path.is_file():
            raise FileNotFoundError(path)
    if options.pairs < 1:
        parser.error("--pairs must be positive")

    active = "trim" if options.switch == "XYO_AOT_LAZY_MALLOC_TRIM" else "enabled"
    values: dict[str, list[tuple[float, int | None]]] = {
        "control": [], active: [],
    }
    for i in range(options.pairs):
        order = ("control", active) if i % 2 == 0 else (active, "control")
        for label in order:
            values[label].append(measure(
                binary, project, obj, options.cpu,
                options.full, label == active, options.switch,
            ))
        print(
            f"pair={i + 1}: "
            + " ".join(
                f"{key}={values[key][-1][0]:.2f}ms/"
                f"{values[key][-1][1] if options.full else '-'}KiB"
                for key in ("control", active)
            ),
            flush=True,
        )
    a = [row[0] for row in values["control"]]
    b = [row[0] for row in values[active]]
    geo = math.exp(statistics.fmean(math.log(x / y) for x, y in zip(a, b)))
    print(
        f"median_control={statistics.median(a):.2f}ms "
        f"median_{active}={statistics.median(b):.2f}ms "
        f"{active}_wins={sum(x > y for x, y in zip(a,b))}/{options.pairs} "
        f"geomean_control_over_{active}={geo:.4f}",
        flush=True,
    )
    if options.full:
        print(
            f"rss_median_control={statistics.median(int(x[1]) for x in values['control']):.0f}KiB "
            f"rss_median_{active}={statistics.median(int(x[1]) for x in values[active]):.0f}KiB",
            flush=True,
        )


if __name__ == "__main__":
    main()
