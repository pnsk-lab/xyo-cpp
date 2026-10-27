# Differential repros against scratch-vm

`tests/differential/` runs the same `.sb3` in the vendored reference
implementation (`refs/scratch-vm`, TurboWarp 2.1.46) and in xyo, and compares
the observable result of the stage-global list `out`.  The reference is run
with TurboWarp's optional compiler disabled (`vm.setCompilerOptions({enabled:
false})`) so it follows official Scratch's scratch-vm interpreter semantics.

These are **regression cases** for the divergences documented in
[docs/scratch-divergences.md](../../docs/scratch-divergences.md).  Before
those fixes, these cases exposed the documented divergences and two of them
terminated xyo with `SIGSEGV`; after the fixes `run_repros.py` must report
`0 differ` in both modes (currently 19 cases).

## Layout

| File | Purpose |
| --- | --- |
| `gen_repros.py` | deterministic generator for every checked-in `repro/*.sb3` |
| `scratch_vm_dump.js` | headless scratch-vm runner; dumps `String(value)` for every variable/list |
| `run_repros.py` | runs scratch-vm + `xyo run` (HotSpot) + `xyo run --jit off`, prints a comparison |
| `repro/*.sb3` | minimal projects using only stock Scratch 3 blocks |

## Usage

Prerequisites: the vendored `refs/scratch-vm` must have its `node_modules`
installed, Node.js must be runnable, and `build/xyo` must be built.

```sh
python3 tests/differential/run_repros.py
python3 tests/differential/run_repros.py --xyo /path/to/xyo --json
python3 tests/differential/gen_repros.py            # regenerate repro/*.sb3
```

Each row prints three results:

* `scratch` — the reference (`String(value)` of `out`)
* `hotspot` — xyo's default HotSpot mode
* `interp` — xyo with `--jit off`

`exit=-11` means xyo crashed with `SIGSEGV`.

## Why `String(value)`

Scratch's `join`, `say`, list display, and equality all use the JavaScript
`String()` / `Number()` conversions. Dumping `String(value)` therefore covers
the observable representation without depending on the renderer.

The scratch-vm runner attaches the repository's `FakeRenderer` and stubs
`document.hidden`; without them TurboWarp's runtime calls
`renderer.destroyDrawable` and touches `document` even in headless mode.

## Cases

| Fixture | What it probes |
| --- | --- |
| `nan_index_delete.sb3` | `delete (0/0) of list` — SIGSEGV in xyo |
| `nan_index_insert.sb3` | `insert (0/0) at 1 of list` — SIGSEGV in xyo |
| `big_number_string.sb3` | `join("", n)` for doubles above 2^53 |
| `unicode_casefold.sb3` | case-insensitive `=` for non-BMP characters |
| `round_negative_half.sb3` | `round(-0.5)` / `round(-1.5)` / sign of `-0` |
| `costume_rounding.sb3` | `switch costume to (0.5)` — `std::round` vs `Math.round` in `setCostume` |
| `jit_mod_extremes.sb3` | inlined JIT `mod` with `Infinity` / `1e21` / denormals |
| `jit_negzero.sb3` | JIT `round`/`mod` normalising `-0` to `+0` |
| `jit_trig_overflow.sb3` | JIT `sin`/`cos` of `1e308` |
| `jit_invtrig_ulp.sb3` | JIT `asin` constant re-association (1 ULP) |
| `libm_ln_log.sb3` | glibc `ln`/`log` vs V8's last digit (platform libm) |
| `math_fdlibm.sb3` | glibc `asin`/`acos`/`atan`/`exp` vs V8's fdlibm (last bit) |
| `final_sigma.sb3` | Greek final sigma in case-insensitive `=`/`contains` |
| `unicode17_casefold.sb3` | Unicode 17 case pairs missing from the generated table |
| `list_contents.sb3` | `data_listcontents` with single-digit numbers (`[1,2,3]`) |
| `point_towards_ulp.sb3` | `point towards` / fdlibm `atan2` last bit |
| `move_steps_ulp.sb3` | `move () steps` / fdlibm `sin`/`cos` last bit |
| `timer_granularity.sb3` | `sensing_timer` is quantized to whole milliseconds |
| `var_id_name_mismatch.sb3` | JIT variable resolution with mismatched id/name |

Time-, random-, renderer-, audio-, and input-dependent blocks are deliberately
not used, so a run is reproducible.
