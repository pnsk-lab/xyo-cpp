# Scheduler reverse wait registry: variable binding invalidation (2026-11-11)

## Problem

`Runtime::variableWaiters` keeps pointer identities into `Target::variables`.
A variable may be removed or recreated while a thread holds an
`eventWatchVariable` snapshot. Although
`canSkipEventWaitByDependency` checks binding generations before dereference,
the reverse index could retain obsolete identities after deletion.

## Change

- `Runtime::invalidateWaitVariableBinding(const Variable&)` detaches
  *all* registered dependencies of each waiting thread that referenced
  the variable, invalidates its dependency snapshot, adds the thread
  to the coalesced candidate-wakeup set, and invalidates the scheduler
  ready-index generation.
- `Target::deleteVariable()` invalidates reverse subscriptions **before**
  erasing storage.
- `Target::dispose()` invalidates watchers on its variable storage
  before clone teardown.
- Multi-variable subscriptions are detached as a unit. The registry
  does not dereference deleted Variable pointers and is opt-in
  (`XYO_WAIT_WAKE_REGISTRY=1`).

## Verification

The LLVM JIT enabled xyo-tests suite passed with
`XYO_WAIT_WAKE_REGISTRY=1 XYO_WAIT_MULTIVAR=1
XYO_SCHED_READY_INDEX=1 XYO_WAIT_DEADLINE_INDEX=1`
(52.56 seconds).

Additional assertions check removal of a single-variable watch and
stale-entry protection across deletion/recreation, and a two-variable
condition whose second dependency is deleted: the other remaining
variable must not produce notifications from the obsolete subscription.

A separate non-JIT standalone scheduler test returned 274/275 checks
for the experimental flags; its sole failing assertion requires
a compiled native JIT and is not valid in the non-JIT configuration.

## Remaining limitations

Direct public access to `Target::variables` can bypass the
`deleteVariable()` hook, and source edits / AOT / WASM and host
writes are not fully wired. The registry only accumulates advisory
wakeup candidates: full event-driven conditional wait scheduling
is **not enabled**. The test duration is not a performance benchmark.
