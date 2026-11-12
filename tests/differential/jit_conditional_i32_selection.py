#!/usr/bin/env python3
"""Verify MATH-019 selection through optimized LLVM IR, not only parity."""

import json
import os
import random
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_repeat, S_set,
    add, eq, gt, mod, make_project, num, sub, text, var, write_sb3,
)

from jit_conditional_repeat_ssa import conditional_body


def make_body(kind: int):
    if kind not in (10, 21, 22):
        return conditional_body(kind)
    # Keep the expression parser under its 48-node / depth-8 limits.
    # With a single modulo of a balanced addition tree, 21 leaves fit
    # the static i32 bound while 22 leaves exceed INT32_MAX.
    terms = [var("index") for _ in range(kind)]
    while len(terms) > 1:
        terms = [
            add(terms[i], terms[i + 1]) if i + 1 < len(terms) else terms[i]
            for i in range(0, len(terms), 2)
        ]
    if kind in (21, 22):
        return [
            S_change("index", num(1)),
            S_if(eq(mod(terms[0], num(3)), num(0)),
                 [S_change("acc", num(1))]),
        ]
    return [
        *conditional_body(8),
        S_if(gt(terms[0], num(0)), [S_change("acc", num(1))]),
    ]


def create_fixture(path: Path, kind: int, index=None, acc=None,
                   repeat_count: int = 130,
                   warp: bool = True,
                   dynamic_count: bool = False,
                   count_expression: str = "variable") -> None:
    g = Gen(["index", "acc", "done"] + (["limit"] if dynamic_count else []), [])
    body = [
        S_set("index", index if index is not None else num(0)),
        S_set("acc", acc if acc is not None else num(0)),
    ]
    if dynamic_count:
        assert count_expression in ("variable", "add", "subtract")
        # Preserve the desired repeat count, but make the expression
        # itself genuinely dynamic (no compile-time literal count).
        count_offset = 1 if count_expression == "add" else (
            -1 if count_expression == "subtract" else 0
        )
        body.append(S_set("limit", num(repeat_count - count_offset)))
        count_input = (
            add(var("limit"), num(1)) if count_expression == "add"
            else sub(var("limit"), num(1)) if count_expression == "subtract"
            else var("limit")
        )
    else:
        count_input = num(repeat_count)
    body.extend([
        S_repeat(count_input, make_body(kind)),
        S_set("done", num(1)),
    ])
    g.define("run", [], body, warp=warp)
    g.script([S_call("run")])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))


