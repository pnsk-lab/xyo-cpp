#!/usr/bin/env python3
"""RT-001: adversarial negatives for paused native Fiber JitCtx register.

Test-only mutation of CFI/ELF witness tuples; real native samples are
verified separately. This checker must never authorize any GC sweep.
"""
from __future__ import annotations

from rt_yield_edge_fiber_pc import validate_register_witnesses


def modified(row: tuple[str, ...], index: int, value: str
             ) -> tuple[str, ...]:
    seq = list(row)
    seq[index] = value
    return tuple(seq)


def main() -> None:
    a = (
        "a", "1905", "1", "1", "1", "1", "0", "1", "0",
        "1", "0", "1", "0", "0",
    )
    b = ("b",) + a[1:]
    live = [("a",), ("b",)]
    witnesses = [(1905, 13), (1905, 13)]
    validate_register_witnesses([a, b], live, witnesses)

    # The same code can be compiled with a different callee-saved
    # DWARF register. That works ONLY when the exact CFI value matches.
    r14_a = modified(modified(a, 4, "0"), 6, "1")
    r14_b = modified(modified(b, 4, "0"), 6, "1")
    validate_register_witnesses(
        [r14_a, r14_b], live, [(1905, 14), (1905, 14)]
    )

    def rejects(label: str, rows: list[tuple[str, ...]],
                locations: list[tuple[int, int]],
                frames: list[tuple[str, ...]] = live) -> None:
        try:
            validate_register_witnesses(rows, frames, locations)
        except AssertionError:
            return
        raise AssertionError(f"accepted forged {label} GC precursor")

    rejects("missing Fiber", [a], witnesses)
    rejects("duplicate Fiber", [a, a], witnesses)
    rejects("foreign Fiber", [a, modified(b, 0, "other")],
            witnesses)
    rejects("wrong JIT return PC", [modified(a, 1, "1906"), b],
            witnesses)
    rejects("ambiguous DWARF registers", [a, b],
            [(1905, 13), (1905, 14)])
    rejects("caller-saved volatile register", [a, b],
            [(1905, 5), (1905, 5)])
    rejects("forged JitCtx pointer", [modified(a, 4, "0"), b],
            witnesses)
    rejects("unavailable JitCtx", [modified(a, 2, "0"), b],
            witnesses)
    rejects("CFI register unavailable", [modified(a, 3, "0"), b],
            witnesses)
    rejects("synthetic or missing genuine FDE",
            [modified(a, 11, "0"), b], witnesses)
    rejects("invalid machine roots permission",
            [modified(a, 12, "1"), b], witnesses)
    rejects("invalid GC permission",
            [modified(a, 13, "1"), b], witnesses)

    print(
        "PASS RT-001 CFI JitCtx register contract: 2 valid callee-saved "
        "register configurations and 12 forged cases correctly refused; "
        "no GC authority"
    )


if __name__ == "__main__":
    main()
