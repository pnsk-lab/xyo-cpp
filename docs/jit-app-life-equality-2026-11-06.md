# JIT numeric-string equality cache and ScratchBench `app_life`

## Root cause

`app_life` is a 12×12 Conway-Life simulation. In its hot neighborhood loop it
compares offsets to zero, neighbor sums to 2/3, and the current cell to 1.
Variable pointer resolution is **already** hoisted to native function entry,
and the existing JIT uses numeric shadows for promotable loop scalars.
Nevertheless, runtime profiling showed about 7.5M calls to `xyo_eq` per
~603ms execution (an instrumented run, therefore not a throughput baseline).

A temporary type-pair probe in `xyo_eq` found that **all 9.1M sampled calls
were heap-string/heap-string comparisons**, and both arguments had SStr
`numState=1`. Each string had already been parsed as a valid number; the
fallback wasn't doing new parsing, just dispatching through generic Scratch
comparison despite a valid numeric cache. The type-pair probe was removed
from the final source. The variation in call totals reflects differing work
completed during the benchmark's approximately fixed runtime window.

## Implementation

`src/jit/codegen.cpp`'s existing `emitNumericLiteralEq` has a promoted
numeric-shadow fast path, but the shadow's compare-valid bit may be false when
an immutable numeric string's parse cache has since become populated. The new
path handles this case without replacing the original boxed string:

1. When the cached numeric-shadow status is invalid and the current `Value`
   has a heap-string tag, load `SStr::numState` with acquire ordering.
2. For state 1 (parsed numeric) or 4 (Infinity), load `SStr::numCache` with
   monotonic ordering and compare to the compile-time numeric literal via
   ordered `fcmp`.
3. For all other states (including never parsed, whitespace, nonnumeric and
   lexically exceptional forms), call the existing `xyo_eq`. A string such as
   lowercase `"infinity"` must retain the normal case-insensitive lexical
   comparison against the literal `"Infinity"`.
4. The existing original `knownNumOk` guard still takes priority. This new
   cache probe runs only when that guard is false, avoiding extra work for
   already-proven numeric values.

This is a general numeric-literal comparison improvement, not a special case
for `app_life`, a particular block ID or the constants 0/1/2/3. It also works
with strings representing integers or floating-point values elsewhere. The
existing evaluation order is unchanged. Compiled fallback uses exactly the
same `xyo_eq` runtime helper.

Compile-time A/B switch: `XYO_JIT_EQ_STRING_CACHE=0` disables this optimization
without affecting other numeric-shadow paths.

## Results

Five alternating A/B measurements with the final **production binary**
(type profiler removed), switch on/off, checksums identical (ops/ms, higher
is faster). Raw results: `build/life-investigation-20261106/ab_final.json`.

| ScratchBench | Off | On | Change |
|---|---:|---:|---:|
| `app_life` | 13,463 | 14,020 | **+4.14%** |
| `app_ast` | 37,936 | 37,763 | -0.46% |
| `app_brainfuck` | 137,646 | 137,143 | -0.37% |
| `math_compare` | 263,234 | 261,275 | -0.74% |
| `loops_ifelse` | 394,534 | 396,623 | +0.53% |

The negative controls show no substantial regression; small differences are
within the limits of five repeated runs. An earlier A/B with temporary type
instrumentation compiled into `xyo_eq` had 13,417 → 14,008 (+4.4%). The final
no-profiler comparison above is the more representative measurement.

In a profiling run with the fast path enabled, the call count to `xyo_eq`
went to **zero**, while the checksum remained 53. That result is not itself a
performance speedup estimate: type-cache probes and branching also have cost.

### Rejected variants

- A speculative extra type-guard in the existing slow branch created another
  layer of CFG and caused LLVM 23's `SimplifyCFG` to segfault on this project.
  That experiment was withdrawn; no such nested branch remains. Its first
  non-crashing version was also approximately 4.4% *slower* in an A/B test.
- Caching a parsed string's number in the promoted variable's dslot/okslot
  did not produce a repeatable gain (14,152 vs 14,124 ops/ms in another five
  pairs). It adds mutation to an otherwise read-only check, so it was removed.

## Verification

`tests/differential/jit_eq_numeric_cache.py` generates a Scratch SB3 with
numeric and nonnumeric strings, numeric shadow literals, Infinity and its
lowercase spelling, negative zero, whitespace, empty strings, Unicode, values
updated during a loop, and both operand orders. Results are checked against
Scratch interpreter, Eager JIT, HotSpot JIT, O2 and disabled fast-path modes.
The `app_life` benchmark checksum supplies an additional real-project parity
check. CTest: `xyo-tests`, `xyo-jit-ascii-fastpaths`,
`xyo-jit-warp-induction`, `xyo-jit-foreach-ssa`,
`xyo-jit-repeat-copy-ssa`, `xyo-jit-eq-numeric-cache`: **6/6 passed**.
AOT generation and execution with the fast path both disabled/enabled also
succeeded: 13,930 vs 14,128 ops/ms in the two single runs, checksum `53` in
both. Those isolated AOT throughputs are not the main performance estimate.

The implementation maintains the existing dependency on `SStr`'s acquire
state-load/relaxed cache-payload protocol. It does not modify the string's
cached value or introduce new GC roots.
