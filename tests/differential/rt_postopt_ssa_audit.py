#!/usr/bin/env python3
"""RT-001: audit ACTUAL LLVM O3 SSA captures under eager and lazy ORC.

Do not compare raw pre-/post-opt live-pointer counts as if they described
the same IR: emitted capture allocas and helper boundaries change them.
"""
from __future__ import annotations

import argparse
from collections import Counter
import os
from pathlib import Path
import re

from native_simple_resume import execute, fixture

SITE = re.compile(
    r"^\[rt001-postopt-ssa\] tier=(eager|lazy) fn=(\S+) site=(\d+) "
    r"pre_unresolved=(\d+) captured=(\d+) "
    r"post_live_ptr=(\d+) post_unknown=(\d+) "
    r"post_stack=(\d+) post_host=(\d+) post_static=(\d+) "
    r"post_numeric=(\d+) post_other=(\d+) valid=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)
TOTAL = re.compile(
    r"^\[rt001-postopt-ssa-total\] tier=(eager|lazy) sites=(\d+) "
    r"post_live_ptr=(\d+) post_unknown=(\d+) invalid=(\d+) "
    r"machine_roots_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    project = fixture(args.out, "ssa_budget_cross_fiber_two_hats")
    expected, _ = execute(args.native.resolve(), project, "off", None)
    original_mode = os.environ.get("XYO_JIT_LAZY_MODE")
    original_audit = os.environ.get("XYO_RT_POSTOPT_SSA_AUDIT")
    try:
        os.environ["XYO_RT_POSTOPT_SSA_AUDIT"] = "1"
        for tier in ("eager", "lazy"):
            os.environ["XYO_JIT_LAZY_MODE"] = (
                "off" if tier == "eager" else "on"
            )
            observed, trace = execute(
                args.native.resolve(), project, "eager", None,
                llvm_ssa_liveness=True,
                detached_llvm_ssa=True,
                trace_detached_llvm_ssa=True,
                lazy_typed_ssa=True,
                require_generated_ssa_roots=True,
                soft_gc_budget_mb=1,
            )
            if observed != expected:
                raise AssertionError(f"{tier}: Interpreter parity failed")
            entries = SITE.findall(trace)
            # Two native procedures, each with two distinct compiler
            # checkpoint site IDs. All post-opt pointers are UNROOTED
            # until a separate machine stack-map proof exists.
            if len(entries) != 4:
                raise AssertionError(
                    f"{tier}: expected 4 postopt sites; got "
                    f"{len(entries)}\n{trace[-4000:]}"
                )
            site_counts = Counter()
            for (
                measured_tier, fn, site, pre, captured,
                live, unknown, stack, host, static, numeric, other,
                valid, safe,
            ) in entries:
                if (
                    measured_tier != tier or
                    int(site) not in (1, 2) or
                    int(pre) < 1 or
                    int(captured) < 1 or
                    int(live) < 1 or
                    int(unknown) < 1 or
                    int(stack) + int(host) +
                    int(static) + int(unknown) != int(live) or
                    valid != "1" or safe != "0"
                ):
                    raise AssertionError(f"{tier}: unsafe SSA record: {fn}")
                site_counts[(fn, int(site))] += 1
            if (
                len({fn for fn, _ in site_counts}) != 2 or
                len(site_counts) != 4 or
                any(count != 1 for count in site_counts.values())
            ):
                raise AssertionError(
                    f"{tier}: missing or duplicated site IDs: {site_counts}"
                )
            totals = TOTAL.findall(trace)
            relevant = [row for row in totals
                        if row[0] == tier and int(row[1]) > 0]
            if len(relevant) != 2 or any(
                row[1] != "2" or row[5] != "0" or row[6] != "0"
                for row in relevant
            ):
                raise AssertionError(
                    f"{tier}: missing fail-closed totals: {relevant}"
                )
            print(
                f"PASS RT-001 {tier} post-O3: 4 capture sites, "
                f"unrooted live SSA pointers, strict machine "
                f"GC veto, Interpreter parity"
            )
    finally:
        for name, old in (
            ("XYO_JIT_LAZY_MODE", original_mode),
            ("XYO_RT_POSTOPT_SSA_AUDIT", original_audit),
        ):
            if old is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = old


if __name__ == "__main__":
    main()
