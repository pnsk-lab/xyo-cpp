#!/usr/bin/env python3
"""RT-001: real JIT FDEs survive Fiber retirement but NOT Runtime teardown.

Create and destroy two completely independent LLVMBackend/ORC engines
within the same process. Check each live FDE and its exact cancellation;
run both Eager and Lazy, normal completion and stop-all cancellation.
No machine GC permission follows from the results.
"""
from __future__ import annotations

import argparse
from http.server import ThreadingHTTPServer
import os
from pathlib import Path
import re
import subprocess
import threading

from plamo_async_paused_callsite_elf import CALLER, REAL_FDE, UNWIND
from plamo_native_fiber import (
    Handler, lazy_root_fixture, mixed_sequential_reporters_fixture,
)

EMIT = re.compile(
    r"\[rt001-orc-fde\] phase=emit registered=(\d+) gc_safe=0"
)
REMOVE = re.compile(
    r"\[rt001-orc-fde\] phase=remove canceled=(\d+) gc_safe=0"
)
LIVE = "[rt001-orc-fde-index] phase=after_fiber registered=1 gc_safe=0"
DEAD = "[rt001-orc-fde-index] phase=after_runtime registered=0 gc_safe=0"


def one_case(
    smoke: Path, directory: Path, tier: str,
    completion: str, port: int,
) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    project = directory / "two-runtimes.sb3"
    if tier == "lazy":
        lazy_root_fixture(project)
    else:
        mixed_sequential_reporters_fixture(project)
    env = dict(
        os.environ,
        XYO_PLAMO_TRANSLATE="1",
        XYO_PLAMO_PORT=str(port),
        XYO_RT001_TEST_USE_ORC_FDE="1",
        XYO_RT001_TEST_REGISTER_FDE="0",
        XYO_RT001_TEST_RECREATE_RUNTIME="1",
        XYO_RT_ORC_FDE_REGISTRATION="1",
    )
    cmd = [str(smoke), str(project), tier]
    if tier == "eager":
        cmd.append("second")  # the earlier reporter's live i64 in R15
    if completion == "resume":
        cmd.append("resume")
    run = subprocess.run(
        cmd, capture_output=True, text=True, env=env, timeout=40
    )
    assert run.returncode == 0, (
        tier, completion, run.stdout[-1200:], run.stderr[-2300:]
    )
    assert run.stderr.count(LIVE) == 2, (tier, completion, run.stderr)
    assert run.stderr.count(DEAD) == 2, (tier, completion, run.stderr)
    assert run.stdout.count(
        "PASS RT-001 paused physical async reporter caller frame"
    ) == 2, (tier, completion, run.stdout)

    frames = REAL_FDE.findall(run.stderr)
    calls = CALLER.findall(run.stderr)
    unwind = UNWIND.findall(run.stderr)
    assert len(frames) == len(calls) == len(unwind) == 2, (
        tier, completion, len(frames), len(calls), len(unwind)
    )
    for fde, caller, regs in zip(frames, calls, unwind):
        assert fde[0] == caller[0] == regs[0] == tier
        assert fde[2:4] == ("orc", "1"), (tier, completion, fde)
        assert fde[1] == fde[5] == fde[6] == fde[7] == "1"
        assert fde[9:13] == ("6", "6", "1", "1"), fde
        assert fde[14] == "0" and fde[-2:] == ("0", "0")
        assert fde[13] == ("1" if tier == "eager" else "-1")
        assert regs[7] == "1", regs  # JIT registered FDE, no fallback
        assert int(caller[1]) == (2 if tier == "eager" else 1)
        assert caller[-1] == "0"

    emit = [int(v) for v in EMIT.findall(run.stderr)]
    remove = [int(v) for v in REMOVE.findall(run.stderr)]
    assert emit and remove and sum(emit) == sum(remove), (
        tier, completion, emit, remove
    )
    assert "phase=emit fde_missing=1" not in run.stderr
    assert "phase=emit invalid_extent=1" not in run.stderr
    print(
        f"PASS RT-001 two ORC Runtimes: tier={tier} exit={completion} "
        f"generations=2 FDE_emitted={sum(emit)} "
        f"FDE_canceled={sum(remove)} JIT_CFI=1 gc_safe=0",
        flush=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--smoke", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        for tier in ("eager", "lazy"):
            for completion in ("cancel", "resume"):
                one_case(
                    args.smoke.resolve(), args.out.resolve() / tier /
                    completion, tier, completion, srv.server_port,
                )
    finally:
        srv.shutdown()
        srv.server_close()


if __name__ == "__main__":
    main()
