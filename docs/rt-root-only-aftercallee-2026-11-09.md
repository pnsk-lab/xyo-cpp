# RT-001: root-only AfterCallee native-to-interpreter transfer (2026-11-09)

## What changed

NativeSourceHandoff::simpleInterpreterResume() now accepts the opt-in
native LLVM CallFrame image with exactly one root frame remaining after a
non-warp Scratch procedure has returned. The previous implementation refused
all image depths less than two.

For this path, the detached IR-001 rootPostCallSuccessor() supplies source
metadata (it does not independently authorize deoptimization). RT-001 checks:

- validFor(thread): live Fiber, matching compiled revision, source digest,
  capture identity and current logical yield;
- exact root callerLoopPath against the saved root CallFrame loop slots;
- the original source procedures_call (non-reporter, non-warp), plus its
  immutable call-site source graph and ordered IF/ELSE branch path;
- root lexical loop counters reconstructed directly from saved numeric
  counterBits (interpreter latch convention subtracts one);
- a proven return edge, not an inferred native/SSA instruction pointer.

Adopted SourceReturnKind subsets:

- NextBlock: original call->next equals the verified source successor;
- AfterBranch: the exact terminating IF frame is removed, preserving outer
  loop counters, and execution continues at the verified branch successor;
- LoopLatch: restore the exact verified root loop latch and unwind any
  completed terminal IF/IF-ELSE ancestors with no NEXT before it;
- ScriptExit: direct terminal root call or a call at the end of a
  source-verified terminal IF/IF-ELSE ancestry; no live loop is allowed
  to be mistaken for a completed script.

ProcedureExit, arbitrary LLVM SSA/effect PCs, and unrecognized control
paths still fail closed. The native Fiber is preserved on refusal. No opt-in behavior
became default-enabled. The legacy IR postCallSuccessor() depth>1 contract
remains unchanged.

## Tests

The expanded tests/differential/native_simple_resume.py suite covers
root-only ScriptExit/NextBlock/AfterBranch/LoopLatch, dynamic TIMES whose
source variable changes during execution, nested 19x7 root repeat at the
200th yield, full GC after adopted deopt, rejected stale revision/digest,
real production cache reset plus GC with suspended native Fiber retained,
and two independent green-flag root CallFrames. Full native resume/failclosed
suite: 546/546 PASS (previously 504/504, +42 runs).

The handoff is deliberately narrow and does not solve generic deoptimization
of arbitrary SSA temporaries or pending side effects. Source cache generation
retirement currently retains old cached operations until Blocks destruction,
pending a separately proved quiescent reclamation protocol.

## Files

- src/engine/source_continuation_handoff.hpp
- src/engine/sequencer.cpp (diagnostic selection + comment only)
- tests/differential/native_simple_resume.py

Shared working tree changes from unrelated agents were preserved. No commit
or push was performed.

## Additional integrated checks

- Real GUI Scratch parity: off/hotspot/eager each 2123/2123 PASS.
- Native LLVM CallFrame mirror with export probes: 25/25 PASS.
- Scheduler-owned GC-rooted native source handoff: 5/5 PASS.
- Related CTest targets (call-frame ABI, native frame revision, rooted
  call stack, production cache-retirement): 4/4 PASS.
- Python harness compilation and git diff --check: PASS.

The executable was rebuilt at
build/async-jit/ir001-return-source/xyo. This is a shared-tree,
opt-in-only verification; it is not a separately published release.

## Follow-up: terminal root branch and loop-arm unwind (2026-11-09)

The original depth-one ScriptExit path accepted only an immediate
procedures_call with zero active root controls. This incorrectly refused
legitimate source-verified exits from the final arm of a control_if or
control_if_else, including nested conditionals. The root-only LoopLatch
path likewise refused when the top lexical control was a completed IF
rather than the loop itself.

At a verified ScriptExit, RT now validates every active root control is
an IF/IF-ELSE with no trailing NEXT, then clears the already-completed
control frames and transitions to STATUS_DONE without re-evaluating any
reporter or replaying committed effects. At a verified LoopLatch, RT
unwinds only terminal conditional frames with no NEXT until the exact
source-saved loop control is on top; all saved numeric counters remain
intact and are used to resume its latch.

The original immutable SourceReturnSite, source digest, source revision,
live CallFrame identity, branch arm path, and loop counter bits remain
mandatory. Unknown branches, nonterminal NEXT edges, live counted or
conditional loops masquerading as ScriptExit, and wrong latches still
fail closed. Neither path adds arbitrary LLVM SSA deoptimization.

Before the fix, an authentic depth-one postcall image was valid but the
executor rejected 6 terminal ScriptExit and 4 terminal LoopLatch fixtures,
all with adopted=0. After the fix, all 10 use adopted=1, preserve the
interpreter final variables, and survive forced full GC after native
Fiber retirement. New tests cover eight nested terminal IFs, both sides
of IF/ELSE, inner and outer counted loops, a callee changing a source
IF condition, and source invalidation (revision/digest/full production
cache reset) of both terminal ScriptExit and LoopLatch.

Native real resume/failclosed suite: 546/546 PASS. The new count
includes 20 root terminal/loop-body checkpoint transfers, 10 strict
root-only AfterCallee transfers, and 12 stale-source rejection runs.
GUI Scratch parity off/hotspot/eager: 2123/2123 each PASS.
Native CallFrame mirror: 25/25 PASS. Scheduler GC handoff: 5/5 PASS.

Frozen native executable:
build/async-jit/rt-001-terminal-controls-frozen-xyo
SHA256: 0687d2391a38b34285d6ae637d0b59d29870a236f453ce6465172e63510507c6

No commit/push. RT-001 remains in_progress and actual deopt default-off.

## Follow-up: nested procedure terminal branch to LoopLatch (2026-11-09)

The source handoff previously allowed terminal IF/IF-ELSE unwinding only
from root-only AfterCallee. The same exact-source successor may instead
occur after a child has returned into a non-root procedure frame, or
after an entire terminal child procedure has returned into an ancestor.

A shared, fail-closed helper
unwindTerminalConditionsToLatch(activeControls, targetPc) now validates
the original source latch and walks back only completed terminal IF or
IF/ELSE controls whose NEXT is empty, stopping at the exact active loop
frame. It does not re-evaluate IF conditions, re-run any already-completed
call, or infer any LLVM SSA temporary. On failure the loop/branch control
vector is unchanged. The saved numeric loop counters remain authoritative.

This shared helper is called for three already-proved sources of
LoopLatch:
- root-only AfterCallee;
- direct completed callee inside a non-root procedure;
- ProcedureExit chain unwinding to an ancestor's LoopLatch.

Five new fixtures cover non-root repeat->IF call, repeat->IF/ELSE else,
repeat->nested IF call, nested repeat->IF call, and
ProcedureExit->LoopLatch through three live native CallFrames. Before
this change, all five had valid original return metadata but refused
native handoff at the first AfterCallee boundary; after this change,
all five adopt exactly once, preserve interpreter variables/argument
strings, and survive full GC after transferring roots.

Native real handoff/failclosed suite: 581/581 PASS (546 + 35). These
new cases add 10 body-yield tests, 10 exact AfterCallee/GC adoption
tests, and 15 stale revision/digest/production cache reset refusals.
GUI Scratch parity: off/hotspot/eager 2123/2123 each.
Native LLVM CallFrame mirror with export probe: 25/25.
Scheduler rooted source continuation: 5/5.

Frozen integration binary:
build/async-jit/rt-001-procedure-latch-frozen-xyo
SHA256: 71701dfdc4989173a3e833647188f3ba797a56edcccca077480f991b8250dbe4

The runtime handoff remains opt-in and RT-001 remains in_progress.
Arbitrary SSA/effect-point resume, fully general conditionals, and
quiescent old-code/cache epoch retirement are not implemented.
Unrelated shared-tree changes are preserved, without commit/push.

## Follow-up: root-only AfterLoopBody effects-loss bug (2026-11-09)

**Actual red regression reproduced in the previous integrated native CLI**:
a nonwarp procedure finished first, leaving the root CallFrame alone.
At a later source-authenticated root repeat AfterLoopBody checkpoint,
simpleInterpreterResume() recognized afterLoop=true with depth 1,
skipped the procedure-frame reconstruction loop, and returned an empty
plan. adoptSimpleNativeSourceHandoff() then returned adopted=1 with
no interpreter stack, silently dropping the remainder of the loop and
the statement following it.

- 2,000 root repeats: Interpreter final x=2009, adopted final x=3.
- 20,000 root repeats: Interpreter final x=20009, adopted final x=3.
- Forced full GC after the bad handoff passed: GC safety alone did not
  establish execution equivalence.

**Fix**: a dedicated depth-one AfterLoopBody branch now requires
the root procedure identity, nonempty live lexical loop path, exact
top loop/resume PC, original source block and valid source snapshot.
sourceControls(root, rootHat, sourceLoopId, ..., atLoopCheckpoint=true)
reconstructs the entire root lexical control stack, including saved
counted-loop slots in Interpreter latch units, active conditional frames,
and the exact target loop. The final control must be that exact loop,
or adoption fails without modifying the live native Fiber. Because
this source-authenticated checkpoint is *after* the loop body, an
earlier completed nonwarp procedures_call in the root prefix can be
skipped without executing it again; other unrecognized prefix effects
remain rejected. No arbitrary native SSA/effect state is reconstructed.

XYO_RT_TEST_DEOPT_ROOT_AFTER_LOOP_ONLY=1 adds an opt-in scheduler
test gate requiring frame depth=1 and phase=AfterLoopBody, so the test
cannot erroneously pass by adopting the earlier nonwarp callee yield.

4 explicit end-to-end regressions: 2,000 and 20,000 numeric root
repeats, runtime-variable TIMES input, and root IF -> repeat. Each
adopts exactly once, matches interpreter/native final variables, and
performs full GC after releasing the Fiber; for each case, distinct
revision, source digest and production cache-reset+GC invalidation
attempts are refused without changed final results.

- Full native real resume/failclosed: **597/597 PASS** (581+16).
- GUI Scratch parity: off/hotspot/eager **2123/2123** each.
- LLVM mirrored CallFrame integration: **25/25**.
- GC-rooted native source handoff: **5/5**.
- CTest smoke for source revision, cache retirement, ABI and rooted
  call-stack: **4/4**; py_compile and git diff --check PASS.

**Shared-tree build caveat**: the regular CMake/Ninja xyo target
failed in the concurrently edited, RT-unrelated SDL renderer:
src/render/sdl_renderer.cpp references undeclared realDlssEnabled_
at line 4904. RT did not modify this file. To test the *new*
sequencer.cpp.o safely, the updated RT-owned object was placed into a
**copy** of an earlier successfully built libxyo.a, and the CLI was
relinked with existing CLI/SDL objects. This is a mixed-generation
integration test, not evidence that the normal full shared-tree build
currently succeeds. The resulting executable is:

build/async-jit/rt-001-root-afterloop-isolated/xyo
SHA256 11cbf85ad3a7adce7f7cb3f651ed591333e964f0a8584eedb7b46ddad97ce642

RT-001 remains opt-in/default OFF and in_progress. No commit or push.

## Follow-up: fail-closed adoption guard and diverse root loop controls (2026-11-09)

After discovering the actual incorrect successful adoption of an empty
depth-one AfterLoopBody restore plan (x=3 versus x=2009 or x=20009),
the executable adopter in Sequencer::adoptSimpleNativeSourceHandoff now
enforces its own final safety invariant BEFORE clearing any Thread stack
or releasing the native Fiber.

An empty plan with no root lexical controls, no live procedure frames,
no verified next Scratch statement, and no source-verified ScriptExit is
never executable; it is rejected and native execution continues.
Additionally, a ScriptExit plan cannot contradict itself by carrying a
pending next statement or live procedure frames. Legitimate
source-verified stackless ScriptExit remains supported.

The test-only XYO_RT_TEST_EMPTY_RESUME_PLAN=1 hook replaces a valid
plan with an empty one just before the check, intentionally simulating
a future source restoration bug. Exactly five independent tests verify
the previously dangerous root AfterLoopBody (including three nested
loops), root AfterCallee LoopLatch, root terminal conditional ScriptExit,
and direct terminal call. All fail closed; the native Fiber runs on to
the exact interpreter final variables. No post-adoption full GC is
performed because there was no adoption.

Seven new real source fixtures broaden the positive AfterLoopBody
coverage beyond the original four: nested repeat, repeat containing IF,
IF surrounding a repeat and subsequent committed statements, a
completed sibling repeat before the current latch, repeat-until,
IF-ELSE arm, and three deeply nested repeats. Every case transfers
once at a strict depth-one AfterLoopBody, preserves source effects and
loop state, survives a full GC, and also rejects stale revision, source
digest, and actual cache reset with GC. The 7 real cases contribute
28 test runs; the 5 fault-injection refusals contribute 5 more.

Final integrated evidence:
- Native LLVM real resume/fail-closed suite: **630/630 PASS** (597+33).
- GUI Scratch parity: off/hotspot/eager **2123/2123** each.
- LLVM native CallFrame mirror: **25/25**.
- GC-rooted source handoff: **5/5**.
- Related CTest: **4/4**; py_compile + git diff --check PASS.

