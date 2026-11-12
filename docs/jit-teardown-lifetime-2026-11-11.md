# Native JIT shutdown lifetime investigation — 2026-11-11

## Observed crashes

Four systemd core dumps from 2026-11-11, recorded at 07:49:22, 07:50:20,
07:52:01 and 07:53:02 JST, had the same crashing stack:

```text
std::_Sp_counted_base<...>::_M_release()
xyo::JitEngine::~JitEngine()
xyo::Runtime::~Runtime()
xyoShellPlay(...)
main(...)
```

The first crash was an actual SDL/NGX offscreen screenshot run (not a
synthetic PBO failure injection). GDB displayed an invalid pointer-like
register value (RDI=0x981) for its shared-pointer release frame.
This establishes that the fault happened in JIT teardown, but **does not**
identify which shared pointer was damaged or when. The saved core's
executable build ID differs from the subsequently rebuilt local
`xyo` binary, so current-symbol source attribution beyond the
systemd stack is not reliable. A cached pointer from an incompatible
translation-unit layout, a previous memory overwrite, a use-after-free
or a destructor-order problem are still hypotheses.

The GPU recovery regression was subsequently isolated with
`--jit off`; therefore those PBO regression passes alone could not
demonstrate that JIT shutdown was healthy. Do **not** treat earlier
"all GPU tests PASS" as a proof that the old JIT destructor crash
was fixed.

## Concrete lifetime ordering hazard and correction

`JitEngine` declares `impl_` **before** its compiled-code caches:
`cache_`, `retiredAot_`, `numericCache_`, etc. Under ordinary C++
reverse-order member destruction, the compiled-code caches are destroyed
**before** `impl_` (including LLVM backend and AOT slot owners).

Previously, `JitEngine::~JitEngine()` explicitly called
`impl_.reset()` inside the destructor body, reversing that natural
ordering. It destroyed the backend/AOT owners **before** compiled-code
caches. The asynchronous JIT worker was joined only by
`Impl::~Impl()`.

Changed `src/jit/jit.cpp` to:

1. Provide an idempotent `Impl::stopWorker()`, which drops outstanding
   compilation requests, signals the worker and joins it before
   teardown proceeds.
2. Call `stopWorker()` explicitly in `JitEngine::~JitEngine()`
   before unregistering the Impl GC root provider.
3. **Remove the premature `impl_.reset()`.** The C++ compiler now
   destroys the ScriptEntry/numeric/AOT cache members naturally and
   the `impl_` member last. `Impl::~Impl()` also calls `stopWorker()`
   safely, then reports compile telemetry.

No JIT ABI layout or generated LLVM IR was changed. This fixes an
independently demonstrated C++ lifetime-order hazard. **It is not proof
that the four historical SIGSEGVs had this root cause.**

## Executed validation on RTX 3070 / LLVM 23.1.4

Rebuilt the full `xyo-cli` target successfully.

New deterministic regression programs (both registered in
`tests/CMakeLists.txt`):

- `xyo-jit-offscreen-teardown` — real NVIDIA NGX initialized; modes
  `--jit off`, `--jit hotspot`, `--jit eager`; 12 invocations per
  mode per CTest run. The test **requires** JIT compilation telemetry
  showing successful asynchronous HotSpot preparation/finish and
  synchronous Eager compilation (not merely a successful JIT option
  parse). Four full CTest invocations passed, i.e. 144 short-lived
  processes completed without crash after the change.
- `xyo-jit-offscreen-capture-teardown` — 4 runs per mode (12 processes)
  with real SDL/NGX, direct-mapped RGB PBO, 16 durable PPM frames and
  background recovery of 3 aged temporary images per process.
  Two CTest invocations passed (24 processes in total).

Existing GPU regressions after the change:

- Headless/DLSS/PBO suite: **21/21 PASS** (includes stale recovery,
  series locking, real SIGKILL, 64-unlink fsync batching and GL fault
  injection).
- Independent GPU Pen quantized parity: **1/1 PASS** (37.95 s).
- Total **24/24 distinct focused GPU CTests PASS** (21 + 1 + 2).
- Python AST check and `git diff --check` performed separately.

Before the ownership-order fix, a simpler JIT Off/HotSpot/Eager check
already passed 12 processes per mode. Therefore the reproduced
historical crash frequency is unknown, and lack of new crashes is
limited negative evidence rather than a proven root-cause fix.

