#!/usr/bin/env python3
"""OSR-013: genuine LLVM phase-3 yield within three lexical counted repeats.

root repeat(3) -> warp procedure:
    repeat(outer_count):
      set times=17
      repeat(times - 15):     // private middle count must remain TWO
        set times=17
        repeat(times):        // private leaf count must remain SEVENTEEN
          change sum by 2
          set times=7         // mutate both reporter inputs mid-loop
      set scratch=...        // after middle source suffix

After live phase-3 native Fiber suspension, change the leaf increment
target to branchCount. Assert that Interpreter resumes the EXACT three
source repeat counters and original caller/GC state without replay.
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
from jit_nested_warp_dynamic_inner_deopt import dynamic_fixture
from jit_nested_warp_inflight_body_edit import (
    run_mode, PROMOTION, RESTORE, EDIT,
)

TRIPLE_DEOPT = re.compile(
    r"\[jit-osr-inflight\] deopt triple outer=([0-9.]+) "
    r"middle=([0-9.]+) leaf=([0-9.]+) source-changed=1"
)

def triple_fixture(path: Path, *, outer_count: int,
                   middle_count: int = 2,
                   fractional: bool = False) -> Path:
    assert 2 <= middle_count <= 8
    dynamic_fixture(path, outer_count=outer_count,
                    kind="add-half" if fractional else "variable",
                    reset_each_outer=True)
    with zipfile.ZipFile(path) as archive:
        data = {name: archive.read(name) for name in archive.namelist()}
    project = json.loads(data["project.json"])
    blocks = project["targets"][1]["blocks"]
    leaf_id = "osr010_nested_fixed_inner"
    # The OSR-012 source has a set-times prefix to the selected outer
    # repeat; replace its successor by a new counted repeat.
    prefix_id = blocks[leaf_id]["parent"]
    prefix = blocks[prefix_id]
    outer = blocks[prefix["parent"]]
    assert outer["opcode"] == "control_repeat"
    assert prefix["opcode"] == "data_setvariableto"
    assert prefix["next"] == leaf_id
    leaf = blocks[leaf_id]
    after_id = leaf["next"]
    assert after_id
    mid_id = "osr013_middle_repeat"
    mid_prefix = "osr013_middle_reset_times"
    mid_var = "osr013_middle_times_var"
    mid_expr = "osr013_middle_times_subtraction"
    assert all(k not in blocks for k in
               (mid_id, mid_prefix, mid_var, mid_expr))
    blocks[mid_var] = {
        "opcode": "data_variable", "next": None, "parent": mid_expr,
        "inputs": {}, "fields": {"VARIABLE": ["times", "v_times"]},
        "topLevel": False, "shadow": False,
    }
    blocks[mid_expr] = {
        "opcode": "operator_subtract", "next": None, "parent": mid_id,
        "inputs": {
            "NUM1": [3, mid_var, [4, "0"]],
            "NUM2": [1, [4, str(
                17 - middle_count + (0.5 if fractional else 0)
            )]],
        },
        "fields": {}, "topLevel": False, "shadow": False,
    }
    blocks[mid_id] = {
        "opcode": "control_repeat", "next": after_id, "parent": prefix_id,
        "inputs": {
            "TIMES": [3, mid_expr, [4, str(middle_count)]],
            "SUBSTACK": [2, mid_prefix],
        },
        "fields": {}, "topLevel": False, "shadow": False,
    }
    blocks[mid_prefix] = {
        "opcode": "data_setvariableto", "next": leaf_id,
        "parent": mid_id,
        "inputs": {"VALUE": [1, [4, "17"]]},
        "fields": {"VARIABLE": ["times", "v_times"]},
        "topLevel": False, "shadow": False,
    }
    prefix["next"] = mid_id
    leaf["parent"] = mid_prefix
    leaf["next"] = None
    blocks[after_id]["parent"] = mid_id
    data["project.json"] = json.dumps(
        project, ensure_ascii=False, separators=(",", ":")
    ).encode()
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, content in data.items():
            archive.writestr(name, content)
    return path


def four_deep_fixture(path: Path, *, outer_count: int) -> Path:
    """Four lexical counted repeats MUST be rejected, even if pure."""
    triple_fixture(path, outer_count=outer_count)
    with zipfile.ZipFile(path) as archive:
        data = {name: archive.read(name) for name in archive.namelist()}
    project = json.loads(data["project.json"])
    blocks = project["targets"][1]["blocks"]
    leaf_id = "osr010_nested_fixed_inner"
    leaf = blocks[leaf_id]
    first = leaf["inputs"]["SUBSTACK"][1]
    assert blocks[first]["opcode"] == "data_changevariableby"
    deeper = "osr013_fourth_repeat"
    assert deeper not in blocks
    blocks[deeper] = {
        "opcode": "control_repeat",
        "next": None, "parent": leaf_id,
        "inputs": {
            "TIMES": [1, [4, "2"]],
            "SUBSTACK": [2, first],
        },
        "fields": {}, "topLevel": False, "shadow": False,
    }
    leaf["inputs"]["SUBSTACK"] = [2, deeper]
    blocks[first]["parent"] = deeper
    data["project.json"] = json.dumps(
        project, ensure_ascii=False, separators=(",", ":")
    ).encode()
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, content in data.items():
            archive.writestr(name, content)
    return path


def verify(harness: Path, project: Path, *,
           outer_count: int, middle_count: int,
           block_count: bool, fractional: bool = False):
    leaf_count = 18 if fractional else 17
    total = 3 * outer_count * middle_count * leaf_count * 2
    os.environ["XYO_OSR_EXPECTED_SUM"] = str(total)
    os.environ["XYO_OSR_EXPECTED_TIMES"] = "s:7"
    baseline, _ = run_mode(
        harness, project, "baseline", count=block_count
    )
    assert baseline["v_sum"] == f"n:{total}", baseline
    assert baseline["v_branchCount"] == "s:0", baseline
    disabled, trace = run_mode(
        harness, project, "disabled", count=block_count
    )
    assert disabled == baseline, (disabled, baseline)
    assert not PROMOTION.search(trace), trace[-3200:]

    state, trace = run_mode(harness, project, "positive",
                            count=block_count)
    assert state == baseline, (state, baseline)
    promos = len(PROMOTION.findall(trace))
    restores = len(RESTORE.findall(trace))
    assert 1 <= promos <= 3 and promos == restores, (
        promos, restores, trace[-3000:]
    )
    assert "yielding inner repeat accepted" in trace, trace[-3000:]
    print(f"PASS positive: native={promos}/{restores}, "
          f"count={'on' if block_count else 'off'}, sum={total}",
          flush=True)

    for mode in ("guarded", "guarded-late"):
        state, trace = run_mode(
            harness, project, mode, count=block_count
        )
        edited = EDIT.findall(trace)
        assert len(edited) == 1, (mode, trace[-3000:])
        leaf_remaining, flag, committed, at_count, yields = edited[0]
        assert flag == "1"
        assert int(yields) >= (4 if mode.endswith("-late") else 1)
        assert 0 < float(committed) < total
        assert float(leaf_remaining).is_integer(), (
            mode, leaf_remaining
        )
        assert 0 <= float(leaf_remaining) < leaf_count, (
            mode, leaf_remaining, trace[-3200:]
        )
        triples = TRIPLE_DEOPT.findall(trace)
        assert len(triples) == 1, (mode, trace[-3000:])
        outer, middle, leaf = map(float, triples[0])
        assert 0 <= outer < outer_count
        assert 0 <= middle < middle_count
        assert 0 <= leaf < leaf_count
        assert leaf == float(leaf_remaining)
        promos = len(PROMOTION.findall(trace))
        restores = len(RESTORE.findall(trace))
        assert 1 <= promos <= 3 and restores == promos-1, (
            mode, promos, restores, trace[-3200:]
        )
        sum_value = int(state["v_sum"][2:])
        redirected = int(state["v_branchCount"][2:])
        assert sum_value == int(float(committed)), (
            mode, committed, state
        )
        assert redirected == total - sum_value, (mode, state)
        assert sum_value + redirected == total
        for key, value in baseline.items():
            if key not in ("v_sum", "v_branchCount"):
                assert state[key] == value, (
                    mode, key, state[key], value
                )
        print(f"PASS {mode}: native={promos}, triple-deopt=1 "
              f"source-counters={triples[0]} sum={sum_value}, "
              f"redirected={redirected}, "
              f"blocks={state.get('__block_count', 'disabled')}",
              flush=True)
    return total, baseline


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--harness", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--outer-count", type=int, default=100_000)
    p.add_argument("--middle-count", type=int, default=2)
    p.add_argument("--without-block-count", action="store_true")
    p.add_argument("--fractional", action="store_true")
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    project = triple_fixture(
        a.out / ("native-triple-fractional-repeat.sb3"
                 if a.fractional else "native-triple-dynamic-repeat.sb3"),
        outer_count=a.outer_count, middle_count=a.middle_count,
        fractional=a.fractional,
    )
    verify(a.harness, project,
           outer_count=a.outer_count, middle_count=a.middle_count,
           block_count=not a.without_block_count,
           fractional=a.fractional)
    # Negative: a fourth lexical repeat is Interpreter-executable but
    # cannot be represented in the current three-counter ABI.
    bad_outer = 1000
    rejected = four_deep_fixture(
        a.out / "four-deep-source-rejected.sb3",
        outer_count=bad_outer,
    )
    os.environ["XYO_OSR_EXPECTED_SUM"] = str(
        3 * bad_outer * 2 * 17 * 2 * 2
    )
    os.environ["XYO_OSR_EXPECTED_TIMES"] = "s:7"
    count = not a.without_block_count
    reference, _ = run_mode(
        a.harness, rejected, "baseline", count=count
    )
    denied, trace = run_mode(
        a.harness, rejected, "rejected", count=count
    )
    assert denied == reference, (denied, reference)
    assert not PROMOTION.search(trace), trace[-2500:]
    print("PASS four lexical counted repeats rejected; exact "
          "Interpreter parity", flush=True)


if __name__ == "__main__":
    main()