The new sequencer.cpp.o was built from the current shared sources,
then linked into a COPY of the previous good archive, preserving its
previously compiled SDL renderer while unrelated concurrent rendering
changes prevent reliable full shared-tree rebuilding. No renderer file
was changed by RT-001; this is explicitly an isolated mixed-generation
integration result, NOT a claim of a clean full CMake build.

build/async-jit/rt-001-root-loop-guard-isolated/xyo
SHA256 5d7d188986d02f595e32b91950af57510217405c7517c235197b09e2220e453d

Default native-to-interpreter deoptimization remains off; generic LLVM
SSA/effect-point resumption is still unimplemented. No commit or push.

## Follow-up: cross-check exact native AfterLoopBody latch vs restore plan (2026-11-09)

The existing empty-plan check prevents discarding an entire native
continuation, but a partial nonempty plan can also lose already
committed effects. We reproduced this with a test-only hook:
XYO_RT_TEST_DROP_RESUME_LOOP=1 removes the innermost loop
control from a valid source restore plan while keeping the enclosing
IF control. Before the new check, the adopter returned adopted=1 and
full GC succeeded, but the script silently lost loop iterations:
original Interpreter x=2009, y=4000 versus corrupted adoption
x=10, y=2. This is a deliberately corrupted plan, not a known
failure of the unmodified source reconstruction algorithm.

Sequencer::adoptSimpleNativeSourceHandoff now independently decodes
the native source continuation immediately before modifying Thread.
When the native leaf checkpoint phase is AfterLoopBody, it requires:
- a saved source loop and exact native resumePc = innermost loop PC;
- no contradictory ScriptExit or post-call NextBlock;
- an Interpreter restore-plan terminal control that is a live loop;
- that exact terminal loop block must equal the immutable source
  symbol corresponding to the original native saved loop PC.

This check handles both root-only and nonroot procedure resumes;
an incomplete IF-only plan is rejected, leaving the native Fiber and
its GC roots intact. Four fault-injection regressions cover root
IF->repeat, terminal repeat->IF, nested dynamic counted loops
and nonroot nested_repeat: all reject, native continues, no GC-after-
adoption marker is present.

Nine new genuine positive root AfterLoopBody fixture families expand
the 11 previous fixtures:
- dynamically calculated repeat TIMES deliberately decremented
  during the loop; it must NOT be evaluated again on handoff;
- nested outer repeat with mutated inner TIMES;
- concurrent second green-flag hat updating shared x/y variables;
- concurrent second green-flag hat plus mutated dynamic TIMES;
- terminal IF->repeat, terminal IF-ELSE->repeat, and three nested
  terminal IF controls;
- outer repeat with terminal IF->repeat;
- outer repeat with terminal IF-ELSE->repeat.

Each performs one real native->Interpreter transfer, matches both
the unmodified LLVM and Interpreter final Scratch variables, survives
full GC, and separately exercises revision, digest and production
cache reset+GC invalidation refusals. 9*4=36 additional checks plus
4 missing-latch fault-injection checks = +40 over the earlier
630/630 baseline. All **670/670** native real/failclosed runs PASS.
GUI Scratch parity off/hotspot/eager **2123/2123** each; LLVM
CallFrame mirror **25/25**; GC-rooted source handoff **5/5**;
related CTest source/ABI/cache/rooted-stack **4/4**.

A root-only loop WITHOUT any procedure call did not generate a
source handoff observation under the tested native setup, so no
adoption occurred; this remains an uncovered capability (not an
incorrect successful deoptimization).

As for earlier RT tests, the unrelated shared SDL renderer compile
issue requires linking updated RT-owned sequencer.cpp.o against
a COPY of the previous working library with existing renderer/CLI
objects. The resulting CLI tests the new RT code but does not prove
a clean all-sources rebuild:

build/async-jit/rt-001-latch-guard-green/xyo
SHA256 d3bfdc395c8b6694de62b0ffaf168f653fbd89170126d066cfd8f95f2b4c4a94

No IR-001 ABI or rendering code changed. RT-001 remains opt-in/default
OFF and in_progress; generic LLVM SSA/effect resume is not implemented.

## Follow-up: source-verified root-only LLVM handoff without procedures_call (2026-11-09)

**Former gap:** An LLVM-compiled root script that NEVER executed
procedures_call did not instantiate NativeProcedureFrameSession:
the legacy source CallFrame mirror was created lazily only by the
first native xyo_proc_frame_enter_* procedure call. Even though LLVM
emitted exact repeat-loop checkpoints, the scheduler could neither
capture a source-root image nor deopt a plain root script.

**New opt-in producer:** When both XYO_JIT_CALLFRAME=1 and
XYO_RT_ROOT_ONLY_DEOPT=1 are configured during JIT compilation, a
top-level compiled script calls xyo_proc_frame_init_root_verified once
at entry, BEFORE any Scratch statement. The code generator passes
the compile-time source revision and 128-bit source digest (lo+hi).
The runtime re-captures the current ScratchCompilationUnit and creates
the existing NativeProcedureFrameSession (same IR-001 CallFrame ABI)
without fabricating a fresh source identity. Any mismatch is rejected
by the existing session constructor/digest verification. No session is
created for an inner/standalone interpreter->LLVM OSR function by this
hook. With the producer disabled, procedure calls continue to use
their original lazy initialization path.

The original nonroot yield-mirror counter deliberately remains
unchanged when the native frame is root-only. Under the strict,
test-only XYO_RT_TEST_DEOPT_ROOT_AFTER_LOOP_ONLY=1 gate, an actual
post-Fiber yield with a source-certified root AfterLoopBody and
depth=1 may be adopted at its first requested checkpoint even though
the nonroot yield counter is still zero. This gate is not used for
arbitrary SSA/InsideLoopBody phases. The existing source revision,
digest, lexical root stack and exact saved latch checks still apply;
the Fiber is never discarded without a verified interpreter plan.

Added five no-procedure-call projects under root_nocall_*:
- Simple counted repeat and a following statement.
- Dynamic TIMES whose source variable changes on every iteration;
  the original saved counter is used, never recalculated.
- Nested repeats (inner repeated over three outer iterations).
- Repeat terminal inside a root IF with no following script statement.
- Two concurrently scheduled green-flag hats, independently
  transferring their root native stacks while sharing Scratch x/y.

All five match ordinary LLVM and Interpreter final variables with
real native->Interpreter adoption and immediate forced GC after
releasing each native Fiber. The two-hat project has exactly two
independent source handoffs. Each fixture separately verifies three
fail-closed invalidations: source revision, same-revision source
digest mutation, and production resetCache()+full-GC while native
is suspended. All rejections leave final Scratch variables identical
to the unmodified Interpreter. New tests: 5*(1 positive+3 negative)
= 20; full native real resume/failclosed **690/690 PASS**
(previous 670/670).

**Integration caution and resolution:** First, the isolated executable
built by replacing only rt.cpp.o, codegen.cpp.o, backend_llvm.cpp.o,
and sequencer.cpp.o into an EARLIER library version passed the 690
source handoff checks but SIGSEGVed on GUI parity eager. Bisecting
isolated object combinations showed even a new rt.cpp.o alone
crashed against the old archive, while the new codegen.cpp.o alone
passed parity; this was an object-generation/link consistency issue,
not evidence of source-root handoff at default settings. Later,
a complete shared-tree CMake build rebuilt library and CLI objects
successfully. With that integrated CLI, GUI Scratch parity PASS
off/hotspot/eager 2123/2123 each, the full 690/690 native real
resume/failclosed suite passed again, 25/25 Native CallFrame
integration programs, 5/5 scheduler GC handoff programs and 4/4
related CTest smokes passed; Python syntax and git diff --check PASS.

Integrated CLI used:
build/async-jit/ir001-return-source/xyo
SHA256 f545e8e43a517f079764030cef8a5f1df10cbe6bd1b3be1e2aaba285e35540b9

RT-owned changes: src/jit/rt.cpp runtime initialization helper,
src/jit/backend_llvm.cpp symbol declaration/registration,
src/jit/codegen.cpp top-level-only compile-time opt-in call,
src/engine/sequencer.cpp strict first real root after-loop yield
adoption gate, tests/differential/native_simple_resume.py fixtures
and provenance regression. The IR-001
src/jit/procedure_frame_state.hpp ABI was deliberately left alone.
Default deoptimization and root producer both remain OFF. Arbitrary
LLVM SSA/effect-PC resume remains out of scope. No commit/push.

## Follow-up: root-only producer available without test-only phase gate (2026-11-09)

The initial proof-of-concept exercised the no-procedure-call root
handoff only with XYO_RT_TEST_DEOPT_ROOT_AFTER_LOOP_ONLY=1, a
diagnostic filter. The production opt-in mode has now been connected
to the exact same source-certified first-yield path: with
XYO_JIT_CALLFRAME=1, XYO_RT_ROOT_ONLY_DEOPT=1, and the existing
XYO_RT_NATIVE_DEOPT_ONCE=1, the first root-only native LLVM Fiber
suspension may transfer ONLY if the decoded root phase is
AfterLoopBody, the saved lexical loop path is nonempty, the saved
resumePc matches its last source loop PC, and the source/deopt
continuation passes every existing immutable proof and plan-frame
check. No test-only phase flag is required. Nonroot mirror-yield
statistics remain unchanged.

Two new end-to-end fixtures independently verify positive handoff,
full GC and exact interpreter parity for simple and nested no-call
root loops WITHOUT any diagnostic phase gate. A third verifies that
the same root project remains native with no adoption at all when
XYO_RT_ROOT_ONLY_DEOPT=0, even if a native deopt is requested.
The standalone root producer and native Fiber transfer are still
explicitly opt-in. Generic SSA/effect deoptimization is not added.

Full integrated xyo executable was rebuilt again and passed GUI Scratch
parity off/hotspot/eager all 2123/2123, CallFrame mirror25/25,
scheduler rooted handoff5/5, related CTest4/4 and Python/diff checks.
The three new cases bring the full native real/fail-closed suite to
**693/693 PASS** (690+3), including the default-off negative test.
The final integrated rebuilt executable has SHA256
26f4ac3fff4b0012b09288c04c27ac0d1dad08acbb1bc67395dbc6dc7f06e493
(build/async-jit/ir001-return-source/xyo). No commit/push.

## Follow-up: IR-001 lexical loop SSA diagnostics cross-checked on real deopt (2026-11-09)

IR-001 has introduced SourceSsaManifest/SourceSsaSnapshot for optional,
source-versioned typed SSA diagnostics. The LLVM-native checkpoint
currently emits **only numeric lexical loop-counter words**, not a
compiler-certified complete live SSA set. RT now integrates these
source diagnostic words with its independent source-authenticated
CallFrame validation, without touching the IR-owned shared ABI.

Opt-in execution settings:
- XYO_JIT_CALLFRAME=1 (existing native source frame capture)
- XYO_JIT_CALLFRAME_LOOP_SSA_DIAGNOSTIC=1 (IR diagnostic producer)
- XYO_RT_REQUIRE_LOOP_SSA_DIAGNOSTIC=1 (new independent RT check)
- XYO_RT_NATIVE_DEOPT_ONCE=1 (existing experimental handoff)
- XYO_RT_ROOT_ONLY_DEOPT=1 for no-procedure-call root scripts

At a decoded native AfterLoopBody leaf, the RT adopter builds a fresh
SourceSsaManifest using the *frozen compiler source dictionary*:
the original source revision/digest, exact owner procedure ID, exact
source loop-latch PC, and the active counted lexical slots in
ascending compiler slot order. The expected words come from the
independently decoded typed CallFrame loop counters; no manifest
fields are copied from the diagnostic sidecar to define what should
be true. The detached sidecar must match that manifest, contain
exactly the expected raw 64-bit counter words, and have no missing
or extraneous slots. No native Fiber/GC roots/Thread stack are
mutated until ALL checks succeed.

Regression:
- Root-only counted repeat with no procedures_call.
- Root-only nested repeat, including two active numeric counters.
- Completed root-loop latch after an earlier native procedure call,
  within an enclosing source IF.
- Nonroot nested repeat with a native procedure CallFrame.
Each of these performs a real native->Interpreter transfer with a
valid diagnostic sidecar and forced post-transfer full GC.
For each, independently reject four deliberately damaged states:
a flipped counter word, a changed slot ID, sidecar explicitly
dropped, and diagnostic producer disabled (sidecar absent).
Every rejection leaves the original LLVM Fiber running and final
Scratch variables equal the interpreter.

A fifth special case covers a repeat-until with no numeric loop
counter: a valid, explicitly captured EMPTY manifest must be
distinguished from an ABSENT sidecar. Valid empty metadata adopts;
a dropped or never-emitted sidecar fails closed. These are 4*5+3
= **23 added suite checks** over 693/693, resulting in
**716/716 native real-resume/fail-closed checks PASS**.
The fully rebuilt xyo CLI also passed GUI Scratch parity
off/hotspot/eager 2123/2123 each, native CallFrame integration
25/25, scheduler-owned GC handoffs 5/5, relevant CTest 4/4,
Python syntax check and git diff --check.

