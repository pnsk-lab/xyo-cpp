# Native LLVM JIT procedure CallFrame bridge (2026-11-08)

Task: IR-001. Owner: agent-common-ir.

## Scope and implementation

LLVM native procedures previously used only the Fiber call stack for
arguments, return PCs, and instruction-level resumption. The portable
CallFrameAbi and scheduler-owned ScratchCallStack were tested in isolation
and used by the experimental WASM multi-PC warp recursion.

An opt-in LLVM bridge now records the actual logical procedure stack in
the same 64-bit fixed-layout CallFrameAbi, with native Fiber execution
remaining authoritative:

- src/jit/procedure_frame_state.hpp captures one immutable
  ScratchCompilationUnit, resolves deterministic procedure IDs from
  ScratchTransferGraph, and maps captured callsite IDs to stable numeric
  caller continuation PCs.
- src/jit/codegen.cpp emits xyo_proc_frame_enter before the existing
  xyo_proc_push helper (which itself may yield in nonwarp recursion).
  Real register arguments are copied into a buffer allocated once in
  the LLVM function entry block, not repeatedly inside Scratch loops.
  xyo_proc_frame_leave follows the usual xyo_proc_pop. This disables
  the optional interpreter-frame elision only when explicitly enabled.
- src/jit/rt.cpp stores/returns the actual argument bits and return PCs
  in the scheduler-owned per-thread ScratchCallStack. It validates stack
  depth and revision around real xyo_yield Fiber switches.
- src/engine/thread.hpp/.cpp holds the optional session across yields,
  clears it at retirement, and optionally prints diagnostic counters.

Configuration:

    XYO_JIT_CALLFRAME=1 XYO_JIT_CALLFRAME_STATS=1 \
      ./build-list-epoch/xyo run project.sb3 --jit eager

A native JIT thread reports the actual call/return/yield counts and
max depth at retirement with the prefix [jit-callframe].

## Value safety

Scratch math_number can initially be represented by a string Value,
even if the literal looks numeric. The numeric CallFrame arena therefore
stores a pointer-free sentinel for heap-string arguments; full typed
Values are retained by GC-rooted sidecars. Numeric, Boolean, color,
undefined and empty-string immediates remain bit-exact. Unknown future
tags fail closed rather than being misclassified.

The test suite includes actual numeric arguments from operator_add and
explicit heap strings (including strings surviving recursive yields).
See the GC-rooted Value arguments section below.

## What remains unfinished

This is a logical per-procedure CallFrame for real LLVM execution.
The machine instruction PC and SSA register state still live in the
native Fiber. The stored resume PC is the procedure entry or caller
continuation, not the exact instruction after a midprocedure yield.
This frame alone CANNOT resume arbitrary LLVM fibers or deopt recursive
stacks. Full scheduler-owned yield/deopt state belongs to RT-001.

Do not enable by default until the scheduler can use this CallFrame
without relying on the native Fiber to preserve the machine PC, and
until remaining GC-bearing runtime data and observable host effects are handled.

## Differential regression

    python3 tests/differential/native_callframe_mirror.py \
      --native build-list-epoch/xyo \
      --out scratch-bench/results/agent-native-callframe-fixtures

    python3 tests/differential/native_callframe_fuzz.py \
      --native build-list-epoch/xyo --seeds 16 \
      --out scratch-bench/results/agent-native-callframe-fuzz-fixtures

    ctest --test-dir build-list-epoch --output-on-failure \
      -R '^(xyo-tests|xyo-call-frame-abi-smoke|xyo-numeric-ir-smoke|xyo-numeric-sequencer-smoke|xyo-numeric-sequencer-auto|xyo-ir-cfg-backends)$'

Tests compare Interpreter / normal native LLVM / mirror native LLVM.
The deterministic 7/7 tests cover one/two numeric args, nested calls,
dynamic arguments, nonwarp recursion with real scheduler yields, and
non-numeric heap-Value fallback. A 16-seed randomized suite checks
warp/nonwarp recursion, 1-2 numeric arguments and depth up to 7.

Observed: 7/7 deterministic, 16/16 randomized, native CTest 6/6.
Nonwarp recursive case: 48 calls, 48 returns, 36 Fiber yields, depth 5.
All result values match Interpreter and the ordinary native LLVM path.

Separate existing full ScratchBench (2,123/2,123 exact output checks),
actual WASM multi-return-PC (10/10), and the expanded 28-case LLVM/Binaryen
common-CFG test pass and protect the other backends. See scratch-bench/results/agent-callframe-*
and /agent-native-callframe-* logs for complete command output.

## Parallel coordination

IR-001 owns CallFrame layout and the LLVM mirror. IR-002 owns GC-safe
Value/Effect semantics; RT-001 owns the true resumable scheduler frame.
Coordination posts have been sent to both task threads. No commit/push.

## 2026-11-08 follow-up: exact loop-yield checkpoints

The first native bridge had a checkpoint blind spot: the regular
nonwarp LLVM loop used xyo_yield_fast, bypassing xyo_yield, so the
scheduler CallFrame failed to observe those real Fiber suspensions.

In opt-in CallFrame mode ONLY, codegen now routes this fast branch through
the usual xyo_yield helper. With the flag disabled the optimized fast
native Fiber-switch path remains untouched. We also exclude AOT codegen
to avoid importing an unsupported runtime bridge into AOT outputs.

The LLVM loop-lowering stack now records the corresponding original
Scratch loop block ID for the active lexical loop. Immediately before a
potential scheduler handoff, it writes a stable source-level resume PC
to the same Thread-owned CallFrame. This is the loop checkpoint that the
native Fiber will resume executing, NOT an instruction-level resume PC.

For ordinary counted repeat loops, LLVM also loads the exact per-loop
private remaining counter before its usual post-yield decrement and
passes the raw IEEE double bits to the CallFrame local storage.
The captured Scratch source is sorted by block ID to allocate a
separate local slot per lexical control loop, so a nested loop never
overwrites the outer loop's saved counter. An uncounted/specialized
loop stores the kNoLoopCounter marker. There is a conservative limit
of 32 distinct captured control loops: larger scripts retain their
native JIT behavior but disable this experimental mirror.

This is a source-level observability and future deoptimization building
block, NOT enough to resume a machine-independent arbitrary LLVM
procedure: other SSA locals, a complete active loop nesting stack,
generalized GC-bearing continuations, and exact side-effect positions still
require explicit scheduler-owned checkpoints in RT-001/IR-002.

Additional verification:

- Native fixed parity 8/8, including nonwarp loops and double-nested
  numeric repeats with Interpreter/LLVM/mirrored LLVM exact outputs.
- Nonwarp 3-repeat fixture: 36/36 actual Fiber yields and 36 saved
  loop-counter values, integer counter sum 72 (3+2+1 over 12 calls).
- Double-nested 2x3 fixture: 96/96 actual yields and snapshots, sum 180,
  with two independently populated lexical loop slots.
- Seeded recursive Warp/nonwarp suite 32/32 with one or two arguments.
- Repeated callsite 50,000 times plus enclosing call: 50,001 pushes and
  50,001 pops, final output equal to uninstrumented native LLVM.
- Native selected unit/CFG/scheduler CTest suite remains a regression
  requirement; see scratch-bench/results/agent-ir001-loopslots-* logs.

Flag: XYO_JIT_CALLFRAME=1 and optionally XYO_JIT_CALLFRAME_STATS=1.
Host logs additionally include checkpoints, last_pc, counters,
counter_sum and max_counter_slots for deterministic CI assertions.

## 2026-11-08: GC-rooted Value arguments (IR-001 + IR-002 integration)

The opt-in LLVM CallFrame bridge now stores procedure arguments in two
synchronized, scheduler-owned structures:

- ScratchCallStack: portable header, return/resume PCs, loop locals,
  and immediate argument bits. Heap-backed string argument slots contain
  val::kUndefined rather than any GC pointer.
- RootedCallFrameValues: one sidecar frame for each non-root procedure
  frame, retaining complete typed Values (including heap strings) via
  GcValueHandles, registered with the global collector.

NativeProcedureFrameSession::argument(frameIndex, argIndex) resolves an
argument from the rooted sidecar (frameIndex == 0 is the root frame).
CallFrame push retains all string handles before publishing the procedure
frame; pop releases them only after the corresponding frame is removed.
Before/after each real Fiber yield, both frame stacks must agree on depth.
On validation failure the optional mirror fails closed; outstanding roots
remain owned by the session until destruction, avoiding premature release
while a native Fiber may still be suspended.

The default JIT path is unchanged (XYO_JIT_CALLFRAME=0). This is an
*observability mirror*, not native execution from the portable frame;
instruction-level resume, SSA reconstruction, non-string host objects
(list identities), and all cross-backend side effects remain separate work.
The GC-handle implementation is explicitly scheduler-safe-point/thread-
confined; concurrent GC/provider mutation must not occur.

native_callframe_mirror.py now checks 10 deterministic cases covering
numeric and heap-string arguments, multiple return sites, nonwarp nested
yields, and recursively passing an existing heap string. For each program
Interpreter, unmodified LLVM, and LLVM+mirror final state must match,
the observer must stay enabled, and all rooted handles/sidecar frames
must be released at thread completion. peak_heap_roots is a positive check
that heap strings were actually retained. The dedicated IR-002
gc_value_handles_smoke.cpp forces GC while the only ownership of live
strings is the exact rooted handle provider.

## Source loop nesting and pointer-free resume snapshot (IR-001 follow-up)

A nested nonwarp loop previously checkpointed only its innermost
source loop ID and private repeat counter. The outer repeat counters
remained stale until their own iteration yielded; such an incomplete
snapshot cannot reconstruct the precise loop nesting after a Fiber exit.

The opt-in LLVM emitter now captures all active lexical loop scopes,
outermost first, at each actual scheduler handoff. It allocates a small
pair of arrays in the LLVM function entry block (never alloca on every
iteration) and submits a single checkpoint_path host call.

A procedure CallFrame reserves 2*N+1 local words for N distinct source
loop IDs (N<=32):
  - [0,N) individual counters, or kNoLoopCounter;
  - [N,2*N) ordered source loop PCs (0 denotes an unused entry);
  - [2*N] current active loop depth (raw uint64).

Every checkpoint validates the complete candidate path, clears stale
loop counters/path entries, and atomically publishes the source resume
PC and all active loop counters *before* the Fiber yields. The optional
sourceCheckpoint() accessor exposes the pointer-free procedure header,
active loop path, and counter bits. It is validated at actual native
yield boundaries. This state is suitable as input to future scheduler
deoptimization, but still does not contain all arbitrary LLVM SSA
temporaries, branch-specific effects, or an executable machine PC.

Validation: Interpreter/ordinary LLVM/opt-in LLVM parity now includes
two-level (96 yields, 72 parent counter samples, sum 108) and three-level
nested repeats (168 yields, 240 parent counter samples, sum 360). The
3-level fixture confirms that all three exact counters are present at
inner yields; parent and inner values never alias. The existing
recursion, string GC handles, and default LLVM path are unchanged.

## Full procedure-stack export and recursive caller loop checkpoints

An opt-in native frame can now export its entire active procedure chain
into a separate RootedScratchCallStack via
NativeProcedureFrameSession::exportContinuation().

The export copies, in root-to-leaf order, every procedure ID, caller
return PC, source resume PC, argument arity, and the exact numeric
CallFrame locals (including all published source loop paths). Values
are resolved from the original rooted sidecar, then retained again in
the exported stack's independent GC root provider. No heap pointer is
copied into the portable numeric words. The resulting object owns its
roots; the source frame may later return or be destroyed without
invalidating the exported argument values.

Export fails closed if the original CallFrame image is invalid, root
sidecar depth is inconsistent, a child's return PC does not match the
parent's committed source continuation, an argument bit pattern does
not match the root-protected typed Value, or any copy operation fails.
No live mutable CallFrame arena is passed to external JIT code.

Before an LLVM procedure call inside a lexical loop, the caller now
checkpoints its own full loop path *before* pushing the callee frame.
Otherwise an inner recursive yield could preserve a correct callee
counter but stale parent loop counters. Recursive procedures therefore
capture independently nested instances of the same source loop.

The XYO_JIT_CALLFRAME_EXPORT_PROBE=1 test option clones and checks
the entire frame image at each actual native yield and compares it
again after Fiber resumption; this probe is not used in the default
execution path. It reports exports, max_export_roots,
max_export_depth, and ancestor_loop_frames. The default JIT still
has no new helper calls or frame allocations.

Validation of the opt-in probe:
- native_callframe_mirror.py --export-probe: 13/13 fixed cases,
  including a new recursive call inside a nonwarp repeat with 84
  procedure calls, 84 returns, 240 observed yields, and live parent
  loop frames in the exported image; an empty-string immediate
  survives repeated yield without needing a GC root.
- Seeded native recursive parity: 32/32.
- Repeated callsite stress: 50,001 pushes = 50,001 pops.
- Native selected CTest: 7/7, including rooted GC smoke.

**Execution limitation:** The exported stack is a transfer object for
RT-001; it is not itself a running continuation. A native Fiber still
owns machine PCs, register/SSA state, conditional branch phase and
opaque host effects. RT-001 may consume this image only when its
compiled shared-CFG lowering can prove that all necessary SSA values
and effects are explicitly represented, and the source revision remains
compatible. The GC root provider is thread-confined and must be
created/destroyed at a scheduler-safe point without concurrent GC.

## Unique after-call PCs and self-contained source symbols

The old native mirror used transfer.continuationBlockId (block.next)
as its return PC, with one shared empty-next sentinel. This loses the
distinct return sites at the end of loops, conditional substacks,
procedure bodies, and scripts. It is not sufficient to reconstruct a
call-chain continuation after discarding a native Fiber.

Each resolved procedure-call block now receives one deterministically
numbered **synthetic post-call PC**, separate from block PCs
(1..numberOfBlocks). Numbering sorts callsite block IDs, so the result
is independent of unordered_map iteration and transfer traversal.

An immutable SourceReturnSite maps each synthetic returnPc to:
  - callBlockPc: the source call opcode
  - targetPc: source next block or loop header (0 on exit)
  - kind: NextBlock, AfterBranch, LoopLatch, ProcedureExit, ScriptExit

The structural resolver walks parent links when block.next is missing.
A parent whose next points to the current node is a previous linear
sibling, not a control ancestor. Only actual control nesting determines
the loop-latch/after-branch successor. The returned source PC is a
semantic **phase identity**, not an executable machine PC.

A new SelfContainedSourceContinuation bundles:
  - independently GC-rooted RootedScratchCallStack;
  - shared, immutable SourceFrameSymbols containing the block-PC names,
    procedure-ID names, and synthetic return-site decode table.

exportSelfContained() retains the original exportContinuation() API as a
compatibility path and validates that every nested frame has a known
procedure and return site, with matching parent resume PC. The symbol
table can outlive the originating native session. The opt-in LLVM
export probe now verifies the complete bundle on both sides of a real
yield.

Validation: native_callframe_mirror.py --export-probe 18/18, including
four new branch/loop/procedure/multiple-callsite cases and an explicit
script-exit callsite. Multiple lexical callsites to the same callee
have different synthetic return PCs. Every final Scratch variable
agrees among Interpreter, normal LLVM and mirrored LLVM; test
diagnostics verify exact distinct callsite counts and return kinds.

**Remaining blocker:** a full native-to-common-CFG SSA mapping, branch
phase/effect replay safety, and scheduler adoption of the independent
frame image. The exported stack and source symbol table do not, by
themselves, provide executable Fiber-independent recovery.

## Explicit resume phases and fail-closed mid-body yields

LLVM source-loop PCs are NOT sufficient to reconstruct execution: a
checkpoint just before entering a nested procedure is not equivalent
to a checkpoint *after* the loop body has completed. Both can have
exactly the same loop PC and repeat counter, but resuming them from
the wrong phase would re-run the caller's side effects.

The IR-001-only opt-in bridge now stores a fixed SourceResumePhase in
the final local of each nonroot frame. For N source loops the layout is:
N raw boxed numeric counter words, N ordered loop-PC words, path depth,
and a phase word (2*N+2 locals). The root has no local words.
Values are:
- ProcedureEntry: a child call has been pushed but its body has not run;
- InsideLoopBody: a nested source call is about to execute;
- AfterLoopBody: the iteration's body completed and the native Fiber yielded;
- AwaitingCallee: the parent is suspended with a child call outstanding;
- AfterCallee: a child has returned to the parent's synthetic after-call PC;
- UnknownBody: source execution has started but the exact SSA/effect phase
  is not captured, so this checkpoint MUST NOT be used for deoptimization.

The native code emitter reports InsideLoopBody before a procedure call and
AfterLoopBody at loop yield points via the versioned
xyo_proc_frame_checkpoint_path_phase helper. It also calls
xyo_proc_frame_body_started after the native Scratch procedure push helper
returns, so a later wait inside a non-loop procedure cannot be
misclassified as a clean ProcedureEntry. The default non-opt-in JIT
does not emit these calls. The original checkpoint helper remains usable
by older opt-in clients.

NativeProcedureFrameSession::sourceCheckpoint() now exposes the phase.
SelfContainedSourceContinuation::decodeFrames() reconstructs the ordered
root-to-leaf typed source-frame descriptors, with parent return-site
metadata, loop PCs/counters, and the phase of each frame. It never
copies unrooted heap-string arguments: those are borrowed on demand
from the independently GC-rooted RootedScratchCallStack.
SelfContainedSourceContinuation::valid() validates phases, known source
loop slots/PCs, numeric repeat counter representation and frame links.

When XYO_JIT_CALLFRAME_EXPORT_PROBE=1 the runtime checks, on each
real Fiber yield, that independent decoder output agrees with the
live leaf CallFrame, and every ancestor procedure is awaiting its child.
Probe diagnostics include phase_entry, phase_after_body,
phase_after_callee, awaiting_frames, awaiting_loop_frames, phase_unknown.

Regression: native_callframe_mirror.py --export-probe 19/19, including
a deliberately inserted real control_wait between nonwarp flat
statements. That wait yields with UnknownBody rather than claiming a
clean ProcedureEntry. Recursive and nested-repeat cases verify that
parent frames are AwaitingCallee and that child yield phases and repeat
counter bits survive export. Scheduler safe-point
native_source_handoff.py also passes 5/5 cases; the scheduler itself
does NOT discard the Fiber or attempt unproven SSA reconstitution.

Remaining work is to connect a proven mapping of all live LLVM SSA
values/side-effect phase to RT-001, and to implement a guarded transfer
that deoptimizes to the interpreter only after an exact replay-safe
boundary. The phases are necessary metadata, not independent execution.

## Compiled-source revision provenance and detached GC roots (2026-11-08)

A source-level numeric CallFrame is valid only for the exact Scratch Blocks
revision that produced its LLVM machine code. Comparing a handoff with the
live revision recorded at the moment of capture is not sufficient: a
previously compiled script may still run after a source edit, with identical
block IDs but different meanings. Interpreter resume against the new
source could then replay the wrong instructions.

ScratchCompilationUnit::capture() now records sourceRevision from the
originating Blocks and fails valid() if revision changed while capturing.
jitEmitScript rejects source snapshots whose revision differs from the
live Blocks either before lowering or before publishing generated code.
Opt-in LLVM procedure calls use xyo_proc_frame_enter_rev instead of the
legacy helper, embedding the captured compilation revision as an i64
constant. The first live call then builds a native frame observer only
when the current revision equals the embedded revision.

