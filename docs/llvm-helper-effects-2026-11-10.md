# LLVM external helper effects — 2026-11-10

## Scope and decision

Adopt a small, default-on whitelist for `xyo_asin` and `xyo_acos`:
`memory(none) nounwind willreturn`. `XYO_JIT_PURE_MATH=0` restores the old
unannotated declarations for paired experiments. No arithmetic formulas, runtime
helper implementations, yield points, GC roots, volatile reloads, alias promises,
or fast-math flags change.

The implementations live outside the generated LLVM module. Previously LLVM saw
only their signatures, and therefore could not common identical calls even when
the input SSA value was identical. The new annotations communicate the audited
contract instead of implementing a custom expression optimizer.

Implementation: [attribute whitelist](../src/jit/llvm_helper_attributes.hpp),
[declaration integration](../src/jit/codegen.cpp).

## Why only these two helpers?

Their wrappers in [rt.cpp](../src/jit/rt.cpp) are uninstrumented forwards to
[v8math.cpp](../src/core/v8math.cpp). Both implementations use constant-initialized
coefficients and local arithmetic. Invalid inputs are rejected before square root;
all square-root operands are nonnegative, so those calls cannot write domain
`errno`. There are no volatile accesses, lazy initializers, allocations, callbacks,
or profiling counters. Return type, argument type/count, varargs and declaration
status are checked before adding attributes.

This assumes the existing ordinary, non-strict FP contract. It does **not** make
floating-point exception flags observable or support changing rounding modes
across an optimized expression. No `speculatable` or fast-math attributes are added.

Explicit exclusions:

- `xyo_atan` and `xyo_exp`: their fdlibm implementations have volatile accesses.
- `xyo_log`, `xyo_round`, boxed arithmetic: runtime profiling can initialize state
  and update counters. An apparently pure Scratch opcode is not necessarily an
  LLVM-pure external call.
- `xyo_to_num`: reads and publishes string conversion caches.
- Yield/GC/list/motion/pen helpers: remain conservative.
- General libc math and custom `sin`/`cos`: no blanket annotation.

The compile-time `XYO_DISABLE_RT_PROF` switch is selectively applied to runtime
sources, not to the code generator. This whitelist does not depend on that macro.

## Observed optimized IR

For a dynamic `x`, the fixture computes:

```text
sum += (asin(x) + asin(x)) + (acos(x) + acos(x))
x changes on each iteration
```

Before, each helper is called twice with the same input SSA operand on the loop
path. After, each is called once and LLVM reuses its result. LLVM also peels/clones
the optimized loop, so the whole-module **static** call count happens to stay at
two per helper. The regression checks repeated calls for identical input operands inside each
function/basic block (SSA names are not global), not an incorrect expectation
that total static calls must decrease.

## Performance

Same Release binary, seven alternating OFF/ON pairs, 5,000,000 iterations. The
project warms up before timing and uses its own `days since 2000` clock. Both
variants produce checksum `900000000`.

| Configuration | Median | Observed range |
|---|---:|---:|
| Attributes OFF | 88 ms | 87–91 ms |
| Attributes ON | 52 ms | 51–54 ms |
| Speedup | **1.69x** | |

An earlier seven-pair run gave 89 → 52 ms (1.71x); the table uses the final
rerun after strengthening the HotSpot/CSE assertions.

This is a deliberately CSE-sensitive synthetic kernel, **not** a general runtime
speedup. The existing canonical ScratchBench performance project contains no
`asin`/`acos` math blocks, so this change is not expected to improve its total
score. No full-suite performance improvement is claimed. Startup time is outside
the timed region; a separate binary build is not used as the ON/OFF baseline.

Local artifacts: [paired measurements](../build/llvm-helper-effects/verified-timing/paired.json),
[optimized OFF IR](../build/llvm-helper-effects/verified-timing/cse-0.ll),
[optimized ON IR](../build/llvm-helper-effects/verified-timing/cse-1.ll),
[timing binary hashes](../build/llvm-helper-effects/verified-sha256.txt).
The timed no-profiler binary is preserved as
[the dedicated timing runtime](../build/llvm-helper-effects/xyo-noprof).

## Validation

- 162 boundary outputs match the interpreter for Eager and HotSpot, with attributes
  ON and OFF. HotSpot uses `XYO_HOT_INVOCATIONS=1` and requires `jit=yes` in the
  statistics, plus actual optimized IR declarations, so interpreter fallback
  cannot masquerade as HotSpot coverage. Cases include endpoints and their
  neighbors, tiny/subnormal values, signed zero (also checked via reciprocal),
  infinities, NaN and mixed strings.
- AOT ON/OFF matches the same reference; the test requires actual nonzero AOT
  script loading and rejects silent fallback.
- Whitelist policy tests check positive/negative names and reject wrong signatures,
  varargs, and function definitions (44 checks per ON/OFF process).
- Seven selected CTests pass, including the full `xyo-tests` executable, type-flow
  safety, if-PHI, conditional repeat SSA, and the three new tests.
- Rebuilt with `XYO_DISABLE_RT_PROF=OFF`: edge parity, CSE and AOT still pass.
  A further `--profile` run enables `XYO_PROF=1` and verifies that the arithmetic
  runtime profiler was actually exercised. The three final attribute CTests pass.
- Existing full Scratch parity headless run: ON/OFF output is byte-identical,
  both **1899/2123**. This is an equality check, not a claim that headless rendering
  and sound tests pass; those baseline failures are unchanged.

Tests: [differential/IR/AOT driver](../tests/differential/jit_llvm_helper_attributes.py),
[standalone policy test](../tests/platform/llvm_helper_attributes_smoke.cpp).
Logs: [selected CTests](../build/llvm-helper-effects-ctest.log),
[active profiler validation](../build/llvm-helper-effects-prof-verified.log),
[final attribute CTests](../build/llvm-helper-effects-verified-ctest.log).

Reproduction:

```sh
cmake -S . -B build/llvm-helper-effects -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DXYO_BUILD_TESTS=ON -DXYO_SDL2=OFF
cmake --build build/llvm-helper-effects \
  --target xyo-cli xyo-tests xyo-llvm-helper-attributes-smoke -j 3
ctest --test-dir build/llvm-helper-effects --output-on-failure \
  -R '^(xyo-tests|xyo-llvm-helper-attributes-(on|off)|xyo-jit-llvm-helper-attributes)$'
python3 tests/differential/jit_llvm_helper_attributes.py \
  build/llvm-helper-effects/xyo --aot --bench --trip 5000000
# With XYO_DISABLE_RT_PROF=OFF:
python3 tests/differential/jit_llvm_helper_attributes.py \
  build/llvm-helper-effects/xyo --aot --profile --trip 10000
```

## Remaining larger barriers

Yield spill/reload, dynamic type transitions, unknown aliasing and strict FP
recurrences are unchanged. Removing them requires guarded typed regions and
correct scheduler/GC/deopt boundaries, not speculative `noalias`, `readnone`,
or fast-math annotations on the general runtime ABI.
