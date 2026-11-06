# LLVM JIT: runtime-free hot string comparisons (2026-11-06)

## Motivation

`xyo_eq`, `xyo_lt` and `xyo_gt` are native C++ helpers, **not GPU-to-CPU
transfers**. However, calls from compiled LLVM JIT code to the general
Scratch runtime limit inlining/loop optimization and may incur repeated
dynamic string coercion. Move comparisons with locally provable semantics into
LLVM IR while retaining the original helper for every ambiguous case.

## Implementation (`src/jit/codegen.cpp`)

`emitCachedNumericStringPair(lhs, rhs, fallback, predicate)` is a shared
backend for `operator_equals`, `operator_lt`, and `operator_gt`.

1. The JIT checks the current NaN-boxed `Value` tags. When both are heap
   strings, it reads the immutable `SStr` numeric parsing states using
   `acquire` atomic loads. When both states are 1 (numeric) or 4 (Infinity
   literal), it loads the cached doubles (`monotonic`) and uses LLVM `fcmp`.
   This handles different textual spellings of the same numeric value.
2. When both cache states are populated but **not both numeric**, and both
   strings are flat with lengths at most **four UTF-16 units**, a bounded SSA
   loop checks every character for ASCII and computes case-insensitive
   lexicographic ordering. The loop compares folded code units, retaining the
   first nonzero difference and comparing lengths only for equal prefixes.
   `eq`, `lt`, and `gt` use the same result. LLVM's regular O2/O3 pipeline
   optimizes the IR; no new custom LLVM pass is required.
3. If either cache is uninitialized, either string has a cons/append body,
   either string has non-ASCII content or is longer than four code units, or
   either operand is not a heap string, call the original `xyo_eq`, `xyo_lt`
   or `xyo_gt` helper. The ordinary numeric-shadow comparison and same-bits
   shortcuts still run before this fallback.
4. Compile-time-proven numeric operands (`knownNum` and native numeric
   literals) skip generating this complex string-string fallback: it is
   impossible for such an operand to have the SStr pointer tag. This avoids
   unnecessary IR/code-size growth in numeric algorithms.

The ASCII scan clamps every out-of-range UTF-16 load to index zero (a flat
SStr has a guaranteed terminating code unit), masks absent values, scans the
*entire* short pair before declaring it ASCII, and compares original lengths
to handle embedded NULs. Unicode lowercase may expand characters (e.g.
`U+0130`); such cases always defer to the original Scratch runtime. The
cached numeric strings are compared before the ASCII path, so `"03" == "3"`
uses numeric semantics rather than literal string comparison. Lexical-zero
whitespace and lowercase `"infinity"` use the original string semantics.

A/B switches (checked at JIT compilation only):

- `XYO_JIT_STRING_PAIR_NUMERIC=0` disables both new string-pair paths;
- `XYO_JIT_STRING_PAIR_ASCII=0` disables only the ASCII lexical scan.

Neither is tied to a project, ScratchBench name, particular variable or
fixed operand literal. The bound of four characters was chosen by A/B
measurements: a 16-character loop increased work for longer strings.

## Measurements

Single fixed binary, five alternating measurements per case, medians in
operations/ms (higher is faster); all checksums matched. Raw data:
`build/comparison-pair-20261106/knownnum-paired.json`.

| ScratchBench | Disabled | Enabled | Ratio |
|---|---:|---:|---:|
| `math_strcompare` | 29,623 | 33,556 | **1.133×** |
| `math_compare` | 265,878 | 266,252 | 1.001× |
| `app_life` | 14,156 | 14,181 | 1.002× |
| `app_brainfuck` | 138,859 | 138,246 | 0.996× |
| `app_ast` | 37,873 | 37,210 | 0.982× |
| `algo_sort` | 372,373 | 373,487 | 1.003× |

The standalone generated test has 40 dynamic string/type pairs, each with
130 repeated `=`, `<` and `>` checks. With `XYO_PROF=1`, the combined
`xyo_eq`/`xyo_lt`/`xyo_gt` call count fell from **8,060 to 4,030 (−50%)**;
this is a *call count*, not a runtime improvement estimate. Cases not proven
safe remain on the helper, including cold parse caches and Unicode text.

A 16-character ASCII extension reduced helper calls further but slowed
`math_strcompare` to ~0.85×; a four-character version restored a speedup.
Generating this path even when either operand was statically numeric caused
~0.95× `algo_sort`; pruning those statically impossible cases restored parity.
We rejected those broader specializations rather than benchmark-maximizing.