NativeProcedureFrameSession retains its borrowed source Blocks identity
and immutable compilation revision. It rejects entry and yield on source
revision mismatch; exportContinuation() and exportSelfContained()
fail closed when the source differs or provenance is missing. The
self-contained source symbol table also contains the original revision
so the scheduler can independently check it. xyo_proc_frame_enter
remains compatible for observation only; frames without compile-time
provenance are never transferred into a different executor.

tests/platform/native_callframe_revision_smoke.cpp exercises correct
matching revision, mismatched compile-time revision before first call,
normal editor mutation via Blocks::createBlock, legacy no-provenance
rejection and a fresh revision. It also exports a real Unicode
heap-string argument, destroys the original native session and forces GC;
the detached, independently rooted CallFrame still retains the string
before and after the subsequent source edit.

The RT-001 native interpreter deoptimization harness simulates source
resetCache() at the real native post-yield scheduler safe point using
test-only XYO_RT_TEST_BUMP_SOURCE_REVISION=1. The test ensures LLVM
continues to the correct Scratch result while stale interpreter adoption
is refused; an unchanged revision still allows validated adoption.
The regression passes 71/71 native resume/rejection cases, including
multi-level procedure calls, counted loops, while/until, string arguments;
the standalone compiled-revision + detached-GC CTest also passes.

**Limitations:** Blocks::revision tracks editor mutation through the
normal Blocks API or resetCache. Direct modification of internal
Block objects without revision invalidation is outside this contract.
A revision guard alone does not establish general SSA/effect replay
safety. Arbitrary recursion, conditional/effect phases and unknown
host-side effects still require separately validated executable
continuations. Default Fiber JIT remains unchanged and native interpreter
deoptimization remains opt-in.

## Same-revision source content validation at native handoff

A Blocks structural revision increment is contractually expected on edits,
but plugin/editor code can also mutate an exposed Block object directly
without calling resetCache(). The revision alone cannot distinguish two
different versions of the same script.

The shared source digest in src/ir/scratch_source_digest.hpp is a pair of
independent 64-bit rolling hashes of the deterministically sorted captured
source graph. It covers executable Block IDs, opcodes, successor and parent
edges, ordered active/shadow inputs, raw field Value bits and reference IDs,
mutation attributes (including procedure definitions/defaults/warp),
sequence ordering, target ID and resolved procedure definition table.
Mutable Mutation::parsed caches are intentionally excluded. Both hashes
are calculated from a ScratchCompilationUnit, never unordered_map order.
This fingerprint is a robustness check for accidental raw editor mutations,
not cryptographic authentication.

When XYO_JIT_CALLFRAME=1, LLVM codegen saves the captured source digest
next to the already embedded revision and emits
xyo_proc_frame_enter_verified(..., compiledRevision, digestLo, digestHi).
At first native call, the live source graph is captured and compared with
the compile-time revision *and* digest. The former xyo_proc_frame_enter_rev
symbol remains for ABI compatibility but its frames have no digest and are
not eligible for cross-backend handoff.

NativeProcedureFrameSession::sourceUnchanged() deliberately remains the
cheap revision guard used at every procedure call/yield. The more expensive
sourceContentMatches() re-captures and fingerprints the graph only at
exportContinuation()/matchesExport(), after LLVM has yielded to the
scheduler. Thus a same-revision change is detected before discarding the
Fiber or reconstructing any interpreter stack, without imposing O(graph)
work on the default JIT hot path.

The standalone xyo-native-callframe-revision-smoke covers same-revision
direct opcode, input successor, literal field and mutation attribute edits,
restoration of the exact original source, revision mismatch, and independent
GC-root survival after destroying the original session. The opt-in native
deopt harness and LLVM CallFrame parity tests still pass for unchanged
source. RT-001 can validate via exportSelfContained() and matchesExport()
without knowing the private digest layout.

Limitations: Fingerprints are noncryptographic. C++ pointer Values for
source string fields use stable raw source bits (not a serialized textual
representation) and are safe for comparisons against the same live Blocks
instance, not for persistent storage or cross-process authentication.
Unsafe concurrent mutation of the original Blocks is outside the
single-threaded scheduler snapshot contract.


## Exact caller/callee identity and source conditional arms

A synthetic post-call return PC is necessary but not sufficient for a
portable deoptimization. An exported child may otherwise be assigned to the
wrong Scratch procedure or parent activation while retaining the same
syntactically valid return PC. Native CallFrame metadata now records:

- callerProcedureId (0 only for the captured root script),
  calleeProcedureId and calleeEntryPc;
- exact argument count for the original callsite;
- a lexical source branchPath, outermost-to-innermost, of the *source*
  control_if / control_if_else block PC with selected SUBSTACK arm
  (1=then, 2=else).

These fields are constructed once from the immutable compilation graph.
Parent edges must be verifiable NEXT, SUBSTACK or SUBSTACK2 edges, and
the parent chain must terminate at the actual root event hat or an
existing procedures_definition. Missing/malformed source parent linkage
causes opt-in observation to fail closed, never guess. This provenance
does not re-evaluate Scratch reporter conditions, record their LLVM SSA
results or certify effect replay.

SelfContainedSourceContinuation::valid() validates for every exported
root-to-leaf frame that the parent procedure ID equals the callsite owner,
the child procedure ID and argument count equal the callsite target and
original arity, and the unique return PC equals the parent frame's saved
resume PC. The immutable source branch path and entry PC must also refer
to known source blocks. Rooted parameter ownership is unchanged.
decodeFrames() exposes this SourceReturnSite through DecodedFrame.caller
for RT-owned recovery without consulting LLVM internal pointers.

The opt-in export probe additionally counts which conditional arm was
actually reached by a suspended LLVM Fiber: branch_then, branch_else and
nested_branch. Tests native_callframe_mirror.py include branch_arms and
nested_branch_arms that visit both sides of real if/else and a nested IF
with a nonwarp yielding callee. The native CallFrame integration corpus
passes 21/21. The C++ revision smoke additionally tests both returned
lexical arm paths (nested then/outer else), target procedure identity and
rejects a forged image that puts the wrong child procedure at a valid
post-call return PC.

This is source-side structural provenance, not yet arbitrary branch
resumption. RT-001 can adopt only where its own committed-prefix and
effect checks prove a replay-safe Interpreter stack. As of this change,
a newly added bounded_recursive_if case still fails opt-in adoption:
its source CallFrame is exported correctly, but RT's committed-prefix
walk currently rejects a previously completed IF before the leaf repeat.
RT-001 received the targeted diagnosis for its separate owner.


## Lexical loop-path and parent-phase validation (2026-11-08)

Native CallFrame export previously checked a child's synthetic post-call
PC against its immediate parent, but a copied image could still claim that
the parent was in an arbitrary execution phase or suspended in an
unrelated loop. The compiler now saves callerLoopPath on each immutable
SourceReturnSite. This is the source-lexical outermost-to-innermost set
of enclosing control_repeat / control_while / control_repeat_until /
control_forever / control_for_each PCs, collected by verifying each
parent NEXT or SUBSTACK edge in the captured compilation graph. The
SourceFrameSymbols table also carries sourceRootPc for the actual hat.

SelfContainedSourceContinuation::valid() proves, for each non-root
parent-to-child edge, that the parent phase is AwaitingCallee and its
active ordered loop PC path equals the child's compiled callerLoopPath.
A child in ProcedureEntry must have its resume PC equal to the
captured callee entry. The root must come from the original hat; with
no child active, it can be at either its original hat PC or a
source-authenticated synthetic post-call PC owned by root. The latter
is essential when the native Fiber yields after its final procedure
call returns; rejecting it breaks valid root-only scheduler handoffs.

At the opt-in native procedure entry, a caller with no lexical loop
surrounding the call resets stale loop counters, path PCs and depth
from an earlier completed loop. Calls inside a loop require an exact
source-scoped checkpoint already emitted by LLVM or fail closed.
No default JIT path is altered.

C++ smoke tests create a three-frame stack with nested counted-loop
call, verify the authentic parent AwaitingCallee + loop PC image, and
reject forged images with wrong parent phase or missing loop path.
They validate a detached root-only image after both children return
and reject an unrelated root resume PC. The differential regression
post_loop_call executes a repeat, calls a procedure outside the loop,
then repeats and calls again. The native export probe confirms the
parent has no active loop path at those child yield points;
Interpreter, regular LLVM and opt-in LLVM results agree.

This is source-structural validation, not general recovery of LLVM
machine PC, SSA temporaries or commit-once host effects.


## Lexical topology and procedure ownership of saved loop PCs

A sequence of individually valid loop source PCs is not necessarily an
executable source path. The old CallFrame validation rejected repeated
loop PCs but could still accept a forged snapshot joining sibling loops,
dropping an enclosing loop, reversing their order, or importing a loop
from a different procedure.

SourceFrameSymbols::Loop now carries the immutable ownerProcedureId and
a complete, ordered enclosingLoops vector. Both are derived from the
captured source's checked NEXT/SUBSTACK parent chain, not the native
LLVM machine stack. At checkpointPath() each loop in an active path
must be owned by the current source procedure and have exactly the
reported parent prefix; this is checked before mutating the frame's
numeric arena. SelfContainedSourceContinuation::valid() independently
checks the same properties for every active loop in every frame,
without depending on the originating native session.

An invalid loop frame fails closed. No arbitrary loop-PC sequence can
be used to certify a recoverable SSA/effect position. This additional
check is only present in the opt-in portable CallFrame tracking path;
the regular LLVM JIT and interpreter remain unchanged.

tests/platform/native_callframe_revision_smoke.cpp adds a real two-level
counted-repeat source graph, accepts the correct outer/inner checkpoint,
and refuses an inner-only checkpoint, reversed order, a detached
image with forged nesting and a loop copied from another procedure.
The regression has no dependency on a live native Fiber for detached
image validation. Differential native_callframe_mirror.py 22 cases
and native_simple_resume.py 209 resumed/refused parity runs pass with
the stricter contract; scheduler handoff 5/5 and recursive fuzz32/32.


## Expiring completed native source checkpoints on statement entry

The LLVM-native opt-in mirror has precisely defined checkpoint phases,
but those phases can become stale as execution progresses. For example,
after a nested callee returns, the parent is marked AfterCallee at that
specific synthetic post-call PC; subsequent Scratch statements may
already have evaluated reporters or changed variables. Likewise a
previous AfterLoopBody checkpoint is not safe to reuse if a new statement
starts and later suspends at a wait. Reusing either stale phase as an
executable interpreter handoff would risk replaying committed effects.

The new xyo_proc_frame_statement_started(ctx, sourceBlockId) hook is
emitted at the beginning of each LLVM-compiled Scratch statement only
when XYO_JIT_CALLFRAME=1. NativeProcedureFrameSession::statementStarted
checks that the source block belongs to the compiled source symbol
dictionary, and expires any previously clean ProcedureEntry,
AfterLoopBody, InsideLoopBody or AfterCallee phase to UnknownBody
*before* the statement's reporters/effects can execute. It rejects a
statement while the current frame claims AwaitingCallee, and also
rejects unknown block IDs. No raw native PC, thread pointer or GC
address is retained; the existing immutable Return PC is kept for
provenance but not treated as replayable when phase is UnknownBody.

This does not manufacture new safe points. The already existing
xyo_proc_frame_checkpoint_path_phase instrumentation establishes a
fresh, exact AfterLoopBody only after a completed loop iteration.
An active parent AwaitingCallee remains intact while the native child
executes because the topmost frame, not its ancestors, receives the
statement-start hook. The RT-001 interpreter handoff requires the
correct AfterLoopBody leaf, so UnknownBody safely declines migration.

The native_callframe_revision_smoke test covers AfterLoopBody ->
UnknownBody -> AfterLoopBody after a fresh checkpoint, AfterCallee ->
UnknownBody on the next statement and rejection of an unknown statement
ID. The differential post_loop_wait workload yields during a wait
after a completed repeat and requires the actual native probe to report
UnknownBody rather than reusing that repeat's old checkpoint.
All of this is opt-in and leaves the ordinary LLVM fast path alone.


## Authenticating statement and callsite procedure ownership (2026-11-08)

A compiled source block PC being present in SourceFrameSymbols does not
prove that it belongs to the currently executing procedure. An otherwise
valid native frame could use a source statement owned by a sibling
procedure (or mistake a reporter or prototype for a stack statement).

SourceFrameSymbols now provides statementOwners aligned with blockIds.
Every entry is the owning source procedure ID, zero for the root, or
CallFrameAbi::kNoPc for an unowned/non-executable block. It is derived in
O(number of reachable nodes + validated executable edges) by traversing
NEXT and supported SUBSTACK/SUBSTACK2 edges from the captured root hat
and each known procedures_definition body. Every traversed edge must
agree with the child's parent field. Reporter inputs and prototypes are
never traversed and never gain executable statement ownership.

NativeProcedureFrameSession::statementStarted validates a statement's
compiled source PC AND its original owner against the top CallFrame
procedure ID. NativeProcedureFrameSession::enter similarly checks the
immutable callerProcedureId for the original procedure callsite before
it changes return PC, local phase or GC-root sidecar. Detached
SelfContainedSourceContinuation::valid() checks callsite ownership for
each parent-to-child edge in the copied image.

A separate long-chain fixture captures 2,048 successive Scratch
stack statements followed by a procedure call; it verifies that the
entire linear source chain stays root-owned and the separate procedure
body stays correctly owned by its procedure, while the prototype
remains unexecutable. It runs without recursive C++ DFS on ownership
and guards the formerly O(nodes * chain depth) precomputation.

The native_callframe_revision_smoke regression covers correct ownership,
cross-procedure statement, reporter/prototype source PC and a root-owned
callsite forged in a child frame. Valid ownership proceeds; all incorrect
cases fail closed. The normal LLVM JIT path is unchanged because the
source-statement instrumentation remains opt-in.


## Verified source post-call successors (2026-11-08)

The independent native CallFrame image now distinguishes a return PC
identifying the native callee's *caller* from a completed call's source
successor inside the executing procedure.

SourceFrameSymbols::validReturnTarget verifies the original immutable
source continuation contract. NextBlock and AfterBranch successors must
name executable statements belonging to the callsite's original source
procedure; LoopLatch must name a tracked loop with the same procedure
owner; ProcedureExit and ScriptExit must have no target block and must
match the procedure/root ownership. This is checked in
SelfContainedSourceContinuation::valid() for each actual parent-child
callsite, not just for a nonzero target PC.

For a nonroot leaf claiming SourceResumePhase::AfterCallee, valid()
further verifies that its resume PC names a source-authenticated
completed callsite owned by this leaf's procedure, and that the
immutable lexical loop path of the callsite equals the active saved
loop path. Invalid or foreign post-call positions fail closed. The new
postCallSuccessor() API exposes the original SourceReturnSite only when
the independently rooted image is valid and its executing leaf phase
is AfterCallee; it returns no successor for UnknownBody, loop checkpoints
or root-only images lacking a source phase.

This API does *not* reconstruct LLVM SSA values or authorize migration
at any arbitrary native machine instruction. RT-001 still controls
whether a source continuation can be executed by the interpreter.
Native revision CTest verifies a true NextBlock after a nested call,
LoopLatch inside a repeat, stale UnknownBody refusal, a forged foreign
caller resume PC and a forged cross-procedure target PC.


## Exact return-edge provenance inside the same procedure (2026-11-08)

Merely checking that a post-call target is an executable block in the
correct Scratch procedure is insufficient: a forged image can redirect
a return from the immediate NEXT to a later, same-owner statement, or
redirect a completed conditional branch to an arbitrary sibling. Both
are valid source PCs yet would skip committed or required effects.

SourceFrameSymbols now captures a pointer-free sourceTopology table
aligned with blockIds. Each node records the original parent PC,
NEXT PC, active SUBSTACK and SUBSTACK2 PCs, plus a narrow source node
kind (root event hat, procedures_definition, tracked loop, conditional,
or other). This is generated once from the immutable
ScratchCompilationUnit; detached GC-rooted continuation images need no
Blocks pointer, native Fiber or LLVM IR to validate the table.

SourceFrameSymbols::structuralReturnTarget(callPc) walks these original
source edges with an explicit node-count traversal bound. A direct
NEXT resolves to NextBlock; terminal calls ascend verified linear
parent NEXT edges until the nearest enclosing tracked loop (LoopLatch),
conditional with a NEXT successor (AfterBranch), procedure definition
(ProcedureExit), or root hat (ScriptExit). Structural SUBSTACK ownership
must agree with each ascent. Unknown/ambiguous edges fail closed.

validReturnTarget() now requires both a correct caller procedure owner
and exact agreement between the original graph-derived return kind/PC
and the exported SourceReturnSite. This is applied to every active
parent-child CallFrame, to nonroot AfterCallee leaves, and also to
root-only images retaining a completed root-owned synthetic call PC.
postCallSuccessor() remains a *structural description* of source
control flow, not a general SSA/effect-resumption authorization.

The native C++ revision smoke includes same-owner NEXT redirection
(redirecting immediately after a call to a later valid statement),
AfterBranch redirection to another root-owned statement, kind
masquerading as ProcedureExit, and a forged root-only post-call image.
Every altered image is rejected even though its target PC and owning
procedure were individually valid; original unmodified images continue
to export.


## Source-derived caller branch and loop ancestry (2026-11-08)

An exact post-call target alone does not establish the lexical caller
path. Two calls in the then/else arms of one Scratch conditional can have
the same AfterBranch target, and a copied native parent frame can carry
loop locals that agree with a *forged* callsite metadata vector while
disagreeing with the actual source graph.

SourceFrameSymbols::structuralCallerContext(callPc) independently ascends
the captured sourceTopology table, verifying every NEXT, SUBSTACK and
SUBSTACK2 parent-child edge. It records the actual ordered then/else arm
path and enclosing tracked-loop PCs, terminating only at the original
root event/clone hat or the matching procedures_definition for the
statementOwners procedure ID. The bounded walk fails closed on missing
parents, non-executable reporter edges, ambiguous unrelated containers,
or parent cycles. The known control_all_at_once wrapper is recognized as
a source SUBSTACK container without introducing a false branch/loop.

validReturnTarget now requires the captured SourceReturnSite's
callerProcedureId, branchPath and callerLoopPath to *exactly* match this
independently computed context, in addition to the already required
exact target kind/PC. This check is enforced in detached rooted image
validation and the native export probe without consulting the LLVM
Fiber or mutable Blocks.

C++ source-revision smoke exercises a then call whose outer IF/ELSE
contains a real ELSE arm: flipping the recorded arm from 1 to 2 must
fail even though arm 2 exists. It also substitutes an alternate root
callsite with the same genuine AfterBranch successor, and tampers both
a nested call's loop-path metadata and its parent saved loop locals to
appear self-consistent. The original source path still rejects every
forgery. An extra clone-hat test verifies control_start_as_clone remains
an accepted legitimate script root.

This establishes the original source *identity and ancestry*, not a
general LLVM SSA/effect continuation or authorization to replay
side-effectful reporter conditions.

The independent LLVM 23 xyo CLI build confirmed the stricter native
CallFrame mirror still exports real suspendable frames for 23/23
interpreter/regular LLVM/mirrored LLVM parity programs. The RT-owned
AfterCallee adoption harness, including its newly added terminal IF
return cases, completed 339/339 native resume/fail-closed parity runs
on a stable copy of the executable. Scheduler GC-rooted handoffs 5/5,
seeded recursive fuzz 32/32 and 50,001 nested call/return stress also
passed. RT-owned files and frontend ABI were not modified by this change.


