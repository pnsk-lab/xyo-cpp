# START-014 — Native AOT metadata allocation optimization (2026-11-11)

## Scope

START-013 proved that AOT v3's peak resident-memory regression on this
workload was mostly glibc [heap] page residency, not extra live Scratch
data, and that malloc_trim reduced RSS only at a real startup penalty.
START-014 profiles individual load phases and experiments with a read-only
metadata representation to reduce transient allocations without trimming.

All tests used the frozen ScratchBench project in
`build/start014-frozen/parity.sb3` and a native AOT v3 object created
from that exact project (`v3.o`). Relative performance numbers from
different binaries, workloads, or days are **not** interchangeable.

## AOT load phase allocation snapshots

Using `XYO_AOT_WORKER_DIAG=1`, native glibc malloc statistics show:

| Step | AOT v2 used heap | AOT v3 used heap |
|---|---:|---:|
| After JSON metadata parse | 65,513,904 B | 66,793,616 B |
| After slot binding | 99,669,616 B | 68,337,536 B |
| After script registration | 100,062,576 B | 68,829,120 B |

The ~30 MiB gap appears in **slot binding**: v2 populates the full
BlockCached cache, consuming many free malloc chunks left by parsing;
v3 instead binds only lazy descriptors, leaving that free arena space
available for later execution. This is expected lazy behavior and does
not by itself imply a leak.

## Discarded: eager container reserve

Experimented with reserving capacities for AOT slot vectors, descriptor
ownership, script cache buckets and per-script manifests. On 20
alternating same-binary first-output A/B pairs, CPU 10:

- Original startup median: 254.87 ms
- Reserve-enabled startup median: 254.12 ms
- Reserve-enabled wins: 9/20
- Geometric original/reserve speed ratio: 0.9969

No repeatable gain was found; the extra reserve experiment was removed
from runtime code. Log: `build/start014-frozen/reserve-startup-ab.log`.

## Experimental native yyjson metadata cursor

New: `src/jit/aot_meta_view.hpp`, opt-in CMake `XYO_AOT_YYJSON=ON`,
runtime `XYO_AOT_FAST_META=1`.

- Standard builds still use the existing self-contained JSON parser and
  have **no** additional yyjson dependency.
- The new mode holds one owned yyjson document and traverses `slots`,
  `scripts`, and nested procedure/manifest arrays with read-only cursors.
  It avoids eagerly constructing thousands of `JsonValue` and
  `std::map` nodes.
- Strings are borrowed as `std::string_view` only while the metadata
  document is alive. Persistent symbol keys and strings are explicitly
  copied into owning structures when binding native AOT resources.
- Invalid yyjson input fails instead of silently falling back.
- The option is experimental; it is not activated by `XYO_AOT_YYJSON=ON`
  alone. Set `XYO_AOT_FAST_META=1` explicitly.

Intermediate phase diagnostics showed ~2.2 MiB less allocated glibc heap
after metadata parsing. This savings is not a whole-run RSS claim.

### Paired measurements — finalized string_view implementation

Same native binary (`build/start014-fastmeta/xyo`), same v3 AOT object,
CPU 10 pinned, alternating control vs `XYO_AOT_FAST_META=1`:

| Metric | Legacy metadata | Direct yyjson |
|---|---:|---:|
| First output median, 24 pairs | 259.03 ms | 254.65 ms |
| First output wins | — | 17/24 |
| Geometric control/yyjson speed ratio | — | 1.0230x |
| Complete 2656/2656 median, 3 pairs | 17.526 s | 17.446 s |
| Complete run wins | — | 3/3 |
| Complete run peak RSS median | 378168 KiB | 378188 KiB |

The read-only metadata path improves *startup* by a modest few
milliseconds. It provides no demonstrated whole-run memory saving.
The three complete runs suggest similar completion times; they are too
few to establish a robust performance gain at that magnitude.

Logs: `build/start014-frozen/meta-borrowed-startup-ab.log` and
`build/start014-frozen/meta-borrowed-full-ab.log`. The current generic
benchmark harness is `tools/bench_start013_trim_ab.py`, with
`--switch XYO_AOT_FAST_META`; the older log labels its experimental
column "trim", referring to the flag-enabled arm.

## Semantic regression coverage

- Native AOT v2 direct yyjson: **Scratch Parity 2656/2656 PASS**.
- Native AOT v3 direct yyjson: **Scratch Parity 2656/2656 PASS**.
- Native AOT v3 parallel worker test under direct yyjson: PASS.
- Dedicated fastmeta build CTest: 4/4 PASS for parallel, serial,
  fastmeta+parallel and trim+parallel AOT smoke tests.
- Default AOT remains v2. AOT v3, malloc trim, and direct yyjson metadata
  are all opt-in; no extra yyjson dependency exists in default builds.
- The native no-JIT CLI also configured, compiled and linked successfully.
- Lazy v3's source-edit safety is **not** proven by this change.

### Reproduction

```sh
cmake -S . -B build/start014-fastmeta \
  -DXYO_AOT_YYJSON=ON -DXYO_BUILD_TESTS=ON
cmake --build build/start014-fastmeta -j4 \
  --target xyo-cli xyo-aot-lazy-parallel-smoke
ctest --test-dir build/start014-fastmeta \
  -R 'xyo-aot-lazy-parallel' --output-on-failure

python3 tools/bench_start013_trim_ab.py \
  --bin build/start014-fastmeta/xyo \
  --project build/start014-frozen/parity.sb3 \
  --aot build/start014-frozen/v3.o \
  --cpu 10 --pairs 24 --switch XYO_AOT_FAST_META

python3 tools/bench_start013_trim_ab.py \
  --bin build/start014-fastmeta/xyo \
  --project build/start014-frozen/parity.sb3 \
  --aot build/start014-frozen/v3.o \
  --cpu 10 --pairs 3 --full --switch XYO_AOT_FAST_META
```

No commits or pushes were performed. The xyo-cpp working tree contains
other ongoing uncommitted work; do not reset or overwrite it.
