# ScratchBench GUI parity 2656/2656 — 2026-11-09

## Result

The latest `scratch-bench/.cache/build/parity.sb3` (2656 checks in 206 groups)
passes its **own Scratch oracle**, not merely XYO-to-XYO differential comparison.
The same frozen SDL+LLVM Release binary
`/home/kicky/.cache/xyo-perf-days-finite-20261109/xyo`
has SHA256:

```
8aa44401d8963bb5c0badcd03ea77a49cd3763087128ba9aabf9d3c75e9428e6
```

| Mode | Result | Verification |
| ---- | ------ | ------------ |
| Interpreter (`off`) | **2656/2656, PASS** | `gui_scratch_parity_regression.py --modes off` |
| HotSpot JIT (`hotspot`) | **2656/2656, PASS** | `gui_scratch_parity_regression.py --modes hotspot` |
| Eager JIT (`eager`) | **2656/2656, PASS** | `gui_scratch_parity_regression.py --modes eager` |
| HotSpot + `XYO_JIT_DAYS_FINITE_SSA=1` | **2656/2656, PASS** | earlier explicit ON A/B |

All runs set `SDL_VIDEODRIVER=offscreen`, `SDL_AUDIODRIVER=dummy`,
`XYO_EXIT_ON_DONE=1`, `XYO_EVENT_SCHED=0`, and
`XYO_IDLE_SOUND_POLL=0`. No test oracle or tolerance was changed.
This establishes full parity **for this fixture, binary, and environment**;
it is not a claim of 100% compatibility across all Scratch projects.

## Corrections

- `operator_mathop`: Scratch's `sin(-0)` and `tan(-0)` emit **+0**,
  with matching interpreter / JIT behavior.
- Color/Whirl: a nonzero raw hue effect of 200 retains grayscale saturation
  conversion despite wrapping to a zero hue offset; fix the transposed GL
  whirl rotation to match Scratch's column-major matrix on CPU and GPU.
- Sound: match named sounds using strict **string** identity;
  restarting the same shared `Sound` source across original/clones stops
  older voices and resolves their play-until-done waiters; clamp pitch to
  `[-360,360]` at actual playback.
- Sprite bounds: use the renderer's rounded measurement position and
  visible pixel-centre hull for edge bounces; remeasure after direction
  reversal for left-right/off-centre costumes; avoid rounding-only moves.
- Sensing: clip sprite/sprite and touching-color scans to the actual stage,
  not off-stage costume intersections.
- Clock: interactive GUI with attached audio uses the **real integer-ms**
  clock, matching Scratch's `Date.now()`. This avoids the previously
  synthetic +1ms/frame advancing timers faster than wall-clock audio. The
  unthrottled headless path retains its historical monotonic progression.
  `XYO_REAL_TIME_CLOCK=1` and `XYO_REAL_TIME_CLOCK=0` explicitly override
  the default for diagnosis.
- Promise-next-tick: resolve no earlier than a new clock millisecond on
  both interpreter and LLVM helper paths; prevents frame-frozen timers from
  remaining identical when turbo GUI frames execute within one ms.
- Native glide: the initial non-warp frame yields *before* any interpolation;
  a Warp glide normally runs without another scheduler handoff until the
  warp budget expires. Infinite glides remain interruptible for clone
  deletion and cooperative scheduling.

## CTest integration and core unit suite

`tests/CMakeLists.txt` uses `unit/test_*.cpp` for the combined
`xyo-tests` executable, excluding standalone `unit/jit_*_smoke.cpp`
programs that define their own `main`. After reconfiguring the dedicated
Release SDL build with `-DXYO_BUILD_TESTS=ON`:

- `cmake --build ... --target xyo-tests -j 4`: PASS; previously linked
  duplicate `main` symbols when all `unit/*.cpp` files were globbed.
- `ctest --test-dir ... -R '^xyo-tests$' --output-on-failure`: PASS
  (52.88 seconds).
- `ctest --test-dir ... -R '^xyo-gui-scratch-parity$'`: PASS
  (23.71 seconds).

An additional, explicitly *conditional* `xyo-gui-scratch-parity-full`
CTest now registers when
`scratch-bench/.cache/build/parity.sb3` is present at CMake configure
time. It exercises the latest 2656 tests in **off, hotspot and eager**
modes and requires all 2656 to pass, with a 400s time limit. The
checked-in smoke fixture and its CTest remain unaffected when ScratchBench
has not generated its large fixture. A clean checkout therefore does not
claim the 2656 tests were run.

## Reproduction

```bash
BIN=/home/kicky/.cache/xyo-perf-days-finite-20261109/xyo
PROJECT=scratch-bench/.cache/build/parity.sb3
python3 tests/differential/gui_scratch_parity_regression.py \
  --native "$BIN" --project "$PROJECT" --minimum 2656 \
  --modes off hotspot eager
```

