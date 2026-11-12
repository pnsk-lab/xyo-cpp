#!/usr/bin/env python3
"""OSR-012: dynamic inner repeat TIMES (variable or pure addition) checkpoint.

Source:
  root repeat(3), parent sets TIMES var=17 before calling warp
  selected warp repeat(outer), inner repeat(times [+ 1])
       change sum by 2
       set times to 7     <-- mutates the live inner loop's TIMES reporter
  source edits change sum -> branchCount after a REAL inner native yield.
Native must retain the original inner-loop countdown, despite changes to
TIMES and later reporter reevaluations.
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
from jit_nested_warp_bounded_inner_deopt import nested_fixture
from jit_nested_warp_inflight_body_edit import run_mode, PROMOTION, RESTORE, EDIT

INNER_DEOPT = re.compile(
    r"\[jit-osr-inflight\] deopt inner outer=([0-9.]+) "
    r"inner=([0-9.]+) source-changed=1"
)


def dynamic_fixture(path: Path, *, outer_count: int, kind: str,
                    reset_each_outer: bool = False):
    """Convert a statically counted source into a variable or expression."""
    if kind not in ("variable", "add", "subtract", "add-half", "multiply"):
        raise ValueError(kind)
    nested_fixture(path, outer_count=outer_count, inner_count=7)
    with zipfile.ZipFile(path) as archive:
        data = {n: archive.read(n) for n in archive.namelist()}
    project = json.loads(data["project.json"])
    blocks = project["targets"][1]["blocks"]
    inner_id = "osr010_nested_fixed_inner"
    inner = blocks[inner_id]
    assert inner["opcode"] == "control_repeat"
    change_id = inner["inputs"]["SUBSTACK"][1]
    change = blocks[change_id]
    assert change["opcode"] == "data_changevariableby"
    assert change["next"] is None

    # The second command of the inner body intentionally changes the
    # reporter's value AFTER its first iteration. It cannot change the
    # private loop countdown, and its effects must never be replayed.
    mutate_id = "osr012_set_times_inside_inner"
    blocks[mutate_id] = {
        "opcode": "data_setvariableto",
        "next": None, "parent": change_id,
        "inputs": {"VALUE": [1, [4, "7"]]},
        "fields": {"VARIABLE": ["times", "v_times"]},
        "topLevel": False, "shadow": False,
    }
    change["next"] = mutate_id

    if reset_each_outer:
        # Force the TIMES variable to change from 17 to 7 during EVERY
        # native inner loop, including those entered after LLVM OSR.
        # Without this prefix the first 17->7 transition occurs before
        # OSR promotion and does not establish native countdown parity.
        outer_id = inner["parent"]
        outer = blocks[outer_id]
        assert outer["inputs"]["SUBSTACK"][1] == inner_id
        prefix_id = "osr012_reset_times_each_outer"
        blocks[prefix_id] = {
            "opcode": "data_setvariableto",
            "next": inner_id, "parent": outer_id,
            "inputs": {"VALUE": [1, [4, "17"]]},
            "fields": {"VARIABLE": ["times", "v_times"]},
            "topLevel": False, "shadow": False,
        }
        outer["inputs"]["SUBSTACK"] = [2, prefix_id]
        inner["parent"] = prefix_id

    var_id = "osr012_read_times"
    reporter = {
        "opcode": "data_variable",
        "next": None, "parent": inner_id,
        "inputs": {},
        "fields": {"VARIABLE": ["times", "v_times"]},
        "topLevel": False, "shadow": False,
    }
    blocks[var_id] = reporter
    if kind == "variable":
        inner["inputs"]["TIMES"] = [3, var_id, [4, "7"]]
    else:
        op_id = "osr012_times_binary"
        assert kind in ("add", "subtract", "add-half", "multiply")
        reporter["parent"] = op_id
        blocks[op_id] = {
            "opcode": {
                "add": "operator_add",
                "subtract": "operator_subtract",
                "add-half": "operator_add",
                "multiply": "operator_multiply",
            }[kind],
            "next": None, "parent": inner_id,
            "inputs": {
                "NUM1": [3, var_id, [4, "0"]],
                "NUM2": [1, [4, "0.5" if kind == "add-half" else "1"]],
            },
            "fields": {}, "topLevel": False, "shadow": False,
        }
        inner["inputs"]["TIMES"] = [3, op_id, [4, "7"]]

    data["project.json"] = json.dumps(
        project, ensure_ascii=False, separators=(",", ":")
    ).encode()
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, contents in data.items():
            archive.writestr(name, contents)
    return path


def verify(harness: Path, project: Path, *, outer_count: int,
           delta: int, block_count: bool,
           reset_each_outer: bool = False):
    # Before EVERY parent warp call, v_times is set to 17. The first
    # inner repeat uses Scratch-rounded(17+delta), and all later outer
    # iterations use Scratch-rounded(7+delta). For the half-integer test,
    # delta=1 denotes Math.round(x+0.5), not raw floating-point addition.
    # The native f64 counter differs from the source integer latch.
    # The *original* root repeat(3) must remain at 3, not become 7.
    expected = 3 * 2 * (
        outer_count * (17 + delta) if reset_each_outer
        else (17 + delta) + (outer_count - 1) * (7 + delta)
    )
    os.environ["XYO_OSR_EXPECTED_SUM"] = str(expected)
    os.environ["XYO_OSR_EXPECTED_TIMES"] = "s:7"
    baseline, _ = run_mode(harness, project, "baseline", count=block_count)
    assert baseline["v_sum"] == f"n:{expected}", baseline
    assert baseline["v_times"] == "s:7", baseline
    assert baseline["v_branchCount"] == "s:0", baseline

    disabled, trace = run_mode(harness, project, "disabled",
                               count=block_count)
    assert disabled == baseline, (disabled, baseline)
    assert not PROMOTION.search(trace), trace[-3000:]
    print("PASS dynamic inner opt-in disabled: no native promotion",
          flush=True)

    positive, trace = run_mode(harness, project, "positive",
                               count=block_count)
    assert positive == baseline, (positive, baseline)
    promoted = len(PROMOTION.findall(trace))
    assert 1 <= promoted <= 3, trace[-3000:]
    assert len(RESTORE.findall(trace)) == promoted, trace[-3000:]
    assert "yielding inner repeat accepted" in trace, trace[-3000:]
    print(f"PASS dynamic positive native={promoted} state=Interpreter "
          f"count={'on' if block_count else 'off'}", flush=True)

    for mode in ("guarded", "guarded-late"):
        state, trace = run_mode(harness, project, mode,
                                count=block_count)
        edits = EDIT.findall(trace)
        assert len(edits) == 1, (mode, edits, trace[-3200:])
        remaining, guard, committed, at_count, yields = edits[0]
        assert guard == "1"
        assert int(yields) >= (4 if mode.endswith("-late") else 1)
        assert 0 < float(committed) < expected
        # The count at a genuine inner-body-completed source boundary
        # must be in the integer interpreter latch domain, independent
        # of mutating the runtime TIMES variable in this same body.
        active_count = 17 + delta
        assert 0 <= float(remaining) < active_count, (
            mode, remaining, active_count
        )
        assert float(remaining).is_integer(), (
            mode, remaining, "fractional native counter leaked to Interpreter"
        )
        deopts = INNER_DEOPT.findall(trace)
        assert len(deopts) == 1, (mode, trace[-3200:])
        promotions = len(PROMOTION.findall(trace))
        restores = len(RESTORE.findall(trace))
        assert 1 <= promotions <= 3, (mode, trace[-3200:])
        assert restores == promotions - 1, (mode, trace[-3200:])
        sum_value = int(state["v_sum"][2:])
        changed_value = int(state["v_branchCount"][2:])
        assert sum_value == int(float(committed)), (mode, committed, state)
        assert sum_value + changed_value == expected, (mode, state)
        for key, value in baseline.items():
            if key not in ("v_sum", "v_branchCount"):
                assert state[key] == value, (mode, key, state[key], value)
        print(f"PASS {mode}: phase2 deopt=1 native={promotions} "
              f"saved={sum_value} redirected={changed_value} "
              f"blocks={state.get('__block_count','disabled')}",
              flush=True)
    return expected, baseline


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--harness", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--outer-count", type=int, default=350_000)
    ap.add_argument("--without-block-count", action="store_true")
    ap.add_argument("--stress-active-times", action="store_true")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    count = not args.without_block_count
    if args.stress_active_times:
        project = dynamic_fixture(
            args.out / "active-native-times-17-to-7.sb3",
            outer_count=args.outer_count, kind="variable",
            reset_each_outer=True,
        )
        total, baseline = verify(
            args.harness, project, outer_count=args.outer_count,
            delta=0, block_count=count, reset_each_outer=True,
        )
        print(f"PASS active native TIMES 17->7 each inner loop, "
              f"total={total}, block_count={baseline.get('__block_count')}",
              flush=True)
        return
    for kind, delta in (
        ("variable", 0), ("add", 1), ("subtract", -1),
        ("add-half", 1),
    ):
        project = dynamic_fixture(
            args.out / f"dynamic-times-{kind}.sb3",
            outer_count=args.outer_count, kind=kind,
        )
        expected, baseline = verify(
            args.harness, project, outer_count=args.outer_count,
            delta=delta, block_count=count,
        )
        print(f"PASS {kind}: count={count}, total={expected}, "
              f"exact_blocks={baseline.get('__block_count', 'disabled')}",
              flush=True)

    # Non-whitelisted multiplication of the SAME live variable by 1 is
    # deterministic and Interpreter-executable but intentionally NOT an
    # eligible inner source proof. It must never produce nested native OSR.
    rejected = dynamic_fixture(
        args.out / "dynamic-times-multiply-rejected.sb3",
        outer_count=6_000, kind="multiply",
    )
    os.environ["XYO_OSR_EXPECTED_SUM"] = str(6 * (17 + 5_999 * 7))
    os.environ["XYO_OSR_EXPECTED_TIMES"] = "s:7"
    baseline, _ = run_mode(args.harness, rejected, "baseline",
                           count=count)
    refused, trace = run_mode(args.harness, rejected, "rejected",
                              count=count)
    assert refused == baseline, (baseline, refused)
    assert not PROMOTION.search(trace), (
        "unapproved operator_multiply bypassed source validation",
        trace[-2900:],
    )
    print("PASS unsupported multiplication TIMES reporter rejected",
          flush=True)


if __name__ == "__main__":
    main()
