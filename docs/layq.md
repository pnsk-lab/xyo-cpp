# LAYQ: Load-Adaptive Yield Quantum (warp-loop deadline polling)

`src/jit/layq.hpp`, gated by `XYO_YIELD_QMODE=off|adaptive|fixed`
(default `adaptive`; set `XYO_YIELD_QMODE=off` to get the pre-LAYQ cadence).
Status: default-on. This note records the design, the safety case, the IR
evidence, and the measurements behind the default recommendation.

## What it is

Warp loops poll their deadline every 64 iterations (`(++n & 63) == 0`).
The poll exists to hand off to other threads and to notice warp-budget /
stop / GC pressure. When no other thread is runnable the check is pure
overhead, so LAYQ widens the poll quantum when the machine is idle and
snaps it back under contention (v2 push model — see below):

- per-thread quantum, always a power of two (keeps the mask form
  `q - (n & (q-1))`), re-synced from the ctx field at loop entry and
  updated at every poll — never computed per iteration;
- target `qmax >> min(competitors, shift)`: halve per competing thread,
  floored at `Q_MIN` (default 64 — never worse than status quo under load);
- body-class caps: call-free numeric fast regions `Q_MAX_PURE=16384`,
  loops that may call/allocate `Q_MAX_CALL=256`;
- stop-epoch handoff: `Runtime::stopEpoch` (bumped by `stopThread`, which
  covers `stopAll`) is compared at every poll; a mismatch hands off,
  bounding stop latency at one quantum regardless of deadline cadence;
- fixed mode pins a constant quantum (`XYO_YIELD_QFIXED`, clamped by the
  body cap) for A/B work — and because it is a compile-time constant, it
  folds through the whole loop (zero extra live values);
- `XYO_YIELD_QSTATS=1` dumps per-thread polls/taken/avg-quantum at release.

With the flag off every new emission folds to constants and the generated
IR is instruction-identical to the pre-LAYQ form (verified by IR diff on
loops/vars/math/strings/lists/scheduler kernels — 0 non-ASLR difflines).

## Load signal and its contract (v2 push model)

`runSchedulerPass` already computes `numActiveThreads` (threads still
runnable after their slice). In adaptive mode it publishes the recommended
pure-class quantum to `Runtime::yieldQuantumHint` — and only when the value
changes, so steady state performs zero stores and JIT readers keep an
L1-shared line. JIT poll/entry blocks read it with one relaxed load and
clamp to the loop's body-class cap; no competitor math, no shifts, no extra
atomics, no danger branch in generated code (danger pins the published
value scheduler-side, which is sound because danger mode is fixed before
execution starts). Contract (see `src/jit/layq.hpp`):

- single writer (scheduler thread), relaxed readers, heuristic input only;
- staleness is bounded by one pass + one quantum and only ever costs
  efficiency, never correctness — every poll still runs the full
  deadline/stop/GC check suite, and the warp time budget backstops
  fairness independently of the quantum.
- Caveat: a very long warp loop extends the scheduler pass it runs in
  (the pass cannot end until the thread yields), so a spawn burst that
  lands mid-loop is not published until the next handoff. This mirrors
  the scheduler's own visibility (it cannot dispatch the new threads
  either until then), so no responsiveness is lost relative to baseline;
  the next poll after the publish snaps to the new target.
- Measured contention response: two concurrent 200M-iteration sin loops
  (guaranteed multi-tick overlap, ~25 s run) show one thread at avgQ=9028
  (shrunk from 16384 while both were active) and the other at 15985, with
  24/49 real handoffs and byte-identical checksums vs off. The mechanism
  tracks contention mid-run; solo loops sit at the max (avgQ=16384).

End-of-pass active count (not a pass-entry snapshot) was chosen deliberately:
it measures "threads that want more CPU", which is exactly the contention
signal a quantum policy needs. Timed-wait sleepers are excluded by the pass
itself (they count 0), so a project full of `wait 10 secs` threads correctly
reads as idle. Initial hint is the pure-class max (solo is the common case);
a loaded start corrects it at the first pass, and per-loop body-class clamps
bound every first chunk regardless.

## Safety case (what widening preserves)

- **Stop/kill**: cooperative today (observed at the next yield return).
  LAYQ *adds* a per-poll stop-epoch check on top of the existing deadline
  check, so stop latency is bounded by one quantum *regardless* of whether
  the deadline is due — strictly more responsive than baseline, which only
  observes stops on taken handoffs. Measured: stop of a 5.6 s warp loop at
  t=0.5 s terminates at 0.55 s (median) in all modes, max 0.574 s over 20
  runs (vs 0.551 s off — within run-to-run jitter).
