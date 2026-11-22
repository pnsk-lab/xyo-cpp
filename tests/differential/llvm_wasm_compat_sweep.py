#!/usr/bin/env python3
"""One-command LLVM <-> WASM/Binaryen execution-compatibility sweep.

The native LLVM JIT and the Emscripten WASM/Binaryen backend are separate
code generators. The compatibility contract (docs/jit-backend-compatibility.md)
is observable equivalence: for the same project, the final Scratch values,
the scheduler-visible ordering and the effect count must match. WASM regions
that do not support an operation must fall back to the interpreter instead
of guessing.

This harness runs the existing focused parity suites in one process and
prints a single summary. Each suite already compares Interpreter, native
LLVM, builtin WASM and Binaryen (when an addon is supplied). Fixtures are
written under --out.

Usage:
  python3 tests/differential/llvm_wasm_compat_sweep.py \\
      build-wasm-be/xyo.js --llvm build/xyo \\
      --addon build-wasm-be/binaryen-host.node --out /tmp/compat

  --quick       fewer fuzz seeds
  --only NAME   run one suite (repeatable)
  --list        print suite names
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
DIFF = ROOT / "tests" / "differential"


def suites(quick: bool) -> list[dict]:
    fuzz = 8 if quick else 32
    cases = 32 if quick else 128
    nested = 12 if quick else 48
    return [
        {"name": "hotspot", "script": "wasm_hotspot_integration.py",
         "args": [], "needs_addon": False},
        {"name": "compat_fuzz", "script": "wasm_compat_fuzz.py",
         "args": ["--cases", str(cases)], "needs_addon": False},
        {"name": "string", "script": "wasm_string_semantics.py",
         "args": [], "needs_addon": True},
        {"name": "list_ext", "script": "wasm_list_extensions.py",
         "args": [], "needs_addon": True},
        {"name": "tagged_list", "script": "wasm_tagged_list_semantics.py",
         "args": [], "needs_addon": True},
        {"name": "string_clock", "script": "wasm_string_clock_extensions.py",
         "args": [], "needs_addon": True},
        {"name": "warp_proc", "script": "wasm_warp_procedure.py",
         "args": [], "needs_addon": True},
        {"name": "loop_limit", "script": "wasm_loop_limit.py",
         "args": [], "needs_addon": True},
        {"name": "nested_default", "script": "wasm_nested_default.py",
         "args": [], "needs_addon": True},
        {"name": "edge_hats", "script": "edge_hat_activation.py",
         "args": [], "needs_addon": True},
        {"name": "timer_reporter", "script": "wasm_timer_reporter.py",
         "args": [], "needs_addon": True},
        {"name": "mouse_reporters", "script": "wasm_mouse_reporters.py",
         "args": [], "needs_addon": True},
        {"name": "loudness", "script": "wasm_loudness.py",
         "args": [], "needs_addon": True},
        {"name": "motion_effects", "script": "wasm_motion_effects.py",
         "args": [], "needs_addon": True},
        {"name": "username", "script": "wasm_username.py",
         "args": [], "needs_addon": True},
        {"name": "return_pc_stack", "script": "wasm_return_pc_stack.py",
         "args": [], "needs_addon": True},
        {"name": "nested_cont", "script": "wasm_nested_continuation.py",
         "args": [], "needs_addon": True},
        {"name": "return_stack_fuzz", "script": "wasm_return_stack_fuzz.py",
         "args": ["--seeds", str(fuzz)], "needs_addon": True},
        {"name": "return_pc_fuzz", "script": "wasm_return_pc_fuzz.py",
         "args": ["--seeds", str(fuzz)], "needs_addon": True},
        {"name": "tail_recursion", "script": "wasm_tail_recursion_fuzz.py",
         "args": ["--seeds", str(fuzz)], "needs_addon": True},
        {"name": "nested_tree", "script": "wasm_nested_tree_fuzz.py",
         "args": ["--seeds", str(nested)], "needs_addon": True},
        # Effect ordering and exactly-once host effects: the scheduler-visible
        # compatibility dimension (these harnesses use --native, not --llvm).
        {"name": "effects_broadcast", "script": "runtime_broadcast_journal.py",
         "args": [], "needs_addon": True, "native_flag": "--native",
         "wasm_option": True},
        {"name": "effects_broadcast_wait",
         "script": "runtime_broadcast_wait_journal.py",
         "args": [], "needs_addon": True, "native_flag": "--native",
         "wasm_option": True},
        {"name": "effects_looks", "script": "runtime_looks_journal.py",
         "args": [], "needs_addon": True, "native_flag": "--native",
         "wasm_option": True},
        {"name": "effects_pen", "script": "runtime_pen_journal.py",
         "args": [], "needs_addon": True, "native_flag": "--native",
         "wasm_option": True},
        {"name": "effects_clone", "script": "runtime_clone_journal.py",
         "args": [], "needs_addon": True, "native_flag": "--native",
         "wasm_option": True},
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path, help="Emscripten xyo.js")
    parser.add_argument("--llvm", type=Path, required=True,
                        help="native LLVM xyo binary")
    parser.add_argument("--addon", type=Path,
                        help="Binaryen Node N-API addon (optional)")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()

    selected = suites(args.quick)
    if args.list:
        for suite in selected:
            print(suite["name"])
        return 0
    if args.only:
        unknown = set(args.only) - {s["name"] for s in selected}
        if unknown:
            parser.error(f"unknown suite(s): {', '.join(sorted(unknown))}")
        selected = [s for s in selected if s["name"] in args.only]

    wasm = args.wasm.resolve()
    llvm = args.llvm.resolve()
    addon = args.addon.resolve() if args.addon else None
    if not wasm.is_file():
        parser.error(f"wasm does not exist: {wasm}")
    if not llvm.is_file():
        parser.error(f"llvm binary does not exist: {llvm}")
    if addon and not addon.is_file():
        parser.error(f"addon does not exist: {addon}")
    args.out.mkdir(parents=True, exist_ok=True)

    results: list[dict] = []
    failed = 0
    for suite in selected:
        if suite["needs_addon"] and addon is None:
            results.append({"name": suite["name"], "status": "skip",
                            "reason": "no --addon (Binaryen unavailable)"})
            print(f"SKIP {suite['name']}: no Binaryen addon")
            continue
        directory = args.out / suite["name"]
        directory.mkdir(parents=True, exist_ok=True)
        cmd = [sys.executable, str(DIFF / suite["script"])]
        cmd += (["--wasm", str(wasm)] if suite.get("wasm_option")
                else [str(wasm)])
        cmd += [suite.get("native_flag", "--llvm"), str(llvm),
                "--out", str(directory)]
        if addon is not None and suite["needs_addon"]:
            cmd += ["--addon", str(addon)]
        cmd += suite["args"]
        print(f"RUN  {suite['name']} ...", flush=True)
        started = time.time()
        proc = subprocess.run(cmd, cwd=ROOT, text=True,
                              capture_output=True, timeout=3600)
        elapsed = time.time() - started
        ok = proc.returncode == 0
        results.append({
            "name": suite["name"], "status": "pass" if ok else "fail",
            "seconds": round(elapsed, 2),
            "command": cmd,
            "tail": (proc.stdout + proc.stderr)[-1200:],
        })
        if ok:
            print(f"PASS {suite['name']} ({elapsed:.1f}s)")
        else:
            failed += 1
            print(f"FAIL {suite['name']} ({elapsed:.1f}s)")
            print(json.dumps(results[-1]["tail"], indent=2))

    counts = {k: sum(1 for r in results if r["status"] == k)
              for k in ("pass", "fail", "skip")}
    print(f"\ncompat sweep: {counts['pass']} passed, {counts['fail']} failed, "
          f"{counts['skip']} skipped")
    if args.json:
        args.json.write_text(json.dumps(
            {"wasm": str(wasm), "llvm": str(llvm),
             "addon": str(addon) if addon else None,
             "quick": args.quick, "results": results}, indent=2) + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
