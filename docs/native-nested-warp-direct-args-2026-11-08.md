# Native read-only warp OSR argument bridge — 2026-11-08

## Scope

This is an **additional experimental optimization**, not general deep OSR.
The existing selector and scheduler must already prove a single, directly
invoked warp procedure, a safe counted `repeat`, exact declared argument
names, a live interpreter call frame, valid lexical source path and remaining
loop counter. The interpreter executes the procedure prefix and suffix; LLVM
executes only the remainder of the selected loop. No committed Scratch effect
or call is replayed.

Enable all of:

```sh
XYO_JIT_MODE=hotspot \
XYO_JIT_ASYNC=1 \
XYO_JIT_OSR=1 \
XYO_JIT_OSR_NESTED=1 \
XYO_JIT_OSR_NESTED_ARG_READS=1 \
XYO_JIT_OSR_NESTED_DIRECT_ARGS=1 \
XYO_JIT_OSR_TIMING=1 \
  ./build/xyo play project.sb3
```

The new `DIRECT_ARGS` flag is **OFF by default**. If it is not set,
already-supported native-loop argument reporters use `xyo_rt_reporter`.

## ABI and safety boundary

- `src/jit/codegen.cpp` emits `xyo_rt_nested_osr_arg_get(ctx, reporterId,
  utf16Name, length)` only from a `standaloneOsrLoop` entry with explicit
  `DIRECT_ARGS=1`. The UTF-16 length is explicit to preserve embedded NULs.
- `src/jit/rt.cpp` retrieves the *live* `Thread::stackFrames[0].params`
  value, requiring `nestedOsrActive`, exactly two source frames, and the
  original interpreter procedure frame. The native continuation does not
  own an unrooted copy of the argument and does not alter `JitCtx` layout.
  That existing interpreter frame remains a GC root.
- A missing or `undefined` argument takes the old reporter execution path,
  preserving the interpreter's numeric/boolean/special-name semantics.
- The helper is declared yield-capable since its fallback may yield. It is
  main-thread-affine like the normal reporter helper.
- A direct reporter read contributes **one** locally buffered block count;
  a fallback read is counted by the interpreter. There is no double-count.
- The source selector continues to reject unknown argument reporters,
  malformed argument names, unsupported effects and non-proven continuation
  shapes. Nested native loops are permitted only within the prior whitelist.

These checks do **not** implement arbitrary SSA locals, nested procedure
call frames, deep recursion, conditionals with unknown committed effects,
or independently restorable native continuation PCs. Those remain deferred
to IR-001 / RT-001.

## Verification

Coherent build of the updated C++ sources via
`tools/consistent-build.ts`, frozen binary
`board/osr-direct-verified-v2-xyo`:

- Source SHA-256: `81af7cb35ea74c7218c387452a22a6ea04e760ba5a82992e31e85a65ffe2c150`
- Binary SHA-256: `f8310dfdadad47ee37811f1e5e100335ed0d03d7899ad1b93dc2aef2c1505e95`

