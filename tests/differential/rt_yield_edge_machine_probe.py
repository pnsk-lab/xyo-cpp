#!/usr/bin/env python3
"""RT-001: actual ORC Eager and Lazy yield-edge StackMap position oracle.

The machine StackMap is an instruction-address *diagnostic*, NOT a GC
root-completeness proof. The real paused Fiber ABI is tested separately.
"""
from __future__ import annotations

import argparse
import os
import json
import re
import subprocess
from pathlib import Path

from native_simple_resume import ROOT, execute, fixture
from rt_yield_edge_fiber_pc import check_emitted


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--readobj", default="llvm-readobj")
    parser.add_argument("--objdump", default="llvm-objdump")
    parser.add_argument(
        "--fast", action="store_true",
        help="Exercise the real xyo_yield_fast ABI with CallFrames OFF",
    )
    args = parser.parse_args()

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    native = args.native.resolve()
    project = fixture(out, "ssa_budget_cross_fiber_two_hats")
    relevant = (
        "XYO_JIT_LAZY_MODE", "XYO_RT_YIELD_EDGE_STACKMAP_PROBE",
        "XYO_RT_MACHINE_STACKMAP_PROBE", "XYO_JIT_DUMP_OBJECT",
        "XYO_RT_ORC_FDE_REGISTRATION",
    )
    previous = {name: os.environ.get(name) for name in relevant}
    try:
        os.environ["XYO_RT_YIELD_EDGE_STACKMAP_PROBE"] = "0"
        os.environ["XYO_RT_MACHINE_STACKMAP_PROBE"] = "0"
        os.environ["XYO_RT_ORC_FDE_REGISTRATION"] = "0"
        interpreter, _ = execute(native, project, "off", None)
        # Negative control: yield-edge instrumentation is truly OFF by
        # default and creates neither source markers nor a machine
        # StackMap section in the real JIT ELF.
        default_dir = out / "default_off"
        default_dir.mkdir(exist_ok=True)
        for file in default_dir.glob("*.o"):
            file.unlink()
        os.environ["XYO_JIT_LAZY_MODE"] = "off"
        os.environ["XYO_JIT_DUMP_OBJECT"] = str(
            (default_dir / "edge").resolve()
        )
        default, default_log = execute(
            native, project, "eager", None,
            llvm_ssa_liveness=True, detached_llvm_ssa=True,
            lazy_typed_ssa=True, require_generated_ssa_roots=True,
            soft_gc_budget_mb=1,
        )
        if default != interpreter or "[rt001-yield-edge-stackmap]" in (
            default_log
        ):
            raise AssertionError("yield-edge OFF changed runtime behavior")
        baseline_objs = sorted(default_dir.glob("*.o"))
        if len(baseline_objs) != 2:
            raise AssertionError("missing default OFF ORC ELF baseline")
        import subprocess
        for obj in baseline_objs:
            sections = subprocess.run(
                [args.readobj, "--sections", str(obj)],
                capture_output=True, text=True, check=True,
            )
            if ".llvm_stackmaps" in sections.stdout:
                raise AssertionError(
                    "yield-edge OFF unexpectedly emitted .llvm_stackmaps"
                )
        for tier in ("eager", "lazy"):
            output_dir = out / tier
            output_dir.mkdir(exist_ok=True)
            for file in output_dir.glob("*.o"):
                file.unlink()
            os.environ["XYO_RT_YIELD_EDGE_STACKMAP_PROBE"] = "1"
            os.environ["XYO_JIT_LAZY_MODE"] = (
                "on" if tier == "lazy" else "off"
            )
            os.environ["XYO_JIT_DUMP_OBJECT"] = str(
                (output_dir / "edge").resolve()
            )
            if args.fast:
                # The native_simple_resume.execute helper enables
                # CallFrames unconditionally. Test the REAL fast-path
                # ABI instead by invoking the same CLI with CallFrames=0.
                run_env = dict(os.environ)
                run_env.update(
                    XYO_JIT_CALLFRAME="0",
                    XYO_JIT_SSA_LIVENESS="0",
                    XYO_RT_CAPTURE_LLVM_SSA="0",
                    XYO_RT_REQUIRE_GENERATED_SSA_ROOTS="0",
                    XYO_DUMPVARS="json",
                    XYO_EXIT_ON_DONE="1",
                    XYO_PAR="0",
                )
                run = subprocess.run(
                    [str(native), "run", str(project), "--jit", "eager",
                     "--fps", "30", "--frames", "100"],
                    cwd=ROOT, env=run_env, capture_output=True,
                    text=True, timeout=120,
                )
                if run.returncode:
                    raise AssertionError(
                        f"{tier} fast: CLI exited {run.returncode}: "
                        f"{run.stderr[-1500:]}"
                    )
                match = re.search(
                    r'(?s)(\{\s*"Bench".*\})', run.stdout
                )
                if not match:
                    raise AssertionError("no fast-path Scratch JSON")
                actual, trace = json.loads(match.group(1)), run.stderr
                if "helper=xyo_yield_fast" not in trace:
                    raise AssertionError(
                        f"{tier}: fast native yield helper wasn't called"
                    )
            else:
                actual, trace = execute(
                    native, project, "eager", None,
                    llvm_ssa_liveness=True, detached_llvm_ssa=True,
                    lazy_typed_ssa=True, require_generated_ssa_roots=True,
                    soft_gc_budget_mb=1,
                )
            if actual != interpreter:
                raise AssertionError(
                    f"{tier}: StackMap changed Scratch behavior"
                )
            pairs, returns, register_witnesses = check_emitted(
                output_dir, trace, args.readobj, args.objdump, tier=tier
            )
            if len(pairs) < 4 or len(register_witnesses) != len(pairs):
                raise AssertionError(
                    f"{tier}: incomplete machine yield-edge census"
                )
            print(
                f"PASS RT-001 {tier}: {len(pairs)} real machine "
                "yield-edge StackMaps paired with exact x86-64 call "
                f"relocations (fast={'on' if args.fast else 'off'}); "
                "Scratch Interpreter parity; GC prohibited"
            )
    finally:
        for key, original in previous.items():
            if original is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = original


if __name__ == "__main__":
    main()
