#!/usr/bin/env python3
"""End-to-end verification for the source-certified Mega scheduler.

Two layers:

1. `unit`   — xyo-tests with `XYO_JIT_MEGA=1` (the preset). The scheduler
              suite must report zero failures with every Mega feature on.
2. `parity` — for each generated fixture, run the interpreter
              (`XYO_JIT_MODE=off`) and the Mega preset (`XYO_JIT_MEGA=1`)
              and require byte-identical Stage variable dumps.
3. `paired` — run the existing per-feature A/B harnesses, which assert the
              exact expected Scratch state for each optimized shape.

The harnesses intentionally filter `XYO_*` from the parent environment, so
they test each feature in isolation; the `parity` layer is what proves the
one-switch preset composes all of them correctly.

Usage:
  python3 tests/mega/run_mega_suite.py --binary build/xyo
  python3 tests/mega/run_mega_suite.py --binary build/xyo --skip-unit
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
ALGOS = ROOT / "tests" / "bench" / "algos"

# fixture name -> (builder args, quick sanity)
FIXTURES = {
    "yield": (["build_mega_yield_fixture.py"], "mega-yield"),
    "batch": (["build_mega_batch_fixture.py"], "mega-batch"),
    "goto": (["build_mega_goto_fixture.py"], "mega-goto"),
    "ifelse": (["build_mega_ifelse_fixture.py"], "mega-ifelse"),
    "two_scalar": (["build_mega_two_scalar_fixture.py"], "mega"),
    "receiver": (["build_mega_receiver_fixture.py"], "mega"),
    "broadcast": (["build_mega_broadcast_fixture.py"], "mega"),
    "broadcast_repeat": (["build_mega_broadcast_repeat_fixture.py"], "mega"),
}

# fixture used by each paired harness (None = the harness builds its own)
PAIRED = [
    ("bench_mega_yield.py", "yield", []),
    ("bench_mega_batch.py", "batch", []),
    ("bench_mega_goto.py", "goto", ["--expected", "40000"]),
    ("bench_mega_ifelse.py", "ifelse", []),
    ("bench_mega_two_scalar.py", "two_scalar", []),
    ("bench_mega_receiver.py", "receiver", []),
    ("bench_mega_broadcast.py", "broadcast", []),
    ("bench_mega_multi_pc.py", "yield", []),
    ("bench_mega_branch_pair.py", "ifelse_pair", []),
    ("bench_mega_multi_repeat_burst.py", None, []),
    ("bench_mega_outer_until.py", None, []),
    ("bench_mega_single_dispatch.py", None, []),
    ("bench_mega_single_receiver.py", None, []),
]


def run(cmd, *, env=None, cwd=None, timeout=600):
    started = time.perf_counter()
    proc = subprocess.run(
        cmd, cwd=cwd, env=env, text=True, capture_output=True, timeout=timeout
    )
    return proc, time.perf_counter() - started


def build_fixtures(out: Path) -> dict[str, Path]:
    fixtures = out / "fixtures"
    fixtures.mkdir(parents=True, exist_ok=True)
    built: dict[str, Path] = {}
    for name, (builder, _) in FIXTURES.items():
        target = fixtures / f"{name}.sb3"
        proc, _ = run(
            [sys.executable, *builder, "--out", str(target)],
            cwd=ALGOS,
        )
        if proc.returncode != 0 or not target.exists():
            raise RuntimeError(
                f"fixture {name} failed\nstdout={proc.stdout[-2000:]}\n"
                f"stderr={proc.stderr[-2000:]}"
            )
        built[name] = target
    pair = fixtures / "ifelse_pair.sb3"
    proc, _ = run(
        [sys.executable, "build_mega_ifelse_fixture.py", "--out", str(pair),
         "--two-writes"],
        cwd=ALGOS,
    )
    if proc.returncode != 0 or not pair.exists():
        raise RuntimeError(f"ifelse_pair fixture failed: {proc.stderr[-2000:]}")
    built["ifelse_pair"] = pair
    return built


def stage_dump(stdout: str) -> dict | None:
    match = re.search(r"\{\s*\"Stage\"\s*:\s*\{.*?\}\s*\}", stdout, re.S)
    if not match:
        return None
    try:
        return json.loads(match.group())["Stage"]
    except json.JSONDecodeError:
        return None


def base_env(**overrides) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(overrides)
    return env


def unit_layer(binary: Path, out: Path) -> list[str]:
    failures: list[str] = []
    # CMake puts xyo-tests in <build>/tests/; a flat install may place it
    # next to the CLI instead.
    tests = next(
        (p for p in (binary.parent / "xyo-tests",
                     binary.parent / "tests" / "xyo-tests")
         if p.exists()),
        None,
    )
    if tests is None:
        return ["xyo-tests not found next to the binary; run with --skip-unit"]
    env = base_env(XYO_JIT_MEGA="1")
    proc, elapsed = run([str(tests)], env=env, timeout=1800)
    log = out / "unit.log"
    log.write_text(proc.stdout + proc.stderr)
    match = re.search(r"scheduler: (\d+) checks, (\d+) failures", proc.stdout)
    if not match:
        failures.append(f"unit: scheduler summary missing (see {log})")
    else:
        checks, failed = int(match.group(1)), int(match.group(2))
        if failed:
            failures.append(f"unit: scheduler {checks} checks, {failed} failures")
        else:
            print(f"  unit: scheduler {checks} checks, 0 failures ({elapsed:.1f}s)")
    if proc.returncode != 0:
        failures.append(f"unit: xyo-tests exit={proc.returncode} (see {log})")
    return failures


def parity_layer(binary: Path, out: Path, fixtures: dict[str, Path]) -> list[str]:
    failures: list[str] = []
    dumps = out / "parity"
    dumps.mkdir(parents=True, exist_ok=True)
    for name, project in fixtures.items():
        results = {}
        for mode, env in (
            ("interp", base_env(XYO_JIT_MODE="off", XYO_DUMPVARS="json")),
            ("mega", base_env(XYO_JIT_MEGA="1", XYO_DUMPVARS="json")),
        ):
            proc, elapsed = run(
                [str(binary), "run", str(project), "--fps", "0"],
                env=env,
                timeout=300,
            )
            (dumps / f"{name}.{mode}.out").write_text(proc.stdout)
            (dumps / f"{name}.{mode}.err").write_text(proc.stderr)
            state = stage_dump(proc.stdout)
            if proc.returncode != 0 or state is None:
                failures.append(
                    f"parity[{name}/{mode}]: exit={proc.returncode} "
                    f"state={state} (see {dumps}/{name}.{mode}.err)"
                )
                results = {}
                break
            results[mode] = state
            _ = elapsed
        if len(results) != 2:
            continue
        if results["interp"] != results["mega"]:
            failures.append(
                f"parity[{name}]: interp={results['interp']} "
                f"mega={results['mega']}"
            )
        else:
            print(f"  parity: {name} (interp == mega preset)")
    return failures


GENERAL_FIXTURES = [
    "general_loop",
    "general_join",
    "general_broadcast",
    "general_clone",
    "general_join_stress",
    "general_parallel",
    "general_procedure_plain",
    "general_wait",
    "general_wait_until",
    "general_procedure_wait",
    "general_procedure_wait_nested",
    "general_bcastwait",
    "general_bcastwait_none",
    "general_bcastwait_loop",
    "general_wait_until_var",
    "general_glide",
    "general_saysecs",
    "general_backdropwait",
    "general_backdrop_receiver",
    "general_stop_after_loop",
    "general_stop_all_after_loop",
    "general_procedure_warp",
    "general_procedure_warp_loop",
    "general_procedure_warp_empty",
    "general_sound_effects",
    "general_playuntildone",
    "general_deleteclone",
    "general_deleteclone_in_proc",
    "general_procedure_warp_bcastwait",
    "general_clone_stopscript",
    "general_bcast_storm",
    "general_clone_wait",
    "general_recursive",
    "general_recursive_mutual_nonwarp",
    "general_recursive_loop_nonwarp",
    "general_recursive_strings",
    "general_recursive_stopall_nonwarp",
    "general_recursive_stopscript_nonwarp",
    "general_recursive_bcastwait_nonwarp",
    "general_warp_rec_wait",
]

GENERAL_NO_LOWER = [
    "general_stop",
    "general_stop_all",
    "general_stop_all_in_proc",
    "general_stop_all_nested_proc",
    "general_recursive_warp",
    "general_recursive_mutual",
    "general_recursive_stopall",
    "general_recursive_stopscript",
    "general_recursive_warp_loop",
]


def coro_layer(binary: Path, out: Path) -> list[str]:
    """General direct-yield continuations: any loop/broadcast/clone script
    must lower to one coroutine and keep interpreter-identical state."""
    fixtures = out / "general"
    fixtures.mkdir(parents=True, exist_ok=True)
    proc, _ = run(
        [sys.executable, str(ALGOS / "build_mega_general_fixture.py"),
         "--out", str(fixtures)],
        cwd=ALGOS,
    )
    if proc.returncode != 0:
        return [f"general-coro fixtures failed: {proc.stderr[-1500:]}"]
    failures: list[str] = []
    dumps = out / "coro"
    dumps.mkdir(parents=True, exist_ok=True)
    cases = [(name, True) for name in GENERAL_FIXTURES]
    cases += [(name, False) for name in GENERAL_NO_LOWER]
    for name, expect_lower in cases:
        project = fixtures / f"{name}.sb3"
        states: dict[str, dict] = {}
        lowered = False
        modes = (
            ("interp", base_env(XYO_JIT_MODE="off", XYO_DUMPVARS="json")),
            ("coro", base_env(XYO_JIT_MODE="eager",
                              XYO_JIT_CORO_GENERAL="1",
                              XYO_JIT_CORO_TRACE="1",
                              XYO_DUMPVARS="json")),
            # The new begin/poll suspension points are also used by the
            # ordinary Fiber JIT, and HotSpot is the default for play.
            ("fiber", base_env(XYO_JIT_MODE="eager",
                               XYO_DUMPVARS="json")),
            ("hotspot", base_env(XYO_JIT_MODE="hotspot",
                                 XYO_JIT_CORO_GENERAL="1",
                                 XYO_DUMPVARS="json")),
            # The default preset: sync/yield batches plus general coroutines.
            ("mega", base_env(XYO_JIT_MEGA="1",
                              XYO_JIT_MEGA_BATCH_TRACE="1",
                              XYO_DUMPVARS="json")),
        )
        for mode, env in modes:
            proc, _ = run(
                [str(binary), "run", str(project), "--fps", "0"],
                env=env, timeout=300,
            )
            (dumps / f"{name}.{mode}.out").write_text(proc.stdout)
            (dumps / f"{name}.{mode}.err").write_text(proc.stderr)
            state = stage_dump(proc.stdout)
            if proc.returncode != 0 or state is None:
                failures.append(
                    f"coro[{name}/{mode}]: exit={proc.returncode} "
                    f"state={state} (see {dumps}/{name}.{mode}.err)"
                )
                break
            states[mode] = state
            if mode == "coro":
                lowered = "lowered=1" in proc.stderr
        if len(states) != len(modes):
            continue
        mismatch = False
        for mode in ("coro", "fiber", "hotspot", "mega"):
            if states["interp"] != states[mode]:
                mismatch = True
                failures.append(
                    f"coro[{name}]: interp={states['interp']} "
                    f"{mode}={states[mode]}"
                )
        if mismatch:
            continue
        if lowered != expect_lower:
            failures.append(
                f"coro[{name}]: lowered={int(lowered)} expected "
                f"{int(expect_lower)} (see {dumps}/{name}.coro.err)"
            )
        elif name in ("general_recursive_warp", "general_recursive_mutual",
                      "general_recursive_warp_loop",
                      "general_recursive_stopall",
                      "general_recursive_stopscript"):
            mega_err = (dumps / f"{name}.mega.err").read_text()
            if "installed=" not in mega_err:
                failures.append(
                    f"coro[{name}]: recursive warp procedure did not join "
                    f"the sync batch (see {dumps}/{name}.mega.err)"
                )
            else:
                print(f"  coro: {name} (interp == coroutine == fiber == "
                      f"hotspot == mega, sync batch, lowered=0)")
        else:
            print(f"  coro: {name} (interp == coroutine == fiber == "
                  f"hotspot == mega, lowered={int(lowered)})")
    return failures


def paired_layer(binary: Path, out: Path, runs: int) -> list[str]:
    failures: list[str] = []
    harness_out = out / "paired"
    for script, fixture, extra in PAIRED:
        cmd = [
            sys.executable,
            str(ALGOS / script),
            "--binary",
            str(binary),
            "--out",
            str(harness_out / Path(script).stem),
            "--runs",
            str(runs),
        ]
        if fixture:
            cmd += ["--project", str(out / "fixtures" / f"{fixture}.sb3")]
        cmd += extra
        proc, elapsed = run(cmd, cwd=ALGOS, timeout=900)
        stem = Path(script).stem
        if proc.returncode != 0:
            failures.append(
                f"{stem}: exit={proc.returncode}\n{proc.stderr[-1500:]}"
            )
        else:
            summary = " ".join(
                line for line in proc.stdout.splitlines()
                if line.startswith("SUMMARY")
            )
            print(f"  paired: {stem} ok ({elapsed:.1f}s) {summary[:160]}")
    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=ROOT / "build-mega-suite")
    ap.add_argument("--runs", type=int, default=1,
                    help="paired A/B runs per feature (default 1: correctness)")
    ap.add_argument("--skip-unit", action="store_true")
    ap.add_argument("--skip-parity", action="store_true")
    ap.add_argument("--skip-paired", action="store_true")
    ap.add_argument("--skip-coro", action="store_true")
    args = ap.parse_args()

    binary = args.binary.resolve()
    if not binary.exists():
        ap.error(f"binary not found: {binary}")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    failures: list[str] = []
    print("mega suite: building fixtures")
    fixtures = build_fixtures(out)

    if not args.skip_unit:
        print("mega suite: unit")
        failures += unit_layer(binary, out)
    if not args.skip_parity:
        print("mega suite: preset parity (interp vs XYO_JIT_MEGA=1)")
        failures += parity_layer(binary, out, fixtures)
    if not args.skip_coro:
        print("mega suite: general-coro continuations")
        failures += coro_layer(binary, out)
    if not args.skip_paired:
        print(f"mega suite: paired harnesses (runs={args.runs})")
        failures += paired_layer(binary, out, args.runs)

    if failures:
        print(f"\nMEGA SUITE FAILED ({len(failures)})")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("\nMEGA SUITE PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
