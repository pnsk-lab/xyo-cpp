#!/usr/bin/env python3
"""OSR-011: real native LLVM yield *inside* a nested Scratch repeat.

The lexical source has root repeat(3) -> warp procedure -> outer
repeat(120k) -> inner repeat(128) -> variable increment, with a string
suffix after the inner control statement. After an actual phase-2 Fiber
suspension, a live source edit redirects inner increment sum->branchCount.
The interpreter must inherit BOTH already-decremented counters without
replaying completed inner effects, outer prefix, or caller suffix.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).parent))
from jit_nested_warp_bounded_inner_deopt import nested_fixture
from jit_nested_warp_inflight_body_edit import run_mode, PROMOTION, RESTORE, EDIT

INNER_DEOPT = re.compile(
    r"\[jit-osr-inflight\] deopt inner outer=([0-9.]+) "
    r"inner=([0-9.]+) source-changed=1"
)


def verify(harness: Path, project: Path, *, outer_count: int,
           inner_count: int, block_count: bool):
    total = 3 * outer_count * inner_count * 2
    os.environ["XYO_OSR_EXPECTED_SUM"] = str(total)
    baseline, _ = run_mode(
        harness, project, "baseline", count=block_count
    )
    assert baseline["v_sum"] == f"n:{total}", baseline
    assert baseline["v_branchCount"] == "s:0", baseline
    expected_blocks = baseline.get("__block_count")

    disabled, disabled_trace = run_mode(
        harness, project, "disabled", count=block_count
    )
    assert disabled == baseline, (disabled, baseline)
    assert not PROMOTION.search(disabled_trace), disabled_trace[-1800:]

    positive, trace = run_mode(
        harness, project, "positive", count=block_count
    )
    assert positive == baseline, (positive, baseline)
    promotions = len(PROMOTION.findall(trace))
    assert promotions >= 1, trace[-3200:]
    assert len(RESTORE.findall(trace)) == promotions, trace[-3200:]
    assert "yielding inner repeat accepted" in trace, trace[-3200:]
    print(f"PASS phase2 positive: LLVM promotions/restores {promotions}/{promotions} "
          f"count={'on' if block_count else 'off'}", flush=True)

    # Without the provenance guard LLVM is allowed to specialize/batch
    # the inner repeat and may have no observable native Fiber yield.
    # Requiring one would accidentally constrain the default optimizer.
    # The negative stale-LLVM comparison is already covered by OSR-009.
    for mode in ("guarded", "guarded-late"):
        state, trace = run_mode(
            harness, project, mode, count=block_count
        )
        edits = EDIT.findall(trace)
        assert len(edits) == 1, (mode, edits, trace[-2400:])
        inner_remaining, flag, committed, at_count, yields = edits[0]
        guarded = mode.startswith("guarded")
        assert int(flag) == int(guarded), (mode, edits)
        assert int(yields) >= (4 if mode.endswith("-late") else 1)
        assert 0 < float(committed) < total
        promotes, restores = (
            len(PROMOTION.findall(trace)), len(RESTORE.findall(trace))
        )
        assert 1 <= promotes <= 3, (mode, promotes, trace[-2500:])
        assert restores == promotes - int(guarded), (
            mode, promotes, restores, trace[-2500:]
        )
        actual_deopts = INNER_DEOPT.findall(trace)
        assert len(actual_deopts) == int(guarded), (
            mode, actual_deopts, trace[-2800:]
        )
        if guarded:
            outer, inner = actual_deopts[0]
            assert 0 <= float(outer) <= outer_count
            assert 0 <= float(inner) < inner_count
        sum_value = int(state["v_sum"][2:])
        branch = int(state["v_branchCount"][2:])
        assert sum_value + branch == total, (mode, state)
        if guarded:
            assert sum_value == int(float(committed)), (
                mode, sum_value, committed, trace[-2600:]
            )
            assert branch == total - sum_value
        else:
            assert sum_value > int(float(committed)), (
                mode, sum_value, committed, trace[-2600:]
            )
        for key, value in baseline.items():
            if key not in ("v_sum", "v_branchCount"):
                assert state[key] == value, (
                    mode, key, state[key], value
                )
        if block_count:
            assert state["__block_count"] == expected_blocks, (
                mode, expected_blocks, state["__block_count"]
            )
        print(f"PASS {mode}: native={promotes}, "
              f"inner-source-deopt={int(guarded)}, "
              f"sum={sum_value}, edited={branch}, "
              f"blocks={state.get('__block_count','disabled')}",
              flush=True)

    # Source-proven inner loop count >8192 is deliberately rejected.
    # Small outer count keeps this rejection fixture inexpensive.
    print("PASS inner phase-2 source edit/counter continuity "
          "and negative guard controls", flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--harness", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    # Async LLVM compilation can complete after a short 40k-iteration
    # outer loop on a busy shared machine. Give every parent invocation
    # enough interpreted work to guarantee an actual native promotion,
    # instead of sporadically skipping the phase-2 yield entirely.
    p.add_argument("--outer-count", type=int, default=120_000)
    p.add_argument("--inner-count", type=int, default=128)
    p.add_argument("--without-block-count", action="store_true")
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    project = nested_fixture(
        a.out / "real-native-inner-yield.sb3",
        outer_count=a.outer_count, inner_count=a.inner_count,
    )
    verify(a.harness, project, outer_count=a.outer_count,
           inner_count=a.inner_count,
           block_count=not a.without_block_count)
    if a.inner_count <= 8192:
        rejected = nested_fixture(
            a.out / "rejected-inner-yield-9000.sb3",
            outer_count=32, inner_count=9000,
        )
        os.environ["XYO_OSR_EXPECTED_SUM"] = str(3 * 32 * 9000 * 2)
        count = not a.without_block_count
        before, _ = run_mode(
            a.harness, rejected, "baseline", count=count
        )
        after, trace = run_mode(
            a.harness, rejected, "rejected", count=count
        )
        assert after == before, (after, before)
        assert not PROMOTION.search(trace), trace[-2500:]
        assert "yielding inner repeat accepted" not in trace
        print("PASS inner count=9000 rejected, full Interpreter parity",
              flush=True)


if __name__ == "__main__":
    main()