- **GC roots**: widening is quantum-independent. Collection is always
  allocation-triggered on the same thread, and the running fiber's machine
  stack (with ABI-spilled SSA) is conservatively scanned by
  `markThread`/`collect` regardless of poll cadence. Pure loops cannot
  trigger GC mid-quantum at all (no calls ⇒ no allocation). Verified by a
  rope-churn + flatten fixture: identical checksums across modes, no crash.
- **Warp budget**: overshoot is bounded by one quantum. Caps keep the worst
  realistic gap at milliseconds against the 500 ms budget (pure loops:
  call-free ⇒ per-iter cost ~ns; may-alloc loops: 256 iters). Iteration
  counts are never treated as wall-clock bounds — the caps are chosen so the
  bound holds for realistic per-iteration costs, and pathological loops
  (ms+/iter) are measured, not assumed (see risks).
- **Broadcast-and-wait / waits / danger**: wait paths (`waitShouldYield`,
  `wait_until`, compat, glide) are not on the warp counter and are untouched.
  Danger mode pins the quantum to the body cap (polls can never fire for
  budget reasons there). `broadcast and wait` internals unchanged.
- **Nested warp / OSR / deopt / live edit**: checkpoints still publish at
  every poll (same content, different cadence). Nested-warp silent-edit and
  in-flight-deopt harnesses pass under adaptive. Granularity coarsens under
  wide quanta; cadence-sensitive tests keep passing because each handoff is
  still fully certified.
- **Parallel workers**: isolation is by batch join, not by yield; widening
  only delays a batch join, bounded by the caps.
- **Non-warp loops**: untouched (every-iteration yield is Scratch step
  semantics, out of scope by design). Scheduler scores are therefore
  expected to be identical — confirmed (`sched_looptick`/`sched_fanwait`
  within noise; `sched_threads` +1.6%).
- **Contention response (v2 push)**: two concurrent 200M-iteration sin loops
  (~25 s run, guaranteed multi-tick overlap) show one thread at avgQ=9028
  (snapped down from 16384 while both were active) and the other at
  avgQ=15985, with 24/49 real handoffs and byte-identical checksums vs off.
  The published hint tracks the scheduler's own active-thread count, so the
  response needs no extra synchronization — and a stale hint only ever
  costs efficiency (see contract above).

Proving rule used throughout: pure-class widening (16384) applies only to
fast regions that are call-free by plan construction (numeric SSA chunks,
warp induction, foreach-sum — verified call-free in IR dumps); everything
else keeps the may-alloc cap (256). Slow/single-step arms keep today's
cadence via the shared scope gate. If a fast loop ever grew a call, the
worst case would be overshoot bounded by the cap, not unsoundness (GC
argument above holds regardless).

Separating the poll into independent deadline-only vs full checkpoints was
considered and declined: the deadline check itself is already minimal
(sampled atomics + branch), the stop check must stay per-poll anyway for
the stop bound, and OSR/deopt checkpoints + spills are handoff-gated
already (they cost nothing when no handoff occurs). Splitting would save
~2 ops per poll at the price of a second cadence to reason about, with no
measured need — the per-poll cost problem was the adaptive *computation*,
which v2 moved out of generated code entirely.

## IR evidence (`XYO_JIT_DUMP2`, isolated scratch-bench kernels)

- Entry: re-sync from the ctx field clamped to the body cap (2 loads +
  select, no R read) — tiny loops that never poll behave exactly as before.
- Hot loops stay pure SSA: zero `@xyo_*` calls, zero atomics, zero spills in
  the fast cycle (verified for loops_repeat / math_arith / list_read fast
  loops); the gate keeps today's shape (`and` + compare). A dynamic quantum
  needs 1–2 loop-carried live values (q + mask) where off/fixed use
  immediates — measured cost on register-pressured validation loops below.
- JIT compile time is unaffected (algo_sort full eager compile: 168 ms
  off vs 164 ms adaptive — noise; the added IR is a few dozen instructions
  per loop, all in cold poll/entry blocks).
- Poll block per quantum (v2): hint relaxed-load, clamp select, stop-epoch
  load/compare, resync store in the handoff (+3 counter stores only with
  `XYO_YIELD_QSTATS=1`). No competitor math, no shifts, no danger branch.
- Counted-repeat bulk optimisation intact (chunk = min(remaining, toPoll)).
- Fixed mode folds everything to constants: no quantum/mask allocas, gate
  is `test reg, imm`, chunk math is constant — zero extra live values.

## Measurements (i7-12700, taskset-pinned, eager, alternating A/B, n=5;
final binary, explicit env both sides; compute checksums identical)

