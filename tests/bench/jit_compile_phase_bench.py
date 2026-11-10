#!/usr/bin/env python3
"""Cold-process JIT startup phases on an immutable ScratchBench SB3.

Every trial launches a fresh xyo process with one fixed CPU affinity.  The
first stdout byte is timed separately from Scratch's first step; those are
different milestones.  LLVM's existing [jit-timing] diagnostic separates IR
emission, IR optimization, ORC submission and lookup/materialization.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import select
import statistics
import subprocess
import time
import zipfile

STAGE_RE = re.compile(r"^\[startup\]\s+([\w-]+)\s+([0-9.]+)ms$", re.M)
ASYNC_RE = re.compile(r"^\[jit-async\] (\S+) finished ([\d.]+)ms$", re.M)
OBJECT_RE = re.compile(
    r"^\[jit-object-timing\] (\S+) objects=(\d+) bytes=(\d+) "
    r"pre-object=([\d.]+)ms post-object=([\d.]+)ms$", re.M
)
INIT_RE = re.compile(
    r"^\[jit-object-timing\] backend-init target=([\d.]+)ms "
    r"lljit=([\d.]+)ms symbols=([\d.]+)ms total=([\d.]+)ms$", re.M
)
COMPILE_RE = re.compile(
    r"^\[jit-timing\] (\S+) emit=([\d.]+)ms opt=([\d.]+)ms "
    r"submit=([\d.]+)ms lookup=([\d.]+)ms total=([\d.]+)ms "
    r"funcs=(\d+) ir=(\d+) tier=(\w+)$", re.M
)
CONFIGS = {
    "interpreter": ("off", None, None, None),
    "hotspot_auto": ("hotspot", None, None, None),
    "hotspot_parfor_on": ("hotspot", None, None, None),
    "hotspot_parfor_partition": ("hotspot", None, None, None),
    "hotspot_async": ("hotspot", None, None, None),
    "hotspot_lazy_on": ("hotspot", None, None, "on"),
    "hotspot_lazy_off": ("hotspot", None, None, "off"),
    "eager_o3": ("eager", None, None, None),
    "eager_o3_parfor_on": ("eager", None, None, None),
    "eager_o2": ("eager", "2", None, None),
    "eager_o3_less": ("eager", None, "less", None),
    "eager_o2_less": ("eager", "2", "less", None),
    "eager_o1": ("eager", "1", None, None),
}


def parse_logs(stderr: str) -> dict:
    stages = {name: float(ms) for name, ms in STAGE_RE.findall(stderr)}
    compilations = []
    for script, emit, opt, submit, lookup, total, funcs, ir, tier in COMPILE_RE.findall(stderr):
        compilations.append({
            "script": script, "tier": tier, "functions": int(funcs),
            "ir_instructions": int(ir), "emit_ms": float(emit),
            "opt_ms": float(opt), "submit_ms": float(submit),
            "lookup_ms": float(lookup), "total_ms": float(total),
        })
    background = [
        {"script": script, "finish_ms": float(ms)}
        for script, ms in ASYNC_RE.findall(stderr)
    ]
    objects = [{
        "script": script, "objects": int(count), "bytes": int(size),
        "pre_object_ms": float(pre), "post_object_ms": float(post),
    } for script, count, size, pre, post in OBJECT_RE.findall(stderr)]
    initialization = [{
        "target_ms": float(target), "lljit_ms": float(lljit),
        "symbols_ms": float(symbols), "total_ms": float(total),
    } for target, lljit, symbols, total in INIT_RE.findall(stderr)]
    return {"stages_ms": stages, "compilations": compilations,
            "background_compilations": background,
            "object_phases": objects, "jit_initialization": initialization}


def count_direct_calls(blocks: dict, top: str) -> tuple[int, int]:
    """Match countDirectProcedureCalls() in backend_llvm.cpp: main next-chain only."""
    seen = set()
    calls = 0
    current = top
    while current and current not in seen and len(seen) < 200000:
        block = blocks.get(current)
        if not isinstance(block, dict):
            break
        seen.add(current)
        calls += block.get("opcode") == "procedures_call"
        current = block.get("next")
    return len(seen), calls


def script_topology(project: Path, script_ids: set[str]) -> list[dict]:
    with zipfile.ZipFile(project) as archive:
        data = json.loads(archive.read("project.json"))
    result = []
    for target in data.get("targets", []):
        blocks = target.get("blocks", {})
        for top in sorted(script_ids.intersection(blocks)):
            chain, calls = count_direct_calls(blocks, top)
            result.append({
                "target": target.get("name"), "script": top,
                "main_chain_blocks": chain, "direct_procedure_calls": calls,
            })
    return result


def run_one(binary: Path, project: Path, config: str, cpu: int,
            frames: int, timeout: int, object_timing: bool) -> dict:
    mode, opt, codegen, lazy = CONFIGS[config]
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("XYO_JIT_") and not k.startswith("XYO_STARTUP_")}
    env.update(XYO_STARTUP_TIMING="1", XYO_JIT_TIMING="1",
               XYO_GPU="0", XYO_PAR="0",
               XYO_PARFOR="1" if (
                   config.endswith("_parfor_on") or
                   config == "hotspot_parfor_partition") else "0")
    if config == "hotspot_parfor_partition":
        env["XYO_JIT_LAZY_PARFOR_PARTITION"] = "1"
    if object_timing:
        env["XYO_JIT_OBJECT_TIMING"] = "1"
    if opt is not None:
        env["XYO_JIT_OPT_LEVEL"] = opt
    if codegen is not None:
        env["XYO_JIT_CODEGEN_OPT"] = codegen
    if config == "hotspot_async":
        env["XYO_JIT_ASYNC"] = "1"
        env["XYO_JIT_ASYNC_TIMING"] = "1"

    cmd = ["taskset", "-c", str(cpu), str(binary), "run",
           str(project), "--jit", mode, "--fps", "0",
           "--frames", str(frames), "--print-list", "output"]
    if lazy is not None:
        cmd.extend(["--jit-lazy", lazy])

    started = time.perf_counter()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env)
    first_output_ms = None
    try:
        ready, _, _ = select.select([proc.stdout], [], [], timeout)
        if ready:
            chunk = os.read(proc.stdout.fileno(), 4096)
            if chunk:
                first_output_ms = (time.perf_counter() - started) * 1000.0
        else:
            proc.kill()
            raise TimeoutError(f"{config}: no project output after {timeout}s")

        remaining, stderr = proc.communicate(timeout=timeout)
        elapsed_ms = (time.perf_counter() - started) * 1000.0
    except BaseException:
        proc.kill()
        proc.communicate()
        raise
    if proc.returncode not in (0, 2):
        raise RuntimeError(f"{config}: exit {proc.returncode}: "
                           f"{stderr.decode(errors='replace')[-1200:]}")

    parsed = parse_logs(stderr.decode(errors="replace"))
    if object_timing and mode != "off" and not parsed["jit_initialization"]:
        raise RuntimeError(
            f"{config}: --object-timing requires an instrumented xyo binary"
        )
    if first_output_ms is None or "first-step" not in parsed["stages_ms"]:
        raise RuntimeError(f"{config}: missing stdout/startup markers")
    for compilation in parsed["compilations"]:
        objects = [o for o in parsed["object_phases"]
                   if o["script"] == compilation["script"]]
        if len(objects) == 1:
            phase_sum = objects[0]["pre_object_ms"] + objects[0]["post_object_ms"]
            if abs(phase_sum - compilation["lookup_ms"]) > 0.1:
                raise RuntimeError(f"{config}: ORC phase mismatch for {compilation['script']}")
    return {
        "config": config, "first_output_ms": round(first_output_ms, 3),
        "process_wall_ms": round(elapsed_ms, 3),
        "first_line": (chunk + remaining).splitlines()[0].decode(errors="replace"),
        **parsed,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--frames", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--object-timing", action="store_true",
                        help="also profile ORC object-ready boundary (requires instrumented xyo)")
    parser.add_argument("--cpu", type=int, default=min(os.sched_getaffinity(0)))
    parser.add_argument("--configs", nargs="+", choices=CONFIGS,
                        default=list(CONFIGS))
    parser.add_argument("--output", type=Path,
                        default=Path("build-startup-probes/phase-results.json"))
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    project = args.project.resolve(strict=True)
    if args.cpu not in os.sched_getaffinity(0):
        parser.error(f"CPU {args.cpu} unavailable")
    if args.trials <= 0 or args.frames <= 0:
        parser.error("trials and frames must be positive")

    rows = []
    metadata = {
        "binary": str(binary), "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "project": str(project), "project_sha256": hashlib.sha256(project.read_bytes()).hexdigest(),
        "cpu": args.cpu, "trials": args.trials, "frames": args.frames,
        "object_timing": args.object_timing,
    }
    print("SOURCE", json.dumps(metadata), flush=True)
    for trial in range(args.trials):
        order = list(args.configs)
        random.Random(1618 + trial).shuffle(order)
        for config in order:
            measured = run_one(binary, project, config, args.cpu,
                               args.frames, args.timeout, args.object_timing)
            measured["trial"] = trial
            rows.append(measured)
            jit_total = sum(x["total_ms"] for x in measured["compilations"])
            print(config, "trial", trial,
                  "first_stdout_ms", round(measured["first_output_ms"], 1),
                  "first_step_ms", measured["stages_ms"]["first-step"],
                  "compile_ms", round(jit_total, 1), flush=True)

    metadata["script_topology"] = script_topology(
        project, {c["script"] for row in rows for c in row["compilations"]}
    )
    metrics = ("first_output_ms", "process_wall_ms")
    summary = {}
    for config in args.configs:
        matches = [r for r in rows if r["config"] == config]
        summary[config] = {
            k: round(statistics.median(r[k] for r in matches), 3) for k in metrics
        }
        summary[config]["jit_compilation_ms"] = round(statistics.median(
            sum(c["total_ms"] for c in r["compilations"]) for r in matches), 3)
        summary[config]["background_compilation_ms"] = round(statistics.median(
            sum(c["finish_ms"] for c in r["background_compilations"])
            for r in matches), 3)
        for stage in ("read-sb3", "runtime-init", "sb3-parse",
                      "before-greenflag", "greenflag-spawn", "first-step"):
            if all(stage in r["stages_ms"] for r in matches):
                summary[config][stage + "_ms"] = round(statistics.median(
                    r["stages_ms"][stage] for r in matches), 3)
        for phase in ("emit", "opt", "submit", "lookup"):
            summary[config][phase + "_ms"] = round(statistics.median(
                sum(c[phase + "_ms"] for c in r["compilations"])
                for r in matches), 3)
        for phase in ("pre_object_ms", "post_object_ms"):
            values = [sum(c[phase] for c in r["object_phases"])
                      for r in matches if r["object_phases"]]
            summary[config][phase] = (
                round(statistics.median(values), 3) if values else None)
        values = [sum(c["total_ms"] for c in r["jit_initialization"])
                  for r in matches if r["jit_initialization"]]
        summary[config]["jit_init_ms"] = (
            round(statistics.median(values), 3) if values else None)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"metadata": metadata, "summary": summary,
                                       "runs": rows}, indent=2) + "\n")
    print("SUMMARY", json.dumps(summary, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
