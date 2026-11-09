# Nested WASM continuation (2026-11-07)

An opt-in, interpreter-owned continuation for Emscripten/Binaryen HotSpot.
The interpreter retains the real Thread stack, procedure frames, repeat
counters, warp timer and yield decisions. Nested WASM runs only atomic
subranges at the *current* statement; it never changes source Blocks.

Enable `XYO_NESTED_WASM=1` to try numeric-only hot subranges.
`XYO_NESTED_WASM_ALL=1` additionally permits mixed tagged Values,
string/list operations and conditionals as a differential-test mode.
`XYO_NESTED_WASM_STATS=1` reports compilation, execution and
warp-specific deoptimization counts. Neither mode is enabled by default.

## Execution rules

- A nonwarp repeat must yield at the original interpreter frame boundary.
  Each eligible numeric body fragment can execute in WASM, but *one*
  iteration is performed per scheduler pass.
- A **warp** repeat is eligible for whole-loop compilation when the entire
  body is pure scalar numeric arithmetic, assignments and increments.
  The emitter verifies the Scratch-rounded count is no greater than
  1024 **before** any speculative iteration. The sequencer also checks
  the live warp budget before jumping to WASM. Any trap or failed guard
  returns to the interpreter at the original control block without
  committing partial variable updates.
- Unsupported calls, nested loops, yield, wait, broadcast, extensions,
  clone, sound and pen instructions never silently disappear from the
  lossless ScratchCompilationUnit. No mid-reporter OSR is attempted.
- At most 48 modules are compiled per Sequencer; candidate sites are
  warmed up for 64 interpreter visits. Default mode requires an
  expression-heavy numeric scalar fragment; the unrestricted tagged
  mode is explicitly opt-in.
- Scalar-only fast execution copies raw Value bits to the WASM slot buffer
  and commits after success, avoiding mixed-list journals and GC deferral.
  Variable* bindings are cached *per Thread and compiled region*, and
  validated against local/stage VariableMap structural generation.
  Blocks::resetCache() advances a globally unique revision and invalidates
  compiled nested regions on source edits.

## Verification

Run the differential suite:

```sh
python3 tests/differential/wasm_nested_continuation.py \
  build-wasm-hotspot-parity/xyo.js \
  --addon build-wasm-hotspot-parity/binaryen-host.node \
  --llvm build-list-epoch/xyo \
  --out scratch-bench/results/nested-wasm-continuation-fixtures
```

Eight fixtures compare five modes: Interpreter, WebAssembly with nested
dispatch disabled, builtin WebAssembly, real Binaryen host optimization,
and LLVM JIT. Scenarios cover nonwarp repeat, if, long warp repeat,
mixed lists, concurrent variable writes, wait inside a repeat, a 320
iteration warp repeat compiled into a single WASM invocation, and a
1100 iteration warp repeat with a pre-execution transactional bailout.
The test checks the **warp_runs** / **warp_bails** counters so merely
falling back to the interpreter cannot produce a false success.

A focused timing (four alternating pairs, including Node startup)
for 210 calls to the 320-iteration warp procedure measured
**99.63ms nested disabled** and **79.87ms nested enabled**, 1.25x.
This is not a claim of overall scratch-bench improvement.
Earlier broad nested dispatch suffered tens of millions of tiny WASM
calls and >900MB RSS. Caps, hotness filtering and cached bindings
reduce overhead, but general recursive/async/effectful procedures are
still interpreted. Preserve opt-in default until representative
workloads demonstrate a broad improvement.

## Performance and warp-only experiment

The flag XYO_NESTED_WASM_WARP_ONLY=1 restricts dispatch to pure
numeric warp repeats, and skips nested checks inside non-warp interpreter
frames. This avoids one extra JIT attempt per ordinary interpreted block.

Renderer-free scratch-bench performance, one run per configuration:

| Configuration | Overall score | Peak RSS |
|---|---:|---:|
| Nested disabled | 666.1 | 151.6 MB |
| Experimental full mixed-type nested dispatch | 657.3 | 149.7 MB |
| Restricted numeric fragments + whole warp repeat | 665.8 | 133.9 MB |
| Warp-only with nonwarp dispatch bypass | 655.9 | 142.9 MB |

With restricted numeric fragments and whole-warp batching, loops_nested
improved from 3,303 to 127,935 ops/ms (38.7x) and math_nbody from
525 to 2,076 ops/ms (3.95x). All 39 deterministic performance checksums
match. Other workload classes lose performance, so the overall score is
unchanged within the noise of one-run tests; this is not a generally
profitable JIT policy and remains disabled by default.