## Reproduction

```sh
ctest --test-dir "$HOME/.cache/xyo-dlss-build/play" \
  -R '^xyo-jit-offscreen-(teardown|capture-teardown)$' \
  --output-on-failure -j1
```

The CTests need a valid NVIDIA NGX SDK dynamic library path and a
working RTX GPU; follow the test's `XYO_DLSS_FEATURE_DIR` environment
property in `tests/CMakeLists.txt`.

## Remaining questions

- Why did the older compiled binary contain a bad shared-pointer
  release target? The original build ID and its matching debug symbols
  are needed for field-level identification.
- Are concurrent edits and partial rebuilds causing mismatched C++
  translation-unit ABI assumptions for appended `CompiledScript`
  fields? A clean isolated build and sanitizer-enabled regressions
  would help distinguish this from a runtime heap overwrite.
- Does a heavier Scratch script involving nested OSR/native reporter
  provenance reproduce the old issue? The new smoke covers a compiled
  Pen script and the async JIT worker, not all JIT code shapes.

Changes remain **uncommitted and unpushed**.

## ASan/UBSan follow-up (2026-11-11)

Independent GCC 16 AddressSanitizer/UBSan build compiled. Validated 36/36 GL headless JIT teardown subprocess runs (JIT off/HotSpot/Eager; half also execute durable async PBO capture and orphan recovery), zero sanitizer reports. Sanitizer build fell back from NVIDIA NGX to OpenGL; LeakSanitizer and UBSan vptr checks were disabled, and JIT-generated machine code is not automatically instrumented. The historical crash root cause remains unproven. Detailed flags, commands and limitations: docs/jit-teardown-asan-ubsan-2026-11-11.md.

## Later finding: ASan ORC lazy memory layout (2026-11-11)

Additional LeakSanitizer CPU fixture testing found an LLVM ORC lazy
materialization error (Delta32 relocation out of range), followed by
a null-PC crash in sanitizer builds. This is distinct from the original
destructor shared_ptr crash. Fixed ASan JIT code model selection and
added a native CPU-only CTest for HotSpot/Eager lazy compilation.
Details: docs/jit-lazy-orc-large-model-asan-2026-11-11.md.

## ASan/UBSan/LSan follow-up (2026-11-11 afternoon)

An existing isolated binary under
`$HOME/.cache/xyo-asan-ubsan-build-20261111/xyo`
was rebuilt from the **current** source without touching the
ordinary Release build. Compiler/link flags include
`-fsanitize=address,undefined -fno-omit-frame-pointer`
with `-O1 -g1` for this build; `vptr` instrumentation
is explicitly disabled (`-fno-sanitize=vptr`). The final ELF
dynamically links libasan.so.8 and libubsan.so.1.

With
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1:detect_stack_use_after_return=1`
and
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`:

- Real JIT Off / HotSpot / Eager on the simple Scratch Pen
  project: **21/21 subprocesses** completed, with 16
  durable RGB PBO frames and three orphaned temporary files
  recovered per run. No ASan/UBSan fault was reported.
- The real `debug/projects/3D2.sb3` ran in HotSpot
  and Eager modes; each saved 8 valid PPM frames, compiled
  native LLVM code and exited cleanly, with zero sanitizer
  reports.
- Two CTests on the instrumented binary passed:
  `xyo-jit-offscreen-capture-teardown` (12 subprocesses,
  52.18s) and `xyo-jit-offscreen-teardown`
  (9 subprocesses, 46.20s).
  The sanitizer setup selects three cycles rather than
  twelve for the latter test because each instrumented
  GPU/GL startup is expensive. Ordinary release builds
  retain the 36-process test.

**Crucial GPU test distinction:** NVIDIA NGX's Vulkan
external extensions may fail during ASan instrumentation,
causing xyo to execute its genuine SDL/OpenGL fallback
rather than NGX. The CTest now accepts this **only when**
the project was compiled with `-fsanitize`; Release
CTest still requires actual `NVIDIA NGX initialized`.
The smoke script counts which backend was reached:
one post-update six-process sanitizer run reported
`NGX=0 OpenGL fallback=6`. Thus, the sanitizer
suite validates CPU-side JIT teardown, OpenGL and PBO
lifetime; it must **not** be reported as a successful
sanitized NVIDIA NGX inference test.

