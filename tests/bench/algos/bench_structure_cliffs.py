#!/usr/bin/env python3
"""Equivalent-work Scratch benchmarks for optimization composition boundaries.

No runtime changes or project-name-specific optimizations. Builds real SB3s,
checks all final states against an independent Python oracle and interpreter,
then measures warmed kernels in randomized forward/reverse process blocks.
Default timing never enables profiling or exact block counting (which can
change optimizer selection). --diagnostics captures IR in separate processes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import time

from gen_algo_bench import (
    Gen, S_add, S_call, S_ifelse, S_repeat, S_set, add, days, item,
    lt, make_project, mod, mul, num, sub, var, write_sb3,
)

ROOT = Path(__file__).resolve().parents[3]
KINDS = ("inline", "scalar_coeff", "scalar_text_coeff", "list_coeff", "helper_body", "empty_call",
         "redundant_branch", "unrelated_write", "nested_repeat", "helper_text_coeff")
PROFILES = {
    "default": {},
    "repeat_ssa_off": {"XYO_JIT_NUMERIC_REPEAT_SSA": "0",
                       "XYO_JIT_NUMERIC_REPEAT_FLOAT": "0"},
    "scalar_copy": {"XYO_JIT_NUMERIC_REPEAT_COPY": "1"},
    "scalar_copy_off": {"XYO_JIT_NUMERIC_REPEAT_COPY": "0"},
    "readonly_string": {"XYO_JIT_NUMERIC_REPEAT_READONLY_STRING": "1"},
    "inline_helpers": {"XYO_JIT_NUMERIC_REPEAT_INLINE": "1"},
    "numeric_region": {"XYO_JIT_NUMERIC_REGION": "1"},
    "composition_all": {"XYO_JIT_NUMERIC_REPEAT_READONLY_STRING": "1",
                        "XYO_JIT_NUMERIC_REPEAT_INLINE": "1"},
}


def oracle(count):
    x, acc = 1, 0
    for _ in range(count):
        x = (x * 17 + 23) % 65521
        acc = (acc + x) % 65521
    return [x, acc, x]


def project(count, kind, samples):
    g = Gen(["x", "acc", "aux", "coeff", "t0", "t1"],
            ["coeffs", "times", "checks"])
    coeff = (var("coeff") if kind in ("scalar_coeff", "scalar_text_coeff", "helper_text_coeff") else
             item("coeffs", num(1)) if kind == "list_coeff" else num(17))
    body = [
        S_set("x", mod(add(mul(var("x"), coeff), num(23)), num(65521))),
        S_set("acc", mod(add(var("acc"), var("x")), num(65521))),
    ]
    # This deliberately redundant branch tests whether source-shape
    # recognition rejects a loop even when LLVM can later fold the branch.
    if kind == "redundant_branch":
        body = [S_ifelse(lt(var("x"), num(32768)), body, body)]
    if kind in ("helper_body", "helper_text_coeff"):
        g.define("step", [], body, warp=True)
        body = [S_call("step")]
    elif kind == "empty_call":
        g.define("empty", [], [], warp=True)
        body = body + [S_call("empty")]
    elif kind == "unrelated_write":
        body = body + [S_set("aux", var("x"))]
    loop = [S_repeat(num(count), body)]
    if kind == "nested_repeat":
        loop = [S_repeat(num(count // 8), [S_repeat(num(8), body)]),
                S_repeat(num(count % 8), body)]
    # A Scratch numeric shadow may still be stored as a numeric String.
    # Arithmetic forces a plain Number; retain an explicit textual control.
    coefficient_init = num(17) if kind in ("scalar_text_coeff", "helper_text_coeff") else add(num(0), num(17))
    g.define("kernel", [], [S_set("x", add(num(0), num(1))),
                            S_set("acc", add(num(0), num(0))),
                            S_set("aux", add(num(0), num(0))),
                            S_set("coeff", coefficient_init)] +
             loop + [S_set("aux", var("x"))], warp=True)
    # Timing surrounds the call, not compilation. Two full warmups also
    # exercise procedure compilation/type feedback before measured runs.
    g.script([S_call("kernel"), S_call("kernel"),
              S_repeat(num(samples), [
                  S_set("t0", days()), S_call("kernel"), S_set("t1", days()),
                  S_add("times", mul(sub(var("t1"), var("t0")), num(86400000))),
                  S_add("checks", var("x")), S_add("checks", var("acc")),
                  S_add("checks", var("aux")),
              ])])
    return make_project(g.vars, g.lists, g, {"coeffs": [17]})


def clean_env(profile):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_GPU": "0", "XYO_PAR": "0",
                "XYO_PARFOR": "0", "SDL_VIDEODRIVER": "dummy"})
    env.update(PROFILES[profile])
    return env


def invoke(binary, sb3, mode, profile, cpu, timeout, log_prefix, diagnostics=False):
    env = clean_env(profile)
    if diagnostics:
        env.update({"XYO_JIT_DUMP": "1", "XYO_JIT_DUMP2": "1",
                    "XYO_JIT_VERBOSE": "1"})
    cmd = [str(binary), "run", str(sb3), "--jit", mode, "--frames", "100000"]
    if cpu is not None:
        cmd = ["taskset", "-c", str(cpu)] + cmd
    start = time.perf_counter()
    p = subprocess.run(cmd, env=env, cwd=ROOT, text=True, capture_output=True,
                       timeout=timeout)
    wall_ms = (time.perf_counter() - start) * 1000
    log_prefix.with_suffix(".stdout").write_text(p.stdout)
    log_prefix.with_suffix(".stderr").write_text(p.stderr)
    if p.returncode:
        raise RuntimeError(f"exit {p.returncode}: {cmd}; see {log_prefix}.stderr")
    try:
        data = json.loads(p.stdout[p.stdout.index("{"):p.stdout.rindex("}") + 1])
        values = data["Bench"]
        times = [float(x) for x in values["times"]]
        checks = [float(x) for x in values["checks"]]
    except (ValueError, KeyError) as exc:
        raise RuntimeError(f"invalid output: {log_prefix}") from exc
    return {"timings_ms": times, "checks": checks, "wall_ms": wall_ms,
            "command": cmd, "environment": {k: v for k, v in env.items()
                                             if k.startswith("XYO_")}}


def validate(result, expected, samples, timed=False):
    if result["checks"] != expected * samples:
        raise RuntimeError(f"oracle mismatch: {result['checks']} != {expected * samples}")
    times = result["timings_ms"]
    if len(times) != samples or any(not 0 <= x < float("inf") for x in times):
        raise RuntimeError(f"invalid timings: {times}")
    if timed and min(times) <= 0:
        raise RuntimeError("timer resolution too low; increase --count")


def summarize(rows, kinds, profiles):
    out = {}
    for profile in profiles:
        base = {r["block"]: statistics.median(r["timings_ms"]) for r in rows
                if r["profile"] == profile and r["kind"] == "inline"}
        for kind in kinds:
            selected = [r for r in rows if r["profile"] == profile and r["kind"] == kind]
            medians = [statistics.median(r["timings_ms"]) for r in selected]
            ratios = [statistics.median(r["timings_ms"]) / base[r["block"]]
                      for r in selected] if base else []
            out[f"{profile}/{kind}"] = {
                "median_ms": statistics.median(medians),
                "process_medians_ms": medians,
                "median_slowdown_vs_inline": statistics.median(ratios) if ratios else None,
                "paired_slowdowns": ratios,
            }
    for control, treatment, label in (
            ("repeat_ssa_off", "default", "repeat_ssa_speedup"),
            ("default", "scalar_copy", "scalar_copy_speedup"),
            ("scalar_copy_off", "default", "scalar_copy_default_speedup"),
            ("default", "readonly_string", "readonly_string_speedup"),
            ("default", "inline_helpers", "inline_helpers_speedup"),
            ("default", "composition_all", "composition_speedup")):
        if control not in profiles or treatment not in profiles:
            continue
        for kind in kinds:
            indexed = {(r["profile"], r["block"]): statistics.median(r["timings_ms"])
                       for r in rows if r["kind"] == kind}
            blocks = sorted({r["block"] for r in rows})
            ratios = [indexed[(control, b)] / indexed[(treatment, b)] for b in blocks]
            out[f"{label}/{kind}"] = {
                "median": statistics.median(ratios), "paired_ratios": ratios}
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True,
                    help="New output directory; refuses to overwrite existing results")
    ap.add_argument("--count", type=int, default=2_000_000)
    ap.add_argument("--blocks", type=int, default=6)
    ap.add_argument("--samples", type=int, default=3)
    ap.add_argument("--cpu", type=int)
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--kinds", default=",".join(KINDS))
    ap.add_argument("--profiles", default="default,repeat_ssa_off")
    ap.add_argument("--source-revision", default="unattested",
                    help="Caller-attested source revision of the binary, not workspace HEAD")
    ap.add_argument("--diagnostics", action="store_true")
    args = ap.parse_args()
    kinds, profiles = args.kinds.split(","), args.profiles.split(",")
    if (not 1 <= args.count <= 2147483647 or args.blocks < 1 or args.samples < 1 or
            not set(kinds) <= set(KINDS) or not set(profiles) <= set(PROFILES)):
        ap.error("invalid count (must fit positive i32), blocks, samples, kinds or profiles")
    if len(profiles) != len(set(profiles)) or len(kinds) != len(set(kinds)):
        ap.error("duplicate kinds/profiles")
    if args.cpu is not None and args.cpu not in os.sched_getaffinity(0):
        ap.error("CPU unavailable")
    binary, out = args.binary.resolve(), args.out.resolve()
    if not binary.is_file():
        ap.error("binary not found")
    out.mkdir(parents=True, exist_ok=False)
    projects, logs = out / "projects", out / "logs"
    projects.mkdir()
    logs.mkdir()
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    def git(*cmd):
        return subprocess.check_output(["git", *cmd], cwd=ROOT, text=True).strip()
    harness_paths = [Path(__file__), Path(__file__).with_name("gen_algo_bench.py")]
    harness_hashes = {}
    for path in harness_paths:
        content = path.read_bytes()
        harness_hashes[path.name] = hashlib.sha256(content).hexdigest()
        (out / path.name).write_bytes(content)
    cache = binary.parent / "CMakeCache.txt"
    if cache.exists():
        (out / "CMakeCache.txt").write_bytes(cache.read_bytes())
    manifest = {"binary": str(binary), "binary_sha256": digest,
                "harness_sha256": harness_hashes,
                "source_revision": args.source_revision,
                "workspace_head": git("rev-parse", "HEAD"),
                "workspace_diff_sha256": hashlib.sha256(subprocess.check_output(
                    ["git", "diff", "HEAD", "--", "src", "CMakeLists.txt"], cwd=ROOT)).hexdigest(),
                "platform": platform.platform(), "cpu": args.cpu,
                "cpu_info": Path("/proc/cpuinfo").read_text() if Path("/proc/cpuinfo").exists() else "",
                "count": args.count, "blocks": args.blocks, "samples": args.samples,
                "kinds": kinds, "profiles": profiles, "seed": 121,
                "notes": "Warmed eager JIT; GPU/parallel scheduling/parfor disabled. "
                         "No danger, profiling or block counting in timed processes. "
                         "Variants preserve final state, not identical instruction counts."}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2))
    # Include zero, chunk boundaries, and nested-loop remainders in parity.
    validations = []
    for count in (0, 1, 7, 8, 63, 64, 65, 129):
        expected = oracle(count)
        for kind in kinds:
            sb3 = projects / f"validate-{kind}-{count}.sb3"
            write_sb3(sb3, project(count, kind, 1))
            for mode, profile in [("off", "default")] + [("eager", p) for p in profiles]:
                result = invoke(binary, sb3, mode, profile, args.cpu, args.timeout,
                                logs / f"validate-{kind}-{count}-{mode}-{profile}")
                validate(result, expected, 1)
                validations.append({"count": count, "kind": kind, "mode": mode,
                                    "profile": profile, "checks": result["checks"]})
    (out / "validation.json").write_text(json.dumps(validations, indent=2))
    print(f"PASS {len(validations)} interpreter/JIT/oracle validations", flush=True)
    expected = oracle(args.count)
    for kind in kinds:
        write_sb3(projects / f"{kind}.sb3", project(args.count, kind, args.samples))
    rows = []
    rng = random.Random(121)
    jobs = [(k, p) for k in kinds for p in profiles]
    for block in range(args.blocks):
        # Adjacent blocks reverse exactly the same order to counter order drift.
        if block % 2 == 0:
            rng.shuffle(jobs)
        else:
            jobs.reverse()
        for kind, profile in jobs:
            result = invoke(binary, projects / f"{kind}.sb3", "eager", profile,
                            args.cpu, args.timeout, logs / f"b{block}-{kind}-{profile}")
            validate(result, expected, args.samples, timed=True)
            rows.append({"block": block, "kind": kind, "profile": profile, **result})
            (out / "results.json").write_text(json.dumps(rows, indent=2))
            print(block, kind, profile, f"{statistics.median(result['timings_ms']):.3f}ms",
                  flush=True)
    summary = summarize(rows, kinds, profiles)
    (out / "summary.json").write_text(json.dumps(summary, indent=2))
    if args.diagnostics:
        for kind in kinds:
            for profile in profiles:
                result = invoke(binary, projects / f"{kind}.sb3", "eager", profile,
                                args.cpu, args.timeout, logs / f"ir-{kind}-{profile}", True)
                validate(result, expected, args.samples)
    if hashlib.sha256(binary.read_bytes()).hexdigest() != digest:
        raise RuntimeError("binary changed during experiment; results invalid")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
