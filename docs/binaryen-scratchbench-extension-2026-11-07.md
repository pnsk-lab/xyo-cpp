# Binaryen + LLVM scratch-bench feature expansion (2026-11-07)

## Status

Implemented in this working tree, not committed. The original working tree
contains unrelated user changes. These files were not reset.

The LLVM backend and Binaryen-backed Emscripten runtime now share a broader
backend-neutral Scratch lowering subset:

- Numeric-only list query, numeric item access, append/replace/delete/insert,
  numeric search, and transactional clear. List-length and clear can handle
  previously mixed-type lists; numeric reads/searches check the monotonic
  numericOnly guard and deopt when semantics cannot be established.
- Rooted scalar string length (UTF-16), plus compile-time string length for
  constant text and numeric shadows. Read-only string slots are never
  overwritten by numeric writeback.
- Exact Scratch days-since-2000 through the runtime's wall-clock helper.
- Nonrecursive warp procedure inline expansion with numeric argument locals.
  Evaluate each caller argument exactly once before entering the body.
- Static or dynamic nested repeats inside nonrecursive warp procedures with
  a shared 1024-iteration fuel limit per WASM segment. Exhaustion discards
  staged scalar/list effects and safely replays the segment in the interpreter.
  General nested loops still retain interpreter scheduler semantics.
- Straight-line / conditional non-warp leaf procedures without loops or
  nested calls can be inlined. Recursive and yielding procedures stay interpreted.

The numeric segment's scalar/list changes are speculative and are committed
only after success; a guard or helper failure cannot expose partial writes.
General string joins/letters, arbitrary mixed-type list edits, recursive or
non-warp procedures, dynamic nested loops, renderer/sound and arbitrary
yielding statements are not compiled by the WASM HotSpot backend.

## Reproduction

From the repository root, with the native LLVM build in `build-list-epoch`
and the Emscripten HotSpot WASM build in `build-wasm-hotspot-parity`:

```sh
cmake --build build-list-epoch --target xyo-cli xyo-numeric-list-abi-smoke xyo-wasm-string-bridge-smoke -j4
source "$HOME/.emsdk/emsdk_env.sh"
cmake --build build-wasm-hotspot-parity --target xyo-cli -j4

python3 tests/differential/wasm_list_extensions.py build-wasm-hotspot-parity/xyo.js \
  --llvm build-list-epoch/xyo --addon build-wasm-hotspot-parity/binaryen-host.node \
  --out scratch-bench/results/binaryen-list-extensions-fixtures
python3 tests/differential/wasm_string_clock_extensions.py build-wasm-hotspot-parity/xyo.js \
  --llvm build-list-epoch/xyo --addon build-wasm-hotspot-parity/binaryen-host.node \
  --out scratch-bench/results/binaryen-string-clock-fixtures
python3 tests/differential/wasm_warp_procedure.py build-wasm-hotspot-parity/xyo.js \
  --llvm build-list-epoch/xyo --addon build-wasm-hotspot-parity/binaryen-host.node \
  --out scratch-bench/results/binaryen-warp-procedure-fixtures
python3 tests/differential/wasm_hotspot_integration.py build-wasm-hotspot-parity/xyo.js \
  --out build-wasm-hotspot-parity --llvm build-list-epoch/xyo
python3 tests/differential/wasm_scratchbench_backend_parity.py \
  build-wasm-hotspot-parity/xyo.js \
  --sb3 scratch-bench/.cache/build/parity.sb3 \
  --addon build-wasm-hotspot-parity/binaryen-host.node \
  --json scratch-bench/results/binaryen-expanded-parity-nested-20261107.json
```

## Measured results

One run per performance measurement, same scratch-bench fixture
(source hash e8bd5be3e7345088), scoring version 2:

| Runtime | Overall | Peak RSS | Notes |
|---|---:|---:|---|
| Native LLVM JIT, before WASM extension | 87,204 | 226.1 MB | hotspot |
| WASM HotSpot + Binaryen, before extension | 681.1 | 134.3 MB | many interpreter fallbacks |
| WASM HotSpot + Binaryen, after extension | 677.5 | 142.8 MB | no demonstrated speedup |
| WASM HotSpot + Binaryen, dynamic nested repeat and non-warp leaf support | 614.2 | 136.2 MB | one isolated run; no demonstrated speedup |

All scores above are single-run measurements in separate time windows,
not a controlled A/B comparison. The current result remains dominated by
the interpreter: the real-performance fixture's primary script uses
unsupported general procedures, string joins and nested control flow.
The observations do not establish an optimizer speedup or regression.

Results:
- Native LLVM (with graphical renderer): 2123/2123 parity passed
  (`scratch-bench/results/llvm-jit-parity-20261107`).
