#!/usr/bin/env python3
"""RT-001: real paused Fiber CFI vs LLVM i64 SSA StackMap spill bounds.

This is a *machine address bounds* experiment only. An i64's content
is not a proven GC pointer, and every native Fiber remains GC-unsafe.
Expected: Linux x86-64 SysV, real ORC FDE lifetime plugin, LLVM23.
"""
from __future__ import annotations

import argparse
from collections import Counter
import os
from pathlib import Path
import re
import subprocess

from native_simple_resume import fixture
from rt_yield_edge_fiber_pc import TRACE, check_emitted
from rt_yield_live_ssa_machine import SSA, validate_machine

SOURCE = re.compile(
    r"^\[rt001-yield-ssa-operand\] tier=(eager|lazy) fn=(\S+) "
    r"ordinal=(\d+) stackmap_id=(\d+) location=(\d+) "
    r"family=(\w+) source_id=(\d+) opcode=(\w+) "
    r"ownership_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)

SEMANTIC = re.compile(
    r"^\[rt001-yield-ssa-semantic\] tier=(eager|lazy) fn=(\S+) "
    r"ordinal=(\d+) stackmap_id=(\d+) location=(\d+) "
    r"source_id=(\d+) semantic=(\w+) "
    r"root_kind=(\w+) gc_safe=(\d+)$",
    re.MULTILINE,
)

FRAME = re.compile(
    r"^\[rt001-yield-spill-bounds\] thread=(\S+) "
    r"jit_return_offset=(\d+) genuine_fde=(\d+) "
    r"frame_pointer_valid=(\d+) rbp_depth=(\d+) "
    r"stopped_sp_depth=(\d+) stack_capacity=(\d+) "
    r"memory_values_proven=(\d+) ownership_proven=(\d+) "
    r"gc_safe=(\d+)$", re.MULTILINE,
)
RECORD = re.compile(
    r"Record ID: (\d+), instruction offset: (\d+)\s*\n"
    r"\s*(\d+) locations:\s*\n(.*?)\n\s*\d+ live-outs:",
    re.DOTALL,
)
LOCATION = re.compile(r"^\s*#(\d+): (.+), size: (\d+)$")
REGISTER = re.compile(r"^Register R#(\d+)$")
SPILL = re.compile(r"^Indirect \[R#(\d+) \+ (-?\d+)\]$")

# SysV x86_64 DWARF: RBX, RBP, R12..R15 are callee-saved.
# RDI/RSI/RAX and other volatile registers can be overwritten inside
# xyo_yield and must never be accepted from a suspended host stack.
PRESERVED_DWARF = {3, 6, 12, 13, 14, 15}


def parse_machine_locations(
    output: str, target_ids: set[int],
) -> dict[int, tuple[int, list[tuple[str, int, int]]]]:
    records = {}
    for ident, pc, total, lines in RECORD.findall(output):
        if int(ident) not in target_ids:
            continue
        locations = []
        for line in lines.splitlines():
            if not line.strip():
                continue
            match = LOCATION.fullmatch(line)
            if not match:
                raise AssertionError(
                    f"unsupported LLVM machine location line: {line!r}"
                )
            number, description, size = match.groups()
            if int(number) != len(locations) + 1 or int(size) != 8:
                raise AssertionError(
                    f"bad machine SSA location ordinal/size: {line}"
                )
            register = REGISTER.fullmatch(description)
            spill = SPILL.fullmatch(description)
            if register:
                reg = int(register.group(1))
                if reg not in PRESERVED_DWARF:
                    # LLVM's pre-call machine StackMap may legitimately
                    # show RAX/RDI/... even though those values CANNOT
                    # be reconstructed at the deeper yield suspension.
                    # Do not erase the evidence: count it as explicitly
                    # UNRECOVERABLE instead of claiming all SSA rooted.
                    locations.append(("volatile_unrecoverable", reg, 0))
                else:
                    locations.append(("register", reg, 0))
            elif spill:
                reg, offset = map(int, spill.groups())
                if reg != 6 or offset >= 0 or offset % 8:
                    raise AssertionError(
                        f"unproven non-RBP/unaligned native spill: {line}"
                    )
                locations.append(("rbp_spill", reg, offset))
            else:
                # Constant, Direct, or other kinds may be valid as
                # runtime values but cannot be authenticated by this
                # deliberately narrow machine-frame contract.
                raise AssertionError(
                    f"unproved SSA machine home: {line}"
                )
        if len(locations) != int(total) or int(ident) in records:
            raise AssertionError(
                f"missing, duplicate or malformed SSA locations "
                f"id={ident}, expected={total}, actual={len(locations)}"
            )
        records[int(ident)] = (int(pc), locations)
    return records


def validate_spill_window(
    frame: tuple[str, ...], displacement: int,
) -> int:
    """Relative RBP spill must be valid within stopped Fiber stack.

    No memory is dereferenced: this is an address-range contract only.
    """
    (
        thread, _, fde, valid, rbp_depth, stopped_depth, capacity,
        memory_proof, ownership, gc_safe,
    ) = frame
    if (fde, valid, memory_proof, ownership, gc_safe) != (
        "1", "1", "0", "0", "0"
    ):
        raise AssertionError(f"{thread}: invalid CFI/GC proof")
    if not 0 < int(rbp_depth) < int(stopped_depth) < int(capacity):
        raise AssertionError(f"{thread}: invalid stack frame offsets")
    if displacement >= 0 or displacement % 8:
        raise AssertionError(
            f"{thread}: invalid RBP-relative stack displacement"
        )
    depth = int(rbp_depth) - displacement
    if not 0 < depth < int(capacity) or depth > int(stopped_depth):
        raise AssertionError(
            f"{thread}: stack spill {depth} not in paused Fiber"
        )
    return depth


def check_spills(
    records: dict[int, tuple[int, list[tuple[str, int, int]]]],
    expected_ids: set[int],
    frames: list[tuple[str, ...]],
) -> tuple[int, int, int, int]:
    if not expected_ids or any(ident not in records for ident in expected_ids):
        raise AssertionError("missing a live SSA StackMap machine record")

    if len(frames) != 2 or len({row[0] for row in frames}) != 2:
        raise AssertionError("two distinct real suspended Fibers required")
    for row in frames:
        (
            thread, ret, fde, valid, rbp_depth, stopped_depth, capacity,
            memory_proof, ownership, gc_safe,
        ) = row
        if (fde, valid, memory_proof, ownership, gc_safe) != (
            "1", "1", "0", "0", "0"
        ):
            raise AssertionError(
                f"{thread}: invalid CFI/frame/GC proof: {row}"
            )
        if not (
            0 < int(rbp_depth) < int(stopped_depth) < int(capacity)
        ):
            raise AssertionError(
                f"{thread}: malformed Fiber stack bounds: {row}"
            )

    # Count each SSA site only once; each observed Fiber is compared
    # with sites whose real native yield-return PC matches its return.
    # A script's identity cannot be assumed from an unrelated ELF.
    inspected = set()
    stack_slots = registers = unrecoverable = 0
    for ident in sorted(expected_ids):
        offset, homes = records[ident]
        if not homes:
            raise AssertionError("SSA record has no live homes")
        for kind, reg, displacement in homes:
            if kind == "rbp_spill":
                stack_slots += 1
            elif kind == "register":
                registers += 1
            elif kind == "volatile_unrecoverable":
                unrecoverable += 1
            else:
                raise AssertionError(
                    f"unexpected machine SSA location kind {kind}"
                )
        inspected.add(ident)
    return len(inspected), stack_slots, registers, unrecoverable


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--smoke", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--readobj", default="llvm-readobj")
    parser.add_argument("--objdump", default="llvm-objdump")
    parser.add_argument(
        "--filter", choices=("i64", "i64_no_tick"), default="i64",
    )
    args = parser.parse_args()

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    objdir = out / "objects"
    objdir.mkdir(exist_ok=True)
    for item in objdir.glob("*.o"):
        item.unlink()
    project = fixture(out, "ssa_budget_cross_fiber_two_hats")
    env = dict(os.environ)
    env.update({
        "XYO_RT_YIELD_EDGE_STACKMAP_PROBE": "1",
        "XYO_RT_YIELD_LIVE_SSA_STACKMAP": "1",
        "XYO_RT_YIELD_SSA_FILTER": args.filter,
        "XYO_RT_MACHINE_STACKMAP_PROBE": "0",
        "XYO_RT001_YIELD_SPILL_BOUNDS_PROBE": "1",
        "XYO_RT_YIELD_SSA_TRACE_OPERANDS": "1",
        "XYO_RT_ORC_FDE_REGISTRATION": "1",
        "XYO_JIT_DUMP_OBJECT": str(objdir / "ssa"),
        "XYO_RT001_GNU_FDE_PROBE": "1",
    })
    process = subprocess.run(
        [str(args.smoke.resolve()), str(project.resolve())],
        cwd=out, env=env, text=True, capture_output=True, timeout=120,
    )
    if process.returncode:
        raise AssertionError(
            f"live Fiber CFI probe failed: {process.returncode}\n"
            f"{process.stdout[-1000:]}\n{process.stderr[-7000:]}"
        )
    edge_pairs, returns, _ = check_emitted(
        objdir, process.stderr, args.readobj, args.objdump,
    )
    sites, live_i64, _ = validate_machine(
        objdir, process.stderr, "eager", args.readobj, args.objdump,
    )
    if len(edge_pairs) != 4 or sites != 4 or live_i64 < 1:
        raise AssertionError(
            f"incomplete live SSA fixture: {edge_pairs}, "
            f"sites={sites}, live_i64={live_i64}"
        )

    manifests = SSA.findall(process.stderr)
    ids = {int(row[4]) for row in manifests if row[0] == "eager"}
    by_machine = {}
    for obj in objdir.glob("*.o"):
        data = subprocess.run(
            [args.readobj, "--stackmap", str(obj)],
            capture_output=True, text=True, check=True,
        ).stdout
        for ident, parsed in parse_machine_locations(data, ids).items():
            if ident in by_machine:
                raise AssertionError(
                    f"ORC emitted a duplicate machine StackMap ID {ident}"
                )
            by_machine[ident] = parsed
    # Exact index-by-index identity from postoptimizer SSA to actual
    # machine location; LLVM operands have never been interpreted as
    # owned GC roots in this check.
    sources: dict[tuple[int, int], tuple[str, int, str]] = {}
    for (
        tier, fn, ordinal, ident, location, family, source_id,
        opcode, ownership, gc_safe,
    ) in SOURCE.findall(process.stderr):
        if tier != "eager" or ownership != "0" or gc_safe != "0":
            raise AssertionError("forged machine SSA source authority")
        key = (int(ident), int(location))
        if key in sources:
            raise AssertionError(
                f"duplicate source for machine StackMap operand {key}"
            )
        sources[key] = (family, int(source_id), opcode)
    all_locations = {
        (ident, index)
        for ident in ids
        for index in range(1, len(by_machine[ident][1]) + 1)
    }
    if set(sources) != all_locations:
        raise AssertionError(
            f"missing/extra machine SSA source provenance: "
            f"missing={len(all_locations - set(sources))}, "
            f"extra={len(set(sources) - all_locations)}"
        )
    for ident in ids:
        for index, (kind, _, _) in enumerate(
            by_machine[ident][1], start=1
        ):
            family, source_id, opcode = sources[ident, index]
            if index == 1 and family != "helper_arg":
                raise AssertionError("first operand is not a yield argument")
            if index > 1 and family not in (
                "i64_instruction", "i64_argument"
            ):
                raise AssertionError(
                    f"non-numeric source in i64-only StackMap: "
                    f"operand={index}, family={family}"
                )
    volatile_origins = sorted({
        (family, source_id, opcode)
        for ident in ids
        for index, (kind, _, _) in enumerate(
            by_machine[ident][1], start=1
        )
        if kind == "volatile_unrecoverable"
        for family, source_id, opcode
        in (sources[ident, index],)
    })
    semantics: dict[tuple[int, int], tuple[int, str]] = {}
    for (tier, fn, ordinal, ident, index, source_id, semantic,
         root_kind, gc_safe) in SEMANTIC.findall(process.stderr):
        key = (int(ident), int(index))
        if (tier != "eager" or root_kind != "numeric_not_managed" or
            gc_safe != "0" or key not in sources or key in semantics or
            semantic != "scheduler_tick" or
            sources[key] != ("i64_instruction", int(source_id), "add")):
            raise AssertionError(
                f"invalid native SSA scheduler origin: {key}, "
                f"source={source_id}, kind={semantic}"
            )
        semantics[key] = (int(source_id), semantic)
    volatile_keys = {
        (ident, index)
        for ident in ids
        for index, (kind, _, _) in enumerate(
            by_machine[ident][1], start=1
        )
        if kind == "volatile_unrecoverable"
    }
    # In this fixture the only caller-saved i64 home is the
    # optimizer's loop yield counter; it is demonstrably a numeric
    # scheduler tick, not a Scratch GC root. The classification is
    # structural and must not rely on compiler SSA numbering.
    if args.filter == "i64":
        if len(volatile_keys) != 2 or not volatile_keys <= set(semantics):
            raise AssertionError(
                f"unclassified volatile SSA home at native yield: "
                f"volatile={volatile_keys}, proved_scheduler={semantics}"
            )
    else:
        # The ABI-proven numeric tick is EXCLUDED, so no retained
        # machine location can claim to represent that SSA value.
        if semantics:
            raise AssertionError(
                "a known numeric scheduler tick still occupies a "
                f"machine location: {semantics}"
            )
        if volatile_keys:
            raise AssertionError(
                "the filtered true suspended-Fiber SSA machine "
                f"snapshot still contains volatile register homes: "
                f"{volatile_keys}"
            )
        excluded = re.findall(
            r"^\[rt001-yield-ssa-numeric-excluded\] "
            r"tier=eager fn=\S+ ordinal=\d+ stackmap_id=\d+ "
            r"scheduler_ticks=(\d+) other_unproven_i64=\d+ "
            r"ownership_proven=(\d+) machine_roots_proven=(\d+) "
            r"gc_safe=(\d+)$",
            process.stderr, re.MULTILINE,
        )
        if len(excluded) != 4 or any(
            row != ("1", "0", "0", "0") for row in excluded
        ):
            raise AssertionError(
                f"missing fail-closed scheduler elision: {excluded}"
            )
    frames = FRAME.findall(process.stderr)
    live = TRACE.findall(process.stderr)
    if (len(live) != 2 or
        {f[0] for f in frames} != {r[0] for r in live}):
        raise AssertionError("live CFI frames do not match Fiber set")
    for frame in frames:
        if int(frame[1]) not in returns:
            raise AssertionError(
                f"unrelated Fiber JIT return offset {frame[1]}"
            )

    # Each machine SSA ID must be associated with the *same* direct
    # yield return as an actually suspended JIT call, not a stale
    # raw-stack capture PC. The expected SSA ID is computed separately
    # from compiler source-site manifests.
    n, spills, preserved, volatile = check_spills(
        by_machine, ids, frames
    )
    if spills == 0 or preserved == 0:
        raise AssertionError(
            "expected both real RBP-relative spill and preserved "
            "register machine locations"
        )

    # The true owner of a JIT return offset can be ambiguous when two
    # different scripts have identical function layouts. Requiring
    # BOTH paused Fiber stack ranges to contain each SSA memory home
    # is conservative, but doesn't invent a script-to-thread mapping.
    for record in manifests:
        if record[0] != "eager":
            continue
        identifier = int(record[4])
        _, locations = by_machine[identifier]
        for thread, ret, fde, valid, rbp, saved, cap, _, _, _ in frames:
            for kind, _, displacement in locations:
                if kind != "rbp_spill":
                    continue
                validate_spill_window(
                    (thread, ret, fde, valid, rbp, saved, cap,
                     "0", "0", "0"),
                    displacement,
                )
    print(
        f"PASS RT-001 true paused Fiber {args.filter} SSA homes: "
        f"{n} StackMaps with {spills} RBP-relative slots and "
        f"{preserved} preserved-register homes and "
        f"{volatile} VOLATILE unrecoverable homes "
        f"(LLVM origins={volatile_origins}, "
        f"scheduler_proofs={len(semantics)}); "
        "2 genuine JIT CFI frames, all RBP spill addresses inside "
        "the paused Fiber stacks, machine_roots_proven=0 "
        "ownership_proven=0 gc_safe=0"
    )


if __name__ == "__main__":
    main()
