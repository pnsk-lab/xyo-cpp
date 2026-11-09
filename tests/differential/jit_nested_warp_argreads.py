#!/usr/bin/env python3
"""Experimental read-only procedure arguments in a native nested warp loop.

The source selector checks exact declared reporter names; the generated
standalone loop delegates these reporters to the interpreter via the normal
xyo_rt_reporter trampoline while preserving the original caller frame.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from jit_nested_warp_args_preserved import execute, fixture


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    source = args.out / "native-warp-argument-reporters.sb3"
    _, expected = fixture(
        source, use_argument_in_loop=True, iterations_override=20_000_000
    )
    baseline, _ = execute(args.native, source, "off", False)
    assert all(baseline[k] == v for k, v in expected.items()), (
        baseline, expected
    )
    for tier, nested, arg_reads, direct_args in (
        ("hotspot", True, False, False),
        ("hotspot", True, True, False),
        ("hotspot", True, True, True),
        ("eager", True, True, True),
    ):
        actual, diagnostics = execute(
            args.native, source, tier, nested,
            arg_reads=arg_reads, direct_args=direct_args,
        )
        assert actual == baseline, (
            tier, arg_reads, actual, baseline, diagnostics[-1500:]
        )
        promoted = "[jit-osr-nested] promoted " in diagnostics
        if tier == "hotspot" and arg_reads:
            assert promoted, (
                "native arg reads OSR did not trigger", diagnostics[-1500:]
            )
            assert "[jit-osr-nested] restored caller" in diagnostics
        else:
            assert not promoted, (
                "Unexpected OSR", tier, arg_reads, diagnostics[-1500:]
            )
        print(
            f"PASS {tier} arg_reads={arg_reads} promoted={promoted}: "
            "value/GC/prefix/suffix match",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
