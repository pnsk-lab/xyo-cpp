# FidelityFX FSR 1 for the Scratch pen layer (experimental)

Only the **display copy** of the pen layer is upscaled; the Scratch 480×360 pen raster, stamps, collision sampling, hot-start snapshots and script execution remain unchanged. Opt-in FSR 1 EASU runs on the GPU through the SDL OpenGL renderer. RCAS is optional (`XYO_PEN_FSR_RCAS=1`, off by default). The FSR result is reused when the accumulated pen image has not changed.

## Try it

```sh
XYO_PEN_FSR=1 XYO_RENDER_SCALE=1 ./build-pen-fsr-jit/xyo play \
  debug/projects/3D2.sb3 --scale 2
```

To compare with traditional SDL scaling, rerun with `XYO_PEN_FSR=0`. Ensure `XYO_HIGH_QUALITY_PEN` is not set: when High Quality Pen already matches the output resolution, there is no need for FSR to upscale. Do not set `XYO_RENDER_SCALE` when comparing *pen-only* results because that independently scales the whole stage.

GLSL 4.20 / OpenGL 4.2 or later and the SDL OpenGL renderer are required. The shader uses AMD's MIT-licensed FidelityFX FSR 1 functions. On unsupported renderers, software OpenGL (llvmpipe, softpipe, SwiftShader), failed shader compilation or failed framebuffer allocation, the original SDL pen display path is used. `XYO_PEN_FSR_ALLOW_SOFTWARE=1` overrides the software GPU safeguard for offline quality tests, but can be extremely slow. Enable `XYO_PEN_FSR_DEBUG=1` to diagnose startup and shader status. Other renderer backends are not changed.

## Validation

- FidelityFX FSR 1 GLSL EASU and RCAS shaders validated using `glslangValidator -S frag` (both pass).
- `cmake -S . -B build-pen-fsr-jit -DXYO_JIT=ON -DXYO_JIT_BACKEND=llvm -DXYO_BUILD_SHELL=ON -DXYO_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release` and `cmake --build build-pen-fsr-jit --target xyo-cli xyo-pen-bench xyo-render-tests` pass. **The original build-fsr-pen accidentally had JIT disabled; do not use it for heavy 3D projects.**
- Renderer test suite passes with `XYO_JIT_DISABLE=1 XYO_PEN_FSR=1 ctest --test-dir build-pen-fsr-jit -R '^xyo-render-tests$' --output-on-failure`.
- This sandbox's `SDL_VIDEODRIVER=offscreen` uses llvmpipe (software OpenGL); the NVIDIA GPU/driver is unavailable here. Actual hardware GPU screenshots, quality and FPS comparisons remain unverified. The `3D2.sb3` 5-frame llvmpipe benchmark produced 1.143 FPS with FSR auto-disabled; that is not a valid estimate for NVIDIA hardware.
- Frame Generation is **not** implemented by this change. It requires a separate presentation cadence and temporal information.

## Upstream source and licensing

`third_party/fidelityfx/ffx_a.h` and `ffx_fsr1.h` were imported from [AMD's FidelityFX FSR 1 repository](https://github.com/GPUOpen-Effects/FidelityFX-FSR/tree/master/ffx-fsr); their MIT license is included in `third_party/fidelityfx/license.txt`.

`tools/embed_fsr1.py` regenerates the build-embedded `src/render/fsr1_embedded.hpp` from the official shader headers and minimal application-specific callbacks. Neither upstream header is modified.