### LSan attribution

Leak detection enabled on GPU paths produced:

| Path | JIT | Leaked bytes / allocations |
|---|---|---|
| SDL/OpenGL, DLSS disabled | Off | 797 B / 14 |
| SDL/OpenGL, DLSS disabled | Eager | 797 B / 14 |
| DLSS enabled | Off | 1,163 B / 22 |
| DLSS requested (fallback in this run) | Eager | 797 B / 14 |

The extra 366 B / 8 allocations in the third case
include allocation call stacks referencing
`libcuda.so.1`; the baseline 797 B stacks are
mostly unresolvable external modules.
These are small but **real LSan observations**,
not clean leak-check passes. They are reproduced even
with native JIT *execution disabled*, and the identical
Off/Eager baseline suggests a GPU/SDL/driver-related
source. The exact owner of *every* allocation has
not been established, so do not blindly suppress
all such reports or assert all are NVIDIA bugs.

To isolate xyo's native JIT lifetime from graphics
libraries, a new
`tests/render/jit_cpu_lsan_teardown_smoke.py`
uses `xyo run project.sb3 --frames 12 --jit ...`
without SDL initialization. A frame limit causes
the **expected exit code 2**; the test verifies this
deliberate result and successful native JIT compile
telemetry, and treats other statuses or leak reports
as failure. The CMake test
`xyo-jit-cpu-lsan-teardown` is registered **only
for AddressSanitizer builds**, and **forces leak
detection ON** through the test's own CTest environment
(`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:exitcode=83`).
It passed 12/12 CPU processes (4 cycles of Off,
HotSpot, Eager) in 5.84 seconds, with no ASan,
UBSan or LSan reports. A separate three-mode manual
CPU-only LSan check also found zero leaks.

The source/environment for these findings is preserved
in the repo under the sanitizer-only CTest and in the
local cache log files:

- `$HOME/.cache/xyo-asan-ubsan-build-20261111/leak-gl-off.txt`
- `$HOME/.cache/xyo-asan-ubsan-build-20261111/leak-gl-eager.txt`
- `$HOME/.cache/xyo-asan-ubsan-build-20261111/leak-ngx-off.txt`
- `$HOME/.cache/xyo-asan-ubsan-build-20261111/leak-ngx-eager.txt`

Reproduce the sanitizer lifecycle tests (with known
third-party graphics leak reports isolated from them):

```sh
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --test-dir "$HOME/.cache/xyo-asan-ubsan-build-20261111" \
  -R '^xyo-jit-offscreen-(teardown|capture-teardown)$' \
  --output-on-failure -j1

# Keep LSan ON for the CPU-only JIT ownership regression:
ctest --test-dir "$HOME/.cache/xyo-asan-ubsan-build-20261111" \
  -R '^xyo-jit-cpu-lsan-teardown$' --output-on-failure
```

**Limits:** This instrumentation tests the host C++
runtime and its visible allocations, but does not
fully instrument dynamically generated JIT machine code,
NVIDIA proprietary libraries or LLVM shared libraries.
The historical 4 SIGSEGVs were not reproduced and
their root cause remains unproven. No commit/push.


### Current-tree ASan/UBSan rebuild and driver-leak differential (2026-11-11)

Reused the isolated GCC AddressSanitizer + UndefinedBehaviorSanitizer
build at `$HOME/.cache/xyo-asan-ubsan-build-20261111`. The executable
was rebuilt from the **current** work tree (41 Ninja compile/link
actions) with `-fsanitize=address,undefined`,
`-fno-sanitize=vptr`, `-O1 -g1`. The ordinary Release
binary was not modified.

With leak detection disabled **only for offscreen GPU testing**,
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1`
and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`:

- Offscreen JIT Off/HotSpot/Eager: 4 runs/mode, 12/12 processes
  clean; actual HotSpot/Eager LLVM compilation verified.
- Offscreen + RGB direct-mapped PBO + 16 durable saved frames
  and 3 aged orphan recoveries: 3 runs/mode, 9/9 processes
  clean. All 9 selected **OpenGL fallback**, not NVIDIA NGX.
