#!/usr/bin/env python3
"""Run the complete supplied Scratch parity project alongside a real ORC
lazy par-for script. This opt-in integration suite does not bundle proprietary
or external ScratchBench fixture bytes.

The added sprite is independent of the parity harness. Both executions must
report the same named parity failures, and the opt-in version must actually
materialize its callback functions and dispatch native worker tasks.
"""
from __future__ import annotations

import argparse
import copy
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zipfile

SUMMARY = re.compile(
    r"^\[(?:FAILED|PASSED)\] [^\n]*Tests[^\n]*?:\s*(\d+)/(\d+)",
    re.M,
)
PARTITION = re.compile(r"^\[jit-lazy-partition\] requested=\d+ materialized=\d+ callbacks=2$", re.M)
SELECTED = re.compile(r"^\[jit-lazy-guard\] script=(\S+) reason=parfor-partition-opt-in$", re.M)
FAILED_GROUP = re.compile(r"^\[FAILED\] (.+): (\d+)/(\d+) \(", re.M)
# Older parity fixtures print only "Test n Failed"; newer ones also show
# the full description and actual output. Match both rather than treating
# an unparsed detail as evidence of equivalence.
FAILED_CASE = re.compile(
    r"^[┣┗] Test (\d+) Failed(?:: (.*?)(?: \| got .*)?)?$"
)

# These checks sample the real wall-clock date. Even two runs of the same
# binary can cross a minute boundary and disagree without a JIT regression.
VOLATILE_GROUPS = frozenset({"Sensing: Date And Time"})


def compare_parity(baseline: dict, candidate: dict) -> dict:
    if baseline["total"] != candidate["total"] or baseline["total"] < 2123:
        raise AssertionError("parity check count changed/shrank")
    stable_names = (baseline["failure_groups"] | candidate["failure_groups"]).keys() - VOLATILE_GROUPS
    divergent = [
        name for name in sorted(stable_names)
        if baseline["failure_groups"].get(name) != candidate["failure_groups"].get(name)
    ]
    if divergent:
        raise AssertionError(f"non-clock parity groups changed: {divergent}")
    for name in stable_names:
        if baseline.get("failure_cases", {}).get(name, []) != (
            candidate.get("failure_cases", {}).get(name, [])
        ):
            raise AssertionError(f"individual failed parity cases changed: {name}")
    clock_failures = lambda result: (
        result["failure_groups"].get("Sensing: Date And Time", (18, 18))[1]
        - result["failure_groups"].get("Sensing: Date And Time", (18, 18))[0]
    )
    # A changed total pass count must be fully explained by the time-of-day
    # group. Unknown mismatches cannot be silently masked by this exception.
    expected_delta = clock_failures(baseline) - clock_failures(candidate)
    actual_delta = candidate["passed"] - baseline["passed"]
    if actual_delta != expected_delta:
        raise AssertionError(
            f"unexplained parity change: actual {actual_delta}, "
            f"wall-clock group accounts for {expected_delta}"
        )
    return {
        "stable_failure_groups": len(stable_names),
        "wall_clock_failures_baseline": clock_failures(baseline),
        "wall_clock_failures_partition": clock_failures(candidate),
        "clock_only_difference": expected_delta != 0,
    }


def make_auto_eligible_probe(path: Path) -> None:
    # The normal HotSpot auto-lazy heuristic needs at least four LLVM
    # functions and 3000 pre-optimization IR instructions. Two real
    # par-for bodies supply the functions; inert source statements add
    # enough IR to exercise that same policy without forcing global Lazy.
    from jit_lazy_parfor_partition import (
        Gen, S_add, S_call, S_change, S_repeat, S_set,
        make_project, num, var, write_sb3,
    )
    gen = Gen(["i", "j", "done"], ["a", "b"])
    body = [
        S_set("i", num(0)),
        S_repeat(num(8192), [
            S_change("i", num(1)), S_add("a", var("i")),
        ]),
        S_set("j", num(0)),
        S_repeat(num(8192), [
            S_change("j", num(1)), S_add("b", var("j")),
        ]),
        S_set("done", num(1)),
    ]
    body.extend(S_change("done", num(0)) for _ in range(240))
    gen.define("fill", [], body, warp=True)
    gen.script([S_call("fill")])
    write_sb3(str(path), make_project(gen.vars.keys(), gen.lists.keys(), gen))


def merge_projects(parity_file: Path, parfor_file: Path, output: Path) -> dict:
    with zipfile.ZipFile(parity_file) as z:
        base = json.loads(z.read("project.json"))
        assets = {n: z.read(n) for n in z.namelist() if n != "project.json"}
    with zipfile.ZipFile(parfor_file) as z:
        extra = json.loads(z.read("project.json"))

    assert sum(t.get("isStage", False) for t in base["targets"]) == 1
    assert len(extra["targets"]) == 2
    sprite = copy.deepcopy(extra["targets"][1])
    assert sprite["name"] == "Bench"
    assert not sprite.get("isStage")
    assert sum(b.get("opcode") == "event_whenflagclicked" for b in sprite["blocks"].values()) == 1

    old_ids = set(sprite["blocks"]) | set(sprite["variables"]) | set(sprite["lists"])
    rename = {item: f"parfor_integration_{item}" for item in old_ids}

    def rewrite(item):
        if isinstance(item, dict):
            return {rename.get(key, key): rewrite(value) for key, value in item.items()}
        if isinstance(item, list):
            return [rewrite(value) for value in item]
        if isinstance(item, str):
            return rename.get(item, item)
        return item

    sprite = rewrite(sprite)
    sprite["name"] = "__parfor_lazy_parity_probe__"
    sprite["layerOrder"] = max(t.get("layerOrder", 0) for t in base["targets"]) + 1
    sprite["visible"] = False
    base["targets"].append(sprite)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(base, separators=(",", ":")))
        for name, data in assets.items():
            z.writestr(name, data)
    return {"targets": len(base["targets"]), "probe_blocks": len(sprite["blocks"]),
            "assets": len(assets), "probe_name": sprite["name"]}


