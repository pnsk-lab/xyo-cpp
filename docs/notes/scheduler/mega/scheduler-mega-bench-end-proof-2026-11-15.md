# Mega Scheduler β — bench_end strict source certification

2026-11-15. **Audit only; NO bench_end LLVM execution.**

## Scope

The original ScratchBench `bench_end("sched_pingpong", 1)` consists of
the argument-bearing `bench_end %s %s` procedure, a warp
`log text %s` procedure call, an expression of ordered nested
`operator_join`, `operator_round`, `operator_multiply` and
`operator_divide`, followed by an external `​​log​​ %s` call and a
`data_addtolist` write to the `output` list.

`src/jit/mega_bench_end_source.hpp` now contains a fail-closed,
source-specific certification routine:

- Checks the call's literal id/opc arguments, argument ID binding,
  procedure definition and prototype (including warp flag).
- Recursively validates every reporter's opcode, exact operand order,
  scalar variable identity, string separator, numeric literal, and
  absence of unrecognized reporter or trailing statement effects.
- Checks the `log text` call/prototype, the external logging call,
  and the `output` list-append statement; rejects a changed target,
  unexpected opcode or extra statement.
- Bounds expression traversal depth and node count and rejects cycles.
- Does not use procedure names alone to justify executing any effect.

The source proof is invoked through the optional diagnostic gate
`XYO_JIT_MEGA_BENCH_END_AUDIT=1` when the outer final-set is followed
by a `bench_end` call. Its diagnostics are:

```
[mega-bench-end-proof] certified=1 fallback=1
[mega-bench-end-proof] certified=0 fallback=1
```

Crucially, **both outcomes remain on the normal Fiber path**. The
audit flag does not enable a new Mega execution path and no logging or
list update is skipped. This avoids prematurely lowering a string
expression without GC roots or calling the list helper with the wrong
`Variable*` ABI.

## Verification

`tests/bench/algos/test_mega_bench_end_proof.py` constructs a reduced
sender with the actual original `bench_begin`, `bench_round`, and
`bench_end` procedure bodies and their transitive dependencies.

- Original real source: `certified=1`, Mega not installed, `output`
  list still produced by Fiber.
- Mutated opcode: `certified=0`.
- Changed separator: `certified=0`.
- Changed output list target: `certified=0`.
- Replaced dynamic argument reporter: `certified=0`.
- Variable-ID alias with an unchanged visible variable name:
  `certified=0`.
- List-ID alias with an unchanged visible list name:
  `certified=0`.
- Appended an additional logging side effect: `certified=0`.

**8/8 checks PASS**. Previous original-source `bench_begin`,
`bench_round` and `bench_end` parity tests also pass. Release
`xyo` target builds successfully and `git diff --check` passes.

## Existing unit-test configuration

The previous turn's failing tests were rerun with
`XYO_JIT_MODE=eager`. With this explicit mode, Scheduler returned
466 checks, 0 failures and E2E returned 99 checks, 0 failures.
The test run without explicit eager mode had a failing
compiled-script expectation. This finding should not be confused with
a new performance claim.

## Required next step for actual LLVM lowering

The necessary execution contract remains:

1. Evaluate dynamic scalar reporters using the original expression
   ordering and Scratch-compatible helpers.
2. Preserve all temporary strings through any allocating helper
   (LLVM stack SSA values are not themselves guaranteed GC roots).
3. Invoke the real external log primitive in the correct order.
4. Append the final message once using the resolved `Variable*`
   to `xyo_list_add(JitCtx*, Variable*, uint64_t)`.
5. Preserve call/return state and source-revision invalidation across
   joined-broadcast resumption; compare live `output` list values
   between Fiber and Mega before enabling the feature.

The `suite_sched` caller chain remains unsupported. All changes
remain uncommitted; unrelated work was not reset or committed.