| kernel | off ops/ms | adaptive (v2 push) | speedup |
|---|---|---|---|
| loops_repeat | 12,399,070 | 666,948,540 | **53.79x** |
| vars_setchange | 10,408,859 | 35,550,517 | **3.42x** |
| algo_sort | 317,046 | 420,586 | **1.33x** |
| list_read | 498,378 | 531,838 | 1.07x |
| math_arith | 231,435 | 244,328 | 1.06x |
| list_append | 525,381 | 554,007 | 1.05x |
| math_strcompare | 34,793 | 36,352 | 1.04x |
| algo_sieve | 166,667 | 169,487 | 1.02x |
| str_join | 3,495,091 | 3,544,173 | 1.01x |
| app_brainfuck | 106,380 | 106,581 | 1.00x |
| app_raycast | 25,997 | 26,638 | 1.02x |
| str_cast | 20,314 | 19,893 | 0.98x |
| sched_looptick | 7,236 | 7,012 | 0.97x |
| sched_threads | 29,458 | 29,495 | 1.00x |
| sched_fanwait | 1,089 | 1,088 | 1.00x |
| sched_clonechurn | 723 | 720 | 1.00x |

(sched_* checksums vary run to run by design — they count throughput, not
output. Earlier campaigns: fixed256/fixed16384 match adaptive on pure
loops; fixed16384 adds ~1.2x on sieve; pin64 (machinery at q=64) costs
~15% on validation-heavy loops — see below. The append row varies ±10%
across campaigns: layout noise, not a fixed effect.)

Key reads:

- **Fixed quanta win; v2 adaptive matches fixed on pure loops.**
  `fixed256` beats off almost everywhere it matters (sort/sieve 1.2–1.3x,
  vars 3.7x, loops 3.8x) because wider chunks amortize the per-chunk
  validation ladders. v2 `adaptive` matches the best fixed policy per loop
  class (loops 53–57x, vars 3.2–3.5x) and trails it only where the poll
  block itself matters — see next point.
- **Per-poll adaptation work cost ~15% on validation-heavy loops (v1).**
  `fixed64` (same LAYQ shape, no R-read/target math) ≡ off (0.98–0.99x on
  sieve/sort), while v1 `pin64`/`adaptive` (identical shape *plus* the
  per-poll R atomic load + target select + danger select) = 0.83–0.87x.
  ~6 extra ops per poll cannot cost 15% by op count; QSTATS-counter
  perturbation of the poll block moves results <3%, ruling out generic
  poll-block size effects. The v2 push model removes the R read + target
  math + danger branch from generated code (scheduler precomputes, publishes
  on change; JIT does one relaxed load + clamp), leaving a poll block close
  to fixed in size. v2 adaptive recovers to ~1.0x on sieve (vs v1 0.87x);
  the residual gap to fixed (1.21x) is the remaining poll-block delta
  (hint load + min + resync store vs const-folded fixed) plus code-shape
  effects — see below.
- **Fixed mode const-folds the whole quantum away.** In `fixed` mode the
  quantum is a C++ immediate everywhere (gate mask, chunk math, no
  quantum/mask allocas), so fixed loops carry zero extra live values and
  the gate stays `test reg, imm8`. This is why fixed matches or beats off
  even before widening helps: it removes nothing, but adds nothing either.
  It also explains the v1 pin64 deficit mechanically: the dynamic quantum
  needs 1–2 loop-carried live values (q + mask), and on register-pressured
  validation loops that spills (observed: per-iteration stack reload of the
  mask instead of `test reg, imm64`).
- **Ramp is pointless**: gradual 64→16384 wastes ~16k iterations converging
  and matches off on short loops; entry now re-syncs from the ctx field
  (2 loads, idempotent — never a reset) and the first poll snaps straight
  to target. §4 answered: immediate wins, gradual adds nothing.
  (`XYO_YIELD_QINIT` was removed with the ramp.) Retargeting a scope to a
  new body cap also clamps the live quantum, so a may-alloc loop nested
  inside a pure loop respects its 256 bound from the first chunk.
- **append: layout noise, not a fixed effect.** Earlier campaigns showed
  −13% for q≥256 on one store-heavy loop (hot loop verified
  instruction-identical modulo the yield-gate idiom, cliff exactly at the
  imm8→imm32 boundary q=128→256). On the final binary the same kernel
  measures 1.05x — the sign flips with layout. No mechanism (call counts,
  memory ops, chunk cadence) distinguishes the configs; treat append as
  ±10% layout-sensitive, not as a policy signal. perf stat for a definitive
  attribution is future work; a capacity-check hoist is the principled fix
  if it ever matters.

## Three bugs found and fixed (all verified in IR + benchmarks/tests)