## November 8: bounded conditional warp loops

Atomic nested warp compilation now also accepts `control_repeat_until`,
`control_while`, and `control_for_each` when the body and reporters
are verified scalar numeric, with no calls/IO/other observable effects.
The original Thread StackFrame remains authoritative. Nonwarp control
statements never receive whole-loop promotion, preserving per-tick yields.

The compiler keeps `Program.allowBoundedConditionalLoop=false` by default,
so standalone/whole-script uses reject those synchronous forms.
A nested warp control statement sets it true only after checking every
statement and expression. The emission is a structured WASM block/loop:
test at the header, leave on until-true or while-false, decrement the
1024-iteration fuel, run the numeric body, then jump to the header.
If a condition remains active after fuel is depleted, it returns a
transactional bailout with no published writes.

For-each uses a private induction counter independent of the named
Scratch variable: on each header it evaluates the current numeric
VALUE reporter, compares `index < limit`, increments the counter and
stages its new value in the named variable *before* running the body.
This handles fractional bounds, numeric-string bounds, runtime changes
to the bound, and scripts that mutate the named iteration variable
inside the body. The named iteration variable is marked writable for
commit and binding-cache validation. Infinite/forever loops remain
interpreted.

Tests at `tests/differential/wasm_nested_continuation.py` now include
22 comparison fixtures across Interpreter, native LLVM, builtin WASM,
actual host-Binaryen, and WebAssembly with nested acceleration disabled.
Cases cover while/until, zero-iteration and overflow guards, for-each
fractional/string/dynamic limits, mutated induction variable, and
nonwarp scheduling interleaved with another script. Counters confirm
actual full-loop WASM executions and overflow guard bailouts.

Process-level A/B timing, alternating 4 pairs (including Node startup):

| Focused fixture | Nested off median | Nested on median | Ratio |
|---|---:|---:|---:|
| 210 warp while calls | 95.51 ms | 73.72 ms | 1.30x |
| 210 warp until calls | 82.68 ms | 67.47 ms | 1.23x |

For-each (5 alternating pairs):
- Fixed bound: 76.58 ms vs 68.08 ms (1.13x).
- Dynamic bound: 78.92 ms vs 68.84 ms (1.15x).

The first full-suite measurement with warp-only opt-in was 585.6 overall,
below the earlier 666.1 disabled measurement. These were uncontrolled
single-run measurements with other tests concurrently building/running
and should not be used to claim either a robust regression or a general
speedup. Preserve the opt-in setting until repeatable broad wins exist.

## November 8: nested dispatch and negative-cache tuning

The runtime originally tested 10+ opcode strings on almost every
interpreted statement when nested WASM was enabled. The resulting
full-suite regression (e.g., 581.7 versus 652.5 without nested JIT)
was traced primarily to interpreter/JIT dispatch overhead, not poor
WASM arithmetic or Binaryen code generation.

`BlockCached` now computes the static nested-candidate and warp-loop
candidate flags once during its normal immutable opcode-cache build.
The interpreter checks those flags before entering any nested JIT
lookup. The nested source compiler also marks permanently unsupported
sites in a relaxed-atomic, revision-scoped negative cache after its
warmup attempt (or two guarded deopts). This prevents repeated module
lookup/candidate analysis for permanently unsupported effects, while
`Blocks::resetCache()` still invalidates the negative result when the
project is edited.

Sequential, non-overlapping, one-run scratch-bench performance A/B:

| Version | Nested OFF | Warp-only ON |
|---|---:|---:|
| Before cached opcode candidates | 652.5 | 581.7 |
| After precomputed opcode candidates | 668.6 | 655.1 |
| After negative-cache elimination | 681.5 | 682.1 |

Last pair peak RSS: 150.1 MB disabled vs 145.5 MB warp-only enabled.
The overall 0.1% difference is within likely benchmark noise,
**not** evidence of a reliable overall acceleration. The loops_nested
workload rose from 3,303.73 to 130,320.51 ops/ms (39.4x), but app_life
fell from 71.77 to 61.81 ops/ms (0.86x). All 39 deterministic
performance checksums matched. This is a measured *tradeoff*, so
warp-only nested JIT remains opt-in, not a new default.

