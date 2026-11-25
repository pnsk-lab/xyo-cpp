# Event-Driven Scheduler: pure wait-until coalescing (2026-11-05)

## Scope

This is a scheduler optimization for `control_wait_until`. It is **not** a general per-variable subscription/event bus. It intentionally avoids introducing write barriers in interpreter/JIT/GPU/UI paths.

Opt-in:
```sh
XYO_EVENT_SCHED=1 build/xyo run file.sb3 --jit eager
```
Disabled by default. Diagnostics: `XYO_EVENT_SCHED_STATS=1` prints skipped/woken passes.

### Mechanism

- At block-cache/JIT compilation, inspect the `CONDITION` AST. Only a strict whitelist of side-effect-free Scratch reporters (variables, literal reporters, arithmetic, comparisons, boolean operations, string operations) can be classified pure.
- Exclude time, keyboard, mouse, sensors, randomness, procedures, lists, extensions and unrecognized reporters.
- A false pure `wait until` registers a wait site and the current sequencer progress epoch. Each successfully advanced Scratch script/batch or start of a new tick invalidates the epoch.
- While no other script has advanced in the **same tick**, the scheduler skips re-running that waiter. Upon progress or at the **next tick**, it re-evaluates through the original Scratch evaluation path.
- A wait that was exited and re-entered at the same site in a warp loop increments a separate exit counter. Site equality alone is insufficient evidence of no progress.
- Parallel-wait resumptions use the serial scheduling path to preserve order with concurrent writers.
- Stopped/restarted scripts have independent Thread objects/state. The hint is never dereferenced as a pointer, and compiled AOT site IDs are treated as opaque identities.

This design can over-wake: executing an unrelated script increments the epoch. That is a safe and intentional tradeoff for correctness and avoiding pervasive mutation tracking.

## Tests

```sh
python3 tests/differential/event_scheduler.py build/xyo
ctest --test-dir build --output-on-failure
```

Synthetic fixtures:
- No-writer pure wait (must remain waiting)
- Writer script satisfies condition
- Two waiters on the same variable
- Warp procedure exits then re-enters the same wait site
- `sensing_timer` (not eligible for skipping)
- `operator_random` (not eligible for skipping)

Differential tests use interpreter, eager JIT, HotSpot, and parallel OFF/ON for each project. `--frames=300` compares the final Scratch variable values between feature OFF and ON.

Validated: 36/36 synthetic differential comparisons, 13/13 other CTest cases.
Native AOT compilation and execution of writer/waiter succeeded (done=1, progress=80).
The tests include a warp procedure that re-enters the same wait site after progress.

## Performance (after reentry counter)

On a CPU-pinned 60-frame test containing only `wait until ready > 0` with no writer (4 runs per mode; process startup and JIT compilation included):

| Runtime | OFF (median) | ON (median) | speedup |
|:--|--:|--:|--:|
| Interpreter | 1510.98 ms | 11.10 ms | 136.2x |
| Eager JIT | 1515.55 ms | 15.31 ms | 99.0x |
| HotSpot | 1511.28 ms | 10.75 ms | 140.6x |

**These are idle waiting CPU/busy-loop elimination measurements**, not a general Scratch throughput speedup. The reported `--frames` count remains unchanged. Waiting scripts still check at least once per tick; the timing measures do not establish end-to-end latency for interactive input.

No-wait and mixed-script workload A/B (10 random-order pairs per configuration, CPU-pinned):

| Workload | Runtime | OFF | ON | OFF/ON |
|:--|:--|--:|--:|--:|
| numeric loop | interpreter | 18.64ms | 18.77ms | 0.993 |
| numeric loop | eager JIT | 28.79ms | 29.16ms | 0.987 |
| string loop | interpreter | 18.37ms | 18.43ms | 0.997 |
| string loop | eager JIT | 28.92ms | 28.83ms | 1.003 |
| wait + writer | interpreter | 11.20ms | 11.30ms | 0.991 |
| wait + writer | eager JIT | 23.19ms | 23.56ms | 0.984 |

For such tiny programs the noise and process/JIT startup dominate; no consistent speed gain was established.

## Next steps

- Re-run a more representative corpus through ScratchBench; compare CPU utilization and scheduler steps, not only wall time.
- A true per-variable/field event dispatcher needs consistent write barriers or conservative script-level write sets, plus guards for callback/extension/IO/clone mutations.
- Keep feature opt-in until broad Scratch parity and interactive timing are verified.

Changes have not been committed/pushed.
