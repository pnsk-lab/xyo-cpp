# IR-002 continuation: rooted Scratch Values and commit-once effects

Date: 2026-11-08

## Implemented modules

1. `src/ir/gc_value_handles.hpp`: `GcValueHandles` registers an exact root
   provider in `Gc` and owns 64-bit, **globally nonreused** opaque tokens.
   Root registration, lookup, replacement and release are host operations.
   A JIT continuation stores only `RootedValueHandle::token`; it does not
   retain an untraced pointer into a GC-managed UTF-16 `SStr`.
   `ScopedValueHandle` is a move-only lexical lease.
2. `src/ir/gc_call_frame_values.hpp`: `RootedCallFrameValues` manages
   heap-string roots for nested/nonwarp CallFrame arguments with push, pop,
   safe update, explicit clear-on-deopt, and post-yield resolution.
   Immediate numbers/booleans/colors remain raw 64-bit bits. Heap strings
   are stored as opaque handles **only** in this sidecar. The owner must
   keep this sidecar aligned with scheduler-owned `ScratchCallStack` depth.
3. `src/ir/host_effect_abi.hpp`: added
   `invokeRootedHostEffect(roots, op, lhsHandle, rhsHandle, mode)`.
   Exact Scratch semantics remain delegated to `invokeHostEffect`, and
   the returned boxed Value is immediately re-rooted. Invalid/stale
   handles or unsupported operations fail without publishing.
4. `src/ir/commit_once_effects.hpp`: `CommitOnceEffects` stages a bounded
   number of observable effects as root-owned messages. `Slice::commit`
   validates scheduler revision, monotonic slice ID and host state
   publication before transferring queued messages. A failed transaction
   drops all roots and produces **zero** actions. `drain` removes each
   message before calling the non-yielding `noexcept` dispatcher,
   preventing duplicate/reentrant delivery. A repeated/stale slice cannot
   commit.

## Scope and safety limits

- The GC is nonmoving but can sweep unrooted objects. Handles are always
  dereferenced by the host (never by generated machine code), and
  registration/removal/collection must be scheduler-thread confined.
- The Scratch boxed `Value` format contains **SStr** references but not
  `SList` references. Lists stay rooted through their `Target::Variable`
  or existing list transaction bindings; the Value handle table is not a
  replacement for list ownership.
- Once `resolve` returns a borrowed Value, the caller may use it only
  until the next yield or allocation boundary unless it takes its own root.
- IR-001 has now connected `RootedCallFrameValues` to the **opt-in**
  LLVM `NativeProcedureFrameSession`: heap-string arguments are rooted
  during enter/leave and checked across real Fiber yields. Interpreter,
  ordinary LLVM JIT and mirrored LLVM JIT have identical outputs for
  10/10 cases, including `string_recursive` with 36 calls/returns,
  96 yields and depth 4. This remains a **Fiber-backed mirror**, not a
  standalone scheduler-driven reconstitution of arbitrary LLVM SSA.
- IR-002 also supplies `RootedScratchCallStack`, a composite wrapper
  guaranteeing lockstep frame/root push and pop. For all tagged
  nonnumeric arguments it stores a deliberately unusable placeholder in
  the numeric ScratchCallStack arena; only the rooted sidecar retains the
  Scratch Value. A 36-level recursive native/WASM test force-collects the
  GC on every level and checks PC, rollback and all argument values.
- `CommitOnceEffects` is opt-in on selected **host-side** primitives:
  non-waiting broadcast, broadcast-and-wait initial receiver launch,
  looks say/think bubble updates, pen stamps, sound starts and clone
  creation. Other renderer/sound/clone actions and speculative CFG effect
  lowering remain on their preexisting paths. A general scheduler
  continuation transaction still requires RT-001's atomic state/PC
  publication at a quiescent point; the dispatcher cannot yield or fail.
- Exactly-once refers to one in-process committed scheduler slice.
  This is not crash-persistent or a distributed exactly-once guarantee.

## Validation

Manual native test builds link against the already-built native
`build-list-epoch/libxyo.a` and Emscripten test builds link against
`build-wasm-hotspot-parity/libxyo.a`.

- `tests/platform/gc_value_handles_smoke.cpp`:
  native **PASS**, Emscripten Node **PASS**. It force-collects live heap
  strings, checks Unicode and UTF-16 lengths, validates nonreused/stale
  tokens, and simulates 12 recursive frames holding string/boolean/numeric
  arguments across repeated collections and yield boundaries.