The code retains fast negative results only within a specific Blocks
revision and original cached interpreter instruction. Replacing or
editing a project invalidates both positive compiled regions and their
negative decisions; a clone's runtime type deopt may conservatively
disable promotion for other clones sharing source Blocks without
changing Scratch semantics.

## November 8: nested warp CFG and helper calls

The previous warp-only verifier accepted one loop root and no nested
control flow. It now verifies the entire lowered numeric statement tree:

- Up to eight statement nesting levels, 256 statements and 2048
  numeric/boolean expression nodes.
- Warp repeat, repeat-until, while and for-each may contain other
  bounded loops and if/if-else branches.
- A single **shared** 1024-iteration fuel counts all loop levels.
  Running out of fuel rejects the complete speculative transaction;
  inner loops can then run using the original interpreter's StackFrames.
- Zero-argument **warp** helper procedures with no recursive cycle and
  pure scalar-numeric bodies can be inlined into the bounded tree.
  The compiler's procedure call graph rejects recursion and the verifier
  rejects nonwarp callees, procedure arguments, strings, lists, waiting
  and observable effects. The warp deadline is checked at entry and
  before transactional commit.
- Nonwarp if/if-else fragments may now use the journal-free numeric
  slot ABI. The surrounding nonwarp loop still yields at the original
  interpreter checkpoint and reloads scalar state on every invocation.

The new CFG verifier rejects orphan, cyclic or aliased statement IDs.
Separately maintained `tree_compiled`, `tree_runs` and `tree_bails`
counters distinguish an actual whole-nested-tree invocation from an
inner loop compiled individually after the outer tree deoptimizes.

`tests/differential/wasm_nested_continuation.py` now covers 33
cross-backend output parity cases, including nested loops, nested
conditionals, aggregate fuel overflow, list effects, direct helper
inlining, nested helper loops, recursive and nonwarp callee rejection,
numeric if/if-else and interleaved thread writes. It requires real
optimized Binaryen module execution for new supported paths.

Arbitrary recursive procedures, nonwarp procedure calls with scheduling
effects, general data/control effects and unbounded loops remain
interpreted. These additions remain **off by default**, gated by
`XYO_NESTED_WASM=1` and optionally
`XYO_NESTED_WASM_WARP_ONLY=1`.

### Focused process-level performance (new numeric control-tree support)

Five alternating A/B pairs, including Node process startup, actual
host-Binaryen optimization and full scratch variable dumps:

| Script | Nested JIT off | Nested JIT on | Ratio |
|---|---:|---:|---:|
| Two-level numeric repeat | 101.431 ms | 76.905 ms | 1.319x |
| Repeat containing a conditional loop | 98.980 ms | 79.020 ms | 1.253x |
| Repeat with no-arg warp helper | 112.228 ms | 90.607 ms | 1.239x |
| No-arg warp helper containing repeat | 87.534 ms | 74.226 ms | 1.179x |
| Nonwarp if-else with interleaved writer | 73.664 ms | 76.255 ms | 0.966x |

Source timings and individual samples:
`scratch-bench/results/nested-warp-tree-timing-20261108.json`.
The four warp cases use `XYO_NESTED_WASM_WARP_ONLY=1`;
the nonwarp conditional case uses the mixed/numeric fragment mode.
The numbers demonstrate a narrow but repeatable per-scenario win for
bounded, pure warp work, **not** a blanket speedup. Nonwarp fragments
are still dispatched once per original yield checkpoint.

### Additional randomized differential testing

The independent randomized harness at
tests/differential/wasm_nested_tree_fuzz.py generates 48 seeded Scratch
projects. Each contains 2–4 nested bounded repeat levels, numeric
branches, optional zero-argument warp helper calls, and interleaved
numeric writes. Every seed compares the Interpreter, builtin WebAssembly,
real host-Binaryen, and native LLVM JIT. It additionally requires
tree_compiled >= 1, tree_runs >= 1 and tree_bails == 0 in both WebAssembly
backends so that silent interpreter fallback cannot count as a pass.
All 48/48 seeds passed on 2026-11-08.

Reproduction:

    python3 tests/differential/wasm_nested_tree_fuzz.py \
        build-wasm-hotspot-parity/xyo.js \
        --addon build-wasm-hotspot-parity/binaryen-host.node \
        --llvm build-list-epoch/xyo \
        --out scratch-bench/results/nested-tree-fuzz-fixtures \
        --seeds 48

### Whole-suite guarded performance (2026-11-08)

