# LLVM ORC lazy JIT Delta32 relocation under AddressSanitizer — 2026-11-11

## Reproduction

Full GCC16/LLVM23.1.4 ASan+UBSan binary:
`$HOME/.cache/xyo-asan-ubsan-build-20261111/xyo`.

The CPU-only `calculator_tests.sb3` contains 148 executable
assertions and generates 54 LLVM functions / roughly 108,856 IR
instructions. The first LeakSanitizer-enabled Eager run (without
lazy ORC materialization) passed all assertions.

With `--jit hotspot --jit-lazy on` **before the fix**, ORC
printed an error like:

```text
JIT session error: ... section .text: relocation target ... is out of range of Delta32 fixup ...
JIT session error: Failed to materialize symbols: { (main.impl, { xyo_script_0_p0 }) }
AddressSanitizer: SEGV on unknown address 0x000000000000 (pc 0x000000000000)
```

The materialized ORC code and an anonymous target appeared
at addresses separated by terabytes in the ASan virtual
address-space layout. This exceeds the signed 32-bit PC-relative
relocation range. That can make lazy symbol materialization
fail and the call-through trampoline can reach a null
failure address. It is distinct from the historical
`JitEngine::~JitEngine` shared-pointer destruction crash.

Control cases:

- Release binary with HotSpot/lazy default: **148/148 PASS**.
- Instrumented binary with `--jit hotspot --jit-lazy off`:
  **148/148 PASS**.
- Instrumented binary with explicit
  `XYO_JIT_CODE_MODEL=large`, HotSpot/lazy on:
  **148/148 PASS**, no ASan/UBSan/LeakSanitizer report.
- Instrumented binary with explicit Large, Eager/lazy on:
  **148/148 PASS**.

## Fix

Modified `src/jit/backend_llvm.cpp`.
A single `configureExperimentalCodegen` helper configures both
the eager `LLJITBuilder` and the
`LLLazyJITBuilder`, retaining the
existing `XYO_JIT_CODEGEN_OPT=less|none` behavior.

`XYO_JIT_CODE_MODEL=large` forces the
`llvm::CodeModel::Large` target machine code model
for both ORC engines. With no override, an ASan
build (GCC `__SANITIZE_ADDRESS__` or Clang
`__has_feature(address_sanitizer)`) now defaults
to Large so normal HotSpot delayed compilation works.
Release builds keep LLVM's previous, unchanged default.
`XYO_JIT_CODE_MODEL=default` can override the
ASan default for targeted reproduction, but may
reproduce the relocation crash.

No changes to compiled Scratch ABI, LLVM optimization
pass selection or production default JIT code model.

## Regression results

Added `tests/differential/jit_large_code_model_lazy.py`
and registered CPU CTest
`xyo-jit-large-code-model-lazy`.
It verifies 148/148 Scratch assertions and actual
`tier=lazy` and successful compilation telemetry
for both HotSpot and Eager. The script accepts
`--code-model large` (default) and
`--code-model auto` to test the sanitizer
compile-time fallback with no environment override.

- Instrumented ASan/UBSan + LeakSanitizer, explicit Large:
  HotSpot/lazy and Eager/lazy **148/148 PASS** each.
- Instrumented ASan/UBSan + LeakSanitizer,
  **no code-model environment variable** after the fix:
  HotSpot/lazy and Eager/lazy **148/148 PASS** each.
- Full rebuilt Release binary, registered CPU CTest:
  **1/1 PASS**, including both modes and 296 assertions.
- Full rebuilt Release binary, independent
  `xyo-jit-offscreen-teardown` and
  `xyo-jit-offscreen-capture-teardown`
  GPU/NGX CTests: **2/2 PASS**.
- ASan/UBSan OpenGL fallback build, after final change:
  six further JIT off/HotSpot/Eager subprocesses running
  16 durable PPM frames and orphan recovery: **6/6 PASS**.

### Commands

```sh
# Instrumented CPU-only regression; detect_leaks=1 is supported
# outside the restricted sandbox execution environment.
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
python3 tests/differential/jit_large_code_model_lazy.py \
  "$HOME/.cache/xyo-asan-ubsan-build-20261111/xyo" \
  --code-model auto

# Production CPU CTest with explicitly selected Large
ctest --test-dir "$HOME/.cache/xyo-dlss-build/play" \
  -R '^xyo-jit-large-code-model-lazy$' --output-on-failure
```

## Caveats and next investigation

This resolves the **ASan-specific Delta32 relocation**
for the tested ORC lazy code path by using Large model.
It does not establish the original shared-pointer
SIGSEGV root cause, and a generic ORC materialization
error may still lead to a null call-through failure
address. A robust future improvement is to provide
an explicit non-returning ORC lazy-compile failure
handler and test error propagation/fail-closed behavior
without a dereference at address zero.

The GPU sanitizer environment fell back to OpenGL
because NVIDIA Vulkan external extensions were
unavailable; the sanitizer's GPU test did not execute
proprietary NGX. No commit/push.
