#!/usr/bin/env python3
"""Analyze randomized complete-block 2^3 string-factorial timing data."""
import argparse
import json
import math
import random
import statistics
from collections import defaultdict
from pathlib import Path

VARS = ("A", "B", "C")
VARIANTS = [f"V{i}" for i in range(8)]
FACTORS = {f"V{i}": {"A": bool(i & 1), "B": bool(i & 2), "C": bool(i & 4)} for i in range(8)}


def percentile(xs, p):
    xs = sorted(xs)
    if len(xs) == 1: return xs[0]
    k = (len(xs) - 1) * p
    lo = math.floor(k); hi = math.ceil(k)
    return xs[lo] * (hi-k) + xs[hi] * (k-lo)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("raw_json")
    ap.add_argument("--out", required=True)
    ap.add_argument("--bootstrap", type=int, default=10000)
    ap.add_argument("--seed", type=int, default=918273)
    args = ap.parse_args()
    if args.bootstrap < 1:
        ap.error("--bootstrap must be positive")
    raw = json.loads(Path(args.raw_json).read_text())
    if not raw.get("rows"):
        raise ValueError("no timing rows found")
    groups = defaultdict(dict)
    kinds = sorted({r["kind"] for r in raw["rows"]})
    for r in raw["rows"]:
        if r.get("variant") not in FACTORS:
            raise ValueError(f"unknown variant: {r.get('variant')}")
        if "factors" in r and r["factors"] != FACTORS[r["variant"]]:
            raise ValueError(f"factor metadata contradicts {r['variant']}: {r['factors']}")
        value = float(r["median_ms"])
        if not math.isfinite(value) or value <= 0:
            raise ValueError(f"invalid duration: {value}")
        key = (r["kind"], r["block"])
        if r["variant"] in groups[key]:
            raise ValueError(f"duplicate row for {key}/{r['variant']}")
        groups[key][r["variant"]] = math.log(value)
    blocks = sorted({r["block"] for r in raw["rows"]})
    if not blocks or any(set(groups[(kind, b)]) != set(VARIANTS) for kind in kinds for b in blocks):
        raise ValueError("data must have every variant in every complete block")
    rnd = random.Random(args.seed)
    summary = {"source": str(Path(args.raw_json).resolve()),
               "blocks": len(blocks), "bootstrap_samples": args.bootstrap,
               "seed": args.seed, "kinds": {}}
    for kind in kinds:
        matrix = {b: groups[(kind, b)] for b in blocks}
        effects = {}
        # Factorial log-linear effects via orthogonal +/-1 contrasts. For a
        # With +/-1 coding, the isolated A/B/C high/low duration ratio is
        # exp(2*coef); pairwise and three-way alternating ratios use 4*coef
        # and 8*coef respectively. Higher-order terms are interactions, not
        # isolated speedups.
        for subset in [("A",), ("B",), ("C",), ("A","B"), ("A","C"),
                       ("B","C"), ("A","B","C")]:
            def contrast(block):
                return sum(((-1) ** (len(subset) - sum(FACTORS[v][x] for x in subset))) *
                           block[v] for v in VARIANTS) / 8
            # For singleton, coef is half the log(high/low); for interactions
            # this is the standard orthogonal coefficient on +/-1 coding.
            estimates = [contrast(matrix[b]) for b in blocks]
            draws = []
            for _ in range(args.bootstrap):
                resampled = [rnd.choice(blocks) for __ in blocks]
                draws.append(statistics.mean(contrast(matrix[b]) for b in resampled))
            mean_coef = statistics.mean(estimates)
            scale = 2 ** len(subset)
            multiplier = math.exp(scale * mean_coef)
            measure = ({1: "high_over_low_duration_ratio",
                        2: "pairwise_ratio_of_ratios",
                        3: "three_way_alternating_ratio"}[len(subset)])
            effects["".join(subset)] = {"mean_log_contrast": mean_coef,
                "contrast_measure": measure,
                "contrast_multiplier": multiplier,
                "ci95_contrast_multiplier": [
                    math.exp(scale*percentile(draws,.025)),
                    math.exp(scale*percentile(draws,.975))],
                "n_blocks": len(blocks)}
        by_variant = {}
        for v in VARIANTS:
            paired_log = [matrix[b]["V0"] - matrix[b][v] for b in blocks]
            draws = [statistics.mean(rnd.choice(paired_log) for __ in blocks)
                     for _ in range(args.bootstrap)]
            by_variant[v] = {"median_ms": statistics.median(math.exp(matrix[b][v]) for b in blocks),
                "geometric_mean_speedup_vs_V0": math.exp(statistics.mean(paired_log)),
                "paired_speedup_ci95": [math.exp(percentile(draws,.025)),
                                         math.exp(percentile(draws,.975))]}
        summary["kinds"][kind] = {"factorial_contrasts": effects, "variants": by_variant}
    Path(args.out).write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))

if __name__ == "__main__": main()