An additional sequential, single-run headless scratch-bench A/B after
the tree extension measured 634.0 overall with nested JIT disabled and
632.9 with nested warp-only enabled (peak RSS 135.5 vs 138.5 MB).
The loops_nested test increased from 3,162.3 to 122,467.5 ops/ms
(38.7x). All 39 deterministic checksum values match.

The full-suite overall delta is -0.2%, too small and variable to
establish a reliable speedup or regression. Individual applications
still lose throughput. The optimization remains experimental/opt-in.

## November 8: bounded warp tail recursion with numeric arguments

The nested WASM compiler can recognize a canonical tail-recursive
Scratch warp procedure and convert it to a guarded WebAssembly do/while:

1. The procedure is warp-enabled and has at most four arguments.
2. The initial argument expressions are numeric constants, and every
   recursive argument update has a provably numeric result.
3. The final procedure block is an if whose only substack block is a
   self-recursive call; there is no instruction after the recursive
   call. Prefix instructions, including small nested numeric loops,
   may perform only verified numeric scalar work.
4. A recursive entry consumes one unit of the shared 1024-step budget
   also used by the enclosing warp control tree. Exhausting the budget
   discards the entire speculative scalar transaction. The original
   interpreter then starts at the same warp root with unchanged values.
5. Next-frame arguments are eagerly evaluated into separate WASM i64
   locals before *any* argument local is changed. This preserves
   simultaneous binding when updates depend on each other, e.g.
   factorial or Fibonacci-style argument shuffles. All argument
   values are numeric, with no GC-managed string lifetime issues.

Non-tail recursion, nonwarp recursive calls, initial string or variable
argument expressions, recursively generated booleans, and more than
four parameters are deliberately excluded. The general Scratch call
stack is still authoritative in these cases. This is NOT arbitrary
recursive procedure compilation and does not replace general
resumable/fiber-based call frames.

The generated path reports tail_compiled, tail_runs and tail_bails
in XYO_NESTED_WASM_STATS=1. These counters separately prove actual
Binaryen compiled tail recursion and budget-triggered rollback.

Tests:
- 47/47 deterministic 5-way continuation cases in
  tests/differential/wasm_nested_continuation.py, including one-,
  two- and four-argument recursion, eager simultaneous updates,
  nested loops, late overflow, and all major rejection paths.
- 32/32 randomized tail-recursion programs (0–4 arguments,
  Interpreter/disabled/builtin/Binaryen/LLVM) using
  tests/differential/wasm_tail_recursion_fuzz.py. Each seed requires
  actual builtin and Binaryen tail-recursion module execution.
- Existing 128-seed compatibility fuzz suite passed.
- Headless ScratchBench: all 2123 expected outputs matched, with
  1899/2123 runnable passes for Interpreter/builtin/Binaryen and 94
  compiled scripts for each WASM backend.

Reproduce:

    python3 tests/differential/wasm_tail_recursion_fuzz.py \
        build-wasm-hotspot-parity/xyo.js \
        --addon build-wasm-hotspot-parity/binaryen-host.node \
        --llvm build-list-epoch/xyo \
        --out scratch-bench/results/nested-tail-fuzz --seeds 32

Focused five-alternating-pair process-level A/B, inclusive of Node
startup, for the small deterministic recursion fixtures:

| Case | Disabled median | Enabled median | Speedup |
|---|---:|---:|---:|
| Zero-argument tail recursion | 61.974 ms | 61.544 ms | 1.007x |
| Recursive prefix with nested loop | 63.373 ms | 70.063 ms | 0.905x |
| One numeric argument | 63.910 ms | 65.151 ms | 0.981x |
| Two arguments, eager swap | 65.519 ms | 65.379 ms | 1.002x |
| Four numeric arguments | 63.169 ms | 72.040 ms | 0.877x |

Raw process timings are in
scratch-bench/results/binaryen-tail-recursion-ab-20261108.json.
These very small recursion workloads are dominated by process/startup
and JIT compilation overhead. This experiment establishes compatibility
and codegen coverage, not an overall performance improvement. Leave
XYO_NESTED_WASM disabled by default.

## November 8: bounded non-tail warp recursion with compact LIFO continuations

The WASM nested interpreter now recognizes one canonical non-tail warp
self-recursion with zero parameters: numeric prefix, a final recursive
IF call, and a nonempty numeric suffix. It lowers this to the
WarpReturnStack IR statement. Its body is the entry prefix, its elseBody
is the deferred suffix, and expr holds the recursive predicate.

