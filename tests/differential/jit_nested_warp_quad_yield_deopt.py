#!/usr/bin/env python3
"""OSR-014: four lexical counted repeats and real native phase-4 yield.

root repeat(3) -> warp procedure:
  outer repeat(40_000):
    set times=17
    middle repeat(times-15):       # TWO private iterations
      set times=17
      deep repeat(times-15):       # TWO private iterations
        set times=17
        leaf repeat(times):        # SEVENTEEN private iterations
          change sum by 2
          set times=7             # mutates EVERY active TIMES reporter

After a genuine phase4 native Fiber suspension, edit the leaf increment
destination sum->branchCount. Both source proof and four exact native
private counters are required before dropping the suspended Fiber.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).parent))
from jit_nested_warp_triple_yield_deopt import triple_fixture
from jit_nested_warp_inflight_body_edit import (
    run_mode, PROMOTION, RESTORE, EDIT,
)

QUAD_DEOPT = re.compile(
    r"\[jit-osr-inflight\] deopt quad outer=([0-9.]+) "
    r"middle=([0-9.]+) deep=([0-9.]+) leaf=([0-9.]+) "
    r"source-changed=1"
)


def quad_fixture(path: Path, *, outer_count: int,
                 fractional: bool = False,
                 fifth_layer: bool = False) -> Path:
    triple_fixture(path, outer_count=outer_count,
                   middle_count=2, fractional=fractional)
    with zipfile.ZipFile(path) as z:
        data = {name: z.read(name) for name in z.namelist()}
    project = json.loads(data["project.json"])
    blocks = project["targets"][1]["blocks"]

    # Existing 3-level structure:
    # selected outer -> set times17 -> middle repeat ->
    # set times17 -> leaf repeat -> change sum -> set times7.
    leaf_id = "osr010_nested_fixed_inner"
    leaf = blocks[leaf_id]
    middle_prefix_id = leaf["parent"]
    middle_prefix = blocks[middle_prefix_id]
    middle_id = middle_prefix["parent"]
    assert blocks[middle_id]["opcode"] == "control_repeat"
    assert middle_prefix["next"] == leaf_id
    extra_id = "osr014_deep_repeat"
    extra_prefix_id = "osr014_deep_reset_times"
    extra_times_id = "osr014_deep_times_variable"
    extra_expr_id = "osr014_deep_times_minus15"
    assert not any(name in blocks for name in
                   (extra_id, extra_prefix_id, extra_times_id,
                    extra_expr_id))
    blocks[extra_times_id] = {
        "opcode": "data_variable", "next": None,
        "parent": extra_expr_id, "inputs": {},
        "fields": {"VARIABLE": ["times", "v_times"]},
        "topLevel": False, "shadow": False,
    }
    blocks[extra_expr_id] = {
        "opcode": "operator_subtract", "next": None,
        "parent": extra_id,
        "inputs": {
            "NUM1": [3, extra_times_id, [4, "0"]],
            "NUM2": [1, [4, "15.5" if fractional else "15"]],
        },
        "fields": {}, "topLevel": False, "shadow": False,
    }
    blocks[extra_id] = {
        "opcode": "control_repeat", "next": None,
        "parent": middle_prefix_id,
        "inputs": {
            "TIMES": [3, extra_expr_id, [4, "2"]],
            "SUBSTACK": [2, extra_prefix_id],
        },
        "fields": {}, "topLevel": False, "shadow": False,
    }
    blocks[extra_prefix_id] = {
        "opcode": "data_setvariableto", "next": leaf_id,
        "parent": extra_id,
        "inputs": {"VALUE": [1, [4, "17"]]},
        "fields": {"VARIABLE": ["times", "v_times"]},
        "topLevel": False, "shadow": False,
    }
    middle_prefix["next"] = extra_id
    leaf["parent"] = extra_prefix_id

    if fifth_layer:
        # The 5th counted loop is semantically valid Scratch, but cannot
        # be represented by this explicitly bounded phase4 ABI.
        body_id = leaf["inputs"]["SUBSTACK"][1]
        assert blocks[body_id]["opcode"] == "data_changevariableby"
        fifth_id = "osr014_fifth_refused_repeat"
        assert fifth_id not in blocks
        blocks[fifth_id] = {
            "opcode": "control_repeat", "next": None,
            "parent": leaf_id,
            "inputs": {
                "TIMES": [1, [4, "2"]],
                "SUBSTACK": [2, body_id],
            },
            "fields": {}, "topLevel": False, "shadow": False,
        }
        leaf["inputs"]["SUBSTACK"] = [2, fifth_id]
        blocks[body_id]["parent"] = fifth_id

    data["project.json"] = json.dumps(
        project, ensure_ascii=False, separators=(",", ":")
    ).encode()
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, contents in data.items():
            z.writestr(name, contents)
    return path


def verify(harness: Path, project: Path, *, outer_count: int,
           fractional: bool, counted: bool) -> None:
    leaf_count = 18 if fractional else 17
    total = 3 * outer_count * 2 * 2 * leaf_count * 2
    os.environ["XYO_OSR_EXPECTED_SUM"] = str(total)
    os.environ["XYO_OSR_EXPECTED_TIMES"] = "s:7"
    baseline, _ = run_mode(harness, project, "baseline", count=counted)
    assert baseline["v_sum"] == f"n:{total}", baseline
    assert baseline["v_branchCount"] == "s:0", baseline

    disabled, trace = run_mode(harness, project, "disabled", count=counted)
    assert disabled == baseline, (disabled, baseline)
    assert not PROMOTION.search(trace), trace[-1600:]
    quad_disabled, no_quad_trace = run_mode(
        harness, project, "quad-disabled", count=counted
    )
    assert quad_disabled == baseline, (quad_disabled, baseline)
    assert not PROMOTION.search(no_quad_trace), (
        "four-depth native OSR accepted with QUAD opt-in disabled",
        no_quad_trace[-1600:],
    )
    print("PASS QUAD opt-in OFF with earlier triple/dynamic flags ON: "
          "no native promotion", flush=True)

    positive, trace = run_mode(harness, project, "positive", count=counted)
    assert positive == baseline, (positive, baseline)
    promotions, restores = (
        len(PROMOTION.findall(trace)), len(RESTORE.findall(trace))
    )
    assert 1 <= promotions <= 3 and restores == promotions, (
        promotions, restores, trace[-2800:]
    )
    assert "yielding inner repeat accepted" in trace, trace[-2800:]
    print(f"PASS positive: native={promotions}/{restores}, "
          f"total={total}, counted={counted}, "
          f"blocks={baseline.get('__block_count', 'disabled')}", flush=True)

    for mode in ("guarded", "guarded-late"):
        state, trace = run_mode(harness, project, mode, count=counted)
        edits = EDIT.findall(trace)
        assert len(edits) == 1, (mode, edits, trace[-3200:])
        leaf_remaining, flag, committed, _, observed = edits[0]
        assert flag == "1", (mode, edits)
        assert int(observed) >= (4 if mode.endswith("-late") else 1)
        assert 0 < float(committed) < total
        checkpoints = QUAD_DEOPT.findall(trace)
        assert len(checkpoints) == 1, (mode, trace[-3600:])
        outer, middle, deep, leaf = map(float, checkpoints[0])
        assert 0 <= outer < outer_count, (outer, outer_count)
        assert 0 <= middle < 2, middle
        assert 0 <= deep < 2, deep
        assert 0 <= leaf < leaf_count, leaf
        assert leaf == float(leaf_remaining)
        promotions, restores = (
            len(PROMOTION.findall(trace)), len(RESTORE.findall(trace))
        )
        assert 1 <= promotions <= 3 and restores == promotions-1, (
            mode, promotions, restores, trace[-2800:]
        )
        sum_value = int(state["v_sum"][2:])
        branch = int(state["v_branchCount"][2:])
        assert sum_value == int(float(committed)), (
            mode, sum_value, committed
        )
        assert branch == total-sum_value, (mode, branch, total, sum_value)
        for key, value in baseline.items():
            if key not in ("v_sum", "v_branchCount"):
                assert state[key] == value, (
                    mode, key, state[key], value
                )
        print(f"PASS {mode}: native={promotions} deopt-phase4=1 "
              f"counts={checkpoints[0]}, sum={sum_value} "
              f"redirected={branch}", flush=True)

    # The 5th layer may be executed by Interpreter but must NOT be
    # passed to an OSR backend with an insufficient source-state schema.
    small_outer = 200
    fifth = quad_fixture(
        project.parent / "fifth-repeat-rejected.sb3",
        outer_count=small_outer, fractional=False, fifth_layer=True,
    )
    os.environ["XYO_OSR_EXPECTED_SUM"] = str(
        3 * small_outer * 2 * 2 * 17 * 2 * 2
    )
    expected, _ = run_mode(harness, fifth, "baseline", count=counted)
    actual, trace = run_mode(harness, fifth, "rejected", count=counted)
    assert actual == expected, (actual, expected)
    assert not PROMOTION.search(trace), trace[-2900:]
    print("PASS 5th nested counted repeat rejected; exact Interpreter "
          "parity", flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--harness", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--outer-count", type=int, default=40_000)
    p.add_argument("--fractional", action="store_true")
    p.add_argument("--without-block-count", action="store_true")
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    project = quad_fixture(
        a.out / ("four-layer-fractional.sb3" if a.fractional
                 else "four-layer-integer.sb3"),
        outer_count=a.outer_count, fractional=a.fractional,
    )
    verify(a.harness, project, outer_count=a.outer_count,
           fractional=a.fractional, counted=not a.without_block_count)


if __name__ == "__main__":
    main()
