# 8–16 character string equality in LLVM SIMD (2026-11-06)

## Motivation

Comparisons in the xyo-cpp LLVM JIT use numeric SSA and, when necessary,
call a C++ runtime helper (`xyo_eq`, `xyo_lt`, `xyo_gt`). These are not
GPU↔CPU transfers; they are calls out of LLVM-compiled code into the native
Scratch runtime. The existing cache-backed string pair path handles short
ASCII/Unicode strings of up to 4 UTF-16 units. Expanding that scalar loop to
16 units was previously 15% slower on `math_strcompare`.

## Added implementation

`emitCachedNumericStringPair` in `src/jit/codegen.cpp` gained a distinct
**equality-only** vectorized path for direct variable-vs-variable Scratch
comparisons. It is generated only for two direct `data_variable` reporters,
not for static numeric values, dynamic list items, and constant-vs-variable
comparisons. Every runtime precondition is checked independently:

- Both operands are heap `SStr` values, with populated numeric caches.
  The existing numeric comparison path takes priority if both caches are
  valid numeric values (states 1/4), so `"03" == "3"` stays true.
- The lexical case requires both strings to be flat (not cons/append), both
  lengths to be exactly equal and in [8, 16], and both strings to have
  already-published `kAsciiKnown | kAsciiValue` classification bits.
  Classification is immutable and flags are read atomically (acquire).
- LLVM constructs two `<8 x i16>` vector loads from the string data, with
  alignment **1**, after the minimum length guard. Uppercase ASCII A-Z is
  lowered via lane-wise comparisons and a masked `+32`, then the two vectors
  are compared as `i128`. The remaining `len-8` characters are checked in a
  bounded scalar SSA loop with case folding. When `len==8`, the vector result
  returns directly. No out-of-bounds vector load can be executed.
- If any guard fails, the prior short-string fast path or native comparison
  helper runs. Non-ASCII values, Unicode folding expansions, different
  lengths and other Scratch types retain the old semantics. The original
  boxed values are never rewritten.

The only new runtime call is **none**. LLVM's regular vector lowering handles
SIMD; a custom pass is not needed. Disable this specialization at JIT/AOT
compilation time using `XYO_JIT_EQ_ASCII_SIMD=0`.

The AST condition is important: enabling the vector code for every potential
string comparison slows `app_brainfuck` by ~4.6%, even though most of its
comparisons are single punctuation literals. Restricting it to two direct
variable reporters preserved the speedup on longer string comparisons and
eliminated the observed regression.

## Local paired benchmark

Fixed compiled binary and five alternated switch ON/OFF pairs. ScratchBench
median rates in **operations/ms** (higher is faster); every checksum matched.
Raw results: `build/comparison-simd-20261106/dynamic-ab.json`.

| Case | OFF | ON | ON/OFF |
|---|---:|---:|---:|
| `math_strcompare` | 31,748 | 41,358 | **1.303×** |
| `app_life` | 13,773 | 13,730 | 0.997× |
| `app_brainfuck` | 134,382 | 134,704 | 1.002× |
| `algo_sort` | 356,925 | 356,605 | 0.999× |

In a separate `XYO_PROF=1` instrumented run, the total number of `xyo_eq`
calls was 19,424,000 without this optimization and 17,384,001 with it,
but the ON run completed more work within the approximately fixed runtime.
Normalizing by the ScratchBench operations completed gives approximately
**2.336 vs 1.163 calls per benchmark operation**, a **50.2% reduction**.
Do not infer a runtime speedup from the raw profiler-instrumented rates.

## Correctness and regression tests

Expanded `tests/differential/jit_numeric_string_pair_cmp.py` to include 8,
9, 11, 12, 15, 16 and 17 UTF-16-character comparisons with equal/mismatched
ASCII case, embedded NULs, same-prefix/different-length strings, mixed
numeric strings, and Unicode (`U+0130`). It compares the interpreter, Eager
JIT, HotSpot, O2, standard generic and all relevant optimization flags for
warp and non-warp. All 14 combinations passed (163 compared fields each).

AOT compilation with the specialization ON and OFF produced bit-identical
results for all **166** dumped variables, including the completion flag.
The existing `xyo-jit-string-pair-compare` CTest exercises the larger fixture.

The initial proof and LLVM IR specialization occur at compile time, but the
string type, ASCII and length checks occur on every execution: independent
scripts may change Scratch variable contents between invocations.

This change is local and has not been committed or pushed. Other dirty working
tree changes belong to separate work and were not reset.

Final selected CTest suite (`xyo-tests`, `xyo-jit-ascii-fastpaths`,
`xyo-jit-eq-numeric-cache`, `xyo-jit-string-pair-compare`): **4/4 PASSED**,
including the larger Unicode and vector boundary cases (158.70 seconds).
Log: `build/comparison-simd-20261106/ctest.log`.
