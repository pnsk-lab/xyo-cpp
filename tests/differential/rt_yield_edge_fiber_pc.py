#!/usr/bin/env python3
"""RT-001: locate LLVM post-O3 StackMap *at the native yield call edge*.

We inspect emitted ORC ELF relocations and a genuinely suspended Fiber.
Matching a yield-call edge is NOT a GC root proof: the native Fiber
actually stops inside a host helper, and boxed i64 roots are incomplete.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import os
from pathlib import Path
import re
import subprocess

from native_simple_resume import fixture
from rt_native_fiber_cfi_unwind import TRACE
from rt_native_fiber_pc_stackmap import RECORD, SYM

EDGE = re.compile(
    r"^\[rt001-yield-edge-stackmap\] tier=(eager|lazy) "
    r"fn=(\S+) ordinal=(\d+) helper=(xyo_yield|xyo_yield_fast) "
    r"stackmap_id=(\d+) locations=(\d+) emitted=(\d+) "
    r"machine_roots_proven=(\d+) gc_safe=(\d+)$", re.MULTILINE,
)
TOTAL = re.compile(
    r"^\[rt001-yield-edge-stackmap-total\] tier=(eager|lazy) "
    r"sites=(\d+) emitted=(\d+) refused=(\d+) "
    r"machine_roots_proven=(\d+) gc_safe=(\d+)$", re.MULTILINE,
)
YIELD_RELOC = re.compile(
    r"^([0-9a-fA-F]+)\s+R_X86_64_PLT32\s+"
    r"(xyo_yield|xyo_yield_fast)-0x4$", re.MULTILINE,
)
# The first operand is the unique yield helper argument, not a
# scratch Value root. LLVM ReadObj reports its EXACT DWARF register.
# A callee-saved register can be compared with a suspended Fiber's
# authenticated CFI frame, but cannot certify all machine GC roots.
FIRST_REGISTER = re.compile(
    r"Record ID: (\d+), instruction offset: (\d+)\s+"
    r"(\d+) locations:\s+#1: Register R#(\d+), size: (\d+)",
)

YIELD_REGISTERS = re.compile(
    r"^\[rt001-yield-register\] thread=(\S+) "
    r"jit_return_offset=(\d+) ctx_available=(\d+) "
    r"dwarf13_valid=(\d+) dwarf13_ctx_match=(\d+) "
    r"dwarf14_valid=(\d+) dwarf14_ctx_match=(\d+) "
    r"dwarf3_valid=(\d+) dwarf3_ctx_match=(\d+) "
    r"dwarf12_valid=(\d+) dwarf12_ctx_match=(\d+) "
    r"genuine_fde=(\d+) machine_roots_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)

PAUSED = re.compile(
    r"^\[rt001-fiber-pc\] thread=(\S+) "
    r"saved_pc_class=host_native_yield "
    r"jit_entry_class=orc_generated "
    r"stackmap_pc_matched=0\s+last_source_site=(\d+) ",
    re.MULTILINE,
)


def check_emitted(
    out: Path, log: str, readobj: str, objdump: str,
    tier: str = "eager",
) -> tuple[list[tuple[int, int]], list[int], list[tuple[int, int]]]:
    expected_tier = tier
    records = EDGE.findall(log)
    if not records:
        raise AssertionError("optimized LLVM emitted no yield-edge sites")
    by_name: dict[str, dict[int, tuple[str, int]]] = defaultdict(dict)
    for (
        tier, fn, ordinal, helper, ident, count, emitted,
        roots, gc_safe,
    ) in records:
        if (tier != expected_tier or emitted != "1" or
                count != "1" or roots != "0" or gc_safe != "0"):
            raise AssertionError(f"unsafe/malformed yield edge: {fn}:{ordinal}")
        if int(ident) in by_name[fn]:
            raise AssertionError("duplicate yield edge site ID")
        by_name[fn][int(ident)] = helper, int(ordinal)
    totals = TOTAL.findall(log)
    # Lazy ORC materializes a number of support modules with no
    # xyo_script_* yield calls. Their zero-site diagnostics are valid,
    # but must never stand in for an expected nonempty source module.
    nonempty = [row for row in totals if int(row[1]) > 0]
    if len(nonempty) != len(by_name) or any(
        found_tier != expected_tier or roots != "0" or
        safe != "0" or refused != "0" or sites != emitted
        for found_tier, sites, emitted, refused, roots, safe in totals
    ):
        raise AssertionError(f"invalid fail-closed site totals: {totals}")

    objects = sorted(out.glob("*.o"))
    if not objects:
        raise AssertionError("native ORC produced no ELF objects")
    pairs = []
    call_returns = []
    register_witnesses: list[tuple[int, int]] = []
    covered = set()
    for obj in objects:
        sym = subprocess.run(
            [objdump, "--syms", str(obj)],
            text=True, capture_output=True, check=True,
        )
        names = [
            (name, int(start, 16))
            for start, name in re.findall(
                r"^([0-9a-fA-F]+)\s+g\s+F\s+\.text\s+"
                r"[0-9a-fA-F]+\s+(xyo_script_\S+)$",
                sym.stdout, re.MULTILINE,
            )
        ]
        # An ORC object can also define the trampoline xyo_script_N
        # next to its procedure xyo_script_N_p0. Only the procedure
        # has matching post-optimizer yield-edge StackMap IDs.
        names = [(name, start) for name, start in names
                 if name in by_name]
        if not names:
            # ORC Lazy also emits non-script engine stubs/trampolines.
            # They are not an excuse for missing the two actual JIT
            # procedure objects, checked via the site-ID bijection.
            continue
        if len(names) != 1:
            raise AssertionError(
                f"expected one probed JIT procedure in {obj}: {names}"
            )
        name, start = names[0]

        relocations = subprocess.run(
            [objdump, "--reloc", str(obj)],
            text=True, capture_output=True, check=True,
        )
        calls = sorted(
            (
                int(offset, 16) - 1 - start,
                int(offset, 16) + 4 - start,
                helper,
            )
            for offset, helper in YIELD_RELOC.findall(relocations.stdout)
        )
        disassembly = subprocess.run(
            [objdump, "--disassemble", "--no-show-raw-insn", str(obj)],
            text=True, capture_output=True, check=True,
        )
        instructions = sorted(
            (int(offset, 16)-start, mnemonic)
            for offset, mnemonic in re.findall(
                r"^\s*([0-9a-fA-F]+):\s+([a-z][a-z0-9_.]*)\b",
                disassembly.stdout, re.MULTILINE,
            )
            if int(offset, 16) >= start
        )
        by_offset = {pc: word for pc, word in instructions}
        read = subprocess.run(
            [readobj, "--stackmap", str(obj)],
            text=True, capture_output=True, check=True,
        )
        first_registers = {
            int(ident): (int(offset), int(count), int(dwarf), int(size))
            for ident, offset, count, dwarf, size
            in FIRST_REGISTER.findall(read.stdout)
            if int(ident) in by_name[name]
        }
        mapped = sorted(
            (
                int(offset),
                int(ident),
                int(locations),
            )
            for ident, offset, locations in RECORD.findall(read.stdout)
            if int(ident) in by_name[name]
        )
        if len(mapped) != len(calls) or len(mapped) != len(by_name[name]):
            raise AssertionError(
                f"{name}: site/relocation count mismatch "
                f"mapped={mapped} calls={calls} compiler={by_name[name]}"
            )

        for (map_pc, ident, locations), (call_pc, return_pc, helper) in zip(
            mapped, calls
        ):
            if locations != 1 or (
                by_name[name][ident][0] != helper
            ):
                raise AssertionError(
                    f"{name}: ELF opcode or StackMap location mismatch"
                )
            # The LLVM StackMap PC can be at the argument register
            # move immediately before the call, not necessarily the
            # CALL itself. Admit only no-op/argument setup instruction
            # sequences with no branches, loads or other calls.
            intermediate = [
                (pc, mnemonic) for pc, mnemonic in instructions
                if map_pc <= pc < call_pc
            ]
            if (map_pc > call_pc or call_pc - map_pc > 16 or
                any(not word.startswith(("mov", "lea", "nop"))
                    for _, word in intermediate) or
                (intermediate and intermediate[0][0] != map_pc) or
                not (call_pc == map_pc or
                     any(pc == call_pc for pc, _ in instructions))):
                raise AssertionError(
                    f"{name}: unverifiable yield edge "
                    f"map={map_pc} call={call_pc} between={intermediate}"
                )
            if return_pc - call_pc != 5:
                raise AssertionError(
                    f"{name}: not a direct x86-64 near call"
                )
            if ident not in first_registers:
                raise AssertionError(
                    f"{name}: no machine register for yield argument "
                    f"at StackMap record {ident}"
                )
            witness = first_registers[ident]
            if witness[:2] != (map_pc, locations) or witness[3] != 8:
                raise AssertionError(
                    f"{name}: wrong machine register width/record "
                    f"at {ident}: {witness}"
                )
            covered.add(ident)
            pairs.append((map_pc, return_pc))
            call_returns.append(return_pc)
            register_witnesses.append((return_pc, witness[2]))

    if len(covered) != len(records):
        raise AssertionError("compiler/ORC site IDs not bijective")
    return pairs, call_returns, register_witnesses


def validate_register_witnesses(
    registers: list[tuple[str, ...]],
    live: list[tuple[str, ...]],
    register_witnesses: list[tuple[int, int]],
) -> None:
    """Fail closed unless both real CFI frames hold exact JitCtx homes.

    This can validate a single ABI pointer; it never grants GC rights.
    """
    if len(registers) != 2 or (
        {row[0] for row in registers} != {row[0] for row in live}
    ):
        raise AssertionError(
            f"missing two authentic JIT-frame register reports: "
            f"{registers}"
        )
    by_return: dict[int, set[int]] = {}
    for return_offset, dwarf_register in register_witnesses:
        by_return.setdefault(return_offset, set()).add(dwarf_register)
    # A single runtime return offset is ambiguous if different
    # compiled scripts associate it with different home registers.
    for row in registers:
        offset = int(row[1])
        candidates = by_return.get(offset, set())
        if len(candidates) != 1:
            raise AssertionError(
                f"ambiguous/missing StackMap register at "
                f"JIT return {offset}: {candidates}"
            )
        reg = next(iter(candidates))
        # CFI can establish callee-saved GPR homes only in this
        # diagnostic. Volatile GPRs are deliberately refused:
        # their values may have changed inside xyo_yield.
        fields = {13: (3, 4), 14: (5, 6),
                  3: (7, 8), 12: (9, 10)}
        if reg not in fields:
            raise AssertionError(
                f"unproved DWARF register R#{reg} at yield boundary"
            )
        valid, same_ctx = fields[reg]
        if (row[2] != "1" or row[valid] != "1" or
            row[same_ctx] != "1" or row[11:] != ("1", "0", "0")):
            raise AssertionError(
                f"yield-edge register does not equal Thread::jitCtx: "
                f"R#{reg} {row}"
            )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--smoke", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--readobj", default="llvm-readobj")
    parser.add_argument("--objdump", default="llvm-objdump")
    parser.add_argument(
        "--require-fde", action="store_true",
        help="Use the opt-in ORC plugin and require genuine DWARF CFI",
    )
    parser.add_argument(
        "--require-register", action="store_true",
        help="Verify StackMap's argument register against unwound "
             "JIT CFI register contents and Thread::jitCtx",
    )
    args = parser.parse_args()
    if args.require_register and not args.require_fde:
        parser.error("--require-register requires --require-fde")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    project = fixture(out, "ssa_budget_cross_fiber_two_hats")
    objdir = out / "objects"
    objdir.mkdir(exist_ok=True)
    for obj in objdir.glob("*.o"):
        obj.unlink()
    env = dict(os.environ)
    env["XYO_RT_YIELD_EDGE_STACKMAP_PROBE"] = "1"
    env["XYO_JIT_DUMP_OBJECT"] = str(objdir / "obj")
    # This is the C++ saved-FiberRegs harness. Use GNU real-FDE
    # ownership verification where available, not raw stack scan.
    env["XYO_RT001_GNU_FDE_PROBE"] = "1"
    env["XYO_RT_ORC_FDE_REGISTRATION"] = (
        "1" if args.require_fde else "0"
    )
    env["XYO_RT001_YIELD_REGISTER_PROBE"] = (
        "1" if args.require_register else "0"
    )
    process = subprocess.run(
        [str(args.smoke.resolve()), str(project.resolve())],
        cwd=out, env=env, text=True, capture_output=True, timeout=120,
    )
    if process.returncode:
        raise AssertionError(
            f"native Fiber failed {process.returncode}\n"
            f"{process.stdout}\n{process.stderr[-7000:]}"
        )
    pairs, call_returns, register_witnesses = check_emitted(
        objdir, process.stderr, args.readobj, args.objdump
    )
    live = TRACE.findall(process.stderr)
    if len(live) != 2 or len({record[0] for record in live}) != 2:
        raise AssertionError(f"no two paused real native Fibers: {live}")
    if args.require_fde:
        for record in live:
            # A real FDE covers the actual function start and yield
            # return PC. The old synthetic 1-byte proc range is NOT
            # accepted, and frame authentication is separate from
            # StackMap/boxed Value GC root completeness.
            if (record[1] != "0" or int(record[2]) < 3 or
                    record[3] != "1" or record[4] != "1" or
                    record[5] != "0" or record[7] != "0" or
                    int(record[8]) <= int(record[6]) or
                    record[9] != "1" or record[10:] != ("0", "0")):
                raise AssertionError(
                    f"not a real CFI-authenticated JIT frame: {record}"
                )
        if process.stderr.count(
            "[rt001-orc-fde] phase=emit registered=2 gc_safe=0"
        ) != 2 or (
            "[rt001-orc-fde] phase=remove canceled=4 gc_safe=0"
            not in process.stderr
        ):
            raise AssertionError(
                "genuine FDE lifetime callback registration missing"
            )
    registers = YIELD_REGISTERS.findall(process.stderr)
    if args.require_register:
        validate_register_witnesses(
            registers, live, register_witnesses
        )
    elif registers:
        raise AssertionError(
            f"machine register probe unexpectedly enabled: {registers}"
        )

    returns = sorted(int(record[6]) for record in live)
    if not all(pc in call_returns for pc in returns):
        raise AssertionError(
            f"true suspended JIT returns {returns} do not "
            f"match any native yield-call return PC {call_returns}"
        )
    if any(record[-2:] != ("0", "0") for record in live):
        raise AssertionError(
            "mistook call-edge address for an already verified "
            "native Fiber StackMap/GC safepoint"
        )
    print(
        f"PASS RT-001 yield-edge: {len(pairs)} real ORC "
        f"StackMap sites map to native x86-64 yield CALL edges "
        f"(only argument-register moves in between); "
        f"2/2 paused Fiber return PCs={returns} match caller return "
        f"(5 bytes after call); "
        f"real_dwarf_fde={'2/2' if args.require_fde else 'not required'}; "
        f"ctx_register_matched={'2/2' if args.require_register else 'not checked'}; "
        "machine_roots_proven=0 gc_safe=0"
    )


if __name__ == "__main__":
    main()
