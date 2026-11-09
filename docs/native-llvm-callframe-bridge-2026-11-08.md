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
