#!/usr/bin/env python3
"""PROF-007: validate selective CFG hint profitability without relaxing type proof."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/differential"))
import jit_cfg_type_hints as existing


def run(binary: Path, project: Path, keys: list[str], jit: str,
        cfg: bool, selective: bool) -> tuple[dict[str, str], str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS": "json",
        "XYO_GPU": "0",
        "XYO_PAR": "0",
        "XYO_JIT_VERBOSE": "1",
        "XYO_JIT_CFG_TYPE_HINTS": "1" if cfg else "0",
        "XYO_JIT_CFG_TYPE_HINTS_SELECTIVE": "1" if selective else "0",
    })
    proc = subprocess.run(
        [str(binary), "run", str(project), "--jit", jit, "--frames", "1200"],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=120,
    )
    if proc.returncode:
        raise RuntimeError((project, jit, cfg, selective, proc.stderr[-1500:]))
    raw = proc.stdout
    result = json.loads(raw[raw.index("{"):raw.rindex("}") + 1])["Bench"]
    return {name: result[name] for name in keys}, proc.stderr


def main() -> None:
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xyo"
    dest = ROOT / "board/prof-007/type-safety"
    dest.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        path = dest / ("warp.sb3" if warp else "nonwarp.sb3")
        keys = existing.fixture(path, warp)
        reference, _ = run(binary, path, keys, "off", False, False)
        broad_promoted = False
        selective_promoted = False
        for tier, cfg, selective in (
            ("eager", False, True),
            ("eager", True, False),
            ("eager", True, True),
            ("hotspot", True, True),
        ):
            actual, logs = run(binary, path, keys, tier, cfg, selective)
            if actual != reference:
                raise AssertionError(("numeric", warp, tier, cfg, selective, reference, actual))
            if cfg and "cfg numeric-shadow" in logs:
                if selective:
                    selective_promoted = True
                else:
                    broad_promoted = True
            print("PASS", "number", warp, tier, cfg, selective, flush=True)
        # This fixture has only two numeric consumers per uncertain write:
        # the old CFG mode promotes it, while the new profitability filter
        # deliberately declines to run the expensive type-flow analysis.
        if not broad_promoted or selective_promoted:
            raise AssertionError(("incorrect profitability filter", warp,
                                  broad_promoted, selective_promoted))

        mixed = dest / ("mixed-warp.sb3" if warp else "mixed-nonwarp.sb3")
        mixed_keys = existing.mixed_fixture(mixed, warp)
        reference, _ = run(binary, mixed, mixed_keys, "off", False, False)
        for tier in ("eager", "hotspot"):
            actual, logs = run(binary, mixed, mixed_keys, tier, True, True)
            if actual != reference or "cfg numeric-shadow" in logs:
                raise AssertionError(("mixed", warp, tier, reference, actual, logs[-800:]))
            print("PASS", "mixed", warp, tier, flush=True)


if __name__ == "__main__":
    main()
