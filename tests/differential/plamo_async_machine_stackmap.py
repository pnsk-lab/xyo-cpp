#!/usr/bin/env python3
"""Validate native async reporter pre-call StackMaps in actual x86-64 ELF.

The record is a compiler-site snapshot, NOT the saved native Fiber RIP or
a collector permission. Machine roots are deliberately unproven.
"""
from __future__ import annotations

import argparse
from collections import Counter
from http.server import ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import subprocess
import threading

from plamo_native_fiber import (
    Handler, lazy_root_fixture, mixed_sequential_reporters_fixture,
)

ROOT = Path(__file__).resolve().parents[2]
SITE = re.compile(
    r"^\[rt001-async-machine-stackmap\] tier=(eager|lazy) fn=(\S+) "
    r"site=(\d+) id=(\d+) ptr=(\d+) stack=(\d+) host=(\d+) "
    r"static=(\d+) unknown=(\d+) live_i64=(\d+) "
    r"expected_locations=(\d+) emitted=(\d+) "
    r"machine_roots_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)
TOTAL = re.compile(
    r"^\[rt001-async-machine-stackmap-total\] tier=(eager|lazy) "
    r"sites=(\d+) emitted=(\d+) refused=(\d+) "
    r"machine_roots_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)
RECORD = re.compile(
    r"Record ID: (\d+), instruction offset: (\d+)\s+(\d+) locations:"
)
LOCATION = re.compile(
    r"Record ID: (\d+), instruction offset: (\d+)\s+"
    r"(\d+) locations:\n((?:[ \t]*#\d+: [^\n]*\n)*)"
)
TYPE = re.compile(
    r"^[ \t]*#\d+: (Register|Direct|Indirect|Constant|ConstantIndex)\b",
    re.MULTILINE,
)
SYM = re.compile(
    r"^([0-9a-fA-F]+)\s+g\s+F\s+\.text\s+"
    r"[0-9a-fA-F]+\s+xyo_script_\d+$",
    re.MULTILINE,
)
RELOC = re.compile(
    r"^([0-9a-fA-F]+)\s+R_X86_64_PLT32\s+"
    r"xyo_rt_async_reporter_rooted-0x4$",
    re.MULTILINE,
)
ORIGIN = re.compile(
    r"^\[rt001-async-pointer-origin\] tier=(eager|lazy) fn=(\S+) "
    r"site=(\d+) stackmap_id=(\d+) location=(\d+) "
    r"region=(native_stack|host_context|static_global|unknown) "
    r"cause=(\S+) def=(\S+) callee=(\S+) "
    r"chain_terminal=(\S+) derefs=(\d+) derives=(\d+) "
    r"host_offset=(-?\d+) candidate_varslot=(-?\d+) "
    r"ownership_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)
AUDIT = re.compile(
    r"^\[rt001-postopt-async-root\] tier=(eager|lazy) fn=(\S+) "
    r"words=(\d+) filled=(\d+) live_i64=(\d+) arg_i64=(\d+) "
    r"covered_i64=(\d+) unknown_i64=(\d+) live_ptr=(\d+) "
    r"signature=(\d+) buffer=(\d+) stores=(\d+) covered=(\d+) "
    r"valid=(\d+) machine_roots_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)


def run(native: Path, out: Path, readobj: Path, objdump: Path,
        tier: str, port: int, guarded: bool = False,
        indexed: bool = False):
    case = "mixed" if tier == "eager" else "lazy"
    folder = out / tier
    folder.mkdir(parents=True, exist_ok=True)
    for obj in folder.glob("*.o"):
        obj.unlink()
    project = folder / f"async-{case}.sb3"
    if tier == "eager":
        mixed_sequential_reporters_fixture(project)
        expected = {
            "prefix": "prefix-new",
            "result": "prefix-oldScratchからこんにちは入れ子の手続き",
            "done": "1",
        }
        expected_sites = 2
    else:
        lazy_root_fixture(project)
        expected = {
            "prefix": "helloworld",
            "result": "helloworldScratchからこんにちは",
            "precheck": "1",
            "done": "1",
        }
        expected_sites = 1

    env = dict(
        os.environ,
        XYO_JIT_LAZY_MODE="on" if tier == "lazy" else "off",
        XYO_RT_GUARD_NATIVE_VAR_SLOTS="1" if guarded else "0",
        XYO_RT_GUARD_NATIVE_VAR_SLOTS_FAST=(
            "1" if indexed else "0"),
        XYO_RT_GUARD_NATIVE_VAR_SLOTS_INDEXED=(
            "1" if indexed else "0"),
        XYO_JIT_PLAMO_FIBER="1",
        XYO_RT_REQUIRE_GENERATED_SSA_ROOTS="1",
        XYO_JIT_PLAMO_ROOT_LIVE_VARS="1",
        XYO_RT_POSTOPT_ASYNC_ROOT_AUDIT="1",
        XYO_RT_ASYNC_MACHINE_STACKMAP_PROBE="1",
        XYO_RT_ASYNC_POINTER_ORIGINS="1",
        XYO_JIT_DUMP_OBJECT=str((folder / "stackmap").resolve()),
        XYO_PLAMO_TRANSLATE="1", XYO_PLAMO_PORT=str(port),
        XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0",
    )
    proc = subprocess.run(
        [str(native), "run", str(project), "--jit", "eager",
         "--frames", "500"], cwd=ROOT, env=env,
        capture_output=True, text=True, timeout=35,
    )
    assert proc.returncode == 0, (tier, proc.stderr[-3000:])
    assert json.loads(
        proc.stdout[proc.stdout.index("{"):proc.stdout.rindex("}")+1]
    )["Bench"] == expected, tier

    sites = SITE.findall(proc.stderr)
    assert len(sites) == expected_sites, (tier, sites, proc.stderr[-3500:])
    declared: dict[int, int] = {}
    categories = Counter()
    for line in sites:
        (logged_tier, function, ordinal, identifier, ptr, stack, host,
         static, unknown, i64, locations, emitted, machine, safe) = line
        assert logged_tier == tier and function.startswith("xyo_script_")
        assert int(ordinal) >= 1
        ptr, stack, host, static, unknown, i64, locations = map(
            int, (ptr, stack, host, static, unknown, i64, locations)
        )
        assert ptr == stack + host + static + unknown
        assert locations == 1 + ptr + i64
        assert emitted == "1" and machine == "0" and safe == "0"
        assert int(identifier) not in declared
        declared[int(identifier)] = locations
        categories.update({"stack": stack, "host": host,
                           "static": static, "unknown": unknown})
    if guarded:
        # A guarded lookup is revalidated at each source access, so
        # hoisted borrowed Variable* values must not survive the call.
        assert categories["unknown"] == 0, categories
    else:
        assert categories["unknown"] >= 1, categories

    # Crosscheck exact operand ordinals against the unmodified
    # machine StackMap. Entry #0 is the explicit JitCtx; all other
    # pointer SSA positions are #1..ptr with their own LLVM provenance.
    origins = ORIGIN.findall(proc.stderr)
    by_id: dict[int, list[tuple[int, str, str, str, str, int]]] = {}
    source_kinds = Counter()
    for (origin_tier, fn, ordinal, stackmap_id, location, region,
         cause, definition, callee, terminal, derefs, derives, host_offset,
         borrow_slot, ownership, safe) in origins:
        assert origin_tier == tier and fn.startswith("xyo_script_")
        assert int(ordinal) >= 1
        assert ownership == safe == "0"
        ident = int(stackmap_id)
        assert ident in declared, (tier, stackmap_id, origins)
        assert region != "unknown" or cause != "other", (
            tier, fn, ordinal, location, cause, definition
        )
        if region != "unknown":
            assert cause == "other"
        assert definition not in {"other", "constexpr"}
        assert int(derefs) >= 0 and int(derives) >= 0
        if cause == "load_unknown":
            assert int(derefs) >= 1 and terminal != "none"
            if int(borrow_slot) >= 0:
                # Shape-only C++ Variable* binding from JitCtx + 96.
                # Keep it UNKNOWN for GC and variable rebind safety.
                assert terminal == "host"
                assert int(host_offset) >= 0
                assert region == "unknown"
        else:
            assert int(borrow_slot) == -1
        if region != "unknown":
            assert terminal == "none" and int(derefs) == 0
        by_id.setdefault(ident, []).append((
            int(location), region, cause, definition, callee,
            int(borrow_slot)
        ))
        source_kinds[
            f"{region}/{cause}/{terminal}/slot={borrow_slot}"
        ] += 1
    assert len(by_id) == expected_sites, (tier, by_id)
    for ident, entries in by_id.items():
        assert [pos for pos, *_ in entries] == list(
            range(1, len(entries) + 1)
        ), (tier, ident, entries)
        # Both Scratch variable bindings outlive the translator
        # callsite as borrowed Variable* in the compiler's IR.
        # They remain UNKNOWN in the collector provenance census.
        assert sorted(borrow for *_, borrow in entries
                      if borrow >= 0) == (
            [] if guarded else [1, 2]
        ), (tier, ident, entries)
    assert len(origins) == sum(categories.values()), (
        tier, len(origins), categories
    )
    assert sum(r == "unknown" for arr in by_id.values()
               for _, r, _, _, _, _ in arr) == categories["unknown"]
    print(
        f"PASS RT-001 native {tier} pointer SSA location origin: "
        f"{len(origins)} operands, {dict(source_kinds)}, "
        f"guarded={int(guarded)} ownership_proven=0 gc_safe=0",
        flush=True
    )

    audit = AUDIT.findall(proc.stderr)
    assert len(audit) == expected_sites, (tier, audit)
    for row in audit:
        assert row[0] == tier
        (words, filled, live, args, covered, unknown, ptr, signature,
         buffer, stores, full, valid, machine, safe) = map(int, row[2:])
        assert words > 0 and words == filled
        assert live == covered and 0 <= args <= live
        assert signature == buffer == stores == full == valid == 1
        assert machine == safe == 0
    totals = [
        x for x in TOTAL.findall(proc.stderr)
        if x[0] == tier and int(x[1]) > 0
    ]
    assert len(totals) == 1, (tier, totals)
    assert tuple(map(int, totals[0][1:])) == (
        expected_sites, expected_sites, 0, 0, 0
    ), totals

    objects = sorted(folder.glob("*.o"))
    assert objects, f"{tier}: missing ORC ELF object"
    found = Counter()
    machine_types = Counter()
    relative_gaps = []
    for obj in objects:
        read = subprocess.run(
            [str(readobj), "--stackmap", str(obj)],
            text=True, capture_output=True, check=True, timeout=10,
        )
        records = LOCATION.findall(read.stdout)
        assert len(records) == len(RECORD.findall(read.stdout))
        markers = []
        for identifier, offset, count, body in records:
            identifier, offset, count = map(int, (identifier, offset, count))
            assert identifier in declared, (tier, obj, identifier)
            assert offset >= 0
            assert count == declared[identifier], (
                tier, obj, identifier, declared[identifier], count
            )
            types = TYPE.findall(body)
            assert len(types) == count, (tier, obj, count, types, body)
            machine_types.update(types)
            found[identifier] += 1
            markers.append(offset)
        if markers:
            syms = subprocess.run(
                [str(objdump), "--syms", str(obj)],
                text=True, capture_output=True, check=True, timeout=10,
            )
            starts = SYM.findall(syms.stdout)
            assert len(starts) == 1, (tier, obj, starts)
            start = int(starts[0], 16)
            relocs = subprocess.run(
                [str(objdump), "--reloc", str(obj)],
                text=True, capture_output=True, check=True, timeout=10,
            )
            call_returns = sorted(
                int(addr, 16) + 4 - start
                for addr in RELOC.findall(relocs.stdout)
            )
            assert len(call_returns) == len(markers), (
                tier, markers, call_returns
            )
            for stackmap_offset, call_return in zip(
                sorted(markers), call_returns
            ):
                # StackMap is immediately before the reporter bridge in
                # LLVM IR, but it is not the Fiber's later saved RIP.
                gap = call_return - stackmap_offset
                assert 0 < gap <= 256, (
                    tier, obj, stackmap_offset, call_return
                )
                relative_gaps.append(gap)
    assert set(found) == set(declared), (tier, found, declared)
    assert all(n == 1 for n in found.values()), (tier, found)
    print(f"PASS RT-001 native async {tier}: {len(declared)}/"
          f"{expected_sites} real machine StackMaps, "
          f"locations={list(declared.values())}, "
          f"pointer regions={dict(categories)}, "
          f"machine location types={dict(machine_types)}, "
          f"pre-call gaps={relative_gaps}, "
          "machine_roots_proven=0 gc_safe=0", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--readobj", required=True, type=Path)
    parser.add_argument("--objdump", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument(
        "--guarded", action="store_true",
        help="verify that per-use native variable rebind removes live Variable*",
    )
    parser.add_argument(
        "--indexed", action="store_true",
        help="use the full-stamp indexed guarded cache; requires --guarded",
    )
    args = parser.parse_args()
    if args.indexed and not args.guarded:
        parser.error("--indexed requires --guarded")
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for tier in ("eager", "lazy"):
            run(args.native.resolve(), args.out.resolve(),
                args.readobj, args.objdump, tier,
                server.server_port, guarded=args.guarded,
                indexed=args.indexed)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