1. **Entry init placed inside the chunk dispatch** (it runs per chunk, not
   once): storing the entry value there clobbered the adapted quantum every
   chunk. Fixed by making entry a *re-sync* (reload from ctx, mirroring the
   yieldCheckN discipline) instead of a store. This also removed the
   per-entry R read that cost ~6% on tiny-trip nested loops (sieve).
2. **Stale .inc objects**: `numeric_region_ssa.inc` et al. converted to
   `yieldToPoll`, but a build did not recompile `codegen.cpp.o` (Makefile
   dependency gap on `.inc` files — worth a build-system follow-up), so one
   benchmark round measured off-equivalent adaptive. Forced rebuild fixed it.
   Lesson: always re-verify `and 63` is gone from the hot loop dump after
   touching `.inc` files.
3. **New pointer-typed JitCtx fields vs the RT-001 pointer-provenance gate**:
   `quantumHintPtr`/`stopEpochPtr` loaded as pointers created new
   "interior_or_unmapped" load chains, failing
   `xyo-native-simple-deopt-parity` (whose allowlist is exactly
   {varSlotData, sampledWarpClockUs}). Fixed without touching the gate:
   generated code now loads those slots as `i64` + `inttoptr`. The slots
   address Runtime-embedded integer atomics (never GC memory, integer
   payloads only), so GC-safety is structural; the census output is
   byte-identical to baseline. Fixed mode already const-folds everything,
   so it never had this exposure.

## Risks and limitations

- Pathological per-iteration costs (ms+/iter string/list behemoths) widen
  the worst-case gap linearly with the cap. Bounded (256 / 16384 iters).
  Measured with a pathological fixture (2 MB string concat + compare per
  iter, ~1–4 ms/iter, plus a plain stop at t=0.5 s and t=4.0 s): all modes
  terminate within ~0.1 s / ~0.7 s of the stop (off 0.55/4.48 s,
  adaptive 0.55/4.49 s, fixed256 0.55/4.43 s medians) — the stop bound
  holds because the stop-epoch check runs per poll regardless of quantum.
- The append −13% is a verified layout-class effect, not an algorithmic
  one: the hot loop is instruction-identical across q=64/256 except the
  yield-gate idiom (`lea+test al,imm8` vs `mov+add al,bpl`, coinciding
  exactly with the imm8→imm32 boundary at q=128→256), with equal uop
  counts and identical loop alignment — yet uniformly ~13% slower. perf is
  unavailable here, so uop-cache/set-aliasing remains a hypothesis; the
  disassembly comparison and the q-sweep cliff are recorded in
  `board/prof-ir/asm/` (`ap64` vs `ap256` full dumps + normalized hot-loop
  diff). Mitigation that keeps semantics: hoist the per-append capacity
  check out of chunk-sized provable regions (also a real win), or accept
  as alignment noise (single kernel; geomean still favors widening).
- AOT bakes the mode active at AOT-compile time (documented behavior).
- WASM backend untouched (separate lowering).
- The `pin64` deficit (dynamic machinery at q=64 ≈ 0.83–0.87x on
  validation-heavy loops) is the reason fixed mode const-folds instead of
  loading: any dynamic quantum needs loop-carried live values. v2 narrowed
  the gap (adaptive ≈ fixed16384 on pure loops); the remainder is
  code-shape, not policy.

## Recommendation

**Adaptive is the default (`XYO_YIELD_QMODE` unset ⇒ `adaptive`).** The
mechanism's wins are large and robust where poll overhead dominates (loops
53–57x, vars 3–3.7x, sort 1.3x, sieve ~1.2x via fixed16384), and correctness
is fully preserved (checksums identical everywhere, including 19/19
differential repros across off/adaptive/fixed; stop/GC/broadcast/nested-OSR
verified, including a pathological stop fixture). The two measured caveats
remain and are documented above rather than treated as blockers: (a) v1's
adaptive poll block cost ~15% on validation-heavy loops vs a fixed quantum
(sieve 0.87x vs fixed256 1.23x) — v2's push model (no R-read/target math in
generated code) recovers most of it (sieve ~1.0x vs fixed 1.21x); the
residue is code shape, and `fixed16384` remains available
for exactly those loops; (b) pathological per-iteration costs widen the
worst-case gap linearly with the cap, but the stop bound held in
measurement (≤0.7 s overshoot on ~4 ms/iter bodies). Operators can opt out
per process with `XYO_YIELD_QMODE=off` (the emitted IR then matches the
pre-LAYQ form exactly — verified 0 non-ASLR difflines on the final binary),
or pin a quantum with `XYO_YIELD_QMODE=fixed XYO_YIELD_QFIXED=256` — a safe
intermediate with zero dynamic quantum state (fully const-folded hot loops).
