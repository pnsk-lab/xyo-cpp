# LLVM SSA variable-copy loops (2026-11-05)

## Bottleneck

Variable handles are **already** resolved once per native procedure invocation by
`hoistVarSlotPointers()`, after `xyo_varslot_init`. No hash lookup is needed for
every Scratch variable read. However, ordinary `repeat` code still processes
`Value` tagged bits, dynamic coercion, numeric mirrors and yield reloads at
every iteration, even when a loop eventually carries only plain integers.

ScratchBench `vars_setchange` performs four serial updates per iteration:

```
a = c
b = a
c = b
c += 1
```

The optimized LLVM IR without this transformation contains repeated
number/type-conversion dispatch branches. The loop's values may initially be
numeric strings, so moving these conversions away unconditionally is unsafe.

## Implementation

A generic AST analysis (`repeatCopyPlan`) recognizes a straight-line `repeat`
body with 2–12 statements over at most 8 distinct scalars. Supported forms:

- `set x to y` where `y` is a direct variable reporter
- `change x by K` where `K` is a numeric literal integer with |K| ≤ 65,536

Assignments are interpreted in their original order, including repeated writes
to the same variable. Each involved scalar must be proven at runtime to be a
**plain IEEE double**, integral, non-negative-zero and within ±2^48. A numeric
string cannot be materialized as a number before a `set` statement: the guard
rejects it, preserving Scratch's exact type and original value representation.

When the loop has a fixed literal count recognized by the existing exact i32
repeat counter, its warp branch generates a conventional integer SSA loop:

- PHI carries the private iteration counter and all scalar values
- copy becomes an SSA value alias; increment becomes `add nsw i64`
- the updated values are converted back to doubles only at each batch boundary
- LLVM O2/O3 simplifies the SSA copy and induction recurrences; no custom
  ScalarEvolution pass is needed

Each batch is limited to the next original **64-iteration warp timeout check**.
The elapsed-clock check, scheduler yield, and variable spill/reload occur at
the same logical iteration. If another script changes the value during a yield,
the next batch rechecks its type and range; failure resumes the ordinary
Scratch execution from the current repeat counter. Unsupported bodies,
non-warp, out-of-range values, signed zero and nonnumeric values always execute
the original code. There is no benchmark name/size/variable-specific shortcut.

Disable with `XYO_JIT_REPEAT_COPY_SSA=0` (per compiled invocation).

## Measurements

Source: `build/variable-investigation-20261105/copy-ab.json`. Same final native
binary with specialization enabled and disabled, 4 alternating runs, medians,
all checksums identical. Units are **operations per millisecond**, with higher
being faster.

| ScratchBench | Disabled | Enabled | Ratio |
|---|---:|---:|---:|
| `vars_setchange` | 2,003,041 | 12,399,675 | **6.19×** |
| `vars_many` | 80,833 | 80,697 | 0.998× |
| `loops_repeat` | 19,037,515 | 19,059,390 | 1.001× |
| `loops_foreach` | 14,427,286 | 14,436,127 | 1.001× |
| `loops_ifelse` | 383,032 | 386,994 | 1.010× |
| `app_ast` | 37,174 | 36,855 | 0.991× |
| `app_brainfuck` | 133,830 | 134,202 | 1.003× |

The more difficult `vars_many` includes **eight modulo operations per logical
iteration** (`k_x = k_x % 1000003`), so its much lower rate is not evidence
that raw variable pointer lookup is slow. Optimized IR still contains integer
and floating modulo branches for each variable. Separating those operations
and proving stronger numeric/range facts is a better next optimization target.

## Validation

`tests/differential/jit_repeat_copy_ssa.py` builds original Scratch SB3s with
copy chains and interleaved operations. It compares interpreter, eager JIT,
hotspot JIT, disabled SSA and LLVM O2, for both warp and non-warp programs.
Coverage includes 0/1/2/3/63/64/65/127/128/129/10000 iterations, initial
numeric strings, invalid strings, fractional values, negative zero, infinity,
precision limits, large increments, and dynamic repeat counts. The actual
numeric-string and nonnumeric paths use the old implementation until all
values are safe. `XYO_JIT_DUMP2=1` shows fast/slow guarded dispatch but the
`rep.copy.header` SSA microloop is eliminated by LLVM O3.

AOT object generation and execution also preserve the same checksum. This
coverage does **not** explicitly force another script to mutate a copied
variable during the exact warp-timeout yield; that concurrency edge remains
worth testing separately.
