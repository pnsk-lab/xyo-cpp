#!/usr/bin/env python3
"""RT-001: real wait-until Fibers are explicitly NOT direct yield edges.

A Scratch wait-until yields inside a helper's own frame:
  xyo_wait_yield or xyo_wait_event_yield -> xyo_yield -> Fiber::yield
The original xyo_script_* callsite cannot be certified by a direct
xyo_yield StackMap. We must report it as UNMAPPED, without allowing GC.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from event_scheduler import append_wait_script
from native_simple_resume import ROOT
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set, gt, make_project,
    num, var, write_sb3,
)

UNMAPPED = re.compile(
    r"^\[rt001-yield-edge-unmapped\] tier=(eager|lazy) "
    r"fn=(\S+) ordinal=(\d+) helper=(\S+) "
    r"reason=(\S+) machine_roots_proven=(\d+) gc_safe=(\d+)$",
    re.MULTILINE,
)
COVERAGE = re.compile(
    r"^\[rt001-yield-edge-coverage\] tier=(eager|lazy) "
    r"direct_mapped=(\d+) direct_refused=(\d+) "
    r"nested_or_unmapped=(\d+) machine_roots_proven=(\d+) "
    r"gc_safe=(\d+)$",
    re.MULTILINE,
)


def execute(native: Path, sb3: Path, tier: str,
            event: bool, probe: bool) -> tuple[dict, str]:
    # Never reuse a caller's global experimental GC flags.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("XYO_")}
    env.update({
        "XYO_EVENT_SCHED": "1" if event else "0",
        # Linux defaults the isolated dependency watcher to ON
        # even when the global event scheduler is OFF. To exercise
        # both *actual* helper ABIs we must disable both features
        # for the regular-wait branch.
        "XYO_WAIT_DEP_FAST": "1" if event else "0",
        "XYO_JIT_LAZY_MODE": "on" if tier == "lazy" else "off",
        "XYO_RT_YIELD_EDGE_STACKMAP_PROBE": "1" if probe else "0",
        "XYO_RT_MACHINE_STACKMAP_PROBE": "0",
        "XYO_RT_ORC_FDE_REGISTRATION": "0",
        "XYO_PAR": "0",
        "XYO_PARFOR": "0",
        "XYO_GPU": "0",
        "XYO_JIT_ASYNC": "0",
        "XYO_JIT_CALLFRAME": "1",
        "XYO_DUMPVARS": "json",
        "XYO_EXIT_ON_DONE": "1",
    })
    proc = subprocess.run(
        [str(native), "run", str(sb3), "--jit",
         "off" if tier == "off" else "eager",
         "--frames", "300"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=60,
    )
    if proc.returncode:
        raise AssertionError(
            f"{tier}/event={event}: native CLI return "
            f"{proc.returncode}: {proc.stderr[-1200:]}"
        )
    start, stop = proc.stdout.find("{"), proc.stdout.rfind("}")
    if start < 0 or stop <= start:
        raise AssertionError(f"missing Scratch state: {proc.stdout[-500:]}")
    return json.loads(proc.stdout[start:stop+1]), proc.stderr


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    sb3 = out / "rt001_wait_until_writer.sb3"
    # Force a genuine ORC Lazy materialization path by including a
    # separately compiled custom procedure. A pure two-hat script
    # without procedures may legitimately stay on the eager tier.
    gen = Gen(["ready", "progress", "done"], [])
    gen.define("waitprobe noop", [], [
        S_change("progress", num(0)),
    ], warp=False)
    append_wait_script(
        gen, gt(var("ready"), num(0)), x=0
    )
    gen.script([
        S_call("waitprobe noop", []),
        S_repeat(num(80), [S_change("progress", num(1))]),
        S_set("ready", num(1)),
    ], x=300)
    write_sb3(sb3, make_project(gen.vars.keys(), gen.lists.keys(), gen))
    native = args.native.resolve()

    for event in (False, True):
        expected, _ = execute(native, sb3, "off", event, False)
        helper = "xyo_wait_event_yield" if event else "xyo_wait_yield"
        for tier in ("eager", "lazy"):
            baseline, control = execute(native, sb3, tier, event, False)
            if baseline != expected or "[rt001-yield-edge" in control:
                raise AssertionError(
                    f"{tier}/event={event}: instrumentation-OFF "
                    "changed runtime behavior or emitted a probe"
                )
            result, diagnostic = execute(
                native, sb3, tier, event, True
            )
            if result != expected:
                raise AssertionError(
                    f"{tier}/event={event}: instrumentation changed "
                    "real Scratch scheduler state"
                )
            unmapped = UNMAPPED.findall(diagnostic)
            coverage = COVERAGE.findall(diagnostic)
            if not unmapped or not coverage:
                raise AssertionError(
                    f"{tier}/event={event}: lost yield hazard census"
                )
            matches = [
                item for item in unmapped
                if item[0] in ("eager", "lazy") and
                item[3] == helper and
                item[4:] == ("nested_helper", "0", "0")
            ]
            if not matches:
                raise AssertionError(
                    f"{tier}/event={event}: {helper} not reported "
                    f"as uncertified nested native suspension: {unmapped}"
                )
            if any(
                (item[0] not in ("eager", "lazy") or
                 item[5:] != ("0", "0"))
                for item in unmapped
            ) or any(
                item[0] not in ("eager", "lazy") or
                item[-2:] != ("0", "0")
                for item in coverage
            ):
                raise AssertionError(
                    "unmapped wait helper unexpectedly authorized GC"
                )
            if not any(
                item[0] in ("eager", "lazy") and
                int(item[3]) > 0
                for item in coverage
            ):
                raise AssertionError("no uncovered yield boundary reported")
            if tier == "lazy" and not any(
                item[0] == "lazy" and int(item[1]) > 0
                for item in coverage
            ):
                raise AssertionError(
                    "ORC Lazy was requested but never materialized "
                    "a live script in this combined real fixture"
                )
            print(
                f"PASS RT-001 wait-until: request={tier} "
                f"actual_wait_tier={matches[0][0]} "
                f"event={int(event)} helper={helper} "
                f"reported_unmapped={len(matches)} "
                "Interpreter parity, machine_roots_proven=0 gc_safe=0",
                flush=True,
            )


if __name__ == "__main__":
    main()