The exact integration executable was frozen as
build/async-jit/rt001-loop-ssa-verified-frozen-xyo
SHA256 1b3ecc70ccff4da5b0049da5ffb952a5dbc629f2d0cb3a16f1bbf1ff3c7761f6

Test fault injection settings:
XYO_RT_TEST_CORRUPT_LOOP_SSA=value|slot|drop

The compatibility guarantee matters: if
XYO_RT_REQUIRE_LOOP_SSA_DIAGNOSTIC is disabled, existing narrow
source-proven resumes do not require or depend on the IR diagnostic.
Even with the new verifier enabled, this is NOT a certificate of
complete LLVM live SSA, pending effects, GC-bearing values, exact
in-statement PC, or arbitrary deopt eligibility. A copied numeric
loop-counter sidecar is additional consistency evidence and cannot
independently authorize execution.

## Follow-up: Nth real suspended root-loop latch (2026-11-09 14:12 JST)

**Previously reproduced RED:** A no-procedure-call LLVM root script has
NativeProcedureFrameSession.yields() == 0 forever, because IR-001's
historical mirror metric counts only active nonroot procedure yields.
With XYO_RT_ROOT_ONLY_DEOPT=1 and XYO_RT_NATIVE_DEOPT_AFTER=2,17,233,
plain root, nested root and two concurrent hats never attempted native
deopt (adopted=0), despite 2200/6603/3900 genuine root LLVM
suspensions. AFTER=1 was the sole supported case.

**Fix:** Each Scratch Thread now has a scheduler-owned append-only
uint64_t nativeRootLoopYieldCount. The LLVM verified root-session
constructor initializes it to 0 at actual script entry, and
Thread::releaseFinished clears it when the thread is retired. It
does not alter IR-001's procedure mirror yield metric, its source
CallFrame ABI, any existing JIT field offset, or the default fast path.

Only with the existing opt-in XYO_RT_ROOT_ONLY_DEOPT=1 and native
deopt enabled, the serial scheduler increments the per-Thread count
ONCE per actual post-Fiber host suspension when:
- the native Fiber is still present and Thread is STATUS_RUNNING;
- its source session remains enabled, identical and valid;
- frame depth is exactly one and nonroot yields() is zero;
- the source checkpoint is a completed AfterLoopBody with an active
  verified lexical loop path and exact saved loop resumePc.

This is a count of completed, source-certified root loop suspensions,
NOT a guess from Scratch iteration indices or monotonic source block PCs.
Only once count >= XYO_RT_NATIVE_DEOPT_AFTER does the existing
source-handoff and full interpreter-plan validation run. On denial the
native Fiber, script variables and GC roots remain unchanged. The
counter saturates rather than wrapping. Per-Thread counting means a
second concurrent green-flag hat cannot donate checkpoints.

XYO_RT_TEST_ROOT_YIELD_COUNT=1 emits the test-only exact selection
marker [native-root-yield] with script, observed count, and target.

Regression evidence: the no-callee project fixtures now test native
deopt at:
- root_nocall_simple: N=2,17,233,2199;
- root_nocall_dynamic: N=17,233, including mutated original TIMES;
- root_nocall_nested: N=2,17,233,6599, across inner and outer repeats;
- root_nocall_two_hats: N=2,17,233,1699, each of two distinct hats
  independently adopting at exactly the requested source-safe yield.

All 14 checks compare final Scratch state with Interpreter, verify
exact selected count and script identity, adopt precisely once per hat
and force GC after releasing the native Fiber. At the later N=17
nested checkpoint, three distinct source invalidations (revision,
unversioned source-digest edit, production resetCache+GC) refuse
adoption and continue native execution correctly. N=2201 on a
2200-body script never invents a checkpoint or adopts. Opt-in IR-001
lexical numeric sidecar consistency at N=17 permits genuine SSA
counters and rejects a deliberately corrupted word. These 14+3+1+2
=20 new checks yield **736/736 PASS** (formerly 716/716).

Full current-tree library and CLI build PASS. GUI Scratch parity
off/hotspot/eager **2123/2123** each; native CallFrame mirror
**25/25**, GC-rooted native handoff **5/5**, relevant CTest
**4/4**, Python syntax and git diff --check PASS. Exact executable:

build/async-jit/rt001-root-nth-verified-frozen-xyo
SHA256 c24292bf4cd68f02336cfe4a954668e9ece28ca4a7216271334838e34835b7a2

The LLVM SSA liveness analyzer in IR-001 currently only reports
compiler-side candidates (including unsupported values), and does
not provide complete runtime capture or effect/replay proof. It was
not modified by RT-001. This new Nth-yield capability still
transfers only source-verified, committed loop latches. Arbitrary
live LLVM SSA or intra-statement effects remain unsupported.

No commit/push. All new features are opt-in; RT-001 remains
in_progress.

## RT-001: detached live LLVM integer SSA diagnostic snapshots (2026-11-09 ~14:57 JST)

IR-001 added a conservative *compiler-side* LLVM SSA liveness analyzer
and a pre-checkpoint volatile alloca spill of representable integer
instruction results. On concrete Scratch projects, a single LLVM
checkpoint can require dozens of integer words but also have dozens
of unsupported live values (pointers, non-integer values and function
arguments). The activation-local alloca was not previously accessible
as a stable scheduler-owned continuation, and numeric completeness
does not prove GC/effect continuity.

This milestone introduces the FIRST scheduler-independent copy of
actual LLVM-emitted, pre-optimization integer SSA values:
- XYO_JIT_SSA_LIVENESS=1 enables the existing IR-001 analysis.
- XYO_RT_CAPTURE_LLVM_SSA=1 enables new *opt-in* codegen
  instrumentation next to IR-001's numeric spill: after the genuine
  source checkpoint marker and before the pending native Fiber yield,
  call xyo_rt_capture_llvm_ssa with the native numeric alloca pointer,
  a constant array of (function-local instruction ID, bit width)
  descriptors, the numeric word count, unsupported count and numeric
  completeness, compiled source revision, original 128-bit digest,
  and a compiler-local site ID.
- The new runtime helper copies those words and descriptors into
  a Thread-owned NativeRtSsaCapture. It independently records the
  current source-native CallFrame owner procedure and resumed source
  loop PC; malformed source phases/widths/IDs/counts fail closed.
  The helper does NOT retain native stack pointers or global metadata
  pointers. A new source checkpoint invalidates the previous snapshot.
- This is inert numeric data: raw i64 may encode opaque/GC-bearing
  bits, so NO captured value is ever dereferenced, marked as a GC
  root, or applied as a restored Scratch Value. Unsupported live
  pointer/argument/float values are explicitly counted and never
  inferred away. No arbitrary executable SSA/effect-PC deopt is
  authorized by this diagnostic.
- XYO_RT_CHECK_LLVM_SSA_CAPTURE=1 asks the existing, otherwise
  independently source-certified RT adopter to require the detached
  compiler capture, exact frozen source revision/digest, current
  procedure ID and resume PC, and a host-owned non-cryptographic
  memory-integrity fingerprint. Absence or inconsistency rejects
  BEFORE Thread state or native Fiber is mutated.
- XYO_RT_REQUIRE_COMPLETE_LLVM_SSA=1 is an even stricter diagnostic
  veto: unsupported live values or compiler-incomplete numeric
  coverage must reject. Real compiled Scratch loops currently still
  contain dozens of unsupported live values and thus reject in this
  mode; this is the intended fail-closed behavior.
- XYO_RT_TRACE_LLVM_SSA_CAPTURE=1 emits metadata-only source
  checkpoint and post-Fiber-release proof logs, with *no* raw machine
  values or addresses. Test-only XYO_RT_TEST_CORRUPT_LLVM_SSA=value
  or =source changes the detached copy without touching LLVM's
  actual registers or native activation to verify rejection.

Observed examples, real LLVM native Fiber -> Interpreter:
- Root-only simple repeat: site 3 / source PC 5,
  41 numeric words and 37 unsupported live SSA values, resumed
  with a post-release verified detached copy and forced full GC.
- Nested root repeat: site 3 / source PC 6,
  49 numeric words and 48 unsupported values, correct loop state.
- Root repeat under IF after a completed nonwarp procedure:
  source PC 14, 41 numeric words / 37 unsupported.
- Nonroot nested repeat: site 1 / source PC 5,
  19 numeric words / 37 unsupported.
- Two concurrent native flag hats each receive an independent
  Thread-owned copy and independently release their own Fiber.

The dedicated regression runner now performs:
5 real positive snapshot/handoff/post-release integrity and full-GC
checks; for EACH, independent fail-closed checks for a flipped
numeric word, modified source PC, missing capture and strict
numeric-incompleteness rejection; another fail-closed check for
compiler-side liveness disabled; and two late-yield (N=17)
checks that preserve/corrupt the detached image. +28 checks
over the prior 736, total **764/764 real native resume/fail-closed
parity checks PASS**.

A fully integrated current-tree CMake xyo CLI build succeeded.
Real GUI Scratch parity off/hotspot/eager each **2123/2123**;
Native LLVM CallFrame mirror **25/25**; GC-rooted source handoff
**5/5**; relevant source/ABI/cache/stack CTest **4/4**;
Python py_compile and git diff --check PASS. Exact frozen CLI:

build/async-jit/rt001-detached-ssa-verified-frozen-xyo
SHA256 bcbe394723f3240c31f4bc7ae71cdf35240d622004be7f6ec7dfc0bf6e6f4e26

No direct changes were made to IR-owned
src/ir/llvm_ssa_liveness.hpp, src/ir/llvm_ssa_spill.hpp, or
src/jit/procedure_frame_state.hpp. New RT bridge code lives in
src/jit/codegen.cpp (additive post-lowering instrumentation),
src/jit/backend_llvm.cpp (registered helper), src/jit/rt.cpp
(copy at native checkpoint), src/engine/thread.hpp and thread.cpp
(detached storage), src/engine/sequencer.cpp (optional
provenance/integrity veto and post-Fiber-release diagnostic), and
tests/differential/native_simple_resume.py.

Remaining: per-site compile-certified COMPLETE SSA liveness
(including unmapped pointers/args and GC roots), effect position
and replay safety, plus a typed machine-to-Interpreter mapping.
A copied raw i64 is NOT a Scratch interpreter input. This capture
does not enable generic deoptimization. All new behavior is off
by default. No commit or push; RT-001 stays in_progress.

## RT-001: typed LLVM SSA extras -- float32/64 + numeric ABI args (2026-11-09 15:38 JST)

Starting from the real detached integer SSA copy, RT-001 now
captures additional representable live LLVM values: float and
double instruction results, i1/i32/i64 function arguments, and
float/double function arguments. **These remain diagnostic raw
bits.** A source-language Value passed as LLVM i64 may contain
NaN-boxed GC/string references; it is NOT assumed numeric and
is never used as a GC root or restored Scratch input.

New RT-owned src/jit/rt_ssa_extra_capture.hpp performs an independent,
conservative SSA reachability+dominance walk alongside the IR-001
numeric liveness report. At a real checkpoint it classifies the
previously unsupported candidates as:
- float32/64 instruction values: copied as bitcast + zero extension;
- i1/i32/i64 and float/double function args: copied as opaque bits
  with a separate, disjoint high-bit argument ID namespace;
- pointer instructions and pointer arguments: EXCLUDED, explicitly
  reported as unresolvedPointer;
- all remaining unsupported aggregate/vector/other types: excluded
  and counted separately as unresolvedOther.

The two analyses are run BEFORE inserting any LLVM diagnostic
allocas/stores, and their candidate populations must match exactly.
A bug in the first attempt was diagnosed and fixed: running the
second pass AFTER IR-001 inserted its diagnostic alloca caused it
to count artificial compiler instrumentation as live pointers.
On any such discrepancy emission is skipped, so opt-in RT capture
fails closed rather than losing a hidden live value.

The enabled compiler pathway builds one sorted, unique descriptor
stream per checkpoint: (32-bit pre-opt instruction ID or
disjoint 0x80000000|arg index, 32-bit tagged family+width).
The words are written to a native i64 array after the source
marker, then copied synchronously by the existing RT helper
BEFORE native Fiber suspension. It retains no original native
stack pointers and never stores or dereferences a pointer/GC
value as a live machine address. Widths and families are checked
at the runtime destination: integer instruction widths 1/32/64,
float instruction widths 32/64, integer arg widths 1/32/64,
float arg widths 32/64. Every i1 must be 0/1 and i32/float32
must fit a 32-bit raw word. IDs must strictly increase.

Both unresolved-pointer and other-value counts are included in
the host-owned snapshot fingerprint, not just their total.
The existing source-bound RT check is still mandatory. Test-only
corruptions XYO_RT_TEST_CORRUPT_LLVM_SSA=kind or =unresolved mutate
the detached metadata after capture and are rejected BEFORE
Fiber destruction.

Real Scratch LLVM -> Interpreter examples with correct final
Interpreter parity, full GC and verified copy after Fiber release:
- root_nocall_simple: 52 raw words, including 11 live double values;
  28 unresolved pointer values, other=0.
- root_nocall_nested: 61 raw words, including 12 floating results;
  38 unresolved pointers.
