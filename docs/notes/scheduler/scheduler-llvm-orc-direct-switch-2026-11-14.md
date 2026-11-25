# LLVM ORC Fast Yield: pre-resolved native switch (2026-11-14)

## Scope

This experiment specializes the already-existing `xyo_yield_fast` native Fiber switch at
**LLVM ORC symbol registration time**. It removes a branch on each fast Yield but
does not eliminate any Scratch Yield or alter the source continuation PC.

Code:
- `src/engine/fiber.cpp`: `xyo_yield_fast_direct(Fiber*)` performs a direct
  `xyo_fiber_switch(f->regs(), &detail::gSchedulerRegs)`. Under ASan it
  falls back to `Fiber::yield()`.
- `src/jit/backend_llvm.cpp`: the ORC symbol named `xyo_yield_fast`
  resolves to the specialized function **only when all three exact flags**
  match. LLVM IR, machine StackMap helper name, default C symbol and AOT ABI
  remain unchanged.
- `tests/unit/test_scheduler.cpp`: both the original guarded helper and
  specialized helper are exercised with two native Fibers, three Yield
  boundaries per Fiber and a serial `resumeChain` fallback.

## Activation

```sh
XYO_JIT_MODE=eager \
XYO_FIBER_CHAIN=0 \
XYO_FIBER_DIRECT_YIELD=1 \
XYO_JIT_DIRECT_SWITCH=1 \
./build-scheduler-rewrite-20261111/xyo run project.sb3 --fps 0
```

The ORC mapping stays original when any flag is missing, or with ASan.
`XYO_FIBER_CHAIN=0` also forces `Fiber::resumeChain` to use serial resumes
under the existing direct-yield experiment. **Default OFF**, because
disabling Fiber chaining can cost more than branch elimination saves.

## Correctness

- Release: `cmake --build build-scheduler-rewrite-20261111 --target xyo-tests xyo-cli -j4` succeeds.
- `XYO_JIT_MODE=eager XYO_FIBER_CHAIN=0 XYO_FIBER_DIRECT_YIELD=1 XYO_JIT_DIRECT_SWITCH=1 ./build-scheduler-rewrite-20261111/tests/xyo-tests`:
  Scheduler **459/459 checks PASS**, all other groups zero failures.
- Direct-helper native parity: **8/8 checks PASS**, both helper implementations
  preserve the same FIFO trace, completion and FiberPool lease count.
- `git diff --check`: PASS.
- This work is on an existing dirty multi-agent worktree: no reset, clean or
  commit, and only the three files named above plus this report were edited.

## ScratchBench paired measurement

The same frozen Release executable and same source archive were compared
with `XYO_FIBER_CHAIN=0` and `XYO_FIBER_DIRECT_YIELD=1` in both arms,
Eager JIT, CPU16, 30 FPS, 8 alternating OFF/ON pairs.
Only `XYO_JIT_DIRECT_SWITCH` changes.

| Case | OFF median | ON median | Paired geometric mean | ON wins |
|---|---:|---:|---:|---:|
| sched_pingpong | 415.709 | 419.162 | 1.0102x | 5/8 |
| sched_clonechurn | 404.225 | 408.201 | 1.0102x | 5/8 |
| sched_threads | 6887.345 | 6966.229 | 1.0064x | 6/8 |
| sched_fanwait | 765.342 | 769.057 | 1.0088x | 6/8 |

Higher is better. Results:
`build-scheduler-rewrite-20261111/fiber-20261113/bench-orc-direct-switch/results.json`.

The approximately 0.6–1% results are **preliminary** and not proof of a
meaningful performance win. Do not enable by default without a stable
improvement on more scheduler workloads and production projects.


### Independent 20-pair pingpong retest

Same Release executable, CPU16, Eager JIT and common Fiber settings,
with only `sched_pingpong` selected:

| Case | OFF median | ON median | Paired geometric mean | ON wins |
|---|---:|---:|---:|---:|
| sched_pingpong (20 pairs) | 430.0545 | 431.1560 | 1.00446x | 11/20 |

Results:
`build-scheduler-rewrite-20261111/fiber-20261113/bench-orc-direct-switch-pingpong/results.json`.

**Decision:** the small improvement is within run-to-run noise; leave
the new ORC switch specialization OFF by default.  The direct-yield
experiment itself also requires disabling a beneficial Fiber chain, and
its net end-to-end value is workload dependent.