def run(binary: Path, project: Path, enabled: bool, opt_level: int,
        tier: str = "eager", trace_guard: bool = False,
        i32_setting: str = "auto", trace_setting: str = "auto") -> tuple[dict, str]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("XYO_")}
    env.update({
        "XYO_JIT_CONDITIONAL_SSA": "1",
        "XYO_JIT_CONDITIONAL_UREM": "1",
        "XYO_JIT_CONDITIONAL_I32": str(int(enabled)),
        "XYO_JIT_CONDITIONAL_I32_TRACE": str(int(trace_guard)),
        "XYO_JIT_DUMP2": "1",
        "XYO_JIT_OPT_LEVEL": str(opt_level),
        "XYO_DUMPVARS": "json",
        "XYO_GPU": "0",
        "XYO_PAR": "0",
        "XYO_PARFOR": "0",
    })
    if i32_setting != "auto":
        if i32_setting == "unset":
            env.pop("XYO_JIT_CONDITIONAL_I32", None)
        else:
            env["XYO_JIT_CONDITIONAL_I32"] = i32_setting
    if trace_setting != "auto":
        if trace_setting == "unset":
            env.pop("XYO_JIT_CONDITIONAL_I32_TRACE", None)
        else:
            env["XYO_JIT_CONDITIONAL_I32_TRACE"] = trace_setting
    proc = subprocess.run(
        [str(binary), "run", str(project), "--jit", tier,
         "--frames", "1000"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=90,
    )
    if proc.returncode:
        raise AssertionError(
            f"{project.name} i32={enabled} exit={proc.returncode}: "
            f"{proc.stderr[-1000:]}"
        )
    result = json.loads(
        proc.stdout[proc.stdout.index("{"):proc.stdout.rindex("}") + 1]
    )["Bench"]
    return result, proc.stderr


def assert_native_width(ir: str, bits: int, expected_moduli: bool) -> None:
    # Named SSA nodes distinguish the optimized conditional loop from
    # unrelated LLVM i32 instructions (including Scratch repeat counters).
    for name in ("iter", "var"):
        assert re.search(
            rf"%rep\.cond\.{name}(?:\d+)? = phi i{bits}\b", ir
        ), f"missing conditional {name} PHI i{bits}"
    assert not re.search(
        rf"%rep\.cond\.(?:iter|var)(?:\d+)? = phi i{96 - bits}\b", ir
    ), f"unexpected conditional PHI i{96 - bits}"
    if expected_moduli:
        assert len(re.findall(
            rf"%rep\.cond\.urem\d* = urem i{bits}\b", ir
        )) == 3, f"expected exactly three conditional urem i{bits}"


def main() -> None:
    binary = (Path(sys.argv[1]) if len(sys.argv) > 1
              else ROOT / "build/xyo").resolve()
    output = ROOT / "build/math019-ir-selection"
    output.mkdir(parents=True, exist_ok=True)
    for kind, name in (
        (8, "eligible-flat"),
        (10, "eligible-bounded-expression"),
        (21, "eligible-expression-boundary"),
        (22, "ineligible-expression-boundary"),
        (7, "ineligible-signed-modulo"),
        (1, "ineligible-ifelse"),
    ):
        project = output / f"{name}.sb3"
        create_fixture(project, kind)
        baseline = None
        for opt_level in (2, 3):
            for enabled in (False, True):
                variables, ir = run(binary, project, enabled, opt_level)
                width = 32 if enabled and kind in (8, 10, 21) else 64
                assert_native_width(
                    ir, width, expected_moduli=kind in (8, 10)
                )
                if kind in (21, 22):
                    # Distinguish an i64 SSA width fallback from total
                    # rejection by the expression parser. The one modulo
                    # must be emitted inside this SSA chunk at that width.
                    assert "rep.cond.ssa.head" in ir
                    assert len(re.findall(
                        rf"%rep\.cond\.urem\d* = urem i{width}\b", ir
                    )) == 1, f"expected one SSA modulo i{width}"
                assert variables["done"] == "1", (name, enabled, variables)
                if baseline is not None:
                    assert variables == baseline, (name, baseline, variables)
                baseline = variables
                print(f"PASS {name} O{opt_level} i32={int(enabled)} "
                      f"conditional SSA={width} bits, parity", flush=True)


    trace_guard = os.environ.get("XYO_MATH019_VERIFY_GUARD_TRACE") == "1"
    optin_flag_matrix(binary, output, trace_guard=trace_guard)
    guard_matrix(binary, output, trace_guard=trace_guard)
    dynamic_count_matrix(binary, output, trace_guard=trace_guard)
    if trace_guard:
        chunk_boundary_matrix(binary, output)
        randomized_chunk_matrix(binary, output)

def optin_flag_matrix(binary: Path, output: Path,
                      trace_guard: bool) -> None:
    # Both opt-in flags must match the literal "1", not merely start
    # with 1. Unknown, empty or absent values must be fail-closed.
    project = output / "optin-flags.sb3"
    create_fixture(project, 8)
    expected, _ = run(binary, project, False, 2, tier="off")
    settings = ("unset", "", "0", "00", "2", "10", "1garbage", "1")
    for opt_level in (2, 3):
        for setting in settings:
            actual, ir = run(binary, project, True, opt_level,
                             i32_setting=setting)
            assert actual == expected, (opt_level, setting, actual)
            bits = 32 if setting == "1" else 64
            assert_native_width(ir, bits, expected_moduli=True)
            assert not trace_events(ir), (setting, "unexpected trace")
            print(f"PASS i32 opt-in {setting!r} O{opt_level} "
                  f"phi=i{bits} parity", flush=True)
        if trace_guard:
            for setting in ("unset", "", "0", "10", "1garbage", "1"):
                actual, ir = run(binary, project, True, opt_level,
                                 i32_setting="1", trace_setting=setting)
                assert actual == expected, (opt_level, setting, actual)
                assert_native_width(ir, 32, expected_moduli=True)
                events = trace_events(ir)
                if setting == "1":
                    assert events, (setting, "missing runtime trace")
                else:
                    assert not events, (setting, "unexpected runtime trace")
                    assert "[math019-i32]" not in ir
                    assert not re.search(r"\bcall i64 @write\(", ir)
                print(f"PASS trace opt-in {setting!r} O{opt_level} "
                      f"events={len(events)} parity", flush=True)


def guard_matrix(binary: Path, output: Path, trace_guard: bool) -> None:
    # Runtime guard inputs are deliberately independent of the static
    # shape (kind=8). A selected i32 IR must still behave correctly
    # when an invocation does not satisfy its entry assumptions.
    values = (
        ("zero", num(0), num(0)),
        ("negative", num(-1), num(0)),
        ("negative-zero", num(-0.0), num(0)),
        ("fraction", num(1.5), num(0)),
        ("string", text("abc"), num(0)),
        ("numeric-string", text("7"), num(0)),
        ("crossing-boundary", num(1048574), num(0)),
        ("first-rejected", num(1048575), num(0)),
        ("boundary", num(1048576), num(0)),
        ("above-boundary", num(1048577), num(0)),
        ("large", num(2**40), num(0)),
        ("acc-fraction", num(0), num(0.5)),
        ("acc-outside", num(0), num(1048577)),
    )
    for label, index, acc in values:
        project = output / f"guard-{label}.sb3"
        create_fixture(project, 8, index=index, acc=acc)
        expected, _ = run(binary, project, False, 2, tier="off")
        if trace_guard and label == "zero":
            off_result, off_ir = run(
                binary, project, False, 2, trace_guard=True
            )
            assert off_result == expected
            assert "[math019-i32]" not in off_ir, (
                "i32 OFF unexpectedly emitted guard trace"
            )
            untraced_result, untraced_ir = run(
                binary, project, True, 2, trace_guard=False
            )
            assert untraced_result == expected
            assert "[math019-i32]" not in untraced_ir, (
                "trace disabled but diagnostic emitted"
            )
            # The opt-in must not merely suppress output: it must
            # omit diagnostic calls from optimized LLVM IR entirely.
            assert not re.search(r"\bcall i64 @write\(", untraced_ir), (
                "trace disabled but diagnostic call remains in IR"
            )
        for opt_level in (2, 3):
            actual, ir = run(binary, project, True, opt_level,
                             trace_guard=trace_guard)
            assert_native_width(ir, 32, expected_moduli=True)
            assert "rep.cond.fast" in ir and "rep.cond.slow" in ir
            assert actual == expected, (label, opt_level, expected, actual)
            if not trace_guard:
                print(f"PASS guard {label} O{opt_level} parity", flush=True)
                continue
            # Count emitted runtime records, not the string constants
            # embedded in the LLVM IR dump.
            events = trace_events(ir)
            fast = events.count("fast")
            slow = events.count("slow")
            assert fast + slow > 0, (label, opt_level, "no trace events")
            if label == "zero":
                assert fast > 0, (label, fast, slow)
            elif label == "crossing-boundary":
                # Guard checks the state at each chunk, not only at
                # procedure entry. One eligible chunk can make the
                # updated value too large for the next chunk.
                assert fast > 0 and slow > 0, (label, fast, slow)
                assert events.index("fast") < events.index("slow", events.index("fast")), (
                    label, events
                )
            elif label in ("negative", "negative-zero",
                           "string", "numeric-string"):
                # The initial state is rejected, but ordinary Scratch
                # operations can subsequently produce eligible numbers.
                assert slow > 0 and fast > 0, (label, fast, slow)
            else:
                # Includes 2^20-1: the first scalar update (+1)
                # already crosses the eligible entry window.
                # Fractional state stays fractional or its integer value
                # exceeds the bound throughout the recurrence.
                assert fast == 0 and slow > 0, (label, fast, slow)
            print(f"PASS guard {label} O{opt_level} "
                  f"fast={fast} slow={slow}, parity", flush=True)



def trace_events(ir: str) -> list[str]:
    # The optimized LLVM dump embeds marker literals. Only count actual
    # runtime lines, never the marker strings inside the IR itself.
    return [
        line.removeprefix("[math019-i32] ")
        for line in ir.splitlines()
        if line.startswith("[math019-i32] ")
    ]


def chunk_boundary_matrix(binary: Path, output: Path) -> None:
    # This covers the scheduler's 64-tick split without assuming that
    # the first two iterations are eligible for the native i32 path.
    # Each string is a sequence of *executed* entry guard branches.
    sequences = {
        1: "S",
        2: "SS",
        31: "SSF",
        63: "SSF",
        64: "SSF",
        65: "SSFF",
        127: "SSFF",
        128: "SSFF",
        129: "SSFFF",
        130: "SSFFF",
        191: "SSFFF",
    }
    for warp in (True, False):
        for count, warp_trace in sequences.items():
            expected_trace = warp_trace if warp else "S" * count
            project = output / f"chunk-{'warp' if warp else 'nonwarp'}-{count}.sb3"
            create_fixture(project, 8, index=num(0), acc=num(0),
                           repeat_count=count, warp=warp)
            expected, _ = run(binary, project, False, 2, tier="off")
            traces = []
            for level in (2, 3):
                actual, ir = run(binary, project, True, level,
                                 trace_guard=True)
                assert actual == expected, (
                    warp, count, level, expected, actual
                )
                # The selected i32 IR is present even for non-warp,
                # but its scheduler eligibility guard rejects batching.
                if count > 1:
                    assert_native_width(ir, 32, expected_moduli=True)
                else:
                    # At one iteration, LLVM folds away the loop PHIs.
                    assert len(re.findall(
                        r"%rep\.cond\.urem\d* = urem i32\b", ir
                    )) == 3
                events = trace_events(ir)
                assert set(events) <= {"fast", "slow"}, (warp, count, events)
                observed = "".join("F" if event == "fast" else "S"
                                   for event in events)
                assert observed == expected_trace, (
                    warp, count, level, expected_trace, observed
                )
                traces.append(observed)
            assert traces[0] == traces[1]
            print(f"PASS chunks {'warp' if warp else 'nonwarp'} "
                  f"count={count} O2/O3 "
                  f"fast={expected_trace.count('F')} "
                  f"slow={expected_trace.count('S')} parity", flush=True)


def randomized_chunk_matrix(binary: Path, output: Path) -> None:
    # Deterministic adversarial cross-product sampling, rather than a
    # benchmark: exercise changing eligibility at 2^20, fractional
    # rejection, and 64-tick boundaries in both scheduler modes.
    rng = random.Random(0x019C0DE)
    counts = (1, 2, 3, 31, 32, 63, 64, 65, 66, 127, 128,
              129, 130, 191)
    starts = (-3, -1, 0, 1, 2, 31, 1048570, 1048574,
              1048575, 1048576, 1048577)
    cases = [
        (rng.choice(counts), rng.choice(starts), rng.choice((0, 1, 7)))
        for _ in range(24)
    ]
    # Exactly around the critical threshold / poll boundaries.
    cases.extend([
        (63, 1048574, 0),
        (64, 1048575, 0),
        (65, 1048574, 1),
        (129, 1048576, 7),
    ])
    for i, (count, index, acc) in enumerate(cases):
        for warp in (True, False):
            project = output / (
                f"random-{i}-{'warp' if warp else 'nonwarp'}.sb3"
            )
            create_fixture(project, 8, index=num(index), acc=num(acc),
                           repeat_count=count, warp=warp)
            expected, _ = run(binary, project, False, 2, tier="off")
            sequences = []
            for opt_level in (2, 3):
                actual, ir = run(binary, project, True, opt_level,
                                 trace_guard=True)
                assert actual == expected, (
                    i, warp, count, index, acc, opt_level,
                    expected, actual,
                )
                events = trace_events(ir)
                assert events, (i, warp, count, index, acc)
                assert set(events) <= {"fast", "slow"}
                if not warp:
                    assert events == ["slow"] * count, (
                        i, count, index, acc, events
                    )
                else:
                    # Each fast event must consume >=1 Scratch
                    # iteration; a slow event consumes one iteration.
                    # No execution can enter the guarded chunk more
                    # times than there are loop iterations.
                    assert len(events) <= count, (
                        i, count, index, acc, events
                    )
                sequences.append(events)
            assert sequences[0] == sequences[1], (
                i, warp, count, index, acc, sequences
            )
        print(f"PASS randomized {i + 1}/{len(cases)} count={count} "
              f"index={index} acc={acc} warp/nonwarp O2/O3", flush=True)


def dynamic_count_matrix(binary: Path, output: Path,
                         trace_guard: bool) -> None:
    # A run-time count cannot use the statically bounded conditional SSA.
    # Also test expressions that use the live variable, rather than
    # accepting a constant disguised as a numeric shadow.
    modes = ("variable", "add", "subtract")
    counts = (-2, 0, 1, 2, 2.5, 63, 64, 65, 130)
    for warp in (True, False):
        for mode in modes:
            for count in counts:
                project = output / (
                    f"dynamic-{'warp' if warp else 'nonwarp'}-"
                    f"{mode}-{str(count).replace('.', '_')}.sb3"
                )
                create_fixture(project, 8, repeat_count=count,
                               warp=warp, dynamic_count=True,
                               count_expression=mode)
                expected, _ = run(binary, project, False, 2, tier="off")
                assert expected["done"] == "1"
                for opt_level in (2, 3):
                    actual, ir = run(binary, project, True, opt_level,
                                     trace_guard=trace_guard)
                    assert actual == expected, (
                        warp, mode, count, opt_level, expected, actual
                    )
                    assert "rep.cond.ssa.head" not in ir, (
                        warp, mode, count, opt_level,
                        "unexpected conditional SSA"
                    )
                    assert not re.search(
                        r"%rep\.cond\.(?:iter|var)(?:\d+)? = phi i32\b",
                        ir
                    ), (warp, mode, count, opt_level,
                        "unexpected i32 loop PHI")
                    assert not trace_events(ir), (
                        warp, mode, count, opt_level,
                        "unexpected i32 guard events"
                    )
                print(f"PASS dynamic {'warp' if warp else 'nonwarp'} "
                      f"{mode} count={count} O2/O3 fallback parity",
                      flush=True)


if __name__ == "__main__":
    main()
