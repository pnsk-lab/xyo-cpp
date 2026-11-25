# xyo-cpp v1.3.2 — MLIR frontend speedups

## Highlights

The default MLIR frontend (`src/mlir/`, active under `--jit eager` /
Mega preset) now matches or beats the legacy codegen on essentially every
workload in the algo bench matrix. Legacy is reachable only via
`XYO_MLIR=0` as an A/B harness.

- **MLIR par-for**: parallel `repeat { change i; add to L }` loops are
  privatized onto index slots and run on the worker pool
  (`xyo.parfor.try` / `emitParForTry`, same semantics as legacy
  `tryEmitParFor`). `micro_append` 27 → 16 ms.
- **Callee-effects folding**: transparent `xyo.call` targets merge their
  writes into the enclosing `LoopScope`, so loop-invariant `to_num`
  caching and list-header hoisting survive procedure calls.
- **Same-block getvar memo**: within one basic block and one write
  epoch, `Variable.value` loads and `to_num` conversion chains are
  shared per variable (equivalent of legacy `readNumberMemos`).
- **numericOnly elision (`numguard`)**: replace/add/insert fast paths
  skip the `SList::numericOnly` read-modify-write when `numFacts` proves
  the written value is numeric (`box.f`/`from_num` of a non-NaN f64).
  `micro_write` 73 → **44 ms** — now *faster* than legacy (59 ms).
- **const-int mod fast path (`mod.c32`)**: `a mod c` with a positive i32
  constant divisor uses `fptosi→srem` (`and` for powers of two) when `a`
  is a proven i32 integer, with `copysign` preserving Scratch's -0.0
  convention. `sha256_65536` 83 → 72 ms.

## Verification

- `tests/mlir/xcheck.py` 45/45, `xyo-mlir-frontend` PASS.
- Full bench matrix re-verified; checksums match legacy everywhere
  except the non-deterministic collatz bench.
- Eager path is at parity-or-better on every measured workload; the only
  remaining hotspot-only gap is `sha256_16384` (~18 ms MIR vs ~7 ms
  legacy, cause not yet identified).

## Known issues

- Unrelated in-flight work on the board can leave `xyo.matpow.try`
  lowering falling back; that path is uncommitted WIP, not part of this
  tag.
