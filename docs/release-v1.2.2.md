# xyo-cpp v1.2.2 — exact daysSince2000 JIT fast path

This release updates v1.2.1 with a small, reversible LLVM JIT optimization.

- **Direct f64 daysSince2000() reporter:** Return the interpreter's exact
  millisecond-derived clock conversion directly to generated LLVM code, then
  NaN-box it without redundant Value type tests.
- This preserves fresh system-clock sampling on every reporter invocation;
  it **does not** substitute cached clock values or enable LLVM fast-math.
- Enabled by default when the experimental shared-days-clock mode is not
  requested. Set `XYO_JIT_DAYS_F64=0` to recover the historical boxed helper.
- The separate `XYO_JIT_DAYS_SHARED_CLOCK` setting retains precedence, and
  Interpreter / Eager / HotSpot modes remain compatible.
- Adds `xyo-days-f64-parity`, covering arithmetic with infinities, zero,
  comparisons, warp/non-warp execution, and fast-path on/off across tiers.

Before publishing, verify the complete CTest suite and cross-platform build
jobs. All v1.2.0/v1.2.1 bug fixes remain included in the v1.2.2 tag.

The +3.38% measured improvement in the separate `sched_looptick` paired
experiment motivated this optimization; it is workload-specific, not a
promise of general performance gains.
