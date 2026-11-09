#!/usr/bin/env python3
"""Run real NVIDIA NGX alpha-occlusion stress in both temporal modes.

Only registered as an opt-in GPU test. A lower ghosting score means less
stale red in the newly uncovered background. This is NOT a perceptual metric.
"""
import argparse
import os
import re
import subprocess
import sys

SCORE_RE = re.compile(
    r"DLSS moving alpha\+occlusion: trailing red ([0-9]+(?:\.[0-9]+)?)"
)


def run(executable: str, safe: bool) -> float:
    env = os.environ.copy()
    env.update({
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_SCENE_CUT_SYNC": "1",
        "XYO_DLSS_MOTION_COST_CLIP": "0",
        "XYO_DLSS_STRESS_FRAMES": "90",
        "XYO_DLSS_STRESS_ALPHA_OCCLUSION": "1",
        "XYO_DLSS_STRESS_ALPHA_SAFE": "1" if safe else "0",
    })
    result = subprocess.run(
        [executable], env=env, capture_output=True, text=True,
        timeout=40, check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"alpha test safe={safe}: exit={result.returncode}\n"
            f"{result.stdout}\n{result.stderr}"
        )
    match = SCORE_RE.search(result.stdout)
    if not match:
        raise RuntimeError(f"alpha score missing, safe={safe}\n{result.stdout}")
    return float(match.group(1))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    args = parser.parse_args()
    try:
        baseline = run(args.binary, safe=False)
        safe = run(args.binary, safe=True)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    improvement = baseline - safe
    print(
        f"NVIDIA alpha-occlusion score: normal={baseline:.3f}, "
        f"safe={safe:.3f}, improvement={improvement:.3f}"
    )
    # Fail regressions in which the safe mode *increases* stale color.
    # On RTX 3070, these are 0.037 and 0.008, respectively.
    if safe > baseline + 0.005:
        print("FAIL: guarded temporal mode increased residual ghosting")
        return 1
    if baseline >= 0.03 and improvement < 0.01:
        print("FAIL: alpha-safe failed to reduce an observable baseline trail")
        return 1
    print("PASS: alpha-safe temporal reset reduces or preserves visual quality")
    return 0


if __name__ == "__main__":
    sys.exit(main())
