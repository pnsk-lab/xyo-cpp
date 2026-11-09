# Scratch CFG type-flow safety and JIT integration (2026-11-06)

## Implementation

The Scratch CFG analysis (`src/analysis/control_flow.*`) models if/else joins,
loop backedges, explicit barriers, reachability and dominance. The fixed-point
solver (`src/analysis/type_flow.*`) tracks sets of possible boxed storage
kinds, where a **missing fact means all kinds**. Unknown reporter inputs,
procedures, extensions and yield boundaries invalidate tracked facts.

### Alias and side-effect hardening

- New optional `Target*` resolution canonicalizes **exact variable IDs**
  and name-only legacy fields to the same scalar `Variable::id`.
- An ID that resolves to a list/broadcast must **never** be reinterpreted as
  a scalar even if another scalar shares its display name.
- An unknown field/variable invalidates all facts. There is no assumed type
  for an external or potentially aliasing write.
- Reporter purity is checked against an **explicit allowlist**, not inferred
  from broad opcode prefixes (`operator_` and `math_` may be extended).
- A loop backedge erases all type facts because another script may have
  changed a variable at the scheduler boundary. CFG analysis starts at
  unknown type, not at a previous profile or project default.

### Implicit loop-variable writes

`control_for_each` changes its VARIABLE implicitly before executing the
body, so a preceding String/Boolean proof cannot be carried into the body.
The fixed-point transfer now kills that variable's type fact at the loop
header (or all facts if its identity is unresolved). It intentionally does
not claim the header's outgoing type is Number: the loop may also take
a zero-iteration exit. A CFG unit regression covers pre-loop String and
loop body/exit unknown states.

### JIT

The optional `XYO_JIT_CFG_TYPE_HINTS=1` path in
`src/jit/codegen.cpp` requests target-aware CFG dataflow. It only changes
the **choice of eager vs lazy numeric shadows** when every reachable write
to a variable is proven to produce Number. It does not remove runtime
coercion, type tags or comparison eligibility guards. The default is OFF:
the startup-inclusive synthetic copy test was not faster (median about
29.7ms vs 27.3ms, 4 runs each).

The `XYO_JIT_LOCAL_VAR_FORWARD` optimization is separately scoped to
within-block/validated branch PHIs, and still falls back across yields.

### Validation

- CFG unit tests cover dominance, loops, string/number union, unknown
  effects, type-literal storage, name-only aliases, list-ID collisions and
  unknown `operator_` reporters.
- `tests/differential/jit_type_flow_safety.py`: 12 configurations of
  interpreter/eager/hotspot, warp/non-warp, numeric strings, whitespace,
  Boolean, color, comparisons, and repeated numeric reads.
- `tests/differential/jit_cfg_type_hints.py`: eight positive paths plus
  warp/non-warp negative cases where a merged String|Number value must stay
  dynamic.
- `tests/differential/jit_type_flow_fuzz.py`: 24 deterministic generated
  SB3 programs, each compared to the interpreter in 4 JIT configurations
  (96 differential comparisons passed); registered in CTest.
- `xyo analyze PROJECT.sb3 --type-flow` prints per-script CFG nodes,
  loop latches, barriers and selected known type sets; this is diagnostic,
  not a proof that a variable remains monomorphic at runtime.
- The independent `3D2.min.sb3` fixture produced 56 scripts, 782 CFG nodes
  and 41 loop latches (analysis only).

## Remaining work

Full interprocedural effect summaries, scheduler-aware proof scope,
IR-level deoptimization and guard elimination, memory GC path validation,
and performance tests across real independent projects are not yet complete.
All non-proven typed uses retain the original boxed guards and conversion.

## Follow-up verification

The four focused tests after target-aware canonicalization passed:
`xyo-tests`, `xyo-jit-lazy-typed-ssa`,
`xyo-jit-type-flow-safety`, and `xyo-jit-cfg-type-hints`.
The CTest-registered differential fuzz and two related tests passed 3/3.
The full repository CTest suite has not been rerun after these changes.

A per-procedure effect summary is intentionally **not** used to keep type
facts across `procedures_call` yet: even a seemingly straight-line
procedure may hit a warp deadline and yield, allowing another script
to mutate the variable. A trusted interprocedural analysis must model
these scheduler and host boundaries first.
