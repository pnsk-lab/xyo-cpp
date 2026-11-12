#!/usr/bin/env python3
"""RT-001: compare actual post-O3 yield SSA operands and ELF StackMap homes.

A machine StackMap records a live value's location. It does not prove
that a boxed i64 contains a heap pointer, that it is rooted, or that a
Fiber parked deep within a host helper preserves those homes. GC veto.
"""
from __future__ import annotations

import argparse
from collections import Counter
import os
from pathlib import Path
import re
import subprocess

from native_simple_resume import execute, fixture
from rt_machine_stackmap_probe import RECORD
from rt_yield_edge_fiber_pc import EDGE, check_emitted

SSA = re.compile(
    r"^\[rt001-yield-ssa-stackmap\] tier=(eager|lazy) fn=(\S+) "
    r"ordinal=(\d+) helper=(xyo_yield|xyo_yield_fast) "
    r"stackmap_id=(\d+) i64=(\d+) pointers=(\d+) "
    r"other_int=(\d+) float=(\d+) other_unsupported=(\d+) "
    r"expected_locations=(\d+) liveness_valid=(\d+) "
    r"emitted=(\d+) ownership_proven=(\d+) "
    r"machine_roots_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)
TOTAL = re.compile(
    r"^\[rt001-yield-ssa-stackmap-total\] tier=(eager|lazy) "
    r"sites=(\d+) emitted=(\d+) refused=(\d+) "
    r"ownership_proven=(\d+) machine_roots_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)


def validate_machine(
    folder: Path, trace: str, tier: str, readobj: str, objdump: str,
) -> tuple[int, int, int]:
    # Reuse the independently verified geometry oracle. The new
    # snapshots may not alter the direct yield-call/return matching.
    pairs, _, _ = check_emitted(folder, trace, readobj, objdump, tier)
    edge: dict[tuple[str, int], tuple[int, str]] = {}
    for entry in EDGE.findall(trace):
        found_tier, fn, ordinal, helper, marker, count, emitted, roots, safe = entry
        if found_tier != tier or (
            count, emitted, roots, safe
        ) != ("1", "1", "0", "0"):
            raise AssertionError(f"invalid direct edge {entry}")
        edge[(fn, int(ordinal))] = (int(marker), helper)

    expected: dict[int, tuple[str, int, int, int]] = {}
    live_words = live_pointers = 0
    for entry in SSA.findall(trace):
        (found_tier, fn, ordinal, helper, marker,
         i64, ptr, smallint, floats, other, locations, valid,
         emitted, ownership, roots, safe) = entry
        if found_tier != tier or (
            valid, emitted, ownership, roots, safe
        ) != ("1", "1", "0", "0", "0"):
            raise AssertionError(f"unproven or missing native SSA site {entry}")
        key = (fn, int(ordinal))
        if key not in edge or edge[key][1] != helper:
            raise AssertionError(f"orphan SSA site, missing direct edge: {key}")
        identifier = int(marker)
        if identifier in expected or int(locations) < 1:
            raise AssertionError("duplicate/empty yield SSA StackMap")
        expected[identifier] = (
            fn, edge[key][0], int(locations), int(i64)
        )
        live_words += int(i64)
        live_pointers += int(ptr)
        # Any still-unrepresented float/small integer is a known
        # hole, never a claim of all-live-SSA completeness.
        if int(smallint) or int(floats) or int(other):
            if ownership != "0" or roots != "0" or safe != "0":
                raise AssertionError("unsupported SSA granted GC")
    if len(expected) != len(pairs):
        raise AssertionError(
            f"declared SSA sites {len(expected)} != direct call sites {len(pairs)}"
        )
    totals = [
        t for t in TOTAL.findall(trace)
        if t[0] == tier and int(t[1]) > 0
    ]
    if not totals or sum(int(t[1]) for t in totals) != len(expected):
        raise AssertionError(f"missing SSA totals: {totals}")
    if any(
        emitted != sites or refused != "0" or
        (ownership, roots, safe) != ("0", "0", "0")
        for _, sites, emitted, refused, ownership, roots, safe in totals
    ):
        raise AssertionError("unsupported ownership authorization in totals")

    found: Counter[int] = Counter()
    for obj in sorted(folder.glob("*.o")):
        obj_symbols = subprocess.run(
            [objdump, "--syms", str(obj)],
            capture_output=True, text=True, check=True,
        ).stdout
        functions = {
            name: int(start, 16)
            for start, name in re.findall(
                r"^([0-9a-fA-F]+)\s+g\s+F\s+\.text\s+"
                r"[0-9a-fA-F]+\s+(xyo_script_\S+)$",
                obj_symbols, re.MULTILINE,
            )
        }
        relevant = {name for name in functions if any(
            item[0] == name for item in expected.values()
        )}
        if not relevant:
            continue  # ORC lazy support/trampoline module
        if len(relevant) != 1:
            raise AssertionError(
                f"multiple target procedures in ORC object {obj}"
            )
        fn = next(iter(relevant))
        function_start = functions[fn]
        machine = subprocess.run(
            [objdump, "--disassemble", "--no-show-raw-insn",
             str(obj)],
            capture_output=True, text=True, check=True,
        ).stdout
        instruction_words = [
            (int(pc, 16), mnemonic)
            for pc, mnemonic in re.findall(
                r"^\s*([0-9a-fA-F]+):\s+([a-z][a-z0-9_.]*)\b",
                machine, re.MULTILINE,
            )
        ]
        decoded = subprocess.run(
            [readobj, "--stackmap", str(obj)],
            capture_output=True, text=True, check=True,
        ).stdout
        records = {
            int(marker): (int(pc), int(count))
            for marker, pc, count in RECORD.findall(decoded)
        }
        for identifier, (owner, edge_id, expected_count, _) in expected.items():
            if owner != fn:
                continue
            if edge_id not in records or identifier not in records:
                raise AssertionError(
                    f"{fn}: missing SSA or geometry machine record"
                )
            # The all-live StackMap and the older one-pointer
            # geometry StackMap are separate LLVM pseudo-instructions.
            # Backend register setup may place up to a few no-op/move
            # instructions between their recorded machine offsets.
            # The source manifest still binds both IDs to the SAME
            # native yield call, and the independent geometry oracle
            # verifies that call's exact ELF relocation and return.
            ssa_pc = records[identifier][0]
            edge_pc = records[edge_id][0]
            if not 0 <= edge_pc - ssa_pc <= 16:
                raise AssertionError(
                    f"{fn}: SSA StackMap wandered off yield edge: "
                    f"ssa={ssa_pc}, direct={edge_pc}"
                )
            intermediate = [
                (pc, word) for pc, word in instruction_words
                if function_start + ssa_pc <= pc <
                    function_start + edge_pc
            ]
            if any(
                not word.startswith(("mov", "lea", "nop", "xchg"))
                for _, word in intermediate
            ):
                raise AssertionError(
                    f"{fn}: branch, call, or other side effect "
                    f"between SSA and direct-yield StackMaps: "
                    f"{intermediate}"
                )
            if (records[identifier][1] != expected_count or
                records[edge_id][1] != 1):
                raise AssertionError(
                    f"{fn}: LLVM machine location count mismatch: "
                    f"{records[identifier]} != {expected_count}"
                )
            found[identifier] += 1

    if len(found) != len(expected) or any(n != 1 for n in found.values()):
        raise AssertionError(
            f"native StackMap IDs missing/duplicated: {found} != {expected}"
        )
    return len(expected), live_words, live_pointers


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--readobj", default="llvm-readobj")
    parser.add_argument("--objdump", default="llvm-objdump")
    args = parser.parse_args()
    native = args.native.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    project = fixture(out, "ssa_budget_cross_fiber_two_hats")
    managed = (
        "XYO_JIT_LAZY_MODE", "XYO_RT_YIELD_EDGE_STACKMAP_PROBE",
        "XYO_RT_YIELD_LIVE_SSA_STACKMAP", "XYO_JIT_DUMP_OBJECT",
        "XYO_RT_MACHINE_STACKMAP_PROBE", "XYO_RT_YIELD_SSA_FILTER",
        "XYO_RT_YIELD_SSA_POINTER_LIMIT",
    )
    old = {k: os.environ.get(k) for k in managed}
    try:
        os.environ["XYO_RT_YIELD_EDGE_STACKMAP_PROBE"] = "0"
        os.environ["XYO_RT_YIELD_LIVE_SSA_STACKMAP"] = "0"
        os.environ["XYO_RT_MACHINE_STACKMAP_PROBE"] = "0"
        expected, _ = execute(native, project, "off", None)
        # A combined i64+pointer StackMap has been observed to
        # corrupt actual Scratch Unicode variables on real LLVM23.
        # It must be rejected rather than merely marked "unsafe".
        os.environ["XYO_JIT_LAZY_MODE"] = "off"
        os.environ["XYO_RT_YIELD_EDGE_STACKMAP_PROBE"] = "1"
        os.environ["XYO_RT_YIELD_LIVE_SSA_STACKMAP"] = "1"
        os.environ.pop("XYO_RT_YIELD_SSA_POINTER_LIMIT", None)
        for refused_filter in (None, "all", "invalid", "pointer"):
            if refused_filter is None:
                os.environ.pop("XYO_RT_YIELD_SSA_FILTER", None)
            else:
                os.environ["XYO_RT_YIELD_SSA_FILTER"] = refused_filter
            refused, refused_trace = execute(
                native, project, "eager", None,
                llvm_ssa_liveness=True, detached_llvm_ssa=True,
                lazy_typed_ssa=True, require_generated_ssa_roots=True,
                soft_gc_budget_mb=1,
            )
            reason = (
                "unsafe_pointer_ssa_probe"
                if refused_filter == "pointer"
                else "unsafe_combined_ssa_probe"
            )
            if (refused != expected or
                f"reason={reason}" not in refused_trace or
                SSA.findall(refused_trace)):
                raise AssertionError(
                    f"combined SSA instrumentation filter="
                    f"{refused_filter!r} wasn't fail-closed"
                )

        for family in ("i64",):
            for tier in ("eager", "lazy"):
                path = out / family / tier
                path.mkdir(parents=True, exist_ok=True)
                for obj in path.glob("*.o"):
                    obj.unlink()
                os.environ["XYO_JIT_LAZY_MODE"] = (
                    "on" if tier == "lazy" else "off"
                )
                os.environ["XYO_RT_YIELD_SSA_FILTER"] = family
                os.environ["XYO_JIT_DUMP_OBJECT"] = str(
                    (path / "ssa").resolve()
                )
                actual, trace = execute(
                    native, project, "eager", None,
                    llvm_ssa_liveness=True, detached_llvm_ssa=True,
                    lazy_typed_ssa=True,
                    require_generated_ssa_roots=True,
                    soft_gc_budget_mb=1,
                )
                if actual != expected:
                    raise AssertionError(
                        f"{tier}/{family}: machine SSA probe changes "
                        "Scratch variables"
                    )
                sites, live_i64, pointers = validate_machine(
                    path, trace, tier, args.readobj, args.objdump
                )
                if sites != 4 or (
                    family == "i64" and live_i64 == 0
                ) or (
                    family == "pointer" and pointers == 0
                ):
                    raise AssertionError(
                        f"{tier}/{family}: incomplete machine "
                        "yield SSA census"
                    )
                print(
                    f"PASS RT-001 {tier}/{family}: {sites} SSA machine "
                    f"StackMaps match real ELF location counts; "
                    f"compiler_live_i64={live_i64}, "
                    f"compiler_live_pointer={pointers}; "
                    "Interpreter parity; ownership_proven=0 gc_safe=0",
                    flush=True,
                )
        # Adversarial real GC-provenance case: a prior pointer-only
        # StackMap corrupted a JIT-created Unicode SStr, even though
        # the forged deopt checkpoint itself was correctly refused.
        # Verify that the pointer filter now refuses to instrument,
        # restores full Scratch variable parity, and never adopts it.
        source = fixture(out / "gc_negative", "ssa_new_join_branch_string")
        baseline, _ = execute(native, source, "off", None)
        os.environ["XYO_RT_YIELD_SSA_FILTER"] = "pointer"
        os.environ["XYO_JIT_LAZY_MODE"] = "off"
        observed, diagnostic = execute(
            native, source, "eager", 1,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=True,
            corrupt_detached_llvm_ssa="generated-word-rehash",
            gc_after_adopt=True,
        )
        if (observed != baseline or
            "reason=unsafe_pointer_ssa_probe" not in diagnostic or
            "adopted=0" not in diagnostic or
            "adopted=1" in diagnostic or
            "[native-gc-after-adopt]" in diagnostic):
            raise AssertionError(
                "pointer-only probe was not refused in real "
                "GC-provenance Unicode string adversarial fixture"
            )
        print(
            "PASS RT-001 pointer-only GC-negative regression: "
            "Unicode string parity restored; corrupted SSA deopt "
            "refused; no pointer machine roots or collection granted",
            flush=True,
        )
    finally:
        for key, value in old.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


if __name__ == "__main__":
    main()