- nested_repeat (procedure): 24 raw words, 4 floating results and
  1 raw integer-ABI argument; 32 unresolved pointers.
- heap_string (GC-bearing argument): 16 raw words, 3 floating
  results and 1 opaque i64 argument; 18 unresolved pointers.
- root_afterloop_nested_if: 52 raw words including 11 floats;
  28 unresolved pointers.
- two green-flag hats independently own typed snapshots, source
  PCs and GC continuation; exactly two real deoptimizations.
- argument_snapshot: 20 words including 5 floating results and
  1 integer-ABI procedure argument; 22 unresolved pointers.

7 new typed real deoptimization/GC tests and 4 forged
descriptor-family/unresolved-pointer-count fail-closed tests:
**775/775 native resume/fail-closed parity tests PASS**
(previous 764/764 + 11).

The test-only standalone LLVM23 frontend smoke in
build/async-jit/rt001-typed-extra-ssa-isolated/
typed-ssa-liveness-smoke.cpp additionally verifies live float32,
live double, numeric i64 argument, both floating arguments, and a
pointer result: numeric=0, previously unsupported=6; classified
2 floating instructions + 1 integer argument + 2 floating arguments
+ 1 pointer, classification conservation PASS.

GUI Scratch parity off/hotspot/eager **2123/2123 each**,
Native CallFrame mirror **25/25**, GC-rooted handoff **5/5**,
related source/ABI/cache/stack CTest **4/4**, Python py_compile
and git diff --check PASS.

**Build caveat:** The current concurrent shared tree cannot
complete a fresh full CMake build because unrelated
src/render/sdl_renderer.hpp references an undeclared
render::DlssOpaqueCoverage during SDL renderer compilation.
No RT code attempted to change renderer/SDL. RT-related objects
(codegen.cpp.o, rt.cpp.o, backend_llvm.cpp.o, sequencer.cpp.o and
the ABI-dependent thread.cpp.o) were rebuilt and linked against
a copy of the previously verified static archive, so the
completed 775/775 + GUI parity tests validate an isolated,
mixed-generation executable, NOT a fresh whole-project build.

An initial negative test on the *partially* relinked executable
crashed at program teardown because the old thread.cpp.o in that
archive knew the previous NativeRtSsaCapture member layout;
replacing the matching thread.cpp.o resolved the allocator
error. No corruption/refusal test crashed with the corrected
isolated binary. Frozen verified executable:

build/async-jit/rt001-typed-extra-ssa-frozen-xyo
SHA256 eeb86ad449de9de2e86ea9842b4628ca310c604e7a4faf02951a90dc2fa20073

Remaining: precise GC/root typing for *all* live values,
effect/side-effect scheduling and replay proof, a typed machine
SSA to Scratch interpreter-frame mapping, actual executable
general SSA deoptimization. The new capture is default-OFF and
must never be mistaken for permission to resume from arbitrary
instruction PCs. No commit or push. RT-001 remains in_progress.

## RT-001: pointer-origin classification without retaining GC addresses (2026-11-09 16:04 JST)

The previous typed LLVM SSA snapshot conservatively excluded and
counted live pointer values, but could not identify where they
originated. This milestone adds an **origin histogram**, not any
claim that a pointer is a safely rooted object.

Within the RT-owned src/jit/rt_ssa_extra_capture.hpp SSA walk,
each *previously unsupported* live pointer is classified into
exactly one of seven groups:
0. function pointer argument;
1. stack AllocaInst;
2. derived pointer GetElementPtrInst / BitCastInst / AddrSpaceCastInst;
3. pointer-returning CallBase;
4. pointer-producing LoadInst;
5. pointer PHINode / SelectInst;
6. other pointer-producing LLVM instructions (including inttoptr).

All seven remain UNCAPTURED, GC-unsafe live values; in particular
the compiler origin does not prove allocation lifetime, address
space ownership, alias validity, or GC traceability. No pointer
is cast to integer or persisted as a machine address.

During opt-in LLVM checkpoint lowering, the histogram is embedded
as a **constant uint32[7] descriptor global** and passed by an
ephemeral pointer into xyo_rt_capture_llvm_ssa, alongside numeric
typed words and immutable source identity. The runtime synchronously
copies exactly seven counts to NativeRtSsaCapture::pointerOrigins
and rejects if their 64-bit widened sum differs from
unresolvedPointers. No descriptor-global pointer escapes the helper.
The seven copied counts are hashed into the detached host-owned
fingerprint, checked again by the source-verified RT adopter,
including after the native Fiber release.

Opt-in RT fault injections:
- XYO_RT_TEST_CORRUPT_LLVM_SSA=origin changes a bucket and its total,
  proving that the sum check refuses the candidate;
- =origin-swap moves one count between buckets, preserving the
  total; the independent saved fingerprint catches this forgery.

Real observed LLVM AfterLoopBody source checkpoints, in bucket order
[arg, alloca, derived, call, load, phi/select, other]:
- root_nocall_simple: [1,20,5,0,2,0,0] = 28 unresolved pointers;
- root_nocall_nested: [1,26,9,0,2,0,0] = 38;
- root_nocall_two_hats: two independent [1,20,5,0,2,0,0];
- nested_repeat native procedure: [1,19,9,0,3,0,0] = 32;
- heap_string with GC-managed argument: [1,10,4,0,3,0,0] = 18.

New persistent synthetic LLVM23
tests/platform/rt_pointer_origin_smoke.cpp independently constructs
a function with all seven origins live past one real checkpoint
and asserts that both liveness analyzers agree on 7 unsupported
pointer slots with histogram [1,1,1,1,1,1,1], no lost live values
and cross-function-marker rejection. Executed with:
c++ $(llvm-config --cxxflags) -std=c++20 -O0 -fno-exceptions
-fno-rtti -I. tests/platform/rt_pointer_origin_smoke.cpp -o
build/async-jit/rt001-pointer-origins-isolated/rt_pointer_origin_smoke
$(llvm-config --ldflags --libs core analysis)
and ran successfully.

Five new real Scratch positive native->Interpreter transfers prove
histogram conservation, exact source identity, full GC after Fiber
release and final Interpreter parity. Four fail-closed negative
checks (two projects x two forged histograms) reject without
discarding the native Fiber; two further tests at the exact 17th
native root loop yield verify normal histogram transfer and
sum-preserving forgery refusal. Thus 11 extra native checks:
**786/786 native resume/fail-closed parity runs PASS**
(previous 775/775).

GUI Scratch parity off/hotspot/eager **2123/2123 each**;
Native CallFrame mirror **25/25**; rooted source handoff **5/5**;
related source/ABI/cache/GC CTest **4/4**; Python syntax and
git diff --check PASS.

A full current-tree CMake xyo rebuild was attempted but remains
BLOCKED by an unrelated ongoing SDL renderer edit:
src/render/sdl_renderer.hpp:191 references undeclared
render::DlssOpaqueCoverage. RT does not modify renderer files.
The tested and frozen executable links the newly built codegen,
RT runtime, backend_llvm, Sequencer and Thread objects into a COPY
of the last healthy archive, so **this is an isolated/mixed-generation
integration test, not a fresh whole-tree build**. All tests above
used this exact executable:

build/async-jit/rt001-pointer-origins-frozen-xyo
SHA256 f10d7353007aa0130ad41b0d42d91f9fda256608fcfc1322dcc793260d72890e

This prepares explicit future work: resolve pointer owner/GC rooting
and source-effect replay, then map all retained typed live values to
a verified interpreter execution state. A histogram alone is not
a GC root or deoptimization certificate. IR-owned CallFrame/SSA
ABI files were not changed, and native->Interpreter experimental
features stay opt-in/default OFF. No commit or push. RT-001 stays
in_progress.

## RT-001: exact native-stack pointer lineage (2026-11-09 ~16:28 JST)

Previous pointer-origin metadata identifies the producing LLVM opcode.
This follow-up distinguishes compiler-live pointers whose entire
provenance can be traced to ONE specific native AllocaInst from
unknown/external/merged values. It is a **negative portability
classification**, not a GC lifetime/ownership certificate:
native alloca-derived addresses are invalid after native Fiber
destruction, must never become Scratch interpreter inputs, and must
not be added to the GC root set.

RT-only src/jit/rt_ssa_extra_capture.hpp implements
nativeStackPointerRoot(Value*). It follows GEP and BitCast back to
the source pointer, and follows PHI/SELECT only when every incoming
pointer is independently proven to reach the SAME alloca.
Arguments, loads, calls, IntToPtr, AddrSpaceCast, different-alloca
merges, cycles and chains >=64 links reject conservatively. The
tracker retains only static compiler Value* identities during IR
analysis, not raw runtime addresses. No pointers are copied to the
detached Thread image.

Per LLVM checkpoint, the extra liveness report now provides the
counts stackDerivedPointers (all live pointer SSA values with
single-root native alloca provenance) and distinctStackAllocas
(unique native stack allocation nodes referenced by those values).
Both counts are emitted as fixed compiler constant arguments in the
existing opt-in diagnostic helper. The runtime validates
0 <= distinctStackAllocas <= stackDerivedPointers <=
unresolvedPointers, copies the two integers into the Thread-owned
NativeRtSsaCapture and includes both in its fingerprint. RT
revalidates these relationships and the fingerprint before native
Fiber destruction. Neither count authorizes generic deopt.

Real Scratch LLVM live checkpoints yielded:
- root_nocall_simple: 28 unresolved ptr, 22 frame-local pointer
  SSA values, 20 distinct native alloca roots.
- root_nocall_nested: 38 unresolved, 30 frame-local, 26 roots.
- root_nocall_two_hats: two distinct Thread-owned independent
  snapshots, each 28/22/20.
- nested_repeat: 32 unresolved, 23 frame-local, 19 roots.
- heap_string: 18 unresolved, 12 frame-local, 10 roots.

New persistent tests/platform/rt_pointer_stack_lineage_smoke.cpp
builds LLVM23 functions with exact-source alloca-GEP chains,
PHI and SELECT where every incoming edge reaches the same native
alloca, mixed allocation roots, external arguments, loads,
IntToPtr, a legal cyclic self-PHI and an 80-step pointer chain.
It verifies positive same-root cases, rejection of ambiguous
cases and a live liveness report with two distinct stack roots.
Standalone compile/execution PASS.

Regression tests add 5 real positive source-certified native
Fiber->Interpreter adoptions with forced post-transfer full GC and
exact Scratch interpreter state; 4 deliberately corrupted
stack-derived/root counts on two different projects rejected
while the native Fiber continued; 2 real source latch N=17
late-yield positive/corrupt cases; and 3 independent stale-source
invalidations (revision, digest edit, production resetCache+full
GC) at N=17 that refuse adoption. 14 new runs means the native
suite reached **800/800 PASS**, previously 786/786.

GUI Scratch parity off/hotspot/eager each **2123/2123**;
native CallFrame integration **25/25**; GC-rooted scheduler
handoffs **5/5**; related source/ABI/GC/cache CTest **4/4**;
Python syntax/git diff whitespace check PASS.

The full current shared-tree CMake build was attempted but is
still blocked by unrelated src/render/sdl_renderer.hpp:191
undeclared render::DlssOpaqueCoverage. No renderer modifications
were made. All reported dynamic checks used an isolated binary
created by linking freshly built RT codegen, runtime, LLVM
backend, Sequencer and Thread object files into a copy of the
last healthy static archive, NOT a full fresh build. Frozen CLI:

build/async-jit/rt001-stack-lineage-frozen-xyo
SHA256 4e8a43e002895d59b228a310513a349bfd6b42548be5bbbf409c2bafa2e43f1e

Remaining: establishing actual GC object owner and correct root
handles for externally stored references, reconstructing typed
interpreter continuation values from LLVM SSA, exact effects/PC
replay proof. Stack-derived pointers are NEVER restorable.
New feature opt-in/default OFF, IR-owned CallFrame/SSA headers
unchanged, no commit or push, RT-001 in_progress.

## RT-001: independent IR-001 typed-SSA source manifest integrated (2026-11-09 ~16:49 JST)

IR-001 now supplies SourceTypedSsaManifest,
SourceTypedSsaSnapshot and verifyDetachedTypedSsa() in its OWN
source_typed_ssa.hpp / source_typed_ssa_bridge.hpp. RT-001 integrates
that existing verifier at genuine source-certified LLVM
AfterLoopBody native-to-Interpreter handoffs; **the new gate remains
diagnostic-only and is disabled by default**.

Enable in addition to the previous native source frame and LLVM
liveness flags:
- XYO_JIT_SSA_LIVENESS=1
- XYO_RT_CAPTURE_LLVM_SSA=1
- XYO_RT_REQUIRE_TYPED_SSA_MANIFEST=1
- XYO_RT_TRACE_LLVM_SSA_CAPTURE=1 for the optional proof trace

