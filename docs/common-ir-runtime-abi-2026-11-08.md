# Common Scratch IR and runtime ABI — 2026-11-08

## Status: foundational integration, not a compiler rewrite

The native LLVM and WebAssembly JITs have two portable contracts:

1. ir::ScratchCompilationUnit captures the lossless Scratch block graph,
   preserving unsupported effects and reachable custom procedures.
2. ir::Executable is the typed SSA/CFG for a restricted numerical
   subset. Both ir::LlvmCfgBackend and ir::WasmCfgBackend consume the
   identical Executable through ir::lowerExecutable.

This change enforces the same execution contract throughout codegen,
runtime and host calls rather than extending Binaryen-only Scratch opcode
handling.

## Numeric continuation ABI

src/ir/execution_abi.hpp defines the platform-neutral ABI:

    run(uint64_t* stagedSlots, uint64_t* stagedFrame) -> uint64_t
    stagedFrame[0] = resume CFG block (0 at entry; UINT64_MAX on completion)
    stagedFrame[1] = final raw Scratch Value bits
    stagedFrame[2 + SSA ValueId] = SSA value bits
    Completed = 0, Yielded = 1, Failed = 2

Consumers:
- src/ir/cfg_lower_llvm.hpp: LLVM return status and frame operations
- src/ir/cfg_lower_wasm.hpp: WASM return status and linear-memory offsets
- src/ir/scratch_continuation.hpp: staged frame, guard and commit
- src/engine/numeric_thread.hpp: interpreter continuation reconstruction
- src/wasm/host_bridge.cpp: WASM traps map to the same Failed status

Failed slices do not commit speculative Scratch writes. A Yielded slice
commits via ScratchNumericSlots and returns to the owning scheduler at
exactly the saved CFG block. Compiled code never owns live Variable
pointers across the yield.

Synchronous nested warp execution is a distinct ABI, not a resumable CFG:
AtomicWarpExecutionAbi uses 0=Completed, 1=Rollback. Frame storage is:

    [staged Scratch scalar slots][3 helper scratch words]
    [up to 1024 frames of (return PC, 0..2 numeric arguments)]

Those memory-layout constants are shared by the generated WASM, the
host's private frame allocation and the compiled cache. Rollback=1
MUST NOT be interpreted as the numeric CFG's Yielded=1.

## Same captured source across full and hybrid WASM

JitEngine::compile already passes one ScratchCompilationUnit to both
backend interfaces. src/jit/backend_wasm.cpp used to capture Blocks a
second time on hybrid fallback. The fallback now consumes the original
source.script/source.effects, including the captured event hat, top-level
statement chain and stable continuation IDs. Repeated expensive frontend
capture is eliminated, as is the risk of observing mixed source revisions.

## Tests

- xyo-numeric-ir-smoke: numeric and atomic-warp ABI invariants
- xyo-scratch-variable-slots-smoke: staged writes, failed-slice rollback,
  rebind after variable replacement, and scheduler resume state
- xyo-numeric-sequencer-smoke / xyo-numeric-sequencer-auto:
  real native compiled shared CFG with yield checkpoints
- tests/differential/common_cfg_backend_parity.py: 22 cases executing
  the same numeric CFG through native LLVM, builtin WASM and Binaryen
- tests/differential/wasm_return_pc_stack.py: 10 cases verifying the
  distinct synchronous nested-warp multi-return-PC path

Reproduction:

    cmake --build build-list-epoch --target xyo-cli xyo-tests xyo-numeric-ir-smoke xyo-scratch-variable-slots-smoke xyo-ir-cfg-smoke
    ctest --test-dir build-list-epoch --output-on-failure -R 'xyo-(numeric-ir-smoke|scratch-variable-slots-smoke|numeric-sequencer.*|ir-cfg-backends)'
    python3 tests/differential/common_cfg_backend_parity.py --native build-list-epoch/xyo --wasm build-wasm-hotspot-parity/xyo.js --addon build-wasm-hotspot-parity/binaryen-host.node

## Next migration milestones

This is NOT a claim that LLVM codegen.cpp and WASM wasm_compile.cpp now
share complete Scratch instruction lowering. Many semantic operations
remain duplicated and require more shared IR work.

1. Add portable typed CFG nodes for Call, Return, Yield, Guard and Effect
   with stable source Scratch block IDs.
2. Design shared GC-safe host helper ABI for nonnumeric Values, lists,
   broadcasts, drawing, sound and thread spawning. Do not speculatively
   replay observable side effects after deoptimization.
3. Give both backends an explicit scheduler-owned continuation frame:
   return PC, arguments, SSA locals, yield reason, and deopt state.
   LLVM can optimize out frames when nonyielding behavior is proven.
4. Route additional Scratch scripts through this common lowering and
   verification. Keep interpreter fallback for unsupported effects.
5. Remove redundant backend semantics only after parity and performance
   tests establish their equivalence.

