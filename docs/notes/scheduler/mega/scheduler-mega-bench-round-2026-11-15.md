# Mega Scheduler β — real ScratchBench `bench_round` LLVM lowering

2026-11-15 / opt-in experiment / **OFF by default** / uncommitted

## What changed

The previous source-certified Mega continuation ended its dynamic clock-based
`repeat until` after two ordered snapshots and a single numeric suffix.

Now the post-measurement suffix can instead be an **argumentless,
non-warp, direct `procedures_call bench_round`**, provided its entire
procedure AST matches the original ScratchBench sequence exactly:

```text
set b_ms to (b_t1 - b_t0) * 86400000
if b_round > 0
  change b_total by b_ms
  change b_chunks by b_n
  if (b_n / b_ms) > b_bestrate
    set b_bestrate to (b_n / b_ms)
  end
end
change b_round by 1
```

The implementation is **not a name-based shortcut**. Every Scratch opcode,
variable identity, read/write position, numeric literal and nested control
structure is validated before the continuation is installed. Extra inputs,
side effects, incompatible expressions and unexpected procedure arguments
cause the normal native Fiber path to be selected.

The required experimental flags are:

```sh
XYO_JIT_MODE=eager \
XYO_JIT_MEGA_BATCH_YIELD=1 \
XYO_JIT_MEGA_SINGLE_SENDER=1 \
XYO_JIT_MEGA_UNTIL_BCAST=1 \
XYO_JIT_MEGA_UNTIL_OUTER=1 \
XYO_JIT_MEGA_UNTIL_POST_SET=1 \
XYO_JIT_MEGA_UNTIL_BENCH_ROUND=1 \
XYO_JIT_MEGA_NORMALIZE_YIELD=1
```

## Implementation details

- `src/jit/mega_yield_source.hpp`: `Source::BenchRound` records the
  eight Scratch scalar identities. `proveBenchRound` follows the
  actual procedure definition and verifies its arithmetic, both
  nested IF branches, write order and unconditional final increment.
  All reporter/statement subtrees must be the exact allowed shape.
- `src/jit/jit.cpp`, `src/jit/backend_llvm.cpp`: verify that every
  referenced variable exists before accepting a native source. A
  bench-round source no longer requires the unrelated constant
  numeric-change suffix.
- `src/jit/backend_llvm.cpp`: emit native LLVM blocks for both IFs.
  The `xyo_var_get`, `xyo_sub`, `xyo_mul`, `xyo_div`,
  `xyo_gt`, `xyo_var_set` and `xyo_var_change` runtime helpers
  preserve Scratch numeric and tagged-value semantics, including
  wait-variable notifications. The IF's rate-expression is computed
  a second time during the best-rate assignment, as in the original
  Scratch reporter tree. Values are reloaded after each completed
  broadcast join and during their original expression evaluations.
- No new JitCtx ABI fields, threads or native CallFrame roots were added.
  Existing Thread-owned continuation PCs, GC protection and receiver
  join identities remain unchanged.

## Testing with the **actual original SB3 procedure**

`tests/bench/algos/bench_mega_outer_until.py` accepts
`--bench-round-from /path/to/original/sched-8.sb3 --post-measure`.

Its `graft_real_bench_round` copies the real procedure definition,
prototype and all reachable Scratch AST blocks **without rewriting
their computation**. The sender's earlier measured-repeat loop is
built as a separate minimal source-certified fixture so this isolates
the exact `bench_round` function rather than falsely claiming the
entire original ScratchBench is eligible.

`tests/bench/algos/test_mega_bench_round.py` exercises that fixture
on the exact same CLI binary with Native Fiber and with Mega:

- normal repeated time-bounded broadcasts
- first round only (conditional aggregates must be skipped)
- initially expired clock budget (zero broadcasts)
- outer repeat(0) (no snapshots, no round call)
- positive initial round count (conditional aggregates active)
- modified arithmetic factor (source proof must reject)
- injected extra statement after the procedure's final increment
  (Mega must reject, Fiber must execute it three times)

**69 grounded assertions PASS.** Test logs are retained in
`build-scheduler-rewrite-20261111/mega-bench-round-20261115/source-parity/`
and `source-parity.log`. This is a **fixture parity test**, not
a claim that the full real `sched_pingpong` sender is compiled by Mega.

## Exploratory 16-pair A/B

CPU 16 pinned, same frozen Release binary and same generated .sb3,
18 ms/outer iteration and five outer iterations. Toggle only
`XYO_JIT_MEGA_UNTIL_BENCH_ROUND`; preserve other flags.
Every run checks correct final `b_round`, post-until snapshots,
numeric suffix and nonnegative accounting totals.

| Metric | Normal native Fiber | New Mega |
| --- | ---: | ---: |
| Median receptions/process-ms | 304.517 | 277.195 |
| Pairwise geometric mean score ratio | — | **0.90342×** |
| Mega wins | — | **0/16** |

Because work is **time-bounded and the number of receptions varies**,
this is exploratory and does not establish a fixed-work throughput
speedup. It nevertheless gives no reason to enable the feature by
default.

Raw results:
`build-scheduler-rewrite-20261111/mega-bench-round-20261115/paired-16/results.json`

## Remaining barriers to full actual ScratchBench application

After the outer repeat, the original `sched_pingpong` performs
`b_chk = b_n` and calls the argument-bearing `bench_end`. The
user-visible Stage's green-flag chain also includes `suite_sched`
and further sibling benchmark procedures. The current Mega source
proof rejects these operations rather than silently skipping them.

A safe full-runtime continuation must preserve the full method
call/return sequence and original observable effects; only then can
performance be measured against unchanged ScratchBench.

All changes remain uncommitted. Other agents' work is preserved.

## Final regression and real-program fallback

All compiled against the final hardened AST-proof implementation.

| Configuration | Scheduler | E2E | Exit |
| --- | ---: | ---: | ---: |
| `XYO_JIT_MEGA_UNTIL_BENCH_ROUND=1` | **815/815 PASS** | **99/99 PASS** | 0 |
| `XYO_JIT_MEGA_UNTIL_BENCH_ROUND=0` | **815/815 PASS** | **99/99 PASS** | 0 |

Focused original-SB3 source/effects test: **69/69 PASS**.
Native Fiber and RT001 GC smoke: **3/3 PASS**.
Python benchmark syntax checks, Release rebuild and `git diff --check`: PASS.

The extracted, unchanged `sched_pingpong`, `sched_fanwait`,
`sched_restart` all finish normally with this new flag ON. No unsupported
singleton sender continuation was installed on these original programs.
This is intentionally different from the grafted original-procedure
fixture, which **does** prove and execute the bench_round LLVM path.

Files:
- `build-scheduler-rewrite-20261111/mega-bench-round-20261115/full-1.out`
- `build-scheduler-rewrite-20261111/mega-bench-round-20261115/full-0.out`
- `build-scheduler-rewrite-20261111/mega-bench-round-20261115/source-parity.log`
- `build-scheduler-rewrite-20261111/mega-bench-round-20261115/real-sched_pingpong.err`
- `build-scheduler-rewrite-20261111/mega-bench-round-20261115/real-sched_fanwait.err`
- `build-scheduler-rewrite-20261111/mega-bench-round-20261115/real-sched_restart.err`

Next step to apply Mega to the unchanged `sched_pingpong` source:
source-certify and execute its outer `b_chk = b_n` assignment, the
argument-bearing `bench_end(id, opc)` call, and the broader `suite_sched`
call chain without reordering, dropping or replaying a Scratch effect.
Performance should be retested only after those effects have been
preserved, rather than relaxing the source proof.
