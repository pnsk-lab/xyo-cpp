#!/usr/bin/env python3
"""OSR-010: a bounded *nested* repeat completes before outer native warp yield.

After genuine LLVM Fiber suspension mutate the inner repeat's real
change-variable target; prove that the outer-loop saved counter transfers
to Interpreter with no duplicate nested-loop effects. Test block-count
on and off, first and fourth native yield, unguarded stale-code controls.
"""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import re
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).parent))
from jit_nested_warp_inflight_body_edit import (
    run_mode, PROMOTION, RESTORE, DEOPT, EDIT,
)
from jit_nested_warp_parent_repeat_osr import fixture as parent_fixture


def nested_fixture(path: Path, *, outer_count: int, inner_count: int):
    parent_fixture(
        path, parent_count=3, inner_iterations=outer_count, gc=True
    )
    with zipfile.ZipFile(path) as archive:
        payload = {name: archive.read(name)
                   for name in archive.namelist()}
    project = json.loads(payload["project.json"])
    blocks = project["targets"][1]["blocks"]
    candidates = [
        (key, block) for key, block in blocks.items()
        if block["opcode"] == "data_changevariableby" and
        block.get("fields", {}).get("VARIABLE", [None])[0] == "sum"
    ]
    assert len(candidates) == 1, candidates
    change_id, change = candidates[0]
    outer_id = change["parent"]
    outer = blocks[outer_id]
    assert outer["opcode"] == "control_repeat", outer
    assert outer["inputs"]["SUBSTACK"][1] == change_id
    after_id = change["next"]
    assert blocks[after_id]["opcode"] == "data_setvariableto"
    assert blocks[after_id]["fields"]["VARIABLE"][0] == "scratch"

    inner_id = "osr010_nested_fixed_inner"
    assert inner_id not in blocks
    inner = copy.deepcopy(outer)
    inner["opcode"] = "control_repeat"
    inner["parent"] = outer_id
    inner["next"] = after_id
    inner["topLevel"] = False
    inner["shadow"] = False
    inner["inputs"] = {
        "TIMES": [1, [4, str(inner_count)]],
        "SUBSTACK": [2, change_id],
    }
    inner["fields"] = {}
    blocks[inner_id] = inner
    outer["inputs"]["SUBSTACK"] = [2, inner_id]
    change["parent"] = inner_id
    change["next"] = None
    blocks[after_id]["parent"] = inner_id
    payload["project.json"] = json.dumps(
        project, ensure_ascii=False, separators=(",", ":")
    ).encode()
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in payload.items():
            archive.writestr(name, data)
    return path


def verify(harness: Path, project: Path, *, outer_count: int,
           inner_count: int, block_count: bool):
    total = 3 * outer_count * inner_count * 2
    baseline, _ = run_mode(harness, project, "baseline", count=block_count)
    assert baseline["v_sum"] == f"n:{total}", baseline
    assert baseline["v_branchCount"] == "s:0", baseline

    positive, trace = run_mode(
        harness, project, "positive", count=block_count
    )
    assert positive == baseline, (positive, baseline)
    promotions = len(PROMOTION.findall(trace))
    assert promotions == len(RESTORE.findall(trace)) == 3, (
        "bounded inner loop was not actually promoted", trace[-2800:]
    )
    assert "bounded inner repeat accepted" in trace, trace[-2800:]
    disabled, noOptTrace = run_mode(
        harness, project, "disabled", count=block_count
    )
    assert disabled == baseline, (disabled, baseline)
    assert not PROMOTION.search(noOptTrace), (
        "opt-in disabled but bounded nested native OSR was promoted",
        noOptTrace[-2200:],
    )
    print("PASS bounded-inner opt-in OFF: no nested native OSR, "
          "Interpreter parity", flush=True)
    print(f"PASS positive bounded-inner native=3/3 "
          f"count={'on' if block_count else 'off'}", flush=True)

    for mode in ("unguarded", "guarded", "unguarded-late", "guarded-late"):
        state, trace = run_mode(
            harness, project, mode, count=block_count
        )
        entries = EDIT.findall(trace)
        assert len(entries) == 1, (mode, entries, trace[-3200:])
        remaining, flag, committed, at_count, yields = entries[0]
        is_guarded = mode.startswith("guarded")
        assert int(flag) == int(is_guarded), (mode, entries)
        assert int(yields) >= (4 if mode.endswith("-late") else 1)
        assert 0 < float(committed) < total
        promotions = len(PROMOTION.findall(trace))
        restores = len(RESTORE.findall(trace))
        assert 1 <= promotions <= 3, (mode, promotions, trace[-2400:])
        assert len(DEOPT.findall(trace)) == int(is_guarded), (
            mode, trace[-2400:]
        )
        # The late fourth yield may be in the second parent invocation;
        # every already-completed native invocation gets exactly one
        # ordinary restore. The mutated invocation is deoptimized instead.
        assert restores == promotions - int(is_guarded), (
            mode, promotions, restores, trace[-2400:]
        )
        sum_value = int(state["v_sum"][2:])
        branch = int(state["v_branchCount"][2:])
        assert sum_value + branch == total, (mode, state)
        if is_guarded:
            assert sum_value == int(float(committed)), (
                mode, sum_value, committed, trace[-2400:]
            )
            assert branch == total - sum_value
        else:
            assert sum_value > int(float(committed)), (
                mode, sum_value, committed, trace[-2400:]
            )
        for key, value in baseline.items():
            if key in ("v_sum", "v_branchCount"): continue
            assert state[key] == value, (mode, key, state[key], value)
        print(f"PASS {mode}: native={promotions} deopt={int(is_guarded)} "
              f"sum={sum_value} redirected={branch} "
              f"blocks={state.get('__block_count', 'disabled')}",
              flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--harness", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--outer-count", type=int, default=4_000_000)
    ap.add_argument("--inner-count", type=int, default=2)
    ap.add_argument("--without-block-count", action="store_true")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    project = nested_fixture(
        a.out / "bounded-nested-repeat.sb3",
        outer_count=a.outer_count, inner_count=a.inner_count
    )
    verify(
        a.harness, project, outer_count=a.outer_count,
        inner_count=a.inner_count,
        block_count=not a.without_block_count,
    )
    # Unsafe high-count nested loops must fail CLOSED. 800_000 * 10 *
    # 2 increments * three parents is still 48M, so the same harness can
    # validate exact final values without changing a product-code branch.
    unsafe = nested_fixture(
        a.out / "rejected-inner-count-10.sb3",
        outer_count=800_000, inner_count=10,
    )
    block_count = not a.without_block_count
    baseline, _ = run_mode(a.harness, unsafe, "baseline",
                           count=block_count)
    rejected, trace = run_mode(a.harness, unsafe, "rejected",
                               count=block_count)
    assert rejected == baseline, (rejected, baseline)
    assert not PROMOTION.search(trace), (
        "nested count >8 incorrectly permitted native OSR",
        trace[-2800:],
    )
    assert "bounded inner repeat accepted" not in trace
    print("PASS inner repeat count=10 rejected (no native OSR), "
          "full Scratch state parity", flush=True)


if __name__ == "__main__":
    main()