## Precomputed immutable caller proofs for repeated handoffs (2026-11-08)

Deeply nested Scratch callsites previously required
SourceFrameSymbols::validReturnTarget() to walk the original
parent/NEXT/SUBSTACK ancestry (up to the size of the source graph)
every time a GC-rooted continuation was validated. A long script with
many yields could repeat that expensive structural analysis for every
active CallFrame.

SourceFrameSymbols::VerifiedCallsite now captures the independently
derived return kind, exact successor PC, source procedure owner,
ordered conditional-arm path, and ordered lexical-loop path. The
constructor computes each proof **once** directly from immutable
sourceTopology/statementOwners, then compares it to the original
SourceReturnSite metadata before enabling native CallFrames. A mismatch
aborts the opt-in bridge. Only after all sites validate is the complete
lookup published through a shared_ptr<const VerifiedCallsiteMap>.
Diagnostic copies of SourceFrameSymbols can change return metadata,
but they share the original read-only verified proof.

Later image.valid() and postCallSuccessor() calls compare each return
descriptor to the already-certified proof in expected O(1) lookup time
plus O(lexical branch/loop path length) vector comparisons; they no
longer walk the entire ancestry chain on every export. A 2,048-NEXT
source chain regression validates 50,000 repeated source callsites and
a tampered diagnostic image that must still be rejected. This shifts
structural graph traversal from the hot deopt/handoff path to
compile/capture time. The normal LLVM JIT remains unaffected; the
native CallFrame instrumentation remains opt-in.

This is an internal-source consistency mechanism, not a cryptographic
signature or a guarantee of arbitrary SSA/effect replay. Source
revision/digest and RT-owned executable handoff checks remain
mandatory before any actual interpreter adoption.


## Root counted-repeat CallFrame ABI (2026-11-08)

A procedure called from a root-script repeat previously had no portable
remaining counter: the root was created with zero locals, and the
native CallFrame session is created lazily by the first procedure call.
An earlier root loop checkpoint could therefore be lost.

The root now uses the same numeric 2*N+2 local-word schema as native
procedure frames: per-source-loop Value counter bits, ordered active
loop source PCs, path depth and SourceResumePhase. It retains no GC
arguments. The opt-in LLVM code generator forwards its captured
loop path and SSA counters to the new verified rootpath host helper
before the first xyo_proc_push. The helper constructs the versioned,
source-digested NativeProcedureFrameSession, publishes the root loop
checkpoint on the first call, and only then pushes the child.
Subsequent nested calls rely on the existing parent checkpoint path.

The independent SelfContainedSourceContinuation validates the original
root loop owner and lexical ancestry, numeric counted-loop bits,
AwaitingCallee caller path, AfterCallee return provenance, and root
loop-latch source PC versus synthetic post-call PC. Its root DecodedFrame
now exposes live loop PCs and counters to RT-001. Unknown or missing
first-call root counts are rejected instead of guessed.

Root-only yields are not executable RT handoff points in this change.
The original child-procedure yield and checkpoint diagnostic counters
are intentionally preserved; root counters are reported separately
through root_loop_frames and root_counted_frames. The new C++ smoke
test checks a root with two nested repeats, remaining counters 19/7,
the first call and return, stale phase expiry and invalid or missing
counter rejection. The differential LLVM mirror also requires real
root numeric counters in the exported image during native child yields.

Actual adoption of counted root loops belongs to RT-001: it must
consume the verified root counter bits and separately prove exactly-once
execution and effect-commit semantics. This change alone does not
claim arbitrary source or SSA resume support.


## Final native root-loop bridge regression (2026-11-08)

The LLVM CallFrame root-counter bridge and the concurrent RT-001
repeat(1) recovery fix were rebuilt into one frozen executable.
Its actual native Fiber -> Interpreter resume/fail-closed harness
passed 426/426 cases, including root_repeat_one_call,
root_repeat_one_terminal_chain, root_nested_repeat_one_call,
root_if_repeat_one_call, and explicit refusal of unsupported dynamic
or nonliteral counted-root repeats. No duplicate Scratch effects
remain in these tested paths.

The opt-in native LLVM CallFrame mirror independently passed 23/23
projects with source export probes; all root-counter-required cases
reported positive root_counted_frames. GC-rooted handoff 5/5,
recursive fuzz 32/32, 50,001 nested call/return stress, and six
related native CTests passed. IR-001 only implements source counter
capture and validation; RT-001 owns and separately gates executable
reconstruction. General counted root repeats with arbitrary remaining
counts, root-only yield, and arbitrary SSA/effect PC resumes are not
newly authorized by this change.


## Root repeat counter exactness and dormant-slot validation (2026-11-08)

The root numeric CallFrame is now tested against *exact* SSA counter
values, not merely their existence. When the optional exported-image
probe is active, it reports root_counter_sum (sum of finite,
nonnegative integral active counters) and root_max_depth (maximum
number of simultaneously active lexical loops in the root frame).
The actual retained image always preserves the original Value bits;
this diagnostic intentionally skips out-of-range, nonintegral values
rather than coercing them into an executable counter.

The LLVM source mirror now tests 25 programs, including two additional
real projects with nonwarp child procedure yields:
- A source root repeat(12) makes 12 calls; 3 child yields per call
  expose the expected counter sum 3*(1+...+12) = 234 and depth 1.
- A root repeat(3) enclosing repeat(4) makes the same 12 calls.
  The 36 child yields expose outer sum 72 and inner sum 90,
  totaling 162, with exact active lexical depth 2.
- A root repeat whose TIMES input reads a variable initialized to 12
  retains 12 iterations even as child procedures modify that variable.
  Its saved counter sum is 234, with depth 1.

Each fixture compares interpreter, ordinary LLVM, and source-mirrored
LLVM outputs, and the export probe requires exact root sum/depth.
The 25/25 cases passed in the dedicated build.

The detached CallFrame validation also rejects numeric values in
inactive source-loop counter slots for both root and procedure frames.
Every newly published loop checkpoint resets all dormant slots to the
kNoLoopCounter sentinel, so a contradictory extra counter cannot be
smuggled into an apparently valid image. The dedicated C++ smoke test
constructs a root with two nested counted repeats, publishes an
outer-only live path, then modifies the *inactive inner loop's*
counter in a fresh detached numeric frame; validation rejects it.
Native source revision CTest passed; GC scheduler handoff 5/5,
recursive fuzz 32/32, 50,001 call/return stress and the six related
CTests also passed.

The concurrently evolving RT-001 harness was not modified. In its
latest in-flight version, full native-simple-resume encounters a
stale expected fail-closed assertion for ProcedureExit after the actual
output parity tests succeeded; the mismatch was reported to RT-001.
Do not infer full RT-001-suite success from the IR-001 results.


## Immutable callee identity and synthetic return-PC binding (2026-11-08)

The prior immutable source proof certified the lexical caller, branch
arms, loop ancestry and exact return successor, but the detached
SourceReturnSite's *callee procedure ID, entry PC, argument count and
synthetic return PC* were checked only against other exported frame
metadata. A malicious or corrupted image could alter a return record
and the corresponding child frame together, making both agree while
describing a different procedure from the original Scratch callsite.

VerifiedCallsite now freezes the authentic synthetic return PC and
callee procedure ID, callee entry PC and arity from the separately
captured CallTarget map. SourceFrameSymbols construction cross-checks
every SourceReturnSite against that independent call map and aborts
the opt-in bridge on disagreement. Every detached image thereafter
requires exact equality against the immutable verified source proof.
The lookup and hot-path complexity remain unchanged.

The C++ native source revision smoke now independently alters each
return-PC/callee/entry/arity field and verifies rejection. A stronger
test alters *both* a return site's callee/entry and its numeric child
CallFrame to reference a different, valid procedure definition with
the same argument count and identical original AfterBranch successor.
This self-consistent but source-inconsistent image is rejected.
No general LLVM SSA/deopt checkpoint is inferred from this metadata.


## Verified source callsite: final 2026-11-08 checks

The new native C++ revision smoke rejects altered return-PC, callee ID,
callee entry PC, and argument arity, including a coherent adversarial
pair of forged callee frame and matching forged call metadata.
Original source images remain valid.

With the compiled, source-derived VerifiedCallsite contract enabled,
the full LLVM differential mirror passed **25/25** cases. Native
Fiber-to-Interpreter adoption/fail-closed regression passed **486/486**
runs, including two-root counted-loop interleaving and strict
GC-after-adoption checks. Scheduler/GC source handoff passed **5/5**,
the 32-seed recursive native CallFrame fuzz passed **32/32**,
and 50,001 native calls/returns preserved interpreter output.
The related six native CTests passed **6/6**. The RT-001 harness was
concurrently updated while the initial tests ran: its expected number
of completed independent root adoptions for later checkpoints was
corrected, and the same frozen CLI passed the revised suite.

Arbitrary LLVM SSA/effect reconstruction remains out of scope.


## Root-only post-call source successor metadata (2026-11-09)

A returned native callee can leave only the root CallFrame alive, with
its typed phase AfterCallee and a source-authenticated synthetic return
PC. Previously, postCallSuccessor() unconditionally rejected frames of
depth one because older root frames lacked a source phase. That legacy
method remains unchanged because RT-001 invokes it in the executable
native-to-Interpreter migration path.

A separate metadata-only rootPostCallSuccessor() now resolves precisely
the original source ScriptExit, NextBlock, or AfterBranch target while
depth == 1 and the root phase is AfterCallee. It accepts only independently
validated GC-rooted images, root-owned original return sites, exact
source successor and original lexical root-loop path. The root's
AfterLoopBody/UnknownBody/InsideLoopBody and invalid image states cannot
be passed off as a completed call.

C++ regression tests exercise root ScriptExit, root nested repeat(19,7)
NextBlock with preserved counters and a root IF/ELSE AfterBranch.
They refuse stale effect/loop checkpoint phases and forged successor
metadata. This API does not authorize migration of a root-only Fiber:
only RT-001 may prove effects, live Scratch state and actual executable
control reconstruction before choosing that transfer.


The dedicated root-only C++ revision smoke and six related CTests
passed. In a frozen integrated LLVM executable
(build/async-jit/ir001-root-return-frozen-xyo), native callframe
mirror25/25, actual Fiber-to-Interpreter resume/fail-closed490/490,
scheduler GC-root handoff5/5, seeded recursive fuzz32/32 and
50,001 call/return stress all passed. No RT-owned source was changed.


## Synthetic return-PC dictionary alias rejection (2026-11-09)

The immutable VerifiedCallsite proof certifies a callsite's original
synthetic return PC, caller/callee identities and source graph edges.
A detached diagnostic copy of SourceFrameSymbols could still insert
a *second dictionary key* pointing to the same authentic SourceReturnSite.
If the numeric root resume PC, or both a child's return PC and its
parent's saved resume PC, were changed to this alias, the old
returnSite(key) would return the authentic payload. Its immutable proof
then succeeded despite the executable numeric PC no longer naming the
original source return site.

SourceFrameSymbols::returnSite(pc) now rejects any entry whose stored
SourceReturnSite::returnPc differs from the requested key. This binds
the *dictionary lookup address* to the proof's callsite identity without
adding a hash lookup or graph traversal to the existing hot path.

The dedicated native C++ regression demonstrably failed before this
guard: a root-only AfterCallee image with a forged PC alias passed
valid(). The same regression now passes, rejecting root-only and
active child/parent aliases. A further detached image contains a
non-ASCII GC-rooted heap-string argument, copies its sidecar, runs a
full collection, verifies that the string remains intact, and still
rejects the forged alias. Authentic source images and the other return
proof checks remain unaffected.

Only IR-001's source metadata lookup and native source-revision smoke
were modified. This is a structural integrity constraint, not a
general source effect/LLVM SSA deoptimizer.


Final integrated regressions for the canonical return-key fix used a
frozen LLVM/RT executable with matching RT-001 source changes:
build/async-jit/ir001-return-key-rt-latest-xyo (SHA256
ca57178339f67705945efe1eb9ff33cddf16956668e661d0f0315cd83ace303c).
The full native Fiber-to-Interpreter resume/fail-closed harness passed
**504/504**, including both root-only AfterCallee and interleaved
dual-root lifetime cases. The standard LLVM/mirrored/interpreter
comparison passed **25/25**, scheduler GC-root handoff **5/5**,
recursive fuzz **32/32**, and 50,001 call/return stress passed.
The six related native CTests passed **6/6** (50.51 seconds).
An earlier harness run against a frozen RT executable older than
the current RT root-only test expectations had stopped at
terminal_call; rebuilding the same IR-001 change with the newer
RT-001 implementation resolved that version mismatch. No RT-owned
code was changed by IR-001.


## Immutable source PC/name and compile-time provenance (2026-11-09)

The existing immutable VerifiedCallsite map pinned exact numeric source
call/target/entry PCs, but a diagnostic copy of the detached source
symbols could replace the corresponding blockIds strings without changing
those PCs, the return kind, the source revision or the digest field.
A real negative C++ test confirmed the old validation ACCEPTED a
root-only AfterCallee image whose next-block PC had been given another
valid statement's name (the name's owner was still the root hat).

The source symbol dictionary now captures the original block IDs and
procedure-definition IDs once as independent
shared_ptr<const vector<string>> values. block(pc) and procedure(id)
compare each dereferenced name to its original captured value. Return
site verification checks the source call, callee entry and procedure,
target, and all branch/loop controller PCs; full image validation also
checks each tracked loop source identity. These are constant-time name
lookups (plus already-required lexical path traversal); full source
arrays are not compared for every yield. The snapshots add one owned
string-table copy per source capture, not per exported image.

A second independent source-identity snapshot freezes the original
root hat PC, compiled source revision and two-lane content fingerprint.
The negative root-only C++ fixture independently confirmed that an
edited root PC, changed revision or changed digest could previously
be accepted by standalone image.valid(). All three now fail against
the original source identity even when a replacement root PC names an
existing block in the same captured source.

The native revision smoke explicitly rejects renaming a valid same-owner
return target, original callsite, procedure definition, nested IF/ELSE
controllers, callee entry, and a currently active root loop. It also
rejects modified root PC/revision/digest metadata. Genuine detached
frames retain their existing GC roots and CallFrame layout; no ABI
word/slot change or new runtime JIT instrumentation was needed.
This checks internal snapshot consistency, not cryptographic origin
authentication of arbitrary untrusted external serialized data.


The subsequent isolated provenance build was frozen at
build/async-jit/ir001-frozen-source-provenance-xyo with SHA256
84efceca10413e4190255578df3b034cf281b8e904b112345858d67f29528cf5.
This exact executable passed native real-adoption/fail-closed **546/546**
(including nested root terminal controls and source edit/resetCache
rejections), native LLVM mirror **25/25**, scheduler GC handoff **5/5**,
recursive fuzz **32/32**, and 50,001 calls/returns stress. All six
related native CTests passed **6/6** (52.37 seconds). Static source
checks also passed. All checkpoints remain opt-in; arbitrary mid-effect
or SSA live-value deoptimization is not implied.

## 2026-11-09: IR-only source deopt boundary classification

`source_resume_phase.hpp` defines `SourceDeoptBoundary` and maps each
`SourceResumePhase` to a source-level boundary (ProcedureEntry, LoopBody,
LoopLatch, PostCall, SuspendedCaller, or UnresolvedBody). Independently rooted
`SelfContainedSourceContinuation::deoptBoundary()` returns the classification
only after complete image validation and decoding. A missing or malformed
image has no classification. The enum is **not** an executable-deopt token:
no LLVM live SSA environment or committed-effect position is materialized.
In particular, `UnknownBody` is explicitly `UnresolvedBody`, even when the
last loop or return source PC is still stored in the frame. A consumer must
not infer a safe resume operation from this enum alone. RT-001 was not edited.

Standalone smoke linked against the existing native static library passed
(including classification checks for PostCall, LoopLatch, and UnresolvedBody).
The full shared build was blocked by an unrelated pre-existing SDL renderer
compile error (`realDlssEnabled_` undeclared in `sdl_renderer.cpp`).

## 2026-11-09: Detached deopt requirements (IR-001)

`SourceDeoptRequirements` is an IR-only conservative obligation bitmask:
`EffectContinuity`, `LiveSsaValues`, `ExactStatementPc`, and `ActiveCallee`.
`SelfContainedSourceContinuation::deoptRequirements()` only returns it for
a fully validated and decoded handoff. Source ProcedureEntry requires
effect-continuity validation; LoopBody/LoopLatch/PostCall require effect and
live-SSA proofs (including a certificate that no SSA values are live); a
suspended caller additionally requires its active callee; UnknownBody
requires effect continuity, live SSA and the exact in-statement PC.
This API publishes *obligations*, never completed proof or execution approval.
RT-001 must continue its independent resume validation. There is no new
SSA storage, effect log, executor or default-path JIT overhead.

Native revision smoke rebuilt in isolation against existing libxyo.a and
passed, exercising effect/SSA/PC requirements for PostCall, LoopLatch and
UnknownBody. The normal shared build is still blocked by unrelated renderer
`realDlssEnabled_` undeclared, so full integrated parity was not rerun.

## 2026-11-09: Compiler-manifested raw SSA diagnostic sidecar

IR-001 added `src/ir/source_ssa_snapshot.hpp`, defining
`SourceSsaManifest` (compiled revision, two-word source digest, procedure
ID, source resume PC, sorted unique SSA IDs and widths 1/32/64) and
`SourceSsaSnapshot` (independently owned raw numeric words). Capture and
validation reject absent/duplicate IDs, unsupported widths, missing or
extra words, noncanonical bit-width encodings and source/PC mismatches.
An explicit empty manifest is distinguishable from absent SSA capture.

`SelfContainedSourceContinuation::diagnosticSsa` is optional and unused by
existing runtime exports; `matchesSsaManifest(manifest)` additionally
validates the complete rooted image and matches the live leaf procedure and
resume PC plus original compiled source identity. Neither this method nor
`valid()` is executable-deopt approval: manifest authenticity, proven LLVM
liveness, GC-rooted nonnumeric Values and effect continuity still need a
compiler/runtime protocol. In particular, these tests use **synthetic** SSA
words; actual LLVM live-register spill and reload are NOT implemented.

Isolated native C++ smoke compiled and linked against the existing
`build/libxyo.a`: PASS, including malformed manifest, bad bit width, missing
SSA word, source digest/PC mismatch, absent/empty snapshots. Shared build
can remain blocked by concurrently edited SDL renderer code, so this is not
a full integrated JIT/RT parity run. RT-001 files unchanged.

## 2026-11-09: Opt-in live lexical loop-counter diagnostics

The existing LLVM emitPortableLoopCheckpoint() generates runtime numeric
counter bits passed into checkpointPath(). With
XYO_JIT_CALLFRAME_LOOP_SSA_DIAGNOSTIC=1, the native frame session now
captures these actual LLVM-emitted loop counter words in an owned
SourceSsaSnapshot, keyed by stable loop local slots and 64-bit width,
source revision/digest, leaf procedure and resume PC. A valid export
propagates only a diagnostic matching its current leaf. The diagnostic
expires on statement start, body start, procedure entry/leave and new
checkpoint. Normal mode does not perform additional vector allocations.

