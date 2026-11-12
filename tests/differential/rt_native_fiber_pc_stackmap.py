#!/usr/bin/env python3
"""RT-001: compare suspended REAL native Fiber saved-PC with ORC StackMaps.

The Fiber's saved RIP is xyo_fiber_switch's host return, not a JIT
safepoint. A conservative bounded stack search may find JIT return
address CANDIDATES near a StackMap site. No DWARF unwind => no GC root
permission; proximity alone is never a certified machine frame.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess

from native_simple_resume import fixture

FIBER = re.compile(
    r"^\[rt001-fiber-pc\] thread=(\S+) "
    r"saved_pc_class=(\S+) jit_entry_class=(\S+) "
    r"stackmap_pc_matched=(\d+) last_source_site=(\d+) "
    r"procedure_count=(\d+) potential_stack_returns=(\d+) "
    r"first_return_offset=(\d+) machine_frame_verified=(\d+) "
    r"gc_safe=(\d+)$",
    re.MULTILINE,
)
RECORD = re.compile(
    r"Record ID: (\d+), instruction offset: (\d+)\s+"
    r"(\d+) locations:"
)

SYM = re.compile(
    r"^([0-9a-fA-F]+)\s+g\s+F\s+\.text\s+"
    r"[0-9a-fA-F]+\s+xyo_script_\d+_p0$",
    re.MULTILINE,
)
RELOC = re.compile(
    r"^([0-9a-fA-F]+)\s+R_X86_64_PLT32\s+"
    r"(xyo_rt_capture_llvm_ssa|xyo_yield)-0x4$",
    re.MULTILINE,
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--smoke", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--readobj", type=Path, default=Path("llvm-readobj"))
    parser.add_argument("--objdump", type=Path, default=Path("llvm-objdump"))
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    project = fixture(out, "ssa_budget_cross_fiber_two_hats")
    objdir = out / "objects"
    objdir.mkdir(exist_ok=True)
    for old in objdir.glob("*.o"):
        old.unlink()

    env = dict(os.environ)
    env["XYO_JIT_DUMP_OBJECT"] = str(objdir / "obj")
    process = subprocess.run(
        [str(args.smoke.resolve()), str(project.resolve())],
        cwd=out, env=env, text=True, capture_output=True, timeout=120,
    )
    if process.returncode or (
        "PASS RT-001 saved Fiber RIPs" not in process.stdout
    ):
        raise AssertionError(
            f"native Fiber test failed {process.returncode}\n"
            f"{process.stdout}\n{process.stderr[-5000:]}"
        )

    fibers = FIBER.findall(process.stderr)
    if len(fibers) != 2 or len({row[0] for row in fibers}) != 2:
        raise AssertionError(f"expected two distinct paused fibers: {fibers}")
    for (
        thread, saved_pc, jit, matched, site, procedures,
        candidates, first, verified, gc,
    ) in fibers:
        if (
            saved_pc != "host_native_yield" or
            jit != "orc_generated" or matched != "0" or
            site != "1" or int(procedures) < 1 or
            int(candidates) < 1 or int(first) <= 0 or
            verified != "0" or gc != "0"
        ):
            raise AssertionError(
                f"{thread}: invalid/falsely certified machine PC"
            )

    objects = sorted(objdir.glob("*.o"))
    if len(objects) != 2:
        raise AssertionError(f"expected two actual ORC ELFs: {objects}")
    site1_offsets = []
    capture_return_offsets = []
    yield_return_offsets = []
    for obj in objects:
        result = subprocess.run(
            [str(args.readobj), "--stackmap", str(obj)],
            capture_output=True, text=True, check=True,
        )
        records = RECORD.findall(result.stdout)
        if len(records) != 2:
            raise AssertionError(f"{obj.name}: no two actual StackMaps")
        site1_offsets.append(int(records[0][1]))
        sym = subprocess.run(
            [str(args.objdump), "--syms", str(obj)],
            capture_output=True, text=True, check=True,
        )
        function_starts = SYM.findall(sym.stdout)
        if len(function_starts) != 1:
            raise AssertionError(
                f"{obj.name}: expected exactly one native procedure "
                f"entry, got {function_starts}"
            )
        function_start = int(function_starts[0], 16)
        relocs = subprocess.run(
            [str(args.objdump), "--reloc", str(obj)],
            capture_output=True, text=True, check=True,
        )
        captures, yields = [], []
        for relocation, symbol in RELOC.findall(relocs.stdout):
            # On x86-64 a PLT32 relocation points at the 4-byte
            # displacement after the call opcode. Thus +4 gives
            # the EXACT return-PC at the end of that instruction.
            return_offset = int(relocation, 16) + 4 - function_start
            if return_offset < 0:
                raise AssertionError("foreign/underflowing relocation")
            (captures if symbol == "xyo_rt_capture_llvm_ssa"
             else yields).append(return_offset)
        if not captures or not yields:
            raise AssertionError(
                f"{obj.name}: missing compiler call relocations"
            )
        capture_return_offsets.append(min(captures))
        yield_return_offsets.append(min(yields))
    # Both scripts are the same procedure body. This is a *proximity
    # observation*, not a claim that the candidate is the call-frame
    # return site or that a proper DWARF unwind succeeded.
    candidate_offsets = [int(row[7]) for row in fibers]
    distance = [
        abs(candidate_offsets[i] - site1_offsets[i])
        for i in range(2)
    ]
    if any(value > 128 for value in distance):
        raise AssertionError(
            "no nearby JIT return-address candidate; "
            f"offsets={candidate_offsets} stackmaps={site1_offsets}"
        )
    # This is the critical counterexample: the raw stack word
    # points EXACTLY to the return of an ALREADY COMPLETED
    # xyo_rt_capture_llvm_ssa call. A range search for return PCs
    # would falsely count a stale, not active, frame as a GC root.
    if (sorted(candidate_offsets) !=
            sorted(capture_return_offsets) or
        any(not (
            candidate_offsets[i] < site1_offsets[i] <
            yield_return_offsets[i]
        ) for i in range(2))):
        raise AssertionError(
            "stale capture-return counterexample changed: "
            f"candidate={candidate_offsets}, "
            f"capture_returns={capture_return_offsets}, "
            f"stackmaps={site1_offsets}, yields={yield_return_offsets}"
        )
    print(
        "PASS RT-001 real suspended Fiber PC classification: "
        "2/2 saved RIPs in host switch code; "
        f"stack words={candidate_offsets} match STALE "
        f"capture-call return PCs={capture_return_offsets}; "
        f"StackMaps={site1_offsets}, "
        f"yield-call returns={yield_return_offsets}; "
        "NO verified machine frame/GC permission"
    )


if __name__ == "__main__":
    main()