The emitter uses two WASM structured loops. The entry loop pushes one
continuation for every entered frame and executes its prefix; the unwind
loop pops frames and executes their suffix in reverse recursive return
order. Because this restricted source form has just one possible return
program counter, a single i32 depth local is an exact compressed LIFO
continuation stack, not a general array of return PCs or argument frames.
General multiple-return-site recursion is still unsupported.

The entry loop charges the same 1024-iteration shared warp fuel as
nested repeats, while and for-each. Exhaustion returns a transactional
bailout before publishing scalar writes; the original interpreter then
re-executes the correct Scratch call frames. Entry/commit warp deadlines
and Block source revision invalidation remain enforced. The verifier
rejects strings, lists, waits, broadcasts, nonwarp calls, effects,
arguments to recursive calls, multiple return PCs, or nonnumeric work.

New diagnostics: return_compiled, return_runs and return_bails, separate
from the existing tail and nested-tree counters. Representative
non-tail fixture: 147 compiled runs, zero bailouts. With aggregated
fuel overflow: a compiled module is tried, then two guarded bailouts
leave execution to the interpreter.

Validation (2026-11-08):
- tests/differential/wasm_nested_continuation.py exercises ordinary,
  nested-prefix/suffix, overflow, side-effect and malformed non-tail
  recursion. The five-mode comparisons use Interpreter, nested-off,
  builtin WASM, optimized host Binaryen, and native LLVM JIT.
- tests/differential/wasm_return_stack_fuzz.py generated 32 random
  numeric non-tail recursion cases (1 to 36 nested frames) including
  numeric if/if-else, nested repeat prefixes and suffixes. All 32
  matched across five modes. Both WASM backends reported actual
  non-tail return-stack compilation and execution.
- Existing 48-seed bounded nested-warp-tree test and 128-seed
  compatibility fuzz still pass.
- The currently cached scratch-bench parity project has 2,125 tests.
  All 2,125 output lines agree across Interpreter, builtin and Binaryen;
  each records 1,883 headless passes and the WASM modes each compile
  94 scripts. Graphics-dependent failures are outside this claim.

Use XYO_NESTED_WASM=1 and XYO_NESTED_WASM_WARP_ONLY=1 to enable the
experiment. The default remains disabled pending repeatable global
performance wins.

The next missing feature is a genuine stack of return PC plus numeric
argument-frame values, followed by yield-aware nonwarp continuations.

### Post-change reproducibility and performance

The complete deterministic differential fixture suite now contains
52 five-way cases. The new non-tail continuation modes and all earlier
warp, list and type-guard modes pass. The dedicated randomized
32-case non-tail suite and the pre-existing 48-case nested tree suite
pass, and the older 128-case interpreter/WASM/LLVM fuzz suite passes.
Native CTest selected five unit/ABI smoke suites (including
xyo-tests), all passing. The WASM build also completes.

Single-run sequential scratch-bench performance A/B, using the
current headless performance project and the actual Binaryen host:

| Mode | Overall score | loops_nested ops/ms | Peak RSS |
|---|---:|---:|---:|
| Nested JIT disabled | 679.9 | 3,430.79 | 142.1 MB |
| Numeric warp-only nested JIT | 690.7 | 128,647.28 | 147.9 MB |

All 39 deterministic performance checksums match. The 1.6% aggregate
score difference comes from one run of each configuration, and is
too noisy to claim a reliable overall improvement. Some individual
tasks still regress with nested JIT enabled.

Seven alternating, process-level A/B pairs for short non-tail cases:
- Ordinary non-tail recursion: off 62.23 ms, on 67.38 ms (0.92x).
- Nested prefix/suffix: off 66.63 ms, on 70.92 ms (0.94x).
- Source file: scratch-bench/results/nested-return-stack-ab-20261108.json.

A hotter and more representative numerical case (1,200 warp-helper
invocations of a depth-32 numeric non-tail procedure, with prefix and
return-suffix work) shows a repeatable benefit in five alternating
A/B pairs: off 107.942 ms, on 85.503 ms, for 1.262x speedup.
The generated compiled path completed 1,137 invocations and the
final Scratch variable outputs matched exactly. Data and .sb3 are in
scratch-bench/results/nested-return-stack-heavy-ab-20261108.json and
scratch-bench/results/nested-return-stack-heavy-20261108.sb3.

This is a restricted, opt-in optimization. Startup and WebAssembly
module compilation overhead dominate very small problems; longer
numeric recursions can amortize the fixed cost.