This is NOT a general LLVM SSA liveness capture: local IDs identify only
lexical loop counters, not arbitrary SSA virtual registers. A valid
diagnostic never certifies effect continuity or executable deopt.
Isolated native regression verifies bit-exact loop counters, source-ID
slot order, invalidation and detached snapshot preservation. Test PASS
after correcting an initial test assumption about lexical nesting order.
Existing libxyo.a was reused: full JIT/RT parity was not run.

## 2026-11-09: Compiler LLVM SSA use/dominance diagnostics

Added `src/ir/llvm_ssa_liveness.hpp`. For a concrete LLVM call marker, it
uses DominatorTree and reachable uses to conservatively report instruction
definitions live across the checkpoint. i1/i32/i64 instruction results are
enumerated in function-local pre-optimization IR order. Unsupported pointer,
aggregate, floating-point or unmapped live function-argument values prevent
the numeric-complete diagnostic flag from being set; this is NOT executable
deoptimization approval. Loop backedges can overestimate liveness by design.
No machine register spills, source-to-SSA reconstruction, effect log, or
runtime frame storage for these arbitrary SSA values is yet implemented.

Opt-in `XYO_JIT_SSA_LIVENESS=1` diagnoses each generated
`xyo_proc_frame_checkpoint_path_phase` call in `codegen.cpp` after
function generation. It prints numeric and unsupported counts without
modifying generated machine code, the default JIT, or RT-001.
The standalone LLVM 23 `tests/platform/llvm_ssa_liveness_smoke.cpp`
passes live/dead/pointer/unmapped-argument and cross-function-marker checks.
The full `codegen.cpp` translation unit passed compiler syntax validation.
An integrated runtime parity run has not yet been performed.

## 2026-11-09: Opt-in LLVM numeric SSA spill emission

`src/ir/llvm_ssa_spill.hpp` now has `emitLlvmNumericSsaSpill` for an
LLVM callsite with a fully numeric, representable liveness set. It emits
a function-entry `i64` alloca and volatile stores immediately before the
checkpoint; i1 and i32 values are zero-extended, and i64 retains exact bits.
The helper validates all definitions and widths before modifying IR and
refuses unsupported live values, empty sets and >4096 slots. The returned
IDs are pre-optimization IR instruction-order IDs (not Scratch ABI locals).

`XYO_JIT_SSA_LIVENESS=1 XYO_JIT_SSA_SPILL_DIAGNOSTIC=1` enables this
diagnostic emission in codegen. It leaves normal execution untouched when
not opted in and does NOT attach the native stack alloca to the detached
GC-rooted CallFrame. Even when a spill is complete, the effect position,
GC value roots, source-to-SSA mapping and executable RT deopt remain absent.

Standalone LLVM 23 IR smoke PASS (i1/i32/i64 zero-extension, volatile
stores, LLVM verifyFunction, unsupported pointer/arg rejection); full
codegen.cpp syntax validation PASS. No integrated runtime test yet.

### Numeric subset capture refinement

Even if a checkpoint contains unmapped pointer or function-argument values,
its representable integer SSA subset is now spilled for diagnostics. The
result sets `complete=false`; no caller may interpret a partial subset as a
complete recoverable state. The standalone LLVM 23 regression tests this
partial-spill case and re-verifies the modified function. The diagnostic
remains opt-in and no SSA snapshot is yet transferred to CallFrame.

## 2026-11-09: IR-001 synchronous detached numeric SSA transfer

Added `emitLlvmNumericSsaTransfer` in `llvm_ssa_spill.hpp`. The opt-in
LLVM emitter now has a separate function to read volatile numeric spill
words and copy them into a caller-owned i64 buffer *before* leaving the
native activation. It requires an explicit capacity and a destination
owned by the same LLVM function; foreign-function pointer operands and
insufficient capacity are rejected before IR mutation.

A standalone LLVM23 MCJIT test executes actual generated native code:
input 123456789 -> SSA 123456796; both the original post-checkpoint
computation and the caller buffer contain exactly 123456796. These
caller-owned words are then captured in `SourceSsaSnapshot` and remain
unchanged when the buffer is overwritten.

`SelfContainedSourceContinuation::captureDiagnosticSsa` copies a
borrowed span into an independently owned diagnostic sidecar only when
the fully validated rooted continuation, compiler source revision/digest,
leaf procedure and resume PC all match. Missing words, wrong digest/PC
and invalid widths fail transactionally without overwriting prior data.
The native revision smoke passed with original input buffer mutations.

Important: the runtime's LLVM codegen currently emits only native
activation-local diagnostic stores by default under the opt-in flag.
The new native-to-caller buffer transfer helper has an executable
standalone test, but it is NOT yet called from production generated
Scratch JIT code with a scheduler-owned destination. There is no
general SSA map authentication, GC handle or effect-position proof, and
no executable deopt authorization. No RT-001 source or CallFrame ABI changed.

## 2026-11-09: IR-001 pre-mutation dominance validation

The concurrent RT-001 branch already integrates an opt-in scheduler-owned
`NativeRtSsaCapture` before Fiber suspension. IR-001 does not alter that
RT-owned bridge. Instead, `llvm_ssa_spill.hpp` now checks that every
proposed live SSA definition dominates its checkpoint and that numeric
SSA IDs are strictly increasing before emitting **any** IR instructions.
The separate copy emitter also rejects a same-function destination
that is only defined after its checkpoint; function ownership alone
was insufficient. These checks prevent malformed/stale liveness data
from producing invalid LLVM IR.

Standalone LLVM23 JIT smoke: PASS, including future-defined numeric
value, duplicate SSA ID, same-function future destination, foreign
function destination, and actual external buffer transfer. Rejected
spill manifests did not mutate IR. Full codegen translation-unit
syntax check: PASS. No full integrated Scratch/RT parity run here.
The scheduler-owned diagnostic remains non-executable and does not
prove SSA effect continuity or GC liveness.

## 2026-11-09: IR-001 typed detached SSA manifest contract

Added `src/ir/source_typed_ssa.hpp` as a standalone, LLVM-free,
thread-independent representation of RT-001 tagged SSA descriptors.
`SourceTypedSsaManifest` binds source revision, two-word content digest,
procedure ID, resume PC, **checkpoint site ID** and a sorted sequence of
64-bit descriptors. Descriptor low 32 bits encode SSA/argument ID; upper
32 bits encode kind and width: integer instruction 0x000, float
instruction 0x100, integer argument 0x200, float argument 0x300.
Integer widths i1/i32/i64 and floating widths f32/f64 are accepted.
Unknown tags, duplicated/decreasing IDs, zero site IDs, invalid widths
and more than 256 words fail closed. `SourceTypedSsaSnapshot::capture()`
performs a synchronous deep copy into owned raw words and checks narrow
values are canonically zero padded. Float NaN payloads are copied exactly.

This is an **inert diagnostic value format**. Raw i64 words might encode
GC-sensitive Scratch Values; they are never object roots, pointer handles,
or evidence for SSA or effect-safe executable deoptimization. The RT-001
bridge currently has its own detached `NativeRtSsaCapture`; no RT code
was modified or automatically redirected to this new format.

Independent C++20 test `typed_ssa_manifest_smoke.cpp` passed with
`-Wall -Wextra -Werror -pedantic -fno-exceptions -fno-rtti`. Covers all
4 kinds, raw float NaN payload, source/procedure/PC/site mismatch,
invalid kind/width/order, size limits, and copy mutation isolation.
Registered a focused `xyo-ir001-typed-ssa-manifest-smoke` CTest target;
full shared CMake reconfiguration and JIT/RT integration were not rerun.

## 2026-11-09: Independent RT detached SSA verifier (IR-001)

New `src/ir/source_typed_ssa_bridge.hpp` provides
`verifyDetachedTypedSsa(state, expected)` as a read-only template.
The test includes the actual `NativeRtSsaCapture` in `thread.hpp`.
A separately held compiler manifest must match revision, digest,
procedure, PC, checkpoint site ID and ordered typed descriptors.
Checks include RT fingerprint, unresolved counter accounting and
the sum of seven pointer-origin counts; contradictory numericComplete
flags are denied. Success deep-copies words into a source typed SSA
snapshot; no runtime handoff is authorized. Fingerprint is not
cryptographic authentication. Digest validity now matches RT (one zero
half permitted, both zero forbidden). Standalone C++20 regression PASS
with real RT type; a pre-existing unused parameter warning in
variable.hpp was suppressed from -Werror. The registered CTest target
was not run through shared CMake and full parity was not tested.
No RT-001 source changes.

## 2026-11-09: IR-001 source boundary proof-gap audit

Added common, header-only `src/ir/source_deopt_audit.hpp`, which
combines `SourceResumePhase` / `SourceDeoptRequirements` with the
compiler-bound typed SSA diagnostic verifier. It reports missing proof
obligations (effect commit, LLVM-to-source live SSA mapping, exact
statement PC, suspended callee), invalid detached snapshot, and any
unresolved pointer origins. **This is diagnostic only**: `executable()`
is deliberately always false, regardless of a complete numeric SSA
snapshot. The existing RT-001 executor remains solely responsible for
any separately validated source-level handoff. A hash fingerprint is
never treated as authorization to replay effects.

The focused C++20 typed-SSA smoke PASS covers LoopLatch,
UnresolvedBody, SuspendedCaller, ProcedureEntry, missing/stale snapshots,
unresolved pointers and a fully numeric snapshot; all correctly retain
required effect/SSA obligations. Standalone LLVM SSA liveness smoke PASS.
Pre-existing unused parameter warning in variable.hpp excluded from
-Werror for the native RT-type test; no shared full CTest or Scratch
parity rerun. No RT-001-owned files modified or commit/push.

## 2026-11-09: IR-001 effect frontier diagnostics

Added source_effect_frontier.hpp: header-only dynamic effect sequence tracker requiring matching begin/commit PC and sequence, rejecting duplicate/skipped commits and checkpoints with outstanding effects. It binds source revision/digest, procedure, site, resume PC and phase to an observed committed count and checks an independently supplied host-journal count.

source_deopt_audit.hpp now adds EffectFrontierMismatch for inconsistent source/phase/site/PC/counts. Matching observations NEVER clear EffectCommit and NEVER authorize executable deopt; actual host delivery, statement atomicity and GC semantics remain unproved. This passive tracker is NOT connected to Runtime CommitOnceEffects.

Standalone C++20 source-effect test PASS with strict warnings; typed SSA plus frontier audit test PASS (except existing unused-parameter warning from variable.hpp). Registered focused CTest target; full project CTest and Scratch parity not run. No RT-001 code modified, no commit/push.

## 2026-11-09: IR-001 real commit-once journal observation

Added src/ir/source_effect_journal_bridge.hpp with
SourceEffectJournalBridge::publishAndDeliver(), a diagnostic adapter
between scheduler-owned CommitOnceEffects and the source effect frontier.
The adapter stages exactly one GC-rooted observable effect, obtains the
next dynamic source-effect sequence, asks the journal to publish the
scheduler state, then drains the observable using a synchronous/noexcept
callback. Only one matching successful delivery advances the frontier;
a failed publication cancels the still-unpublished sequence. Any
unexpected event, invalid GC root, missing delivery, backlog or
reentrant invocation fails closed with no source checkpoint permitted.
The staged block ID is independently copied before invoking the
publisher, so changes to a caller-owned string cannot mislabel delivery.

Important distinction: Slice::commit publishes the state and queues
an effect, but does not prove that the external dispatcher ran. This
bridge records success only after CommitOnceEffects::drain delivers it.
The caller must still supply an atomic state-publisher callback, and
this adapter does not authenticate a compiler-generated effect PC or
make arbitrary source-level replay safe. It is not integrated into the
main Runtime::journalObservable or compiled Scratch primitives.

New actual-GC integration test
tests/platform/source_effect_journal_bridge_smoke.cpp PASS, linked
against libxyo.a using existing commit_once_effects_smoke link inputs.
Covers publication rejection/rollback, stale slice ID rejection,
repeated dynamic source PC, GC collection during delivery, unprocessed
foreign event backlog rejection, reentrant publication denial,
mutable block-ID retention, unsupported op rejection and a combined
audit with the actual RT-001 NativeRtSsaCapture structure. Even when
the real journal callback count, typed SSA manifest, site ID and
effect frontier match, auditSourceDeoptWithEffectFrontier still reports
EffectCommit and LiveSsa gaps and executable()==false.

Registered focused native xyo-ir001-effect-journal-bridge-smoke CTest
target; it has not been built via shared CMake and full Scratch parity
has not been run. No RT-001-owned source files changed, no commit/push.

## 2026-11-09: IR-001 real compiled LLVM effect-source-PC diagnostics

XYO_IR001_TRACE_EFFECT_PC=1 enables opt-in source-location probes around
compiled event_broadcast and pen_stamp. Codegen computes the one-based
lexicographically sorted block-ID index from the frozen
ScratchCompilationUnit, matching the native SourceFrameSymbols PC scheme.
Each LLVM wrapper receives the compiled source block ID, PC, revision and
dual-word digest, executes the ORIGINAL synchronous helper once, and
logs Runtime journal publication/delivery deltas, immediate fallback
counts, slice delta and remaining pending/GC-rooted entries. The default
JIT path is unchanged when the flag is absent.

The runtime revision_matches flag checks the live Blocks revision and
presence of the block ID only. The compiled dual digest is emitted for
diagnostics, NOT revalidated against live source content at dispatch.
These observations are not a proof of effect atomicity, replay safety
or executable deoptimization.

Real LLVM E2E tests:
- tests/differential/ir001_effect_source_trace.py: two distinct broadcast
  source sites independently matched to exact PCs, plus three repeated
  dynamic executions of ONE loop-body block, all with the same source PC.
  Interpreter-visible results unchanged with journal on/off and the
  diagnostic disabled; broadcast journal deliveries match expectations.
- tests/differential/ir001_pen_effect_source_trace.py: two compiled
  pen_stamp source sites match exact PCs; each yields one published and
  delivered event, with no pending/GC-rooted journal entries; Scratch
  outcomes unchanged with tracing and journal toggled.

Both differential tests PASS with an isolated native binary built from
fresh codegen, LLVM backend, RT and RT-block objects, replacing members
in a COPY of the existing libxyo.a. The shared build was not modified.
The ordinary xyo-cli rebuild was blocked by unrelated concurrent
renderer sdl_renderer.hpp: unresolved render::DlssOpaqueCoverage.
CI/whole-tree CTest and broader parity remain unverified. Two focused
CTest registrations were added. No commit or push.

Additional E2E validation: existing tests/differential/runtime_broadcast_journal.py PASS against the isolated native binary (interpreter/LLVM/LLVM+journal all equal; 2 committed, 0 immediate, 0 pending, 0 roots). Existing tests/differential/runtime_pen_journal.py PASS for interpreter/LLVM and journal on/off (2 committed pen effects, 0 immediate). The new repeated-loop case confirms 3 dynamic deliveries from one immutable source-PC block and x=33, done=1.

## 2026-11-09: IR-001 exact host dispatch provenance -> native loop checkpoint

New IR-only headers:
- src/ir/source_effect_dispatch.hpp: a stack-scoped, thread-local source
  origin used during ONE compiled observable operation. It claims one
  source-tagged CommitOnceEffects record, records the exact journal
  publication slice/revision, and accepts delivery only when the journal
  dispatcher actually invokes its matching operation/block label.
  Nested compiled effect scopes restore their parent's source context;
  unrelated/reentrant fallback events cannot borrow its label.
- src/ir/source_effect_runtime_trace.hpp: a compact per-Thread owned
  diagnostic timeline. It keeps only the latest delivered site, an
  increasing observed-delivery sequence, and an optional source-loop
  checkpoint. Missing/immediate delivery, stale/nonmonotone slice/revision,
  source digest or revision mismatch invalidates the timeline. The
  registry is indexed by Thread address, protected by a mutex, and
  cleared by Thread::releaseFinished and ~Thread before pointer reuse.
  Thread layout/ABI is not extended. Cleanup does not touch the registry
  when XYO_IR001_TRACE_EFFECT_PC is disabled.

The real Runtime::journalBroadcast and Runtime::journalObservable
publish one source-tagged block label when a scoped origin is active.
Both call noteSourceEffectDelivered() AFTER their existing synchronous
dispatcher callbacks; failed publication or immediate delivery does not
produce an exact-delivery certificate. No host-effect semantics or FIFO
order changes and no source labels are required in default execution.

The existing opt-in LLVM helpers for event_broadcast and pen_stamp now
send independently attributed deliveries into this scheduler-Thread
timeline. At a portable native loop checkpoint, generated LLVM calls
xyo_ir001_effect_checkpoint AFTER the normal root CallFrame checkpoint.
It requires a real verified root session, an exact loop PC and phase,
and NativeProcedureFrameSession::sourceContentMatches() (live captured
source content digest, not revision alone). A successful checkpoint
copies source digest, original effect block/PC, journal slice, observed
dynamic sequence and loop PC into scheduler-owned diagnostic storage.
There is NO detachment to an executable RT-001 handoff; the checkpoint
has executable=0 and uninstrumented effects / active callees / GC roots /
SSA-to-source mapping are still unproven.

Actual native LLVM root-loop integration under:
  XYO_IR001_TRACE_EFFECT_PC=1
  XYO_JIT_CALLFRAME=1
  XYO_RT_ROOT_ONLY_DEOPT=1
  XYO_EFFECT_JOURNAL=1

One Scratch broadcast block (source PC=6) was dispatched three times in
a repeat; the corresponding root loop checkpoint (PC=3) observed
(captured=1, sequence=1, slice=1),
(captured=1, sequence=2, slice=2),
(captured=1, sequence=3, slice=3); final x=33, done=1.
With the journal disabled, the same Scratch outcome was preserved and
all three diagnostic checkpoints were rejected (captured=0).

Verification:
- Clean native headless CMake build of xyo-cli: PASS. The existing
  unrelated sdl_renderer.hpp error (DlssOpaqueCoverage declaration)
  required an isolated, compiler-only forced include for that object;
  no renderer source was edited.
- tests/differential/ir001_effect_source_trace.py PASS: exact
  dispatch tags, two distinct broadcast blocks, repeated loop source PC,
  strict root checkpoint and journal-off denial.
- tests/differential/ir001_pen_effect_source_trace.py PASS: two exact
  pen stamp source event deliveries.
- Existing runtime_broadcast_journal.py and runtime_pen_journal.py:
  PASS across native Interpreter/LLVM and journal on/off.
- tests/platform/source_effect_dispatch_smoke.cpp PASS with strict
  C++20 warnings: nested attribution, stale slice, source mismatch,
  copied checkpoint and registry address-reuse hygiene.
- Focused CTest xyo-ir001-effect-dispatch-smoke: 1/1 PASS.
- git diff --check: PASS; full Scratch parity/CI was not run.
No RT-001 executable deopt permission added. No commit/push.

## 2026-11-09: IR-001 fail-closed host effect epoch validation

Hardened src/ir/source_effect_dispatch.hpp to flag a mismatched
observation when two effects stage under one native source wrapper, when
a differently typed effect stages in the same scope, or when a nested
compiled source scope appears. Foreign published/delivered labels and
immediate fallback deliveries likewise invalidate the diagnostic. The
real host Runtime::journalBroadcast / ::journalObservable now pass the
ACTUALLY STAGED block ID and slice ID to noteSourceEffectPublished,
rather than reconstructing them from the current thread-local source
scope after publication.

