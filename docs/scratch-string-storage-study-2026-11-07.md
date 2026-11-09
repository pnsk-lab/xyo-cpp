# Scratch-specific String storage research (2026-11-07)

## Scope and correctness

- Reference semantics: vendored `refs/scratch-vm/src/blocks/scratch3_operators.js`
  and `refs/scratch-vm/src/util/cast.js`. `join` is `String(a)+String(b)`;
  `letter of` and `length` count UTF-16 code units; `contains` lowercases
  both operands. Scratch comparison first tries numerical coercion and falls
  back to case-insensitive string comparison.
- `operator_length` can be constant-time in every representation (`SStr::len`).
- `data_listcontents` currently builds an ordinary `std::u16string`
  and then one flat `SStr`. Switching join representation has no direct
  effect on this path; verify that the list's delimiter choice is preserved.
- Persistent string snapshots matter: variable assignments and lists can
  preserve earlier values; later joins must never mutate them.
- `SStr::view()` requires a contiguous buffer. Deep Rope is cheap to
  concatenate but is flattened when a contiguous consumer requests it.
  `unitAt()` can walk shallow trees, flattening deep ones.
- JIT's existing transient append optimization is a separate, proven
  single-variable / no-observer specialization. **Do not attribute its
  improvement to the globally selected storage type.**

## Static occurrence counts (not runtime frequencies)

Analyzed 26 locally available SB3 projects from `debug/projects`
and `scratch-bench/.cache/build`; several are related project variants.
Syntactic self-append (`s = join(s, x)`) is found in
189 places.
Selected opcode counts:

| Opcode | Static count |
|---|---:|
| `operator_join` | 1346 |
| `operator_letter_of` | 411 |
| `operator_contains` | 252 |
| `operator_length` | 157 |
| `operator_equals` | 5369 |
| `operator_lt` | 3061 |
| `operator_gt` | 1715 |
| `data_listcontents` | 0 |

These counts alone do not measure runtime hotness, and `operator_equals`
may compare numbers, not just strings.

## Measured results

Harness: `tests/bench/algos/bench_scratch_string_storage.py`; actual
generated Scratch 3 SB3 scripts; Release xyo binary from `build-list-epoch/xyo`;
LLVM 23.1.4; Intel Core i7-12700 (20 logical CPUs); `--jit eager`;
`XYO_GPU=0`; `XYO_PAR=0`.
For each workload/policy, three separate process runs, each with one warmup
and three internally timed invocations. Numbers below are medians of
process medians, in **milliseconds** (lower is better). CPU affinity not
pinned (`--cpu -1`). Scratch's days-since-2000 clock here has roughly
1 ms quantization, so comparisons of nearly equal small values should
not be treated as significant.

### Raw storage policy (XYO_JIT_TRANSIENT_STRINGS=0)

| Workload | Rope | Flat | Append-log |
|---|---:|---:|---:|
| 自己追記（読み取りなし） | 3 | 34 | 11 |
| 自己追記＋先頭文字 | 9 | 34 | 13 |
| 自己追記＋末尾文字 | 5 | 35 | 14 |
| 自己追記＋contains | 13 | 12 | 16 |
| 旧文字列から分岐 | 6 | 57 | 283 |
| 連結で構築後の位置アクセス | 7 | 6 | 7 |
| 数字の文字列追記 | 3 | 36 | 11 |
| 短い文字列の動的連結 | 14 | 13 | 15 |
| リスト内容の文字列化 | 4 | 4 | 4 |

### Realistic JIT policy (XYO_JIT_TRANSIENT_STRINGS=1)

| Workload | Rope | Flat | Append-log |
|---|---:|---:|---:|
| 自己追記（読み取りなし） | 1 | 37 | 1 |
| 自己追記＋先頭文字 | 10 | 36 | 14 |
| 自己追記＋末尾文字 | 5 | 37 | 13 |
| 自己追記＋contains | 13 | 13 | 16 |
| 旧文字列から分岐 | 6 | 61 | 289 |
| 連結で構築後の位置アクセス | 6 | 6 | 6 |
| 数字の文字列追記 | 1 | 35 | 1 |
| 短い文字列の動的連結 | 14 | 13 | 14 |
| リスト内容の文字列化 | 4 | 4 | 4 |