- Emscripten headless interpreter / builtin WASM / Binaryen:
  1899/2123 each, **all 2123 output lines identical**, 35 compiled scripts
  in each WASM mode, 0 in interpreter. Visual checks cannot be measured
  faithfully without a renderer. This comparison establishes zero
  Binaryen-specific parity differences, not full UI parity.
- New focused list, clock/string and warp-procedure tests compare live
  native LLVM, interpreter, builtin WASM and actual Binaryen optimization.
- 4 focused CTest ABI/smoke tests passed.

## Follow-up

The next performance milestone needs a full effectful, resumable shared IR,
including arbitrary procedure calls, string values, list value ABI and
scheduler/yield continuation. Until then, score comparisons primarily measure
interpreter execution rather than Binaryen-generated WASM.

## Additional verification (continued work)

- Dynamic warp repeat tested with 4 iterations (compiled directly) and
  1050 iterations (fuel exhaustion, late deopt).
- Transactional rollback after 1024 speculative list appends tested with a
  full 1050-item reference list; LLVM, interpreter, builtin WASM, and Binaryen
  all produced identical final output without duplicate items.
- Non-warp straight-line leaves compile successfully. Yielding non-warp
  procedures, general nested loops, and recursive procedures still fall back.
- Full headless parity remains 1899/2123 in interpreter, builtin WASM and
  Binaryen; all 2123 output lines match exactly, and the WASM paths compiled
  35 scripts.
- LLVM versus Binaryen scratch-bench performance run: 39/39 non-scheduler
  checksums match. Scheduler's 8 throughput counters naturally differ
  between engines and cannot be checked as deterministic output.

The next performance-enabling work requires resuming compilation after
unsupported procedure and scheduler boundaries. Extending individual
expression opcodes does not accelerate the benchmark root script, which
currently contains a non-yielding setup followed by unsupported suite calls.

## Whole-script fallback eliminated for safe top-level numeric regions

A new hybrid executor combines real Binaryen-compiled top-level regions
with original interpreter frames for unimplemented blocks. It resumes
compilation after interpreted loops, non-warp procedures and explicit
waits. A guard failure falls back at the current region boundary, never
replaying earlier committed code.

The WASM numeric compiler now supports exact tagged scalar copies,
true/false assignments, Scratch numeric conversion of variable Values,
and a direct-variable comparison guard to retain string comparison
semantics. All these cases share the same captured Scratch IR; no LLVM
code generator was duplicated.

Reproducible evidence from the active November 7 checkout:

- Hybrid focused fixtures: **4/4**, comparing native LLVM, interpreter,
  builtin WASM and the real Binaryen host optimizer.
- Existing WASM HotSpot integration: all cases passed against native
  LLVM, including restart/yield/list/dynamic-loop cases.
- Fuzz: **128/128** with native LLVM, WASM and interpreter parity, with
  the Binaryen emitter enabled.
- Headless scratch-bench parity: interpreter, builtin and Binaryen all
  reported **1899/2123** (missing graphical renderer); all **2123
  output lines** matched exactly. This does not assert that unrendered
  visual tests themselves pass.
- Isolated scratch-bench performance: **633.6** overall, startup **203.2ms**,
  run **32.065s**, peak RSS **133.3MB**, only one run. This did NOT achieve
  LLVM performance parity; prior binaryen measurement was **681.1**.
  The difference is not a controlled A/B measurement.

Logs: scratch-bench/results/binaryen-hybrid-2123-20261107.log,
binaryen-hybrid-fuzz128.log, hybrid-region-validation4.log,
binaryen-hybrid-integration7.log, and
binaryen-hybrid-performance-20261107.log.

The remaining blocker for *full native LLVM JIT equivalence* is not merely
opcode coverage: a common effectful, resumable ABI for arbitrary
procedures, interpreter-independent string and list Values, and all
scheduler/clone/pen/sound operations is still needed.

## Extended exact string operations and comparison (continued)

Binaryen/WASM gained a typed UTF-16 host bridge for string constants,
operator_join, operator_letter_of, operator_contains, composite
operator_length, case-insensitive mixed-type comparisons and Scratch
boolean truthiness. Host operations call the same C++ Scratch semantics
as Interpreter and native LLVM JIT. Numeric-to-numeric comparisons stay
on an in-WASM fast path; nonnumeric Values delegate to the runtime.
String results and scalar-to-scalar copies preserve original tagged
Values, including booleans, strings, -0 and canonical NaN.

GC-managed strings created by WASM helpers are not published into
Target variables until the *entire* speculative segment succeeds.
A scoped collector defer protects pending SStr values across nested
host calls. String literals are compiler-owned UTF-16 values and are
resolved/cached within an invocation, never embedded GC pointers.
A failing late string/list guard falls back without partial publication
or replaying previously committed segments.

