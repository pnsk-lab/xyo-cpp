# xyo-cpp: NVIDIA DLSS Super Resolution (Linux / Vulkan + SDL OpenGL)

## Status

The experimental display scaler invokes **NVIDIA's actual NGX DLSS Super
Resolution SDK 310.9.16**, not the unrelated OpenGL Catmull-Rom filter.
On an RTX 3070 (driver 615.78.08), both standalone smoke tests and
`xyo play` have run successfully.

Enable it **only when explicitly requested**. The normal SDL/OpenGL stage
path remains available as a fallback. The SDK binaries/models stay in a
separate SDK checkout and are **not redistributed** with this repository.

The pipeline composites a low-resolution SDL stage texture, uses OpenGL
framebuffer copies into Vulkan-exported shared GPU memory, performs NVIDIA
DLSS through the Vulkan SDK, waits with shared GPU semaphores, then copies
the output to an SDL render target. Full-frame pixel readback is not
required during playback; opt-in Optical Flow confidence detection reads a
small cost map (10,800 bytes for a 480×360 input) instead.

## Build

A developer-owned NVIDIA DLSS SDK checkout is required, including
`include/nvsdk_ngx_helpers_vk.h`, `lib/Linux_x86_64/libnvsdk_ngx.a`,
and the runtime model at `lib/Linux_x86_64/rel/`.

```sh
cmake -S . -B build-dlss \
  -DXYO_JIT=ON \
  -DXYO_NVSDK_DLSS_VK=ON \
  -DXYO_NVSDK_DLSS_SDK_ROOT="$HOME/.cache/xyo-dlss-sdk" \
  -DXYO_BUILD_SHELL=ON
cmake --build build-dlss --target xyo-cli -j 6
```

The normal build defaults to `XYO_NVSDK_DLSS_VK=OFF` and does not need
NVIDIA SDK files.

## Launching a project

```sh
XYO_DLSS_REAL=1 \
XYO_DLSS_PRESET=K \
XYO_DLSS_MODE=quality \
XYO_DLSS_REAL_DEBUG=1 \
XYO_DLSS_GPU_TIMING=1 \
XYO_RENDER_SCALE=0.5 \
XYO_PEN_FSR=0 \
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
  ./build-dlss/xyo play debug/projects/3D2.sb3 --scale 2
```

On the working RTX 3070 host, the existing LLVM JIT-enabled binary is
`$HOME/.cache/xyo-dlss-build/play/xyo`.

`XYO_DLSS_PRESET` supports `K` (default, transformer Quality/Balanced
hint), `J`, `M` (Performance-mode hint), and `auto` (SDK/driver default). These are requested hints,
not proof of which neural network the driver ultimately selected.

`XYO_DLSS_MODE` supports `quality` (default), `balanced`, and
`performance`. The quality mode and preset are **independent**.
`XYO_DLSS_REAL_DEBUG=1` reports the SDK's optimal input resolution
and allowable range for the selected output resolution.
`XYO_DLSS_GPU_TIMING=1` enables Vulkan timestamp queries around actual
NGX GPU work, not just CPU command submission.

For a fair baseline, use the same render scale with `XYO_DLSS_REAL=0`
and `XYO_PEN_FSR=0`.

## NVIDIA hardware Optical Flow (experimental)

The optional motion-vector path uses actual `VK_NV_optical_flow` on two
consecutive rendered Scratch stage frames, instead of zero motion.
The hardware runs on Vulkan optical-flow queue family 5, while NGX and
motion-vector conversion run on compute queue family 0. Two GPU
semaphores synchronize the queues with no per-frame CPU pixel readback.

The engine outputs `VK_FORMAT_R16G16_SFIXED5_NV` vectors on a 4x4 grid.
A compute shader expands these to input-resolution `VK_FORMAT_R16G16_SFLOAT`.
The vector direction is current frame to previous frame, in input pixels.
First frame after initialization or resize resets DLSS temporal history.

Enable both real optical motion and experimental temporal reuse:

