#!/usr/bin/env python3
"""OSR-016: independently test both sides of Scratch repeat's .5 boundary.

Reuse OSR-013's real three-level native Fiber yield and source-edit harness.
The two dynamic TIMES reporters intentionally change their input variable
inside each active loop: restoring from re-evaluated reporters is incorrect.

All four combinations are tested independently; no production ABI changes.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import zipfile

from jit_nested_warp_inflight_body_edit import EDIT, PROMOTION, RESTORE, run_mode
from jit_nested_warp_triple_yield_deopt import TRIPLE_DEOPT, triple_fixture


CASES = {
    "both-below": (False, False),
    "middle-tie": (True, False),
    "leaf-tie": (False, True),
    "both-tie": (True, True),
}


def make_fixture(path: Path, *, outer_count: int,
                 middle_tie: bool, leaf_tie: bool) -> Path:
    triple_fixture(path, outer_count=outer_count, fractional=True)
    with zipfile.ZipFile(path) as archive:
        data = {name: archive.read(name) for name in archive.namelist()}

    project = json.loads(data["project.json"])
    blocks = project["targets"][1]["blocks"]
    middle = blocks["osr013_middle_times_subtraction"]
    leaf = blocks["osr012_times_binary"]
    assert middle["opcode"] == "operator_subtract"
    assert leaf["opcode"] == "operator_add"

    # 17 - 14.500000000001 = 2.499999999999 -> 2
    # 17 - 14.5            = 2.5            -> 3
    # 17 + 0.499999999999  = 17.499999999999 -> 17
    # 17 + 0.5             = 17.5           -> 18
    # Values are sufficiently apart at binary64 precision.
    middle["inputs"]["NUM2"][1][1] = (
        "14.5" if middle_tie else "14.500000000001"
    )
    leaf["inputs"]["NUM2"][1][1] = (
        "0.5" if leaf_tie else "0.499999999999"
    )
    raw_middle = 17.0 - float(middle["inputs"]["NUM2"][1][1])
    raw_leaf = 17.0 + float(leaf["inputs"]["NUM2"][1][1])
    assert math.floor(raw_middle + 0.5) == (3 if middle_tie else 2)
    assert math.floor(raw_leaf + 0.5) == (18 if leaf_tie else 17)

    data["project.json"] = json.dumps(
        project, ensure_ascii=False, separators=(",", ":")
    ).encode()
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, content in data.items():
            archive.writestr(name, content)
    return path


def verify(harness: Path, project: Path, *, outer_count: int,
           middle_tie: bool, leaf_tie: bool, count: bool,
           late: bool) -> None:
    middle_trip = 3 if middle_tie else 2
    leaf_trip = 18 if leaf_tie else 17
    expected_sum = 3 * outer_count * middle_trip * leaf_trip * 2
    os.environ["XYO_OSR_EXPECTED_SUM"] = str(expected_sum)
    os.environ["XYO_OSR_EXPECTED_TIMES"] = "s:7"

    baseline, _ = run_mode(harness, project, "baseline", count=count)
    assert baseline["v_sum"] == f"n:{expected_sum}", baseline
    assert baseline["v_branchCount"] == "s:0", baseline

    disabled, trace = run_mode(harness, project, "disabled", count=count)
    assert disabled == baseline, ("disabled", disabled, baseline)
    assert not PROMOTION.search(trace), trace[-3000:]

    positive, trace = run_mode(harness, project, "positive", count=count)
    assert positive == baseline, ("positive", positive, baseline)
    promoted = len(PROMOTION.findall(trace))
    assert 1 <= promoted <= 3, trace[-3000:]
    assert len(RESTORE.findall(trace)) == promoted, trace[-3000:]
    assert "yielding inner repeat accepted" in trace, trace[-3000:]

    modes = ("guarded", "guarded-late") if late else ("guarded",)
    for mode in modes:
        actual, trace = run_mode(harness, project, mode, count=count)
        edits = EDIT.findall(trace)
        assert len(edits) == 1, (mode, trace[-3200:])
        remaining, guarded, committed, at_count, yields = edits[0]
        assert guarded == "1"
        assert int(yields) >= (4 if mode.endswith("-late") else 1)
        assert float(remaining).is_integer()
        assert 0 <= float(remaining) < leaf_trip
        assert float(committed).is_integer()
        sum_at_edit = int(float(committed))
        assert 0 < sum_at_edit < expected_sum
        if count:
            assert 0 <= int(at_count) < int(baseline["__block_count"][2:])

        deopts = TRIPLE_DEOPT.findall(trace)
        assert len(deopts) == 1, (mode, trace[-3200:])
        outer, middle, leaf = map(float, deopts[0])
        assert all(v.is_integer() for v in (outer, middle, leaf))
        assert 0 <= outer < outer_count
        assert 0 <= middle < middle_trip
        assert 0 <= leaf < leaf_trip
        assert leaf == float(remaining)

        promotions = len(PROMOTION.findall(trace))
        restorations = len(RESTORE.findall(trace))
        assert 1 <= promotions <= 3
        assert restorations == promotions - 1
        assert actual["v_sum"] == f"n:{sum_at_edit}"
        assert actual["v_branchCount"] == f"n:{expected_sum - sum_at_edit}"
        for key, expected in baseline.items():
            if key not in ("v_sum", "v_branchCount"):
                assert actual[key] == expected, (mode, key, actual[key], expected)
        print(
            f"PASS {mode}: source counters={deopts[0]}, "
            f"sum={sum_at_edit}, redirected={expected_sum - sum_at_edit}",
            flush=True,
        )

    print(
        f"PASS middle={middle_trip} leaf={leaf_trip} "
        f"count={'on' if count else 'off'}: "
        f"expected-sum={expected_sum}, native-promotions={promoted}",
        flush=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--outer-count", type=int, default=100_000)
    parser.add_argument("--case", choices=CASES, default="both-below")
    parser.add_argument("--all-cases", action="store_true")
    parser.add_argument("--without-block-count", action="store_true")
    parser.add_argument("--late", action="store_true",
                        help="also inject the source edit at the fourth native yield")
    args = parser.parse_args()
    assert args.outer_count >= 1000
    args.out.mkdir(parents=True, exist_ok=True)
    cases = CASES.items() if args.all_cases else [(args.case, CASES[args.case])]
    for name, (middle_tie, leaf_tie) in cases:
        fixture = make_fixture(
            args.out / f"fractional-boundary-{name}.sb3",
            outer_count=args.outer_count,
            middle_tie=middle_tie, leaf_tie=leaf_tie,
        )
        verify(args.harness, fixture, outer_count=args.outer_count,
               middle_tie=middle_tie, leaf_tie=leaf_tie,
               count=not args.without_block_count, late=args.late)


if __name__ == "__main__":
    main()