Key observations:
- Global Rope is consistently competitive and usually fastest here.
- Flat copies every growing prefix; very poor at incremental building.
  It is competitive on repeated `contains` and on *already-flat* inputs.
- Global append-log supports sequential append but introduces a mutex/
  segmented lookup per join/read. More importantly, appending to two
  branches of the same immutable prefix forces a fork and copy; the
  branch workload takes 283 ms versus 6 ms for Rope.
- The transient JIT escape proof eliminates per-iteration Rope/Append-log
  snapshots on some pure `s <- join(s, suffix)` loops:
  the append and numeric-append workloads drop to roughly 1 ms. This
  **does not** help branching, arbitrary read-inside-loop or `contains`.
- The short dynamic join benchmark is deliberately below the cons-string
  threshold, so changing large-string storage is largely irrelevant.
- `random_read` explicitly builds its immutable input through real Scratch
  joins **before timing**. It measures repeated reads of a retained value,
  after a warmup that can materialize Rope/Append-log.
- `list_contents` is a control: it always uses the existing flat builder.
- All nine generated SB3 fixtures were independently executed with the
  vendored TurboWarp compiler and passed their `check=OK` parity assertions.
  TurboWarp timings were not used in the storage-mode rankings.

## Architectural candidates (not experimentally measured)

| Candidate | Why it may help | Why not to adopt blindly | Priority |
|---|---|---|---|
| Existing Rope + JIT transient builder | Persistent snapshots; O(1) composition; proven self-append optimization | Very deep paths / flatten-on-observation | **Keep as default** |
| 8-bit Latin-1/ASCII leaves + 16-bit UTF-16 leaves | Reduces leaf memory and bandwidth for mostly-ASCII text; cheap short numeric strings | Requires safe lazy UTF-16 materialization and GC/JIT ABI audit | **High: prototype** |
| Small-string immediate / interning | Avoids many GC objects for short generated strings | NaN-box payload/ABI and intern table GC/concurrency concerns | High: measure first |
| Balanced/chunked Rope | Depth-limited indexing and better random reads after repeated append | More balancing overhead on join; no Scratch substring/splice block | Medium: benchmark |
| Finger tree / RRB tree | Efficient end operations + persistence + random access | Scratch lacks common middle insertion/split operation; pointer overhead | Low |
| Charguéraud–Pottier transient sequences (2026) | Supports both immutable and mutable access with shared chunks | O(K log_K n) indexed access, complex, and no need for general split | Research-only |
| Global append-log | Fast pure single-owner append | O(length) fork fallback, synchronization, poor persistence | **Do not default** |

Research pointers:
- V8 string representations: <https://chromium.googlesource.com/v8/v8/+/refs/heads/main/docs/objects/strings.md>
- Boehm et al., *Ropes: An Alternative to Strings* (1995),
  <https://doi.org/10.1002/spe.4380251203>
- Hinze & Paterson, *Finger Trees* (2006),
  <https://doi.org/10.1017/S0956796805005769>
- Charguéraud & Pottier, *A Catenable, Splittable, Transient Sequence Data
  Structure* (ICFP 2026), <https://doi.org/10.1145/3828706>

## Reproduction

```sh
python3 tools/analyze_scratch_string_workloads.py \
  --json build-list-epoch/string-static-workloads.json
python3 tests/bench/algos/bench_scratch_string_storage.py \
  --reps 3 --multiplier 8 \
  --output build-list-epoch/string-storage-study-controlled
python3 tests/bench/algos/bench_scratch_string_storage.py \
  --reps 3 --multiplier 8 --transient \
  --output build-list-epoch/string-storage-study-transient
```

Each runner verifies its generated project's expected result before
accepting a timing, randomizes mode order with a fixed seed, and persists
raw per-process timings in its `results.json`.

## Next experiments

1. Add an optional instrumentation mode to log *executed* string op counts
   and join input/output length histograms for representative real projects;
   current static counts are not dynamic profiles.
2. Prototype one-byte immutable leaves with a fallback UTF-16 view (without
   changing `Value`'s 8-byte ABI), then compare GC bytes, branch/mixed-script
   parity and common short-string kernels.
3. Tune Rope flatten threshold and compare its node-count/depth/memory tradeoff,
   including a pathological read-from-front growing string case.
4. Profile real ScratchBench text workloads alongside these synthetic
   controlled SB3 microbenchmarks. Avoid treating a single synthetic benchmark
   as an overall performance winner.

