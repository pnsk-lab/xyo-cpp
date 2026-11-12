# LLVM arithmetic list projections and discarded numeric classifier — 2026-11-10

## Adopted: communicate the result of complete numeric coercion

[Mixed numeric-region lowering](../src/jit/numeric_region_mixed_ssa.inc) already
classifies every ListRead SSA result by all of its consumers:

- **Exact**: a written output or an input to a Phi; preserve the original tag.
- **Comparison/index**: use stricter Scratch numeric-comparison compatibility.
- **Arithmetic-only**: only Add/Subtract/Multiply/Divide/Modulo consumers.

The arithmetic-only path performed full `emitToNumF64(raw)` and then checked the
original `raw` tag again, accepting Number/String/Bool and rejecting Color or
undefined. That extra acceptance test is unnecessary: Scratch numeric coercion
accepts **all** Values and turns invalid numeric inputs into zero. It has already
happened before the old acceptance gate.

The adopted path therefore:

1. Retains complete numeric coercion, including string cache/parsing and effects.
2. Omits the additional acceptance test on the raw tag for arithmetic-only use.
3. Keeps the finite-element guard when enabled. With finite-element checking OFF,
   it omits an additional ORD check because numeric coercion cannot return NaN.
4. Preserves exact/Phi and comparison/index projection rules, bounds/layout
   guards, arithmetic NaN bailouts, rollback, publication and scheduler boundaries.

Default **ON**; `XYO_JIT_COERCION_PROJECTION=0` restores the historical gates.
No helper memory attributes, noalias or fast-math flags are added. A Value read
from a list is not replaced or numerically canonicalized in the original list.

This differs from assuming a homogeneous list: each element still undergoes full
Scratch coercion, and arbitrary mixed-type lists remain supported.

## Optimized IR evidence

The diagnostic sum fixture selects the real `mixed.list.bounded` region. Before,
its post-conversion merge contains a string-tag mask, tag comparisons, an OR and
an AND with the finite test. After, only the required finite test remains.

The regression names the historical gate `mixed.list.raw.accepted` and the
finite test `mixed.list.finite`. It asserts on **optimized** IR that:

- The typed list region actually exists.
- The redundant raw gate exists only with the optimization OFF.
- The finite guard remains exactly when finite-element policy is enabled.

[Expanded differential test](../tests/differential/jit_numeric_classify.py).

## Normal source-derived workload measurements

Same frozen Release executable and unchanged source-derived ScratchBench SB3s,
CPU16 pinned, Eager, `--fps 30`, 10 interleaved blocks per configuration. Order
uses a fixed shuffle and reversal of the preceding block. GPU/PAR/PARFOR are OFF;
ordinary warp budgeting remains ON. Project clocks, kernel bodies and operations
per unit were not changed. All checksums agree.

| Workload | Paired geometric-mean speed ratio | Winning blocks |
|---|---:|---:|
| `list_read` | **1.3471x** | **10/10** |
| `algo_matmul` | **1.1527x** | **10/10** |
| `list_replace` control | 0.9872x | 5/10 |

The write control does not select the modified arithmetic list-read path. Its
**complete optimized IR** is byte-identical after normalizing only host pointers
and heap-string literal payload addresses. This is not based on a truncated IR
prefix. [Control evidence](../build/llvm-helper-effects/control-ir/result-full.json).

[All paired results](../build/llvm-helper-effects/paired-trials/summary.json),
[frozen inputs and hashes](../build/llvm-helper-effects/paired-trials/manifest.json),
[raw runs](../build/llvm-helper-effects/paired-trials/runs.json),
[measurement driver](../tests/bench/algos/bench_llvm_numeric.py).

After removing both discarded experiments, a final independently frozen build
repeated eight blocks on the same CPU:

| Final retained build | Paired speed ratio | Winning blocks |
|---|---:|---:|
| `list_read` | **1.3690x** | **8/8** |
| `algo_matmul` | **1.1984x** | **8/8** |
| `list_replace` control | 0.9954x | 1/8 |

[Final paired summary](../build/llvm-helper-effects/final-projection-paired/summary.json),
[final frozen provenance](../build/llvm-helper-effects/final-projection-paired/manifest.json).
These are the primary final measurements; small differences from the earlier
batch are not treated as extra speedup attributable to this change.

### Full 47-workload suite

The full unmodified performance SB3 was also frozen. HotSpot, CPU16,
`--fps 30`, all **47/47** positive rates and **39/39** deterministic checksums are
required for every run. The official `scoreRuntime` implementation (score v2)
calculates the aggregate from per-item medians. Scheduler checksums are not
compared because they intentionally depend on timing.

| Batch | OFF aggregate | ON aggregate | Ratio | list_read | algo_matmul |
|---|---:|---:|---:|---:|---:|
| Initial, 3 pairs | 52,406 | 51,843 | 0.9892x | 1.3907x | 1.1677x |
| Balanced order, 4 pairs | 57,939 | 58,324 | 1.0066x | 1.3595x | 1.2187x |

The local list-read/matmul improvement reproduces in both full-suite batches.
The small aggregate change is **not established as a stable general speedup**:
the first batch is negative and unrelated/timing-sensitive items fluctuate.
Do not report the last +0.66% alone as a proven aggregate gain or compare these
E-core/headless absolute scores to earlier P-core/SDL measurements.

