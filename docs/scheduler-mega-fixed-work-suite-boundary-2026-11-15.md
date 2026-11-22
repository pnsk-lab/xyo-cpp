# Mega Scheduler β — fixed-work sched_pingpong and suite_sched boundary (2026-11-15)

## Scope

This iteration adds an opt-in **two-variable repeat-until comparison**
to the source-certified Mega LLVM path. It allows a repeat-until that
checks live `sc_hits < b_budget` (inside the existing `operator_not`
wrapper). Both variable values are retrieved again after each joined
broadcast, so the receivers' mutations are observed; no number or
borrowed Variable pointer is retained across a yield.

The flag is `XYO_JIT_MEGA_UNTIL_VAR_PAIR=1` (OFF by default).
The two-variable comparison is recognized only if both are direct
non-shadow `data_variable` reporter blocks without extra effects.
The bounded outer-repeat specialization additionally requires the left
operand to be the exact variable reset by its first outer setter, an
unreversed less-than comparison, and the original negation.
These conditions are checked from the loaded Scratch AST, not from
procedure names. Both variables are checked when installing and
constructing the Mega dispatcher.

## Fixed-work fixture

`tests/bench/algos/test_mega_real_pingpong_full.py --fixed-hits N`
retains the original ScratchBench `sched_pingpong`, `bench_begin`,
`bench_round`, `bench_end` procedure definitions and original
`sc_ping` receiver. The ONLY source condition change is to substitute
the original clock-based `operator_lt` left reporter with a live
`sc_hits` reporter; `b_budget` is assigned N.

Since `sc_hits` is reset to zero at the start of every outer repeat,
and the original `sc_ping` receiver increments it once per broadcast,
the test executes **N broadcasts per round, five rounds**.
The original bench_round aggregates positive rounds only, so
`b_chunks = 4*N`; `b_chk = N`, `b_round = 5`.

N = 0, 1, 3, 7, 50, and 200: source-certified Mega installed,
and the Fiber/Mega counter values agree. The output string's
identifier, total chunks, and final checksum agree. Durations and
rates must NOT be checked for exact equality because they read the
real wall clock even under fixed-work conditions.

## Benchmark methodology and results

`tests/bench/algos/bench_mega_fixed_pingpong.py` runs warm-ups then
alternating AB/BA paired subprocess trials on CPU 16 with
`XYO_JIT_MODE=eager`. Each subprocess executes the same fixed-work
SB3. Correct output and Mega installation are asserted during every
run. Timing is end-to-end *process elapsed time* including process
start, SB3 loading, JIT compiling, native scheduling and dumps.
All comparisons use the geometric mean of **paired Fiber/Mega ratios**;
1.0x means parity, below 1.0x means Mega is slower.

| Broadcasts per round | Paired samples | Fiber median | Mega median | Paired speedup |
|---|---:|---:|---:|---:|
| 200 | 8 | 75.373 ms | 78.923 ms | 0.9481x |
| 2,000 | 8 | 100.371 ms | 103.527 ms | 0.9448x |
| 20,000 | 8 | 267.741 ms | 283.386 ms | 0.9415x |

Raw JSON is in
`build-scheduler-rewrite-20261111/mega-fixed-pingpong-{200,2000,20000}-8pairs.json`.
The results **do not demonstrate a speedup**; keeping the experiment
disabled by default is justified. A per-scheduler-slice profiler or
JIT-precompiled benchmark will be needed to attribute overhead.

## Caller continuation and suite_sched safety

The untouched `suite_sched` in the original SB3 is a chain of ten
heterogeneous procedures:

`sched_looptick`, `sched_pingpong`, `sched_restart`,
`sched_clonechurn`, `sched_spawn`, `sched_threads`,
`sched_fanout`, `sched_fanwait`, `sched_waituntil`,
`sched_cleanup`.

The current Mega dispatcher cannot execute this entire chain as one
source-certified continuation. Critically, it does NOT skip the other
calls.

The new `--post-change` test adds a supported constant numeric caller
suffix after the real pingpong procedure. The source-certified Mega
runs it exactly once on return; it matches Fiber.

The new `--post-procedure` test adds a distinct argumentless
procedure that increments a sentinel variable by 17. Source proof
refuses Mega and the native Fiber still executes that procedure exactly
once, preserving observable effects. This is a **safety boundary test**,
not complete suite_sched integration.

## Remaining

1. Add mixed native/Fiber continuation support at arbitrary
   procedure boundaries to optimize `sched_pingpong` *within*
   the original `suite_sched` without dropping later calls.
2. Identify and eliminate the ~5–6% fixed-work end-to-end regression,
   with profiler-backed measurements.
3. Keep all these flags opt-in until full-suite correctness and speed
   are demonstrated.

All changes remain uncommitted and unrelated agents' work was
neither reset nor staged.