Separate targeted verification:

- `tests/differential/audio_async.py`: PASS sync, hybrid, forced-async and cancel.
- `tests/differential/audio_clone_safety.py`: PASS 120/120 clone disposal in
  both interpreter/sync and hotspot/forced-async modes.
- `tests/differential/gl_effects.py`: PASS CPU/GL effects comparison; 339/691200
  pixels with >5 channel difference (allowed maximum 0.3%), six GL passes.

`git diff --check` passes. Additional agents' modifications remain untouched
outside the bounded changes above. No commit or push performed.


## CI fixture pin and strict parity gate

The existing `scratch-bench` GitHub Actions job already ran a Parity
benchmark, but used an older pinned ScratchBench checkout and allowed
`96%` across all runtime variants. Its earlier pin
`cca6ab15108301bee6b7249db60f00e82f141159` references the older
parity submodule `7e5fe235443c63230991ef108ff2fe3607fcb382`.

The CI checkout now pins `gradylink/scratch-bench` to
`4099437b264c56844ded45c612202815ea025a16`, which points to the
2656-case parity source at
`4dd504403a339da5362e67e467e0d4a99e80492d`.

`tools/ci/scratch-bench-parity-gate.ts` requires **2656/2656** and
the `passed` verdict for **all four** xyo-cpp variants: Interpreter,
HotSpot, Eager, and AOT. This rejects an accidentally stale 2123-case
fixture and fails closed on an unknown runtime variant.

`bun test tools/ci/scratch-bench-parity-gate.test.ts`: **17/17 PASS**.
The CI also checks the exact nonempty AOT object for the current SB3 byte
size. Its final AOT-only replay verifies that a native
`aot: N scripts precompiled` line is present and no AOT load-failure line
appeared, rather than treating silent JIT fallback as AOT success. These
are local tests; the updated GitHub Actions workflow has **not yet run**.

Rebuilt the full project with `fractch 0.9.0` from the pinned submodule
source. All 47 source files passed `fractch check`. The newly generated
SB3 contains 26 targets and 25,580 blocks; although the ZIP file SHA
changes, every uncompressed ZIP member matches the earlier generated
`scratch-bench/.cache/build/parity.sb3` byte-for-byte.

The rebuilt file also passes the real SDL-offscreen Scratch oracle
in all three validated modes: **2656/2656 each** using the dedicated
Release binary with SHA256
`7f3e6a359daf5a87818b83317bc169de2af8da355297fbc864b5d3e9a16e12c1`.


## AOT completion and end-to-end ScratchBench verification

The dedicated Release SDL+LLVM executable compiled
`/home/kicky/.cache/xyo-parity-ci-2656-20261109.sb3` into a genuine
ELF64 relocatable AOT object of **13,864,912 bytes**,
SHA256 `a31be9ce8a2cf085fe6ba48ce7ac63805487035e4b75173adda4282e26cf5038`.
The compiler processed 487 scripts; 479 script bodies and 8423 slots
were registered when loading that object. A direct
`xyo play ... --aot <object>` run emitted
`aot: 479 scripts precompiled (8423 slots)` and
`[aot-run] AOT ロード完了`, and returned **2656/2656 PASS**.

A separate *complete* ScratchBench invocation generated the project
again from the currently pinned parity source and executed the actual
four runtime variants with `XYO_BIN` pointing at the same Release
binary, `--offline --suite parity --parity-runs 1 --timeout 180`:

| Variant | Scratch oracle | Startup | Execution | Peak RSS |
| --- | --- | ---: | ---: | ---: |
| HotSpot | 2656/2656 | 854.7 ms | 14,441.7 ms | 637.5 MB |
| Interpreter | 2656/2656 | 282.9 ms | 14,823.7 ms | 609.8 MB |
| Eager | 2656/2656 | 12,060.3 ms | 19,314.6 ms | 573 MB |
| **Native AOT** | **2656/2656** | **337 ms** | **14,129 ms** | **535.8 MB** |

The end-to-end output is at
`scratch-bench/results/parity001-final-2656/`; the actual ScratchBench
AOT object is
`scratch-bench/.cache/runtimes/xyo-aot/parity.sb3.448652.o`.
That specific output was independently checked using
`gui_scratch_parity_regression.py --modes aot --aot-object ...`,
requiring both actual precompiled loading and 2656/2656.
This independently rejects a silent JIT fallback.

The optional CTest full parity test still covers three modes; the
ScratchBench CI workflow verifies the fourth native AOT mode separately.
The updated workflow still requires an actual GitHub Actions run
after commit/push, which has not been performed.
