#!/usr/bin/env python3
"""RT-001: remove ONLY the proven numeric yield counter from i64 SSA maps.

Compare exact real LLVM23 ORC Eager/Lazy source manifests and ELF
StackMap counts under identical Scratch fixture and opt-in flags.
This is diagnostic-only; it NEVER establishes GC roots/safepoints.
"""
from __future__ import annotations

import argparse
from collections import Counter
import os
from pathlib import Path
import re
import subprocess

from native_simple_resume import execute, fixture
from rt_yield_live_ssa_machine import SSA, validate_machine
from rt_yield_ssa_spill_bounds import parse_machine_locations

EXCLUDED = re.compile(
    r"^\[rt001-yield-ssa-numeric-excluded\] tier=(eager|lazy) "
    r"fn=(\S+) ordinal=(\d+) stackmap_id=(\d+) "
    r"scheduler_ticks=(\d+) other_unproven_i64=(\d+) "
    r"ownership_proven=(\d+) machine_roots_proven=(\d+) "
    r"gc_safe=(\d+)$", re.MULTILINE,
)
MANAGED = (
    "XYO_JIT_LAZY_MODE", "XYO_RT_YIELD_EDGE_STACKMAP_PROBE",
    "XYO_RT_YIELD_LIVE_SSA_STACKMAP", "XYO_JIT_DUMP_OBJECT",
    "XYO_RT_MACHINE_STACKMAP_PROBE", "XYO_RT_YIELD_SSA_FILTER",
    "XYO_RT_YIELD_SSA_TRACE_OPERANDS",
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--readobj", default="llvm-readobj")
    parser.add_argument("--objdump", default="llvm-objdump")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    native = args.native.resolve()
    project = fixture(out, "ssa_budget_cross_fiber_two_hats")
    prior = {key: os.environ.get(key) for key in MANAGED}
    try:
        os.environ["XYO_RT_YIELD_EDGE_STACKMAP_PROBE"] = "0"
        os.environ["XYO_RT_YIELD_LIVE_SSA_STACKMAP"] = "0"
        os.environ["XYO_RT_MACHINE_STACKMAP_PROBE"] = "0"
        os.environ["XYO_RT_YIELD_SSA_TRACE_OPERANDS"] = "0"
        reference, _ = execute(native, project, "off", None)
        for tier in ("eager", "lazy"):
            manifests = {}
            volatile = {}
            for mode in ("i64", "i64_no_tick"):
                folder = out / tier / mode
                folder.mkdir(parents=True, exist_ok=True)
                for obj in folder.glob("*.o"):
                    obj.unlink()
                os.environ["XYO_JIT_LAZY_MODE"] = (
                    "on" if tier == "lazy" else "off"
                )
                os.environ["XYO_RT_YIELD_EDGE_STACKMAP_PROBE"] = "1"
                os.environ["XYO_RT_YIELD_LIVE_SSA_STACKMAP"] = "1"
                os.environ["XYO_RT_YIELD_SSA_FILTER"] = mode
                os.environ["XYO_JIT_DUMP_OBJECT"] = str(
                    (folder / "ssa").resolve()
                )
                actual, trace = execute(
                    native, project, "eager", None,
                    llvm_ssa_liveness=True, detached_llvm_ssa=True,
                    lazy_typed_ssa=True,
                    require_generated_ssa_roots=True,
                    soft_gc_budget_mb=1,
                )
                if actual != reference:
                    raise AssertionError(
                        f"{tier}/{mode}: experimental numeric filtering "
                        "changed Scratch variables"
                    )
                count, candidates, pointers = validate_machine(
                    folder, trace, tier,
                    args.readobj, args.objdump,
                )
                if count != 4 or candidates < 4:
                    raise AssertionError(
                        f"{tier}/{mode}: insufficient real ORC SSA sites"
                    )
                source = {}
                id_set = set()
                for row in SSA.findall(trace):
                    found_tier, fn, ordinal, helper, ident = row[:5]
                    if found_tier != tier:
                        raise AssertionError("foreign JIT tier in SSA log")
                    key = (fn, int(ordinal))
                    if key in source:
                        raise AssertionError("duplicate compiler source")
                    location_count = int(row[10])
                    source[key] = (int(row[5]), location_count, int(ident))
                    id_set.add(int(ident))
                if len(source) != 4 or len(id_set) != 4:
                    raise AssertionError(
                        f"{tier}/{mode}: incomplete emitted source map"
                    )
                manifests[mode] = source
                machines = {}
                for obj in folder.glob("*.o"):
                    data = subprocess.run(
                        [args.readobj, "--stackmap", str(obj)],
                        text=True, capture_output=True, check=True,
                    ).stdout
                    for ident, machine in parse_machine_locations(
                        data, id_set
                    ).items():
                        if ident in machines:
                            raise AssertionError("duplicate machine SSA ID")
                        machines[ident] = machine
                if set(machines) != id_set:
                    raise AssertionError("machine SSA record lost")
                volatile[mode] = sum(
                    kind == "volatile_unrecoverable"
                    for _pc, locations in machines.values()
                    for kind, _reg, _offset in locations
                )
                skipped = EXCLUDED.findall(trace)
                if mode == "i64":
                    if skipped:
                        raise AssertionError(
                            "legacy i64 mode unexpectedly removed a tick"
                        )
                else:
                    if len(skipped) != 4:
                        raise AssertionError(
                            "no proof for every filtered scheduler "
                            f"tick: {skipped}"
                        )
                    for (found_tier, fn, ordinal, marker, ticks, others,
                         ownership, roots, safe) in skipped:
                        key = (fn, int(ordinal))
                        if (found_tier != tier or ticks != "1" or
                            (ownership, roots, safe) != ("0", "0", "0") or
                            key not in source or
                            int(marker) != source[key][2] or
                            int(others) != source[key][0] - 1):
                            raise AssertionError(
                                f"unsafe counter-elision proof: {key}"
                            )

            ordinary, filtered = (
                manifests["i64"], manifests["i64_no_tick"]
            )
            if set(ordinary) != set(filtered):
                raise AssertionError(
                    f"{tier}: failed source-to-machine site bijection"
                )
            for site in ordinary:
                n_i64, n_locations, _ = ordinary[site]
                trimmed_i64, trimmed_locations, _ = filtered[site]
                if (n_i64 != trimmed_i64 or
                    n_locations != trimmed_locations + 1):
                    raise AssertionError(
                        f"{tier}: filtered a non-scheduler SSA or missed "
                        f"tick at {site}, before={ordinary[site]}, "
                        f"after={filtered[site]}"
                    )
            print(
                f"PASS RT-001 {tier}: 4/4 machine yield StackMaps "
                "exclude exactly one ABI-proven numeric scheduler "
                "tick per site; Interpreter parity; "
                f"volatile_homes_before={volatile['i64']} "
                f"after={volatile['i64_no_tick']}; "
                "ownership_proven=0 gc_safe=0",
                flush=True,
            )
    finally:
        for key, value in prior.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


if __name__ == "__main__":
    main()