- `tests/platform/commit_once_effects_smoke.cpp`:
  native **PASS**, Emscripten Node **PASS**. It checks discard and failed
  state commit, simultaneous speculative snapshots, stale revision,
  replayed slice IDs, ordered delivery, payload GC rooting through a
  second forced collection during callback, no duplicate delivery,
  and nested commit/drain attempts that cannot reorder events.
- `tests/platform/rooted_call_stack_smoke.cpp`: 36 recursive
  frame transitions with real `ScratchCallStack` and GC roots,
  forced GC and exact source PC restoration: native/WASM both PASS.
- `tests/platform/effect_callstack_transaction_smoke.cpp`: actual
  `ScratchCallTransaction` commit bridges to `CommitOnceEffects`;
  stale continuation publication emits zero events while the successful
  state/PC is visible before dispatch: native/WASM both PASS.
- `tests/differential/wasm_effect_fallback_parity.py`: broadcast,
  broadcast-and-wait, unsupported wait, looks and an unknown extension: 5/5
  Interpreter/LLVM/Builtin/Binaryen output parity; broadcast receiver
  ran exactly once (x=3, y=13).
- All five host effect / GC / journal / CallFrame smoke binaries
  are registered in `tests/CMakeLists.txt` and native CTest 5/5 PASSES
  in 0.08s.
- Final rebuilt toolchains: shared CFG 33/33 across
  Interpreter/LLVM/Builtin/Binaryen; ScratchBench 2123/2123 output
  equality with real Binaryen optimization of 94 scripts (all engines
  have identical 1899/2123 successful headless checks).
- Earlier IR-002 integration (hosted string operations) had 8/8
  Interpreter/LLVM/Builtin/Binaryen four-way string parity, 26/26
  shared CFG parity, and 2123/2123 ScratchBench output equality.

Test logs: `board/ir002-wasm-roots-build.log`,
`board/ir002-wasm-roots-run.log`, `board/ir002-wasm-effects-build.log`.

## Next integration

IR-001 owns the active LLVM `NativeProcedureFrameSession` implementation;
its opt-in rooted mirror is now validated. RT-001 owns the independent
scheduler continuation state/PC protocol and
can adopt `CommitOnceEffects` after an ordered host-state publication
boundary exists. Both must retain the interpreter fallback for unsupported
effectful or unrooted operations. Avoid changing the default JIT path until
the end-to-end tests exercise actual nonwarp GC strings over multiple yields.

## Opt-in live Runtime broadcast integration

`XYO_EFFECT_JOURNAL=1` now activates the same `CommitOnceEffects` queue
for the native LLVM JIT's **non-waiting** `xyo_broadcast_v` host call and
the interpreter's `event_broadcast` primitive. The interpreter callback
retains its original `startHats` route and the native JIT callback retains
the optimized `Runtime::startBroadcastImmediate` route; both use the
host-side `Runtime::journalBroadcast` commit boundary. Pure numeric
Binaryen regions still execute in WASM, while unsupported broadcast opcodes
bridge to the original interpreter primitive and thus journal there.

The default remains OFF: `Runtime::startBroadcast` directly calls the
original fast method without callback indirection, and the interpreter uses
its original `startHats` branch. When opted in,
`Runtime::startBroadcastAndWait` journals **only the initial startHats
launch** and returns the exact started `Thread*` list. The normal
interpreter `ExecContext::hasStartedThreads` and LLVM Fiber
`joinRefs` keep the receiver references stable across every yield.
Completion, waiting and resumption themselves are not moved into the
journal.

GC collection is deferred only until the name has entered the exact
root table; the callbacks may then allocate and collect freely. While a
committed broadcast is being dispatched, a synchronous/reentrant nested
broadcast executes on the original immediate path to preserve event order.
A separate runtime C++ test forces GC from both callback levels.

`XYO_EFFECT_JOURNAL_STATS=1` emits
`[effect-journal] committed_broadcasts=... immediate_broadcasts=...
pending=... rooted=...` at Runtime destruction. The two committed events
below are measured dispatches, not inferred from final variable values.

**Validated after build**:

- `tests/differential/runtime_broadcast_journal.py`: native interpreter,
  native LLVM, native interpreter+journal, native LLVM+journal, WASM builtin
  +journal and Binaryen +journal all result in
  `events=11, done=1`. Each opt-in path records exactly
  `committed_broadcasts=2`, `pending=0`, `rooted=0`.
  The Binaryen variant uses real `emitter=binaryen-host` compiled regions.
- `tests/platform/runtime_broadcast_journal_smoke.cpp`: nested callback
  invokes a second broadcast synchronously and forces GC between dispatches;
  FIFO semantics, one committed outer event, one immediate inner event,
  zero remaining roots.