```sh
XYO_DLSS_REAL=1 \
XYO_DLSS_OPTICAL_FLOW=1 \
XYO_DLSS_TEMPORAL=1 \
XYO_DLSS_MODE=performance \
XYO_DLSS_PRESET=M \
XYO_RENDER_SCALE=0.5 \
XYO_PEN_FSR=0 \
XYO_DLSS_REAL_DEBUG=1 \
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
  "$HOME/.cache/xyo-dlss-build/play/xyo" play \
  debug/projects/3D2.sb3 --scale 2
```

`XYO_DLSS_OPTICAL_FLOW=1` enables per-pixel optical motion estimation.
`XYO_DLSS_TEMPORAL=1` separately enables NGX history reuse. Both options
are OFF by default; enabling Optical Flow alone leaves temporal history
reset every frame as a conservative diagnostic mode.

### Confidence-map scene-cut rejection (experimental)

The RTX 3070 NVOFA supports an R8_UINT cost map, one byte per 4×4
optical-flow grid cell (10,800 bytes for a 480×360 image). Higher cost
means less reliable estimated motion. Only this small cost map is
transferred to a host-coherent buffer; full-resolution images and motion
vectors stay on GPU.

A frame is classified as **unreliable motion** if both the mean cost
is at least 8 and at least 70% of cells have cost at least 8.
These thresholds are empirical, not NVIDIA-standard confidence levels.
Override with `XYO_DLSS_COST_MEAN` and `XYO_DLSS_COST_FRACTION`
(fraction from 0 to 1); disable with `XYO_DLSS_SCENE_CUT=0`.

By default the cost is read after the existing end-of-frame GPU fence
and resets DLSS history on the **next** frame if tracking failed.
`XYO_DLSS_SCENE_CUT_SYNC=1` adds an optical-flow fence wait, permitting
a decision on the *current* frame before NGX evaluation. It trades
extra CPU/GPU synchronization for fewer ghosting artifacts at scene cuts.

Synthetic RTX 3070 measurements:

| Frame transition | Mean cost | Fraction cost >=8 | Rejected? |
|---|---:|---:|---|
| 6px horizontal translation | 0.19 | 1.5% | No |
| 24px translation | 0.48 | 5.0% | No |
| 48px translation | 0.88 | 10.0% | No |
| 96px translation (tracking failure) | 11.07 | 86.1% | Yes |
| Uncorrelated frame | 13.75 | 100% | Yes |

SDL/NGX end-to-end tests with immediate classification detected 10/10
uncorrelated frames after warm-up, without misclassifying 10/10 frames
of 6px motion. On a 240-frame mixed stress test, delayed vs immediate
detection took 3,023 ms vs 3,061.5 ms respectively. Those timings are
not project FPS. Uniform flashes, occlusions and transparent surfaces
still require better confidence handling and real-project validation.

### Local Optical Flow cost masking (experimental)

The optional `XYO_DLSS_MOTION_COST_CLIP=1` mode instructs a Vulkan compute
shader to inspect NVIDIA's R8_UINT cost map for **each 4×4 optical-flow
cell** when expanding SFIXED5 vectors to RG16F motion at input resolution.

When a cell's cost is at least 8, its motion vector is replaced by
(0, 0). Otherwise, its original motion is preserved. Override the byte
threshold with `XYO_DLSS_MOTION_COST_LIMIT` (1–255).
This costs no additional CPU round-trip: the cost buffer is concurrently
accessible to the optical-flow queue and the compute queue, with a GPU
semaphore between them.

**Caution:** A zero vector is not a known-correct vector for disoccluded
pixels. The GPU mask is diagnostic and can **increase** ghosting, especially
when temporal history stays enabled. Global confidence-based history reset
still takes precedence. The new mask is OFF by default and should be
compared with an unmasked baseline in real moving scenes.

An RTX 3070 hardware smoke test observed a correct (-6, 0) translation
remaining (-6, 0) after masking. An uncorrelated synthetic frame's
spurious center estimate (14.312, 4.625) was masked to (0, 0).
This only checks data flow, not perceptual image quality.

**True subpixel jitter remains unimplemented.** NVIDIA recommends applying
jitter **during scene rasterization**, not just offsetting the already
rasterized SDL stage texture. The renderer will need a common, subpixel-
accurate stage/view transform before passing matching jitter to NGX.
Sending a fabricated nonzero jitter offset now would be incorrect.

