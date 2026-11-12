#!/usr/bin/env python3
"""RT-001: count all real machine i64 sources without granting GC."""
from __future__ import annotations

import argparse
from collections import Counter
import os
from pathlib import Path
import re
import subprocess

from native_simple_resume import execute, fixture
from rt_yield_live_ssa_machine import SSA, validate_machine
from rt_yield_ssa_spill_bounds import SOURCE, parse_machine_locations

ORIGIN = re.compile(
    r"^\[rt001-yield-ssa-origin\] tier=(eager|lazy) fn=(\S+) "
    r"ordinal=(\d+) stackmap_id=(\d+) location=(\d+) "
    r"source_id=(\d+) origin=(\w+) ownership_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)


def verify(log: str, tier: str, machine: dict) -> Counter:
    sites = {int(r[4]): (r[1], int(r[2]), int(r[10]))
             for r in SSA.findall(log) if r[0] == tier}
    if len(sites) != 4 or set(sites) != set(machine):
        raise AssertionError("missing actual ORC machine SSA sites")
    expected = {(ident, i) for ident, (_, _, n) in sites.items()
                for i in range(1, n + 1)}
    sources = {}
    for (t, fn, ordinal, ident, index, family, source_id,
         opcode, owner, safe) in SOURCE.findall(log):
        key = int(ident), int(index)
        if (t != tier or (owner, safe) != ("0", "0") or
            key not in expected or key in sources or
            (fn, int(ordinal)) != sites[key[0]][:2]):
            raise AssertionError("untrusted or duplicate SSA definition")
        sources[key] = (int(source_id), family, opcode)
    if set(sources) != expected:
        raise AssertionError("machine location has no SSA source")

    origins = {}
    for (t, fn, ordinal, ident, index, source_id, origin,
         owner, safe) in ORIGIN.findall(log):
        key = int(ident), int(index)
        if (t != tier or (owner, safe) != ("0", "0") or
            key not in sources or key in origins or
            int(source_id) != sources[key][0] or
            (fn, int(ordinal)) != sites[key[0]][:2]):
            raise AssertionError("invalid location origin identity")
        if index == "1":
            if origin != "unknown_invalid" or sources[key][1] != "helper_arg":
                raise AssertionError("JitCtx is not a boxed i64")
        elif (sources[key][1] not in ("i64_instruction", "i64_argument")
              or not origin.startswith(("candidate_", "unknown_",
                                              "no_tagged_string_"))
              or origin == "unknown_invalid"):
            raise AssertionError("unsound boxed i64 classification")
        origins[key] = origin
    if set(origins) != expected:
        raise AssertionError("missing SSA/machine origin")
    if any(len(machine[ident][1]) != n for ident, (_, _, n) in sites.items()):
        raise AssertionError("incorrect actual ELF StackMap location count")
    tally = Counter(v for (ident, index), v in origins.items() if index > 1)
    if sum(tally.values()) != len(expected) - 4:
        raise AssertionError("incomplete i64 provenance partition")
    return tally



def adversarial_contract(log: str, tier: str, machine: dict) -> None:
    """Real compiler output must fail closed under every tampered witness."""
    origin = ORIGIN.search(log)
    source = SOURCE.search(log)
    if not origin or not source:
        raise AssertionError("missing real compiler witness")
    raw_origin = origin.group(0)
    raw_source = source.group(0)
    ident = int(origin.group(4))
    scenarios = (
        ("missing origin", log.replace(raw_origin + "\n", "", 1), machine),
        ("duplicate origin", log.replace(
            raw_origin, raw_origin + "\n" + raw_origin, 1), machine),
        ("forged GC claim", log.replace(
            raw_origin, raw_origin.replace("gc_safe=0", "gc_safe=1"), 1), machine),
        ("forged source ID", log.replace(
            raw_origin,
            re.sub(r"source_id=\d+", "source_id=17", raw_origin), 1), machine),
        ("forged tier", log.replace(
            raw_origin, raw_origin.replace(
                "tier=" + tier, "tier=foreign"), 1), machine),
        ("missing SSA manifest", log.replace(raw_source + "\n", "", 1), machine),
        ("missing machine record", log, {
            k: v for k, v in machine.items() if k != ident
        }),
        ("corrupted machine location count", log, {
            **machine,
            ident: (machine[ident][0], machine[ident][1][1:]),
        }),
    )
    for label, forged_log, forged_machine in scenarios:
        try:
            verify(forged_log, tier, forged_machine)
        except AssertionError:
            continue
        raise AssertionError(f"accepted forged native SSA proof: {label}")


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--native", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--readobj", default="llvm-readobj")
    p.add_argument("--objdump", default="llvm-objdump")
    args = p.parse_args()
    objbase = args.out.resolve()
    objbase.mkdir(parents=True, exist_ok=True)
    native = args.native.resolve()
    project = fixture(objbase, "ssa_budget_cross_fiber_two_hats")
    flags = ("XYO_RT_YIELD_EDGE_STACKMAP_PROBE",
             "XYO_RT_YIELD_LIVE_SSA_STACKMAP",
             "XYO_RT_YIELD_SSA_FILTER",
             "XYO_RT_YIELD_SSA_TRACE_OPERANDS",
             "XYO_JIT_LAZY_MODE", "XYO_JIT_DUMP_OBJECT",
             "XYO_RT_MACHINE_STACKMAP_PROBE")
    old = {flag: os.environ.get(flag) for flag in flags}
    try:
        os.environ["XYO_RT_YIELD_EDGE_STACKMAP_PROBE"] = "0"
        os.environ["XYO_RT_YIELD_LIVE_SSA_STACKMAP"] = "0"
        expected, _ = execute(native, project, "off", None)
        for tier in ("eager", "lazy"):
            folder = objbase / tier
            folder.mkdir(exist_ok=True)
            for obj in folder.glob("*.o"):
                obj.unlink()
            os.environ.update({
                "XYO_RT_YIELD_EDGE_STACKMAP_PROBE": "1",
                "XYO_RT_YIELD_LIVE_SSA_STACKMAP": "1",
                "XYO_RT_YIELD_SSA_FILTER": "i64_no_tick",
                "XYO_RT_YIELD_SSA_TRACE_OPERANDS": "1",
                "XYO_RT_MACHINE_STACKMAP_PROBE": "0",
                "XYO_JIT_LAZY_MODE": "on" if tier == "lazy" else "off",
                "XYO_JIT_DUMP_OBJECT": str((folder / "ssa").resolve()),
            })
            actual, trace = execute(
                native, project, "eager", None,
                llvm_ssa_liveness=True, detached_llvm_ssa=True,
                lazy_typed_ssa=True, require_generated_ssa_roots=True,
                soft_gc_budget_mb=1,
            )
            if actual != expected:
                raise AssertionError("Scratch parity regression")
            count, _, _ = validate_machine(
                folder, trace, tier, args.readobj, args.objdump)
            if count != 4:
                raise AssertionError("missing yield sites")
            ids = {int(r[4]) for r in SSA.findall(trace) if r[0] == tier}
            machine = {}
            for obj in folder.glob("*.o"):
                dump = subprocess.run(
                    [args.readobj, "--stackmap", str(obj)],
                    text=True, capture_output=True, check=True,
                ).stdout
                for ident, record in parse_machine_locations(dump, ids).items():
                    if ident in machine:
                        raise AssertionError("duplicate ORC StackMap ID")
                    machine[ident] = record
            tally = verify(trace, tier, machine)
            adversarial_contract(trace, tier, machine)
            # Record only instruction opcodes and source identifiers, never
            # raw heap pointers or live Scratch variable contents.
            evidence = ORIGIN.findall(trace)
            origins = {(int(row[3]), int(row[4])): row[6]
                       for row in evidence if row[0] == tier}
            opcodes = Counter(
                (origins[(int(row[3]), int(row[4]))], row[7])
                for row in SOURCE.findall(trace)
                if row[0] == tier and int(row[4]) > 1
            )
            (folder / "opcode-breakdown.txt").write_text(
                "\n".join(f"{category} {opcode}: {n}"
                          for (category, opcode), n
                          in sorted(opcodes.items())) + "\n",
                encoding="utf-8")
            candidates = sum(n for name, n in tally.items()
                             if name.startswith("candidate_"))
            no_string_tag = tally["no_tagged_string_zext"]
            unknown = sum(n for name, n in tally.items()
                          if name.startswith("unknown_"))
            if candidates + no_string_tag + unknown != sum(tally.values()):
                raise AssertionError("provenance labels fail to partition i64")
            print(f"PASS RT-001 {tier}: {sum(tally.values())} i64 homes, "
                  f"{candidates} boxed-origin candidates, "
                  f"{no_string_tag} no-NaN-box-string-tag zext, "
                  f"{unknown} unknown; "
                  f"categories={dict(sorted(tally.items()))}; "
                  "ownership_proven=0 gc_safe=0; "
                  "8 adversarial witnesses rejected", flush=True)
    finally:
        for flag, value in old.items():
            if value is None:
                os.environ.pop(flag, None)
            else:
                os.environ[flag] = value


if __name__ == "__main__":
    main()
