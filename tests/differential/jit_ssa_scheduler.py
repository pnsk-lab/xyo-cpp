#!/usr/bin/env python3
"""Check LLVM SSA scheduler parity across Scratch loop forms."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from jit_repeat_ssa import fixture as repeat_fixture, run as repeat_run
from jit_repeat_copy_ssa import fixture as copy_fixture, run as copy_run
from jit_foreach_ssa import fixture as foreach_fixture, run as foreach_run
from jit_warp_induction import (
    create_fixture as induction_fixture,
    create_extended_fixture as extended_fixture,
    run as induction_run,
)

def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xyo"
    out = ROOT / "build/ssa-scheduler-differential"
    out.mkdir(parents=True, exist_ok=True)
    cases = [
        ("repeat", repeat_fixture,
         lambda p, keys, tier, fast, env: repeat_run(binary, p, tier, fast, keys, env)),
        ("copy", copy_fixture,
         lambda p, keys, tier, fast, env: copy_run(binary, p, keys, tier, fast, env)),
        ("foreach", foreach_fixture,
         lambda p, keys, tier, fast, env: foreach_run(binary, p, tier, fast, keys, env)),
        ("induction", induction_fixture,
         lambda p, keys, tier, fast, env: induction_run(binary, p, tier, fast, keys, 1000, env)),
        ("induction_extended", extended_fixture,
         lambda p, keys, tier, fast, env: induction_run(binary, p, tier, fast, keys, 1000, env)),
    ]
    for label, generate, execute in cases:
        for warp in (True, False):
            path = out / f"{label}-{'warp' if warp else 'nonwarp'}.sb3"
            keys = generate(path, warp)
            expected = execute(path, keys, "off", False,
                               {"XYO_JIT_SSA_SCHEDULER": "0"})
            assert expected.get("done") == "1", (label, warp, expected)
            for clock in ("1", "0"):
                for tier in ("eager", "hotspot"):
                    for new in ("0", "1", "auto"):
                        settings = {"XYO_JIT_WARP_CLOCK": clock}
                        if new != "auto":
                            settings["XYO_JIT_SSA_SCHEDULER"] = new
                        got = execute(path, keys, tier, True, settings)
                        if got != expected:
                            diff = {k: (expected[k], got[k])
                                    for k in keys if expected[k] != got[k]}
                            raise AssertionError((label, warp, clock, tier, new, diff))
                        print("PASS", label, "warp" if warp else "nonwarp",
                              tier, "scheduler=" + new, "clock=" + clock,
                              flush=True)

if __name__ == "__main__":
    main()
