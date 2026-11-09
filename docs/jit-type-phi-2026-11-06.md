# Branch-join SSA forwarding for mixed Scratch variables (2026-11-06)

## Implementation

The existing loop-local write-through SSA forwarding is extended across `control_if` and `control_if_else` join points. Only variables with an active guarded lazy typed SSA cache and a unique resolved `Variable*` can be forwarded. The compile-time dataflow map is intentionally conservative:

1. Clear block-local facts at a branch boundary.
2. Compile both successors independently, preserving only facts produced in their final LLVM predecessor blocks.
3. If both successors reach the merge and define the same variable, emit a **boxed `i64` PHI** with the two producer values.
4. Consume the boxed PHI on subsequent reads in the merge block. When **both** incoming SSA producers carry a validated `knownNumD` value and a corresponding `knownNumOk` eligibility bit, also emit `f64` and `i1` PHIs. These preserve the pair `toNumber(bits)` / Scratch comparison eligibility and let numeric consumers reuse it without recomputing conversions. When either incoming fact is missing, fall back to the boxed PHI and normal runtime coercion.
5. Otherwise retain the ordinary promoted variable load and guarded conversion path.

No facts are propagated across a yield, loop backedge, unresolved/aliased variable IDs or an unproven branch path. The existing LLVM IR and `mem2reg` still own PHI construction for numeric shadow allocas.

`XYO_JIT_LOCAL_VAR_FORWARD=0` disables both same-block and new branch-join forwarding for A/B measurement. The default enables these only for eligible mixed-type lazy shadows.

## Verification

- xyo-cli and xyo-tests build succeeded.
- The mixed typed-SSA differential fixture was expanded with both-sided `if_else` writes of different storage kinds and a second join where both sides produce numeric SSA values, followed by repeated numeric use. All 16 eager/hotspot, warp/nonwarp, lazy/type-flow configurations passed.
- With `XYO_JIT_DUMP=1`, the unoptimized IR visibly contains `typed.merge.bits = phi i64` and, for the two-numeric-producer join, `typed.merge.number = phi double` and `typed.merge.cmpok = phi i1`. An ensuing addition consumes the numeric PHI.
- Synthetic mixed-type fixture startup-inclusive wall-time for 4 runs each: OFF 0.2767 / 0.2546 / 0.2625 / 0.2691 seconds, ON 0.1811 / 0.1780 / 0.1854 / 0.1775 seconds. The fixture was designed to exercise this code path, and the result is not representative of real projects.
- Existing CTest checks for lazy typed SSA and xyo-tests passed. A concurrent CTest run hit transient PermissionError executing build/xyo in two tests, and those are to be rerun serially.
- No broad performance improvement claim is made. The type lattice is not a global fixed-point analysis, and Scratch semantics are protected by retaining boxed values and the existing runtime guards.

The initially parallel targeted CTest run encountered transient PermissionError executing the binary in two cases (likely concurrent build activity). Both cases, `xyo-jit-ssa-scheduler` and `xyo-jit-string-pair-compare`, passed when re-run serially. The numeric PHI modification was subsequently checked with the expanded 16-case differential test (all passed).
