# Native par-for lazy ORC: exercising the real full Parity suite

Branch `work/parfor-parity-integration-v123`, parent commit `2201d13`
(`work/parfor-lazy-safety-v123`). The optimization remains **opt-in**:
`XYO_JIT_LAZY_PARFOR_PARTITION=1`.

## Why another integration test?

Earlier testing proved correct native callbacks on an isolated Scratch
project but did not execute the experimental ORC partition path during
the 2,656-case extended Parity run. That project did not naturally
contain a par-for-eligible script.

This patch adds an independent, invisible Scratch sprite to the
existing full Parity `.sb3`, preserving its 26/24 original targets,
assets, monitors and original block graphs. The sprite has two separate
warp/par-for loops, each with 8,192 iterations and its own callback.
It also has 240 inert variable updates, yielding four LLVM functions
and **3,738 pre-optimization IR instructions**. With HotSpot's normal
`--jit-lazy auto` heuristic, this is enough to select the deferred
tier without forcing unrelated Parity scripts to Lazy.

The comparator runs the **same augmented project and same binary**
with the normal callback safety guard, then with the opt-in partition.
It verifies:
- all reported Parity tests still run (at least 2,123 checks);
- the exact same failed group counts and failed test identities
  for all deterministic groups;
- the overall count difference, if any, is *fully explained* by the
  real-time `Sensing: Date And Time` test group (reported explicitly);
- only the experiment executes ORC
  `requested=1 materialized=3 callbacks=2`;
- the real native worker pool dispatches both 8,192-iteration callbacks;
- missing/partial failure details cause an error, rather than
  silently satisfying the comparison.

The Date/Time exception is required because consecutive independent
runs can cross a time boundary. This is not a general waiver for
scheduler- or timing-dependent groups.

## Local evidence

LLVM 23.1.4 Release x86_64. Binary from safety branch `2201d13`.
`XYO_PARFOR=1`, `XYO_JIT_LAZY_MODE=auto`, SDL offscreen,
`XYO_JIT_ASYNC=0`.

### CI-pinned source

The CI workflow pins `gradylink/scratch-bench` at
`cca6ab15108301bee6b7249db60f00e82f141159`; its
`parity` submodule is `7e5fe235443c63230991ef108ff2fe3607fcb382`.
The submodule commit was retrieved from the existing local Git
object database, without modifying the shared checkout, and
repackaged via locally installed `fractch` into:
`build-integration/pinned-parity.sb3`, SHA256
`947c9ece232832979ddf3eac64682ea2a414f72058af50a62efd11b477e345bc`.

Because the local conversion tool's version may differ from the
version used by the pinned GitHub workflow, this is the *pinned
source*, not a claim that the locally generated archive is byte
identical to GitHub Actions' archive.

- Original parity fixture, without injected sprite:
  **2,121/2,123** (two failures in
  `Looks: Size bounds per costume`) under these local
  SDL and HotSpot flags.
- Same augmented project, default safety guard:
  **2,121/2,123**, 2 worker-pool dispatches,
  no experimental ORC partition.
- Same augmented project, experimental ORC co-partition:
  **2,121/2,123**, two worker-pool dispatches, one actual
  ORC partition with two callbacks; failed test identities
  and counts match exactly.

The unmodified parity project's two failures also occur without
the additional sprite; they are **not introduced by ORC partitioning**.
The GitHub ScratchBench CI run #81 previously reported 2,123/2,123
for its standard (non-augmented) run; that is a different runner/
renderer/packaging context and does not contradict the local result.

### Extended 2,656-case source

The existing extended source, SHA256
`63baccef6c7288cfb13778ef9a6015a6ee0e310af78d5378ce6bd3346c12f13b`,
also accepted the augmented probe. The initial experiment's baseline
and opt-in both reported **2,623/2,656**, and the experiment selected
and grouped both callbacks.

A second run crossed a real-time test boundary and produced
2,623 vs 2,622 with the single extra failure in
`Sensing: Date And Time`. That prompted the comparator to
recognize *only* the enumerated wall-clock group as variable, with
all other failures and count differences still checked strictly.

## CI automation and unit coverage

- `tests/differential/jit_lazy_parfor_parity_integration.py` generates
  the independent probe, merges the source project and runs both
  configurations. It emits
  `parity-comparison.json` without storing the external source in Git.
- `tests/differential/test_jit_lazy_parfor_parity_integration.py`
  checks asset/target preservation, block ID remapping,
  exact failure count and test identity comparison, and
  older/newer output formats.
- CTest `xyo-jit-parfor-parity-merge-unit`: **PASS**
  (10 Python unit cases).
- The ScratchBench GitHub job now runs this integration **after**
  the four official variants' normal Parity measurement; its
  independent **2,123/2,123** strict gate is unchanged.
- The generated comparison JSON is included among CI artifacts.

## Lifetime / concurrency boundary

`xyo_par_for` uses a dedicated WorkerPool. Its synchronous
`WorkerPool::run` waits for every task's completion after the
native callback has returned; only then can the dispatching script
return to the owning execution thread. Background JIT compilations
are similarly joined during `JitEngine::Impl` destruction before
its backend member is destroyed. These source-level ordering
properties support **ordinary single-threaded runtime teardown**
after an execution batch.

**Destroying a Runtime from a different thread while its native
callback is executing is not a supported operation** and has not
been proved safe. Doing so would require an explicit lifetime
protocol; this change does not claim to solve it or perform a
potentially undefined-behavior destruction stress test.

This branch changes only tests, documentation and the CI workflow.
It does not change the JIT implementation, the default par-for
guard, or `main`.


## CI run #82 / LLVM 18 header compatibility

GitHub Actions run
`https://github.com/pnsk-lab/xyo-cpp/actions/runs/42934249451`
ran all eight jobs on push `77c391d`.

Six jobs passed: Linux (LLVM 23), dedicated native OSR,
Windows JIT, both macOS architectures and ScratchBench.
ScratchBench's official Parity gate **passed**, and the added
co-partition integration also **passed** with real ORC
materialization.

Two LLVM 18.1.3 jobs failed before link:
Linux fully static and the nanosvg fallback build both failed at
`#include <llvm/ExecutionEngine/Orc/IRPartitionLayer.h>`.
This is a compile-time API mismatch introduced by the new experimental
policy header, not a Parity failure.

The compatibility fix keeps a version-specific ORC partition type:
- LLVM 18/19: `CompileOnDemandLayer::GlobalValueSet`
- LLVM 20+: `IRPartitionLayer::GlobalValueSet`

A shared `ParForPartitionValueSet` alias avoids version-specific
types in the callback implementation and policy IR regression.
The previous function-pointer `Use` validation, default Eager
fallback and actual callback co-materialization are unchanged.

The nanosvg LLVM 18 CI job also now builds and runs
`xyo-jit-parfor-partition-policy`, providing explicit LLVM 18
API regression coverage beyond merely compiling `xyo`.

Local LLVM 23 Release rebuild: **PASS**; four focused CTests:
**4/4 PASS**; with the freshly rebuilt executable, the pinned
2,123-case augmented Parity result remained **2121/2123 for both
configurations**, with both failed-case IDs 8 and 9 unchanged,
and verified lazy co-materialization.

The LLVM 18 CI outcome after this fix must be checked separately.