- `tests/differential/wasm_effect_fallback_parity.py`: a completely unknown
  `extension_private_unknown_side_effect` opcode is explicitly retained
  as an interpreter-only hybrid region, not deleted from the source graph.
  Full four-way output parity also covers broadcast, broadcast-and-wait,
  control_wait and looks_say.
- Native CTest now includes five original ABI smokes, two real
  Runtime smoke tests and five Scratch project parities: **12/12 PASS**.
- With journal enabled, the shared CFG remains 33/33 and ScratchBench
  still shows 2123/2123 exact output equality against the interpreter
  (1899/2123 environment-passing checks for each engine; Binaryen
  compiles 94 scripts).

**Remaining**: the host journal is **not** a general speculative,
yield-spanning JIT transaction. Pen lines/points and other renderer
operations, sound playback completion/wait-resolution, clone disposal and
arbitrary reentrant effects still use their historical pathways.
Even though broadcast-and-wait's receiver launch is committed exactly
once at its synchronous host boundary, arbitrary Fiber-independent
yield/deopt recovery is not provided. Queue delivery is in-process
at-most-once for successfully published slices, not crash persistent.

## Opt-in looks, pen, sound and clone Runtime paths

All of these use `Runtime::journalObservable(op, exact Value payload,
callback, context)` at a **synchronous host boundary**. The queue
GC-roots each tagged payload and commits it before invoking the
original non-yielding primitive. Reentrant callbacks execute immediately
to preserve Scratch's depth-first observable ordering. Ordinary execution
with `XYO_EFFECT_JOURNAL` unset still calls the original primitive;
the compiled WASM numeric regions remain transactionally isolated and
unsupported effect opcodes use interpreter-only hybrid regions.

- `looks_say`, `looks_think` and `looks_sayforsecs`: one shared
  `updateBubble` route for Interpreter and native LLVM, using
  `ObservableOp::Say` and an exactly rooted Unicode Scratch `Value`.
  The original timer / promise path for timed bubbles is unchanged.
- `pen_stamp`: both engines reach `penStampCore` and
  `ObservableOp::PenStamp`; the actual `RendererAPI::penStamp`
  callback runs only after commit. Other pen operations are not
  journaled.
- `sound_play`, `sound_playuntildone`: shared
  `startSoundPlayback` publishes `ObservableOp::SoundStart`, then
  calls `SoundBankAPI::playSound` or `playSoundAndWait`. The
  original `Thread` promise/wait and asynchronous audio completion
  remain intact and are **not** part of the journal.
- `control_create_clone_of`: shared `createCloneForTarget` publishes
  `ObservableOp::CreateClone`, then calls the unmodified
  `makeClone` / `startHats(control_start_as_clone)` path and adds
  the clone to the Runtime. Limit-exhausted requests do not commit an
  event. No clone pointer is stored in a persistent queue.
- `event_broadcastandwait`: the initial `startHats` uses the same
  GC-rooted broadcast name and returns its exact `Thread*` list.
  The sender's continuation waits using the original scheduler/Fiber.

### Actual results

- `runtime_broadcast_wait_journal.py`: two sequential
  broadcast-and-wait commands with two receivers each; receivers
  yield for 0.03/0.05 s. The sender checkpoints `checkpoint=1111`
  before the second broadcast; final `events=2222, done=1`.
  Exactly 2 broadcast starts in all Interpreter/LLVM/Builtin/Binaryen
  opt-in runs, with zero remaining queued events and roots.
- `runtime_looks_journal.py`: 3 timed/untimed say/think operations
  with Unicode text; exactly 3 committed host effects with no replay,
  Interpreter/LLVM/Builtin/Binaryen all agree.
- `runtime_pen_journal.py`: two pen stamps separated by numeric
  operations; 2 effects, 4-way output parity and a real Binaryen
  hybrid interpreter fallback around the stamps.
- `runtime_clone_journal.py`: three actual Scratch clones executing
  their `control_start_as_clone` hats; shared stage `count=3`,
  exactly 3 committed clone creation effects, 4-way parity.
- `runtime_observable_journal_smoke.cpp`: actual
  `RendererAPI::penStamp` twice, `Target::setSay/onSay` twice,
  mock `SoundBankAPI` nonwaiting and waiting starts once each, one
  real `Target::makeClone` plus clone-limit rejection. Fake callbacks
  force GC, verify Unicode and check zero GC-root/queue leaks.
- Native `ctest` selection for IR-002: **12/12 PASS**.
  Fresh native/wasm builds PASS. Shared CFG 33/33 and
  ScratchBench 2123/2123 output equality (94 Binaryen-compiled
  scripts, the same 1899/2123 successful headless checks for all
  engines) under `XYO_EFFECT_JOURNAL=1`.

