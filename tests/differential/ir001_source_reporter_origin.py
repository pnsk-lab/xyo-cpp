#!/usr/bin/env python3
"""IR-001 production Scratch reporter -> LLVM Value* provenance diagnostics.

Uses real native JIT expression lowering and the frozen source PC
dictionary. These observations never certify boxed Scratch Values as
recoverable numeric SSA or enable interpreter deoptimization.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import sys
import zipfile
import json

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))
from gen_algo_bench import (
    Gen, S_set, S_add, S_change, S_replace, S_repeat, num, var, add,
    item, length, make_project, write_sb3,
)
from runtime_broadcast_journal import execute

LINE = re.compile(
    r"\[ir001-source-reporter\] pc=(\d+) block=(\S+) "
    r"parent=(\S+) input=(\S+) raw_numeric=(\d) "
    r"live_checkpoints=(\d+) source_kind=(\w+) "
    r"gc_root_proven=0 executable=0"
)
SUMMARY = re.compile(
    r"\[ir001-source-reporter-summary\] observed=(\d+) "
    r"remaining=(\d+) discarded=(\d+) constants=(\d+) "
    r"raw_numeric=(\d+) boxed_or_unknown=(\d+) "
    r"live_at_checkpoint=(\d+) checkpoint_sites=(\d+) "
    r"source_complete=0 executable=0"
)
GAP = re.compile(
    r"\[ir001-source-resume-gap\] pc=(\d+) block=(\S+) phase=(\d+) "
    r"live_reporters=(\d+) boxed_unrooted=(\d+) "
    r"numeric_unmapped=(\d+) source_slot_missing=(\d+) "
    r"unsupported_llvm=(\d+) variables=(\d+) lists=(\d+) "
    r"arguments=(\d+) expressions=(\d+) literals=(\d+) other=(\d+) "
    r"loop_path_depth=(\d+) loop_path_ids=(\S+) "
    r"loop_path_restorable=0 source_complete=0 executable=0"
)
SCHEDULER_JOIN = re.compile(
    r"\[ir001-scheduler-reporter-audit\] site=(\d+) "
    r"observed=(\d+) unmapped=(\d+) unsupported=(\d+) "
    r"source_complete=0 executable=0"
)
RUNTIME_OWNERS = re.compile(
    r"\[ir001-runtime-reporter-owners\] site=(\d+) "
    r"matched=(\d) scalars=(\d+) lists=(\d+) missing=(\d+) "
    r"wrong_type=(\d+) nonstorage=(\d+) unresolved=(\d+) "
    r"value_restorable=0 executable=0"
)
SCHEDULER_OWNERS = re.compile(
    r"\[ir001-scheduler-reporter-owners\] site=(\d+) "
    r"matched=(\d) epoch_stable=(\d) lists=(\d+) scalars=(\d+) "
    r"missing=(\d+) wrong_type=(\d+) unresolved=(\d+) "
    r"value_restorable=0 executable=0"
)
RUNTIME_SCALAR_ROOT_CHECK = re.compile(
    r"\[ir001-runtime-scalar-root-check\] site=(\d+) "
    r"valid=(\d) live=(\d+) source_complete=0 executable=0"
)
RUNTIME_SCALAR_PREFIX = re.compile(
    r"\[ir001-runtime-scalar-prefix\] site=(\d+) "
    r"matched=(\d+) missing=(\d+) ambiguous=(\d+) "
    r"wrong_owner=(\d+) order_unproven=(\d+) rejected=(\d+) "
    r"live=(\d+) live_missing=(\d+) live_ambiguous=(\d+) "
    r"live_invalid=(\d+) expected=(\d+) "
    r"source_complete=0 executable=0"
)
RUNTIME_LIVE_SCALAR = re.compile(
    r"\[ir001-runtime-live-scalar\] site=(\d+) "
    r"reporter_pc=(\d+) variable_id=(\S+) seq=(\d+) "
    r"rooted=(\d) source_complete=0 executable=0"
)
RUNTIME_REPORTED_PREFIX_PLAN = re.compile(
    r"\[ir001-runtime-reported-prefix-plan\] site=(\d+) "
    r"captured=(\d+) unsupported=(\d+) missing=(\d+) "
    r"ambiguous=(\d+) invalid_roots=(\d+) order_bad=(\d+) "
    r"gaps=(\d+) verified=(\d) executable=0"
)
RUNTIME_REPORTED_ENTRY_MAP = re.compile(
    r"\[ir001-runtime-reported-entry-map\] site=(\d+) "
    r"mapped=(\d+) unavailable=(\d+) rootless=(\d+) "
    r"duplicate=(\d+) incomplete_prefix=(\d+) "
    r"contiguous_prefix=(\d+) missing_prefix_statements=(\d+) "
    r"input_count=(\d+) source_complete=0 executable=0"
)
RUNTIME_REPORTED_ENTRY_SLOT = re.compile(
    r"\[ir001-runtime-reported-entry-slot\] site=(\d+) "
    r"reporter_pc=(\d+) op_id=(\S+) parent=(\S+) input=(\S+) "
    r"statement=(\S+) position=(\d+) ops=(\d+) "
    r"kind=(\d+) source_complete=0 executable=0"
)
SCHEDULER_REPORTED_ENTRY_MAP = re.compile(
    r"\[ir001-scheduler-reported-entry-map\] site=(\d+) "
    r"mapped=(\d+) input_count=(\d+) "
    r"incomplete_prefix=(\d+) contiguous_prefix=(\d+) "
    r"source_complete=0 executable=0"
)
RUNTIME_JOIN = re.compile(
    r"\[ir001-runtime-reporter-join\] site=(\d+) pc=(\d+) "
    r"matched=(\d) observed=(\d+) arguments=(\d+) generated=(\d+) "
    r"heap_roots=(\d+) unmapped=(\d+) unsupported=(\d+) "
    r"source_complete=0 executable=0"
)
GAP_PHYSICAL_SITE = re.compile(
    GAP.pattern + r" physical_site=(\d+)"
)
RUNTIME_PATH = re.compile(
    r"\[ir001-runtime-loop-path\] block=(\S+) phase=(\d+) "
    r"loop_path_depth=(\d+) loop_path_ids=(\S+) "
    r"readback=(\d) counted=(\d+) uncounted=(\d+) "
    r"gc_root_proven=0 source_complete=0 executable=0"
)
GAP_ITEM = re.compile(
    r"\[ir001-source-resume-gap-item\] checkpoint_pc=(\d+) "
    r"block=(\S+) phase=(\d+) reporter_pc=(\d+) reporter=(\S+) "
    r"parent=(\S+) input=(\S+) source_kind=(\w+) "
    r"gc_root_proven=0 source_slot_proven=0 executable=0"
)
GAP_ITEM_SSA = re.compile(
    GAP_ITEM.pattern + r" ssa_id=(\d+) ssa_width=(\d+)"
)
GAP_ITEM_OWNER = re.compile(
    GAP_ITEM_SSA.pattern + r" source_owner=(\w+) storage_id=(\S+)"
)
GAP_ITEM_INPUT_SLOT = re.compile(
    GAP_ITEM_OWNER.pattern + r" input_ordinal=(\d+) selected_shadow=(\d)"
)
RUNTIME_LIST_INDEX_ITEM = re.compile(
    r"\[ir001-runtime-list-index-item\] site=(\d+) "
    r"reporter_pc=(\d+) list_id=(\S+) index_source=(\S+) "
    r"index0=(-?\d+) seq=(\d+) len_after=(\d+) "
    r"storage_epoch_after=(\d+) rooted=(\d) "
    r"content_unchanged=0 executable=0"
)
RUNTIME_LIST_CONTENT_EPOCH = re.compile(
    r"\[ir001-runtime-list-content-epoch\] site=(\d+) "
    r"stable_since_observer=(\d+) changed_since_observer=(\d+) "
    r"untracked=(\d+) content_unchanged=0 executable=0"
)
RUNTIME_LIST_READ_PROOF = re.compile(
    r"\[ir001-runtime-list-read-proof\] site=(\d+) "
    r"expected=(\d+) accepted=(\d) "
    r"source_complete=0 executable=0"
)
RUNTIME_LIST_INDEX = re.compile(
    r"\[ir001-runtime-list-index\] site=(\d+) "
    r"matched=(\d+) numeric_index=(\d+) missing=(\d+) "
    r"ambiguous=(\d+) value_mismatch=(\d+) rejected=(\d+) "
    r"content_unchanged=0 executable=0"
)
RUNTIME_INPUT_SLOTS = re.compile(
    r"\[ir001-runtime-reporter-inputs\] site=(\d+) matched=(\d) "
    r"inputs=(\d+) immediate=(\d+) rooted_heap=(\d+) "
    r"unrooted_heap=(\d+) nonboxed=(\d+) invalid=(\d+) "
    r"ambiguous=(\d+) restorable=0 executable=0"
)
SCHEDULER_INPUT_SLOTS = re.compile(
    r"\[ir001-scheduler-reporter-inputs\] site=(\d+) "
    r"inputs=(\d+) immediate=(\d+) rooted_heap=(\d+) "
    r"unrooted_heap=(\d+) ambiguous=(\d+) "
    r"restorable=0 executable=0"
)
GAP_SUMMARY = re.compile(
    r"\[ir001-source-resume-summary\] checkpoints=(\d+) "
    r"attributed=(\d+) unowned_checkpoints=(\d+) "
    r"invalid_source_observations=(\d+) source_complete=0 executable=0"
)
COMPILER = re.compile(
    r"\[ir001-source-reporter-compiler\] source_revision=(\d+) "
    r"source_digest=([0-9a-f]+):([0-9a-f]+) dropped=(\d+) "
    r"edge_rejected=(\d+) checkpoint_rejected=(\d+) "
    r"source_complete=0 executable=0"
)


def fixture(path: Path) -> Path:
    gen = Gen(["x", "done"], [])
    gen.script([
        S_set("x", num(0)),
        S_repeat(num(3), [
            S_change("x", add(var("x"), num(2))),
            S_set("done", add(var("x"), num(1))),
        ]),
        S_set("done", add(var("x"), num(1))),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(["x", "done"], [], gen))
    return path


def nested_list_fixture(path: Path) -> Path:
    gen = Gen(["x", "done"], ["history"])
    gen.script([
        S_set("x", num(0)),
        S_add("history", num(4)),
        S_repeat(num(2), [
            S_repeat(num(2), [
                S_change("x", add(var("x"),
                                  item("history", num(1)))),
                S_set("done", length("history")),
            ]),
        ]),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path),
              make_project(["x", "done"], ["history"], gen))
    return path


def changed_list_fixture(path: Path) -> Path:
    # Identical reporter path to nested_list_fixture, with a NATIVE
    # LLVM-inline list-element overwrite AFTER the actual list read and
    # BEFORE the loop's physical SSA checkpoint.
    gen = Gen(["x", "done"], ["history"])
    gen.script([
        S_set("x", num(0)),
        S_add("history", num(4)),
        S_repeat(num(2), [
            S_repeat(num(2), [
                S_change("x", add(var("x"),
                                  item("history", num(1)))),
                S_replace("history", num(1), num(5)),
                S_set("done", length("history")),
            ]),
        ]),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(
        ["x", "done"], ["history"], gen))
    return path


def source_pcs(project: Path) -> dict[str, int]:
    with zipfile.ZipFile(project) as data:
        project_json = json.loads(data.read("project.json"))
    all_blocks = {
        key: value for target in project_json["targets"]
        for key, value in target["blocks"].items()
    }
    visited: set[str] = set()
    for block_id, block in all_blocks.items():
        if not block.get("topLevel"):
            continue
        pending = [block_id]
        while pending:
            current = pending.pop()
            if current in visited or current not in all_blocks:
                continue
            visited.add(current)
            node = all_blocks[current]
            if isinstance(node.get("next"), str):
                pending.append(node["next"])
            for spec in node.get("inputs", {}).values():
                if not isinstance(spec, list):
                    continue
                for value in spec[1:]:
                    if isinstance(value, str) and value in all_blocks:
                        pending.append(value)
    return {block_id: i for i, block_id in enumerate(sorted(visited), 1)}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    project = fixture(args.out / "ir001-source-value-origin.sb3")
    pcs = source_pcs(project)

    settings = {
        "XYO_IR001_TRACE_SOURCE_VALUES": "1",
        "XYO_IR001_TRACE_EFFECT_PC": None,
        "XYO_JIT_CALLFRAME": "1",
        "XYO_RT_ROOT_ONLY_DEOPT": "1",
        "XYO_JIT_SSA_LIVENESS": None,
        "XYO_RT_CAPTURE_LLVM_SSA": None,
        "XYO_IR001_RUNTIME_REPORTER_AUDIT": None,
        "XYO_IR001_TEST_DROP_REPORTER_INPUT_ROOT": None,
        "XYO_IR001_TEST_DUPLICATE_LIST_EVAL": None,
        "XYO_IR001_TEST_DROP_SCALAR_READ_ROOT": None,
        "XYO_LIST_STORAGE": None,
        "XYO_RT_REQUIRE_REPORTER_SSA_AUDIT": None,
        "XYO_RT_NATIVE_DEOPT_ONCE": None,
        "XYO_RT_NATIVE_DEOPT_AFTER": None,
        "XYO_RT_TEST_CORRUPT_LLVM_SSA": None,
        "XYO_RT_TEST_DEOPT_ROOT_AFTER_LOOP_ONLY": None,
    }
    original = {key: os.environ.get(key) for key in settings}
    try:
        for key, value in settings.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        traced, _, log = execute(args.native, project, "eager", False)
        # Compiler-only source tracing must not install runtime list
        # observers or retain list-item roots on otherwise normal JIT runs.
        assert not RUNTIME_LIST_INDEX.findall(log), log[-3000:]
        assert not RUNTIME_SCALAR_ROOT_CHECK.findall(log), log[-3000:]
        lines = LINE.findall(log)
        summary = SUMMARY.findall(log)
        compiler = COMPILER.findall(log)
        gaps = GAP.findall(log)
        physical_gaps = GAP_PHYSICAL_SITE.findall(log)
        assert len(physical_gaps) == len(gaps)
        assert sorted(int(row[-1]) for row in physical_gaps) == list(
            range(1, len(physical_gaps) + 1)), physical_gaps
        gap_items = GAP_ITEM.findall(log)
        gap_items_ssa = GAP_ITEM_SSA.findall(log)
        assert len(gap_items_ssa) == len(gap_items)
        for detail in gap_items_ssa:
            assert detail[:8] in gap_items
            assert int(detail[8]) >= 0 and int(detail[9]) in (1, 32, 64)
        gap_summary = GAP_SUMMARY.findall(log)
        assert lines, log[-4500:]
        assert summary and compiler and gap_summary, log[-4500:]
        assert gaps and len(gap_summary) == 1, (
            gaps, gap_summary, log[-4500:])
        # Each actual compiled checkpoint must have exact frozen
        # loop site, source PC and phase; duplicate/unknown sites are
        # not treated as known continuation origins.
        for checkpoint_count, attributed, unknown, invalid in gap_summary:
            assert int(checkpoint_count) > 0
            assert int(checkpoint_count) == int(attributed) == len(gaps)
            assert int(unknown) == int(invalid) == 0
        for (pc, block, phase, live, boxed, numeric, missing, other,
             variables, lists, arguments, expressions, literals,
             unknown, loop_depth, loop_path) in gaps:
            assert block in pcs and int(pc) == pcs[block], (pc, block, pcs)
            assert int(phase) in (1, 2), phase
            assert int(live) == int(boxed) + int(numeric) == int(missing)
            assert int(live) == sum(map(int, (
                variables, lists, arguments, expressions, literals, unknown
            )))
            path = loop_path.split(",")
            assert int(loop_depth) == len(path) >= 1
            assert path[-1] == block and len(set(path)) == len(path)
            assert int(other) >= 0
        assert sum(int(gap[3]) for gap in gaps) > 0, gaps
        assert sum(int(gap[11]) for gap in gaps) >= 1, (
            "no live computed Scratch expression was classified", gaps)
        # Per-checkpoint reporter records name the ORIGINAL captured
        # source and cannot assert GC roots or a source-local target.
        assert gap_items, log[-3500:]
        for (pc, block, phase, reporter_pc, reporter, parent,
             input_name, source_kind) in gap_items:
            assert source_kind in (
                "variable_read", "list_read", "procedure_argument",
                "expression", "literal", "other_reporter", "unknown")
            assert any(
                int(row[0]) == int(pc) and row[1] == block and
                int(row[2]) == int(phase) for row in gaps
            )
            assert reporter in pcs and int(reporter_pc) == pcs[reporter]
            assert parent in pcs and input_name
        assert "[ir001-effect-source]" not in log
        observed = {}
        origin_kinds = set()
        for pc, block, parent, input_name, raw, live, source_kind in lines:
            origin_kinds.add(source_kind)
            assert source_kind in (
                "variable_read", "list_read", "procedure_argument",
                "expression", "literal", "other_reporter", "unknown")
            if block in pcs:
                assert int(pc) == pcs[block], (block, pc, pcs)
            else:
                # The native loader creates shadow fallback Blocks
                # absent from the source .sb3. These are included in
                # the ACTUAL frozen ScratchCompilationUnit, sorted
                # lexicographically after the original b000N IDs.
                assert block.startswith("xyo_") and int(pc) > len(pcs), (
                    block, pc, pcs)
            assert parent in pcs and input_name, (parent, input_name)
            assert raw == "0" and int(live) >= 0, (
                "NaN-boxed Scratch Value claimed raw numeric", block, raw)
            if block in observed:
                assert observed[block] == int(pc)
            observed[block] = int(pc)
        assert any(row[1] in pcs for row in lines)
        assert "variable_read" in origin_kinds, origin_kinds
        assert "expression" in origin_kinds, origin_kinds
        assert any(int(row[5]) > 0 for row in lines), (
            "no real reporter LLVM value survived a checkpoint", lines)
        assert any(row[1] == "xyo_00000003" or
                   row[1].startswith("xyo_") for row in lines), (
            "runtime-created shadow sources were not included", lines)
        for (seen, retained, discarded, constants, raw, boxed,
             live, checkpoints) in summary:
            values = [int(value) for value in (
                seen, retained, discarded, constants, raw, boxed,
                live, checkpoints)]
            assert values[0] >= 1
            assert values[1] + values[2] == values[0], values
            assert values[4] + values[5] == values[1], values
            assert values[4] == 0, (
                "boxed reporter output certified as raw numeric", values)
            assert values[3] <= values[1]
            assert 1 <= values[6] <= values[1], values
            assert values[7] >= 1, values
        for revision, lo, hi, dropped, rejected, rejected_checkpoints in compiler:
            assert int(revision) > 0 and (int(lo, 16) or int(hi, 16))
            assert int(dropped) >= 0
            assert int(rejected) == int(rejected_checkpoints) == 0, (
                "compiled reporter or checkpoint lacks frozen source proof",
                rejected, rejected_checkpoints, log[-2400:])

        # A real nested Scratch repeat emits TWO separate frame path
        # lengths; only the LLVM ABI-declared path DEPTH is observed,
        # not a certified restored runtime loop counter.
        nested = nested_list_fixture(
            args.out / "ir001-nested-reporter-roles.sb3")
        nested_traced, nested_stats, nested_log = execute(
            args.native, nested, "eager", False)
        assert nested_traced.get("x") == "60", nested_traced
        assert nested_traced.get("done") == "1", nested_traced
        assert nested_traced.get("history") == ["4"], nested_traced
        nested_pcs = source_pcs(nested)
        nested_gaps = GAP.findall(nested_log)
        nested_physical = GAP_PHYSICAL_SITE.findall(nested_log)
        assert len(nested_physical) == len(nested_gaps)
        assert sorted(int(row[-1]) for row in nested_physical) == list(
            range(1, len(nested_physical) + 1)), nested_physical
        nested_ssa = GAP_ITEM_SSA.findall(nested_log)
        nested_owners = GAP_ITEM_OWNER.findall(nested_log)
        nested_slots = GAP_ITEM_INPUT_SLOT.findall(nested_log)
        assert len(nested_slots) == len(nested_ssa)
        assert all(int(row[-2]) > 0 and row[-1] == "0"
                   for row in nested_slots), nested_slots
        assert {int(row[-2]) for row in nested_slots} == {1, 2}, nested_slots
        assert nested_owners and len(nested_owners) == len(nested_ssa)
        assert all(owner == "list_owner" and owner_id != "none"
                   for *_, owner, owner_id in nested_owners), nested_owners
        assert nested_ssa and all(int(row[9]) == 64 for row in nested_ssa)
        assert all(row[7] == "list_read" for row in nested_ssa), nested_ssa
        nested_origins = LINE.findall(nested_log)
        runtime_paths = RUNTIME_PATH.findall(nested_log)
        assert runtime_paths, nested_log[-4000:]
        compiled_paths = {(g[1], g[2], g[14], g[15]) for g in nested_gaps}
        runtime_depths = set()
        for block, phase, depth, path, readback, counted, uncounted in runtime_paths:
            assert (block, phase, depth, path) in compiled_paths, (
                "runtime path not in compiler checkpoints",
                (block, phase, depth, path), compiled_paths)
            assert readback == "1", (block, phase, depth, path, nested_log[-3000:])
            assert int(counted) == int(depth) and uncounted == "0", (
                "counted repeat counter not witnessed", block, path)
            runtime_depths.add(int(depth))
        assert runtime_depths == {1, 2}, runtime_depths
        assert nested_gaps and nested_origins, nested_log[-3000:]
        observed_depths = {int(gap[14]) for gap in nested_gaps}
        assert observed_depths == {1, 2}, (nested_gaps, nested_log[-3300:])
        nested_lexical_paths = {tuple(gap[15].split(",")) for gap in nested_gaps}
        assert len(nested_lexical_paths) == 2, nested_lexical_paths
        for source_path in nested_lexical_paths:
            assert len(source_path) in (1, 2)
            assert len(set(source_path)) == len(source_path)
        assert any(
            len(source_path) == 2 and source_path[1] in nested_pcs and
            source_path[0] in nested_pcs
            for source_path in nested_lexical_paths
        ), nested_lexical_paths
        for gap in nested_gaps:
            assert gap[1] in nested_pcs and int(gap[0]) == nested_pcs[gap[1]]
            assert int(gap[3]) == int(gap[4]) + int(gap[5])
            assert int(gap[3]) == sum(int(v) for v in gap[8:14])
        assert any(
            int(gap[14]) == 2 and int(gap[9]) >= 1 and
            int(gap[3]) >= 1 for gap in nested_gaps
        ), ("inner loop must carry an unresolved list read", nested_gaps)
        assert any(
            origin[-1] == "list_read" and int(origin[5]) > 0
            for origin in nested_origins
        ), nested_origins
        assert any(
            origin[-1] == "variable_read" for origin in nested_origins
        ), nested_origins
        assert GAP_SUMMARY.findall(nested_log) == [
            (str(len(nested_gaps)), str(len(nested_gaps)), "0", "0")
        ], nested_log[-3300:]

        # Freeze every compiler site's live SSA IDs BEFORE inserting any
        # checkpoint spill. A second runtime pass still resolves the
        # original 4 physical LLVM CallBases and preserves Scratch state.
        os.environ["XYO_JIT_SSA_LIVENESS"] = "1"
        os.environ["XYO_RT_CAPTURE_LLVM_SSA"] = "1"
        os.environ["XYO_IR001_RUNTIME_REPORTER_AUDIT"] = "1"
        with_capture, _, capture_log = execute(
            args.native, nested, "eager", False)
        assert with_capture == nested_traced
        captures = re.findall(
            r"\[ir001-ssa-spill\] saved=(\d+) complete=(\d)",
            capture_log)
        captures = [tuple(map(int, item)) for item in captures]
        assert len(captures) == len(nested_gaps), captures
        assert all(saved > 0 for saved, _ in captures)
        assert capture_log.count("[rt001-ssa-extra] fn=") == len(captures)
        assert "unsupported-mismatch" not in capture_log
        assert "invalid-count-or-ids" not in capture_log
        assert RUNTIME_PATH.findall(capture_log)
        joins = RUNTIME_JOIN.findall(capture_log)
        source_plans = RUNTIME_REPORTED_PREFIX_PLAN.findall(capture_log)
        assert len(source_plans) == len(joins) >= 4, (
            source_plans, joins)
        assert all(
            site != "1" or
            (int(captured) == 2 and int(unsupported) == 1 and
             int(missing) == int(ambiguous) == int(invalid) ==
             int(order) == int(gaps) == 0 and int(valid) == 1)
            for site, captured, unsupported, missing, ambiguous, invalid,
                order, gaps, valid in source_plans
        ), source_plans
        cached_maps = RUNTIME_REPORTED_ENTRY_MAP.findall(capture_log)
        cached_slots = RUNTIME_REPORTED_ENTRY_SLOT.findall(capture_log)
        assert len(cached_maps) == len(joins) >= 4, (
            cached_maps, joins, capture_log[-4200:])
        assert all(int(mapped) == int(inputs) and
                   int(unavailable) == int(rootless) == int(duplicate) == 0
                   for _, mapped, unavailable, rootless, duplicate, _,
                       _, _, inputs in cached_maps), cached_maps
        # Actual scalar read instrumentation keeps its SSA word live in
        # this fixture, while the independent observer checks exact
        # historical Value/GC handle without re-reading the variable.
        assert any(int(site) == 1 and int(mapped) == 3 and
                   int(incomplete) == 0 and int(prefix) == 3 and
                   int(missing) == 0
                   for site, mapped, _, _, _, incomplete,
                       prefix, missing, _ in cached_maps), cached_maps
        scalar_proofs = RUNTIME_SCALAR_PREFIX.findall(capture_log)
        scalar_items = RUNTIME_LIVE_SCALAR.findall(capture_log)
        assert len(scalar_proofs) == len(joins), scalar_proofs
        assert all(int(site) != 1 or
                   (int(live) == 1 and int(missing_live) == 0 and
                    int(ambiguous_live) == 0 and int(invalid_live) == 0 and
                    int(expected) == 0)
                   for site, _, _, _, _, _, _, live, missing_live,
                       ambiguous_live, invalid_live, expected
                   in scalar_proofs), scalar_proofs
        assert len(scalar_items) >= 4 and any(
            site == "1" and var_id == "v_x" and
            int(seq) > 0 and int(rooted) == 1
            for site, _, var_id, seq, rooted in scalar_items
        ), scalar_items
        scalar_roots = RUNTIME_SCALAR_ROOT_CHECK.findall(capture_log)
        assert len(scalar_roots) == len(joins), scalar_roots
        assert all(int(valid) == 1 and int(live) == 1
                   for site, valid, live in scalar_roots
                   if site == "1"), scalar_roots
        assert len(cached_slots) >= 4 and all(
            int(site) == 1 and int(position) < int(count) and
            opcode != "" and parent != "" and input_name != ""
            for site, _, opcode, parent, input_name, _,
                position, count, _ in cached_slots
        ), cached_slots
        content_epochs = RUNTIME_LIST_CONTENT_EPOCH.findall(capture_log)
        read_proofs = RUNTIME_LIST_READ_PROOF.findall(capture_log)
        assert len(content_epochs) == len(read_proofs) == len(joins)
        assert all(int(stable) == 1 and int(changed) == 0 and
                   int(untracked) == 0
                   for site, stable, changed, untracked in content_epochs
                   if site == "1"), content_epochs
        assert all(int(accepted) == 1
                   for site, expected, accepted in read_proofs
                   if site == "1" and int(expected) == 1), read_proofs
        indexed_reads = RUNTIME_LIST_INDEX.findall(capture_log)
        index_items = RUNTIME_LIST_INDEX_ITEM.findall(capture_log)
        assert len(scalar_items) == len(index_items) and all(
            scalar_site == list_site == "1" and
            int(scalar_seq) + 1 == int(list_seq)
            for (scalar_site, _, _, scalar_seq, _),
                (list_site, _, _, _, _, list_seq, *_)
                in zip(scalar_items, index_items)
        ), (scalar_items, index_items)
        assert len(index_items) >= 4, (index_items, capture_log[-4600:])
        assert all(int(site) == 1 and int(index0) == 0 and
                   int(seq) > 0 and int(length) == 1 and
                   int(epoch) > 0 and int(rooted) == 1 and
                   source != "none"
                   for site, _, _, source, index0, seq, length, epoch,
                       rooted in index_items), index_items
        assert len(indexed_reads) == len(joins) >= 4, indexed_reads
        assert any(int(site) == 1 and int(matched) == 1 and
                   int(numeric) == 1 and int(missing) == 0 and
                   int(ambiguous) == 0 and int(mismatch) == 0
                   for site, matched, numeric, missing, ambiguous,
                       mismatch, _ in indexed_reads), indexed_reads
        assert all(int(site) != 1 or (
                   int(matched) == 1 and int(numeric) == 1 and
                   int(missing) == 0 and int(ambiguous) == 0 and
                   int(mismatch) == 0 and int(rejected) == 0)
                   for site, matched, numeric, missing, ambiguous,
                       mismatch, rejected in indexed_reads), indexed_reads
        input_reads = RUNTIME_INPUT_SLOTS.findall(capture_log)
        assert len(input_reads) == len(joins), (input_reads, joins)
        assert all(int(matched) == 1 and int(unrooted) == 0 and
                   int(invalid) == 0 and int(ambiguous) == 0
                   for _, matched, _, _, _, unrooted, _, invalid,
                       ambiguous in input_reads), input_reads
        assert any(int(inputs) == 3 and int(immediate) >= 1 and
                   int(rooted) >= 1 and int(nonboxed) == 0
                   for _, _, inputs, immediate, rooted, _, nonboxed,
                       _, _ in input_reads), input_reads
        owner_reads = RUNTIME_OWNERS.findall(capture_log)
        assert len(owner_reads) == len(joins) >= 4, (
            owner_reads, joins, capture_log[-3600:])
        assert all(int(matched) == 1 and int(missing) == 0 and
                   int(wrong_type) == 0 and int(unresolved) == 0
                   for _, matched, _, _, missing, wrong_type, _,
                       unresolved in owner_reads), owner_reads
        assert any(int(lists) == 2 for _, _, _, lists, *_ in owner_reads), (
            owner_reads, "two observed list reads must resolve their owners")
        compiled_identity = {
            (int(row[-1]), int(row[0])) for row in nested_physical
        }
        assert all(int(site) in {p for p, _ in compiled_identity}
                   for site, *_ in joins)
        assert all(int(matched) == 1 for _, _, matched, *_ in joins), joins
        assert any(int(observed) == 3 and int(unmapped) == 3 and
                   int(roots) == 0 and int(unsupported) > 0
                   for _, _, _, observed, _, _, roots, unmapped,
                       unsupported in joins), joins
        assert any(int(observed) == 0 and int(unmapped) == 0
                   for _, _, _, observed, _, _, _, unmapped, _ in joins), joins

        # A genuine LLVM inline list overwrite after an actual list read
        # changes the GLOBAL CONTENT epoch even when SList::storageEpoch
        # is unchanged. Fail-closed certificate, native Fiber survives.
        changed = changed_list_fixture(
            args.out / "ir001-list-read-then-replace.sb3")
        changed_baseline, _, _ = execute(
            args.native, changed, "off", False)
        changed_native, _, changed_log = execute(
            args.native, changed, "eager", False)
        assert changed_native == changed_baseline, (
            changed_native, changed_baseline)
        changed_epochs = RUNTIME_LIST_CONTENT_EPOCH.findall(changed_log)
        changed_proofs = RUNTIME_LIST_READ_PROOF.findall(changed_log)
        assert any(site == "1" and int(stable) == 0 and
                   int(mutated) == 1 and int(untracked) == 0
                   for site, stable, mutated, untracked
                       in changed_epochs), (
                           changed_epochs, changed_log[-4300:])
        assert any(site == "1" and int(expected) == 1 and
                   int(accepted) == 0
                   for site, expected, accepted in changed_proofs), (
                       changed_proofs, changed_log[-4300:])

        # Test BOTH LLVM inline flat-list updates and the helper-backed
        # deque/gap/chunked paths. Storage backend changes cannot turn an
        # in-flight list write or a stale read into a valid certificate.
        for storage in ("flat", "deque", "gap", "chunked", "adaptive"):
            os.environ["XYO_LIST_STORAGE"] = storage
            for scenario, expected_state, accept in (
                (nested, nested_traced, 1),
                (changed, changed_baseline, 0),
            ):
                actual, _, trace = execute(
                    args.native, scenario, "eager", False)
                assert actual == expected_state, (
                    storage, actual, expected_state)
                reads = RUNTIME_LIST_READ_PROOF.findall(trace)
                epochs = RUNTIME_LIST_CONTENT_EPOCH.findall(trace)
                assert reads and all(
                    int(accepted) == accept
                    for site, count, accepted in reads
                    if site == "1" and count == "1"
                ), (storage, accept, reads)
                assert any(
                    site == "1" and count == "1"
                    for site, count, _ in reads
                ), (storage, reads)
                assert any(
                    site == "1" and int(stable) == accept and
                    int(changed) == 1 - accept and int(untracked) == 0
                    for site, stable, changed, untracked in epochs
                ), (storage, accept, epochs)
        os.environ.pop("XYO_LIST_STORAGE")

        # A real scalar reporter's GC handle may be dropped without
        # altering the JIT SSA, Scratch variables or running Fiber.
        os.environ["XYO_IR001_TEST_DROP_SCALAR_READ_ROOT"] = "1"
        missing_scalar_root_state, _, scalar_root_log = execute(
            args.native, nested, "eager", False)
        assert missing_scalar_root_state == nested_traced
        tampered_scalar_roots = RUNTIME_SCALAR_ROOT_CHECK.findall(
            scalar_root_log)
        assert any(site == "1" and valid == "0" and int(live) == 1
                   for site, valid, live in tampered_scalar_roots), (
                       tampered_scalar_roots, scalar_root_log[-4000:])
        os.environ.pop("XYO_IR001_TEST_DROP_SCALAR_READ_ROOT")

        # A real SSA-list string's exact GC root is deliberately removed
        # after registration. Only the diagnostic snapshot is denied;
        # no reporter re-evaluation or native Fiber retirement is allowed.
        os.environ["XYO_IR001_TEST_DROP_REPORTER_INPUT_ROOT"] = "1"
        dropped_state, _, dropped_log = execute(
            args.native, nested, "eager", False)
        assert dropped_state == nested_traced, (
            dropped_state, nested_traced)
        dropped_inputs = RUNTIME_INPUT_SLOTS.findall(dropped_log)
        assert dropped_inputs, dropped_log[-4200:]
        assert any(int(site) == 1 and int(matched) == 0
                   for site, matched, *_ in dropped_inputs), dropped_inputs
        assert not any(int(site) == 1 and int(matched) == 1
                       for site, matched, *_ in dropped_inputs), (
            "forged GC root accepted", dropped_inputs)
        assert all(int(site) != 3 or int(matched) == 1
                   for site, matched, *_ in dropped_inputs), dropped_inputs
        os.environ.pop("XYO_IR001_TEST_DROP_REPORTER_INPUT_ROOT")

        # Two read-origin events for one frozen reporter PC cannot be
        # attached to one SSA word. The detached certificate fails,
        # while the original native execution reaches the same output.
        os.environ["XYO_IR001_TEST_DUPLICATE_LIST_EVAL"] = "1"
        duplicate_state, _, duplicate_log = execute(
            args.native, nested, "eager", False)
        assert duplicate_state == nested_traced, (
            duplicate_state, nested_traced)
        duplicate_indexes = RUNTIME_LIST_INDEX.findall(duplicate_log)
        assert any(int(site) == 1 and int(matched) == 0 and
                   int(ambiguous) > 0 for site, matched, _,
                   _, ambiguous, _, _ in duplicate_indexes), (
                       duplicate_indexes, duplicate_log[-4500:])
        os.environ.pop("XYO_IR001_TEST_DUPLICATE_LIST_EVAL")
        os.environ.pop("XYO_IR001_RUNTIME_REPORTER_AUDIT")

        # Force a real native Fiber -> interpreter handoff ATTEMPT at
        # the first completed root repeat body. Source reporter SSA is
        # not a complete source environment, so the strict scheduler
        # checker must veto the handoff and preserve native execution.
        os.environ["XYO_RT_REQUIRE_REPORTER_SSA_AUDIT"] = "1"
        os.environ["XYO_RT_NATIVE_DEOPT_ONCE"] = "1"
        os.environ["XYO_RT_NATIVE_DEOPT_AFTER"] = "1"
        os.environ["XYO_RT_TEST_DEOPT_ROOT_AFTER_LOOP_ONLY"] = "1"
        strict_state, _, strict_log = execute(
            args.native, project, "eager", False)
        assert strict_state == traced, (strict_state, traced)
        scheduler = SCHEDULER_JOIN.findall(strict_log)
        scheduler_cached = SCHEDULER_REPORTED_ENTRY_MAP.findall(strict_log)
        assert len(scheduler_cached) == len(scheduler), (
            scheduler_cached, scheduler, strict_log[-4200:])
        assert all(int(mapped) == int(inputs)
                   for _, mapped, inputs, _, _ in scheduler_cached), (
                       scheduler_cached, strict_log[-4200:])
        scheduler_inputs = SCHEDULER_INPUT_SLOTS.findall(strict_log)
        assert len(scheduler_inputs) == len(scheduler), (
            scheduler_inputs, scheduler, strict_log[-4200:])
        assert all(int(unrooted) == 0 and int(ambiguous) == 0
                   for _, _, _, _, unrooted, ambiguous
                       in scheduler_inputs), scheduler_inputs
        scheduler_owners = SCHEDULER_OWNERS.findall(strict_log)
        assert scheduler_owners and len(scheduler_owners) == len(scheduler), (
            scheduler_owners, scheduler, strict_log[-4500:])
        assert all(int(matched) == 1 and int(stable) == 1 and
                   int(missing) == 0 and int(wrong_type) == 0 and
                   int(unresolved) == 0
                   for _, matched, stable, _, _, missing, wrong_type,
                       unresolved in scheduler_owners), scheduler_owners
        assert scheduler, strict_log[-4500:]
        assert all(int(unmapped) > 0 and int(unsupported) > 0
                   for _, _, unmapped, unsupported in scheduler), scheduler
        adoption = re.findall(
            r"\[native-simple-deopt\] script=\S+ adopted=(\d)",
            strict_log)
        assert adoption and all(item == "0" for item in adoption), (
            "unsafe source reporter restored", adoption, strict_log[-4500:])
        assert RUNTIME_JOIN.findall(strict_log)

        # A repaired/unchanged LLVM SSA fingerprint must NOT make a
        # stale Scratch variable-owner generation acceptable. Only
        # the detached diagnostic owner epoch is corrupted here; the
        # real Target still owns the same variables and native state.
        os.environ["XYO_RT_TEST_CORRUPT_LLVM_SSA"] = (
            "reporter-owner-epoch")
        stale_state, _, stale_log = execute(
            args.native, project, "eager", False)
        assert stale_state == traced, (stale_state, traced)
        stale_owners = SCHEDULER_OWNERS.findall(stale_log)
        assert stale_owners and all(
            int(matched) == 1 and int(stable) == 0
            for _, matched, stable, *_ in stale_owners
        ), (stale_owners, stale_log[-4500:])
        stale_adoption = re.findall(
            r"\[native-simple-deopt\] script=\S+ adopted=(\d)",
            stale_log)
        assert stale_adoption and all(item == "0"
                                      for item in stale_adoption), (
            stale_adoption, stale_log[-4500:])
        os.environ.pop("XYO_RT_TEST_CORRUPT_LLVM_SSA")

        for key in (
            "XYO_RT_REQUIRE_REPORTER_SSA_AUDIT",
            "XYO_RT_NATIVE_DEOPT_ONCE",
            "XYO_RT_NATIVE_DEOPT_AFTER",
            "XYO_RT_TEST_DEOPT_ROOT_AFTER_LOOP_ONLY",
        ):
            os.environ.pop(key, None)

        os.environ.pop("XYO_JIT_SSA_LIVENESS")
        os.environ.pop("XYO_RT_CAPTURE_LLVM_SSA")

        os.environ.pop("XYO_IR001_TRACE_SOURCE_VALUES")
        baseline, _, off_log = execute(args.native, project, "eager", False)
        assert traced == baseline, (traced, baseline)
        assert not LINE.findall(off_log)
        assert not SUMMARY.findall(off_log)
        assert not COMPILER.findall(off_log)
        assert not GAP.findall(off_log)
        assert not GAP_SUMMARY.findall(off_log)
        assert not GAP_ITEM.findall(off_log)
        assert not RUNTIME_PATH.findall(off_log)
        nested_off, nested_off_stats, nested_off_log = execute(
            args.native, nested, "eager", False)
        assert nested_traced == nested_off
        assert nested_stats == nested_off_stats
        assert not GAP.findall(nested_off_log)
        assert not LINE.findall(nested_off_log)
        assert not RUNTIME_PATH.findall(nested_off_log)
        print("PASS IR-001 production Scratch reporter -> LLVM Value* "
              "source ID/PC provenance, boxed numeric gap and "
              "diagnostic-on/off state parity")
    finally:
        for key, value in original.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


if __name__ == "__main__":
    main()
