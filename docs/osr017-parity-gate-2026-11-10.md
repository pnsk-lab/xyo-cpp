# OSR-017 / CI: enforce exact ScratchBench parity

Parent: `a460e19`, work branch `work/osr-ci-v123`.

The 2026-11-10 GitHub Actions CI #80 report demonstrated
`2123/2123` official ScratchBench checks for each of
Interpreter, JIT HotSpot, JIT Eager and AOT. This tightens the
existing CI gate from a 96% tolerance to exact, per-variant parity.

- Require `passed === total` **and** `verdict === "passed"`.
- Require `total >= 2123` to prevent successful runs with a
  truncated suite, while allowing new official checks to be added.
- Keep malformed or incomplete result validation intact.
- Retain performance statistics as diagnostic data, not a gate.
- Add a hermetic Bun self-test with synthetic result files for six
  cases: exact pass, expanded pass, mismatch, suite shrinkage,
  inconsistent verdict, invalid counts.
- Register the self-test before scratch-bench installation and
  execution in the existing CI job. No JIT or interpreter source
  changes; no changes to benchmark upstream pin.

## Local validation

- `bun tools/ci/test-scratch-bench-summary.ts`: **6/6 PASS**.
- Positive: 2123/2123 for four variants and expanded 2124/2124.
- Negative: 2122/2123 with a misleading `passed` verdict,
  2122/2122 after suite truncation, `failed` verdict despite
  complete counts, and malformed negative count all correctly rejected.
- CI workflow YAML parsed, the new gate self-test is ordered after
  Bun setup and before the benchmark, and the branch push trigger
  remains intact.
- `git diff --check`: PASS.
- GitHub Actions CI #80 previously measured **2123/2123** for all
  four variants. This is evidence that the pinned benchmark version
  met the new gate, not a claim that the new gate ran on GitHub.
- The CI job still treats performance as informational; the
  externally measured TurboWarp reference is best-effort.

No runtime, JIT, AOT, interpreter, benchmark pin, or main branch
changes are part of this patch.
