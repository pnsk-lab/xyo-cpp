# Scratch short-string interning experiment — 2026-11-07

## Rationale

The 8-byte NaN-boxed Value stores the existing tagged string as a GC-owned
SStr pointer. Direct immediate small-string values would require coordinated
changes to JIT, GC, AOT and WASM tag handling. Instead this experiment reuses
existing immutable strings and preserves the Value/SStr ABI.

## Implementation

Enable with XYO_STRING_SHORT_INTERN=1 or --string-short-intern=on.
Default: off. The cache admits only 2..4 UTF-16 code unit ASCII strings,
including embedded NUL. The direct-mapped table has 4096 slots, with
strong GC roots for successful admissions. Keys include the leaf encoding
(UTF-16 or Latin-1). Collisions bypass the interner; there is no eviction.
Atomic publications allow concurrent creation without stale pointers.
The cache covers only newly produced short join results, through flat and
direct-leaf concat helpers. Plain SStr::create and sstrFromAscii bypass it,
preserving the lexical scope of existing SB3-list and numeric caches.
Existing character, integer and SB3-list caches remain.

Caution: admitted entries remain reachable for the lifetime of the
process, with up to 4096 pinned GC objects plus a 32 KiB pointer table.

## Native benchmark: i7-12700, Release

250,000 operations and three independent processes per variant, GC
collection deferred for measurement, process median elapsed time.
The GC allocation count is not retained RSS.

| Leaf | Workload | Off time | On time | Off GC alloc | On GC alloc |
|---|---|---:|---:|---:|---:|
| UTF-16 | Ten repeated 2-char joins | 20.8ms | 12.5ms | 38.50MB | 19.00MB |
| Latin-1 | Ten repeated 2-char joins | 17.0ms | 11.9ms | 37.25MB | 18.50MB |
| UTF-16 | 250k unique 4-char joins | 10.2ms | 16.6ms | 20.50MB | 20.50MB |
| Latin-1 | 250k unique 4-char joins | 9.0ms | 14.8ms | 19.25MB | 19.25MB |

Thus repeated small results reduce GC allocation bytes by approximately
50%, with a speed improvement in this native workload. Unique short
joins produce no allocation savings and regress by roughly 60% in this
native workload. The extra work is hash, cache probe and admission check.

## Scratch SB3 benchmark: eager LLVM JIT, times in ms

Four generated Scratch workloads, each checked against an expected
result; five randomized interleaved process repetitions for each of
Rope/Flat/Append-log x intern on/off, internal three-timing median,
work multiplier 8, transient-append optimization disabled.
Times have about 1ms resolution.

| Workload | Rope off/on | Flat off/on | Append-log off/on |
|---|---:|---:|---:|
| Short dynamic join | 19 / 15 | 20 / 15 | 20 / 15 |
| Long append | 3 / 3 | 35 / 34 | 10 / 10 |
| Contains during append | 12 / 12 | 12 / 12 | 16 / 15 |
| Fork and share snapshots | 6 / 5 | 56 / 57 | 284 / 281 |

Interleaved report: build-list-epoch/short-intern-ab-interleaved/results.json
with per-run samples. A separate numeric-join batch showed no robust
benefit beyond millisecond quantization. Most long-string joins bypass
the short-result interner entirely.

The win is localized to hot repeated short joins. Other differences are
at or below the precision of the clock and do not justify a global claim.

## Correctness

Native CTest verifies intern on/off with UTF-16/Latin-1, binary-exact
comparison, GC root retention, a concurrent admission stress, and
non-interned literal/numeric constructors. The generated Scratch SB3 test
verifies 42 combinations of join structure, leaf structure, JIT mode and
intern mode including six CLI overrides. Existing xyo-tests also ran
successfully **after a complete rebuild** with interning enabled
(40.73s). A previous broad SStr::create interning prototype failed tests
for project-load scope and numeric string identity; it was replaced by
join-result-only interning before the passing run.

## Reproduction

- CMake target: xyo-string-short-intern-smoke
- CMake target: xyo-string-short-intern-native-bench
- CTest: xyo-string-short-intern-sb3-parity
- Script: tests/bench/algos/bench_scratch_string_storage.py --short-intern
- Interleaved results: build-list-epoch/short-intern-ab-interleaved/results.json
- Interleaved runner: tests/bench/algos/bench_short_intern_ab.py

## Conclusion

Keep short interning disabled by default. Direct interning has a clear
repeated-string benefit, but high cardinality creates measurable
overhead. A next experiment should measure actual runtime hit ratios
and consider adaptive admission before shipping a default.
