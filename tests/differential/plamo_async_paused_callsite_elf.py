#!/usr/bin/env python3
"""RT-001: real suspended Fiber caller return PC vs actual ORC ELF.

An ABI frame return-PC is the physically active JIT caller, not a raw
stack-scan candidate. Lazy code uses the genuine GNU JIT FDE owner
instead of the entry trampoline. Neither proves machine GC roots.
"""
from __future__ import annotations

import argparse
from http.server import ThreadingHTTPServer
import os
from pathlib import Path
import re
import subprocess
import threading

from plamo_native_fiber import (
    Handler, lazy_root_fixture, mixed_sequential_reporters_fixture,
)
from plamo_async_machine_stackmap import LOCATION, RECORD, RELOC, SYM, ORIGIN

FRAME_ARG = re.compile(
    r"^\s*([0-9a-fA-F]+):.*?\tleaq\s+-0x([0-9a-fA-F]+)"
    r"\(%rbp\),\s*%rdx\b",
    re.MULTILINE,
)
UNWIND = re.compile(
    r"^\[rt001-async-unwind\] tier=(eager|lazy) init=(-?\d+) "
    r"frames=(\d+) jit_reached=(\d+) rbx_ctx=(\d+) "
    r"rbp_frame=(\d+) saved_callee_regs=(\d+) jit_cfi=(\d+) "
    r"register_mask=(\d+) saved_context_diff_mask=(\d+) "
    r"live_i64_root_match=(-?\d+) rbp_from_saved_sp=(\d+) stack_top_from_rbp=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)
LOCATION_REG = re.compile(
    r"^\s*#\d+: (Register|Direct|Indirect) R#(\d+)"
    r"(?: \+ (-?\d+))?, size: (\d+)$"
)
CALLEE_SAVED = {3, 6, 12, 13, 14, 15}
EXPECTED_REGISTER_MASK = sum(1 << register for register in CALLEE_SAVED)

JIT_SYMBOL = re.compile(
    r"^([0-9a-fA-F]+)\s+g\s+F\s+\.text\s+"
    r"([0-9a-fA-F]+)\s+xyo_script_\d+$",
    re.MULTILINE,
)
ELF_FDE = re.compile(
    r"\bFDE cie=[0-9a-fA-F]+ pc="
    r"([0-9a-fA-F]+)\.\.\.([0-9a-fA-F]+)"
)

VARSLOT_CFI = re.compile(
    r"^\[rt001-async-varslot-cfi\] tier=(eager|lazy) site=(\d+) "
    r"slot=(\d+) reg_mask=(\d+) ctx_array=1 gc_header=(\d+) "
    r"ownership_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)

REAL_FDE = re.compile(
    r"^\[rt001-async-unwind-fde\] tier=(eager|lazy) "
    r"registration=(\d+) owner=(scoped|orc) orc_live=(\d+) "
    r"before=(-?\d+) during=(\d+) "
    r"fresh_reached=(\d+) fresh_cfi=(\d+) fde_bytes=(\d+) "
    r"fresh_regs=(\d+) regs_match=(\d+) rbx_ctx=(\d+) "
    r"rbp_frame=(\d+) live_i64_root=(-?\d+) canceled=(\d+) "
    r"after=(-?\d+) machine_roots_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)

CALLER = re.compile(
    r"^\[rt001-async-paused-caller\] tier=(eager|lazy) "
    r"site=(\d+) saved_pc_host=(\d+) jit_caller_anonymous=(\d+) "
    r"frame_in_stack=(\d+) frame_return_matched=(\d+) "
    r"jit_frame_in_stack=(\d+) input_in_jit_frame=(\d+) "
    r"input_rbp_offset=(\d+) input_words=(\d+) "
    r"entry_region=(\d+) entry_offset=(\d+) "
    r"fde_owner=(\d+) fde_offset=(\d+) owners=(\d+) "
    r"stackmap_pc_matched=(\d+) machine_frame_verified=(\d+) "
    r"gc_safe=(\d+)$",
    re.MULTILINE,
)


def one_case(native: Path, out: Path, readobj: Path, objdump: Path,
             dwarfdump: Path, mode: str, ordinal: int,
             exit_path: str, server_port: int, orc_plugin: bool) -> None:
    folder = out / ("orc" if orc_plugin else "scoped") / mode / (
        f"site-{ordinal}") / exit_path
    folder.mkdir(parents=True, exist_ok=True)
    for obj in folder.glob("*.o"):
        obj.unlink()
    project = folder / "paused-translate.sb3"
    if mode == "eager":
        mixed_sequential_reporters_fixture(project)
        wanted_sites = 2
    else:
        lazy_root_fixture(project)
        wanted_sites = 1

    env = dict(
        os.environ,
        XYO_PLAMO_TRANSLATE="1",
        XYO_PLAMO_PORT=str(server_port),
        XYO_JIT_DUMP_OBJECT=str((folder / "jit").resolve()),
        # Two independent FDE ownership contracts: the existing
        # test-scoped registration vs the real ResourceTracker plugin.
        XYO_RT001_TEST_REGISTER_FDE="0" if orc_plugin else "1",
        XYO_RT001_TEST_USE_ORC_FDE="1" if orc_plugin else "0",
        XYO_RT_ORC_FDE_REGISTRATION="1" if orc_plugin else "0",
        XYO_RT_ASYNC_POINTER_ORIGINS="1" if orc_plugin else "0",
        XYO_RT001_TEST_VARSLOT_CFI="1" if orc_plugin else "0",
    )
    argv = [str(native), str(project), mode]
    if exit_path == "resume":
        argv.append("resume")
    if ordinal == 2:
        argv.append("second")
    result = subprocess.run(
        argv, capture_output=True, text=True, env=env,
        timeout=20,
    )
    assert result.returncode == 0, (
        mode, exit_path, result.stdout, result.stderr[-3000:]
    )
    assert "PASS RT-001 paused physical async reporter caller frame" in (
        result.stdout
    )
    records = CALLER.findall(result.stderr)
    assert len(records) == 1, (mode, exit_path, records, result.stderr[-3000:])
    (tier, site, saved_host, jit_anon, frame, return_matches,
     jit_frame, input_frame, rbp_offset, root_words,
     entry_region, entry_offset, fde_owner, fde_offset, owners,
     source_match, machine, safe) = records[0]
    assert tier == mode and int(site) == ordinal
    assert int(root_words) == ordinal
    assert [int(saved_host), int(jit_anon), int(frame),
            int(return_matches), int(jit_frame), int(input_frame),
            int(fde_owner), int(owners)] == [1]*8
    assert 0 < int(rbp_offset) < 65536, rbp_offset
    assert (source_match, machine, safe) == ("0", "0", "0")
    if mode == "eager":
        assert entry_region == "1"
        assert entry_offset == fde_offset

    # Provenance: begin from the actual paused FiberRegs and unwind
    # C++ host frames to the captured physical JIT return-PC. The
    # libunwind JIT FDE is not currently complete: we verify only
    # the callee-saved register snapshot, not arbitrary GC roots.
    unwinds = UNWIND.findall(result.stderr)
    assert len(unwinds) == 1, (mode, exit_path, unwinds)
    (utier, init, frames, reached, rbx_ctx, rbp_frame,
     available, jit_cfi, register_mask, saved_diff_mask,
     live_i64_root_match, rbp_from_sp, stack_top_from_rbp,
     safe) = unwinds[0]
    assert utier == mode and init == "0"
    assert int(frames) >= 3 and reached == "1"
    assert rbx_ctx == rbp_frame == "1"
    assert available == "6"
    assert int(register_mask) == EXPECTED_REGISTER_MASK
    assert int(saved_diff_mask) & ~EXPECTED_REGISTER_MASK == 0
    # Host context-switch RBP is NOT the paused JIT RBP. Reading
    # FiberRegs directly as a StackMap register would be incorrect.
    assert int(saved_diff_mask) & (1 << 6)
    # Second native reporter: previous reporter's boxed SSA value
    # in DWARF R15 is byte-identical to Thread-owned root buffer[1].
    assert live_i64_root_match == ("1" if ordinal == 2 else "-1"), (
        tier, ordinal, live_i64_root_match
    )
    assert safe == "0"
    assert int(rbp_from_sp) >= 64
    assert int(stack_top_from_rbp) > 0

    # Verify libunwind's direct lookup and a newly initialized cursor
    # use the genuine GNU JIT FDE, rather than the old 1-byte fallback.
    registrations = REAL_FDE.findall(result.stderr)
    assert len(registrations) == 1, (
        mode, ordinal, exit_path, registrations, result.stderr[-2000:]
    )
    (registered_tier, registration, owner, orc_live, before, during,
     fresh_reached, fresh_cfi, fde_bytes, fresh_regs, regs_match,
     fresh_rbx, fresh_rbp, fresh_live_root, canceled, after,
     machine, safe) = (
        registrations[0]
    )
    assert registered_tier == mode
    assert all(int(flag) == 1 for flag in (
        registration, during, fresh_reached, fresh_cfi, fresh_rbx,
        fresh_rbp
    )), registrations[0]
    assert 64 < int(fde_bytes) <= 4096, fde_bytes
    assert (fresh_regs, regs_match) == ("6", "6")
    assert fresh_live_root == ("1" if ordinal == 2 else "-1")
    # Avoid querying unregistered ORC FDE bytes: this libunwind
    # build can crash when searching its unindexed unwind table.
    assert before == after == "-1"
    if orc_plugin:
        assert (owner, orc_live, canceled) == ("orc", "1", "0")
        assert (
            "[rt001-orc-fde-index] phase=after_fiber registered=1 gc_safe=0"
            in result.stderr and
            "[rt001-orc-fde-index] phase=after_runtime registered=0 gc_safe=0"
            in result.stderr
        ), (mode, ordinal, exit_path, result.stderr[-2200:])
        assert f"phase=install tier={mode} ready=1 gc_safe=0" in result.stderr
        emitted = [
            int(v) for v in re.findall(
                r"\[rt001-orc-fde\] phase=emit registered=(\d+) "
                r"gc_safe=0", result.stderr)
        ]
        removed = [
            int(v) for v in re.findall(
                r"\[rt001-orc-fde\] phase=remove canceled=(\d+) "
                r"gc_safe=0", result.stderr)
        ]
        assert emitted and sum(emitted) == sum(removed), (
            mode, ordinal, emitted, removed, result.stderr[-1800:]
        )
        assert "phase=emit fde_missing=1" not in result.stderr
        assert "phase=emit invalid_extent=1" not in result.stderr
    else:
        assert (owner, orc_live, canceled) == ("scoped", "0", "1")
        assert "[rt001-orc-fde] phase=install" not in result.stderr
    assert machine == safe == "0"

    varSlotOrigins = []
    varSlotMasks: dict[int, int] = {}
    if orc_plugin:
        # LLVM IR origin records enumerate pointer operands from zero
        # after the explicit JitCtx, while llvm-readobj uses #1-based
        # physical locations including JitCtx as its first operand.
        varSlotOrigins = [
            origin for origin in ORIGIN.findall(result.stderr)
            if int(origin[2]) == ordinal and int(origin[13]) >= 0
        ]
        assert len(varSlotOrigins) == 2, (
            mode, ordinal, exit_path, varSlotOrigins,
            result.stderr[-1800:]
        )
        assert {int(origin[13]) for origin in varSlotOrigins} == {1, 2}
        for origin in varSlotOrigins:
            assert origin[5:7] == ("unknown", "load_unknown")
            assert origin[9] == "host" and int(origin[10]) == 2
            assert int(origin[12]) >= 0
            assert origin[-2:] == ("0", "0")
        snapshots = VARSLOT_CFI.findall(result.stderr)
        assert len(snapshots) == 2, (mode, snapshots)
        for tier, site, slot, mask, gc_header, ownership, safe in snapshots:
            assert tier == mode and int(site) == ordinal
            assert gc_header == ownership == safe == "0"
            assert int(mask) > 0 and int(slot) not in varSlotMasks
            varSlotMasks[int(slot)] = int(mask)
        assert set(varSlotMasks) == {1, 2}

    objects = sorted(folder.glob("*.o"))
    assert objects, (mode, exit_path, "missing ORC object dumps")
    return_pcs: list[int] = []
    stackmaps: list[int] = []
    frame_buffer_deltas: dict[int, int] = {}
    for obj in objects:
        read = subprocess.run(
            [str(readobj), "--stackmap", str(obj)],
            capture_output=True, text=True, check=True, timeout=8,
        )
        locations = LOCATION.findall(read.stdout)
        assert len(locations) == len(RECORD.findall(read.stdout))
        if not locations:
            continue
        if orc_plugin:
            for origin in varSlotOrigins:
                marker = int(origin[3])
                position = int(origin[4]) + 1
                slot = int(origin[13])
                matching = [
                    body for ident, _, _, body in locations
                    if int(ident) == marker
                ]
                assert len(matching) == 1, (marker, locations)
                lines = matching[0].splitlines()
                assert 1 <= position <= len(lines)
                location = LOCATION_REG.fullmatch(lines[position - 1].strip())
                assert location, (mode, ordinal, lines[position - 1])
                kind, reg, displacement, size = location.groups()
                # Current x86-64 O3 fixtures materialize the two
                # borrowed Variable* values in callee-saved registers.
                # A volatile register or indirect location requires
                # a new physical state reconstruction proof.
                assert kind == "Register" and displacement is None
                assert size == "8"
                assert varSlotMasks[slot] & (1 << int(reg)), (
                    mode, ordinal, slot, reg, varSlotMasks[slot]
                )
        sym = subprocess.run(
            [str(objdump), "--syms", str(obj)],
            capture_output=True, text=True, check=True, timeout=8,
        )
        fn_starts = SYM.findall(sym.stdout)
        assert len(fn_starts) == 1, (obj, fn_starts)
        fn_start = int(fn_starts[0], 16)
        symbol_ranges = JIT_SYMBOL.findall(sym.stdout)
        assert len(symbol_ranges) == 1, (mode, obj, symbol_ranges)
        symbol_start, symbol_size = (
            int(field, 16) for field in symbol_ranges[0]
        )
        assert symbol_start == fn_start
        # The real GNU FDE registered against the paused ORC function
        # must have precisely the PC extent encoded in the ELF's own
        # .eh_frame, and it must equal the function symbol's range.
        dwarf = subprocess.run(
            [str(dwarfdump), "--eh-frame", str(obj)],
            capture_output=True, text=True, check=True, timeout=8,
        )
        fde_extents = [
            int(end, 16) - int(start, 16)
            for start, end in ELF_FDE.findall(dwarf.stdout)
        ]
        assert len(fde_extents) == 1, (mode, obj, fde_extents)
        assert fde_extents[0] == symbol_size == int(fde_bytes), (
            mode, obj, fde_extents, symbol_size, fde_bytes
        )
        relocs = subprocess.run(
            [str(objdump), "--reloc", str(obj)],
            capture_output=True, text=True, check=True, timeout=8,
        )
        returns = sorted(
            int(addr, 16) + 4 - fn_start
            for addr in RELOC.findall(relocs.stdout)
        )
        markers = sorted(int(offset) for _, offset, _, _ in locations)
        assert len(returns) == len(markers), (obj, returns, markers)
        for identifier, offset, count, body in locations:
            lines = [line.strip() for line in body.splitlines()]
            assert len(lines) == int(count), (obj, count, body)
            for line in lines:
                location = LOCATION_REG.fullmatch(line)
                # Fail closed if the emitter starts using a volatile
                # register, a constant, or an unexpected location kind.
                # The saved SysV FiberRegs do NOT preserve volatile regs.
                assert location, (mode, obj, line)
                kind, reg, displacement, size = location.groups()
                reg, size = int(reg), int(size)
                assert reg in CALLEE_SAVED
                assert int(register_mask) & (1 << reg), (reg, register_mask)
                assert size == 8
                if kind in {"Direct", "Indirect"}:
                    assert displacement is not None
                    # The active stack location is RBP-relative. Only
                    # the site that actually suspended can be mapped to
                    # this runtime RBP; other compiled sites are not
                    # called during this snapshot.
                    if int(offset) == sorted(markers)[ordinal-1] and (
                        int(fde_offset) == sorted(returns)[ordinal-1]
                    ):
                        pos = int(rbp_from_sp) + int(displacement)
                        assert 0 <= pos and pos + size <= (
                            int(rbp_from_sp) +
                            int(stack_top_from_rbp)
                        ), (reg, displacement, pos)
        return_pcs.extend(returns)
        stackmaps.extend(markers)
        objcode = subprocess.run(
            [str(objdump), "--disassemble", str(obj)],
            capture_output=True, text=True, check=True, timeout=8,
        )
        frame_areas = [
            (int(address, 16) - fn_start, int(delta, 16))
            for address, delta in FRAME_ARG.findall(objcode.stdout)
        ]
        for ret in returns:
            candidates = [
                (pc, delta) for pc, delta in frame_areas
                if ret - 64 <= pc < ret
            ]
            if candidates:
                pc, delta = max(candidates)
                frame_buffer_deltas[ret] = delta

    assert len(return_pcs) == wanted_sites, (
        mode, return_pcs, stackmaps
    )
    assert len(stackmaps) == wanted_sites
    assert int(fde_offset) == sorted(return_pcs)[ordinal-1], (
        mode, ordinal, exit_path, fde_offset, return_pcs
    )
    # Pre-call machine StackMap sites are before the real active caller
    # return PC; they are NOT the native Fiber's saved switching RIP.
    assert all(
        0 < ret - marker <= 256
        for ret, marker in zip(sorted(return_pcs), sorted(stackmaps))
    )
    assert int(fde_offset) not in stackmaps
    # Actual JIT RBP frame, physical root buffer, and O3 machine
    # encoded third-argument LEA must describe the SAME stack address.
    assert frame_buffer_deltas.get(int(fde_offset)) == int(rbp_offset), (
        mode, exit_path, fde_offset, rbp_offset, frame_buffer_deltas
    )
    print(
        f"PASS RT-001 paused {mode} site={ordinal} {exit_path}: physical "
        f"return={fde_offset}, ELF returns={return_pcs}, "
        f"StackMap sites={stackmaps}, "
        f"root_spill_rbp_offset={rbp_offset}, "
        f"unwind_frames={frames}, available_regs={available}, "
        f"reg_mask={register_mask}, jit_cfi={jit_cfi}, "
        f"real_fde_bytes={fde_bytes}, dwarf_fde_exact=1, "
        f"fresh_cfi={fresh_cfi}, "
        f"fresh_regs_matched={regs_match}, owner={owner}, "
        f"lease_canceled={canceled}, "
        f"live_i64_root_match={live_i64_root_match}, "
        f"borrowed_varslots_matched={len(varSlotOrigins)}, "
        "frame_return_verified=1, machine_frame_verified=0 gc_safe=0",
        flush=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--smoke", type=Path, required=True)
    parser.add_argument("--readobj", type=Path, required=True)
    parser.add_argument("--objdump", type=Path, required=True)
    parser.add_argument("--dwarfdump", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--orc-plugin", action="store_true",
        help="Use the real ORC ResourceTracker-owned JIT FDE, no scoped lease",
    )
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for tier in ("eager", "lazy"):
            for ordinal in ((1, 2) if tier == "eager" else (1,)):
                for exit_path in ("cancel", "resume"):
                    one_case(args.smoke.resolve(), args.out.resolve(),
                             args.readobj, args.objdump,
                             args.dwarfdump, tier, ordinal,
                             exit_path, server.server_port,
                             args.orc_plugin)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