Optical Flow estimates pixel displacement, not geometric depth or true
object velocity. Scratch still lacks real depth, render jitter, robust
scene-cut handling validated on real projects or disocclusion confidence. Temporal DLSS therefore
remains experimental, and is not equivalent to an engine supplying all
the data expected by NVIDIA's recommended integration.

RTX 3070 hardware validation:

- Device supports Optical Flow with a dedicated queue and 1x1/2x2/4x4 grids.
- Synthetic 6-pixel translation: hardware SFIXED5 (-6,0) and GPU RG16F
  conversion (-6,0), at pixel precision.
- Two-queue GPU semaphore bridge: PASS.
- 240-frame SDL optical-flow + temporal integration: PASS.
- 240-frame combined resize/mode/preset switching with temporal enabled: PASS.
- 240-frame Optical Flow synthetic test: 2.135 ms/frame including periodic
  readback and SDL input updates, not Scratch project FPS.

Optional GPU CTest:

```sh
cmake --build build-dlss --target \
  xyo-dlss-optical-flow-bridge xyo-dlss-sdl-color-smoke
ctest --test-dir build-dlss \
  -R 'xyo-dlss-(optical-flow-bridge|sdl-optical-flow-smoke)' \
  --output-on-failure
```

To rebuild the embedded GLSL-to-SPIR-V shader: `python3 tools/embed_dlss_optical.py`.

## Limitations

- **Image-space motion rather than geometric motion.** Opt-in Optical Flow
  generates genuine displacement vectors, but Scratch still has no scene
  depth or authoritative object velocities. Without Optical Flow, the
  zero-vector path remains; temporal history is separately opt-in.
- **No Scratch interpreter/JIT acceleration.** DLSS acts after drawing.
  Projects such as `3D2.sb3` are dominated by script execution and
  typically remain around 1 FPS under the tested configuration.
- **Requested preset vs effective model.** SDK/driver overrides are
  possible. The API indicates the hint was provided, not which model
  was actually selected.
- Only Linux with a compatible NVIDIA GL/Vulkan device and external
  memory/semaphore support is implemented. Failure falls back to OpenGL.
- The current pipeline recycles one Vulkan command buffer after a
  fence. Multi-frame in-flight DLSS is not yet implemented.
- On `480×360 → 1440×1080` (3× linear enlargement), the SDK recommends
  an input resolution of at least `720×540` for all three tested modes:
  the selected `480×360` input is below the recommended range. At
  `480×360 → 960×720`, Performance's optimal input is `480×360`,
  while Quality recommends `640×480`.

## RTX 3070 validation (2026-11-08)

Native NVIDIA NGX initialization, Vulkan image creation, GPU memory
export/import, GPU semaphores, SDL rendering, multiple frames, resize,
full cleanup/reinitialization, and the four-quadrant color/orientation
test all passed. The independent stress test passed 360 alternating-color
frames with periodic readback.

Runtime configuration changes to `XYO_DLSS_MODE` or `XYO_DLSS_PRESET`,
**or changes to stage input/output resolution**, now retire and recreate the
full Vulkan device/NGX pipeline after waiting for outstanding GPU work. A 360-frame stress test
with alternating resolutions (40-frame period), mode changes (60-frame
period), and per-frame input color changes passed on the RTX 3070. A
240-frame K→J→auto preset-switch test also passed. Earlier revisions
could trigger `VK_ERROR_DEVICE_LOST (-4)` because DLSS features were
released after their Vulkan input/output images; resource teardown
order has been corrected. The original intermittent GPU-device-loss
was still reproducible on repeated resizes, so the current release also
restarts the entire pipeline on resize. Three consecutive mixed
240-frame GPU stress runs then passed (720/720 total); switching settings
is intentionally more expensive than steady-state DLSS.

With `XYO_DLSS_PRESET=K` and 480×360→1440×1080, the first controlled
240-frame test measured an average 0.973 ms for NGX GPU execution. J
measured 0.931 ms; SDK default measured 0.925 ms. These are short
hardware-specific measurements, not formal image-quality comparisons.
On the same 480×360→1440×1080 test using Performance mode, the
240-frame NGX GPU dispatch averages were: preset K **0.994 ms**,
preset M **0.927 ms**, and SDK default **0.915 ms**. M is NVIDIA's
recommended preset for Performance and was about 7% faster than K
in this short synthetic test; measure the visual result on actual
projects before selecting a permanent default.


