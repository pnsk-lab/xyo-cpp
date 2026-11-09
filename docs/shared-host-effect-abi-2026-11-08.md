# Shared host-effect ABI (IR-002, 2026-11-08)

## Goal and boundaries

LLVM's native JIT and Binaryen-optimized WebAssembly now call the SAME
C++ host semantic entry point, `ir::invokeHostEffect`, for six value-level
operations: Scratch string length, tagged comparison, join, letter-of,
contains, and Scratch boolean coercion.

This is an incremental integration, **not** a complete general Scratch
effectful CFG. Observable actions such as drawing, broadcasts, clones,
audio and sensing still run through their existing interpreter/JIT contracts.

## ABI

`src/ir/host_effect_abi.hpp` declares:
- `HostEffectOp` with stable opcode IDs compatible with the Emscripten
  `env.h` helper discriminator (16, 17, 18, 19, 20, 22).
- `invokeHostEffect(op, lhsBits, rhsBits, compareMode, resultBits)`.
  Each input and result is exactly one raw, tagged 64-bit Scratch Value.
  No runtime `Variable*`, `SStr*`, `Thread*` or backend-owned native
  pointer is stored in the compiled code or returned by the ABI itself.
- `describeHostEffect`: argument arity, may-allocation and
  transactional/commit-only/unsupported policy.

The native entrypoints `xyo_join`, `xyo_letterof`, `xyo_strlen`,
`xyo_strcontains` now delegate to this API. The WASM helper `xyo_wasm_h`
also delegates to it, converting the tagged boolean/number results back
to historical f64 scratch cells when required by WASM's already-shipped ABI.

**GC ownership:** Passing a raw Value containing a heap-string pointer is
legal only during a host invocation with the source rooted; the common ABI
is a borrowed-value contract, **not** a long-lived heap handle. The
Emscripten HotSpot caller still stages slot values and defers collection
for speculative string operations; it commits to rooted variables only
after success and discards speculative results on failure. A future
general effectful IR must implement persistent GC-root/handle tables
for arbitrary suspendable frames before enabling new effectful opcodes.

## Replay barriers

`src/ir/scratch_effect.hpp` now adds `Observable` and `MaySpawn`
metadata to effects. Broadcasting, cloning, sound, pen, motion, looks,
sensing, data monitors, random, and known external extension prefixes
cannot be replayed as part of an optimistic retry.

`mayReplaySpeculatively` rejects any unknown opcode, scheduler yield,
procedure call, observable operation or spawn. This is only an eligibility
test; compilation still requires a backend-specific complete lowering
and transactional storage checks. Unknown extensions remain opaque.

## Verification

- Native and Emscripten xyo-cli builds: passed.
- Standalone `tests/platform/host_effect_abi_smoke.cpp` (compiled against
  `build-list-epoch/libxyo.a`): passed, including UTF-16 lengths, Unicode
  joins, numeric/string comparisons, Boolean coercion and invalid IDs.
- `wasm_string_semantics.py`: 8/8 four-way Interpreter/Builtin/Binaryen/LLVM
  parity, including Unicode, tagged comparison and rollback.
- `common_cfg_backend_parity.py`: 26/26 four-way parity.
- `wasm_scratchbench_backend_parity.py`: 2123/2123 output equality across
  interpreter, built-in WASM and Binaryen (1899/2123 successful checks in
  the headless environment for **each** backend; 94 JIT scripts compiled
  with both WASM engines).

Reproducibility logs: `board/ir002-*.log`; ScratchBench summary
`scratch-bench/results/ir002-scratchbench.json`.

## Remaining work

1. General owned, persistent GC-root handles for yieldable SSA and
   CallFrame values; no pointer may survive a scheduler yield without
   being rooted.
2. Explicit commit-once effect journal / continuation protocol for actual
   externally observable actions (draw, audio, events, broadcasts, clones).
3. LLVM and Binaryen common executable effect nodes rather than the
   still-separate general-purpose lowering paths.
4. Full parity with renderer/sound enabled. The headless 224 failures
   also occur in the interpreter and are not evidence of Binaryen issues.

Do not activate new observable opcodes on the numeric CFG before these
conditions are met.