At the emitted LLVM checkpoint, the RT runtime now independently
copies the *compile-time read-only LLVM descriptors* into a
SourceTypedSsaManifest with compiled revision and 128-bit digest,
the live independently source-certified procedure/resumePc, and
compiler checkpoint siteId. This is a SEPARATE vector of original
emitted type tags and instruction/argument SSA slot IDs: it is
not recreated from the detached runtime vector on adoption.
To avoid a further Thread ABI-size change in the concurrently
rebuilt project, the separately copied manifest is stored as
NativeRtSsaCapture::compilerManifest rather than an extra Thread
member. Only the already-existing Thread unique_ptr to the SSA
diagnostic object is used. The manifest is discarded with that
object on a new source marker, native->Interpreter adoption, or
Thread retirement.

In opt-in RT adoption, XYO_RT_REQUIRE_TYPED_SSA_MANIFEST=1 implies
the existing detached LLVM SSA integrity gate. It additionally
requires an intact compiler-origin manifest whose source
revision/digest/procedure/resume PC independently match the source
dictionary. IR-001's verifyDetachedTypedSsa() validates exact
site ID, descriptor IDs/type tags/widths, canonical raw 1/32/64
bits, and fingerprint, producing an independently owned typed
SSA snapshot. Its .matches(expected) must also succeed.
The typed raw bits are checked again AFTER discarding the native
Fiber and before clearing the inert diagnostic copy.

For explicit proof of the NEW compiler manifest check (rather
than only checking its FNV), test-only
XYO_RT_TEST_CORRUPT_LLVM_SSA supports:
- site-rehash: change the runtime site ID and RECOMPUTE its local
  integrity hash; the separate original compiler site still rejects;
- kind-rehash: change a syntactically valid slot family, then
  recompute its local hash; original typed descriptors reject;
- manifest-drop / manifest-site / manifest-kind: damage only the
  separate original manifest; independent matching rejects it.

There are **23 new native tests**:
6 real positive typed SSA manifest adoptions (root simple, root
nested, two concurrent native hats, IF-contained root loop,
nonroot procedure nested repeat and GC-managed string argument),
each with exact interpreter state, forced post-Fiber-release GC
and typed source/site/word count post-release traces; 10 forged
typed descriptor/site/manifest rejection checks (5 faults on
simple and string-managed programs); two separate fail-closed
checks without the runtime producer or LLVM analyzer; one
valid and one forged N=17 native root checkpoint; and 3
source revision/digest/production resetCache invalidations
rejecting stale original compiler manifests.

**823/823 native resume/fail-closed parity PASS**
(previous 800/800 + 23).

GUI Scratch parity off/hotspot/eager all **2123/2123**;
Native CallFrame mirror **25/25**;
scheduler-owned GC-root handoff **5/5**;
relevant source/ABI/cache/rooted-call-stack + IR-001 typed
SSA manifest CTest **5/5**;
Python syntax and git diff --check PASS.
The newly available IR-001 manifest/adapter smoke target was built
and run as a real CTest, not only a standalone compiler test.

**Full build caveat:** current full shared-tree xyo build remains
blocked in concurrently edited src/render/sdl_renderer.hpp:191 by
undeclared render::DlssOpaqueCoverage. RT did not touch SDL.
The 823 native + GUI/GC tests used an isolated executable linked
from freshly rebuilt runtime, Sequencer and Thread objects with
the previously healthy native archive. This is NOT a fresh
whole-source-tree integration build. Frozen tested executable:

build/async-jit/rt001-typed-manifest-frozen-xyo
SHA256 fea9bfedd1c32a536424c0c2844409c46772bcfd32026468ae053e36c4b4748b

**Still not an executable arbitrary SSA deoptimizer.**
The trace holds raw opaque bit patterns, including potentially
NaN-boxed Scratch strings, but never adds them as GC roots or
applies them to Interpreter state. Pointers unresolved by
IR liveness, live effect state and mid-instruction/source PC
continuation remain unsupported. The source-certified, already
committed interpreter loop-latch plan remains the sole
basis for adopting; this typed verifier is an optional extra
fail-closed diagnostic consistency check, not authorization.

No IR-owned header changes, no commit/push, RT-001 in_progress.

## RT-001: native i64 SSA arguments vs GC-rooted Scratch CallFrame (2026-11-09 ~17:12 JST)

This milestone adds the first explicit, source-authenticated verification
of live **native LLVM argument SSA values** against their independently
GC-rooted Scratch arguments. It does NOT create roots for arbitrary SSA
temporaries or enable generic LLVM mid-instruction deoptimization.

Scratch JIT procedure ABI is function(JitCtx*, i64 boxedArg1, ...).
LLVM argument zero is the native context pointer, not a Scratch Value.
A captured typed SSA instruction ID with the high-bit argument namespace,
tag family 0x200, width 64, and index 1..N maps exactly to the corresponding
argument in the currently executing original source CallFrame.

New RT-only src/jit/rt_rooted_ssa_arguments.hpp implements
verifyRootedSsaArguments(), using the separately GC-rooted
RootedScratchCallStack exported by the source handoff. It rejects
frame depth/arity/index/source mismatch, i1/i32/floating native argument
types (not Scratch procedure ABI), missing roots, and full-64-bit mismatch
between captured native i64 bits and the Value resolved through the
GC-rooted CallFrame. String Value bits are NEVER dereferenced from SSA;
they are compared to the existing source-owned GC handle result.
The verifier counts matched observed native ABI arguments, matched
GC-managed string arguments, and all still-unverified instruction SSA
words. There is no claim of general live-SSA or arbitrary GC safety.

Opt-in XYO_RT_REQUIRE_ROOTED_SSA_ARGS=1 additionally IMPLIES the
independent IR-001 typed compiler SSA manifest check, even if
XYO_RT_REQUIRE_TYPED_SSA_MANIFEST is unset. This is a new veto on the
ALREADY source-certified committed loop-latch transfer, not authorization
for an arbitrary source effect point. The source-rooted argument audit
runs before and after native FiberPool release; no new GC Value handles
are allocated or exported from the diagnostic. New metadata-only
logs: [native-ssa-rooted-args] and
[native-ssa-rooted-args-post-release] when
XYO_RT_TRACE_LLVM_SSA_CAPTURE=1.

New test-only XYO_RT_TEST_CORRUPT_LLVM_SSA=rooted-arg-rehash flips
one captured live native i64 argument bit and recomputes the detached
SSA FNV fingerprint. The previous IR typed manifest-only check accepts
such a runtime word change because the compiler cannot know the runtime
argument value. The NEW rooted source-owner comparison rejects it,
without touching the actual native Fiber or GC roots. Both paths
retain exact final Scratch Interpreter parity. This isolates a real
previously unverified gap rather than merely collecting tests.

Real source-handoff results, all exact Interpreter parity with forced
GC after native Fiber release:
- nested_repeat: 1 live native arg matched, 0 heap strings, 23
  other still-unverified native instruction SSA words;
- heap_string: 1 native arg matched to 1 GC-rooted SStr,
  15 unrelated unverified instruction words;
- argument_snapshot: 1 native arg matched, no heap string;
- nested_call_string: 1 native arg matched to 1 GC-rooted SStr;
- root-only simple/nested and two concurrent hats: zero native Scratch
  procedure arguments, and no spurious source GC roots.

A separate persistent C++20 smoke
tests/platform/rt_rooted_ssa_argument_smoke.cpp verifies the pure
argument mapping logic against a mock read-only rooted-frame view:
exact string/numeric bits, malformed slot indices/tags/widths, a
rewritten fingerprint, invalid or missing source frame, arity mismatch
and root script bogus args. Compiled with -Wall -Wextra -Werror
-Wno-unused-parameter (one pre-existing unrelated warning)
and executed PASS. Actual GC collection is exercised in real Scratch
integration tests, not inferred from this mock.

Added 22 end-to-end native checks: 7 successful rooted ABI argument
handoffs, 4 forged word refusals, 2 typed-only vs root-owner controls,
4 N=2/3 real rooted string handoff/forgery tests, 2 missing
compiler producer/analyzer refusals, 3 source revision/digest/real
cache-reset invalidation refusals. Native resume/fail-closed
**845/845 PASS** (previous 823/823 +22).

GUI Scratch parity off/hotspot/eager **2123/2123 each**;
CallFrame mirror **25/25**; scheduler native rooted handoff **5/5**;
source/ABI/cache/rooted-call-stack/IR typed manifest CTest **5/5**;
Python syntax and git diff --check PASS.

Full current-tree CMake rebuild STILL BLOCKED by unrelated ongoing
SDL renderer edit: src/render/sdl_renderer.hpp:192 unknown
render::DlssOpaqueCoverage. RT left SDL untouched. All results above
used the ISOLATED executable, current RT Sequencer object linked into a
copy of a previously verified static library, NOT a consistent
whole-current-source rebuild.

Frozen tested CLI: build/async-jit/rt001-rooted-ssa-args-frozen-xyo
SHA256 fc8429adc7828d095de30c944c391effc222193ab0063fc02defc2792a518847

Generic live LLVM SSA, original effect/replay position, root ownership
for nonargument temporaries and truly arbitrary deopt remain unsupported.
Feature default OFF. No commit/push, RT-001 in_progress.

## RT-001: proven bit-identical LLVM SSA PHI/SELECT aliases of rooted Scratch arguments (2026-11-09 ~17:42 JST)

The existing rooted-source check covered live native *function arguments*,
not LLVM **instruction results**. RT-001 now provides a narrow static
provenance proof for an i64 LLVM instruction SSA value that must equal one
of those original GC-rooted Scratch argument values for every defined
execution of the corresponding LLVM instruction.

In RT-owned src/jit/rt_ssa_extra_capture.hpp,
sameBoxedArgumentRoot() recursively examines only i64 SSA sources:
- LLVM Argument 1..N (JitCtx* is Arg0 and never a Scratch Value):
  returns the argument's original LLVM index.
- An i64 PHI whose EVERY incoming operand traces to the EXACT SAME
  original argument: return that index. Different arguments reject.
- An i64 SELECT where both true and false inputs trace to that same
  original argument: return the index. Mixed inputs reject.
- Numeric arithmetic, bitcasts, load results, calls, freezes,
  pointer/int casts, unrecognized definitions, non-i64 types and
  self-referential/cyclic PHIs are all rejected. No speculative
  dataflow or permission to dereference raw SSA GC pointers.

The first synthetic test exposed exponential work on shared SELECT DAG
subgraphs: a chain with two arms pointing to the same value recursively
revisited the same subgraph. Fixed with a common-arm fast path and a
global budget of 512 recursive visits per query, alongside an existing
depth cap of 64. Budget exhaustion **returns no proof**. LLVM23 standalone
tests/platform/rt_ssa_rooted_alias_smoke.cpp PASS:
same-argument PHI and SELECT, nested PHI/SELECT, different-arg PHI/select,
arithmetic, freeze, load, pointer/non-i64 types, self-cycles and an
unfolded 70-Select depth limit.

The compiled native LLVM checkpoint now emits a fixed readonly
uint32[slot_count] table of exact source argument indices for the
existing sorted per-instruction typed SSA manifest: zero means
UNPROVEN. For original LLVM i64 Arguments the index is their native
function argument number; for an instruction it can be nonzero
ONLY if the compiler found the exact bit-preserving PHI/SELECT proof.
The analysis is performed BEFORE inserting diagnostic spills, using
the same IR-001 pre-instrumentation numeric live value definitions.
No raw heap pointer is emitted into this mapping.

The runtime copies the per-slot mapping twice into the detached,
Thread-owned NativeRtSsaCapture:
- compilerAliasArguments, original compile-time readonly mapping;
- aliasArguments, the runtime diagnostic copy, hashed into FNV.
The copied mappings are required to agree exactly, and each nonzero
index is checked against the source procedure's actual argument count
and SSA slot family/width. A mapping of a root script's native
context pointer to a Scratch argument is never allowed.

src/jit/rt_rooted_ssa_arguments.hpp now verifies exact 64-bit live
nonargument instruction words against the same independent
RootedScratchCallStack::argument(depth-1, argIndex-1) used to keep
heap-string procedure arguments alive. A matching alias counts as
matchedAliasedInstructions, and a confirmed GC string owner counts as
rootedAliasedStrings, distinct from direct argument counts. No new
GC handles are registered or inferred; no SSA value is made an
Interpreter input. This remains an opt-in supplementary veto for
already source-certified AfterLoopBody transfers:
XYO_RT_REQUIRE_ROOTED_SSA_ARGS=1 (with the prior LLVM liveness and
detached capture flags). It rechecks after Fiber release.

Test-only XYO_RT_TEST_CORRUPT_LLVM_SSA=alias-map-rehash modifies a
proven origin index in the runtime copy and recomputes FNV. The
original independent compiler map catches this even if the raw SSA
word/typed compiler descriptor are unchanged. Negative control:
IR typed-manifest-only adoption still succeeds after modifying only
this RT-specific alias map, correctly illustrating the extra check.
Another test-only mode alias-word-rehash flips a live proven
NONARGUMENT instruction word; its semantics are verified in the
dedicated C++ mock because the real Scratch fixtures below currently
have no such instructions.

The C++20 mock in tests/platform/rt_rooted_ssa_argument_smoke.cpp
also verifies a nonargument i64 SSA result matching the same
GC-rooted original string argument, with a separate independently
matched direct argument; wrong SSA bits, wrong owner index,
forged compile-time mapping and missing mapping entries all reject.
Both dedicated C++ smoke programs PASS (LLVM 23 used for the
compiler test).

