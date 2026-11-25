# GPU graphics effects for Scratch playback (2026-11-08)

## Implementation

On SDL's OpenGL 3.3+ renderer, colour, brightness, fisheye, whirl,
pixelate and mosaic are performed by a fragment shader on the GPU.
Ghost uses SDL's GPU texture alpha modulation. The underlying costume
texture remains unchanged.

- Each drawable owns at most one effect texture, irrespective of how
  many effect values have occurred previously.
- The GPU effect cache has a 256 MiB default soft limit and evicts the
  least-recently-used drawable. SDL queued renders are flushed before
  releasing texture resources. One oversized texture is allowed.
- Pixel-exact queries for collision, touching-colour, and pen stamps
  continue through the original CPU effect implementation.
- OpenGL state (program, VAO, framebuffer, viewport, active texture,
  filtering, blend, scissor, depth and culling) is restored after the
  shader pass, preventing renderer corruption.
- Non-OpenGL/unsupported GL renderers retain the CPU fallback. This
  includes Metal, Direct3D, OpenGL ES and SDL dummy video drivers.

Relevant files: src/render/gl_effects.hpp, src/render/gl_effects.cpp,
src/render/sdl_renderer.cpp, src/render/sdl_renderer.hpp,
src/render/effects.cpp.

## Configuration

- XYO_FX_CPU=1 forces software graphics effects for comparison.
- XYO_FX_CACHE_MB=256 selects a 1–4096 MiB soft texture-cache limit.
- XYO_FX_DEBUG=1 logs GL initialization and shader/FBO failures.
- SDL_RENDER_DRIVER=opengl requests the OpenGL SDL backend.
- XYO_PLAY_PROFILE=path.csv records per-frame gpu_effect_passes and
  gpu_effect_ms alongside the existing profiler fields.

gpu_effect_ms measures CPU time spent submitting GPU work, not actual
GPU execution time. It is already part of draw_ms.

## Reproducibility

~~~sh
python3 tests/differential/gl_effects.py build/xyo

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy XYO_JIT_MODE=eager \
  build/tests/xyo-render-tests

SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=opengl \
SDL_AUDIODRIVER=dummy XYO_JIT_MODE=eager XYO_PEN_CPU=1 \
  build/tests/xyo-render-tests
~~~

ASTERIA real-image stress test:

~~~sh
python3 tests/fixtures/gen_asteria_fx_stress.py \
  'debug/projects/【3D】ASTERIA(Ver.1.1).sb3' \
  debug/asteria-fx-stress.sb3 --count 8

SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=opengl \
SDL_AUDIODRIVER=dummy XYO_EXIT_FRAMES=36 \
XYO_PLAY_PROFILE=debug/stress-gpu.csv \
  build/xyo play debug/asteria-fx-stress.sb3 --jit=hotspot --fps 30

# Set XYO_FX_CPU=1 for the CPU comparison.
~~~

## Validation (Mesa llvmpipe OpenGL, 8 animated effects)

36 frames of 960x720 ASTERIA source imagery. Steady-state timings
exclude frame zero.

| Metric | CPU effects | OpenGL effects |
|---|---:|---:|
| Mean draw_ms, frames 1–35 | 161.46 ms | 16.19 ms |
| CPU effect bitmap rebuilds | 284 | 0 |
| GPU effect passes | 0 | 284 |
| Costume/effect texture uploads | 292 | 8 |
| Maximum resident memory | 1302.7 MiB | 259.4 MiB |

The stress test's screenshot showed at most 1 RGB intensity level
of discrepancy between GPU and CPU. Forcing a 4 MiB cache and taking
the screenshot at the same frame produced identical pixels to the
uncapped run.

The original ASTERIA introductory 140-frame run was ghost-only,
so both the earlier alpha optimization and this shader code had
little additional work to do there. The actual battle scene has
not been reproduced in this automated harness.

The existing GPU pen tests on Mesa llvmpipe fail 125 checks with
or without XYO_FX_CPU=1. This is independent of the new effect path.
With XYO_PEN_CPU=1, all 1118 rendering checks pass, including
the accelerated ghost/brightness regression.