Keep the experimental WASM nested-recursion path opt-in and preserve
existing mature LLVM fast paths while moving semantics to the common IR.

## November 8 follow-up: shared Call / Return / Yield / Effect graph

Created src/ir/scratch_transfer.hpp as a backend-neutral, lossless
Scratch control-transfer manifest. The same ScratchCompilationUnit now
contains script, effects, transfers and a capture-time procedure
resolution table. Each site preserves its source Scratch block ID and
following block ID, and, if resolvable, the procedure definition ID.

- Call: explicitly records the call site and its continuation.
- ProcedureEntry/Return: records definition and a semantic return site.
  This return marker is not yet an executable arbitrary-return stack.
- Yield: marks Scratch scheduler checkpoints.
- Effect: preserves unknown extension opcodes and observable operations
  that a pure numeric CFG cannot silently omit.

Undefined procedures are FIRST-CLASS unresolved Call sites with no
callee definition. A previously introduced overly strict validation
caused a native JIT e2e regression, because undefined Scratch calls may
execute without aborting the surrounding script. This is fixed:
capture stays valid; the numeric CFG rejects the unresolved site, and
the native general JIT/interpreter retain Scratch's behavior.

Actual LLVM / Binaryen reuse now extends beyond metadata. The
common numeric frontend, src/ir/scratch_f64_kernel.hpp, recognizes a
verified zero-to-two-argument nonrecursive warp procedure whose body
consists of scalar numeric set/change statements and, recursively,
calls to other similarly verified warp helpers. Such procedure bodies
are inlined ONCE into the common SSA/CFG, not separately translated
from Scratch in both backends. Each numeric argument is evaluated at
the call site and bound as an immutable SSA value before the callee
can mutate caller-visible Scratch variables. An active procedure set
prevents recursive expansion. The entire lowering is transactional.

The following are explicitly rejected by this common numerical path:
- Nonwarp procedures, recursive procedures, 3+ arguments.
- Heap strings and nonnumerical argument expressions.
- Lists, drawing, audio, unknown extensions and other side effects.
- Loops inside a callee (warp still has a scheduler deadline).
- Repeated static calls whose distinct inlining requires re-visiting
  the same source body; conservative fallback for now.

The host scheduler attachment guard re-captures the same
ScratchCompilationUnit and compares the exact executable CFG so stale
compiled code cannot attach after source edits.

Verification:
- tests/platform/scratch_ir_smoke.cpp: real Blocks-built transfer
  graph with Call/Return/Yield/Effect, source stability, invalid
  return-target rejection, and a preserved undefined call.
- tests/differential/common_cfg_procedure_parity.py:
  11 cases comparing interpreter, native LLVM common CFG, builtin
  WebAssembly common CFG, and optimized host Binaryen common CFG.
  Five supported cases require actual compiled common-CFG execution;
  six unsupported cases require fallback and exact variable equality.
- Existing tests/differential/common_cfg_backend_parity.py:
  22 real LLVM/Builtin/Binaryen shared-CFG test cases.
- Existing 10-case WASM nested two-return-PC suite and 2123-check
  headless ScratchBench output parity suite guard older code paths.

To execute the new real common-IR call path, set
XYO_JIT_COMMON_CFG=1 together with the usual JIT mode. The setting
remains opt-in. The complete general Scratch JIT, LLVM Fiber and
Binaryen hybrid paths remain available as fallback. This is the
first incremental migration of procedure code generation into the
same backend-neutral executable IR; general recursive or yieldable
procedures are NOT yet compiled in that path.

## Agent IR-001: portable scheduler-owned CallFrame

The new CallFrameAbi defines a fixed-width numeric procedure frame:

    word 0: deterministic source-snapshot procedure ID (0 is root)
    word 1: return PC (UINT64_MAX for root)
    word 2: resume PC
    word 3: argument count
    word 4: SSA-local count
    words 5+: arguments followed by SSA-local values

ScratchCallStack is a host-owned, bounded-depth array of 64-bit words.
Each push/pop records and restores a return address, raw numeric arguments
and SSA-local values. ScratchCallTransaction stages a speculative slice,
rejects invalid frame headers and stale generations, and publishes only
after host validation. GC values still require separately rooted handles.

The experimental Binaryen nested warp multi-return-PC emitter now writes
the same CallFrameAbi header and argument payload, and the host allocates
using the matching shared stride. Its atomic rollback status remains
different from a yieldable numeric continuation status.

The native C++ and Emscripten-WASM builds of call_frame_abi_smoke.cpp have
identical raw frame digests. A Python parity checker compares their outputs.
The real 10-case WASM non-tail recursion tests and the existing 22-case
LLVM/Binaryen common-CFG tests also pass after this layout migration.

General LLVM procedure JIT and nonwarp yieldable/recursive execution do NOT
yet consume ScratchCallStack. This is a scheduler-owned ABI milestone,
not completion of IR-001. The remaining work is to integrate this
continuation into both JIT execution paths with exact deopt/GC semantics.