**Limitation explicitly observed:** On the real Scratch examples
root_nocall_simple, root_nocall_nested, root_nocall_two_hats,
nested_repeat, heap_string, nested_call_string, argument_snapshot
and parent_ifelse_repeat_string, the actual compiler liveness
checkpoints identified **zero new nonargument i64 PHI/SELECT aliases**.
The real engine nevertheless emitted/transported the new mapping,
validated original i64 source *argument* identities, and safely
refused rehashed provenance changes. The new nonargument mapping's
positive behavior has been proven only with synthetic LLVM IR and
the rooted CallFrame mock, NOT with an observed real Scratch
nonargument SSA source match.

Added 24 real source-certified handoff tests: 8 positive projects
(including two simultaneous hats), 5 forged original source-index
refusals, 2 typed-only vs rooted-map control cases,
4 positive/negative second/third native yields, 2 absent
producer/analyzer refusals and 3 stale revision/digest/production
resetCache refusals. **869/869 native real resume/fail-closed parity
PASS** (previous 845/845 +24). All successful transfers perform a
real post-Fiber-release full GC and match the Interpreter state.

Standard GUI Scratch parity off/hotspot/eager each **2123/2123**,
Native CallFrame integration **25/25**, source GC handoff **5/5**,
related ABI/IR typed SSA CTest **5/5**, C++ pure LLVM/ABI smoke
both PASS, Python py_compile and git diff --check PASS.

The tests used an ISOLATED executable linked from freshly
recompiled codegen.cpp.o, rt.cpp.o, backend_llvm.cpp.o,
sequencer.cpp.o and thread.cpp.o plus updated rt_blocks.cpp.o
for a concurrent datetime-helper symbol, into a copy of the
last healthy static archive; this is NOT a completely fresh
whole-project rebuild. The shared tree includes concurrently
modified SDL/DLSS code. Frozen tested executable:

build/async-jit/rt001-ssa-alias-verified-frozen-xyo
SHA256 f1dde7645f4c2bec314500ad4552ce4cce726372f1f9e8ae2016f3f25dc8a67f

No general GC ownership for other native SSA temporaries, pointer
liveness completion, effect replay or arbitrary instruction-PC
deoptimization. No existing behavior is enabled by default.
No commit/push, RT-001 remains in_progress.

Full-tree integration build verification at ~17:44 JST:
cmake --build build/async-jit/ir001-return-source --target xyo -j 4
FAILED at src/render/sdl_renderer.cpp.o because
src/render/sdl_renderer.hpp:192 uses unresolved
render::DlssOpaqueCoverage. This is a concurrent SDL/DLSS change,
not the RT SSA compiler or verifier. No SDL edits were made.
Do NOT claim full whole-current-tree integration build success.

## RT-001: real Scratch nonargument LLVM PHI -> already rooted string (2026-11-09 ~18:19 JST)

The previous LLVM23 static ancestry and RootedScratchCallStack mock had no real Scratch positive case: all sampled source checkpoints showed zero nonargument SSA aliases. This turn added the actual generated Scratch .sb3 fixture ssa_alias_branch_string in tests/differential/native_simple_resume.py. It passes a long GC-managed Unicode string (including a supplementary UTF-16 character, 𠮷) to a nonwarp custom procedure. In repeat(2400), both IF/ELSE branches set x to original argument a, followed by set z to x and change y by 1. The condition changes as y progresses. Source argument roots remain owned by the original GC-rooted CallFrame.

Precisely XYO_JIT_LAZY_TYPED_SSA=1 causes existing loop promotion to produce the real i64 LLVM typed.merge.bits PHI. With the switch enabled, compiler traces show [jit-phi] candidates=1 boxed=1 and [rt001-ssa-extra] aliasable=1 at both real AfterLoopBody checkpoints. In the actual source-authenticated native-to-Interpreter transfer, [native-ssa-rooted-args] reports matched=1 heap_strings=1 nonargument_words=22 checked=1 aliased=1 aliased_strings=1. Both original native i64 argument and its nonargument PHI are bit-for-bit identical to the same original rooted SStr argument. After the native Fiber is released and a full Scratch GC runs, rechecking the independently rooted source CallFrame reports intact=1 with both matches. Interpreter final x/y/z state matches exactly. With XYO_JIT_LAZY_TYPED_SSA=0, boxed PHI does not appear and aliasable=0 (correct negative control).

16 new real native regressions: four live PHI GC transfers at N=1/2/5/17 native yields; four fail-closed corruption modes alias-word-rehash (PHI raw bits forged with local FNV recomputed), alias-map-rehash (source owner forged), alias-both-drop (both compiler and runtime alias map missing), and rooted-arg-rehash; two typed-manifest-only negative controls documenting the missing source owner proof; one lazy SSA OFF no-invented-PHI control; two missing compiler producer/analyzer refusals; and three stale source revision/digest/production resetCache and full GC refusals. All incorrect candidates are rejected without discarding the actual native Fiber, and final Scratch state still matches Interpreter.

Security correction: production verifyRootedSsaArguments now requires a complete original compiler alias mapping even if the mutable runtime map AND original map have been erased with a repaired FNV. This is a strict requireCompiledAliases=true argument at the scheduler both BEFORE and AFTER Fiber release; the existing permissive legacy API remains available for standalone mock tests. Extended tests/platform/rt_rooted_ssa_argument_smoke.cpp verifies both modes.

FULL RESULT: 885/885 native actual source resume/fail-closed parity PASS (prior 869 +16). Actual native procedure CallFrame mirror 25/25 PASS, GC-rooted scheduler handoff 5/5 PASS. Real GUI Scratch parity off/hotspot/eager each 2123/2123 PASS. Related CallFrame/ABI/rooted/cache/IR typed SSA CTest 5/5 PASS. Standalone LLVM23 PHI/SELECT compiler ancestry smoke and C++ rooted source argument/verifier smoke PASS. Python py_compile and git diff --check PASS.

Build reproducibility: concurrent agents deleted shared build/async-jit in the middle of the first test execution, causing a FileNotFoundError unrelated to Scratch parity. RT rebuilt TWO complete internally consistent LLVM JIT Release binaries OUTSIDE the shared build directory:
- /home/kicky/.rt001-phi-build-20261109/xyo (headless, 885/885 PASS);
- /home/kicky/.rt001-phi-sdl-build-20261109/xyo (SDL-enabled, 885/885 PASS, GUI parity 3x2123/2123, CTest 5/5).
Both needed a strictly build-local forced inclusion of existing src/render/dlss_opaque_coverage.hpp because the concurrently edited SDL header references render::DlssOpaqueCoverage without including the defining header. The headless renderer object used the isolated Ninja object's -include flag; the SDL enabled build used CMAKE_CXX_FLAGS with -include path. NO SDL renderer source or shared CMake file changed. These are full recompiles with a documented header workaround, not an unmodified stock build.

Frozen verified SDL binary /home/kicky/.rt001-phi-verified-sdl-20261109-xyo ; SHA256 a2b9e556430b0eaa71b46db5bc346068d6c39c1cef88e3a007f9e6323be7e550 .
Frozen verified headless binary /home/kicky/.rt001-phi-verified-headless-20261109-xyo ; SHA256 be95c9f68539df56a0760750ca925f808d57ce3d271198c6ec46a3e28c637e5e .

This is still ONLY an opt-in supplementary fail-closed diagnostic for source-certified AfterLoopBody native-to-Interpreter handoff. It neither creates GC roots for arbitrary SSA nor restores arbitrary pointer SSA/side effects/mid-statement PCs. Source procedure CallFrame owns the original string. General SSA GC liveness and effect replay remain unsolved. RT-001 in_progress, no commit or push.

## RT-001: native-generated GC-managed strings protected across a true Fiber handoff (2026-11-09 ~19:23 JST)

This extends the earlier *original argument identity* certificate to a
distinct source of values: JIT-owned i64 Scratch SStrs returned by known,
boxed string-producing native helpers. It does NOT certify arbitrary
nonargument SSA, pointers or effect replay.

Compiler-side src/jit/rt_ssa_extra_capture.hpp now has
trustedGeneratedStringSource(): a bounded (64 recursion / 512 total visits),
cycle-rejecting LLVM23 analysis. An i64 value qualifies only if it is a
direct CallBase to one of xyo_join, xyo_join_suffix, xyo_join_strings,
or an i64 PHI/SELECT whose *every* incoming source qualifies. Other
operators, arbitrary calls, loads, arithmetic, function arguments,
pointer values, missing edges and cycles are unproven. This is independent
of sameBoxedArgumentRoot(); a generated new value cannot be relabeled as
the GC-rooted original source argument.

src/jit/codegen.cpp exports a compiler-readonly per-live-SSA-slot
uint32 producer certificate (0 or 1) alongside existing typed SSA
descriptors and argument alias information. src/jit/rt.cpp copies the
original and runtime producer masks, and only when the explicit opt-in
XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1 is enabled, attempts
rt::retainGeneratedSsaRoots() at the original source-certified native
checkpoint. It rejects bad descriptor types/owner aliases, invalid
Scratch boxed tags, nonexistent GC allocations, wrong GC object type,
and GC handle capacity failures. The exact membership check uses
Gc::isObject(header) *before* dereferencing header->type; the
newly generated heap-string Value must name an allocated GcType::Str.
Each validated string is retained by a Thread-owned GcValueHandles root
provider; an empty immediate string needs no heap root. On any error,
the capture is not published. This is NOT an unsafe conservative root of
arbitrary memory. The ownership is attached to a detached snapshot
that outlives the native Fiber and is released on normal Thread reset
or successful interpreter adoption.

src/engine/sequencer.cpp adds independent strict validation of the
source marker, typed manifest, original argument mapping and newly
generated string certificate before committing the transfer. It rejects
forged i64 words with recomputed FNV, forged/dropped producer masks,
and missing released GC handles. It checks exact resolved values
again after native FiberPool::release; test-only
XYO_RT_TEST_GC_DURING_SSA_TRANSFER=1 performs a **real full GC**
between Fiber retirement and the second check.

New real Scratch fixture ssa_new_join_branch_string has a nonwarp
procedure with repeat(2400) and changing IF/ELSE arms, each generating
a distinct Unicode heap string using operator_join and consuming the
merged result afterward. With XYO_JIT_LAZY_TYPED_SSA=1, LLVM emits a
genuine i64 boxed PHI merging the two newly created host join results:
[rt001-ssa-extra] generated=1, aliasable=0 (not an original arg alias).
At real native N=1/2/5/17 source checkpoints:
[native-ssa-generated-roots] certified=1 rooted=1 immediate=0 checked=1,
and [native-ssa-generated-post-release] certified=1 rooted=1
immediate=0 intact=1 **after real full GC**.
Interpreter vs native final states match exactly.

18 additional real checks: 4 successful new-string handoffs and two
forced GCs per handoff; 4 independent forged word/producer-map/both-maps/
removed GC-handle checks; 2 negative controls showing typed-manifest
alone cannot prove ownership; 1 no-lazy-PHI control; 2 missing LLVM
producer/analyzer refusals; 3 stale revision/digest/production cache
reset refusals; and 2 original argument/argument-PHI controls to
exclude accidental reclassification. All incorrect source handoffs
refuse and the original suspended native Fiber executes correctly.

**903/903 native real resume/failclosed parity PASS** on frozen, fully
rebuilt SDL-enabled LLVM JIT Release binary (prior 885 +18).
GUI Scratch parity off/hotspot/eager each **2123/2123**; native
CallFrame 25/25; rooted source handoff 5/5; LLVM23 compiler
synthetic smoke now also verifies generated join/PHI/SELECT and
mixed/arithmetic/cycle/loaded/untrusted source rejection, PASS.
Python py_compile and git diff --check PASS.

Frozen CLI: /home/kicky/.rt001-generated-ssa-verified-20261109-xyo
SHA256 c9ba328f4579b859ddfe4b549ef23f5eb8299dff92c6f8840c021418b084c1b5.
Full SDL source was built with the previously documented
*build-local forced include* of src/render/dlss_opaque_coverage.hpp
to avoid another concurrent agent's missing include issue; SDL
source files were not modified by RT.

Important: the root is acquired only after the native source checkpoint.
This proves that the observed generated string was alive then and is
kept alive during the handoff; it does not establish that earlier
uncheckpointed native SSA values were GC safe, nor does it produce a
general executable SSA deoptimization environment. Compiler recognition
and exact GC membership are BOTH mandatory. Feature default OFF,
RT-001 in_progress, no commit/push.

## RT-001: defer native Fiber auto-GC until certified source checkpoint (2026-11-09 ~20:00 JST)

The prior milestone installs an exact GC handle for newly JIT-created Unicode
strings only when the native Fiber reaches a source-certified LLVM
AfterLoopBody checkpoint. Before this checkpoint a new string may still
exist only as a raw SSA register, which is not an exact GC root.