The default-on decision is based on the proven redundant gate, exact parity,
repeatable target improvements and unaffected-control IR, not a claim that
47/47 items or total score all improve.

[Initial full-suite summary](../build/llvm-helper-effects/full-suite-trials/summary.json),
[balanced summary](../build/llvm-helper-effects/full-suite-balanced/summary.json),
[full-suite driver](../tests/bench/algos/bench_llvm_projection_suite.py).

## Discarded: integer-only classification of plain IEEE doubles

An independent opt-in trial replaced the fast signed tag test + FP NaN repair
with the exact predicate:

```text
unsigned(bits << 1) <= 0xffe0000000000000
```

It accepts both signs of finite values and infinity, rejects every sign/payload
of NaN and all tagged Values, and lets the accepted path use the bitcast without
an ORD/select. The existing slow coercion was retained.

Semantics passed, including raw signed/signaling NaNs. Nevertheless, on the same
10-block experiment it gave **0.9331x** list_read throughput (1/10 wins),
0.9876x write control and 1.0225x matmul. Combined with the accepted projection,
it was worse for list_read than projection alone. It has been **removed from
production code**, rather than retained as another unsuccessful experimental
flag. This is related to earlier rejected NaN-excluding predicates, not evidence
that integer classification is universally faster.

The archived paired-trials executable still contains that trial for reproducible
historical measurements. Benchmark/test drivers reject requests for discarded
profiles when the input executable lacks the feature literal. Normal profiles
compare only baseline and arithmetic projection.

## Correctness coverage

- Initial four-profile matrix: 30 list fixtures, **492** exact JIT/AOT comparisons.
- Expanded selected-optimization matrix: **40** fixtures and **326** comparisons
  at that checkpoint, with 4 additional native raw-Value runs. Added subtract,
  multiply, both sides of division/modulo, chains, branch/Phi, shared comparison
  and list-derived index uses; AOT coverage is also extended for modulo/Phi/index.
- Finite-element policy ON/OFF, real Eager/HotSpot compilation, loaded AOT,
  mixed numeric/nonnumeric/whitespace strings, Boolean, ±0, subnormal, NaN/Inf,
  bounds and flat/SBO/deque/gap/chunked/adaptive storage are covered.
- The [native raw-Value test](../tests/platform/llvm_numeric_projection_raw_smoke.cpp)
  injects Color/undefined and signed/signaling NaN payloads not expressible in
  SB3 JSON. It independently evaluates the sum and checks all original list bits
  unchanged, in interpreter, actual JIT and actual AOT (780 checks per process).
- Full `xyo-tests` passed with both initial trials enabled, including added raw
  arithmetic inputs. Final default-on projection regression is registered as
  `xyo-jit-numeric-coercion-guards` in CTest.
- Independent review found no correctness defect; the requested operation and
  Phi/index/IR assertions were added rather than relying only on sum fixtures.
- Final CTest matrix includes **190** JIT/AOT comparisons, **4 x 780** raw-Value
  checks, and **18 actual native-yield configurations** using the existing
  list-checkpoint test. Every configuration completes 1,025 iterations with
  16 real handoffs and 16 host mutations, checking clear/resize/reallocation,
  storage epoch, direct string writes and flat/deque/adaptive transitions.
- Final retained-source `xyo-tests` and the complete matrix both pass. The
  latest full headless Scratch parity project produces byte-identical ON/OFF
  output: **2325/2656** in each. This is not a 100% parity claim; the existing
  headless failures are unchanged.

[Final native checkpoint/unit CTests](../build/llvm-projection-checkpoint-ctest.log),
[final matrix summary](../build/llvm-helper-effects/tests/numeric-coercion-guards/summary.json),
[native checkpoint ON log](../build/llvm-helper-effects/tests/numeric-coercion-guards/checkpoints-1.log),
[full headless parity evidence](../build/llvm-helper-effects/final-headless-parity/summary.json).

[Initial parity log](../build/llvm-classify-parity.log),
[expanded log](../build/llvm-projection-expanded.log),
[initial unit log](../build/llvm-classify-unit.log),
[raw CTest](../build/llvm-projection-raw-ctest.log),
[default-adoption CTest](../build/llvm-projection-adoption-ctest.log).

## Reproduce

```sh
cmake --build build/llvm-helper-effects \
  --target xyo-cli xyo-tests xyo-llvm-numeric-projection-raw-smoke \
    xyo-numeric-region-list-checkpoint-smoke -j 3
ctest --test-dir build/llvm-helper-effects --output-on-failure \
  -R '^(xyo-tests|xyo-jit-numeric-coercion-guards)$'
python3 tests/differential/jit_numeric_classify.py build/llvm-helper-effects/xyo \
  --out build/projection-recheck --aot \
  --raw-smoke build/llvm-helper-effects/tests/xyo-llvm-numeric-projection-raw-smoke \
  --checkpoint-smoke build/llvm-helper-effects/tests/xyo-numeric-region-list-checkpoint-smoke
python3 tests/bench/algos/bench_llvm_numeric.py build/llvm-helper-effects/xyo \
  --out build/projection-paired-recheck --blocks 10 --cpu 16
python3 tests/bench/algos/bench_llvm_projection_suite.py build/llvm-helper-effects/xyo \
  --out build/projection-full-recheck --blocks 4 --cpu 16
```

Benchmark output directories must be new. The previous asin/acos helper-attribute
optimization is independent and unchanged.