## Correctness

`tests/differential/jit_numeric_string_pair_cmp.py` generates real Scratch
SB3s with numeric, leading-zero, radix, exponent, Infinity, NaN, empty,
whitespace, mixed-case ASCII, UTF-16 NUL, Unicode-folding expansion, CJK,
emoji, and mixed numeric/string pairs. It compares the Scratch interpreter,
Eager JIT, HotSpot, O2, the disabled paths and generic equality, for warp
and non-warp programs. AOT code generation and execution are also compared
with the same variables. The existing `xyo-jit-eq-numeric-cache`, ASCII
fastpaths, and plain-list comparison tests cover adjacent behavior.

The remaining helper calls are deliberately retained for cases requiring
Unicode-aware coercion/ordering, not accidental JIT exits. Profiling and
concurrent GC or type writes may affect performance; no blanket speedup is
claimed outside the tested cases.

## Follow-up: short Unicode with proved case folding (2026-11-06)

A direct 5–32 code-unit ASCII extension was tested first. Even after checking
`SStr`'s cached ASCII flags, it slowed `math_strcompare` from 32,736 to
24,768 ops/ms (**0.757×**), with no checksum changes. That branch was
**removed** rather than widening an expensive SSA scan unconditionally.
Raw data: `build/comparison-followup-20261106/paired.json`.

Instead, for the already-fast **≤4 UTF-16 code-unit** dynamic string pair,
the LLVM scan now handles Unicode as well when *both* immutable `SStr` values
have already published a sound case-folding proof:

- `kLowerStableKnown | kLowerStableValue`: Unicode lowercasing is a no-op;
- `kAsciiFoldKnown | kAsciiFoldValue`: Unicode lowercasing changes only ASCII
  A–Z, so the same per-character ASCII fold used by the existing LLVM loop
  gives the exact Scratch lowercase order.

The two proofs can be mixed across the operands, since a stable string is
also ASCII-fold compatible. An acquire load of `SStr::flags` reads each proof
only when the string actually contains non-ASCII units. The numeric-string
path and ordinary ASCII path are unchanged. Unproven Unicode values, longer
strings, cons/append strings, one-to-many case folding (e.g. `İ`), and cold
flags continue using the host helper. The new check is a guard on existing
**general properties of immutable strings**, not a table of benchmark words.
The optional compile-time switch is
`XYO_JIT_STRING_PAIR_UNICODE_STABLE=0`.

On the 40-pair dynamic-comparison generated SB3, the total
`xyo_eq`/`xyo_lt`/`xyo_gt` calls fell **4,030 → 2,604**, 35.4% fewer than
the prior optimized path (and 67.7% fewer than the all-disabled path at
8,060). Checksums match the interpreter under Eager/HotSpot/O2, both warp and
non-warp. The test covers case-changing Unicode (`K`, `İ`), stable Japanese,
emoji, ASCII-only spelling changes, embedded NUL, numeric/non-numeric cases
and both sides of all three comparisons.

Against the fixed pre-follow-up native binary, five paired ScratchBench
measurements of the Unicode-enhanced binary gave:

| ScratchBench | Before ops/ms | After ops/ms | Ratio |
|---|---:|---:|---:|
| `math_strcompare` | 32,589 | 32,623 | 1.001× |
| `math_compare` | 259,577 | 260,000 | 1.002× |
| `algo_sort` | 368,382 | 368,303 | 1.000× |
| `app_life` | 14,021 | 13,879 | 0.990× |
| `app_ast` | 37,122 | 37,671 | 1.015× |
| `app_brainfuck` | 135,470 | 138,855 | 1.025× |

Raw data: `build/comparison-followup-20261106/unicode-paired.json`.
The improvement is **fewer host helper calls for known Unicode**, not an
established broad runtime speedup. AOT was tested with this extension on and
off; all 124 dumped variables were identical. The original comparison
fallback is preserved, and no new native helper was added.

Follow-up final verification: **5/5 CTest passed** (`xyo-tests`,
`xyo-string-unicode-fast`, `xyo-jit-ascii-fastpaths`,
`xyo-jit-eq-numeric-cache`, `xyo-jit-string-pair-compare`).
Logs: `build/comparison-followup-20261106/ctest.log`,
`unicode-parity-v2.log`, and `aot-on.json`/`aot-off.json`.
