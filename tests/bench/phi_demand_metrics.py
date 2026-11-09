#!/usr/bin/env python3
"""Compare actual optimized LLVM IR, JIT .text and compilation time for PHI modes.

Run: python3 tests/bench/phi_demand_metrics.py --runs 3
     python3 tests/bench/phi_demand_metrics.py --projects path/to/a.sb3 --runs 5

Uses existing JIT diagnostics. Measurement runs never dump IR/objects (which
would distort compilation timing). A separate inspection run records the LLVM
post-optimization IR and the ORC-produced native object.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PROJECTS = [
    "build/if-phi-fixtures/warp.sb3",
    "tests/bench/algos/out/quicksort_4000.sb3",
    "tests/bench/algos/out/mergesort_4000.sb3",
    "tests/bench/algos/out/sieve_100000.sb3",
    "tests/bench/algos/out/fib_27.sb3",
    "tests/bench/algos/out/micro_loop_1000000.sb3",
]
MODES = (
    ("baseline", {"XYO_JIT_IF_PHI": "0", "XYO_JIT_PHI_DEMAND": "0"}),
    ("all_phi", {"XYO_JIT_IF_PHI": "1", "XYO_JIT_PHI_DEMAND": "0"}),
    ("demand", {"XYO_JIT_IF_PHI": "1", "XYO_JIT_PHI_DEMAND": "1"}),
)
LOCAL_READ_MODES = (
    ("read_off", {"XYO_JIT_LOCAL_READ_FORWARD": "0"}),
    ("read_on", {"XYO_JIT_LOCAL_READ_FORWARD": "1"}),
)
DUPLICATE_SCALAR_MODES = (
    ("scalar_off", {"XYO_JIT_DUPLICATE_SCALAR_NUM": "0"}),
    ("scalar_on", {"XYO_JIT_DUPLICATE_SCALAR_NUM": "1"}),
)
DUPLICATE_SCALAR_RE = re.compile(
    r"^\[jit-duplicate-scalar\] (\S+) pairs=(\d+) self_changes=(\d+)$",
    re.MULTILINE,
)
CONVERSION_CALL_RE = re.compile(r"\bcall double @xyo_to_num\(")

LOCAL_READ_RE = re.compile(
    r"^\[jit-local-read\] (\S+) reuses=(\d+)$", re.MULTILINE,
)
TIMING_RE = re.compile(
    r"^\[jit-timing\] (\S+) emit=([\d.]+)ms opt=([\d.]+)ms "
    r"submit=([\d.]+)ms lookup=([\d.]+)ms total=([\d.]+)ms "
    r"funcs=(\d+) ir=(\d+) tier=(\w+)$",
    re.MULTILINE,
)
PHI_STATS_RE = re.compile(
    r"^\[jit-phi\] (\S+) merges=(\d+) demand_scans=(\d+) "
    r"candidates=(\d+) boxed=(\d+) boxed_skipped=(\d+) "
    r"numeric=(\d+) numeric_skipped=(\d+)$",
    re.MULTILINE,
)
PHI_RE = re.compile(r"^\s*%[^\n]*?\s=\sphi\s", re.MULTILINE)
IF_PHI_RE = re.compile(r"^\s*%typed\.if\.[^\n]*?\s=\sphi\s", re.MULTILINE)


def environment(mode: dict[str, str], diagnostic: bool,
                opt_level: str = "3") -> dict[str, str]:
    # Do not allow unrelated XYO_* environment variables from a developer
    # shell to affect reproducibility.
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_PAR": "0",
        "XYO_GPU": "0",
        "XYO_DUMPVARS": "json",
        "XYO_JIT_LAZY_TYPED_SSA": "1",
        "XYO_JIT_OPT_LEVEL": opt_level,
    })
    env.update(mode)
    if not diagnostic:
        env["XYO_JIT_TIMING"] = "1"
    return env


def execute(
    binary: Path, project: Path, env: dict[str, str], frames: int,
    timeout: float,
) -> tuple[dict, str, float]:
    start = time.perf_counter()
    result = subprocess.run(
        [str(binary), "run", str(project), "--jit", "eager",
         "--frames", str(frames)],
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=timeout,
    )
    wall_ms = (time.perf_counter() - start) * 1000
    if result.returncode != 0:
        raise RuntimeError(
            f"{project.name}: exit {result.returncode}\n"
            f"{result.stderr[-2000:]}"
        )
    output = result.stdout
    first, last = output.find("{"), output.rfind("}")
    if first < 0 or last < first:
        raise ValueError(f"no XYO_DUMPVARS JSON: {project.name}")
    variables = json.loads(output[first:last + 1])
    return variables, result.stderr, wall_ms


def signature(variables: dict) -> dict:
    # Scratch benchmark projects may store wall-clock readings in variables.
    # Check their documented deterministic checksums, not timer reporters.
    keys = ("check", "hash", "ok", "sum", "result", "done")
    return {
        target: {key: value[key] for key in keys if key in value}
        for target, value in variables.items()
        if isinstance(value, dict) and any(key in value for key in keys)
    }


def optimized_ir_metrics(text: str) -> dict[str, int]:
    return {
        "phi_total": len(PHI_RE.findall(text)),
        "phi_if": len(IF_PHI_RE.findall(text)),
        "ir_bytes": len(text.encode("utf-8")),
        "xyo_to_num_calls": len(CONVERSION_CALL_RE.findall(text)),
    }


def native_object_metrics(objects: list[Path]) -> dict[str, int]:
    if not objects:
        raise RuntimeError("no ORC native object generated")
    if not shutil.which("size"):
        raise RuntimeError("binutils size command not found")
    text_size = 0
    rodata = 0
    files_bytes = 0
    text_digest = hashlib.sha256()
    has_objcopy = shutil.which("objcopy") is not None
    for obj in objects:
        files_bytes += obj.stat().st_size
        if has_objcopy:
            native_text = obj.with_suffix(".text.bin")
            subprocess.run(
                ["objcopy", "-O", "binary", "--only-section=.text",
                 str(obj), str(native_text)],
                capture_output=True, check=True,
            )
            text_digest.update(native_text.read_bytes())
            native_text.unlink()
        proc = subprocess.run(
            ["size", "-A", "-d", str(obj)],
            capture_output=True, text=True, check=True,
        )
        for line in proc.stdout.splitlines():
            columns = line.split()
            if len(columns) < 2 or not columns[1].isdigit():
                continue
            if columns[0] == ".text" or columns[0].startswith(".text."):
                text_size += int(columns[1])
            if columns[0] == ".rodata" or columns[0].startswith(".rodata."):
                rodata += int(columns[1])
    return {
        "object_count": len(objects),
        "object_file_bytes": files_bytes,
        "native_text_bytes": text_size,
        "native_rodata_bytes": rodata,
        "native_text_sha256": text_digest.hexdigest() if has_objcopy else None,
    }


def inspect(
    binary: Path, project: Path, settings: dict[str, str],
    frames: int, timeout: float, opt_level: str = "3",
) -> tuple[dict, dict]:
    with tempfile.TemporaryDirectory(prefix="xyo-phi-", dir=ROOT / "build") as temp:
        prefix = Path(temp) / "script"
        env = environment(settings, diagnostic=True,
                          opt_level=opt_level)
        env["XYO_JIT_DUMP2"] = "1"
        env["XYO_JIT_PHI_STATS"] = "1"
        env["XYO_JIT_LOCAL_READ_STATS"] = "1"
        env["XYO_JIT_DUPLICATE_SCALAR_STATS"] = "1"
        env["XYO_JIT_DUMP_OBJECT"] = str(prefix)
        values, ir, _ = execute(binary, project, env, frames, timeout)
        objs = sorted(Path(temp).glob("script-*.o"))
        phi_rows = PHI_STATS_RE.findall(ir)
        local_read_reuses = sum(int(count) for _, count in
                                LOCAL_READ_RE.findall(ir))
        duplicate_scalar_pairs = sum(
            int(pairs) for _, pairs, _ in DUPLICATE_SCALAR_RE.findall(ir))
        duplicate_scalar_changes = sum(
            int(changes) for _, _, changes in DUPLICATE_SCALAR_RE.findall(ir))
        fields = (
            "merges", "demand_scans", "candidates", "boxed",
            "boxed_skipped", "numeric", "numeric_skipped",
        )
        phi_counts = {
            field: sum(int(row[i + 1]) for row in phi_rows)
            for i, field in enumerate(fields)
        }
        return {
            **optimized_ir_metrics(ir),
            **native_object_metrics(objs),
            "codegen_phi_opportunities": phi_counts,
            "local_read_reuses": local_read_reuses,
            "duplicate_scalar_pairs": duplicate_scalar_pairs,
            "duplicate_scalar_changes": duplicate_scalar_changes,
        }, signature(values)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/xyo"))
    parser.add_argument("--projects", nargs="+", default=DEFAULT_PROJECTS)
    parser.add_argument("--runs", type=int, default=3)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--local-read", action="store_true",
                      help="compare XYO_JIT_LOCAL_READ_FORWARD=0/1")
    mode.add_argument("--duplicate-scalar", action="store_true",
                      help="compare exact same-scalar arithmetic CSE")
    parser.add_argument("--opt-level", choices=("1", "2", "3"),
                        default="3", help="LLVM optimization level")
    parser.add_argument("--frames", type=int, default=650)
    parser.add_argument("--timeout", type=float, default=110)
    parser.add_argument(
        "--output", type=Path,
        default=Path("build/phi-demand-metrics.json"),
    )
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    binary = (ROOT / args.binary).resolve()
    if not binary.is_file():
        parser.error(f"binary does not exist: {binary}")

    mode_specs = (DUPLICATE_SCALAR_MODES if args.duplicate_scalar else
                  LOCAL_READ_MODES if args.local_read else MODES)
    report: dict[str, dict] = {}
    for given in args.projects:
        project = (ROOT / given).resolve()
        if not project.is_file():
            print(f"SKIP missing: {project}", flush=True)
            continue
        results: dict[str, dict] = {}
        reference: dict | None = None

        # Two passes through all modes, alternating order between passes.
        # Each timing run is free of costly object/IR diagnostic hooks.
        for run in range(args.runs):
            modes = mode_specs if run % 2 == 0 else mode_specs[::-1]
            for name, flags in modes:
                values, stderr, wall = execute(
                    binary, project,
                    environment(flags, diagnostic=False,
                                opt_level=args.opt_level),
                    args.frames, args.timeout,
                )
                sig = signature(values)
                if not sig:
                    raise ValueError(
                        f"{project.name}: no deterministic checksum variable "
                        "found; refusing to claim output parity"
                    )
                if reference is None:
                    reference = sig
                if sig != reference:
                    raise AssertionError(
                        f"{project.name}: {name} changed output: {reference} vs {sig}"
                    )
                matched = list(TIMING_RE.finditer(stderr))
                if not matched:
                    raise RuntimeError(f"missing [jit-timing]: {project.name}")
                row = results.setdefault(name, {
                    "flags": flags,
                    "time_samples": [],
                })
                row["time_samples"].append({
                    "wall_ms": round(wall, 3),
                    "compile_ms": round(sum(float(m[6]) for m in matched), 3),
                    "emit_ms": round(sum(float(m[2]) for m in matched), 3),
                    "opt_ms": round(sum(float(m[3]) for m in matched), 3),
                    "lookup_ms": round(sum(float(m[5]) for m in matched), 3),
                    "scripts_compiled": len(matched),
                })

        for name, flags in mode_specs:
            metrics, sig = inspect(
                binary, project, flags, args.frames, args.timeout,
                opt_level=args.opt_level,
            )
            if sig != reference:
                raise AssertionError(f"{project.name}: IR inspection run changed output")
            results[name]["optimized_ir_and_object"] = metrics
            samples = results[name]["time_samples"]
            results[name]["median_times_ms"] = {
                key: round(statistics.median(sample[key] for sample in samples), 3)
                for key in ("wall_ms", "compile_ms", "emit_ms", "opt_ms", "lookup_ms")
            }

        report[str(project.relative_to(ROOT))] = {
            "output_signature": reference,
            "modes": results,
            "runs": args.runs,
        }
        print(f"\n{project.name}", flush=True)
        print("mode       wall(ms)   compile(ms)    opt(ms)  phi    if-phi  .text(bytes)  to_num  pairs self", flush=True)
        for name, _ in mode_specs:
            r = results[name]
            t = r["median_times_ms"]
            m = r["optimized_ir_and_object"]
            print(
                f"{name:<11}{t['wall_ms']:>8.2f}{t['compile_ms']:>14.2f}"
                f"{t['opt_ms']:>12.2f}{m['phi_total']:>6}"
                f"{m['phi_if']:>10}{m['native_text_bytes']:>14}"
                f"{m['xyo_to_num_calls']:>8}"
                f"{m['duplicate_scalar_pairs']:>7}"
                f"{m['duplicate_scalar_changes']:>5}",
                flush=True,
            )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({
        "measurement": "wall/compile timing without IR dumps; IR and native .text from separate inspection run",
        "optimization": (
            f"XYO_JIT_OPT_LEVEL={args.opt_level}, eager, GPU/PAR disabled, lazy typed SSA enabled; "
            + ("exact duplicate scalar conversion ON/OFF" if args.duplicate_scalar
               else "local read forwarding ON/OFF" if args.local_read
               else "IF PHI modes")
        ),
        "projects": report,
    }, indent=2, ensure_ascii=False) + "\n")
    print(f"Saved {args.output}", flush=True)


if __name__ == "__main__":
    main()