def parse_failures(stdout: str) -> tuple[dict[str, tuple[int, int]],
                                         dict[str, list[tuple[int, str]]]]:
    groups = {
        name: (int(passed), int(total))
        for name, passed, total in FAILED_GROUP.findall(stdout)
        if name != "Some Tests Were Not Successful"
    }
    failure_cases: dict[str, list[tuple[int, str]]] = {}
    active_group: str | None = None
    for line in stdout.splitlines():
        match = FAILED_GROUP.match(line)
        if match:
            active_group = match.group(1)
            if active_group == "Some Tests Were Not Successful":
                active_group = None
            continue
        if not line.startswith(("┣", "┗")):
            active_group = None
            continue
        case = FAILED_CASE.match(line)
        if active_group is not None and case:
            failure_cases.setdefault(active_group, []).append(
                (int(case.group(1)), case.group(2) or "")
            )
    for name, (passed, total) in groups.items():
        if len(failure_cases.get(name, [])) != total - passed:
            raise AssertionError(f"failed-case detail incomplete: {name}")
    return groups, failure_cases


def run(binary: Path, project: Path, enable_partition: bool,
        timeout: int) -> dict:
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen", "SDL_AUDIODRIVER": "dummy",
        "XYO_EXIT_ON_DONE": "1", "XYO_PEN_HQ": "0", "XYO_GPU": "0",
        "XYO_PAR": "0", "XYO_PARFOR": "1", "XYO_JIT_MODE": "hotspot",
        "XYO_JIT_LAZY_MODE": "auto", "XYO_HOT_INVOCATIONS": "1",
        "XYO_PARFOR_DEBUG": "1", "XYO_JIT_LAZY_CALLBACK_TRACE": "1",
        "XYO_JIT_ASYNC": "0",
    })
    if enable_partition:
        env["XYO_JIT_LAZY_PARFOR_PARTITION"] = "1"
    args = [str(binary), "play", str(project), "--jit", "hotspot",
            "--jit-lazy", "auto", "--print-list", "output",
            "--list-limit", "200000"]
    proc = subprocess.run(args, env=env, capture_output=True,
                          text=True, timeout=timeout)
    if proc.returncode:
        raise AssertionError(f"exit={proc.returncode}:\n{proc.stderr[-2600:]}")
    summary = SUMMARY.search(proc.stdout)
    if not summary:
        raise AssertionError(f"missing parity summary: {proc.stdout[-1800:]}")
    failures = [line for line in proc.stdout.splitlines()
                if line.startswith("[FAILED]") and
                "Some Tests Were Not Successful" not in line]
    groups, failure_cases = parse_failures(proc.stdout)
    return {
        "passed": int(summary.group(1)), "total": int(summary.group(2)),
        "failures": failures,
        "failure_groups": groups,
        "failure_cases": failure_cases,
        "selected_scripts": SELECTED.findall(proc.stderr),
        "grouped_callbacks": len(PARTITION.findall(proc.stderr)),
        "parfor_parallel_dispatches": len(re.findall(r"^parfor: parallel trip=8192", proc.stderr, re.M)),
        "parfor_sequential_dispatches": len(re.findall(r"^parfor: sequential trip=8192", proc.stderr, re.M)),
        "stderr_tail": proc.stderr[-1500:],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--parity", type=Path, required=True)
    parser.add_argument(
        "--probe", type=Path,
        help="existing 2-callback fixture; generated automatically when omitted",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    binary = args.native.resolve(strict=True)
    parity = args.parity.resolve(strict=True)
    if args.probe:
        probe = args.probe.resolve(strict=True)
    else:
        # Reuse the same generator as the native sequential/worker-pool
        # regression. Keep the probe out of the repository and independent
        # of any external ScratchBench checkout.
        probe = args.output / "two-parfor-auto-8192.sb3"
        probe.parent.mkdir(parents=True, exist_ok=True)
        make_auto_eligible_probe(probe)

    merged = args.output / "parfor-augmented-parity.sb3"
    manifest = merge_projects(parity, probe, merged)
    print("MERGE", json.dumps(manifest), flush=True)

    disabled = run(binary, merged, False, args.timeout)
    print("BASELINE", json.dumps(
        {k: v for k, v in disabled.items() if k not in {"failures", "failure_cases", "stderr_tail"}}),
        flush=True)
    enabled = run(binary, merged, True, args.timeout)
    print("PARTITION", json.dumps(
        {k: v for k, v in enabled.items() if k not in {"failures", "failure_cases", "stderr_tail"}}),
        flush=True)

    result = {"manifest": manifest, "baseline": disabled, "partition": enabled}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "parity-comparison.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    comparison = compare_parity(disabled, enabled)
    print("PARITY_COMPARISON", json.dumps(comparison), flush=True)
    assert enabled["selected_scripts"] and enabled["grouped_callbacks"], (
        "par-for ORC path did not execute", enabled)
    assert enabled["parfor_parallel_dispatches"] >= 2, (
        "par-for native worker pool path did not execute", enabled)
    assert not disabled["selected_scripts"] and not disabled["grouped_callbacks"], (
        "baseline unexpectedly used experimental ORC", disabled)
    print("PASS: parity fixture + real selected native par-for co-partition", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