Under the SAME opt-in XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1 (default
OFF), src/engine/sequencer.cpp now wraps each c->fiber->resume()
with a stack-scoped Gc::DeferCollect. This suppresses the
heap-threshold **automatic collector inside the running native
activation** and restores the previous deferred state immediately
when the Fiber yields or completes. It preserves nesting with any
outer GC deferral. When the flag is not set, the original fast path
continues to call c->fiber->resume() directly. The protection is
restricted to the actual native Fiber activation (not a full Scratch
frame). This is an experimental memory/GC tradeoff, not an assertion
that every LLVM live SSA value is GC-rooted.

Thread-safety fix: src/engine/sequencer.cpp::parallelEnabled() now
forces the serial scheduler when the generated SSA root experiment is
active, even if XYO_RT_NATIVE_DEOPT_ONCE is unset. This avoids running
GcValueHandles root-provider registration on workers or taking the
optimized Fiber::resumeChain path, neither of which was protected by
per-Fiber serial checkpoint assumptions. No global performance change
occurs when the opt-in flag is disabled.

The test-only XYO_RT_TRACE_NATIVE_GC_DEFERRAL=1 emits
[native-gc-deferred] script=... before=<count> after=<count> unchanged=1
for the collector count across each native resume. A failure in this
property flags a collector that ran despite automatic deferral; it
is NOT evidence that a threshold was actually crossed in every tested
resume. The main test sets XYO_GC_THRESHOLD=1 MiB and explicitly
collects AFTER native Fiber release and BEFORE the new retained
string root is destroyed. These intentional explicit collect calls are
allowed and the exact source-rooted string is still verified.

Five new end-to-end native tests: for true JIT-generated string PHI
ssa_new_join_branch_string, native source yield N=1/2/5/17 at low
threshold with deferral trace, forced post-Fiber-release full GC and
exact Interpreter parity; plus a default-off heap_string control
which proves no new deferral trace occurs when generated roots are
disabled. **908/908 native source resume/fail-closed parity PASS**
(previous 903 +5) on a full-source SDL-enabled native Release build,
with current concurrent project sources. Native CallFrame **25/25**,
scheduler-owned rooted source handoffs **5/5**, relevant IR/ABI/cache
CTest **5/5**, Python py_compile and git diff --check PASS.

Full-source frozen executable:
/home/kicky/.rt001-precheckpoint-gc-frozen-20261109-xyo
SHA256 cf71aa8394539edbc603e299ef82f6ec5b6be404d76c40cb9df63c9c58cac5f3.
This CMake configuration still uses a build-local forced include for
the independently edited SDL renderer header's missing type definition.
No RT changes to SDL renderer source; no commit/push.

Limitations: Gc::DeferCollect suppresses threshold-triggered collection
from Gc::alloc / allocCons, but DOES NOT veto direct Gc::collect() or
collectIfNeeded() calls. Other unsupported live SSA values are not
made roots. An activation that allocates many strings without
returning to the scheduler may use more peak memory. This is NOT
general arbitrary SSA GC safety/effect-PC deoptimization.

GUI Scratch parity on this *new* full-source build currently TIMES OUT
at 120 seconds in play/off, including a solo rerun; do NOT claim a
2123/2123 pass for this binary. The preceding frozen generated-string
binary passed GUI parity 3x2123 earlier, but current concurrent changes
may have affected GUI completion; this needs isolated attribution.

## RT-001: 20:33 JST verification — actual threshold crossing and GUI regression resolution

A previous RT-001 opt-in patch protected newly JIT-created boxed SStr
SSA by scoping the serial native Fiber::resume() in Gc::DeferCollect
when XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1. The four first
XYO_GC_THRESHOLD=1 MiB Scratch integration cases had verified that
Gc::collectCount did not change DURING each native activation, and that
a full GC AFTER native Fiber release preserved the exact GC-rooted
new string. However, just setting a low threshold did not establish
that any individual native activation actually crossed its effective
GC allocation threshold (Gc defaults to an initial 8 MiB limit).

This turn adds a DETERMINISTIC native GC primitive test
tests/platform/rt_native_gc_deferral_smoke.cpp. It sets the actual
singleton GC heap threshold to 1 MiB with Gc::setThreshold, roots
one real SStr with ir::GcValueHandles and allocates 140 additional
real 8K UTF-16 flat strings from Gc::alloc inside Gc::DeferCollect.
At least **2,320,578 bytes** allocated exceeds the TRUE active
1,048,576-byte threshold by >1 MiB, while the collector invocation
count remains unchanged. Immediately after the defer scope, the very
next large SStr::create automatically triggers Gc::collect, and
the originally rooted string's exact handle resolves and the
GcHeader is still a registered object of GcType::Str. The test
also forces assertions active under Release -DNDEBUG via #undef
NDEBUG before <cassert>. This is a direct test of the GC primitive,
not a synthetic claim about native SSA register state.

CTest integration appended in tests/CMakeLists.txt as
xyo-rt001-native-gc-deferral-smoke (normal, non-WASM target linked
against xyo; no concurrent audio/IR test registrations removed).
It builds and PASSes in the current Release configuration. Two
preexisting RT-owned CTests for newly generated SSA roots and
compiler source certificates PASS too. Full selected CTest
**8/8 PASS**, including IR-001 typed SSA manifest,
native CallFrame revision, cache retirement, ABI and rooted stack.

GUI TIMEOUT ATTRIBUTION: the earlier full-build frozen CLI
/home/kicky/.rt001-precheckpoint-gc-frozen-20261109-xyo
was built before recent concurrent changes to src/audio/sdl_audio.cpp.
XYO_MUSIC_TRACE at startup showed that old build loading the
51-MB /usr/share/mscore-4.7/sound/MS Basic.sf3 via
the FluidSynth backend and attempting ALSA/JACK/PulseAudio
connections EVEN with SDL_AUDIODRIVER=dummy, before the Scratch
parity program completed. Both parallel and solo GUI off-mode
tests reached the 120-second timeout. The current source's
audio backend now selects MIT TinySoundFont SF2 by default
unless XYO_MUSIC_BACKEND=fluidsynth is explicitly requested;
the RT agent made NO audio source changes. After rebuilding the
current source, the REAL GUI Scratch parity is restored:
off **2123/2123**, hotspot **2123/2123**, eager
**2123/2123**, all PASS. This establishes an audio initialization
difference correlating with the timeout, rather than a GUI
interpreter miscompute; the precise internal blocking operation
in old FluidSynth was not isolated further.

New full SDL-enabled LLVM JIT Release binary (current source and
RT-001 auto-GC deferral changes, retaining the previously documented
build-local renderer include workaround) frozen to
/home/kicky/.rt001-native-gc-protected-20261109-xyo
SHA256
57d333b7cb9579bbe8a0d0eebaf569cc611c0586dc0b23c829fda41326d7e9b5.

On that EXACT frozen binary, full source-certified native resume /
fail-closed regression **908/908 PASS**, including low-threshold
generated-string + explicit post-Fiber full-GC integration cases.
Native CallFrame integration **25/25**, scheduler-owned GC-rooted
handoff **5/5**, current CTest selected **8/8**. Python py_compile
and git diff --check PASS. No commit/push.

Call-site audit: the normal runtime's direct Gc::collect() calls
identified in src/engine/sequencer.cpp were scheduler-side source
invalidation/transfer/test-after-release paths; explicit
collectIfNeeded() is at the end of the parallel scheduler batch.
For the supported serial native code path xyo_join* allocation
uses Gc::alloc/allocCons and its heap-threshold automatic
collection. This explains why the opt-in DeferCollect protects
the intended interval. The collector API still permits other
code/embedders to call Gc::collect directly, and such calls are
NOT prevented. Long native activations may temporarily increase
heap use. Arbitrary LLVM SSA liveness, general pointer rooting,
and effect-PC deoptimization remain unfinished. RT-001 stays
in_progress and the new feature default is OFF.

## RT-001: direct GC is now deferred in opt-in native Fiber (2026-11-09 ~21:05 JST)

This supersedes the earlier statement that a native direct Gc::collect()
call remains unprotected. Gc::DeferCollect guarded only automatic
threshold collections. Added separate Gc::DeferExplicitCollect with
nested, atomic depth counting and an atomic pending direct-GC request.
Gc::collect() now remembers requests made inside that guard, without
scanning or sweeping live unrooted LLVM SSA values. Gc::collectIfNeeded()
honors the pending request even below the heap threshold, but never
runs while the outermost explicit guard remains active; successful
collection clears the pending flag. Default behavior outside the
guard remains unchanged.

Under XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1 only, the serial
Sequencer::jitStepThread wraps native Fiber::resume in BOTH auto
and explicit GC defer guards. A pending request is fulfilled at
an authenticated native-to-Interpreter handoff only AFTER native
Fiber and detached SSA snapshot retirement. When deopt is refused
and the Fiber remains runnable, no request is serviced at that
unsafe yield: it is serviced after the native script finishes
and the Fiber is retired. A manual full post-adoption GC test
also fulfills pending requests normally. Nested deferral cannot
re-enable explicit sweeping early.

Test-only XYO_RT_TEST_EXPLICIT_GC_BEFORE_SSA_ROOT=1 in the LLVM
host capture callback triggers a REAL Gc::collect() once inside
the native Fiber BEFORE exact generated-string GC handles are
created, at a source-certified live i64 PHI checkpoint. It can
only run when generated SSA roots are explicitly enabled. The
expected event is [native-ssa-explicit-gc-guard] before=0 after=0
unchanged=1 pending=1. Real Unicode Scratch join PHI examples
at yields N=1,2,5,17 verified deferred collection and exact
post-Fiber GC root preservation with Interpreter state parity.
An absent-opt-in negative control keeps this injection inert.

The existing deterministic Release CTest
xyo-rt001-native-gc-deferral-smoke now also tests nested explicit
requests: no GC under the outer guard, no GC when the inner scope
ends, one pending collection once collectIfNeeded is invoked
outside BOTH scopes, even with heap threshold raised to 128 MiB.
Original exact rooted SStr remains intact. This is in addition
to the true 1-MiB automatic GC threshold crossing test
(2,320,578 bytes allocated while deferred).
It passed in Release mode with assert enabled despite -DNDEBUG.

Diagnostic XYO_RT_TRACE_NATIVE_GC_DEFERRAL=1 now includes
allocated_bytes=<n>, the allocation delta across each native
activation, to assess temporary memory pressure. This is a
metric, NOT a hard cap or proof of bounded memory use.

New 7 real native source handoff test cases:
four actual pre-root native direct-GC requests at yields
N=1/2/5/17, one feature-disabled inert injection control,
one post-handoff pending-collection drain without test-only GC,
and one forged SSA with deopt refusal followed by native Fiber
completion before the outstanding request is drained.
Full suite count will be recorded after the full rerun finishes.

Frozen current-source LLVM JIT SDL-enabled Release binary:
 /home/kicky/.rt001-explicit-gc-frozen-20261109-xyo
 SHA256 9f468ff8c9d5caa07d4421b8b7bee333dba4f6648a1ed80f53806e4550d482e7

Selected related CTest 8/8 PASS. On this frozen binary,
native CallFrame 25/25 and GC-rooted scheduler handoff 5/5
PASS; GUI Scratch parity off, hotspot and eager each 2123/2123
PASS. All changes remain uncommitted. Default feature OFF.

Limits: a native Fiber running without yielding can increase
temporary heap consumption because both automatic and direct
collection requests are delayed. It cannot prove arbitrary
LLVM SSA GC/pointer ownership or general effect-PC deopt.

### Final full regression results and allocation metric caveat (21:08 JST)

The full native real source-resume/fail-closed suite on the exact frozen
binary above completed **915/915 PASS** (prior 908 +7).
The four authentic pre-root direct-GC injections with N=1,2,5,17
all observed unchanged collector count and pending=1, followed
by correctly rooted generated Unicode SStr and full post-Fiber
GC; the native-vs-Interpreter final state matched. The negative
case with a forged generated SSA word rejected deopt, kept the
original native Fiber running, and drained its one pending direct
GC only after native script completion. The positive handoff
without test-only post-GC drained one explicit request at the
authenticated Interpreter-owned safe point. The feature-disabled
injection was inert.

Selected RT/IR/ABI CTest **8/8 PASS**, native CallFrame
integration **25/25**, scheduler-owned rooted GC handoff
**5/5**, GUI Scratch parity off/hotspot/eager **2123/2123
each**. Python py_compile and git diff --check PASS.
No commit or push.

The optional allocated_bytes native-activation counter uses
Gc::bytesAllocated() before/after resume, and is a diagnostic
accounting delta only. In the sampled N=2 join fixture it was
0 for both observed activations: this does NOT establish zero
physical allocation, because the GC small-object allocator
batches thread-local pending bytes and not every yield may
create new heap objects. No reliable hard peak-memory bound
has been established by these tests. A running native Fiber
without any yield can still accumulate unbounded deferred
allocations; a real allocation budget with a safe yield path
remains an open task.

## RT-001: soft GC memory budget via verified LLVM source checkpoint (2026-11-09 ~22:00 JST)

The preceding opt-in generated-string producer feature deferred
automatic and explicit Gc::collect while an LLVM Fiber held
not-yet-rooted SSA strings. That preserved safety, but a long
activation could consume memory until a normal yield/finish.

