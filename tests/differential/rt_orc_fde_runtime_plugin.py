#!/usr/bin/env python3
"""RT-001: test the ACTUAL xyo-cpp LLVMBackend ORC FDE callback plugin.

Build with -DXYO_RT001_ORC_FDE_PLUGIN=ON. Runtime registration still
requires XYO_RT_ORC_FDE_REGISTRATION=1 and must NEVER enable native GC.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re

from native_simple_resume import execute, fixture

INSTALL = re.compile(
    r"^\[rt001-orc-fde\] phase=install tier=(eager|lazy) "
    r"ready=1 gc_safe=0$", re.MULTILINE,
)
EMIT = re.compile(
    r"^\[rt001-orc-fde\] phase=emit registered=(\d+) "
    r"gc_safe=0$", re.MULTILINE,
)
REMOVE = re.compile(
    r"^\[rt001-orc-fde\] phase=remove canceled=(\d+) "
    r"gc_safe=0$", re.MULTILINE,
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--stackmap", action="store_true",
        help="Run together with the independent post-O3 StackMap probe",
    )
    args = parser.parse_args()
    native = args.native.resolve()
    project = fixture(args.out.resolve(), "ssa_budget_cross_fiber_two_hats")
    names = (
        "XYO_RT_ORC_FDE_REGISTRATION",
        "XYO_RT_MACHINE_STACKMAP_PROBE",
        "XYO_JIT_LAZY_MODE",
    )
    old = {name: os.environ.get(name) for name in names}
    try:
        # Benchmark correctness against the REAL interpreter, not
        # against an alternate JIT build or a cached expected value.
        os.environ["XYO_RT_ORC_FDE_REGISTRATION"] = "0"
        os.environ["XYO_RT_MACHINE_STACKMAP_PROBE"] = "0"
        expected, _ = execute(native, project, "off", None)
        default, default_trace = execute(
            native, project, "eager", None,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            require_generated_ssa_roots=True, soft_gc_budget_mb=1,
        )
        if default != expected or "[rt001-orc-fde]" in default_trace:
            raise AssertionError(
                "experimental ORC FDE plugin changed JIT with flag OFF"
            )
        for tier in ("eager", "lazy"):
            os.environ["XYO_RT_MACHINE_STACKMAP_PROBE"] = (
                "1" if args.stackmap else "0"
            )
            os.environ["XYO_RT_ORC_FDE_REGISTRATION"] = "1"
            os.environ["XYO_JIT_LAZY_MODE"] = (
                "on" if tier == "lazy" else "off"
            )
            actual, trace = execute(
                native, project, "eager", None,
                llvm_ssa_liveness=True, detached_llvm_ssa=True,
                require_generated_ssa_roots=True,
                soft_gc_budget_mb=1,
            )
            if actual != expected:
                raise AssertionError(
                    f"{tier}: LLVM native result != interpreter"
                )
            installed = INSTALL.findall(trace)
            if tier not in installed:
                raise AssertionError(
                    f"{tier}: missing ORC lifecycle plugin install"
                )
            emits = [int(n) for n in EMIT.findall(trace)]
            removed = [int(n) for n in REMOVE.findall(trace)]
            if not emits or not removed or sum(emits) < 2 or (
                sum(emits) != sum(removed)
            ):
                raise AssertionError(
                    f"{tier}: FDE lifecycle leak, "
                    f"emits={emits}, removed={removed}, "
                    f"tail={trace[-3000:]}"
                )
            if "phase=emit fde_missing=1" in trace or (
                "phase=emit invalid_extent=1" in trace
            ):
                raise AssertionError(
                    f"{tier}: incomplete FDE registration"
                )
            if args.stackmap:
                summaries = [
                    line for line in trace.splitlines()
                    if line.startswith("[rt001-machine-stackmap-total]")
                ]
                if not summaries or any(
                    "machine_roots_proven=0 gc_safe=0" not in line
                    for line in summaries
                ):
                    raise AssertionError(
                        f"{tier}: invalid/unavailable machine StackMap "
                        "diagnostic or erroneous GC authorization"
                    )
            print(
                f"PASS RT-001 native {tier}: "
                f"{sum(emits)} machine FDE leases installed, "
                f"{sum(removed)} canceled; "
                "Interpreter parity, GC permission unchanged; "
                f"StackMap={'on' if args.stackmap else 'off'}"
            )
    finally:
        for name, value in old.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


if __name__ == "__main__":
    main()
