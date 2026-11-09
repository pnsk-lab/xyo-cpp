#!/usr/bin/env python3
"""Compare full ScratchBench parity output across Emscripten VM modes.

This does NOT assert that the headless runtime passes all official
checks: the Emscripten/Node profile lacks the SDL renderer, so the visual
checks are expected to fail. ScratchBench may add new checks; all backends
must report exactly the same total as the Interpreter and identical output. It asserts that the *actual Binaryen backend*
introduces zero differences from Builtin WASM and the interpreter, and that
at least one script was really compiled through Binaryen.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
VERDICT = re.compile(r"Tests Were (?:Not )?Successful:\s*(\d+)/(\d+)")


def execute(node: Path, module: Path, sb3: Path, mode: str,
            addon: Path) -> dict:
    env = dict(os.environ)
    env.update(
        XYO_JIT_VERBOSE="1",
        XYO_HOT_INVOCATIONS="1",
        XYO_EXIT_ON_DONE="1",
        XYO_PAR="0",
        XYO_WASM_HOT_EMITTER=("binaryen" if mode == "binaryen" else "builtin"),
    )
    if mode == "binaryen":
        env["XYO_BINARYEN_NODE_ADDON"] = str(addon)
        env["XYO_BINARYEN_DIAGNOSTICS"] = "1"
    else:
        env.pop("XYO_BINARYEN_NODE_ADDON", None)
    cmd = [str(node), str(ROOT / "web/xyo-node.mjs"), str(module),
           "run", str(sb3), "--jit", "off" if mode == "interpreter" else "hotspot",
           "--fps", "30", "--frames", "2400", "--print-list", "output",
           "--list-limit", "200000"]
    proc = subprocess.run(cmd, cwd=ROOT, env=env, text=True,
                          capture_output=True, timeout=120)
    if proc.returncode:
        raise RuntimeError(f"{mode} exited {proc.returncode}: {proc.stderr[-1600:]}")
    found = VERDICT.findall(proc.stdout)
    if not found:
        raise RuntimeError(f"{mode} did not print parity verdict: {proc.stdout[-1100:]}")
    passed, total = map(int, found[-1])
    if total < 2123:
        raise RuntimeError(
            f"{mode} ran only {total} checks; expected at least 2123")
    compiled = (proc.stderr.count("xyo-wasm-hot: compiled script=") +
                proc.stderr.count("xyo-wasm-hot: hybrid script="))
    if mode == "binaryen":
        optimized = sum(
            (line.startswith("xyo-wasm-hot: compiled script=") or
             line.startswith("xyo-wasm-hot: hybrid script=")) and
            "emitter=binaryen-host" in line
            for line in proc.stderr.splitlines())
        native_calls = proc.stderr.count("[binaryen-native] optimized #")
        if not compiled or optimized != compiled or native_calls == 0:
            raise RuntimeError(
                f"Binaryen not really active: compiled={compiled}, "
                f"emitter=binaryen-host {optimized}, "
                f"native optimization calls={native_calls}\n"
                f"{proc.stderr[-800:]}")
    return {"mode": mode, "passed": passed, "total": total,
            "compiled_scripts": compiled, "stdout": proc.stdout}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("module", type=Path, help="Emscripten xyo.js")
    parser.add_argument("--sb3", type=Path, required=True)
    parser.add_argument("--addon", type=Path, required=True)
    parser.add_argument("--node", type=Path, default=Path("node"))
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    module = args.module.resolve()
    sb3 = args.sb3.resolve()
    addon = args.addon.resolve()
    for path in (module, sb3, addon):
        if not path.is_file():
            parser.error(f"file does not exist: {path}")

    results = [
        execute(args.node, module, sb3, mode, addon)
        for mode in ("interpreter", "builtin", "binaryen")
    ]
    reference = results[0]["stdout"]
    expected_total = results[0]["total"]
    for result in results[1:]:
        if result["total"] != expected_total:
            raise AssertionError(
                f"{result['mode']} ran {result['total']} checks but "
                f"interpreter ran {expected_total}"
            )
        if result["stdout"] != reference:
            a, b = reference.splitlines(), result["stdout"].splitlines()
            first = next((i for i in range(min(len(a), len(b)))
                          if a[i] != b[i]), min(len(a), len(b)))
            lhs = a[first] if first < len(a) else "<EOF>"
            rhs = b[first] if first < len(b) else "<EOF>"
            raise AssertionError(
                f"{result['mode']} diverged from interpreter at line {first+1}: "
                f"{lhs!r} != {rhs!r}")
    for result in results:
        print(f"{result['mode']}: {result['passed']}/{result['total']}, "
              f"compiled={result['compiled_scripts']}")
    print(f"PASS: all {expected_total} output checks exactly match "
          "the interpreter; Binaryen JIT was executed")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        payload = {"results": [{k: v for k, v in result.items()
                                if k != "stdout"} for result in results],
                   "all_outputs_identical": True}
        args.json.write_text(
            json.dumps(payload, indent=2, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()
