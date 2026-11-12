# JIT shutdown: isolated ASan/UBSan validation (2026-11-11)

Built a separate LLVM 23 / SDL / Vulkan-enabled native xyo CLI at
`$HOME/.cache/xyo-asan-ubsan-build-20261111/xyo`.
The ordinary Release executable and build directory were left unchanged.

## Build configuration

GCC 16.2.1, `-O1 -g1 -DNDEBUG`, and
`-fsanitize=address,undefined -fno-omit-frame-pointer`.
The first link failed on `typeinfo for SdlRenderer` /
`typeinfo for SdlAudioBank` because UBSan's vptr checking expects
RTTI while xyo builds with `-fno-rtti`. Reconfigured with
`-fno-sanitize=vptr` for C++ compilation and linking, keeping
ASan and the other UBSan checks enabled; full `xyo-cli` built
successfully.

```sh
cmake -S . -B "$HOME/.cache/xyo-asan-ubsan-build-20261111" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer' \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -g1 -DNDEBUG' \
  '-DCMAKE_C_FLAGS_RELEASE=-O1 -g1 -DNDEBUG' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined -fno-sanitize=vptr' \
  -DXYO_BUILD_TESTS=ON -DXYO_JIT=ON -DXYO_JIT_BACKEND=llvm \
  -DXYO_NVSDK_DLSS_VK=ON \
  -DXYO_NVSDK_DLSS_SDK_ROOT="$HOME/.cache/xyo-dlss-sdk" \
  -DXYO_DLSS_REGISTER_GPU_TESTS=ON

CPLUS_INCLUDE_PATH="$HOME/.cache/xyo-pen-verify/include/blocks" \
  cmake --build "$HOME/.cache/xyo-asan-ubsan-build-20261111" \
  --target xyo-cli -j4
```

## Executed tests

Under ASan/UBSan, the Vulkan external-extension initialization returned
unavailable, and the SDL renderer used the **OpenGL fallback**, not
NVIDIA NGX. Added an explicit
`--allow-dlss-fallback` option to
`tests/render/jit_destructor_offscreen_repro.py` for sanitizer
experiments. It permits a genuine SDL OpenGL context **only when**
the engine also reports fallback. The existing production GPU CTests
retain their original strict NVIDIA NGX initialization assertion.

Set the sanitizer runtime environment:

```sh
export ASAN_OPTIONS='detect_leaks=0:halt_on_error=1:abort_on_error=1'
export UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1'
export XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel"

python3 tests/render/jit_destructor_offscreen_repro.py \
  "$HOME/.cache/xyo-asan-ubsan-build-20261111/xyo" \
  --runs 4 --frames 16 --allow-dlss-fallback

python3 tests/render/jit_destructor_offscreen_repro.py \
  "$HOME/.cache/xyo-asan-ubsan-build-20261111/xyo" \
  --runs 4 --frames 16 --capture --allow-dlss-fallback
```

Both scripts were also run for an initial two iterations per
JIT mode. HotSpot and Eager assert successful actual LLVM compile
telemetry; the OFF mode asserts no compiler attempts.
The capture cases validate 16 durable PPM output images and
three aged orphan file recoveries per process.

| Test | JIT off | HotSpot | Eager | Total |
|---|---:|---:|---:|---:|
| Headless GL without capture | 6/6 | 6/6 | 6/6 | 18/18 |
| GL + async durable PBO and recovery | 6/6 | 6/6 | 6/6 | 18/18 |
| **Total clean exits** | **12/12** | **12/12** | **12/12** | **36/36** |

**No ASan/UBSan error reports** in these 36 successful
subprocess runs. A separate attempt at a larger repetition
count returned an internal tool error and is not included.

## Limits and next check

- AddressSanitizer instruments the C++ runtime but **not**
  arbitrary native machine code emitted dynamically by LLVM JIT.
- The proprietary GPU SDK and system GPU drivers were not rebuilt
  with sanitizers. Real NGX execution did **not** run in this build.
- UBSan's `vptr` check was disabled because the project has RTTI
  disabled; other UBSan checks remained on.
- LeakSanitizer was disabled (`detect_leaks=0`) to avoid
  reporting unreclaimable allocations from third-party GPU libraries;
  these tests therefore do not prove absence of memory leaks.
- The original shared-pointer SIGSEGV was not reproducible in the
  release binary during the previous test suite either. These
  negative results do **not** prove its cause or its correction.

Suggested follow-up: **CPU-only leak sanitizer** and
clean ABI-coherent build targeting native JIT/OSR source symbol
lifetimes, then a longer mixed workload.

No commit/push.

## Later finding: sanitizer-only LLVM ORC Delta32 relocation

On the larger CPU-only calculator fixture (148 assertions), ASan/UBSan
with default ORC HotSpot lazy compilation initially failed to materialize
xyo_script_0_p0 due to an out-of-range Delta32 fixup and crashed at
PC=0. HotSpot with lazy disabled passed the same fixture. A dedicated
fix uses the LLVM Large code model in sanitizers, opt-in elsewhere.
See docs/jit-lazy-orc-large-model-asan-2026-11-11.md for full evidence,
how to reproduce the fix, and the still-unhandled ORC error path.