Added SourceEffectHostEpoch: journal slice and revision, immediate
broadcast/effect counters, pending event count, rooted count, and
journal presence. LLVM's synchronous wrapper captures this AFTER the
host dispatch, and a root loop source checkpoint captures it again.
Exact delivery additionally requires a quiescent journal and matching
slice/revision. SourceEffectTimeline rejects:
- intervening journal slices or immediate fallbacks between two
  attributed effects (requires contiguous slice/revision sequence);
- an intervening uninstrumented event after the last attributed effect
  and before a source checkpoint;
- stale/foreign sites, unsupported source boundaries and missing roots.

These fail-closed checks are deliberately stronger than minimum
diagnostics. Runtime-independent variables/lists/graphics changes can
still escape the journal, so this is NOT an exhaustive Scratch effect
certificate, does NOT prove source-level LLVM SSA or GC reconstruction,
and must NEVER authorize executable deoptimization.

The LLVM broadcast regression now generates a genuine Scratch repeat
containing event_broadcast followed by an UNINSTRUMENTED looks_say. The
same compiled source and verified root CallFrame still exist, but the
host journal revision changes and every checkpoint is rejected
(captured=0, source_owner=1, executable=0). Instrumentation on/off
preserves Scratch's output. Two other generated broadcast source sites,
three normally repeated broadcasts, and pen_stamp attribution pass.

Testing with a freshly built isolated headless native xyo-cli:
- IR-001 live broadcast differential PASS, including the untraced
  looks_say and journal-off denial;
- IR-001 live pen differential PASS;
- Existing native interpreter/LLVM broadcast and pen journal parity
  PASS with journal on/off;
- Strict C++20 source_effect_dispatch_smoke PASS: nested source
  attribution, foreign staged/delivered events, duplicate operations,
  intervening slice/revision and immediate count drift, thread registry
  reuse;
- Focused xyo-ir001-effect-dispatch-smoke CTest 1/1 PASS;
- git diff --check PASS.

Build note: Concurrent SDL renderer sources still require a temporary
compiler-only forced include of dlss_opaque_coverage.hpp for their
isolated compilation; no renderer source edited by this work. In the
shared codegen.cpp, an unrelated RT-001 local vector named generated
shadowed the llvm::Function generated and broke the build. Renamed ONLY
that local vector to generatedStrings (no behavior change), allowing
the complete isolated native xyo-cli build to pass. Full CI and total
Scratch parity remain unverified. No commit/push.

## 2026-11-09: IR-001 nonjournal native mutation guard

IR-001 2026-11-09: Opt-in native VM mutation barrier. Normal LLVM emitStackBlock emits xyo_ir001_vm_write(ctx) before data_setvariableto, data_changevariableby, data_addtolist, data_deleteoflist, data_deletealloflist, data_insertatlist, data_replaceitemoflist when XYO_IR001_TRACE_EFFECT_PC=1. Only possible same-Thread writes are flagged; Scratch state/semantics unaffected. After an attributed host delivery, a potential VM write invalidates diagnostic source-checkpoint capture. A write after a successful prior checkpoint expires the old record, and a subsequent attributed delivery can begin a new diagnostic interval. Real LLVM repeat with broadcast then data_changevariableby: captured=0 at all 3 checkpoints, x=39 parity unchanged; broadcast then data_addtolist: captured=0 at all 3 checkpoints, output and dispatch parity unchanged. Previously observed 3-iteration broadcast with earlier variable write still captures seq 1/2/3. Native C++20 smoke and focused CTest 1/1 PASS; existing broadcast/pen JIT and interpreter journal parity PASS; git diff --check PASS. This covers only ordinary LLVM statement emission, not hoisted stores, specialized GPU/parallel loops, interpreter receivers, reporter effects, GC or replay safety. No executable deopt, no full CI, no commit/push. Renderer build needed compiler-only forced include for unrelated DLSS type declaration; no renderer source modified.

## 2026-11-09: IR-001 optimized LLVM VM-write fences

The ordinary LLVM statement-level xyo_ir001_vm_write fence was not
sufficient: specialized SSA reduction and list fast-path lowering can
write Scratch-visible variables without entering emitStackBlock for
each statement. Introduced a common Codegen::notePossibleNativeVmWrite()
and called it from storeNumericPair, emitVarSet, emitVarChange,
emitListAdd, and emitListReplace. The calls emit ONLY for native JIT
with XYO_IR001_TRACE_EFFECT_PC=1, and are conservative: a no-op or
duplicate numeric shadow update may also invalidate a diagnosis.

Checkpoint logging now exposes vm_write_pending separately from
captured and source_owner. In a generated real Scratch repeat with
event_broadcast followed by control_for_each summation, LLVM logs
"foreach SSA sum b0007" (optimized path actually selected) and the
subsequent checkpoints explicitly show vm_write_pending=1.
In this nested case source_owner=0 too, so the captured=0 condition
alone is not proof of barrier coverage. x=33, acc=18 and host-journal
statistics remain identical with or without diagnostics.

Native LLVM broadcast and pen source-PC/commit journal differential
tests PASS, plus old interpreter/LLVM journal parity. Dedicated
jit_foreach_ssa.py PASS across ten warp/nonwarp eager/hotspot
SSA/fallback and LLVM -O2 cases. Focused CTest 1/1 PASS.

Remaining unproven: custom GPU kernel stores, special par-for runtime
mutations, interpreter-side listeners, other Scratch stateful APIs,
arbitrary nonjournal reporters, GC roots, deopt replay equivalence.
This is not an executable deoptimization proof.
An unrelated concurrent renderer header needed a compiler-only include
during isolated headless compilation; renderer sources were not changed.
No full CI, no commit/push.

Additional tests/differential/jit_repeat_copy_ssa.py PASS in ten warp/nonwarp eager/hotspot optimized/fallback and LLVM -O2 runs with the same rebuilt native binary.

## 2026-11-09: IR-001 post-LLVM-optimizer source-effect write audit

Added src/ir/llvm_effect_write_audit.hpp with a conservative
intra-procedural forward CFG may-analysis after LLVM O1/O2/O3 has run.
The two-state analysis tracks whether a traced broadcast/pen delivery
could have happened on the current path without a subsequent
xyo_ir001_vm_write guard. It explicitly joins branch predecessors and
iterates through loop backedges to a fixed point. It counts unguarded
StoreInst/atomic writes/llvm.mem intrinsic operations, and unknown
memory-writing/indirect helper calls separately. Stack-candidate writes
are counted conservatively: their pointer may escape. Functions with
checkpoint calls but no locally observed source effect are identified
as unproven across function boundaries.

With BOTH XYO_IR001_TRACE_EFFECT_PC=1 and
XYO_IR001_OPT_WRITE_AUDIT=1, the LLVM backend performs this check
AFTER optimization in eager, prepared async and lazy transform paths.
It reports the findings with [ir001-postopt-write], executable=0.
For each function with an uncovered write, opaque call, or a checkpoint
without a local source effect, it inserts a call to
xyo_ir001_vm_write immediately BEFORE each xyo_ir001_effect_checkpoint.
This expires the runtime Thread-owned diagnostic timeline, instead of
mistakenly accepting an unproven source checkpoint. The check never
suppresses Scratch effects and is disabled by default.

Real O2 Scratch counted repeat sample: one traced broadcast,
six existing write-fence calls, eight LLVM store instructions,
five potentially unguarded stores (four stack candidates), one
unclassified memory-writing helper call. One checkpoint was fenced.
With the audit enabled, each of three subsequent source checkpoints
reports captured=0, source_owner=1, vm_write_pending=1; the same
Scratch variables x=33/done=1 and journal results remain unchanged.
With the audit disabled, the three original diagnostic checkpoints
continue to report captured=1 with sequences 1/2/3.
In O3 the live differential test reports six fenced checkpoints and
the same Scratch-visible results.

New regression tests:
- tests/platform/llvm_effect_write_audit_smoke.cpp: constructs real
  LLVM CFGs with a correctly guarded store, an unguarded store,
  a one-sided branch guard, a loop backedge carrying unguarded state,
  an opaque memory-writing call, a stack store and a checkpoint-only
  callee. The exact same cases are checked AFTER the real LLVM O2
  optimization pipeline. It verifies that fail-closed call insertion
  affects the unproven functions, not the guarded one, and that the
  resulting LLVM module verifies.
- tests/differential/ir001_postopt_write_audit.py: actual native xyo
  JIT O2/O3 compiling and executing an SB3 counted repeat with opt-in
  write audit on/off. Requires injected fail-closed checkpoints and
  exact Scratch variable / journal parity.
- CTest targets xyo-ir001-postopt-write-audit-smoke and
  xyo-ir001-postopt-write-audit-parity registered.

Observed: standalone LLVM O2 smoke PASS; actual native O2/O3
differential PASS; existing broadcast + pen live IR-001 differential
PASS; focused IR-001 CTest smoke PASS; full CI not run.

Limits: CFG analysis is intra-procedural and deliberately errs toward
unverified. A zero report is NOT proof of all Scratch-visible writes,
interpreter/GC side effects, native alias/non-escaping roots, inter-
procedural effects, or sound source deoptimization. Nothing makes
executable deopt eligible. No commit or push.

## 2026-11-09: IR-001 non-escaping stack classification and per-checkpoint precision

Post-optimizer LLVM write auditing distinguishes a narrow class of
nonvolatile/non-atomic stores through alloca-backed pointers when
the entire allocation passes LLVM PointerMayBeCaptured(..., true).
getUnderlyingObject handles GEP-derived pointers. Passing a derived
pointer to an opaque native helper captures its parent allocation,
so that stack write remains unproven. Volatile/atomic/escaped stack
stores, unknown writes and opaque calls are not reclassified.
local_stack, isolated_stack and unguarded_isolated_stack are separate
diagnostic counters. A nonescaping alloca can still carry source SSA
state needed for native deoptimization: it is not a deopt certificate.

The previous post-opt injection invalidated ALL source checkpoints
inside a function with ANY unguarded memory write. The new
findUnprovenLlvmEffectCheckpoints(Function&) tracks three may-state
bits through the optimized LLVM CFG: an active source delivery
interval, an unguarded potential write and a path missing any newly
observed source event. It computes the fixpoint through branch joins
and loop backedges and inserts a runtime VM-write rejection only at
locally unproven checkpoint calls. Existing runtime write fences
already invalidate previous unguarded evidence. An immediate
preceding xyo_ir001_vm_write prevents duplicate insertion when
auditing the same module twice.

LLVM O2 smoke PASS, including two source checkpoints within ONE LLVM
function: an unguarded first interval gets precisely one denial
marker, while the later independently guarded interval does not.
Other unit cases cover escaped vs nonescaping stack locals,
GEP-derived pointers with/without escape, volatile storage,
one-sided branch guards, loop-carried paths, opaque calls,
checkpoint-only functions and LLVM module verification. A second
post-opt injection pass inserts zero duplicate markers.

Fresh native LLVM O2/O3 Scratch differential PASS with audit on/off:
identical Scratch output and journal deliveries. The O2 sample has
events=1 checkpoints=1 barriers=6 writes=8, unguarded=5,
unguarded_opaque=1, local_stack=4, isolated_stack=0 and fenced=1.
The zero isolated count is deliberate fail-closed precision: none
of this sample's stack candidates qualified as nonescaping.
O3 reports 6 fenced checkpoints with unchanged visible results.
Live broadcast and pen JIT source-PC tests and interpreter/LLVM
journal parity PASS. Focused CTest 2/2 PASS; git diff --check PASS.
No complete SSA, GC, cross-function or external Scratch write proof;
no executable deopt. No commit or push.

## 2026-11-09: IR-001 conservative interprocedural LLVM helper summaries

The post-LLVM-optimizer source-effect write audit now maintains
LlvmLocallyPureCallees, an opt-in, memoized summary of direct calls to
internal/private non-variadic LLVM functions. An internal callee may be
considered locally non-observable only when all REACHABLE instructions
consist of proven non-escaping nonvolatile/nonatomic alloca stores,
non-side-effecting LLVM operations, or direct calls to another similarly
proven non-throwing internal function. Escaping/global writes,
memintrinsics/atomics, foreign side-effecting calls, inline asm, source
effect/VM-write/checkpoint/CallFrame hooks, exceptions and recursion
are not accepted. Cyclic call graphs fail closed through a Visiting
state, and indirect/external calls are not independently certified.

The module-level audit and checkpoint-specific CFG analysis reuse this
summary to avoid treating proven pure internal calls as unknown native
writes. A new proven_local_calls diagnostic counter reports the number
of such optimized-module callsites; it never grants executable
deoptimization. A readonly external call that can throw is still
classified as opaque: LLVM readonly memory effects alone do not prove
the absence of host-visible side effects.

Focused LLVM O2 regression fixtures now cover:
- internal leaf with solely nonescaping stack state and a scalar return;
- another internal helper calling the pure leaf transitively;
- an internal callee storing through a caller-provided pointer;
- an internal callee invoking xyo_ir001_broadcast_v_source;
- self recursion, and an indirect call through a function pointer;
- a readonly-but-possibly-throwing external function;
- the previous missing/barrier CFG, stack escape/GEP, branch, loop and
  two-independent-checkpoint tests.
The pure chain is not fenced, while external/nested/indirect/escaping
and source-unknown callsites are rejected. The same test runs the
actual LLVM O2 pass pipeline and verifies the resulting LLVM module
and fail-closed write-fence insertion. LLVM O2 standalone and focused
CTest 2/2 PASS.

Native xyo-cli was rebuilt in the isolated headless CMake directory.
Real native LLVM O2/O3 postopt source-effect parity PASS:
O2 fenced=1 and O3 fenced=6 in the existing Scratch counted repeat.
IR-001 broadcast and pen compiled-source PC differentials PASS, as do
the original interpreter/LLVM broadcast and pen journal parity tests.
Existing for-each SSA and repeat-copy SSA ten-mode regression suites
also PASS. Compilation errors were not introduced by this work.

This interprocedural analysis proves only a narrow absence of
externally visible writes in selected internal LLVM callees. It is
NOT an effect-summary proof for arbitrary indirect/foreign calls,
cross-thread writes, GC/rooting, source-variable snapshots, native
exception handling, SSA recovery or runnable deoptimization. IR-001
remains in_progress, audit opt-in only. Full CI not run; no commit/push.

## 2026-11-09: IR-001 hardening of external call attributes and memory-order effects

Updated src/ir/llvm_effect_write_audit.hpp: post-optimization
checkpoint auditing no longer assumes that an unresolved external or
native function is harmless based on LLVM readonly, readnone or
nounwind attributes. Only explicitly known IR-001 diagnostic helpers,
side-effect-free compiler-owned LLVM intrinsics, and proved
non-escaping/internal callees can be locally disregarded.
All other foreign/indirect calls remain opaque, even if the IR claims
memory(none) and nounwind. This makes the source-effect audit
independent of unverified external host-function attributes.

Conservative MemoryWrite diagnostics additionally count volatile and
atomic LoadInst and FenceInst as potentially externally observable
memory/synchronization events. The existing writes diagnostic counter
therefore includes these non-store effects. In the interprocedural
purity analysis a reachable volatile/atomic load or fence prevents a
helper from being marked pure; transitive callers also lose proof.

Extended actual LLVM O2 unit regression:
- external readnone + nounwind i64-returning call remains opaque and
  receives a source-checkpoint denial marker;
- an internal, nounwind wrapper that returns that external call
  remains unproven (no false pure call summary);
- volatile load, acquire atomic load and sequentially consistent fence
  each trigger checkpoint denial even without an LLVM StoreInst;
- an internal helper performing a volatile load invalidates its
  caller's source checkpoint;
- a side-effect-free LLVM sqrt intrinsic is NOT mistaken for an
  unknown external callback (no extraneous denial marker).
All tests also run real LLVM O2 module optimization and verify exact
post-opt checkpoint marker insertion and module validity.
xyo-ir001-postopt-write-audit-smoke and
xyo-ir001-effect-dispatch-smoke CTest 2/2 PASS, strict C++20 compile
-Wall -Wextra -Werror PASS, git diff --check PASS.

Native build limitation: concurrently edited unrelated sources currently
reference absent analysis/numeric_region_lower.hpp and
jit/numeric_region_ssa.inc. The fresh native full build stops at these
missing files; IR-001 backend itself compiled successfully in that
attempt. Replacing ONLY the backend in the prior archive linked but
produced a JIT runtime SIGSEGV due to a mixed object state; that
isolated executable is NOT accepted as native parity evidence.
The prior unchanged 21:10 native binary still passes O2/O3 source
audit parity tests, but it does NOT contain this latest hardening and
must not be claimed as a fresh end-to-end pass.

The new audit is diagnostic-only and gated behind
XYO_IR001_TRACE_EFFECT_PC=1 + XYO_IR001_OPT_WRITE_AUDIT=1.
It neither certifies whole-program effect replay nor permits runnable
deoptimization. Interpreter/foreign/GC state, source-local SSA and
cross-thread effects remain unverified. No commit or push.

Build blocker update at 2026-11-09 ~22:00 JST:
analysis/numeric_region_lower.hpp and jit/numeric_region_ssa.inc were
subsequently created by the concurrent work, but numeric_region_lower.hpp
now includes the still-missing ir/numeric_region.hpp. The fresh native
full build remains blocked there. Reported the exact missing dependency
to MATH-019 via tools/board.ts; IR-001 did not modify the concurrent
numeric-region implementation. Strict LLVM O2 standalone and CTest 2/2
are the completed validations of the newest patch; the full native
O2/O3 differential remains NOT run on that newest patch.

## 2026-11-09: IR-001 completed-GC and cross-Thread native VM-write epochs

Added a read-only, atomic Gc::collectionGeneration() accessor for the
existing poolGen_ generation, which is advanced by the real GC after
each completed sweep/slab-registry rebuild. IR-001's SourceEffectHostEpoch
now contains this generation and the existing atomic
Gc::explicitCollectionPending() flag. The native broadcast/pen effect
wrapper captures them AFTER exact host delivery, and the native root
checkpoint captures them AGAIN. A changed GC generation, a missing
generation or an outstanding deferred explicit collection makes the
host epoch nonquiescent or mismatched and rejects the source diagnostic.
This is not a GC root or native SSA-liveness certificate.

NativeSourceEffectTimelineRegistry now also maintains a process-wide
monotonic VM-write epoch under its existing mutex. Every opt-in native
xyo_ir001_vm_write call advances the epoch, EVEN if that source Thread
does not have its own registered diagnostic timeline. Upon a tracked
Thread's next source checkpoint, a mismatch from its observed delivery
epoch conservatively invalidates the source record (foreign_vm_write=1,
vm_write_pending=1). A mismatch BETWEEN two delivered source events
without an intervening checkpoint is also rejected; a write AFTER an
already completed checkpoint but BEFORE the next independent delivery
is allowed to precede the new interval. Saturated uint64_t epochs
remain permanently fail-closed. This only detects LLVM JIT write-fence
calls, not arbitrary interpreter writes or native callbacks. Across
different Runtime objects it may reject unnecessarily; those false
positives are preferable to unproven source replay.

Validation:
- Strict C++20 source_effect_dispatch_smoke PASS, including GC
  generation drift, deferred explicit GC, cross-Thread Scratch
  identity mutation, journal drift and pending/complete boundaries.
- Added tests/platform/source_effect_gc_epoch_smoke.cpp. It invokes
  the REAL Gc::collect() and confirms the actual generation changes,
  invalidates the pending diagnostic without a journal change, then
  starts a fresh post-GC interval. It also starts a real std::thread
  whose potential VM write invalidates the main Thread's record.
  Standalone test linked against native libxyo.a PASS.