Logs are located under `board/ir002-broadcastwait-*.log`,
`board/ir002-looks-*.log`, `board/ir002-pen-*.log`,
`board/ir002-sound-*.log`, `board/ir002-clone-*.log`
and `board/ir002-journal-all-effects-*.log`.

### Reproducibility and outstanding intermittent failure

- Native and WebAssembly release builds succeeded. The same
  `runtime_observable_journal_smoke.cpp` was also manually compiled with
  Emscripten and executed under Node, verifying real bubble, renderer,
  audio, clone callbacks and GC: **PASS on both targets**.
- With `XYO_EFFECT_JOURNAL=1`, the latest shared procedure/CFG
  parity harness passed **55/55** cases across Interpreter/LLVM/Builtin
  WASM/Binaryen, including nonwarp argumented procedures and deopt.
  The concurrently evolving baseline suite also passed **51/51** cases
  with journal disabled. The IR-001/RT-001 agents are extending this
  harness on the shared worktree, so snapshots of its case count differ.
  Unknown-extension fallback and other host effects pass 5/5.
- **Unresolved intermittent native failure:** an earlier full
  procedure-parity invocation exited in `nonwarp_loop` after the
  LLVM shared CFG reported completion, with `free(): invalid pointer`.
  Independent repeats of this exact case with journal disabled
  (30 runs) and enabled (30 runs) all succeeded, as did the subsequent
  entire subsequent 47- and 55-case harnesses. Do not assume that 60
  successful repetitions
  establish the absence of a native heap bug. Preserve the log
  `board/ir002-journal-all-effects-procedures.log`; repeated logs
  and successful replay are under `board/ir002-repro-*.log`,
  `board/ir002-journal-all-effects-procedures-retry.log`,
  `board/ir002-journal-off-procedures-control.log` and
  `board/ir002-journal-on-procedures-current.log`.
  The cause is not determined, so IR-002 remains in_progress.

## Opaque effects: dual source checkpoints and reentrant GC

The common `ScratchTransferGraph::lower` now preserves **both**
`ScratchTransferKind::Effect` and `ScratchTransferKind::Yield`
for an opaque Scratch opcode whose effect mask includes `MayYield`.
Previously the opaque branch emitted only `Effect`, silently losing
the potential yield continuation even though the frontend correctly
classified it as unknown. This does not make opaque code executable in
LLVM/Binaryen: it makes the semantic manifest conservatively complete
for a future scheduler to determine where an interpreter fallback or
yield resume is necessary. Known `control_wait` still emits just
the explicit `Yield` site.

`tests/platform/scratch_ir_smoke.cpp` now checks 10 unknown/externally
observable opcode types: generic statement/reporter extensions,
looks bubble operations, pen stamp, broadcast/broadcast-and-wait,
sound play-until-done, and clone creation. Every opaque opcode must
have both site types and must be rejected by
`lowerCapturedScript`, `lowerCapturedStatement`, and
`lowerCapturedRange`, with no partial WASM `Program` publication.

`tests/differential/wasm_effect_fallback_parity.py` now additionally
puts opaque statements inside `control_if` and `control_repeat`,
and an unknown reporter in an arithmetic input. All **8/8** test
projects match Interpreter, native LLVM, builtin WASM and real Binaryen
hybrid execution. This is a tested set, not an exhaustive proof that
arbitrary third-party future extension semantics are supported.

A separate runtime safety correction covers **nested** host effects:
`Runtime::journalBroadcast` now copies reentrant borrowed UTF-16
name views before invoking a GC-capable receiver callback.
`Runtime::journalObservable` roots reentrant tagged Scratch Values in
a temporary exact `GcValueHandles` table instead of preventing all GC
while recursively invoking arbitrary observable callbacks. It retains
the original fail-safe `Gc::DeferCollect` fallback only if a temporary
root cannot be obtained. `runtime_broadcast_journal_smoke.cpp`
passes a borrowed GC-managed SStr name and a nested Unicode Value,
forces collection from both callbacks, and checks ordering and zero
GC-root leaks. Native and Emscripten/Node versions both PASS.

Validation: native `scratch_ir_smoke` and related runtime CTests
**13/13 PASS**; on the journal-enabled full ScratchBench the
Interpreter/builtin/Binaryen outputs match on all 2123 checks
(94 real compiled Binaryen scripts), and shared CFG parity is 33/33.
A separate earlier intermittent `free(): invalid pointer` during
native procedure parity is still **unresolved**, not silently
treated as fixed by these unrelated changes.
