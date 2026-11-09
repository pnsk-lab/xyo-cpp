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