- A fresh full headless xyo-cli build PASS. IR-001 live LLVM
  broadcast and pen source-PC tests PASS; O2/O3 postopt audit + native
  Scratch output/journal parity PASS; interpreter/LLVM broadcast/pen
  journal parity PASS.
- A new CTest xyo-ir001-gc-cross-thread-smoke target is registered
  and links xyo + Threads::Threads. Focused CTest reconfiguration and
  native target build were requested to validate registration.

Remaining scope: completed GC-generation matches do NOT prove that
all JIT registers, raw object addresses or GC-managed Scratch values
are precisely rooted or can be rebuilt from source SSA. The epoch
records no per-object root identity and cannot prove object lifetime.
Some native memory writes and every uninstrumented interpreter write
can escape the process-wide JIT-only write marker. Diagnostic-only;
no executable source-level deopt, commit or push.

Final real-GC regression update: source_effect_gc_epoch_smoke.cpp now also exercises the actual Gc::DeferExplicitCollect protocol: collect() is deferred, explicitCollectionPending() becomes true without changing collectionGeneration(), the held source checkpoint is denied, and collectIfNeeded() after leaving the defer scope drains the request, clears pending and changes generation. The standalone program also uses an actual std::thread to advance the process-wide VM-write epoch while another Thread holds an attributed source record. Registered and built CTest xyo-ir001-gc-cross-thread-smoke PASS, alongside xyo-ir001-postopt-write-audit-smoke and xyo-ir001-effect-dispatch-smoke (3/3 PASS). Fresh native headless JIT binary built and broadcast/pen source-PC, O2/O3 postopt, and interpreter/LLVM journal parity PASS. No executable deopt, no full CI, no commit/push.

## 2026-11-09: IR-001 flat interpreter variable/list write interop

Integrated native JIT source-effect diagnostics with the actual flat
interpreter's central primitive dispatch in src/engine/execute.cpp.
The new pure opcode predicate in
src/ir/source_interpreter_mutations.hpp recognizes the seven native
Scratch variable/list mutators: data_setvariableto,
data_changevariableby, data_addtolist, data_deleteoflist,
data_deletealloflist, data_insertatlist and data_replaceitemoflist.
Only with XYO_IR001_TRACE_EFFECT_PC=1, immediately before execution
of one of those primitives, the interpreter notifies
NativeSourceEffectTimelineRegistry::noteInterpreterMutation(thread).
It advances the existing global potential-VM-write epoch, plus an
independent interpreter-write epoch. The latter is reported as
interp_vm_write=1 at root source checkpoints, alongside the older
foreign_vm_write and vm_write_pending flags. The instrumented opcode
still executes exactly as before; any no-op or invalid list index is
intentionally a possible false-positive denial. All default
diagnostic-off execution remains unchanged apart from the disabled
flag branch. Extra per-opcode logging requires BOTH the main flag and
XYO_IR001_TRACE_INTERP_WRITE=1.

The central interpreter can use a separate nested-WASM promotion path
that returns before running individual primitives. The opt-in native
source-diagnostic timeline is conservatively expired before attempting
that path, rather than silently claiming all optimized mutations were
observed. Neither interpreter mutations nor nested WASM can enable
executable deoptimization.

A synchronous compiled source-effect wrapper may itself execute
nested interpreted or native stateful callbacks BEFORE the global
write epoch is sampled. Added noteSourceEffectUnjournaledMutation()
to the thread-local ScopedSourceEffectDispatch attribution scope.
Both the interpreter primitive barrier and xyo_ir001_vm_write notify
it. If any such mutation occurs inside the scope, exactDelivery()
becomes false even when the original journal event was published and
delivered precisely once. A unit regression verifies this nested
callback contamination without affecting events outside the scope.

New native differential test
tests/differential/ir001_interpreter_mutation_trace.py executes a
generated real SB3 through --jit off; all 7 recognized mutator opcodes
are actually executed (9 primitive calls including repeated set/add),
the diagnostic lines contain the exact opcode counts, and traced
and untraced Scratch variables are identical (x=2, done=1).
The optional diagnostic event lines do NOT appear if the main flag is
disabled, even if the verbose interpreter write flag is set.

A second new native differential test
tests/differential/ir001_hotspot_interpreter_interleaving.py uses
HotSpot with an LLVM-compiled counted repeat sender and a cold
interpreted broadcast receiver. A control_wait (0) after broadcast
yields to the receiver BEFORE the sender's source loop checkpoint.
In each of 3 iterations the receiver genuinely executes interpreted
data_changevariableby; the JIT's 3 loop checkpoints report
source_owner=1, captured=0, vm_write_pending=1,
foreign_vm_write=1 and interp_vm_write=1. The broadcast journal still
reports three committed deliveries and no pending/rooted payloads.
Scratch output remains x=33/done=1 with diagnostics on/off.
A source event after delivery can remain exact_delivery=1 even when
the later interpreted receiver mutation invalidates the checkpoint,
which is the intended distinction between delivery provenance and
source-state replay safety.

CTest registrations:
xyo-ir001-interpreter-mutation-parity and
xyo-ir001-hotspot-interpreter-interleaving. Reconfigured the isolated
native headless test build with xyo-cli and executed the eight focused
IR-001 CTests, including those new real differential tests, both
broadcast/pen source-PC tests, O2/O3 postopt parity, and GC/cross-thread
smokes: 8/8 PASS. The fresh native xyo-cli also rebuilt successfully.
Standalone strict C++20 registry smoke PASS, existing native
interpreter/LLVM broadcast and pen journal parity PASS, and
git diff --check PASS.

Limitations: this detects the seven interpreter data mutators, NOT
all interpreter-side or callback-side Scratch state changes,
external/extension writes, stateful reporters or arbitrary native
memory mutations. Nested-WASM is conservatively denied rather than
proven complete. The global epoch may reject unrelated Runtime
writes. No exact GC roots, full source SSA restoration, and no
executable native deopt. Full CI not run. No commit or push.

## 2026-11-09: IR-001 widened built-in interpreter mutation coverage and delivery-start race guard

Expanded opt-in flat interpreter mutation classification from seven data variable/list mutators to a conservatively explicit table of additional Scratch built-ins: motion geometry/rotation, looks and costume/visibility/layer/bubbles/backdrops, pen drawing/style, sound playback/effects/volume, monitor visibility, and selected scheduler/sensing state. Read-only Scratch reporters, operators and local control loops remain excluded. Recognition is in src/ir/source_interpreter_mutations.hpp and the central execute() dispatcher still only records a potential write when XYO_IR001_TRACE_EFFECT_PC=1; diagnostics do not modify the primitive result or ordering. Unknown extension/native callback effects remain unproven and this list is not an exhaustive effect certificate.

Extended the live HotSpot LLVM sender / cold interpreted broadcast receiver test with a separate real-Scratch SB3. The receiver executes motion_setx(42), stores the observed motion_xposition reporter in a variable, then looks_hide and pen_penDown. The sender yields after broadcast, so the receiver's state changes occur BEFORE its native repeat checkpoint. Three iterations produce twelve interpreted mutation records (motion_setx, data_setvariableto, looks_hide, pen_penDown each repeated three times), all three native checkpoints deny (captured=0, source_owner=1, vm_write_pending=1, interp_vm_write=1), while the measured Scratch position is 42 and journal deliveries are 3/3 with zero pending or rooted payloads. All visible variables and effect-journal stats match trace-off execution. The pre-existing data_changevariableby receiver also yields three denied checkpoints and retains parity. Classifier static assertions additionally cover sound_setvolumeto, sensing_resettimer, pen_stamp, event_broadcastandwait, clone operations, and read-only negative cases.

Closed another race: a different OS thread may mutate Scratch DURING the synchronous host effect, before the source-effect wrapper records the mutation epoch. NativeSourceEffectTimelineRegistry::mutationEpochSnapshot() now obtains the global potential-write epoch BEFORE the real perform() call. After delivery the new observeDuringDelivery() checks the snapshot and registers the resulting source timeline while holding one mutex. A mismatch marks the delivery observation as invalid (exact_delivery=0), not just a later checkpoint. A saturated generation fails closed. Existing thread-local scoped mutation tracking still detects same-thread nested host callbacks. Unit tests cover clean delivery, intervening interpreted write, intervening JIT write, both forced rejections, and the actual worker-OS-thread test checks concurrent mutation before registration. The method remains a diagnostic and never executes deopt.

Fresh isolated native headless xyo-cli rebuilt successfully; the 8 focused IR-001 CTests all PASS (broadcast source PC, pen source PC, post-opt smoke/parity O2+O3, interpreter all-seven mutation parity, mixed HotSpot receiver interleaving including new visual case, effect dispatch smoke, and real-GC / OS-thread smoke). Existing native interpreter/LLVM broadcast and pen journal parity PASS. Standalone C++20 -Wall -Wextra -Werror smoke PASS, Python scripts compile, git diff --check PASS. No broad/full CI run, no commit or push. Outstanding: arbitrary custom extension/native callbacks, other untracked interpreter stateful reporter paths, exact GC object roots and LLVM SSA reconstruction remain unproven.

Additional host-delivery GC guard: the compiled source-effect wrapper now also captures Gc::collectionGeneration() just BEFORE its synchronous perform() call and passes it to observeDuringDelivery(). If the completed-collection generation changed before recording the host receipt, that receipt is marked mismatch and cannot be called exact_delivery=1, even if the later loop checkpoint observes the same post-delivery generation. The registry checks the VM-write epoch and before/after GC generation before accepting the new source timeline, under its existing lock. The real-GC CTest invokes an actual collector between the pre-delivery snapshot and registration; it fails closed without any intervening VM-write. C++20 source-effect unit checks the same drift synthetically. Native LLVM JIT and all eight focused IR-001 CTests rebuilt and PASS after this revision.

## 2026-11-09: IR-001 detached LLVM SSA / exact GC root coverage

Added src/ir/source_ssa_gc_audit.hpp, a read-only common-IR
diagnostic for the NativeRtSsaCapture format already produced by
RT-001. It first verifies the caller's independently supplied
SourceTypedSsaManifest and the captured word/descriptors/fingerprint,
then checks the original and copied generated-string masks and
SSA-argument alias masks. For compiler-certified generated i64
Scratch strings, it resolves ONLY previously retained RT-owned
RootedValueHandles, checks bit-for-bit identity with the saved word,
and checks exact GcHeader allocation membership plus GcType::Str
BEFORE accessing any object type. It never registers new roots or
dereferences an arbitrary NaN-boxed pointer to manufacture evidence.

The audit reports separate counts and gaps for:
- certified generated strings with exact rooted heap handles;
- immediate empty strings needing no heap roots;
- unproven boxed-string-looking i64 values (not dereferenced);
- boxed string values from LLVM argument aliases needing the distinct,
  independently GC-rooted Scratch CallFrame (unverified by this audit);
- unresolved native LLVM pointers and other unsupported SSA;
- missing, truncated or rewritten compiler masks, invalid/released
  handle slots, and missing source-local SSA mappings.

The rooted helper proves only CONSISTENCY of a known generated-string
subset. Even a clean known-generated-root audit is not a complete
source-level recovery or live-register safety proof. SourceDeoptGap
now includes GcRootContinuity alongside LiveSsa wherever SSA liveness
is required, and it remains unproven. source_typed_ssa_bridge.hpp
also rejects impossible stackDerivedPointers/distinctStackAllocas
metadata even when its local fingerprint was recomputed.

New native test tests/platform/source_ssa_gc_audit_smoke.cpp
obtains a real GC-allocated string, retains it through RT-001's
exact native generated-SSA root helper, runs real full GC twice
while one detached snapshot keeps the shared provider alive, and
verifies the ROOTED word remains intact. It rejects stale source
site IDs, mutated words with repaired FNV, missing/rewritten masks,
misplaced or released handles, and an artificially minted handle
whose value contains an arbitrary forged string pointer not
present in the GC allocation registry. It also verifies that
floating SSA bits resembling string tags are NOT interpreted as
boxed values, while unrooted i64 string candidates, source argument
aliases, and outstanding native pointers remain explicit gaps.

Registered xyo-ir001-ssa-gc-audit-smoke in native CMake CTest
(linking xyo for its collector). After the final exact GC membership
check, all 12 xyo-ir001 focused CTests AND
xyo-rt001-generated-ssa-roots-smoke were rerun: 13/13 PASS.
The native JIT broadcast/pen, O2/O3 postopt parity and mixed
HotSpot/interpreter cases are included in that selection.
The unrelated existing SoundBankAPI unused-parameter warning requires
-Wno-error=unused-parameter for standalone strict C++20 test build;
the CMake target compiles and runs cleanly.
git diff --check PASS. No full CI run; no commit/push.

Remaining: independently authenticated source-level mappings for EVERY
live LLVM value, source argument root-provider ownership, general
native pointer lifetime, host-effect replay and scheduler continuity.
Do NOT use this diagnostic to enable generic executable deopt or
to infer whole-program GC safety from a GC-generation match alone.

## 2026-11-10: IR-001 SSA -> source-procedure argument / native temporary location bridge

Added src/ir/source_ssa_location_bridge.hpp. This is a deliberately
read-only, fail-closed **partial** location map for the exact
NativeRtSsaCapture ABI and independently rooted Scratch CallFrame.
The location entries are (SSA descriptor, kind, source-argument index).
The only supported kinds are: (1) original procedure argument, where
the compiler-origin alias mask names the exact one-based Scratch
argument, including bit-identical PHI/SELECT instructions; (2) JIT
generated string temporary, where the RT-produced exact handle must
resolve to a live GcType::Str and match the captured bits; and (3)
unmapped. There is NO Scratch variable / general expression temporary
reconstruction and no executable deopt.

The verifier checks independently frozen source revision, dual digest,
procedure ID, source PC, site ID, exact descriptor sequence and
captured fingerprint through verifyDetachedTypedSsa. It checks the
actual RootedScratchCallStack top procedure/continuation PC and
root-script versus nested-procedure frame depth, confirms all original
and aliased boxed argument words against the separately rooted
source CallFrame using RT-001's strict existing
verifyRootedSsaArguments (requireCompiledAliases=true), and checks
the generated-string exact GC handles using
auditDetachedSsaGcRoots. It NEVER creates roots from raw SSA words,
exports object pointers, mutates the frame, or writes interpreter
state. Other SSA words, raw floating values, unresolved native
pointers, and source locals remain explicitly unmapped or unresolved.

New tests/platform/source_ssa_location_bridge_smoke.cpp constructs
a REAL independent RootedScratchCallStack with a procedure frame
containing a heap string and numeric argument, and an independently
rooted generated native string SSA result. Both survive two ACTUAL
full GC passes. A detached capture copy retains its generated string
root provider after the original is cleared. The test rejects
changed SSA words despite repaired FNV, rewritten or missing
compiler/source alias masks, changed generated producer masks,
missing generated handle, stale source manifest, wrong source
procedure, moved source-frame PC, and frame pop.

The same test also builds valid LLVM IR and invokes the ACTUAL native
codegen provenance analyzers sameBoxedArgumentRoot and
trustedGeneratedStringSource. An identical-value SELECT inherits
source argument #1, while mixing different arguments has no proven
identity; a known xyo_join call is a compiler-certified native
string producer, while an unknown host call is not. These real
LLVM-derived masks drive the detached source-location test instead
of synthetic classification flags. This reuses the compiler's
existing mask emission and does not change RT-owned JIT/Thread ABI.

Registered native CTest xyo-ir001-ssa-location-bridge-smoke (xyo
linkage). The new target's live LLVM provenance + actual GC smoke
PASS. A full focused xyo-ir001* and RT generated-root regression
suite passed 14/14 before the additional LLVM-analysis assertions;
rechecking the complete focused suite follows. No commit/push.
Remaining proof gaps include mapping arbitrary Scratch variable and
expression state to exact LLVM SSA values, volatile source-local
lifetime, pending host effect replay, native pointer rooting and
source execution recovery. The location bridge is diagnostic-only.

## 2026-11-10 IR-001: frozen Scratch source to live LLVM numeric SSA

Added IR-owned source_numeric_resume_coverage.hpp and
llvm_source_numeric_bindings.hpp. For one exact compiler site, a
source requirement consists of frozen typed-SSA identity
(revision, dual digest, procedure ID, resume PC, site ID) and
sorted (Scratch statement PC, source slot index, value kind,
integer width) entries. The LLVM bridge binds only the EXACT live
instruction pointer found by analyzeLlvmCheckpointLiveness, with an
identical typed descriptor present in the native compiler manifest.
Non-live definitions, future definitions, foreign functions, invalid
widths, duplicate source destinations, duplicate SSA definitions and
stale checkpoint IDs are rejected.

The strict frozen-source path also validates original compiler-owned
SourceFrameSymbols against the original source/digest and separately
verified Block IDs. Each source-PC key must identify an executable
statement belonging to the same procedure, independently reproduced
by the structural parent/NEXT/SUBSTACK traversal. A numeric loop
temporary must correspond to a real frozen tracked-loop site;
ordinary Scratch data statements cannot be rebranded as loops.
This depends on trusted compiler-owned original symbols, not an
untrusted mutated detached dictionary.

A LLVM i64 value is NOT by itself a Scratch numeric value. Unknown
host callback results, loads, pointer casts and arguments are refused
as numeric roots. The bounded, cycle-rejecting raw LLVM numeric
provenance checker accepts integer comparison results, safe integer
extensions and operations on already proven raw integer values.
There is NO promotion of arbitrary NaN-boxed data to GC roots.

New test source_numeric_resume_coverage_smoke.cpp constructs both:
(1) real LLVM IR with live/pre/post-checkpoint definitions; and
(2) real Scratch Blocks -> ScratchCompilationUnit -> RT-001
NativeProcedureFrameSession -> frozen SourceFrameSymbols. It checks
the exact owner and source dictionary, then tests wrong widths,
wrong IDs, duplicate provenance, unknown host i64, forged statement
owner/parent/name, fake loop role, and a silent direct Scratch opcode
edit without resetCache that changes the digest but not revision.

The explicit source slot requirements and LLVM Value* annotations
are a NEW compiler-side API, not yet wired into all production
native Scratch expression lowering. Even if every enumerated raw
numeric slot is present, sourceEnvironmentComplete and executable
ALWAYS return false: Scratch variable/list state, GC-managed values,
general temporary recovery, effects and arbitrary native pointers
have not been reconstructed.

Native CTest xyo-ir001-source-numeric-coverage-smoke added and PASS.
Focused CTest suite includes 14 xyo-ir001 tests plus two RT-001
generated-string root/LLVM provenance smokes: 16/16 PASS. Live
broadcast/pen, O2/O3 postopt parity, mixed HotSpot/Interpreter and
real-GC cases are included. No full CI, commit or push.

## 2026-11-10: IR-001 production reporter -> LLVM Value* observations

Added new IR-owned llvm_source_reporter_trace.hpp and connected it to
the REAL native LLVM source expression emission in
src/jit/codegen.cpp::Codegen::emitInputOf. Only with
XYO_IR001_TRACE_SOURCE_VALUES=1, each actually evaluated reporter
input records its frozen source block ID, deterministic source PC,
parent input name and the ACTUAL returned llvm::Value* as a
WeakTrackingVH. After regular native inlining / pointer hoisting, a
compiler-only audit checks whether that LLVM value survived, whether
it remains live at an actual xyo_proc_frame_checkpoint_path_phase
callsite, and whether it has conservative RAW numeric LLVM ancestry.
No native/LLVM instruction is emitted for the trace; the stored
pointers do not leave this one compiler invocation.