With K at the same output size, 180-frame sample tests measured:

| Mode | NGX GPU average | Total test average |
|---|---:|---:|
| Quality | 0.987 ms | 1.363 ms/frame |
| Balanced | 0.917 ms | 1.386 ms/frame |
| Performance | 0.928 ms | 1.299 ms/frame |

These numbers are not direct game FPS measurements. They include different
setup conditions, and the total test average includes input updates and
periodic readback. Avoid claiming that Performance always wins from this
small dataset.

To build and run an optional real-GPU integration test:

```sh
cmake -S . -B build-dlss \
  -DXYO_NVSDK_DLSS_VK=ON \
  -DXYO_NVSDK_DLSS_SDK_ROOT="$HOME/.cache/xyo-dlss-sdk" \
  -DXYO_DLSS_REGISTER_GPU_TESTS=ON
cmake --build build-dlss --target xyo-dlss-sdl-color-smoke
ctest --test-dir build-dlss -R '^xyo-dlss-sdl-color-smoke$' -V
```

Hardware tests are not part of normal CPU-only CI. Standalone
`tests/render/dlss_vk_*.cpp` files have independent `main()` entrypoints
and are excluded from the regular renderer test source glob.

## RTX 3070 visual/performance comparisons (2026-11-09)

A deterministic 45-frame run of `debug/projects/3D2.sb3` displayed its
**obj importer instruction page** (static text), not the 3D game scene.
The GUI was 960×720, with the stage rendered at 480×360 for the four
upscale variants. The native-resolution (`XYO_RENDER_SCALE=1`) image
was used as a pixel-error reference. This is a text/UI comparison, *not*
a temporal ghosting or motion-quality benchmark.

| Rendering mode | Mean draw ms (frames 3–45) | PSNR vs native |
|---|---:|---:|
| Legacy OpenGL upscale | 0.447 | 15.628 dB |
| NVIDIA NGX DLSS M/Performance, no Optical Flow | 0.588 | 15.680 dB |
| DLSS M/Performance + Optical Flow + history | 1.700 | 15.739 dB |
| DLSS + Optical Flow + history + local cost mask | 1.546 | 15.745 dB |

First two frames are excluded to reduce initialization distortion.
Values are from *one* short run, without confidence intervals. The
DLSS/local-mask PSNR differences are small, and the rasterization
scale changes how fonts are drawn. They do **not** establish that DLSS
or cost masking looks better in moving scenes. The underlying Scratch
script execution time remained around 5.8 ms/frame for all modes.

A dedicated SDL GPU test was also added, with a red opaque 100×80
rectangle moving **6 input pixels per frame** on a uniform background.
The test samples the trailing 6-pixel strip left behind by its motion
(7,560 sampled pixels over 30 frames) and measures excess red above
the expected background. This is not a full perceptual quality metric.

| Moving rectangle mode | Mean residual red, 0–255 |
|---|---:|
| DLSS spatial, Optical Flow off | 0.000 |
| DLSS + Optical Flow, history off | 0.000 |
| DLSS + Optical Flow + temporal history | 0.007 |
| DLSS + Optical Flow + history + cost mask | 0.007 |

Neither the temporal mode nor the local cost mask showed a material
improvement on this opaque shape. Alpha blending, depth discontinuities
and dense moving geometry are not exercised by this test. It registers
the two spatial/temporal moving-square cases as optional real-GPU CTests.

**Diagnostic tip:** `XYO_DLSS_REAL_DEBUG=1` logs the SDL renderer
type, OpenGL context, stage size and render scale, plus one-time
`draw()` state. This was used to confirm the **OpenGL renderer and
real NGX** activation on RTX 3070 after a full JIT-enabled rebuild.
A short, deterministic `XYO_EXIT_FRAMES=45` run is preferable to a
timed run that could exit before rendering is initialized.

### Alpha and occlusion safety (2026-11-09)

