#!/usr/bin/env python3
"""RT-001: verify REAL machine StackMap locations against compiler site IDs.

The probe emits llvm.experimental.stackmap *after* O2/O3 on the native
capture helper. No GC roots or collection permission is inferred.
"""
from __future__ import annotations

import argparse
from collections import Counter
import os
from pathlib import Path
import re
import subprocess

from native_simple_resume import execute, fixture

EMITTED = re.compile(
    r"^\[rt001-machine-stackmap\] tier=(eager|lazy) fn=(\S+) "
    r"site=(\d+) stackmap_id=(\d+) unknown_ptrs=(\d+) "
    r"context_ptrs=(\d+) emitted=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)
TOTAL = re.compile(
    r"^\[rt001-machine-stackmap-total\] tier=(eager|lazy) "
    r"sites=(\d+) emitted=(\d+) refused=(\d+) "
    r"machine_roots_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)
RECORD = re.compile(
    r"Record ID: (\d+), instruction offset: (\d+)\s+"
    r"(\d+) locations:"
)


def run_tier(
    native: Path, project: Path, out: Path, tier: str, expected: dict,
    readobj: Path,
) -> None:
    output_dir = out / tier
    output_dir.mkdir(parents=True, exist_ok=True)
    for obj in output_dir.glob("*.o"):
        obj.unlink()

    original = {
        name: os.environ.get(name)
        for name in (
            "XYO_JIT_LAZY_MODE", "XYO_RT_MACHINE_STACKMAP_PROBE",
            "XYO_RT_POSTOPT_SSA_AUDIT", "XYO_JIT_DUMP_OBJECT",
        )
    }
    try:
        os.environ["XYO_JIT_LAZY_MODE"] = (
            "on" if tier == "lazy" else "off"
        )
        os.environ["XYO_RT_MACHINE_STACKMAP_PROBE"] = "1"
        os.environ["XYO_RT_POSTOPT_SSA_AUDIT"] = "1"
        os.environ["XYO_JIT_DUMP_OBJECT"] = str(
            (output_dir / "stackmap").resolve()
        )
        observed, trace = execute(
            native, project, "eager", None,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=True,
            soft_gc_budget_mb=1,
        )
    finally:
        for name, value in original.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value

    if observed != expected:
        raise AssertionError(f"{tier}: StackMap changed Scratch result")

    declared = {}
    records = EMITTED.findall(trace)
    if len(records) != 4:
        raise AssertionError(
            f"{tier}: expected 4 compiler StackMap sites, got "
            f"{len(records)}\n{trace[-2000:]}"
        )
    scripts = set()
    for (
        logged_tier, fn, site, identifier, unknown, context,
        emitted, gc_safe,
    ) in records:
        if (logged_tier != tier or emitted != "1" or
                gc_safe != "0" or context != "1" or
                int(unknown) < 1 or int(site) not in (1, 2)):
            raise AssertionError(f"{tier}: invalid site record {fn}:{site}")
        scripts.add(fn)
        stackmap_id = int(identifier)
        if stackmap_id in declared:
            raise AssertionError(f"{tier}: duplicate StackMap ID")
        declared[stackmap_id] = 1 + int(unknown)
    if len(scripts) != 2:
        raise AssertionError(f"{tier}: expected 2 compiled scripts")

    totals = [
        result for result in TOTAL.findall(trace)
        if result[0] == tier and int(result[1]) > 0
    ]
    if len(totals) != 2 or any(
        sites != "2" or emitted != "2" or refused != "0" or
        machine_roots != "0" or safe != "0"
        for _, sites, emitted, refused, machine_roots, safe in totals
    ):
        raise AssertionError(f"{tier}: incorrect fail-closed totals {totals}")

    objects = sorted(output_dir.glob("*.o"))
    if not objects:
        raise AssertionError(f"{tier}: ORC did not emit ELF objects")
    found: Counter[int] = Counter()
    for obj in objects:
        read = subprocess.run(
            [str(readobj), "--stackmap", str(obj)],
            text=True, capture_output=True, check=True,
        )
        for identifier, offset, locations in RECORD.findall(read.stdout):
            stackmap_id = int(identifier)
            count = int(locations)
            if stackmap_id not in declared:
                raise AssertionError(
                    f"{tier}: undeclared machine record {stackmap_id}"
                )
            if count != declared[stackmap_id]:
                raise AssertionError(
                    f"{tier}: StackMap ID {stackmap_id} expected "
                    f"{declared[stackmap_id]} locations; actual {count}"
                )
            found[stackmap_id] += 1
    if set(found) != set(declared) or any(v != 1 for v in found.values()):
        raise AssertionError(
            f"{tier}: missing/duplicate real machine locations: {found}"
        )
    print(
        f"PASS RT-001 {tier}: 4/4 machine StackMap records; "
        f"locations={[declared[key] for key in sorted(declared)]}; "
        f"2 Scratch Fibers match Interpreter; GC veto retained"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--readobj", type=Path, default=Path("llvm-readobj"))
    args = parser.parse_args()
    native = args.native.resolve()
    out = args.out.resolve()
    project = fixture(out, "ssa_budget_cross_fiber_two_hats")
    expected, _ = execute(native, project, "off", None)
    for tier in ("eager", "lazy"):
        run_tier(native, project, out, tier, expected, args.readobj)


if __name__ == "__main__":
    main()