The original separately captured ScratchCompilationUnit must also
prove the precise parent -> input -> child Block relationship,
including auto-created shadow fallback Blocks. Rejected or missing
frozen edges do not produce an observation; the explicit
edge_rejected counter tracks these cases. Source IDs are assigned by
sorting the ACTUAL captured frozen block IDs, not by assuming an
original .sb3 file contains all runtime-synthesized shadows. The
existing full-source revision/digest validation still runs after
lowering and before publication.

Critical distinction: production emitInputBlock returns a boxed
Scratch Value, usually LLVM i64. Even an LLVM ConstantInt holding
what looks like a numeric constant might be NaN-boxed value bits.
The provenance observer therefore refuses to certify any i64
reporter return as RAW numeric solely from instruction ancestry;
boxed_or_unknown remains the default, GC/root_proven=0 and
executable=0 always. Only genuinely unboxed i1/i32 values could
qualify as local raw numeric evidence. This does NOT complete the
prior source numeric resume requirement set or authorize arbitrary
SSA deopt. No codegen runtime behavior is modified when the
diagnostic flag is unset.

tests/differential/ir001_source_reporter_origin.py generates a
REAL Scratch project with dynamic operator_add/data_variable
reporters inside a counted repeat. It compares diagnostic-on vs off
Scratch state, verifies source-PC identity against all original
SB3 blocks, recognizes independently sorted native loader-generated
xyo_ shadow block IDs, and proves at least one reporter LLVM value
remains live across a genuine source checkpoint. With the local
native build, 11 reporter observations, 3 native checkpoint sites
and 1 checkpoint-live reporter were observed. All production
reported i64 values are unproven boxed/unknown numeric roots;
the Scratch output matched with tracing on/off.

The existing source_numeric_resume_coverage_smoke.cpp now tests
WeakTrackingVH deletion, synthetic LLVM i32 raw proof, untrusted
host i64, i64 constant bits resembling a boxed string, and frozen
Scratch input-edge/child-parent mismatch tests using a REAL
ScratchCompilationUnit. Registered the new native CTest
xyo-ir001-source-reporter-origin-parity; both the new integration
and expanded source-numeric unit tests PASS.

Remaining: annotations for every actual source expression into
a complete independently enumerated recovery environment, exact
Scratch variables/lists/loop locals and their GC roots, the
resulting value reconstruction operations, scheduler/host effect
replay proof and generic native deopt. No actual continuation
adoption, no commit/push. The new trace uses WeakTrackingVH to
handle deleted/RAUW'd LLVM instruction identities safely.

## 2026-11-10: IR-001 per-checkpoint recovery gap diagnostics from real native codegen

Extended IR-owned src/ir/llvm_source_reporter_trace.hpp with
SourceReporterCheckpointObservation and SourceReporterCheckpointGap.
The native emitter's existing emitPortableLoopCheckpoint now registers
the EXACT llvm::CallBase of
xyo_proc_frame_checkpoint_path_phase, with the compiler's original
frozen Scratch loop ID/source PC and SourceResumePhase. The
registration only happens under XYO_IR001_TRACE_SOURCE_VALUES=1,
validates membership in the captured ScratchCompilationUnit and
bounds diagnostic site count. Neither the CallFrame call nor runtime
native behavior is changed. WeakTrackingVH is used for checkpoint
and reporter LLVM values, so deleted/inlined values cannot become
dangling proof.

After the usual codegen inlining/hoisting, the audit matches each
registered source site by exact LLVM CallBase pointer (not merely
its Function), checks the actual emitted phase operand against the
recorded phase, refuses duplicate attribution of one checkpoint,
and applies analyzeLlvmCheckpointLiveness to that precise site.
For every live, originally observed Scratch reporter, the audit
records an unresolved source-local destination. Its boxed i64 has
no independently proven GC root; i1/i32 numeric results also have
no independently proven source slot. Unsupported live LLVM values
are reported separately from known reporter observations. Missing,
invalid, deleted or mis-phased checkpoint sites are counted, never
automatically assigned a source PC. No source resume is authorized:
sourceEnvironmentComplete() and executable() are always false.

The native diagnostics now emit:
- [ir001-source-resume-gap-item] per live reporter,
  with checkpoint_pc, frozen loop block, phase, reporter_pc,
  source reporter block, parent/input identity, no root/slot proof.
- [ir001-source-resume-gap] for every exact source checkpoint:
  live_reporters, boxed_unrooted, numeric_unmapped,
  source_slot_missing, unsupported_llvm, always executable=0.
- [ir001-source-resume-summary] totals of actual LLVM sites,
  attributable sites, unknown physical sites, invalid source
  observations, always source_complete=0/executable=0.

A real Scratch .sb3 with a counted repeat and nested
data_variable/operator_add reporters yields three correctly
attributed physical LLVM source checkpoints (source PC 3,
phase AfterLoopBody), with (live reporters / unrooted boxed /
other unsupported live LLVM values) of respectively
0/0/29, 1/1/29, and 1/1/31. The counts are conservative
*diagnostic observations*, not proof of required source temporaries.
The same reporter value can cross two distinct physical checkpoints,
and both sites must independently refuse to assume a GC root.
The project produces identical Scratch final states with diagnostics
on and off.

Extended the existing source_numeric_resume_coverage_smoke test to
construct TWO different real physical LLVM CallBases in one Function
with distinct phase constants, plus an SSA call defined between
them. It demonstrates that the second site has one more live
reporter; that forged phase, duplicated same CallBase attribution
or pointing a source record to a noncheckpoint call fails closed.
The real native interpreter/JIT differential CTest checks exact
source ID/PC/phase, per-site gap consistency and zero unknown
source sites. The 17 focused IR-001 + related RT-001 CTests PASS;
interpreter/LLVM broadcast and pen journal parity PASS;
git diff --check PASS. No full CI run, no commit or push.

Next evidence needed: an independently complete source-local
requirement enumeration and direct GC-rooted Scratch frame mapping
for every live value, proof that arbitrary untracked LLVM values
do not affect the continuation, scheduler/host effect-commit proof,
and exact recovery code. Without those the gap counter is not an
executable deopt plan.

## 2026-11-10: IR-001 source-origin roles and nested loop path diagnostics

Extended opt-in compiler source reporter tracking in src/ir/llvm_source_reporter_trace.hpp and production src/jit/codegen.cpp. Each actual reporter Value* is classified using the frozen Scratch block opcode into VariableRead, ListRead, ProcedureArgument, ComputedExpression, Literal, or Other/Unknown. This is source provenance only; it does not certify any GC roots, raw numeric SSA or source restoration destinations. Individual checkpoint reports now separately count live variable/list/argument/expression/literal/other reporter observations. Production compiler still requires original frozen parent/input/child source edge and records a WeakTrackingVH to the real LLVM definition.

The native emitPortableLoopCheckpoint now additionally records the ordered lexical source-loop IDs from the actual yieldStack and current loop alongside the exact LLVM CallBase, phase, and emitted source-path depth. The diagnostic compares the constant native CallBase path depth/phase, requires a 1..32-entry nonduplicate loop path ending in the expected current source loop, and reports loop_path_ids and loop_path_depth. This proves only compiler-side path attribution. It never claims that runtime CallFrame loop counter values have been validated; loopPathRestorable and executable always remain false.

Expanded actual LLVM and frozen Scratch source smoke checks source reporter opcode roles, different live source value classes per physical checkpoint, invalid/duplicate/wrong-depth/empty loop-path fail-closed cases. Expanded real SB3 differential generates nested repeat(2) x repeat(2), list reads via data_itemoflist and data_lengthoflist and checks both outer path depth=1 and inner path depth=2 with original source IDs. Two live list reporter observations at inner loop checkpoints remain boxed_unrooted, without claiming actual GC unreachability. The project ends with x=60, done=1, history=[4] (Scratch change-by-expression semantics), identical in diagnostic on/off mode.

Fresh native xyo-cli built successfully; 2 new focused tests PASS, full IR-001 + related RT-001 CTest selection 17/17 PASS. Existing interpreter/LLVM broadcast and pen journal parity PASS. Python compile and git diff --check PASS. No full CI and no commit/push. Remaining: independently rooted GC source values, complete Scratch local/variable/list resume mapping, actual runtime loop-counter value proof, unmanaged native pointers and scheduler/effect continuity. Generic executable deopt is NOT enabled.

## 2026-11-10: IR-001 runtime loop readback and frozen SSA reporter descriptors

The runtime LLVM checkpoint now supports opt-in **diagnostic-only** readback
with `XYO_IR001_TRACE_SOURCE_VALUES=1` plus native CallFrame support. The
compiler-supplied ordered source loop IDs, IEEE-754 counter *bits*, depth and
phase are checked against the stored `NativeProcedureFrameSession` CallFrame,
the frozen `SourceFrameSymbols` name table, exact lexical ownership/ancestors,
and each counted-loop slot. This cannot establish the complete source
environment or a GC root. A counted loop with an absent counter produces a
missing readback witness, but native execution is not disabled. Existing
runtime frame validation still rejects structurally malformed source paths.

The actual nested Scratch fixture has `repeat(2)` inside `repeat(2)` with
list reporters. At the inner checkpoints, runtime path
`b0004,b0005` has depth 2 and two independently read back counted-loop
counters. Outer checkpoints have path `b0004` and one counter. All logged
paths match an exact compiler-emitted physical `CallBase` and phase in the
differential test; diagnostic on/off final states agree
(`x=60, done=1, history=["4"]`). Negative native tests cover swapped
IDs, wrong IDs, forged count, omitted count, changed phase, a missing
counted-loop value, and exact bit retention of `-0.0` / `2.5`.

Each live Scratch reporter now records its actual LLVM numeric SSA
instruction descriptor (`ssa_id` + `ssa_width`), alongside frozen source
reporter PC and origin. Two list reads survive inner-loop LLVM checkpoints as
boxed i64 values, with **no proven GC root and no Scratch restoration slot**.
A plain i64 Scratch Value is not a raw numeric/GC ownership certificate.

A correctness issue was reproduced: generating the first native SSA spill
renumbers instruction traversal IDs at the next checkpoint if the analysis
is rerun on already modified IR. Production `src/jit/codegen.cpp` now
freezes all physical checkpoint numeric and extra-live liveness analyses
**before** emitting any spill, so the reporter descriptors and RT-001
detached typed SSA capture refer to the same original function traversal.
The synthetic two-CallBase unit reproduces the renumbering after an inserted
spill and verifies the preserved pre-spill descriptor.

With `XYO_JIT_SSA_LIVENESS=1` and
`XYO_RT_CAPTURE_LLVM_SSA=1`, the real nested list fixture captures four
physical checkpoints (29, 33, 15 and 19 numeric instructions), and remains
behaviorally identical. Every capture reports `complete=0` because
non-numeric live LLVM state remains unsupported (observed 32, 34, 31 and
33 unresolved pointers). The runtime-generated string origin audit reports
zero generated-string candidates in this fixture; no source-list/variable
local mapping has been proven.

Existing `RootedCallFrameValues` only owns genuine procedure argument
Values; `rt_generated_ssa_roots` owns separately proven native generated
strings. Neither can be used to assume a `data_itemoflist` or
`data_lengthoflist` value has a separately retained root. Next work must
join compiler-verified source-reporter descriptors to the exact
scheduler-owned detached SSA capture and independently GC-rooted handles
at a matching physical site, then define an explicit Scratch resume target
without replaying effects. This is **not** executable deoptimization.

Built native `xyo-cli` and the focused tests; full IR-001 + related RT-001
CTest selection **17/17 PASS** after integration, including new negative
counter and two-CallBase SSA ID regressions. Python syntax check and
targeted `git diff --check` PASS. No full CI run; no commit or push.


## 2026-11-10: IR-001 reporter SSA -> rooted location audit (follow-up)

Added diagnostic-only `src/ir/source_reporter_ssa_location_bridge.hpp` to
join the existing compiler trace's frozen source-reporter identity to
RT-001's independently checked typed SSA snapshot and `RootedScratchCallStack`.
This does not assume that NaN-boxed i64 values are GC-safe or restorable.

Each compiler checkpoint now records a 1-based `physicalSiteId` in addition
to source PC, ordered loop IDs and reporter->SSA descriptor mappings. The
RT detached capture side assigns its site ID for **every physical LLVM
checkpoint**, even where one checkpoint cannot be captured; previously
site numbering depended on successful captures. Thus different physical
yield sites at the *same* Scratch source PC cannot accidentally alias.
The synthetic dual-CallBase regression verifies distinct IDs; a real
nested-repeat/list project observes four distinct compiler sites 1..4.

The bridge requires a distinct `SourceReporterCompiledSite` (compiler-frozen
source origin contract) and RT's independently supplied
`SourceTypedSsaManifest`, checking matching revision, digest, physical
site, source PC, phase, loop path, complete reporter binding sequence,
typed SSA descriptors and actual rooted frame revision. Unlike checking
only descriptor membership, this rejects malicious retargeting of a
reporter to **another valid** SSA descriptor. The caller must retain
the compiled site separately and MUST NOT reconstruct it from mutable
runtime data; its persistence and scheduler integration are not yet built.

The location audit classifies actual reporter SSA into independently checked
procedure-argument aliases, RT-owned generated string roots, and unresolved
values. It reports GC-rooted heap-string observations separately from
numeric or immediate arguments; repeated SSA aliases are not counted as
independent GC objects. Even all-matched known locations do not establish
a Scratch variable/list/expression restoration destination or complete
the live environment/effect continuity. `sourceEnvironmentComplete()` and
`executable()` remain false.

Native unit tests exercise the RT-001 real LLVM alias/source-producer analyzers,
independent rooted procedure arguments and generated strings through two
real GC cycles, the numeric-argument/no-GC-root case, and forged same-PC
site IDs, phases, lexical paths, stale IDs, duplicated bindings, and
retargeting to an existing alternate descriptor. The real LLVM
source-numeric test freezes the compiler site contract from two distinct
physical `CallBase` observations. The real nested Scratch fixture still
reports two live i64 list reporter values at inner checkpoints, **without**
a verified root/restore slot. `XYO_JIT_SSA_LIVENESS=1` and
`XYO_RT_CAPTURE_LLVM_SSA=1` continue to generate four detached capture
sites and preserve the Scratch state, but this fixture did **not** exercise
a scheduler-side retained typed SSA handoff (no
`[native-typed-ssa-manifest]` entry). Do not claim runtime reporter->root
handoff is complete.

Fresh native compilation and the related IR-001/RT-001 CTest selection
**17/17 PASS**; targeted Python compile and `git diff --check` also run.
No full CI run, no commit, no push. Next: retain compiler-owned
`SourceReporterCompiledSite` within the compiled native artifact, expose it
only to the matching scheduler/Thread capture site, compare against actual
rooted detached handles and a verified source environment, and reject any
unmapped list/variable value before considering interpreter adoption.

## 2026-11-10: IR-001 compiled reporter-site owner and actual scheduler re-audit

Added opt-in CompiledScript::nativeReporterSites as an append-only
shared_ptr<const SourceReporterRuntimeSites>. Native LLVM codegen
freezes the reporter-to-SSA binding, physical checkpoint ordinal, Scratch
source PC, phase, loop path and source revision/digest BEFORE native SSA
spills. This table belongs to the compiled script/JitEngine, NOT the
mutable Thread or a detached SSA snapshot. It contains no raw LLVM
pointers, heap values or executable continuation locations.
XYO_IR001_RUNTIME_REPORTER_AUDIT=1 automatically enables compiler
source reporter tracing and retains the frozen table.

At an actual xyo_rt_capture_llvm_ssa checkpoint, the compiled table
entry is joined to the native source CallFrame and typed RT-001 SSA
manifest. The exact generated-string-root handle vector is created
through the existing strict allocator/type verifier when reporter
auditing is active, including checkpoints with zero generated strings.
An independently rooted CallFrame export is then validated through
auditSourceReporterSsaLocations. A mismatch refuses the diagnostic
SSA snapshot without stopping the still-running LLVM Fiber. The
runtime result always says source_complete=0 executable=0.

The real nested-repeat and list-read project has six dynamically
observed source checkpoint captures, all matched=1. The inner-loop
physical site 1 has two list reads, unmapped=2, heap_roots=0 and
unsupported=41. The outer-loop physical site 3 has zero live source
reporters but unsupported=32. Scratch final state is unchanged:
x=60, done=1, history=["4"]. Other compiled physical sites are
not dynamically reached by this fixture.

An independent opt-in scheduler gate,
XYO_RT_REQUIRE_REPORTER_SSA_AUDIT=1, repeats the full compiled-origin
site and rooted detached SSA check at the actual Fiber-to-interpreter
handoff safe point. Missing/stale evidence, unrooted reporters or
unresolved native SSA veto the handoff, preserving the running Fiber.
This never grants a new generic deoptimization permission. On a real
simple repeat project, forced native handoff is rejected
([native-simple-deopt] adopted=0) while the project still finishes
x=14 and done=15. The scheduler recorded unmapped=1 unsupported=29.

Both diagnostics are OFF by default. To capture runtime evidence,
enable XYO_JIT_CALLFRAME=1, XYO_JIT_SSA_LIVENESS=1,
XYO_RT_CAPTURE_LLVM_SSA=1, and
XYO_IR001_RUNTIME_REPORTER_AUDIT=1.
Use XYO_RT_REQUIRE_REPORTER_SSA_AUDIT=1 only for testing the strict
source adoption veto. The native spill, GC handle and rooted frame
copy overhead is not present with both flags off.

Native build and related IR-001/RT-001 CTests 17/17 PASS.
Real Scratch differential now requires matched runtime reporter joins
and a genuinely attempted/rejected scheduler handoff. Python syntax
and git diff --check validated. Full CI not run, no commit/push.

Remaining: independently proven Scratch variable/list/expression
reinstatement slots, fully mapped GC roots and native SSA/pointers,
effect continuity and no replay of non-idempotent reporter evaluation.
No general Fiber-independent deopt is enabled.

## 2026-11-10: IR-001 lexical owner metadata and live Target epoch readback

Compiler reporter-to-SSA provenance now carries a frozen lexical reporter
block, parent/input edge, and Scratch source storage identity. The storage
classification includes scalar-owner (data_variable), list-owner
(data_itemoflist/data_itemnumoflist/data_lengthoflist/
data_listcontainsitem/data_listcontents), procedure argument,
expression temporary, literal, other and unresolved. Only string IDs and
opcodes from the frozen ScratchCompilationUnit are copied. No BlockField
Value bits, mutable Block pointers, SStr or SList pointers are persisted
into the CompiledScript's immutable reporter site dictionary.

New source_reporter_storage_owner_audit.hpp validates the compiler-frozen
storage label against the reporter opcode, then resolves the CURRENT
variable by ID using the active Scratch Target's normal local-first
stage-second lookup. Only matching id + scalar/list type is reported as
a typed source owner. A list also requires the current Variable to have
a non-null list object, but this does NOT certify its contents/GC roots.
The snapshot reads Target::variableBindingStamp() before and after the
readback to prevent stale Variable* use and records no Variable* in
the output.

