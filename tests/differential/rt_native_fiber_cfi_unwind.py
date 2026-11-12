#!/usr/bin/env python3
"""RT-001: classify a *real* suspended Fiber unwind and verify ORC calls.

Libunwind can recover a JIT return IP by following frame pointers, while
synthesizing a one-byte procedure range instead of a real JIT DWARF FDE.
That is NOT a certified GC machine-frame. This test never authorizes GC.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess

from native_simple_resume import fixture
from rt_native_fiber_pc_stackmap import RECORD, RELOC, SYM

GNU_FDE = re.compile(
    r"^\[rt001-gnu-fde\] thread=(\S+) lookup_available=(\d+) "
    r"registered_fde_found=(\d+) owner_matches=(\d+) "
    r"machine_frame_verified=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)
DIRECT = re.compile(
    r"^\[rt001-libunwind-direct\] thread=(\S+) "
    r"cursor_rc=(-?\d+) by_ip_rc=(-?\d+) by_ip_real_fde=(\d+) "
    r"before_registered=(\d+) fresh_cursor_fde=(\d+) "
    r"fresh_steps=(\d+) runtime_fde_bytes=(\d+) safe=(\d+)$",
    re.MULTILINE,
)
CANCEL = re.compile(
    r"^\[rt001-libunwind-cancel\] thread=(\S+) "
    r"real_fde_after_cancel=(\d+) machine_roots_proven=(\d+) "
    r"gc_safe=(\d+)$",
    re.MULTILINE,
)
BRIDGE = re.compile(
    r"^\[rt001-libunwind-bridge\] thread=(\S+) "
    r"offset_fits=(\d+) fde_found=(\d+) "
    r"owner_matches=(\d+) native_safepoint_verified=(\d+) "
    r"gc_safe=(\d+)$",
    re.MULTILINE,
)

TRACE = re.compile(
    r"^\[rt001-cfi-trace\] thread=(\S+) init=(-?\d+) "
    r"cfi_frames=(\d+) jit_frames=(\d+) jit_cfi=(\d+) "
    r"one_byte_fallbacks=(\d+) jit_return_offset=(\d+) "
    r"jit_cfi_start=(\d+) jit_cfi_end=(\d+) "
    r"machine_frame_verified=(\d+) stackmap_pc_matched=(\d+) "
    r"gc_safe=(\d+)$", re.MULTILINE,
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--smoke", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--readobj", type=Path, default=Path("llvm-readobj"))
    parser.add_argument("--objdump", type=Path, default=Path("llvm-objdump"))
    parser.add_argument("--dwarfdump", type=Path, default=Path("llvm-dwarfdump"))
    parser.add_argument(
        "--bridge", action="store_true",
        help="Register GNU's actual JIT FDE transiently with libunwind",
    )
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    project = fixture(out, "ssa_budget_cross_fiber_two_hats")
    objects_dir = out / "objects"
    objects_dir.mkdir(exist_ok=True)
    for obj in objects_dir.glob("*.o"):
        obj.unlink()

    environment = dict(os.environ)
    environment["XYO_JIT_DUMP_OBJECT"] = str(objects_dir / "obj")
    # Every run is an independent process: no stale JIT FDE tables
    # or environment contamination from other smoke tests.
    environment["XYO_RT001_GNU_FDE_PROBE"] = "1"
    environment["XYO_RT001_LIBUNWIND_DYNAMIC_BRIDGE"] = (
        "1" if args.bridge else "0"
    )
    environment["XYO_RT001_RESUME_AFTER_CANCEL"] = (
        "1" if args.bridge else "0"
    )
    process = subprocess.run(
        [str(args.smoke.resolve()), str(project.resolve())],
        cwd=out, env=environment, capture_output=True,
        text=True, timeout=120,
    )
    if process.returncode != 0:
        raise AssertionError(
            f"native Fiber CFI trial failed {process.returncode}\n"
            f"{process.stdout}\n{process.stderr[-6000:]}"
        )
    traces = TRACE.findall(process.stderr)
    if len(traces) != 2 or len({record[0] for record in traces}) != 2:
        raise AssertionError(f"expected two independent native Fibers: {traces}")

    gnu_records = GNU_FDE.findall(process.stderr)
    if len(gnu_records) != 2 or set(row[0] for row in gnu_records) != set(
        row[0] for row in traces
    ) or any(row[1:] != ("1", "1", "1", "0", "0")
             for row in gnu_records):
        raise AssertionError(
            f"GNU has no valid JIT FDE owner for both Fibers: {gnu_records}"
        )
    direct_records = DIRECT.findall(process.stderr)
    bridge_records = BRIDGE.findall(process.stderr)
    cancel_records = CANCEL.findall(process.stderr)
    if args.bridge:
        if (process.stderr.count(
            "[rt001-libunwind-resume] completed=1 y=48005 "
            "dynamic_fde_cancelled=1 gc_safe=0"
        ) != 1):
            raise AssertionError(
                "native code failed to resume after unregistering "
                "both temporary JIT FDEs"
            )
        if (len(direct_records) != 2 or
            len(bridge_records) != 2 or len(cancel_records) != 2):
            raise AssertionError(
                "missing dynamic libunwind observations: "
                f"direct={direct_records}, bridge={bridge_records}, "
                f"cancel={cancel_records}"
            )
        for row in direct_records:
            thread, cursor, by_ip, real, before, fresh, steps, fde_size, safe = row
            if (
                cursor != "0" or by_ip != "0" or real != "1" or
                before != "0" or fresh != "1" or
                int(steps) < 1 or int(fde_size) < 64 or safe != "0"
            ):
                raise AssertionError(
                    f"{thread}: bogus libunwind CFI bridge: {row}"
                )
        for row in bridge_records:
            thread, fits, found, owner, safepoint, gc_safe = row
            if (fits, found, owner, safepoint, gc_safe) != (
                "1", "1", "1", "0", "0"
            ):
                raise AssertionError(
                    f"{thread}: dynamic registration failed: {row}"
                )
        if set(row[0] for row in cancel_records) != set(
            row[0] for row in traces
        ) or any(
            row[1:] != ("0", "0", "0") for row in cancel_records
        ):
            raise AssertionError(
                f"dynamic JIT FDE survived unregister: {cancel_records}"
            )
    elif direct_records or bridge_records or cancel_records:
        raise AssertionError(
            "transient FDE registration unexpectedly enabled "
            f"without --bridge: direct={direct_records}, "
            f"bridge={bridge_records}"
        )

    jit_return_offsets = []
    for (
        thread, init, frames, jit_frames, jit_cfi, synthetic,
        offset, cfi_start, cfi_end, verified, matched, gc_safe,
    ) in traces:
        if init != "0" or int(frames) < 3 or jit_frames != "1":
            raise AssertionError(f"{thread}: cannot unwind live JIT frame")
        is_synthetic = (
            jit_cfi == "0" and synthetic == "1" and
            cfi_start == offset and int(cfi_end) == int(offset) + 1 and
            verified == "0"
        )
        is_registered_fde = (
            jit_cfi == "1" and synthetic == "0" and verified == "1"
        )
        if not (is_synthetic or is_registered_fde):
            raise AssertionError(
                f"{thread}: false CFI owner certification: "
                f"jit_cfi={jit_cfi} synthetic={synthetic} "
                f"start={cfi_start} end={cfi_end} verified={verified}"
            )
        if matched != "0" or gc_safe != "0":
            raise AssertionError(f"{thread}: GC authorization unsupported")
        jit_return_offsets.append(int(offset))

    objects = sorted(objects_dir.glob("*.o"))
    if len(objects) != 2:
        raise AssertionError(
            f"expected two JIT ELF objects, actual {len(objects)}"
        )
    yield_returns = []
    capture_returns = []
    stackmaps = []
    elf_fdes = []
    for obj in objects:
        sym = subprocess.run(
            [str(args.objdump), "--syms", str(obj)],
            capture_output=True, text=True, check=True,
        )
        functions = SYM.findall(sym.stdout)
        if len(functions) != 1:
            raise AssertionError(f"{obj}: missing unique JIT procedure")
        start = int(functions[0], 16)
        reloc = subprocess.run(
            [str(args.objdump), "--reloc", str(obj)],
            capture_output=True, text=True, check=True,
        )
        captures = []
        yields = []
        for displacement, symbol in RELOC.findall(reloc.stdout):
            offset = int(displacement, 16) + 4 - start
            if symbol == "xyo_yield":
                yields.append(offset)
            else:
                captures.append(offset)
        if not captures or not yields:
            raise AssertionError(f"{obj}: missing expected native call")
        capture_returns.append(min(captures))
        yield_returns.append(min(yields))

        sm = subprocess.run(
            [str(args.readobj), "--stackmap", str(obj)],
            capture_output=True, text=True, check=True,
        )
        sites = RECORD.findall(sm.stdout)
        if len(sites) != 2:
            raise AssertionError(f"{obj}: missing two stackmap sites")
        stackmaps.append(int(sites[0][1]))
        dwarf = subprocess.run(
            [str(args.dwarfdump), "--eh-frame", str(obj)],
            capture_output=True, text=True, check=True,
        )
        fdes = re.findall(
            r"FDE cie=\S+ pc=([0-9a-fA-F]+)\.\.\.([0-9a-fA-F]+)",
            dwarf.stdout,
        )
        function_return = start + yield_returns[-1]
        matching = [
            (int(lo, 16), int(hi, 16))
            for lo, hi in fdes
            if int(lo, 16) <= function_return < int(hi, 16)
            and int(hi, 16) - int(lo, 16) > 64
        ]
        if not matching:
            raise AssertionError(
                f"{obj.name}: no actual full-length JIT DWARF FDE"
            )
        elf_fdes.append(matching[0])

    if args.bridge:
        dwarf_lengths = sorted(hi - lo for lo, hi in elf_fdes)
        runtime_lengths = sorted(int(row[7]) for row in direct_records)
        if dwarf_lengths != runtime_lengths:
            raise AssertionError(
                "libunwind dynamic bridge does not use exact ORC FDE "
                f"extent: LLVM={dwarf_lengths}, runtime={runtime_lengths}"
            )

    # A real unwind return PC must be after xyo_yield. The 1849 word
    # found by a raw stack scan is instead an already returned capture
    # helper. The StackMap location is yet another distinct PC.
    if (
        sorted(jit_return_offsets) != sorted(yield_returns) or
        any(not (
            capture_returns[i] < stackmaps[i] < yield_returns[i]
        ) for i in range(2))
    ):
        raise AssertionError(
            "unwind/ELF return address mismatch: "
            f"unwound={jit_return_offsets} yield={yield_returns} "
            f"capture={capture_returns} stackmaps={stackmaps}"
        )
    print(
        "PASS RT-001 suspended Fiber unwind: "
        f"2/2 recovered live JIT return offsets={jit_return_offsets}; "
        f"stale capture returns={capture_returns}, "
        f"StackMap offsets={stackmaps}, "
        f"actual yield returns={yield_returns}; "
        f"ORC ELF real DWARF FDEs={elf_fdes}; "
        f"transient GNU->libunwind FDE bridge="
        f"{'2/2 PASS' if args.bridge else 'OFF'}; "
        "original suspended-PC mismatch and GC veto retained"
    )


if __name__ == "__main__":
    main()