`tests/differential/jit_nested_warp_direct_args.py` (registered as
`xyo-nested-warp-direct-arguments-osr`, CTest #54):

- Numerical argument in a warp repeat: existing reporter and new direct
  bridge both *actually promoted* and restored the caller.
- Nested `repeat` with a Unicode / astral-plane string argument, and
  non-ASCII declared parameter names (`表示🌠`, `増分`): both
  paths promoted and preserved exact final Scratch state.
- Undeclared reporter name inside the loop: both paths safely refused OSR
  and matched the interpreter.
- Exact block counts for the 8-million-iteration numeric program:
  interpreter = existing reporter path = direct bridge = **24,000,015**.
- Forced GC with `operator_join` inside the native loop, argument string
  read via direct bridge: **368 GC collections**, real promotion and
  restored caller, final state identical to the interpreter.

Existing `jit_nested_warp_args_preserved.py`: 6/6 PASS. Existing
`jit_nested_warp_args_refusal.py`: 3/3 PASS.
`gui_scratch_parity_regression.py`: interpreter, HotSpot, Eager each
**2,123 / 2,123 PASS**. `git diff --check` PASS.

The CTest configuration recognizes the new test, but the coherent frozen
CLI was exercised directly instead of using a potentially mixed/stale
binary in the shared CTest build directory.

## Remaining

Benchmark per-native-loop instruction rates against the interpreter
reporter trampoline under comparable OSR promotion points; the raw speed
gain has not been quantified. A complete native argument-array / SSA
continuation ABI with effect positions and source revision validation is
not yet available. No commit or push.

## Continued: hoisted native SSA arguments (same day)

New opt-in: `XYO_JIT_OSR_NESTED_HOIST_ARGS=1`, additionally requiring
`XYO_JIT_OSR_NESTED_DIRECT_ARGS=1`. The existing
`XYO_JIT_OSR_NESTED=1` and `XYO_JIT_OSR_NESTED_ARG_READS=1` prerequisites
are unchanged. All feature flags remain opt-in; default behavior is
unchanged.

`src/jit/codegen.cpp` now emits exactly one
`xyo_rt_nested_osr_arg_peek(ctx, utf16Name, length)` call into the
standalone LLVM native-entry basic block **per distinct referenced
procedure argument name**. The 64-bit Scratch `Value` is held as an
LLVM SSA value (not a new GC root). The original `Thread`'s interpreter
call `StackFrame::params` map remains live and GC-rooted across native
fiber yields. `src/jit/rt.cpp` exposes this read-only entry helper;
`src/jit/backend_llvm.cpp` registers the ORC symbol. No
`Thread` or `JitCtx` layout changes were made.

An absent or `undefined` entry produces an `undefined` sentinel:
the actual source reporter still calls `xyo_rt_reporter` at **each
actual read**, preserving timing-sensitive special argument names. A
valid non-undefined cached value is returned directly at each reporter
site, and this site increments the native buffered block counter
exactly once. There is no eager reporter evaluation or double-counting.
The fallback remains marked may-yield.

### Correctness / safety

A new `hoist` mode is covered by
`tests/differential/jit_nested_warp_direct_args.py`:

- Numeric procedure argument, deep warp repeat: reporter, direct and
  hoisted paths all **actually promoted** and returned to the source
  interpreter caller; final state equal to Interpreter.
- Nested `repeat`, Japanese / surrogate-pair names (`表示🌠`, `増分`),
  GC-managed Japanese/emoji value: same promotion and parity.
- Invalid undeclared argument name: **all three modes reject OSR**.
- `XYO_BLOCK_COUNT=1`: interpreter, reporter, direct and hoisted
  all count exactly **24,000,015** executed Scratch primitives.
- `XYO_GC_THRESHOLD=1` with string allocations during native execution:
  **368 collections**, no loss of the original argument or caller frame
  and exact Scratch final state, both direct and hoisted.
- GUI Scratch oracle: Interpreter, HotSpot, Eager each **2123/2123**.

The coherent isolated binary used for these tests:
`board/osr-hoisted-verified-xyo`, SHA-256
`47247f5e3d2ec2702576ee1def468feaea341da7682ae84e349876828431ec12`.
Its recorded source digest is
`b6492df71b3a2fc9575a8b55e8d1b927c54dba0ba7d7e890c20f0486485f13cd`.

### Native-only benchmark (single numeric loop)

`tests/bench/bench_nested_warp_osr_args.py` records a timestamp at
`[jit-osr-nested] promoted` and `[jit-osr-nested] restored caller`.
It uses the actual **remaining iteration count** at promotion to
normalize out variability from asynchronous compilation. Same frozen
binary, same 12-million-iteration Scratch project, two ABCCBA rounds
(four native intervals per mode), final Scratch state checked.

| Native mode | Median million iterations/sec | Reporter-relative |
| --- | ---: | ---: |
| Interpreter reporter trampoline | 8.9565 | 1.00x |
| Live frame map lookup per argument read | 92.6054 | 10.34x |
| Hoisted, GC-rooted SSA argument | 483.7816 | 54.02x |

The SSA path was approximately **5.22x** faster than the per-read map
lookup in this microbenchmark. Raw measurements:
`board/.osr-hoisted-bench/results.json`.

These are **native-only loop** measurements, not application end-to-end
speedups. They omit process startup and JIT compilation costs and
include a small scheduler/continuation-return overhead. Other Scratch
workloads may behave differently.

General source-PC/SSA/effect commit migration across multiple simultaneous
interpreter procedure frames is **not implemented** by this optimization.
Commit and push have not been performed.
