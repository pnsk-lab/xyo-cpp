# Scratch-specific Latin-1 leaf experiment (2026-11-07)

## Summary

**Keep `rope+utf16` as the default pending representative ScratchBench
measurement.** Optional one-byte leaves save GC memory and make some
Flat-join workloads faster, but Rope does not benefit significantly in the
controlled JIT workload set, and the first contiguous UTF-16 view becomes
more expensive. If all 100,000 compact leaves in the native benchmark
materialize their 48-code-unit UTF-16 views, they also allocate at least
100,000 * (48 + 1) * 2 = 9,800,000 bytes of external UTF-16 buffers,
before allocator overhead, which **can outweigh the original GC savings**.

```sh
XYO_STRING_STORAGE=rope XYO_STRING_LEAF=latin1 ./build/xyo run project.sb3
./build/xyo run project.sb3 --string-storage=flat --string-leaf=latin1
```

Two independent options are supported:
- `XYO_STRING_STORAGE=rope|flat|append-log` (existing policy)
- `XYO_STRING_LEAF=utf16|latin1` (new policy, default utf16)

Latin-1 leaves encode each **UTF-16 code unit <= U+00FF** in one byte.
Other leaves are ordinary UTF-16; composite nodes are unchanged.
`SStr::len` remains a UTF-16 code-unit count, and `unitAt()` reads
compact leaves without widening. `data()/view()` lazily materialize a
cached UTF-16 buffer using atomic publication; the GC frees it at sweep.

The JIT's direct inlined UTF-16 accesses now exclude Latin-1 leaves.
The numeric short-string cast fast path also checks the representation.
Native `sstrConcatStrings` and `sstrConcatStringAscii` copy compact
bytes directly. The ASCII comparison/search fast paths compare compact
bytes; longer ASCII needle searches use BMH to avoid quadratic scanning.

## Native allocation result

`tests/bench/string_leaf_native.cpp` allocates 100,000 independent
48-code-unit ASCII strings per mode, then reads a code unit and creates
the first contiguous view. Both configurations returned the same checksum.

| Metric | UTF-16 | Latin-1 | Difference |
|---|---:|---:|---:|
| GC payload bytes | 15,400,000 | 10,500,000 | -31.8% |
| GC accounted allocated bytes | 17,000,000 | 12,099,274 | -28.8% |
| Construction (2-run samples) | 8.5 ms | 5.6-5.7 ms | Faster |
| `unitAt` 100k calls | 0.9-1.0 ms | 0.7-1.1 ms | Similar |
| First `view` 100k calls | 0.9-1.0 ms | 3.9-4.0 ms | Slower |

The GC allocated bytes figure **does not include malloc-backed widened
buffers** created on first `view`, so it is not a measurement of ultimate
retained resident memory after observing every string. The benchmark does
not attribute GC collection time or process startup costs.

## End-to-end generated Scratch SB3 comparison

Same Release binary (`build-list-epoch/xyo`, LLVM JIT eager),
`XYO_JIT_TRANSIENT_STRINGS=0` to isolate storage policies,
one warmup + three in-project timed iterations per process, three
independent processes per mode; timing excludes project load/JIT compile.
Work multiplier 8. All projects checked their expected result. Samples
are milliseconds rounded to a millisecond by Scratch timing.

| Scratch workload | Rope UTF16 | Rope Latin1 | Flat UTF16 | Flat Latin1 | Append-log UTF16 | Append-log Latin1 |
|---|---:|---:|---:|---:|---:|---:|
| Self append | 3 | 3 | 34 | 23 | 10 | 10 |
| Append + first letter | 9 | 9 | 34 | 24 | 13 | 13 |
| Append + last letter | 4 | 5 | 33 | 24 | 13 | 13 |
| Append + contains | 12 | 12 | 12 | 11 | 15 | 16 |
| Fork/retain old string | 5 | 6 | 56 | 42 | 276 | 283 |
| Random read after join | 7 | 8 | 6 | 7 | 7 | 8 |
| Numeric suffix append | 3 | 3 | 33 | 23 | 10 | 10 |
| Short dynamic join | 16 | 16 | 17 | 15 | 17 | 15 |
| List contents | 4 | 5 | 4 | 5 | 4 | 5 |

Takeaways:
- Rope is consistently strong for immutable, shared concatenation.
  Latin-1 has little effect on the Rope workload timings here.
- The direct compact concatenation path makes Flat append 34->23ms
  and Flat branch 56->42ms. An earlier naive Latin-1 implementation
  was far worse (Flat append 131ms) because it widened every prefix.
- Flat contains is 12->11ms, approximately equal at this timer
  resolution. Compact ASCII search avoids expanding inputs.
- Append-log keeps its existing high fork cost (276->283ms): Latin-1
  leaves do not fix its copy-on-write behavior.
- `list_contents` builds an independent UTF-16 buffer and is unaffected.
- For read-after-join, the Rope/Flat difference is small; this specific
  generator warms up the immutable string before timing the read.
- These microbenchmarks **do not establish an overall ScratchBench winner**.
  A representative suite with actual runtime operation frequencies and
  allocation/GC metrics is still necessary.

## Reproduction

```sh
cmake --build build-list-epoch --target xyo-cli xyo-string-leaf-smoke \
  xyo-string-leaf-native-bench -j6
ctest --test-dir build-list-epoch -R 'xyo-string-(leaf|storage)-' \
  --output-on-failure
build-list-epoch/tests/xyo-string-leaf-native-bench utf16
build-list-epoch/tests/xyo-string-leaf-native-bench latin1
python3 tests/bench/algos/bench_scratch_string_storage.py \
  --leaf utf16 --reps 3 --multiplier 8 \
  --output build-list-epoch/string-leaf-study-final-utf16
python3 tests/bench/algos/bench_scratch_string_storage.py \
  --leaf latin1 --reps 3 --multiplier 8 \
  --output build-list-epoch/string-leaf-study-final-latin1
```

The `xyo-string-leaf-cli-parity` test exercises a generated SB3 across
both leaf policies, three join policies, interpreter/eager/hotspot modes
and CLI overrides. `xyo-string-leaf-smoke` checks exact GC payload
size, BMP fallback, surrogate indexing, numeric coercion, ASCII
comparison/contains, direct compact concatenation and concurrent
lazy publication.

## Next steps

1. Measure actual ScratchBench project memory retention/GC timings under
   Latin-1, including first-view materialization and unobserved leaves.
2. Replace eager `view()` conversion for additional common narrow-leaf
   consumers (notably numeric scans and case-insensitive equality).
3. Compare a 1-byte leaf with SSO/interned short strings, which may save
   more memory than payload compression given the `SStr` header cost.
4. Benchmark ASCII-heavy and CJK-heavy workloads separately; retain
   Unicode parity testing for multi-unit case mappings and surrogates.

This is an uncommitted local experiment; no commit/push or release was
performed.