**Opt-in `XYO_DLSS_ALPHA_SAFE=1`** identifies visible Scratch
sprites with a partial ghost effect (0 < ghost < 100) **or a decoded
PNG/SVG costume containing a stable region of partial alpha**. When
such a sprite intersects the stage, the renderer passes a per-frame
`forceHistoryReset` flag to NGX. Fully opaque, hidden, fully
offstage or ghost-100% sprites do not trigger it.

The RGBA pixel scan runs **once when a costume skin is decoded**, including
disk-cache hits. The result is cached as a boolean in the skin. For
stable partial-opacity interiors, a 3x3 window must contain alpha 8..247,
with neighbors within 24 of the center pixel. This excludes one-pixel
antialiasing fringes on opaque artwork, but may miss very thin partial
alpha strokes. The scan runs only when both `XYO_DLSS_REAL=1`
and `XYO_DLSS_ALPHA_SAFE=1` are active. No extra RGBA scan
is performed in the default renderer.

```sh
XYO_DLSS_REAL=1 \
XYO_DLSS_OPTICAL_FLOW=1 \
XYO_DLSS_TEMPORAL=1 \
XYO_DLSS_ALPHA_SAFE=1 \
XYO_RENDER_SCALE=0.5 \
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
  "$HOME/.cache/xyo-dlss-build/play/xyo" play examples/fx_demo.sb3 --scale 2
```

On RTX 3070 the real `fx_demo.sb3` Scratch project activated
the safety mode while NVIDIA NGX and SDL OpenGL were running, and
exited successfully after eight frames.

The GPU test uses a moving 50%-alpha rectangle behind a static opaque
foreground occluder, moving 6 input pixels every frame. After 90 frames,
the residual red channel in the trailing-background strip, on a 0–255
scale, was:

| Configuration | Mean residual red |
|---|---:|
| DLSS + Optical Flow + temporal history | 0.037 |
| Same with per-frame alpha-safe history reset | 0.008 |
| DLSS without temporal history | 0.008 |

Three independent runs returned the same displayed scores. This is
about 78% less trailing red on this **synthetic** scene, not a general
perceptual quality guarantee or real-game FPS result. A Python GPU
CTest compares both configurations and checks that the opt-in safe
path does not increase residual ghosting.

**Limitations:** PNG/SVG broad partial-alpha interiors and Scratch
ghost modulation are now detected, but one-pixel strokes, pen-layer
translucency and alpha introduced by post-processing effects are not.
Any eligible on-screen sprite forces a **whole-frame** reset even when
stationary; this can reduce temporal reconstruction detail. The
per-frame metadata/viewport scan costs O(number of drawables) when
enabled. NVIDIA SDK 310.9.16 documents `pInTransparencyMask`
as unused/reserved, so per-region NGX history reset remains unsupported
by this integration. True geometric depth and rasterization jitter are
also missing. The alpha-safe flag remains OFF by default.

GPU-verified synthetic Scratch projects, with no ghost effects:

| Costume/state | Temporal reset | Test |
|---|---|---|
| PNG alpha 255 | No | PASS |
| PNG interior alpha 128 | Yes | PASS |
| Opaque PNG with 1px antialiased alpha border | No | PASS |
| PNG alpha 128 entirely outside stage | No | PASS |
| Opaque SVG | No | PASS |
| SVG fill-opacity=0.5 | Yes | PASS |
| Opaque SVG circle with antialiased edge | No | PASS |
| SVG alpha 128 with sprite hidden | No | PASS |

Real NVIDIA NGX on RTX 3070 passed all eight cases. Including the
standalone CPU classifier, aggregate unit suite, SDL renderer suite and
15 GPU regression tests, targeted CTest passed 18/18.

## Future improvements

- Validate Optical Flow confidence and temporal-history resets on moving
  actual Scratch projects. The R8 cost map and global reset heuristic are
  already implemented, but not calibrated for occlusion or translucent
  objects.
- Depth semantics for non-3D Scratch content and suitable per-frame jitter.
- Multi-buffering GL/Vulkan images and synchronization to reduce per-frame
  Vulkan fence stalls.
- Longer tests with moving 3D2 scenes, not just colored synthetic patterns.

NVIDIA SDK/API reference: https://github.com/NVIDIA/DLSS
Review NVIDIA licensing requirements before redistributing SDK components.