- Instrumented CTests: `xyo-jit-offscreen-teardown` and
  `xyo-jit-offscreen-capture-teardown`, both PASS
  (63.02 s and 52.94 s). No ASan/UBSan report.
- CPU-only `xyo-jit-cpu-lsan-teardown`: 12/12 actual
  JIT executions PASS, with `detect_leaks=1` enforced.
- CPU-only ORC lazy Large code-model regression
  `xyo-jit-large-code-model-lazy`: PASS; 148/148
  Scratch assertions for HotSpot and Eager. This also
  exercises the earlier ASan-specific Delta32 relocation
  correction.

LeakSanitizer was separately enabled for a *direct*
single-process SDL/OpenGL comparison under the same
ASan binary. Observed reports:

| Offscreen condition | LSan report | Example allocation origin |
|---|---|---|
| `XYO_DLSS_REAL=0` | 797 bytes / 14 allocations | Mostly external unresolved modules |
| `XYO_DLSS_REAL=1` (GL fallback) | 5,278 bytes / 32 allocations | `libdbus-1.so.3`, `libcuda.so.1` and unresolved modules |

These figures are **single-run observations**, not
stable driver-leak sizes: another DLSS attempt reported
1,163 bytes / 22 allocations. Different external
graphics initialization paths can allocate different
amounts. No native JIT stack was conclusively identified
as a leak origin in these reports. CPU-only leak-enabled
JIT teardown was clean, but the precise ownership of
every GPU-related allocation remains unresolved. Avoid
silencing all leak reports with broad suppressions.

The sanitizer test suite does **not** fully instrument
proprietary driver libraries, generated LLVM JIT machine
code or every external allocation. Neither these clean
runs nor the independently corrected destructor ordering
prove the historical shared_ptr SIGSEGV root cause.

Local diagnostic logs:
`$HOME/.cache/xyo-asan-ubsan-build-20261111/lsan-cpu-{off,hotspot,eager}.log`
and `lsan-gl-dlss-{0,1}.log`.

No commit/push.


### ASan/UBSan focused JIT teardown and 48-frame stress (2026-11-11)

Using the existing independent `$HOME/.cache/xyo-asan-ubsan-build-20261111`
build with `-fsanitize=address,undefined -fno-sanitize=vptr -O1 -g1`,
CMake was reconfigured and the full sanitized `xyo-cli` successfully
rebuilt (52 incremental Ninja actions). `ldd` confirms libasan.so.8
and libubsan.so.1. The regular Release binary was not modified.

The JIT teardown regression harness now supports `--modes` and
`--timeout`, with per-process progress and stderr on timeout.
An initial combined run exceeded the default 35-second timeout
during Eager compilation; that was not an ASan/UBSan report.
The normal CTest still uses its existing 35-second timeout.

After setting `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1`
and `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`,
and allowing the supported OpenGL fallback:

- 8-frame no-capture: JIT Off, HotSpot, Eager, 1 each (3/3).
- 8-frame durable PBO + 3 aged orphan recoveries:
  5 iterations per JIT mode, 15/15.
- 48-frame durable PBO + 3 aged orphan recoveries:
  2 HotSpot and 2 Eager iterations, 4/4.

All 22 successfully completed instrumented subprocesses
exited normally without fatal ASan/UBSan diagnostics.
Actual HotSpot/Eager compilation was required by the test.
Every sanitized GPU subprocess reported NGX=0 and
OpenGL fallback=1; this does NOT verify NVIDIA NGX under
sanitizers.

The regular release-build JIT offscreen CTests passed 2/2
after the test-harness changes. The existing sanitizer
CPU LSan JIT teardown and LLVM large-code-model-lazy
CTests were re-run and both passed 2/2.

Reproduce a longer sanitized case:

```sh
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" python3 tests/render/jit_destructor_offscreen_repro.py   "$HOME/.cache/xyo-asan-ubsan-build-20261111/xyo"   --modes hotspot,eager --runs 2 --frames 48   --timeout 120 --capture --allow-dlss-fallback
```

Leak detection was disabled in these GPU runs; the separate
CPU-only LSan tests have it enabled. JIT-generated machine
code and proprietary GPU components are not fully instrumented.
The historical destructor SIGSEGV remains unexplained, not
proven fixed by these negative tests. No commit/push.