The optional RT native capture stores ONLY structural epoch generation
counters and target/stage identities in NativeRtSsaCapture, separate
from compiler metadata and detached LLVM word bits. The strict scheduler
gate re-resolves the owners and checks the identity/generations against
the capture. Mismatched generation, missing/wrong-type owner, or an
unclassified input vetoes any handoff, keeping the native Fiber alive.
New log records: [ir001-runtime-reporter-owners] and
[ir001-scheduler-reporter-owners]. Both report
value_restorable=0 executable=0.

Unit regression exercises frozen owner extraction, a real Target
VariableMap, typed scalar and list, missing list, scalar/list shadow
mismatch, same-ID deletion/recreation with changed global generation,
and malformed compiler labels. Real nested-list SB3 observes two typed
list owners at the inner physical checkpoint, no missing or wrong-type
owner, and identical final Scratch variables to baseline. Real forced
scheduler handoff sees a stable owner epoch and rejects the incomplete
LLVM environment; test-only reporter-owner-epoch corruption changes
ONLY the detached owner generation and produces epoch_stable=0,
adopted=0 with identical Scratch results.

Native build and related IR-001/RT-001 CTests: 17/17 PASS. Diagnostic
flags remain opt-in. Python syntax and git diff --check also pass.
Full CI not run. No commit or push was made.

Unresolved: a valid current list owner does not establish the historical
item/index/version evaluated by the native reporter, nor does it map the
SSA result to an interpreter ReportedEntry. The proof must eventually
capture exact evaluation time, input evaluation order/slot identity,
GC-rooted historical value and effect replay, instead of re-reading a
possibly mutated list. Generic source deoptimization remains disallowed.

## 2026-11-11: historical Scratch input slots and live GC-rooted reporter SSA

IR-001 now freezes the one-based lexical source input ordinal and active
shadow-vs-block selection alongside every compiler-observed reporter
SourceReporterLiveSsaBinding. The binding already carries the reporter
Block ID, parent ID, input name, exact RT-001 physical CallBase site,
native SSA descriptor, source revision/digest and loop path. The ordinal
is derived from the immutable ScratchCompilationUnit; no mutable
Blocks lookup or GC Value is used to create the compiled dictionary.
The interpreter's ReportedEntry records the reporter BlockCached ID and
writes to its parent's argValues[inputName], not directly to the
Scratch variable/list storage. The frozen parent input ordinal is a
lexical input location only, NOT yet an executable ReportedEntry.

New source_reporter_input_slot_audit.hpp joins verified detached SSA
descriptors with those lexical input slots WITHOUT re-evaluating any
Scratch expression or dereferencing NaN-boxed pointers. Only canonical
pointer-free boxed immediate bits (number, boolean, empty string,
undefined and color) may be copied to a diagnostic field. Native f64,
invalid noncanonical NaN, ambiguous repeated lexical input ownership
and unrooted heap strings are differentiated and never staged for
interpreter execution. Every audit reports restorable=0/executable=0.

New source_reporter_input_roots.hpp adds a separate, OPT-IN, exact GC
root provider for source reporter string-valued i64 instruction SSA.
The capture is permitted only after independent compiler reporter
origin/site and real rooted CallFrame checks. The value's tagged
address MUST name an exact allocated GcType::Str header via Gc::isObject
before dereferencing the header or retaining the Value in
GcValueHandles. A mismatched/forged/stale string address aborts capture.
Handles live with NativeRtSsaCapture and survive a copy, independently
of RT-001 compiler-known generated-string handles and original
procedure argument roots. Verifiers match every handle to its exact
saved SSA descriptor and bits; a dropped/replaced handle fails closed.
No raw heap bits are placed into the diagnostic immediate result.

Real nested Scratch repeat/list project: inner physical site 1
has TWO lexical input values, ordered parent input slots 2 and 1,
one canonical boxed immediate and one heap string. Without the new
root, the latter was unrooted_heap=1; with membership-checked exact
capture, logs now show immediate=1 rooted_heap=1 unrooted_heap=0,
ambiguous=0, restorable=0 executable=0. Six dynamic native checkpoint
observations match the compiler's frozen sites. Scratch output remains
history=["4"], done=1, x=60.

Unit tests: real string allocation with no trusted native-generated
string mask becomes individually source-reporter-rooted; survives two
full GC collections. Fake pointer and removed root handle are
rejected. Direct and shadow lexical source input ordinal/edge checks,
ambiguous reporter slot, uncanonical NaN and non-boxed SSA cases are
covered. Live negative Scratch test
XYO_IR001_TEST_DROP_REPORTER_INPUT_ROOT=1 drops only the retained
string handle at the native checkpoint. The diagnostic copy is denied,
the native Fiber remains runnable and Scratch output is unchanged.

No source-verified interpreter ReportedEntry writing, list item index
recovery, same-evaluation historic mutation epoch, full native machine
pointer/effect proof or generic deoptimization permission was added.
The optional strict scheduler gate continues to reject all nonempty
unrestored source reporter input slots; a rooted heap string alone is
not an interpreter continuation. The normal JIT path is unchanged
when IR-001 diagnostic opt-in flags are disabled.

After final native rebuild: related IR-001/RT-001 focused CTests
17/17 PASS; differential includes real GC root-drop failure.
Full CI not run; commit and push were not performed.

## 2026-11-11: Real list INDEX evaluation -> source SSA checkpoint evidence

IR-001 now observes data_itemoflist at the ACTUAL LLVM-native read:
the codegen emits an opt-in xyo_ir001_list_read_eval call immediately
after the exact helper result or inline list-read PHI, passing the
already-evaluated INDEX Value, already-evaluated result Value, compiler
reporter source PC and pinned immutable list ID. There is NO second
Scratch reporter execution, no call to listGet/listIndex and no second
RNG draw for a random index. This call is compiled ONLY when runtime
reporter auditing and native SSA capture are opted in; compiler-only
XYO_IR001_TRACE_SOURCE_VALUES does not enable the observer.

The compiler's independently frozen SourceReporterStorageSource
now also retains the specific Scratch block feeding the list's INDEX
input, validated through frozenSourceReporterInputEdge. The runtime
read event retains a bounded monotonically sequenced index/result
record, source list ID, list length and physical storageEpoch
observed immediately AFTER the read; no raw unrooted heap word is
persisted. Original boxed number input is saved as a pointer-free
immediate. Numeric Scratch shadow inputs that arrive as boxed string
"1" or other pure decimal digits are inspected only after exact GC
object membership validation and yield a zero-based numericPosition.
"last", "any", "random", other string conversions and nonnumeric
values remain index-unproven; they are never re-evaluated. The actual
result is held either as a canonical pointer-free immediate or in
the read event's own exact GcValueHandles, checked against an allocated
GcType::Str before rooting.

Each real native physical SSA checkpoint joins event reporter PC/list
ID/SSA descriptor against the independently frozen compiler site and
the RT-001 detached SSA word. Duplicate events from the same reporter,
missing events, mismatched words, missing INDEX source block, stale
rooted handles and event overflow are not certified. The detached
NativeRtSsaCapture retains a shared owner of the original GC-rooted
read evidence and the scheduler verifies it again without touching
live Scratch list contents. Any unproven required data_itemoflist
input vetoes the diagnostic capture; the original native Fiber
continues. No interpreter ReportedEntry is written and
executable=0 remains explicit.

The real nested-repeat/list SB3 observed site 1 with one matched
actual data_itemoflist evaluation, numeric_index=1 (count of numeric
positions) and a per-event 0-based index0=0 from the "1" shadow.
It matched the already-captured heap-string SSA word with an exact
GC-rooted handle. There were no missing, ambiguous or mismatched
read events; site 3 had zero live data_itemoflist reporters.
Scratch final state was unchanged: history=["4"], done=1, x=60.

Tests verify a real GC-retained result survives a collector pass,
string numeric and random-index classification, duplicated reporter
read rejection, source input forgery, SSA value mismatch, dropped read
root, and unchanged Scratch execution after test-only
XYO_IR001_TEST_DUPLICATE_LIST_EVAL=1. A separate unit demonstrates
that SList::write0 changes an element while storageEpoch stays
unchanged, so the epoch is NOT a list-content mutation generation
and cannot prove safe deoptimization/replay. All runtime behavior is
opt-in. The new Thread trace owner was appended after the previous
last field to preserve all existing compiled field offsets.

Related IR-001/RT-001 native rebuild and focused CTests: 17/17 PASS,
Python syntax verified, git diff --check passed. Full CI not run;
no commit or push. Remaining: list-content mutation epoch covering
EVERY interpreter/JIT helper and inline write, historical list
index/effect and interpreter ReportedEntry reconstruction proof,
all live machine SSA and GC roots. IR-001 still in_progress.

## 2026-11-11: Bracketed list-content writes and before-read epochs (IR-001)

The previous process-wide list mutation counter only advanced AFTER
publishing an updated list item. That allowed a physical LLVM load
overlapping an instrumented writer to observe the same epoch before
the writer completed. This has now been replaced with a two-phase
bracketed writer window: beginSourceListContentWrite() increases the
in-flight writer count and advances the global generation BEFORE
any list data is published; endSourceListContentWrite() advances
the generation again and decreases the writer count. The diagnostic
accessor sourceListContentEpoch() returns 0 whenever a writer is
active or a stable epoch cannot be observed. An overflowed counter
also fails closed. Atomic operations use sequentially-consistent
ordering. This is not a substitute for application-level
synchronization of otherwise-racing SList reads and writes.

All normal SList content mutations (push, write0/replace0,
insert0, erase0, non-empty clear) now use a scoped begin/end guard.
These operations cover interpreter primitives, JIT helper paths,
and list initialization. LLVM's successful inline fast paths
(data_addtolist and data_replaceitemoflist) emit separate opt-in
C ABI begin/end calls around their item/len stores. WASM's raw
transactional list commit and parfor raw reserve/worker slots
also bracket writes; audit-mode codegen continues to exclude GPU
and parfor accelerated loops so the unfamiliar async mutation
paths cannot accidentally be certified.

Compiler-emitted native data_itemoflist now samples the global
stable epoch immediately AFTER evaluation of Scratch INDEX and
immediately BEFORE the real helper/inlined item read. The original
source observer retains this stamp, samples again after the
original result/GC roots have been observed, and then compares
both to the physical LLVM SSA checkpoint stamp. Any zero token,
in-flight writer, generation change or stale scheduler snapshot
causes the source list read certificate to fail closed. The helper
does not re-evaluate INDEX, listGet, or random. The compiler-only
origin trace still never installs the native observers.

Real nested-list Scratch parity across all five backends
(flat/deque/gap/chunked/adaptive):
  - unchanged list between read and checkpoint: accepted=1, stable=1
  - LLVM inline or backend helper replace-after-read:
    accepted=0, changed=1
  - final Scratch values match interpreter in every case:
    unchanged history=["4"], done=1, x=60;
    changed history=["5"], done=1, x=67
The source proof remains diagnostic and never declares source
environment executable/restorable.

Regression now includes actual nested in-flight write scopes, a
zero/untracked pre-read token, stale saved roots, exact historical
SSA words, list storage-layout epoch unchanged despite content
replacement, and both changed/unchanged source projects in each
storage mode. Besides the IR-001/RT-001 focused suite, CTest
checks WASM host list ABI, parfor, numeric list ABI, five list
backend smokes and adaptive JIT list epoch.

Validation: 26/26 targeted related CTests PASS, plus
XYO_IR001_RUNTIME_REPORTER_AUDIT=1
XYO_RT_CAPTURE_LLVM_SSA=1 xyo-ir001-ssa-location-bridge-smoke PASS.
Python script syntax and git diff --check PASS. No full CI.
No commit/push.

LIMITATIONS: the process-wide generation can only notice writes
from included in-process paths; plugins/external uninstrumented
writers, GPU-owned asynchronous writes, or another independently
linked copy of the counter are not covered. The atomic stamps
do not make an unsynchronized list load safe, and equality is not
a proof of immutable content, GC machine-root completeness or
replay-safe source continuation. The exact interpreter
ReportedEntry/ArgValues frame, side-effect order and machine
liveness remain to be restored and proven independently. Generic
Fiber-to-interpreter deoptimization stays disabled.

## 2026-11-11: interpreter ReportedEntry/ArgValues destination and flattened _ops prefix audit

IR-001 now has src/ir/source_reported_entry_destination_audit.hpp.
This independent, read-only bridge checks an already rooted/type-audited
native SSA reporter against the REAL interpreter BlockCached cache:
the exact source revision, reporter Block ID, selected parent/input
edge, one-based frozen InputSet ordinal, typed SSA descriptor,
parentValueSlot pointer identity to the matching ArgValues::vals
map node, reporter operation occurrence/ordering in the immediate
parent's flattened _ops and the outermost connected source statement's
flattened _ops. Traversal only follows selected normal input edges;
it cannot follow NEXT or SUBSTACK as a reporter parent. Shadow
literals, BROADCAST_INPUT (IdName, not Value), unrooted/invalid
boxed bits, numeric-not-boxed SSA, duplicated input mapping,
malformed parent pointer, stale source revision or edited source
input edge are not accepted.

The new audit constructs optional Blocks cache nodes lazily on the
same native scheduler thread, but calls NO blockFunction or reporter;
it does NOT write existing ArgValues, Set/StackFrame, ReportedEntry,
Thread status, or native Fiber. Only source IDs, bounded op offsets
and canonical pointer-free boxed immediate bits are returned.
Previously rooted heap values are labelled as ROOTED observations,
not copied as raw unrooted pointers. Source resume still needs full
native machine/GC/effect and interpreter frame proofs; the audit
always reports source_complete=0 executable=0.

For each source statement the audit computes which captured inputs
form the EXACT contiguous _ops prefix and independently lists every
missing earlier op (ID, opcode, position) in the ORIGINAL cache
order. A gap cannot be treated as a replayable reporter merely
because a later SSA value was captured. The real nested-list
Scratch SB3 produces for site 1:
  mapped=2, unavailable=0, rootless=0,
  incomplete_prefix=1, contiguous_prefix=1,
  missing_prefix_statements=1.
Its list reporter lives at flattened position 1 in a four-op
statement, but the earlier position-0 data_variable reporter
(b0008) is no longer in the checkpoint's live SSA bindings.
The other captured numeric reporter is at position 0 in a
different two-op statement. This exact source operation gap is
now surfaced as [ir001-runtime-reported-entry-missing].
All Scratch results and native Fiber behavior are unchanged.

Strict opt-in scheduler revalidates the live interpreter cache
destination map before contemplating a handoff. Incomplete
reported-prefix state still prevents executable reporter resume;
the original Fiber continues on failure. No support for generic
source deoptimization or actual ReportedEntry/ArgValues writes was
added. Source editor edits that occur before revision publication
are caught by an independent live input-edge check.

Unit tests use REAL Runtime and Blocks caches with a nested
operator_add/data_variable/data_itemoflist/changevariable _ops
list. They verify both full and one-reporter-missing prefixes,
parent value pointers, no ArgValues mutation, stale revision,
wrong ordinal, wrong SSA descriptor, forged boxed GC bits, wrong
shadow semantics, deleted parentValueSlot, and live source edge
tampering without a revision change. Real nested-list regression
requires all two source destinations to match and reports the
missing data_variable op explicitly. Strict scheduler path
tests forced failed adoption without changing Scratch results.

Validation: full focused native rebuild; 26/26 related CTests
PASS including IR-001/RT-001, WASM list ABI, five list backends,
adaptive list epoch and parfor; standalone IR-001 GC/SSA smoke
with runtime audit enabled PASS; Python syntax and git diff
--check PASS. Full CI not run. No commit or push made.

Next: capture the actual data_variable read result and its
structural/GC identity AT READ TIME even when LLVM has killed its
SSA result before the physical checkpoint. Do not re-read the
current variable or replay the earlier source operation.
Only after a complete ordered reported-prefix, source frame PC,
effect replay and machine root proof may interpreter adoption
be considered. IR-001 remains in_progress.

## 2026-11-11: ordered read-time ReportedEntry input prefix candidate (IR-001)

The runtime now captures scalar data_variable values at the ORIGINAL
native evaluation point (including the value whose LLVM SSA later dies),
and preserves the original data_itemoflist result/index in separate
exact GC-rooted read event providers. These are not post-checkpoint
re-reads of mutable Scratch variables or list elements. For the real
nested-list fixture the source record order is scalar sequence 1 then
list item sequence 2; both are independently joined to the frozen
compiler physical SSA site. The previously missing scalar reporter
appears in typed live SSA after enabling the observer but the missing
SSA historical path is independently tested.

New src/ir/source_reported_entry_prefix_plan.hpp constructs a
diagnostic SourceReportedPrefixPlan by combining the actual real
BlockCached flattened _ops/ArgValues destination audit with the
independent historical scalar and list read events. For each supported
reporter it stores a fully checked source operation ID, parent input,
statement op position, exact source VARIABLE/LIST field ID, native
reporter PC and actual evaluation sequence. It stores either canonical
pointer-free immediate Value bits or an exact GcValueHandles provider
and handle. It does NOT store unrooted SStr pointer bits or freshly
evaluate Scratch reporter functions. A previous scalar value whose
LLVM SSA was killed can fill a missing source op position, but ONLY
if the historical scalar event's exact GC root and compiler PC/source
identity were validated by the existing independent scalar-prefix
audit. The plan checks the exact immediate against the captured
native SSA immediate, or demands an independent rooted string
observation. It rejects invalid roots, repeated input/statement
positions, missing earlier flattened ops, reversed evaluation order,
stale source revision, mismatched source owner field ID and a
revisionless parent-input graph edit.

A non-executable snapshot of this candidate is attached to the
diagnostic NativeRtSsaCapture and revalidated at the strict scheduler
safe point before any contemplated interpreter adoption. It does not
mutate any ArgValues, ReportedEntry, StackFrame, Thread/Fiber or
Scratch storage. Site 1 of the real nested-list fixture yields:
    captured=2 unsupported=1 missing=0 ambiguous=0
    invalid_roots=0 order_bad=0 gaps=0 verified=1 executable=0
in repeated native iterations, with Scratch final state
history=["4"], x=60, done=1. The one unsupported site-local
reporter is tracked separately; "verified=1" means ONLY the
present two-value ordered diagnostic prefix is coherent and
GC-owned, NOT that the full native source/environment is restorable.
Sites without any supported inputs report verified=0.

Unit regressions on real Blocks cache cover a two-input contiguous
prefix, earlier variable reporter absent from live LLVM SSA (recovered
from actual historical read event), inverse event order, forged
heap-tagged immediate, wrong reporter ID, stale revision, silent
source VARIABLE/LIST owner change, parent input reassignment,
GC-rooted historical heap string retained across an actual collection,
dropped provider, and a forged immediate in an otherwise rooted
candidate. No source values are written into interpreter ArgValues.
Live differential requires exactly two source values with valid
order and GC roots, while one unimplemented reporter remains
explicitly unsupported. The original Scratch interpreter/native
output is unchanged.

Native and related IR-001/RT-001, five list backends, WASM and
parfor CTests 26/26 PASS after full focused rebuild. Opt-in
SSA location bridge smoke PASS; Python syntax and git diff --check
PASS. Full CI not run and no commit/push performed.

CRITICAL NEXT PROOF: the interpreter's ReportedEntry is part of
the resumed StackFrame semantics (typically while a reporter is
asynchronously suspended). A correct ordered input subset is not
a suspended interpreter frame or final parent arithmetic result.
The remaining reporter kinds, exact effects, source continuation
PC/stack and all live native roots must be recovered before
any generic native Fiber retirement. IR-001 stays in_progress.