Numeric list append/replace/insert with variable items now uses a
live Value-tag guard. For numeric items the original bits are staged;
strings/booleans trigger transactional interpreter fallback. This
also fixes the false rejection of numeric-variable list append in
the inlined warp procedure benchmark.

Regression evidence with the real Binaryen Node optimizer:

- 9/9 focused typed string/list fixtures passed in LLVM, Interpreter,
  builtin WASM and Binaryen, including a 1030-iteration string rollback
  and numeric-to-string list deoptimization.
- 8/8 warp procedure scenarios passed, including 1050-item rollback,
  non-warp procedure handoff and recursive interpreter fallback.
- All existing WASM HotSpot integration cases passed against LLVM.
- 128/128 seeded cases passed in Interpreter, WASM and LLVM.
- Full scratch-bench parity: **83 scripts compiled** (full + hybrid)
  for both builtin and Binaryen, compared with 34 from the initial
  hybrid version. Interpreter, builtin WASM, Binaryen each passed
  1899/2123 in the renderer-free Node environment, and all **2123
  test outputs match exactly**. The test explicitly verifies actual
  Binaryen native optimization calls, not just emitter labels.
- Native ABI tests 4/4 succeeded before the final list-guard patch.

Isolated scratch-bench performance after these changes:
**658.3** overall, **191.4 ms** startup, **32051 ms** runtime,
**138.3 MB** peak RSS (one run). Previous hybrid-only score
**633.6**. These are uncontrolled, one-run results and do not prove
a speed gain, nor approach the **87204** native LLVM JIT score.
Despite increased coverage, the scratch-bench performance project
still spends much of its run in the interpreter due to recursive
procedures, scheduler operations and other unsupported effects.

Logs and fixtures:
scratch-bench/results/binaryen-string-final-2123-20261107.log,
binaryen-string-final-fuzz128.log,
binaryen-string-listguard-fixtures2.log,
binaryen-string-warp-regression3.log,
binaryen-string-final-performance-20261107.log.


## Full tagged list transactions and procedure arguments

The numeric-only list journal was extended to stage complete tagged
Scratch Value objects, with exact read/append/replace/insert/delete
and mixed-item search. The helper now implements correct contains vs
indexOf comparison, UTF-16 list contents formatting, and special
last/all/numeric-string indices. Random/any indices explicitly bail to
the interpreter without consuming RNG state during speculative execution.
The monotonic numericOnly optimization flag remains conservative on
commit. A per-segment GC deferral protects staged Value pointers until
they become authoritative Target-owned list elements.

Warp and eligible synchronous non-warp procedure arguments now carry
raw tagged Values in i64 locals. Conversions happen at Scratch use
sites, rather than when entering procedures. Five additional fixtures
exercise string, boolean, nested procedure and numeric-string arguments.

Validation with native LLVM, Interpreter, builtin WASM and the real
Binaryen Node optimizer:

- **16/16** focused tagged-list/procedure fixtures passed, including
  last/all/numeric-string indices, Unicode listContents, 1050-iteration
  mixed-list rollback, and RNG-preserving fallback.
- Legacy numeric-list and string-regression fixtures passed.
- **8/8** warp-procedure tests passed, including late rollback.
- Full HotSpot integration suite passed versus LLVM.
- **128/128** deterministic differential fuzz cases passed.
- Headless parity: builtin and Binaryen compiled **94** scripts each
  (full or hybrid). All **2123 output checks** matched Interpreter.
  Each mode reports **1899/2123** official passes because 224
  graphics-related tests cannot be validated in this headless build.
- Final native ABI CTest **4/4** after moving ListGet to the tagged
  item-load helper ID 23. The ABI tests verify both direct host calls
  and transaction/rollback.
- LLVM vs Binaryen performance workload checksums **39/39** matched
  (excluding 8 nondeterministic scheduler throughput counters).
- Isolated scratch-bench **643.9** overall, **189.5 ms** startup,
  **32.127 s** runtime, **142.7 MB** maximum RSS (one run).
  Earlier string-only run was **658.3**. This is *not* evidence of
  higher performance or full LLVM backend parity.

Reproducible logs:
scratch-bench/results/binaryen-tagged-proc-parity.log,
scratch-bench/results/binaryen-tagged-proc-fuzz.log,
scratch-bench/results/binaryen-tagged-proc-native-ctest-fixed.log,
scratch-bench/results/binaryen-typed-procedure-fixtures.log,
scratch-bench/results/binaryen-typed-list-proc-performance-20261107.log.

Full LLVM-equivalent instruction coverage still needs a resumable
effectful procedure/scheduler IR and graphics/sound/extension host
operations; the current safe hybrid interpreter handoff remains
necessary for those features.
