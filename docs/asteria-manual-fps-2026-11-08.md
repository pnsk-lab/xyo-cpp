# ASTERIA MANUAL frame-time investigation (2026-11-08)

The talk target has manual1–manual5 costumes. In Chapter=0 the STAGE
broadcast starts the tutorial sequence. The same broadcast also
activates the background renderer.

To reproduce a subset of the MANUAL path from the title screen
(without navigating the entire game UI), use the test-only
XYO_INJECT direct broadcast extension:

~~~sh
SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=opengl \
SDL_AUDIODRIVER=dummy XYO_EXIT_FRAMES=100 \
XYO_INJECT='20:bc:STAGE' \
XYO_PLAY_PROFILE=debug/asteria-manual.csv \
build/xyo play 'debug/projects/【3D】ASTERIA(Ver.1.1).sb3' \
  --jit=hotspot --fps 30
~~~

This diagnostic is not a complete reproduction of the interactive
MANUAL screen.

## Causes

- The initial STAGE broadcast synchronously compiled multiple large
  HotSpot scripts, one requiring around 250 ms. Default interactive
  HotSpot compilation now uses the asynchronous codegen path, while
  XYO_JIT_ASYNC=0 is a manual opt-out. CLI/noninteractive behavior
  remains synchronous.
- The STAGE event script a{U (target スプライト1) clears and stamps
  the pen backdrop on each frame. Original execution used
  skinPxAt() separately for every output pixel and uploaded the
  entire pen texture after stamping.
- The CPU fallback precomputes costume, effect image and transform
  once per stamp. On OpenGL 3.3+ with a shared GPU pen context,
  a new stamp shader now composites into the pen FBO directly.
  XYO_PEN_STAMP_CPU=1 forces the CPU fallback.

## Measurements

100-frame direct-STAGE test, excluding first 25 frames and with
JIT off to isolate stamping (Mesa llvmpipe OpenGL):

| Metric | CPU stamps | GPU stamps |
|---|---:|---:|
| Mean VM time | 9.01 ms | 1.11 ms |
| Mean draw time | 2.73 ms | 4.34 ms |
| Mean step time | 11.74 ms | 5.46 ms |
| 80 stamp calls cumulative | 731.2 ms | 92.3 ms |

When using HotSpot JIT and GPU stamps:

| Frame | Synchronous JIT | Automatic async JIT |
|---|---:|---:|
| Frame 20 | 858.1 ms | 198.4 ms |
| Frame 21 | 399.3 ms | 67.2 ms |

There remains an early initialization spike (~200 ms) and the real
MANUAL transition needs a user-controlled capture. These tests
cannot establish actual battle FPS.

## Regression

~~~sh
python3 tests/differential/gl_stamp.py build/xyo
python3 tests/differential/gl_effects.py build/xyo

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy XYO_JIT_MODE=eager \
  build/tests/xyo-render-tests
SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=opengl \
SDL_AUDIODRIVER=dummy XYO_JIT_MODE=eager XYO_PEN_CPU=1 \
  build/tests/xyo-render-tests
~~~

All 1118 renderer checks passed in both of the latter environments.
The four-stamp GPU/CPU output comparison differed at 632 pixels
with channel delta > 5 out of 691,200, mostly one-pixel boundaries.
The existing GPU pen point/line parity tests on llvmpipe separately
have 125 mismatches even when GPU effects are disabled.

## Usage

~~~sh
SDL_RENDER_DRIVER=opengl build/xyo play \
  'debug/projects/【3D】ASTERIA(Ver.1.1).sb3' --jit=hotspot --fps 30

SDL_RENDER_DRIVER=opengl XYO_PLAY_PROFILE=debug/real-manual.csv \
XYO_PENPROF=1 build/xyo play \
  'debug/projects/【3D】ASTERIA(Ver.1.1).sb3' --jit=hotspot --fps 30
~~~

The opt-in XYO_TOPPROF=1 reports expensive Scratch hat IDs at
process exit. Pen profiling includes stamps, gpuStamps and stampMs.

## Follow-up: removing the 200ms initialization hitch

Trace mode found the actual main-thread causes:

| Sound | Synchronous decode | Decoded PCM |
|---|---:|---:|
| PerituneMaterial_PV_Emotional | 103.2 ms | 42.7 MB |
| nc29206 | 187.4 ms | 86.3 MB |

SdlAudioBank previously decoded the entire MP3 on the render thread
at the moment a sound block requested playback. It now has a single
background decoding worker. A queued voice is published and started
from the main-thread update; play-until-done remains pending until
completion and stop-all resolves canceled waiters. There is no
concurrent mutation of the decoded sample map or the SDL voice list.

Short sound effects retain synchronous decoding for low-latency onset:
WAV <= 1 MiB and non-WAV <= 64 KiB. Other assets are decoded on the
worker. XYO_AUDIO_SYNC=1 forces the legacy synchronous behavior;
XYO_AUDIO_ASYNC_ALL=1 forces the worker even for short sounds.
XYO_AUDIOPROF=1 logs decoding cost.

90-frame STAGE direct-broadcast run (HotSpot + async JIT + GPU pen):

| Metric | Sync audio | Async audio |
|---|---:|---:|
| Frame 0 (title initialization) | 332.4 ms | 246.0 ms |
| Frame 20 (STAGE start) | 204.0 ms | 33.2 ms |
| Frame 21 | 70.0 ms | 66.9 ms |

Frame 21 was due primarily to the "wirefloor" SVG rasterization,
which was independently measured at 55.5–63.9 ms on Mesa llvmpipe.

A persistent cache for larger SVG costumes stores the exact
already-rasterized RGBA image and the geometry metadata, while SDL
textures are always recreated on the main thread. Repeated renderings
avoid the large librsvg decode. The cache validates its header,
dimensions, source fingerprint and pixel checksum and regenerates
corrupted or incompatible files. Cache files use XDG_CACHE_HOME or
~/.cache/xyo-cpp/skins by default. The default budget is 512 MiB,
with oldest files removed after inserts.

- XYO_SKIN_CACHE=0: disable disk caching
- XYO_SKIN_CACHE_DIR=/path: override cache directory
- XYO_SKIN_CACHE_MB=512: disk budget, valid 64–4096
- XYO_SKINPROF=1: report costume decode and hit/miss status

Comparison on 35-frame ASTERIA direct-STAGE run:

| Metric | Cold cache | Warm cache |
|---|---:|---:|
| wirefloor costume | 63.94 ms | 8.76 ms |
| Frame 21 total | 77.07 ms | 18.53 ms |
| Frame 0 total | 246.31 ms | 101.02 ms |

Frame 20 varied between cold/warm runs because other initialization
and background work overlapped. This was not a controlled real-time
battle FPS study. Screenshots of the animated title also vary with
timing; a separate deterministic static SVG fixture confirms pixel
exactness across fresh, cached and deliberately corrupted cache runs.

Regression commands:

~~~sh
python3 tests/differential/audio_async.py build/xyo
python3 tests/differential/skin_cache.py build/xyo
python3 tests/differential/gl_stamp.py build/xyo
python3 tests/differential/gl_effects.py build/xyo
~~~

All four passed, as did the 1118 SDL dummy renderer checks and
1118 OpenGL checks with XYO_PEN_CPU=1. GPU pen line parity on
llvmpipe still has previously observed independent mismatches.

## Interactive New Game path and idle audio watcher optimization

The **real** New Game path (rather than direct `STAGE` injection) can
be followed by pulsing the `Z` key through the text boxes:

~~~sh
INJECT=$(python3 - <<'PY'
a = ['30:key:z', '31:ku:z']
for n in range(95, 450, 8):
    a += [f'{n}:key:z', f'{n+1}:ku:z']
print('|'.join(a))
PY
)
SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=opengl \\
SDL_AUDIODRIVER=dummy XYO_INJECT="$INJECT" XYO_EXIT_FRAMES=235 \\
XYO_PLAY_PROFILE=debug/asteria-real-manual.csv \\
build/xyo play 'debug/projects/【3D】ASTERIA(Ver.1.1).sb3' \\
  --jit=hotspot --fps 30
~~~

At frame ~182 the actual gameplay begins. The first run spent
~67.5ms on frame182 and ~67.9ms on frame183. Warming the SVG cache
improved frame183 to 17.1ms, but frame182 still took ~70ms. The
per-script and runtime phase profilers attribute the remainder mostly
to newly started scripts and costume initialization; JIT polling was
negligible (<0.01ms). In a later trace of frame182, the `Still？` OP
script alone consumed ~40.6ms, while the newly started talk clone
consumed ~20.4ms. Some of these costs include nested costume decoding.

An additional problem was a busy-polling, read-only audio watcher:
the Stage `OP` handler (hat `a[E`) repeatedly rechecked chapter
conditions even when no other runnable scripts could change them. In
the initial 235-frame run it executed ~1,767,183 times; with the
conservative idle-sound-poll classifier this dropped to ~133 times.
Only a lone script with a statically verified conditional branch tree
and `sound_playuntildone` actions can be deferred; reporters with
random, time, state mutations or unsupported opcodes are rejected.
When another runnable script exists, its normal scheduler order is
preserved. `XYO_IDLE_SOUND_POLL=0` disables this optimization.

| Actual New Game range | Baseline avg | Optimized avg |
|---|---:|---:|
| Frames 70–79 | 24.27ms | 1.65ms |
| Frames 162–180 | 26.67ms | 1.73ms |
| Frames 195–234 | 3.42ms | 3.38ms |
| Frame 182 | 70.34ms | 74.17ms |

The last frame (235) screenshot was pixel-identical with this
optimization enabled vs disabled. Short waiting scripts now use
`STATUS_YIELD_TICK` in interactive renderer-attached playback, rather
than rechecking a timer thousands of times in a frame. **Unpaced CLI
execution** retains its original within-tick yielding because it has
no frame timer; the existing async-JIT headless regression depends on
that. The extra `XYO_STEP_PHASES=1` and `XYO_TOPTRACE=1` switches
report slow frame components and individual slow Scratch scripts.

Tests:

~~~sh
python3 tests/differential/idle_sound_poll.py build/xyo
python3 tests/differential/jit_async_compile.py build/xyo
python3 tests/differential/audio_async.py build/xyo
python3 tests/differential/skin_cache.py build/xyo
python3 tests/differential/gl_stamp.py build/xyo
python3 tests/differential/gl_effects.py build/xyo
~~~

All six passed, including the idle audio watcher's response to
another script changing the watched variable in both interpreter and
HotSpot modes. Further work is needed on the ~70ms **first entry**
into battle (the preceding idle loops are no longer the cause).


## 2026-11-08: Missile firing SIGSEGV (SDL audio callback)

Two desktop coredumps (08:45 and 08:47 JST) identified the crashing thread as
`SDLAudioP19`. Both stack traces reached `SdlAudioBank::mix()` via SDL's
audio callback; one explicitly resolved `std::map<std::string, double>::find`
while reading the target's `soundEffects` map.

The old callback dereferenced `Voice::t` (a `Target*`) to read volume,
pitch and pan concurrently with script execution and clone deletion. This was
both a data race on the effect map and a use-after-free hazard when a missile or
other sound-playing clone was disposed.

Fix:
- Each Voice now carries precomputed resampling step and L/R gains.
- Voice properties are snapshotted on the main thread, under the SDL audio
  device lock when published or refreshed; `mix()` accesses only Sample
  data and Voice scalars, never Target fields.
- `Runtime::disposeTarget` notifies the SoundBank before freeing the clone;
  this removes active/pending voices and resolves outstanding waits safely.
- `peak_` decay was moved under the audio device lock as well.

Verification:
- Two 160-frame modes of `tests/differential/audio_clone_safety.py`:
  interpreter/synchronous short WAV, and HotSpot/forced asynchronous WAV,
  both generated and destroyed 120 sound-playing clones without SIGSEGV.
- Audio async, idle sound polling, and SVG cache differential tests passed.
- A 320-frame offscreen ASTERIA run with injected shooting inputs exited
  normally. This is a smoke test and does not replace testing the exact
  real-device controls and timing.

Regression:
```bash
python3 tests/differential/audio_clone_safety.py build/xyo
```
