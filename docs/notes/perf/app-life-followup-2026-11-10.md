# PERF-001: app_life continuation (2026-11-10)

## Starting point

The preceding `XYO_JIT_LIST_STRING_NUM_FAST` optimization remains default ON.
Original ScratchBench performance suite (`e8bd5be3e7345088`): `app_life`
16,157.564 ops/ms in the previous single-run ON snapshot (vs 15,846.68
with that earlier optimization disabled); stored MistWarp reference 22,910.307.

`app_life` simulates 12x12 Conway Life with numeric-string list contents,
nested loops, 8 neighbor reads per cell, list replacements, and modulo-based
toroidal indices. The test sources are unchanged.

## Experiments

1. **Constant-modulus one-wrap lowering:** an opt-in guarded branch would
   avoid `urem` on exact positive values below twice a constant divisor. The
   candidate compiled but aborted with `LLVM ERROR: SmallVector unable to
   grow` when compiling the real `app_life` diagnostic. Entire trial was
   reverted from shared `src/jit/codegen.cpp`; original code rebuilt.
2. **32-bit list index conversion:** an opt-in x86 32-bit `cvttsd2si` path,
   retaining 64-bit conversion when `list.len > INT32_MAX` and preserving
   invalid-index behavior. On identical binary, CPU8, 14 alternating pairs,
   baseline median 27.840 vs candidate 25.3975 diagnostic ops/ms; speed ratio
   0.913992, **0/14 wins**. The entire change was reverted and rebuilt.
3. **Direct cold string parse:** after a `data_itemoflist` numeric consumer
   has proved a heap SStr and `numState == 0`, call the established
   `xyo_to_num_uncached(SStr*)` helper instead of re-running the complete boxed
   numeric coercion. The source remains generic and retains full fallback
   paths for non-string list items. Enabled by
   `XYO_JIT_LIST_STRING_MISS_DIRECT=1`; disabled by default.

### Direct cold string parse measurement

- i7-12700 CPU8 pinned, same native Release executable, 14 interleaved
  candidate/baseline pairs, unchanged source-derived `life.sb3` fixture:
  baseline median **27.852** diagnostic ops/ms, candidate **27.877**;
  paired geometric-mean ratio **1.002695**, candidate wins **12/14**.
- Every diagnostic pair had checksum **53**, exited 0, printed DONE.
- Original 47-workload suite, **one** separate run with candidate enabled:
  overall score **110,794**, `app_life` **16,107.021 ops/ms**.
  The earlier snapshot with candidate disabled showed overall **111,027**,
  `app_life` **16,157.564 ops/ms**. These independent runs are noisy, so no
  full-suite benefit was established. Keep candidate opt-in.
- Differential test `tests/differential/jit_list_string_numeric_fast.py`
  extended to compare cold ON vs OFF, old path and Interpreter, in both
  Eager and HotSpot JIT. All **14 project cases passed**, including mixed
  numeric/nonnumeric strings, mutations, other script, invalid list indices.
- The original ScratchBench GUI Parity suite with
  `XYO_JIT_LIST_STRING_MISS_DIRECT=1` also passed **2656/2656** in each
  mode: Interpreter, HotSpot JIT, Eager JIT and AOT. Report:
  `scratch-bench/results/perf-life-miss-direct-parity-20261110/`.

Raw data:
- `build/perf-life-wrap-ab.log` (LLVM abort)
- `build/perf-life-index32-ab.{json,log}` (rollback evidence)
- `build/perf-life-miss-direct-ab.{json,log}`
- `scratch-bench/results/perf-life-direct-miss-20261110/performance-xyo-cpp.json`
- `build/perf-life-miss-direct-parity.log` (targeted differential)

### Native machine-code examination

JIT object dumps created from the unchanged `life.sb3` under
`build/perf-life-jit-objects/` show that optimized code still contains
numeric index conversions, 64-bit multiplicative-constant remainder
lowering and many fallback calls. These are static observations, not a
sampled hot-path count; further structural optimization needs a stronger
profiling/profitability case.

## Safety and rollout

The new cold path is OFF by default because its measured overall benefit is
unconfirmed. The earlier numeric-string cache feature remains default ON.
The working tree was not committed or pushed. Other concurrent edits were
retained; no benchmark-source or saved rival data were modified.
