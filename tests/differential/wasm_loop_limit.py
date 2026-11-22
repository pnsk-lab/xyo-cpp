#!/usr/bin/env python3
"""Sequential top-level loop-count boundary parity (LLVM vs WASM/Binaryen).

The HotSpot compiler splits every top-level loop into resumable modules. The
historical 32-loop cap forced longer straight-line scripts into the
interpreter even though each loop is independent. The cap is now 128; this
test locks in the compiled path at/below the boundary and the safe hybrid
fallback above it, with identical Scratch values in every engine.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))
sys.path.insert(0, str(ROOT / "tests" / "differential"))
from gen_algo_bench import (  # noqa: E402
    Gen, S_set, S_change, S_repeat, num, var, make_project, write_sb3,
)
from wasm_hotspot_integration import run, run_native  # noqa: E402


def fixture(path: Path, loops: int) -> None:
    g = Gen(["x", "y"], [])
    body = [S_set("x", num(0))]
    for _ in range(loops):
        body.append(S_repeat(num(1), [S_change("x", num(1))]))
    body.append(S_set("y", var("x")))
    g.script(body)
    write_sb3(str(path), make_project(["x", "y"], [], g))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--llvm", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    addon = str(args.addon.resolve())
    for loops, expect_compiled in ((40, True), (128, True), (129, False)):
        sb3 = args.out / f"loops{loops}.sb3"
        fixture(sb3, loops)
        native = run_native(args.llvm, sb3, "hotspot")
        interpreter, _ = run("node", ROOT / "web/xyo-node.mjs",
                             args.wasm, sb3, "off")
        builtin, blog = run("node", ROOT / "web/xyo-node.mjs", args.wasm,
                            sb3, "hotspot",
                            initial={"XYO_WASM_HOT_EMITTER": "builtin"})
        binaryen, clog = run(
            "node", ROOT / "web/xyo-node.mjs", args.wasm, sb3, "hotspot",
            initial={"XYO_WASM_HOT_EMITTER": "binaryen",
                     "XYO_BINARYEN_NODE_ADDON": addon})
        if len({native, interpreter, builtin, binaryen}) != 1:
            raise AssertionError(
                f"loops{loops}: LLVM={native}\ninterp={interpreter}\n"
                f"builtin={builtin}\nbinaryen={binaryen}")
        for label, log in (("builtin", blog), ("binaryen", clog)):
            whole = "xyo-wasm-hot: compiled script=" in log
            hybrid = "xyo-wasm-hot: hybrid script=" in log
            if expect_compiled and not whole:
                raise AssertionError(
                    f"loops{loops} ({label}) should compile the whole script: "
                    f"{log[-1500:]}")
            if expect_compiled and f"loops={loops}" not in log:
                raise AssertionError(
                    f"loops{loops} ({label}) compiled a different loop count")
            if not expect_compiled and whole:
                raise AssertionError(
                    f"loops{loops} ({label}) exceeded the cap but compiled "
                    f"the whole script")
            if not expect_compiled and not hybrid:
                raise AssertionError(
                    f"loops{loops} ({label}) should fall back to hybrid")
        print(f"PASS loops={loops}: "
              + ("whole-script compile" if expect_compiled
                 else "bounded hybrid fallback")
              + f"; values {builtin}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
