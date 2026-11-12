#!/usr/bin/env python3
"""RT-001 negative contracts for paused Fiber machine SSA spill bounds.

These synthetic records are NOT an independent proof of real native
machine locations; rt_yield_ssa_spill_bounds.py verifies those on
actual ORC ELF and suspended LLVM Fibers.
"""
from __future__ import annotations

from rt_yield_ssa_spill_bounds import (
    check_spills, parse_machine_locations, validate_spill_window,
)

NORMAL = (
    "fiber-a", "2172", "1", "1", "96", "1488", "1048576",
    "0", "0", "0",
)


def rejected(label: str, test) -> None:
    try:
        test()
    except AssertionError:
        return
    raise AssertionError(f"accepted forged machine SSA {label}")


def record(text: str, *, ident: int = 123, count: int = 2) -> str:
    return (
        "LLVM StackMap Version: 3\n"
        f"  Record ID: {ident}, instruction offset: 2145\n"
        f"    {count} locations:\n"
        f"{text}"
        "    0 live-outs: [ ]\n"
    )


def parsed(line: str) -> dict:
    return parse_machine_locations(
        record(
            "      #1: Register R#13, size: 8\n"
            f"      #2: {line}\n"
        ), {123}
    )


def main() -> None:
    valid = parsed("Indirect [R#6 + -64], size: 8")
    assert len(valid[123][1]) == 2
    assert validate_spill_window(NORMAL, -64) == 160
    assert validate_spill_window(NORMAL, -400) == 496
    volatile = parsed("Register R#0, size: 8")
    assert volatile[123][1][1] == ("volatile_unrecoverable", 0, 0)
    records = {
        123: valid[123],
        124: (2145, [("volatile_unrecoverable", 0, 0)]),
    }
    frames = [NORMAL, ("fiber-b",) + NORMAL[1:]]
    assert check_spills(records, {123, 124}, frames) == (2, 1, 1, 1)

    rejected(
        "unreconstructable register base",
        lambda: parsed("Indirect [R#7 + -64], size: 8"),
    )
    rejected(
        "unaligned spill",
        lambda: parsed("Indirect [R#6 + -7], size: 8"),
    )
    rejected(
        "positive frame offset",
        lambda: parsed("Indirect [R#6 + 8], size: 8"),
    )
    rejected(
        "incorrect machine SSA word width",
        lambda: parsed("Indirect [R#6 + -64], size: 4"),
    )
    rejected(
        "constant-as-stack-location",
        lambda: parsed("Constant 1234, size: 8"),
    )
    rejected(
        "direct-pointer-as-i64-home",
        lambda: parsed("Direct [R#6 + -64], size: 8"),
    )
    rejected(
        "duplicate StackMap operand index",
        lambda: parse_machine_locations(
            record(
                "      #1: Register R#13, size: 8\n"
                "      #1: Register R#12, size: 8\n"
            ), {123},
        ),
    )
    rejected(
        "incorrect StackMap operand count",
        lambda: parse_machine_locations(
            record("      #1: Register R#13, size: 8\n"),
            {123},
        ),
    )
    rejected(
        "expired/unmapped frame",
        lambda: validate_spill_window(
            ("fiber-a", "2172", "0") + NORMAL[3:], -64,
        ),
    )
    rejected(
        "GC authorization falsely present",
        lambda: validate_spill_window(
            NORMAL[:-1] + ("1",), -64,
        ),
    )
    rejected(
        "spill beyond suspended stack",
        lambda: validate_spill_window(NORMAL, -2048),
    )
    rejected(
        "spill into guard page",
        lambda: validate_spill_window(NORMAL, -1048576),
    )
    rejected(
        "misordered stopped SP and frame pointer",
        lambda: validate_spill_window(
            NORMAL[:5] + ("32",) + NORMAL[6:], -64,
        ),
    )
    rejected(
        "missing machine record ID",
        lambda: check_spills({123: valid[123]}, {123, 124}, frames),
    )
    rejected(
        "duplicate paused Fiber ID",
        lambda: check_spills(
            records, {123, 124}, [NORMAL, NORMAL],
        ),
    )
    rejected(
        "unproven live managed roots",
        lambda: check_spills(
            records, {123, 124},
            [NORMAL, NORMAL[:-2] + ("1", "0")],
        ),
    )

    print(
        "PASS RT-001 paused SSA machine-home contract: 1 genuine "
        "stack-window example, 1 volatile register correctly marked "
        "UNRECOVERABLE, 16 corrupted machine/CFI/GC claims rejected"
    )


if __name__ == "__main__":
    main()