New opt-in XYO_RT_GC_SOFT_BUDGET_MB=<integer 1..1024>, usable ONLY
alongside XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1:
- The native RT measures the current GC-managed heap lower-bound
  with Gc::bytesAllocated() at loop yield polls. Invalid or zero
  settings are ignored; the flag does not change default behavior.
- For a normal loop, a real AfterLoopBody checkpoint whose immutable
  lexical source record and generated-SSA diagnostic roots already
  exist may publish a one-shot per-Scratch-Thread budget request.
- For warp loops, a positive budget poll forces the EXISTING
  variable spill + lexical checkpoint + normal Fiber yield path.
  The shared-clock fast path gets the same opt-in budget poll.
  This is *not* a mid-instruction/opaque-PC suspension.
- The serial Sequencer consumes the request. It attempts the
  SAME strict source-certified native->Interpreter deopt path used
  by RT-001; the source revision/digest, live pointer provenance,
  GC-owner certificates and source-plan reconstruction still gate
  adoption. XYO_RT_NATIVE_DEOPT_ONCE need NOT be enabled to use
  the soft budget. One request per Thread limits repeated failed
  deopt attempts.
- On a proven handoff, the native Fiber and non-executable SSA
  snapshot are destroyed FIRST, then Gc::collect reclaims heap.
  This is reported as
  [native-gc-soft-budget] requested=1 adopted=1 collected=1
  bytes_before=<B> bytes_after=<B>.
- On an unproven or corrupted source plan, NO GC is attempted
  over the suspended runnable native Fiber; the script continues
  natively. Only after it FINISHES and the Fiber has been retired
  does the collector run (logged by
  [native-gc-soft-budget-finished] collected=1).
  This is fail-closed and may exceed the budget.

New generated Scratch fixtures:
ssa_budget_join_string and ssa_budget_join_warp. They run
24,000 genuine native Unicode string joins and a changing
IF/ELSE PHI; the GC pool's cons-cell allocations accumulate
more than 1 MiB over the run. The allocation accounting includes
batched 4-KiB TLS pending cells only once flushed; it is a
lower bound and NOT process RSS or a hard allocation limit.

Verified on the experimental isolated executable:
 /home/kicky/.rt001-gc-soft-budget-verified-isolated/xyo
 SHA256 0a2de9753731fe00050a617bec3485380805a6dcad510f8a50ec070802cb444d

Real Scratch nonwarp:
XYO_RT_GC_SOFT_BUDGET_MB=1 detected heap bytes 1,049,370,
automatically adopted verified source continuation and completed
collection with 31,222 bytes recorded afterward. Final Scratch
variables exactly match independent Interpreter execution.
The 2-MiB and 4-MiB controls remain below budget, perform NO
automatic handoff, and preserve parity. Disabled owner feature
also prevents budget-triggered deopt.

Forged native generated-string SSA word with locally repaired
integrity hash: adoption refused, collected=0 while the Fiber
remained live, Interpreter parity after native completion;
ONLY after native Fiber retirement was GC executed, with
1,755,262 -> 31,294 GC-accounted bytes.
Warp source: configured memory poll forced a native checkpoint,
but the current warp lexical recovery was NOT source-certified
for this case. RT refused deopt, collected=0 while Fiber live,
native completion preserved Interpreter parity; GC afterward
was 1,755,258 -> 31,294. Do NOT claim warp recovery is supported.

5 new differential regressions added to
tests/differential/native_simple_resume.py: proven nonwarp
budget takeover; below-2MiB and disabled-feature quiet controls;
corrupt SSA fail-closed; warp fail-closed. The prior full suite
contained 915 cases; final suite is expected 920 if all pass.

Build integration issue: during the RT code/test implementation,
another agent's code introduced src/jit/codegen.cpp include
../analysis/numeric_region_lower.hpp which itself referenced a
temporarily missing ../ir/numeric_region.hpp. The existing SDL
full rebuild was therefore blocked by concurrent NumericRegion
work, NOT an RT error. To verify this precise milestone without
editing NumericRegion source, RT linked the freshly compiled
backend_llvm.cpp.o (registering xyo_rt_gc_soft_budget_due) and
RT sequencer.cpp.o into the last consistent already-built
libxyo.a archive, then separately linked the CLI. Its parent
build had compiled the new RT Thread, JIT runtime and codegen
objects together immediately before the unrelated missing
header was introduced. This is an isolated, mixed-epoch link
with explicitly verified object provenance; it is NOT a claim
of fully fresh source tree integration success.

This soft budget cannot guarantee a maximum heap size: a single
huge allocation, a long unyielding native activation, missing
safe source checkpoints or a source-verification failure may
exceed it. No arbitrary LLVM SSA/pointer deopt or effect-PC
replay. Feature default OFF. RT-001 remains in_progress.
No commit/push.

### Final RT-001 soft-GC-budget verification (~22:09 JST)

The full native differential suite on the isolated linked CLI
completed **920/920 PASS** (previous 915 +5). The first run of the
new tests gave a TEST-ONLY false failure: both the legacy
[native-simple-deopt] event and new [native-gc-soft-budget] event
legitimately contain adopted=1, so an assertion counting all
adopted=1 substrings as one was wrong. Corrected it to count the
single [native-simple-deopt] event. No runtime parity failure
occurred; the test rerun fully completed.

The exact same isolated CLI also passed actual GUI Scratch parity:
off **2123/2123**, hotspot **2123/2123**, eager **2123/2123**.
Native CallFrame integration **25/25**, scheduler-owned rooted
source handoff **5/5**, eight selected RT/IR/ABI CTests **8/8**,
py_compile and git diff --check PASS.

One-megabyte budget: real Scratch nonwarp loop with 24,000 dynamic
Unicode string joins generated a live native string PHI and
triggered an automatic source-authenticated deopt at
GC-counted bytes 1,049,370. A full GC AFTER Fiber retirement
reduced measured heap to 31,222 bytes; Interpreter final state
matched. With a 2-MiB budget, no memory-pressure deopt occurred.
With the source ownership feature OFF, no budget request occurred.

Independent fail-closed cases: forged live SSA Value with recomputed
FNV refused deopt, native Fiber continued, and after FINISH
GC reclaimed 1,755,262 -> 31,294 bytes. The warp variant
also requested a regular yield on the memory budget (including
shared-clock polling), but its current lexical interpreter
resume plan FAILED source verification. No early GC, native
completion matched Interpreter, and after-Fiber GC reclaimed
1,755,258 -> 31,294 bytes. Successful warp recovery is explicitly
NOT claimed.

Source integration remains incomplete because another agent's
numeric region lowering introduced a nested include from
src/analysis/numeric_region_lower.hpp to
src/ir/numeric_region.hpp which did not exist at the time.
RT did not modify that other task. Tests therefore verify the
isolated binary formed from the previously consistent Release
LLVM JIT SDL build (which included RT's source budget codegen
and runtime) and the freshly rebuilt RT Sequencer plus LLVM
backend symbol registration; this does NOT represent a
fully successful build of the latest changing shared working
tree. The verified executable and SHA256 appear above.

The soft budget measures GC-allocated managed payload state,
not RSS, and pooled allocations are published in batches.
It is a lower-bound pressure signal; a single large allocation
or long native activation can exceed the chosen budget. If
no authenticated source checkpoint is available, exact GC
ownership and effect replay are NOT guessed.

### Full-current-tree integration recheck (~22:14 JST)

The unrelated parallel NumericRegion owner subsequently created
src/ir/numeric_region.hpp, unblocking the full SDL/LLVM23 project
build. This supersedes the preceding report that a current-tree
build was unavailable.

cmake --build /home/kicky/.rt001-phi-sdl-build-20261109
--target xyo -j 6: **PASS**, rebuilding the new numeric region
lowering and current RT source integration together. The CMake
configuration still retains the previously documented build-local
forced inclusion of the existing DLSS header; there were no RT
edits to renderer or NumericRegion source.

Frozen independently from this successful full-source link:
 /home/kicky/.rt001-gc-soft-budget-fullsource-frozen-20261109-xyo
 SHA256
 e236dffda299a65792b7c4f0d30ea5cd3f3b3fe73ddf05a6e735a3f1857cfaf8

On that EXACT fully rebuilt CLI:
- native source-authenticated real resume/fail-closed **920/920 PASS**
  (including all five new soft-budget cases);
- GUI Scratch parity Interpreter/off **2123/2123**,
  hotspot **2123/2123**, eager **2123/2123**;
- Native CallFrame integration **25/25**;
- rooted native source handoff integration **5/5**;
- selected RT/IR/ABI/cache/root-stack CTest **8/8**.

The isolated binary's earlier 920/920 and GUI results remain
valid as independent diagnostics, but the full source rebuild
and tests above are the authoritative latest integration result.
Default feature OFF, memory limit still SOFT and not a hard
RSS/heap cap. No commit or push.

## RT-001: source-certified warp procedure soft-budget recovery (~22:32 JST)

This supersedes the earlier report in this document that a warp
procedure MUST refuse memory-pressure native->Interpreter deopt.
That previous refusal was traced to a blanket
`prototype->warp()` veto in
`NativeSourceHandoff::simpleInterpreterResume`, not to a failed
SSA owner, source digest, CallFrame or GC-root certificate.

Now, ONLY for a genuinely pending one-shot per-Thread
GC soft-budget request with
`XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1` and an authentic
`AfterLoopBody` source checkpoint, a source-verified warp
procedure may be included in the executable interpreter plan.
The permission checks BOTH
`thread.nativeGcSoftBudgetYieldDue` and
`thread.nativeGcSoftBudgetAttempted`, not merely a process-wide
environment variable. A later test-only Nth-yield deopt or a stale
budget flag cannot silently enable warp restoration. The existing
strict source topology, complete committed-loop-body phase,
typed/SSA manifest, pointer origins, exact generated-string root
handles, live argument ownership and source/revision/digest checks
are unchanged. The older root-only post-callee warp veto is also
unchanged.

`SimpleNativeInterpreterResume::Procedure` now retains the
exact `prototype->warp()` setting from the validated source
graph as `declaredWarp`. When rebuilding interpreter CallFrames,
`call->warpMode = call->warpMode || restored.declaredWarp`.
This preserves both a warp-declared procedure and its inherited
warp flag in nested ordinary-declared procedures; loop/IF
control frames continue to inherit from their parent's existing
`Thread::pushStack` semantics. The restored frames are not
inferred from a native program counter.

Real Scratch verified with the full SDL+LLVM23 current-source
release CLI and independently generated 24,000-join UTF-16
branching fixtures:

* `ssa_budget_join_warp`: source-certified warp procedure
  automatically adopted and a full GC executed **AFTER** Fiber
  retirement, with GC managed allocations **1,049,370 bytes ->
  31,222 bytes**, and **2** restored warp-mode CallFrames.
  Final Scratch state exactly equals independent Interpreter run.
* `ssa_budget_join_warp_nested`: warp outer procedure calls
  declared-nonwarp inner procedure which owns the live 24k join
  loop. The original warp flag is inherited through both calls.
  Automatic source-verified adoption succeeds, GC lowers managed
  memory **1,049,396 -> 31,828 bytes**, and **3** resumed
  warp-mode CallFrames are retained. Interpreter state matches.
* Changing one live generated string SSA word with a recomputed
  hash in EACH warp fixture refuses handoff:
  `requested=1 adopted=0 collected=0` while the native Fiber
  remains runnable. The Fiber finishes natively; then a complete
  GC runs (the nested case **1,759,396 -> 31,900 bytes**), with
  interpreter final-state parity.
* Raising the soft budget to 2 MiB, with all other experiment
  flags unchanged, causes **no speculative handoff** in either
  warp fixture; both keep independent Interpreter parity.

The former one warp-refusal differential test has been replaced
by the genuine warp-success test, and new nested-warp positive,
two forged-SSA negatives, and two no-pressure controls are
included. Full `tests/differential/native_simple_resume.py`
now **925/925 PASS** (previous 920 +5).

The exact FULL current-tree CMake/LLVM JIT/SDL build succeeded
before these tests, with one existing build-local DLSS header
inclusion workaround. The matching executable frozen for the
full differential, GUI and CallFrame runs is:

`/home/kicky/.rt001-gc-warp-handoff-fullsource-frozen-20261109-xyo`
SHA256:
`d679da135b4a655d1395c8febc31bea64155dd0a305c91792b973cdde28931a2`.

On that exact frozen CLI:
* Native source resume/fail-closed **925/925 PASS**.
* GUI Scratch parity off **2123/2123**, hotspot
  **2123/2123**, eager **2123/2123**.
* Native CallFrame **25/25**, scheduler rooted source
  handoff **5/5**, eight selected RT/IR/ABI CTests **8/8**.
* Python py_compile and git diff --check PASS.

Remaining limits: the budget remains a GC-managed allocation
lower-bound SOFT threshold, not RSS or a hard limit; the source
handoff has only committed-loop-body scope and valid source/SSA
proofs, not arbitrary LLVM SSA/effect-PC state; warp procedures
with unsupported lexical or pointer structure still fail
closed without early collection. Experimental feature stays
OFF by default, no commit/push, RT-001 still in_progress.
