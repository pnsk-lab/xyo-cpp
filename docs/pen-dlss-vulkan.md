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

### GPU-local current-color bias mask experiment (2026-11-09)

The opt-in `XYO_DLSS_ALPHA_BIAS=1` path creates an actual
`VK_FORMAT_R8_UNORM` mask image on the Vulkan compute queue,
and provides it to NGX using the SDK's optional
`pInBiasCurrentColorMask` parameter. This is distinct from
`pInTransparencyMask` (reserved) and from the **entire-frame**
`XYO_DLSS_ALPHA_SAFE=1` reset described above.

- Scratch sprites with a partial ghost effect or an intrinsically
  semi-transparent PNG/SVG costume are identified using the same
  cached alpha metadata.
- Their on-screen rotated **axis-aligned bounding boxes** are converted
  to input-resolution rectangles, clipped to the stage and dilated by
  two input pixels. This is a coarse approximation, **not** a
  per-pixel alpha/disocclusion mask.
- Only up to 64 rectangles are uploaded per frame via a small host-coherent
  SSBO (1 KiB). If more are needed, the renderer falls back to a
  conservative full-frame history reset.
- A Vulkan compute shader produces the full R8_UNORM mask **on GPU**.
  The mask has strength 1.0 inside marked regions, 0.0 outside.
  The strength can be set to 0–1 using
  `XYO_DLSS_ALPHA_BIAS_STRENGTH`.
- It uses sampled/shader-read layout before NGX Evaluate, with GPU
  pipeline barriers and no full-stage CPU image readback. When no eligible
  translucent region is visible, the compute pass is skipped and NGX
  receives no bias input for that frame.
- The mask is only allocated/created if the experimental flag is on;
  `XYO_DLSS_ALPHA_SAFE=1` can still independently force a reset.

**Hardware verification (RTX 3070):** A standalone Vulkan test copied
the GPU-generated 64×48 mask into a staging buffer and validated the
two requested rectangles: 208 pixels at 255 and 2,864 pixels at zero,
with **zero pixel errors**. A real `xyo play examples/fx_demo.sb3`
run confirmed the native NVIDIA NGX pipeline and an active R8 bias
mask with one sprite rectangle.

**Measured efficacy:** The moving half-transparent rectangle /
occlusion scene was run for 90 frames, DLSS Performance/M,
at 480×360 → 960×720, with Optical Flow enabled and temporal history
retained. The residual red metric was:

| NGX configuration | Residual red |
|---|---:|
| Temporal history, no bias mask | 0.037 |
| Local rectangle, R8 mask strength 1 | 0.037 |
| Full-frame R8 mask strength 1 | 0.037 |
| Full-frame R8 mask strength 0 | 0.037 |
| Forced full-frame history reset | 0.008 |

Captured full-frame BMP outputs for **local bias**, **full bias 1**
and **full bias 0** were byte-for-byte equal to the no-mask temporal
baseline on the last captured frame. By contrast, forcing history
reset changed pixels. The tested NGX/model/driver combination did not
show any useful bias-mask response **in this particular scene**.
This does not prove other models, assets or SDK integrations ignore
the input. Do not enable bias masking for claimed quality/performance
improvements without additional evidence.

Two new hardware CTests verify the GPU image's exact contents and
SDL/NGX integration; another cross-configuration diagnostic CTest
records pixel output differences without asserting the bias helps.
Together with all other DLSS and standard renderer regressions,
**20/20** targeted CTests passed. An additional 240-frame test with
alternating NGX quality presets and modes completed successfully while
the alpha-occlusion bias experiment was enabled.

The integration uses `src/render/dlss_vk_bias_mask.cpp` and
`src/render/shaders/dlss_bias_mask.comp`; its embedded SPIR-V
is generated using `tools/embed_dlss_bias_mask.py`.
The GPU mask is experimental and **OFF by default**. For now,
`XYO_DLSS_ALPHA_SAFE=1` remains the better-validated
ghosting mitigation, with the whole-frame detail tradeoff noted above.

### Scratch rigid sprite geometry motion (2026-11-09)

`XYO_DLSS_RIGID_MOTION=1` is an **opt-in experimental**
extension to `XYO_DLSS_OPTICAL_FLOW=1` that combines
NVIDIA hardware Optical Flow with *known Scratch sprite positions*.

The SDL renderer caches each drawable's costume identity, current
position, size, direction, rotation style, opaque coverage and bounding
rectangle **at the last rendered frame**. It computes the exact
current-to-previous input-pixel translation for eligible sprites.
The GPU compute shader expands Optical Flow to RG16F as before, then
overwrites vectors **only inside the sprite's conservative inner
rectangle**. Background and other regions retain Optical Flow.

Eligibility is deliberately restrictive:

- Every pixel in the decoded PNG/SVG skin must have alpha=255, using
  a one-time opt-in alpha scan; any graphic effect disqualifies it.
- Only unrotated, unchanged-size, unchanged-direction,
  unchanged-costume sprites are supported. Changes invalidate motion.
- Movement must be an integer number of input render pixels
  (to match SDL's integer SDL_RenderCopyEx rasterization), have
  maximum 32px per axis, and be nonzero.
- Any overlap with a different visible sprite in the current or
  previous frame disables that candidate. Rectangle edges are inset
  by three input pixels to avoid sampling/rasterization errors.
- At most 64 rectangles are forwarded each frame. Others retain
  hardware Optical Flow. No software full-frame motion readback.

The extra 2KiB host-coherent SSBO and compute descriptor are allocated
only when rigid motion is enabled. The GPU shader reads rectangle
metadata per input pixel; this has a measurable cost even with few
sprites. Set both flags to experiment:

```sh
XYO_DLSS_REAL=1 \
XYO_DLSS_OPTICAL_FLOW=1 \
XYO_DLSS_TEMPORAL=1 \
XYO_DLSS_RIGID_MOTION=1 \
XYO_RENDER_SCALE=0.5 \
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
"$HOME/.cache/xyo-dlss-build/play/xyo" play examples/fx_demo.sb3 --scale 2
```

**Verified behavior on RTX 3070:** A Vulkan cross-queue readback test
observed Optical Flow (-6,0) outside the override rectangle and the
exact requested (-11,-3) vector at its center. A synthetic Scratch
`.sb3` project using a forever-loop with
`motion_changexby 6` produced an actual renderer/NGX log
of one sprite rectangle at (-6,0), with 28 frames executed
successfully. The same project run with the flag OFF produced no
override. CPU unit tests reject changed costumes, large teleports,
sprite overlap and fractional moves.

**One-run image comparison** at 960×720, after 28 frames of the
same synthetic moving opaque sprite, relative to
`XYO_RENDER_SCALE=1` output:

| Mode | PSNR vs native | Median draw ms (frames 3–28) |
|---|---:|---:|
| NGX + Optical Flow, temporal | 50.05 dB | 1.463 |
| Same plus rigid motion | 49.97 dB | 1.506 |

The two output images differed at 773 pixels. This simple scene
demonstrates correct integration but **does not show an image quality
or speed improvement**. It is not a scene-depth or optical-flow
replacement. Genuine per-pixel geometric coverage, subpixel
render jitter, occlusion and conventional game-engine depth remain
unavailable. The feature stays OFF by default.

A new pure-CPU CTest, real cross-queue Vulkan override CTest and
real `xyo play` Scratch project CTest were registered.
All **23/23** targeted DLSS/renderer tests passed.

### Experimental affine motion for opaque Scratch sprites (2026-11-09)

`XYO_DLSS_AFFINE_MOTION=1` extends the previous translation
experiment to rotating and scaling 2D Scratch sprites. The renderer
captures **actual SDL-rounded** destination rectangles, integer
rotation pivots and angles at successive rendered frames.
It computes the current-to-previous affine mapping
`P * inverse(C)`, where C and P map local costume UV coordinates
to the current and previous input render pixels.

The hardware Optical Flow conversion shader supports a 96-byte
std430 structure per sprite (up to 64). Inside eligible rotated
sprite polygons it overrides the RG16F motion vector using the
affine transform, and retains Optical Flow outside the polygon,
including inside the exterior corners of the sprite AABB.

The policy accepts only fully opaque (alpha 255 everywhere),
effect-free costumes with the same costume and rotation style on
successive frames, no overlap with another visible sprite on
either frame, finite SDL raster coordinates, sizes between 8 and
8,192 pixels, scale ratio 0.75–1.35, and motion not exceeding 32px
per coordinate at the bounding-box corners. Unsupported sprites
fall back to hardware Optical Flow. Transparency, flips, disocclusion,
per-pixel depth and render jitter remain unimplemented. The
experiment stays OFF by default.

```sh
XYO_DLSS_REAL=1 \
XYO_DLSS_OPTICAL_FLOW=1 \
XYO_DLSS_TEMPORAL=1 \
XYO_DLSS_AFFINE_MOTION=1 \
XYO_RENDER_SCALE=0.5 \
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
"$HOME/.cache/xyo-dlss-build/play/xyo" play examples/fx_demo.sb3 --scale 2
```

**RTX 3070 validation:** A 45-degree synthetic rotated quad
overrode motion only inside the polygon; the external AABB corners
retained the Optical Flow translation. A real 20-frame Scratch
project rotating 15 degrees per frame produced the
current-to-previous 2x2 rows (0.9659, 0.2588;
-0.2588, 0.9659); a separate project growing size by
2 percent per frame yielded (0.9800, 0; 0, 0.9800).
No geometry override occurred with the feature disabled.

**Preliminary image comparison:** Single runs of 20 frames,
960x720 output, DLSS Performance/M + hardware Optical Flow,
native 960x720 as PSNR reference, excluding first two
draw timings:

| Scene | OF-only PSNR | Affine PSNR | Median draw ms (OF -> affine) |
|---|---:|---:|---:|
| +15 degrees/frame | 50.493 dB | 50.633 dB | 1.594 -> 1.717 |
| +2% sprite size/frame | 38.666 dB | 38.773 dB | 1.595 -> 1.650 |

The differences are small and the experimental path adds work,
so no robust visual-quality gain is established. New CPU, Vulkan
GPU cross-queue and real Scratch GPU tests cover the feature.
Rebuild the embedded motion shader with
`python3 tools/embed_dlss_optical.py` when editing
`src/render/shaders/dlss_optical_vectors.comp`.

### Conservative opaque-pixel coverage in affine motion (2026-11-09)

`XYO_DLSS_OPAQUE_COVERAGE=1` extends
`XYO_DLSS_AFFINE_MOTION=1` to some PNG/SVG sprites
containing transparency, without reprojecting the transparent pixels.
The decoded RGBA costume is scanned **once per skin** and a
**128×128 conservative bitmask** is cached. A tile is considered opaque
only if *all* original texels it covers, plus a one-source-texel guard
border, have alpha 255. This avoids interpolating transparent or
antialiased edges as geometric motion.

The affine compute shader maps each output pixel center back into
the current sprite's original costume UV space. If the coverage bit
is clear, it leaves the hardware Optical Flow value intact; if
opaque, it uses the current-to-previous affine vector. The ordinary
NGX temporal path is unchanged. The feature only runs if both
`XYO_DLSS_AFFINE_MOTION` and
`XYO_DLSS_OPAQUE_COVERAGE` are 1 and real NVIDIA
DLSS / Optical Flow are active. It is OFF by default.

```sh
XYO_DLSS_REAL=1 \
XYO_DLSS_OPTICAL_FLOW=1 \
XYO_DLSS_TEMPORAL=1 \
XYO_DLSS_AFFINE_MOTION=1 \
XYO_DLSS_OPAQUE_COVERAGE=1 \
XYO_RENDER_SCALE=0.5 \
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
"$HOME/.cache/xyo-dlss-build/play/xyo" play examples/fx_demo.sb3 --scale 2
```

**Limitations and performance:** This is a *conservative
128×128 coverage approximation*, not a lossless full-resolution
alpha texture: narrow solid strokes and small opaque islands can
be rejected when the original costume exceeds 128 pixels.
The one-texel guard also intentionally excludes sprite borders.
Whole-frame temporal reset is not performed by this feature.
It cannot solve depth, overlapping sprites, pen-layer opacity,
GPU post-effects, or texture sampling differences at all scales.
The enlarged GPU rectangle SSBO carries 512 uint32 mask words per
eligible rectangle (2,144 bytes per entry); only individual
requested mask words are read per pixel by the shader. The
generation is once per costume, not once per rendered frame.

**RTX 3070 verification:**

- The pure-CPU test creates a 64×64 opaque ring with a transparent
  hole and checks opaque interior tiles, transparent-hole exclusion,
  border guard and half-alpha rejection.
- The Vulkan cross-queue readback compares the hardware Optical Flow
  against a 45-degree affine override with an opaque central patch.
  The patch center gets the affine vector; the transparent area and
  AABB corners retain Optical Flow.
- Real native NVIDIA NGX `xyo play` ran a generated
  Scratch project rotating a PNG with a transparent hole. With
  coverage enabled, its affine rectangle was active; with coverage
  disabled, the partially transparent sprite was rejected.
- All **28/28** targeted DLSS/GPU/renderer CTests passed.

**20-frame rotating PNG reference, 960×720 output:** PNG native
960×720 rendering is the comparison target. In each of three
independent runs, Optical Flow-only PSNR was **42.6344 dB**,
and conservative-coverage affine PSNR **43.1266 dB**
(+0.4922 dB). First-run median draw times were 2.2413 ms
for Optical Flow and 2.2392 ms with coverage. Subsequent
timings varied (OF: 2.1127 and 1.8121 ms; coverage:
1.7092 and 2.1379 ms); there is no established speed
improvement. This is a controlled synthetic scene, not a
general quality guarantee. The mode remains experimental.

Implementation:
`src/render/dlss_opaque_coverage.hpp`,
`src/render/dlss_rigid_motion.hpp`,
`src/render/shaders/dlss_optical_vectors.comp`,
and `sdl_renderer.cpp`. Tests:
`tests/unit/dlss_opaque_coverage_unit.cpp`,
`tests/render/dlss_vk_optical_flow_bridge_smoke.cpp`,
and `tests/render/dlss_coverage_project_smoke.py`.

### Real SDL subpixel raster jitter for temporal NGX (2026-11-09)

The opt-in `XYO_DLSS_RASTER_JITTER=1` path applies a repeatable
**8-sample Halton 2×3** sequence in **low-resolution input pixel space**
to the actual SDL sprite and pen-layer rendering. It is enabled
only when `XYO_DLSS_REAL=1`,
`XYO_DLSS_OPTICAL_FLOW=1` and
`XYO_DLSS_TEMPORAL=1` are active. Other configurations
preserve the previous integer-based raster path.

It uses `SDL_RenderCopyExF` for rotating sprites and
GPU-backed pen-layer blits, `SDL_RenderCopyF` for regular
pen layers. The backdrop uses the same sprite draw route.
The renderer stores current and previous jitter on successful
NGX frames, sends the exact current jitter to
`NGX_VULKAN_EVALUATE_DLSS_EXT` as
`InJitterOffsetX/Y`, and subtracts
`(previousJitter-currentJitter)` from NVIDIA Optical
Flow vectors when converting SFIXED5 into RG16F. Known geometric
affine vectors are already unjittered and are therefore not
corrected a second time. Resizing resets the jitter sequence.
If NGX fails, the renderer re-rasterizes the frame without
jitter for the conventional GL upscaler fallback. When
temporal reuse is explicitly disabled, the jitter path is OFF.

```sh
XYO_DLSS_REAL=1 \
XYO_DLSS_OPTICAL_FLOW=1 \
XYO_DLSS_TEMPORAL=1 \
XYO_DLSS_RASTER_JITTER=1 \
XYO_RENDER_SCALE=0.5 \
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
"$HOME/.cache/xyo-dlss-build/play/xyo" play examples/fx_demo.sb3 --scale 2
```

The first three offsets in input pixels are:
(0.0000, -0.1667), (-0.2500, 0.1667),
and (0.2500, -0.3889).

For real pre-NGX SDL raster diagnostics, specify
`XYO_DLSS_RASTER_INPUT_CAPTURE=/path/input.ppm`.
The file is written **only when requested**, at frame three by
default, or `XYO_DLSS_RASTER_CAPTURE_FRAME=N`.
This is an expensive GPU readback intended solely for testing.

**RTX 3070 validation:**

- The NVIDIA hardware Optical Flow readback test confirmed
  a synthetic correction of (0.25, -0.125) yields
  (-6.250, 0.125) instead of the uncorrected (-6, 0)
  RG16F motion vector, and baseline behavior is unchanged.
- Real NGX Scratch playback completed 32 frames and logged
  the first three actual jitter offsets.
- The identical rotating PNG Scratch project produced
  **79 changed input image pixels (237 RGB bytes)** at
  frame three between jitter OFF and ON. Thus the SDL
  input raster *actually changed*, not just the NGX
  metadata. Disabling temporal history restored input
  byte equality with the no-jitter baseline.
- New standalone CPU and GPU tests plus all existing
  DLSS and renderer regressions passed **31/31**.

**One-run image comparison** after 24 frames, output 960×720,
vs native 960×720 renderer. Median draw times exclude the
initial two frames:

| Scene | Optical Flow PSNR | With jitter PSNR | Draw median ms (OF → jitter) |
|---|---:|---:|---:|
| Opaque +6px/frame sprite | 50.08 dB | 48.97 dB | 2.027 → 2.088 |
| Rotating PNG with transparent hole | 42.37 dB | 42.58 dB | 2.034 → 2.518 |

The first scene *regressed*, so do **not** enable this
experiment by default. These times and PSNRs are single
runs and are not sufficient to establish general quality
or speed improvements.

**Important limitations:** This is **fractional destination
rasterization of SDL sprite and pen-layer textures**,
not a conventional 3D camera-projection jitter.
The pen's internal stroke rasterizer is not resampled
at jittered positions; only the persistent pen image's
display transform is shifted. Default SDL texture
sampling and integer destination dimensions also limit
true subpixel information. The static white stage clear
does not shift. NGX still has only a dummy constant-depth
texture, while overlapping sprites can invalidate motion
vectors and some scene resets remain approximate. Accurate
subpixel costume sampling, pen primitive jitter and
depth/occlusion support would be required for a
production-quality integration.

Relevant files: `src/render/dlss_jitter.hpp`,
`src/render/sdl_renderer.cpp`,
`src/render/dlss_gl_pipeline.cpp`,
`src/render/dlss_vk_motion_converter.cpp`, and
`src/render/shaders/dlss_optical_vectors.comp`.
Tests: `tests/unit/dlss_jitter_unit.cpp`,
`tests/render/dlss_vk_optical_flow_bridge_smoke.cpp`,
`tests/render/dlss_raster_jitter_smoke.py`.

### Adaptive jitter suppression and pen-layer linear sampling (2026-11-09)

Both additions below are optional extensions of the real
`XYO_DLSS_RASTER_JITTER=1` path. Neither changes the
default renderer or disables the underlying NVIDIA Optical Flow.

**Adaptive jitter** (`XYO_DLSS_ADAPTIVE_JITTER=1`) samples
visible Scratch sprite centers each rendered frame in *input pixel*
coordinates. `XYO_DLSS_JITTER_MAX_MOTION` sets the maximum
accepted per-axis motion, default 3 input pixels (valid values
0.1–32). The actual jitter becomes (0,0) on fast translations,
abrupt costume changes, changed visible-sprite count, scaling
changes over 20 size points, or orientation changes over 30 degrees.
The first observation after initialization or resize also skips
jitter because no movement estimate exists. Stable scenes resume
the Halton sequence without advancing its phase while suppressed.
Each successful NGX frame records the **actual** applied jitter,
so the GPU Optical Flow correction remains synchronized with the
current/previous frame. Rejected NGX frames reset the tracker.

**Pen-layer sampling** (`XYO_DLSS_JITTER_PEN_FILTER=1`)
temporarily switches the current SDL pen-display texture's sampler
to `SDL_ScaleModeLinear` during fractional
`SDL_RenderCopyF/SDL_RenderCopyExF` raster jitter.
Immediately after the copy, the previous texture filter is
restored. The existing nearest-neighbor path and normal Pen APIs
are unchanged when the flag is absent. This is fractional **display
sampling of a persistent pen texture**; it is *not* per-stroke
re-rasterization at jittered sample locations.

Sample configuration:

```sh
XYO_DLSS_REAL=1 \
XYO_DLSS_OPTICAL_FLOW=1 \
XYO_DLSS_TEMPORAL=1 \
XYO_DLSS_RASTER_JITTER=1 \
XYO_DLSS_ADAPTIVE_JITTER=1 \
XYO_DLSS_JITTER_MAX_MOTION=3 \
XYO_DLSS_JITTER_PEN_FILTER=1 \
XYO_RENDER_SCALE=0.5 \
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
"$HOME/.cache/xyo-dlss-build/play/xyo" play examples/play_demo.sb3 --scale 2
```

**Hardware verification (RTX 3070):** Added
`tests/render/dlss_adaptive_jitter_smoke.py` and
`tests/render/dlss_pen_jitter_smoke.py` as GPU CTests.
The first generates a +6 input-pixel/frame sprite script and
checks that its adaptive-jitter input image is **byte-identical**
to the unjittered input; the GPU path and motion-suppression
diagnostic are both active. A separate CPU unit checks the first
frame, translation, costume changes, sprite appearance/disappearance,
resize and resumed stable frames. The pen smoke generates a
Scratch Pen-down/move script: SDL reports original filter mode
0 (nearest), switches to linear for the fractional jitter copy,
then restores the old mode. The pre-NGX input differs in
56 RGB bytes from the unfiltered input on the captured frame.

**Synthetic scene quality vs native 960×720** (24-frame runs,
NGX Performance/M + Optical Flow):

| Scene/mode | PSNR |
|---|---:|
| Opaque moving +6px/frame, no jitter | 50.08317 dB |
| Same, unconditionally jittered | 48.9727 dB |
| Same, motion-adaptive jitter | **50.08317 dB** |
| Pen drawing, jitter with default pen sampler | 37.43147 dB |
| Same, jitter with linear pen sampler | **41.49635 dB** |

For moving sprites, the adaptive output and no-jitter image
were **pixel-identical** after withholding jitter on the
unknown-motion first frame. The pen scene's +4.06488 dB PSNR
gain was reproduced in three separate deterministic 24-frame
runs (each 37.43147 → 41.49635 dB). Median draw times fluctuated
between runs and did not establish a performance benefit.
The extra per-frame CPU tracking cost and filtered pen copy
may slow down some projects.

The moving sprite's flat axis-aligned texture **can rasterize
identically at fractional offsets** even without adaptation;
this is a limitation of SDL texture rasterization, not proof
that temporal jitter metadata is harmless. The pen-path quality
result is also specific to this test scene; filtering can soften
pixel art and cause altered edge blending. These modes remain
**OFF by default** and are not substitutes for a real
jittered pen-stroke rasterizer or scene-depth/occlusion information.

### Analytic jittered Pen line/point re-rasterization (2026-11-09)

`XYO_DLSS_PEN_PRIMITIVE_REPLAY=1` is an **opt-in
experimental, OFF-by-default** improvement for projects using
real NGX plus `XYO_DLSS_RASTER_JITTER=1`. Rather than
only shifting the persistent Pen texture, it re-rasterizes the
original Scratch Pen round-capped line and point commands at the
current fractional input-pixel sample position.

After each observed `penClear`, the renderer records
Pen line/point commands in a bounded ledger and computes analytic
capsule coverage into a separate, low-resolution straight-RGBA
SDL texture. This texture is composited **without a second jitter
offset**. The original Pen FBO and CPU pixels are *unchanged*;
collision, stamping and persistence continue using their original
Pen image.

The replay is safe only for a fully known drawing history:
a stamp, a restored Pen image, a missing clear, invalid numeric
values, or reaching the **512 stroke** limit disables replay until
the next clear. Each frame also has a **one-million sample visit**
budget and a **1,048,576 input pixel** limit; exceeding either
falls back to the original texture. Requires
`XYO_PEN_FSR=0`. Display-only linear Pen filtering
is bypassed when analytic replay succeeds; there is no
double filtering or double jitter. An optional mode has no
effect on normal/default drawing.

```sh
XYO_DLSS_REAL=1 \
XYO_DLSS_OPTICAL_FLOW=1 \
XYO_DLSS_TEMPORAL=1 \
XYO_DLSS_RASTER_JITTER=1 \
XYO_DLSS_PEN_PRIMITIVE_REPLAY=1 \
XYO_PEN_FSR=0 \
XYO_RENDER_SCALE=0.5 \
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
"$HOME/.cache/xyo-dlss-build/play/xyo" play examples/play_demo.sb3 --scale 2
```

**RTX 3070 test:** A generated real Scratch project
with Pen clear, Pen down and repeated x movement produced a
positive native NGX replay log. The SDL pre-NGX input changed
relative to legacy Pen texturing. Identical projects with
a Pen stamp or without a preceding clear **did not replay**:
their input captures were byte-for-byte identical to the
legacy path. A standalone CPU unit validates deterministic
capsule sampling, fractional motion, alpha blending, and
overflow guards. The targeted DLSS/SDL CTest suite passed
**35/35**, including **29 GPU tests**.

**Single synthetic Pen scene, 24 frames, 960×720 output:**
native 960×720 is the PSNR reference; other modes use NGX
Performance/M and NVIDIA Optical Flow:

| Pen display approach | PSNR |
|---|---:|
| Fractional texture shift, nearest sampler | 37.43147 dB |
| Fractional texture shift, linear sampler | 41.49635 dB |
| Analytic jittered Pen primitive replay | **42.96385 dB** |

This is a **+1.46750 dB** difference relative to the previous
linear sampler **on this scene only**. Three deterministic
24-frame trials returned the same replay PSNR. Replay median
draw time varied from **2.2264 to 2.5692 ms**;
single-run linear comparison was **2.2466 ms**.
There is no established performance benefit. Results do not
generalize to arbitrary Pen projects and replay differs from
native renderer sampling under some transparency/oversampling
situations. The mode remains OFF by default.

Implementation: `src/render/dlss_pen_replay.hpp`,
`src/render/sdl_renderer.cpp`. Tests:
`tests/unit/dlss_pen_replay_unit.cpp` and
`tests/render/dlss_pen_primitive_smoke.py`.

### GPU-accelerated jittered Pen replay (2026-11-09)

`XYO_DLSS_PEN_REPLAY_GPU=1` additionally requests a
**direct GPU raster path** for the experimental
`XYO_DLSS_PEN_PRIMITIVE_REPLAY=1` mode. Without this
flag, the CPU analytic replay from the previous section remains
the default experimental path.

Instead of replaying strokes to an RGBA CPU buffer and uploading
the whole image each frame, the new path reuses the existing GL 3.3
instanced Pen-capsule vertex/fragment program. It uploads a bounded
10-float record for each stroke, adds the current subpixel jitter
as a vertex shader uniform, and renders directly into a dedicated
SDL OpenGL render-target texture through a shared OpenGL context.
The texture is composited once, vertically flipped and with
**premultiplied alpha blending**, matching the native Pen GPU FBO
convention. It does not touch the persistent Pen framebuffer,
collision data, stamps or saves.

The normal persistent-Pen draw explicitly sets the shared shader's
jitter uniform to (0,0), preventing a previous DLSS replay
from leaking into subsequent Scratch Pen commands. The replay
FBO is detached after drawing; old framebuffer, viewport, shader,
blend, culling, stencil and clear state are restored. A GPU/SDL
context synchronization (`glFinish`) is currently required,
and may limit performance.

Requires a shared GL context, working GL instancing and framebuffer
support, and the same known Pen history and bounds as the CPU path:
**512 strokes, 1,000,000 estimated raster sample visits, at most
1,048,576 input pixels**, invalidated on stamp/restore or missing
Pen clear. The GPU path preflights geometry before rendering. If
GPU setup, budget or draw is unsupported, it falls back to the
CPU replay; if replay as a whole is ineligible, it uses the
persistent Pen display as before. The CPU replay and the new GPU
path both remain OFF in normal/default rendering.

```sh
XYO_DLSS_REAL=1 \
XYO_DLSS_OPTICAL_FLOW=1 \
XYO_DLSS_TEMPORAL=1 \
XYO_DLSS_RASTER_JITTER=1 \
XYO_DLSS_PEN_PRIMITIVE_REPLAY=1 \
XYO_DLSS_PEN_REPLAY_GPU=1 \
XYO_PEN_FSR=0 \
XYO_RENDER_SCALE=0.5 \
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
"$HOME/.cache/xyo-dlss-build/play/xyo" play examples/play_demo.sb3 --scale 2
```

**RTX 3070 checks:** Real NVIDIA NGX with a generated Scratch Pen-down
line-drawing project logged active GPU replay and completed 18 frames.
At captured frame 5, GPU and CPU replays differed in only
**2 RGB bytes**. A control suite added
`tests/render/dlss_pen_gpu_replay_smoke.py`;
it verifies the GPU path is active only for eligible Pen histories,
the shader input matches CPU closely, and Pen stamp or missing
initial clear falls back to byte-identical conventional input.
Full targeted DLSS/renderer tests passed **36/36**,
including **30 GPU-dependent tests**.

**24-frame synthetic Pen project, output 960×720:**

| Mode | PSNR vs native | Median draw ms (runs 1 / 2 / 3) |
|---|---:|---|
| CPU analytic replay | **42.96385 dB** | 1.8388 / 2.1765 / 1.9196 |
| GPU instanced replay | 42.17704 dB | 1.5362 / 1.7718 / 2.4761 |

Both PSNR results were exactly reproducible over three runs,
but GPU final output was **0.79 dB worse** than CPU replay in
this scene despite nearly identical captured input on frame 5.
The NVIDIA temporal reconstruction can amplify tiny
per-frame raster or synchronization differences. The draw
timings are variable and do **not** prove a consistent speedup.
This is not production-ready and remains an opt-in experiment.

Implementation in `src/render/sdl_renderer.cpp`
(uses the GL Pen record shader); cost guard in
`src/render/dlss_pen_replay.hpp`.

### Shared-GL GPU fence and per-frame raster diagnostics (2026-11-09)

Experimental `XYO_DLSS_PEN_REPLAY_SYNC=fence` replaces
`glFinish` in the optional GPU Pen replay path. The producer
context inserts `glFenceSync`, calls `glFlush`, and
the shared SDL consumer context uses `glWaitSync` before
sampling the texture. A failed fence creation falls back to glFinish.
**The default remains glFinish.**

Real RTX 3070/NVIDIA NGX tests with **16 and 120 frames** confirmed
fence and glFinish produce **byte-identical pre-NGX and final images**.
The regression test is
`tests/render/dlss_pen_gpu_fence_smoke.py`.

Debug-only `XYO_DLSS_RASTER_INPUT_SERIES=/path/prefix`
writes the first 32 low-resolution pre-NGX input frames
(`prefix-1.ppm` ... `prefix-32.ppm`).
With no variable set there is no additional readback.
CPU versus GPU Pen replay yielded **7/32 byte-identical frames**;
frames 1–4 matched, frame 5 differed by 1 pixel / 2 RGB bytes,
and frame 32 by 401 pixels / 716 RGB bytes. The largest
per-channel difference over 32 frames was 2 (8-bit levels),
while the final 32-frame NGX output differed in 3,303 pixels.
This shows single-frame input comparisons are inadequate;
the cause of final output divergence is not yet isolated.
The new
`tests/render/dlss_pen_gpu_temporal_diagnostic.py`
automatically measures differences over all 32 frames.

Eight independent 120-frame ABBA/BAAB runs (first four excluded)
reported average per-run median draw times of
**1.8725 ms for glFinish** and **1.8477 ms for fence**,
with average per-run p95s **3.9191** and **3.8182 ms**.
These differences are smaller than run-to-run variance:
**no consistent speedup has been demonstrated**. Vulkan/NGX
still have additional synchronization. Both GPU replay and fence
remain opt-in, with no changes to the default Pen path.

### FP64 fragment-coverage Pen replay experiment (2026-11-09)

`XYO_DLSS_PEN_REPLAY_PRECISE=1` enables a **third,
opt-in** Pen reconstruction variant when the prior
`XYO_DLSS_PEN_PRIMITIVE_REPLAY=1` and
`XYO_DLSS_PEN_REPLAY_GPU=1` flags are already set.

The existing GPU shader interpolates segment-local coordinates
across its Pen capsule and evaluates an approximate distance
in `float`. Capturing 32 pre-NGX inputs found the
CPU/GPU error restricted to a 4-pixel-high band along the
moving stroke: almost all color differences were ±1 level,
with no blue-component mismatch.

The new **GLSL 4.00** fragment program passes the original
per-stroke attributes through **flat varyings**, uses
`gl_FragCoord` to recover each raster pixel center,
and evaluates capsule projection, distance and fractional
coverage in `double` before generating
premultiplied RGBA. That matches the analytic CPU replay's
coordinate model without relying on interpolated UVs.
The separate program is compiled **only on request** and
falls back to standard GPU replay if the GPU/driver cannot
compile or link it. The persistent GL Pen shader is
unchanged except for unused flat attributes, and the
persistent Pen image, sync mode and NGX motion-vector paths
are unaffected. This mode remains **OFF by default**.

```sh
XYO_DLSS_REAL=1 \
XYO_DLSS_OPTICAL_FLOW=1 \
XYO_DLSS_TEMPORAL=1 \
XYO_DLSS_RASTER_JITTER=1 \
XYO_DLSS_PEN_PRIMITIVE_REPLAY=1 \
XYO_DLSS_PEN_REPLAY_GPU=1 \
XYO_DLSS_PEN_REPLAY_PRECISE=1 \
XYO_PEN_FSR=0 \
XYO_RENDER_SCALE=0.5 \
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
"$HOME/.cache/xyo-dlss-build/play/xyo" play examples/play_demo.sb3 --scale 2
```

**RTX 3070, 32-frame opaque Pen comparison:**

| Metric vs CPU analytic replay | Standard GPU | FP64 GPU |
|---|---:|---:|
| Total different pre-NGX pixels over 32 frames | 2,202 | 766 |
| Total absolute RGB difference over 32 inputs | 4,186 | 1,476 |
| Different pixels at input frame 32 | 401 | 29 |
| Final NGX output absolute RGB difference | 96,045 | 10,569 |
| Final NGX output PSNR vs CPU | 45.85 dB | **64.06 dB** |

The final-output PSNR here is a measure of **agreement with
the CPU replay**, not the native Scratch full-resolution
renderer. For the latter, a separate 24-frame synthetic Pen
comparison at 960×720 output found CPU replay **42.96385 dB**,
regular GPU **42.17704 dB**, and FP64 GPU
**43.02807 dB**. The small FP64-over-CPU gain is
scene-specific, not proof of generally better image quality.
One 24-frame run measured nearly identical regular/FP64
GPU median draw times (**2.2507 / 2.2524 ms**); broad
throughput regressions have not been ruled out, especially
for many partially translucent strokes or GPUs with
limited FP64 throughput.

**50%-transparent Scratch Pen test, 32 frames:**
the total CPU/GPU input absolute RGB difference fell from
**8,720 to 7,545**; final output RGB difference fell from
**99,639 to 85,519**. Both improvements were smaller than
on the opaque scene. The 8-bit GPU framebuffer and blending
still round differently from the CPU's intermediate
straight-alpha accumulation; this precision mode does
**not** guarantee pixel parity.

Added GPU CTests
`tests/render/dlss_pen_gpu_precision_smoke.py`
and `tests/render/dlss_pen_gpu_transparency_smoke.py`;
both pass on RTX 3070. The complete targeted DLSS +
SDL renderer CTest suite passed **40/40** (including
**34 GPU-dependent tests**). The tests skip gracefully
(return code 77) when GLSL 4.00/FP64 shader support
is absent. The mode remains experimental and uncommitted.

### CPU-equivalent GPU Pen quantization (2026-11-09)

`XYO_DLSS_PEN_REPLAY_QUANTIZED=1` enables a separate, opt-in GPU
Pen reconstruction method; it does not depend on `..._PRECISE`.
It requires the real NGX, temporal jitter, primitive replay and GPU
replay options shown below. A **GLSL 4.30 / FP64 / SSBO** shader
is compiled only on request, falling back to standard GPU replay
if unavailable. The persistent Pen buffer is not modified.

The fragment shader iterates the recorded Pen strokes in source order.
Each pixel's **straight RGBA is rounded to 8-bit after every stroke**,
equivalent to `dlssReplayPenStrokes()`'s intermediate quantization and
final SDL straight-alpha blending, rather than hardware premultiplied
blend precision. Pixel-center capsule distances are evaluated in FP64,
including Scratch's special 0.5 logical-pixel center offset for
diameters 1 and 3. The NVIDIA GLSL driver initially failed to link
dynamic FP64-vector indexing (`__setVectorIndex`); scalar channel
assignments resolved the issue.

**RTX 3070, real NGX, 480x360 input → 960x720 output, 32 frames
per generated Scratch project:**

| Scratch Pen case | Prior FP64 input abs RGB delta | Quantized input delta | Prior FP64 final delta | Quantized final delta |
|---|---:|---:|---:|---:|
| Opaque 3px | 1,476 | **0** | 10,569 | **0** |
| 50% transparency, 3px | 7,545 | **0** | 85,519 | **0** |
| 25% transparency, 1px | 571 | **0** | 10,376 | **0** |
| 75% transparency, 5px | 2,213 | **0** | 82,651 | **0** |

All 128 tested pre-NGX frames and four final NGX outputs are
byte-identical to the CPU analytic replay. This measures agreement
with CPU replay, **not** fidelity to native full-resolution Scratch.
Other GPUs and other stroke histories might still differ. One
initial alpha75/size5 run produced a malformed/missing intermediate
PPM at frame 13; it did not reproduce on a standalone run or in
the next complete CTest. The cause remains unconfirmed.

A naive fullscreen pass calculates all strokes for every pixel.
After clearing the RGBA8 render target, the default quantized path
restricts fragment work with `glScissor` to the conservative
union of the stroke-support bounding boxes. Disable that optimization
for A/B comparison with `XYO_DLSS_PEN_REPLAY_SCISSOR=0`. Scissored
and fullscreen quantized passes produced byte-identical inputs
and final NGX output across a 32-frame half-transparent test.

Eight alternating ABBA/BAAB 32-frame trials (first two frames
excluded) had mean per-trial median `draw_ms`:
**3.6827 ms fullscreen** vs **2.5764 ms scissored**, about
**30.0% shorter**. Scissor won all four paired comparisons.
This includes NGX, GL/Vulkan interop and renderer overhead, and
is **not** an isolated GPU kernel timing. The test's thin strokes
are not representative of arbitrary high-coverage scenes.

Guardrails: quantized replay allows at most **64 strokes**, and
no more than **4 million** union-rectangle pixels × stroke
evaluations; otherwise it falls back to the **CPU analytic replay**,
not the lower-fidelity GPU mode. An unavailable GPU quantized
shader also selects CPU replay. The CPU replay itself must still
pass its preflight bounds.
The general 512-stroke/1-million sample-visit replay preflight
still applies. This is research-only and OFF by default.

A further real-NGX stress smoke in
`tests/render/dlss_pen_gpu_quantized_stress.py` passed on
RTX 3070: **64 strokes** execute the GPU quantized shader with
zero 32-frame input and final output error; **65 strokes** trigger
CPU fallback with zero error; **31 widely distributed strokes**
exceeding the 4M fragment×stroke bound also use CPU fallback
with zero error. This guarantees accuracy only while the existing
CPU analytic replay itself passes its safety preflight.

```sh
export XYO_DLSS_REAL=1
export XYO_DLSS_OPTICAL_FLOW=1 XYO_DLSS_TEMPORAL=1
export XYO_DLSS_RASTER_JITTER=1
export XYO_DLSS_PEN_PRIMITIVE_REPLAY=1
export XYO_DLSS_PEN_REPLAY_GPU=1
export XYO_DLSS_PEN_REPLAY_QUANTIZED=1
export XYO_PEN_FSR=0 XYO_RENDER_SCALE=0.5
export XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel"
"$HOME/.cache/xyo-dlss-build/play/xyo" play your-project.sb3 --scale 2
```

The registered hardware CTest is
`xyo-dlss-pen-gpu-quantized-parity` in
`tests/render/dlss_pen_gpu_quantized_smoke.py`.
Manual alternating A/B measurements are implemented in
`tests/render/dlss_pen_gpu_quantized_benchmark.py`.
### Experimental 16x16 GPU Pen tile masks (2026-11-09)

Enable **both** `XYO_DLSS_PEN_REPLAY_QUANTIZED=1` and
`XYO_DLSS_PEN_REPLAY_TILED=1`. The latter is OFF by default.
The renderer builds two 32-bit source-ordered masks per 16x16 input
tile, using the same conservative stroke support bounds as the
scissored quantized path. The GLSL 4.30 shader visits only the
strokes whose mask bits are set, in ascending original stroke order.
It still rounds RGBA to 8 bits **after each stroke**.
The persistent Pen texture and ordinary GL 3.3 path are unchanged.

The **4-million evaluation** limit now uses the conservative
sum of `16*16*popcount(tile mask)` when tiled mode is selected.
The limit remains unchanged for the untiled path. The 64-stroke
and general CPU-replay preflight limits remain in force.

Real RTX3070/NGX tests:
- All four existing transparency/diameter scenes, 32 input frames
  each and final NGX image, exactly matched CPU analytic replay in
  tiled and untiled modes (zero summed RGB byte difference).
- 64 consecutive strokes ran the tiled GPU path with zero error.
- **61 scattered short strokes** exceeded the untiled work budget
  and used CPU fallback, but ran in the tiled GPU shader with
  **zero 32-frame input and final-output difference**.
- 65 strokes and the existing 31-wide-stroke case still correctly
  fall back to CPU.

Two ABBA/BAAB blocks (8 runs, 32 frames/run, first 2 excluded);
average of per-run `draw_ms` medians:

| Scene | Baseline | Tiled GPU | Difference |
|---|---:|---:|---:|
| 61 scattered strokes (baseline CPU fallback) | 2.5672 ms | 2.2909 ms | 10.76% lower |
| 64 nearby strokes (baseline untiled GPU) | 2.0274 ms | 1.9527 ms | 3.68% lower |

The measurements include the full renderer, GL/Vulkan handoff
and NGX; they are **not** isolated GPU shader times. The nearby
stroke gain is small relative to run-to-run variation. Do not
generalize these results to every project or GPU.

Reproduce with `tests/render/dlss_pen_gpu_tiled_benchmark.py`
(`--scene sparse` or `--scene contiguous`). The parity/stress
smokes also verify tile selection through the debug log.
The JIT worktree is concurrently being edited; the initial tile
implementation used isolated compilation/relinking. A subsequent
`xyo-cli` target build succeeded after the unrelated
`src/ir/numeric_region.hpp` became available; a fresh clean build
has not been separately verified.

No proprietary SDK artifacts are vendored, and no commit was made.

## Invisible SDL window for DLSS debugging (2026-11-09)

Set `XYO_HEADLESS=1` when running `xyo play` to create the renderer's
SDL window with `SDL_WINDOW_HIDDEN`. This suppresses the desktop window
while retaining the desktop OpenGL context, SDL render targets,
GL/Vulkan interop, and NVIDIA NGX. The default (`XYO_HEADLESS` unset
or `=0`) is unchanged.

Example with final-frame capture:

```sh
export XYO_HEADLESS=1
export XYO_DLSS_REAL=1
export XYO_DLSS_REAL_DEBUG=1
export XYO_RENDER_SCALE=0.5
export XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel"
export XYO_EXIT_FRAMES=32
export XYO_SHOT=/tmp/xyo-dlss-hidden.ppm
"$HOME/.cache/xyo-dlss-build/play/xyo" play your-project.sb3 --scale 2
```

This is **not** a display-server-free implementation: SDL still needs
a functional accelerated desktop OpenGL driver/context (for example,
X11 or a compatible graphical session). `SDL_VIDEODRIVER=dummy`
does not provide the GL/Vulkan interop used here. It is suitable for
automated visual regression and debugging on a GPU desktop without
showing an interactive window. A separate SDL/EGL offscreen mode
is available as documented below.

A screenshot bug surfaced during hidden-window validation:
`XYO_SHOT` previously read the SDL back buffer *after*
`SDL_RenderPresent`, when its contents are undefined. For opt-in
shots on the final frame, `xyo play` now takes the capture using
`SdlRenderer::setFrameCapture()` **before** presentation. A failed screenshot or a missing final-frame draw is now reported
with a nonzero exit status instead of attempting an invalid post-swap
back-buffer read.

Native RTX 3070 NGX run: 32/32 pre-NGX input frames and the final
NGX output image were byte-identical between `XYO_HEADLESS=0` and
`XYO_HEADLESS=1` with tiled, CPU-quantized Pen rendering. The hidden
window is confirmed with `SDL_GetWindowFlags` in debug logs.
Registered hardware test:
`xyo-dlss-hidden-window-gpu-parity`, via
`tests/render/dlss_hidden_window_smoke.py`. The `xyo-cli` target
built successfully. Changes uncommitted.

### SDL/EGL offscreen execution without X11 or Wayland (2026-11-09)

`XYO_HEADLESS=offscreen` is distinct from `XYO_HEADLESS=1`:
it selects SDL's `offscreen` video driver **before** video
subsystem initialization and creates an invisible SDL/EGL OpenGL
window/context. It does **not** require an X11 or Wayland display server.
OpenGL-to-Vulkan resource sharing, optical flow and NVIDIA NGX DLSS
continue to run on the RTX GPU. Unlike Vulkan-only headless rendering,
this path still depends on SDL's offscreen EGL backend and a
compatible NVIDIA EGL/OpenGL driver and Vulkan GPU; not all systems
support that stack. The existing `XYO_HEADLESS=1` remains the
hidden-window option using the desktop display driver.

Example without a desktop session:

```sh
unset DISPLAY WAYLAND_DISPLAY
export XYO_HEADLESS=offscreen
export XYO_DLSS_REAL=1
export XYO_DLSS_OPTICAL_FLOW=1
export XYO_DLSS_TEMPORAL=1
export XYO_DLSS_REAL_DEBUG=1
export XYO_RENDER_SCALE=0.5
export XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel"
export XYO_EXIT_FRAMES=32
export XYO_SHOT=/tmp/xyo-dlss-offscreen.ppm
"$HOME/.cache/xyo-dlss-build/play/xyo" play your-project.sb3 --scale 2
```

Neither `SDL_VIDEODRIVER=offscreen` nor
`EGL_PLATFORM=surfaceless` needs to be specified manually; the
mode selects the SDL backend. If SDL video was previously initialized
with another driver, the explicit offscreen request is rejected
instead of silently falling back to an X11 window.

On RTX 3070, the SDL offscreen driver and NGX ran with both
`DISPLAY` and `WAYLAND_DISPLAY` absent. Comparing visible
X11 and offscreen EGL runs gave zero difference in all **32 actual
pre-NGX input frames** and in the **final NGX PPM** for the same
CPU-quantized/tiled Pen project. A separately registered hardware
CTest, `xyo-dlss-surfaceless-gpu-parity`, deliberately unsets
the display variables and compares CPU analytic vs GPU tiled Pen
rendering entirely within SDL offscreen: 32/32 input frames and
the final NGX output matched byte-for-byte. The existing hidden-window
CTest also passed after the new mode was added.

A further real-project smoke used `debug/projects/3D2.sb3`:
with no display variables and `XYO_HEADLESS=offscreen`,
20 frames of Optical Flow + temporal NGX completed (exit 0),
the final 960x720 RGB PPM was written (2,073,615 bytes), and
the play profiler recorded 20 rows, with median `draw_ms` =
1.1552 ms over frames 3-20. The image was not scored for
visual quality and no performance baseline was recorded.

### Per-frame post-NGX screenshot series (2026-11-09)

For temporal artifacts, enable `XYO_SHOT_SERIES` to save *final
reconstructed* display frames rather than only pre-NGX raster inputs:

```sh
unset DISPLAY WAYLAND_DISPLAY
export XYO_HEADLESS=offscreen
export XYO_DLSS_REAL=1
export XYO_DLSS_OPTICAL_FLOW=1 XYO_DLSS_TEMPORAL=1
export XYO_RENDER_SCALE=0.5
export XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel"
export XYO_EXIT_FRAMES=120
export XYO_SHOT_SERIES=/tmp/dlss/frame
export XYO_SHOT_EVERY=2
export XYO_SHOT=/tmp/dlss/final.ppm
mkdir -p /tmp/dlss
"$HOME/.cache/xyo-dlss-build/play/xyo" play your-project.sb3 --scale 2
```

This produces `frame-000002.ppm`, `frame-000004.ppm`, and so
on (1-based frame numbers), plus `final.ppm`. With
`XYO_SHOT_EVERY` absent, **every** frame is captured.
Interval must be a positive decimal integer no greater than 1,000,000;
an invalid value exits with code 2. Images use PPM P6 RGB24 and
are captured **before** SDL buffer presentation, including in the
offscreen EGL mode. The capture is opt-in and each frame requires
a GPU readback and disk write, so exclude it from production
performance measurements. Parent directories must already exist.
The PPM writer now packs one RGB scanline and writes it with
`fwrite`, replacing millions of per-channel `fputc`
calls. This reduces debug capture overhead without changing
the P6 pixel format.

A failed sequence or final-frame capture returns exit code 2.
The old final-frame post-present fallback is removed because
readback of the swapped GL back buffer is undefined.

Native RTX 3070 hardware test
`xyo-dlss-offscreen-frame-series` verifies eight consecutive
NGX output frames without DISPLAY/WAYLAND_DISPLAY, every-second
frame capture (4/4 frames), byte-identity with simultaneous
`XYO_SHOT` capture, and failure on invalid intervals and unwritable
output directories. Existing hidden-window and surfaceless tests
remained green. The existing quantized Pen GPU parity test also
passed after the buffered PPM writer was introduced.

At 960x720, an interleaved 40-frame `3D2.sb3` benchmark on
RTX 3070, using median `draw_ms` of frames 3-40 (the full render
path, including screenshot capture) found:

| Capture | Before buffered writer | After buffered writer |
|---|---:|---:|
| Disabled | 0.6479 ms | 0.7203 ms |
| Every 8th frame | 0.7095 ms | 0.7164 ms |
| Every frame | 32.5327 ms | 4.6820 ms |

The every-frame capture reduces mean trial-median time by
**85.6%**, roughly **6.95x** faster than the per-byte writer.
The difference in no-capture baseline is ordinary run-to-run
variation; these are not isolated GPU timings or a claim of
consistent capture overhead on other machines.

Reproduce the new-writer measurements with:

```sh
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
python3 tests/render/dlss_offscreen_capture_benchmark.py \
    "$HOME/.cache/xyo-dlss-build/play/xyo" --frames 40 --blocks 1
```

### Optional GL PBO asynchronous screenshot capture (2026-11-09)

The experimental `XYO_SHOT_SERIES_ASYNC=1` moves screenshot
transfer and file output away from the synchronous per-frame
`SDL_RenderReadPixels` + PPM path. It requires SDL's desktop
OpenGL renderer and this Linux NVIDIA NGX-enabled build.

```sh
unset DISPLAY WAYLAND_DISPLAY
mkdir -p /tmp/dlss
export XYO_HEADLESS=offscreen
export XYO_DLSS_REAL=1
export XYO_DLSS_OPTICAL_FLOW=1 XYO_DLSS_TEMPORAL=1
export XYO_RENDER_SCALE=0.5
export XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel"
export XYO_EXIT_FRAMES=120
export XYO_SHOT_SERIES=/tmp/dlss/frame
export XYO_SHOT_SERIES_ASYNC=1
export XYO_SHOT_EVERY=1
"$HOME/.cache/xyo-dlss-build/play/xyo" play debug/projects/3D2.sb3 --scale 2
```

Frame names and P6 RGB output are unchanged. Each displayed frame
is flushed from the SDL renderer and read back into an OpenGL pixel
buffer object (`GL_PIXEL_PACK_BUFFER`). GL sync fences allow later
frames to overlap the GPU transfer. CPU-side mapping occurs after
a fence signals; a separate single writer thread packs RGBA into
top-down RGB scanlines and performs disk I/O in capture order.
Up to four PBOs and eight queued CPU images bound memory usage.
Individual screenshots are limited to 32 MiB of raw RGBA and
GPU fence waits are bounded to five seconds; unsupported GL
renderers and failures report a nonzero exit status rather than
silently using a different capture mode. All pending buffers and
writes are flushed before normal process exit.

**Important:** `XYO_SHOT` remains an independent synchronous
final-frame capture when set. Leave it unset for a pure PBO
performance benchmark; otherwise the last draw still includes a
synchronous screenshot. Both capture modes require real GPU
readback, so PBOs do not make image capture free.

SDL's offscreen EGL backend exposes `GL_BACK` as its working
read buffer even when `GL_DOUBLEBUFFER` reports false. The new
code preserves SDL's read-buffer selection instead of forcing
`GL_FRONT`; doing otherwise produced
`GL_INVALID_OPERATION (0x502)` on RTX3070.

The new native test `xyo-dlss-offscreen-pbo-parity` runs
without DISPLAY or WAYLAND_DISPLAY, verifying **32/32** complete
post-NGX PPM frames byte-identical to synchronous capture, 8/8
frames with a stride of four, final-frame exactness, and
asynchronous write-error propagation. The three existing hidden
window, offscreen NGX, and frame-series tests also passed: **4/4**.

An alternating 12-run test (two 6-run blocks, 40 frames/run,
2 warmup frames excluded) on RTX3070 using
`debug/projects/3D2.sb3` at `--fps 1000` measured:

| Capture mode | Mean run-median draw_ms | Mean full process wall time |
|---|---:|---:|
| Disabled | 1.8889 ms | 1.0596 s |
| Buffered synchronous PPM | 4.8997 ms | 1.2499 s |
| PBO + background writer | 2.1431 ms | 1.1024 s |

The PBO mode reduced measured draw time **56.3%** compared with
the optimized synchronous writer, and process wall time about
**11.8%**. Process time includes startup and NGX initialization;
draw_ms includes the renderer and capture scheduling but excludes
end-of-run PBO draining. Hence both metrics are reported.
Performance depends on the GPU, driver, filesystem and project.

Reproduce with:

```sh
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
python3 tests/render/dlss_offscreen_pbo_benchmark.py \
    "$HOME/.cache/xyo-dlss-build/play/xyo" \
    --frames 40 --fps 1000 --blocks 2
```

The SDL renderer and CLI translation units were initially
compiled and linked separately because a concurrent change in
`src/engine/fiber.cpp` lacked `<atomic>`. The header was subsequently
corrected by its owning workstream. On 2026-11-10 the full
`xyo-cli` target build succeeded, and all five offscreen/headless
screenshot CTests and the separate
`xyo-dlss-pen-gpu-quantized-parity` test passed with the newly linked
binary. No commit/push.

#### Bounded queue telemetry and pressure tests (2026-11-10)

To diagnose delayed disk output, enable
`XYO_SHOT_ASYNC_STATS=1` alongside
`XYO_SHOT_SERIES_ASYNC=1`. After the final PBO has been
collected and the writer thread joined, stderr reports
`submitted`, `saved`, `max_pbos`,
`max_cpu_queue`, `gpu_waits`, `gpu_wait_ms`,
`queue_waits`, `queue_wait_ms` and `error`.
The counters distinguish GPU readback waits from disk
backpressure and verify that every submitted screenshot was
persisted before exit. Telemetry is opt-in.

The native `xyo-dlss-offscreen-pbo-pressure` CTest
stress-tests 96 frames at **1920x1440** with SDL offscreen EGL
and real NVIDIA NGX, checks six sampled frames against
synchronous PPM SHA-256 hashes, then tests one-frame shutdown
with no dropped captures. It also uses a **test-only**
`XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS=25` delay in the
background writer to force the eight-frame CPU queue to fill.
The producer must block until space is available; it must
neither drop images nor exceed four PBOs / eight queued images.
Do not use the test delay for normal screenshots.

On RTX3070, 96 large frames (796,264,032 bytes of PPM
output) all completed with exit code 0. One run observed
`max_cpu_queue=8`, `queue_waits=16`,
`queue_wait_ms=21.229`, with a maximum process RSS
of 508,724 KiB; the exact figures vary with disk contention.
In the deterministic 48-frame slow-writer test,
`queue_waits=39`, `queue_wait_ms=943.397`;
all 48 images were saved. The new test plus four existing
Headless/NGX screenshot CTests passed **5/5**.

Reproduce the validation:

```sh
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
python3 tests/render/dlss_offscreen_pbo_pressure_smoke.py \
    "$HOME/.cache/xyo-dlss-build/play/xyo"
```

#### Experimental packed RGB PBO transfer (2026-11-10)

For PPM screenshots, alpha is discarded. With
`XYO_SHOT_SERIES_ASYNC=1`, enable
`XYO_SHOT_ASYNC_RGB=1` to request
`glReadPixels(..., GL_RGB, GL_UNSIGNED_BYTE)` instead of
`GL_RGBA`. A tightly packed 3-byte scanline is read through
the same PBO/fence/worker pipeline using `GL_PACK_ALIGNMENT=1`.
The background writer reverses the GL row order and writes each
RGB row directly to the PPM file, avoiding RGBA-to-RGB packing.

The feature is *opt-in* because direct RGB GPU readback performance
depends on the driver and framebuffer pixel format; the original
RGBA path remains default. Both modes produce exactly the same P6
images. Use `XYO_SHOT_ASYNC_STATS=1` to inspect transfer format,
bytes/pixel, and total submitted bytes alongside the existing
queue/backpressure metrics.

At 960x720 over 32 frames on RTX 3070, transferred PBO bytes were
**88,473,600 (RGBA)** versus **66,355,200 (RGB)**, exactly **25% less**.
The RGBA-to-RGB *copy/packing step* is removed in RGB mode;
the PPM output size itself remains unchanged. All **32/32** output
frames were byte-identical among synchronous SDL capture, RGBA
PBO capture, and RGB PBO capture. `xyo-dlss-offscreen-pbo-parity`
now asserts full image parity and actual transferred byte counts.

Interleaved end-to-end `3D2.sb3` rendering benchmarks, all PBO
captures enabled, `--fps 1000`:

| Scale / output | RGBA PBO mean run-median draw_ms | RGB PBO | Difference |
|---|---:|---:|---:|
| 2 / 960x720 (12 runs, 48 frames each) | 1.4341 ms | 1.2360 ms | -13.81% |
| 4 / 1920x1440 (8 runs, 40 frames each) | 5.0329 ms | 4.6799 ms | -7.01% |

Process wall times averaged 1.0067 vs 0.9847 s at 960x720,
and 1.0723 vs 1.0420 s at 1920x1440 (RGBA vs RGB).
These are small samples from a shared GPU and not isolated
OpenGL timings; the transfer bandwidth reduction is exact,
but the performance benefit is workload-dependent.

```sh
# RGB readback in ordinary offscreen DLSS capture:
export XYO_SHOT_SERIES_ASYNC=1
export XYO_SHOT_ASYNC_RGB=1
export XYO_SHOT_ASYNC_STATS=1

# Reproduce the A/B benchmark (run from repository root):
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
python3 tests/render/dlss_offscreen_pbo_benchmark.py \
  "$HOME/.cache/xyo-dlss-build/play/xyo" \
  --compare-rgb --frames 48 --fps 1000 --blocks 3 --scale 2
```

The pressure regression additionally accepts `--rgb`.
RTX 3070 passed a 96-frame 1920x1440 RGB transfer
(796,262,400 raw bytes), six synchronous comparison frames,
a single-frame drain, and a 48-frame deliberately slow writer
backpressure test (39 queue waits, no image loss). Dedicated
CTest: `xyo-dlss-offscreen-pbo-rgb-pressure`.

After a full `xyo-cli` target build, the six screenshot/headless
GPU CTests passed together (**6/6**). The independent
`xyo-dlss-pen-gpu-quantized-parity` CTest also passed (38.34 s),
giving **7/7** relevant native RTX3070 GPU checks. The new
mode is still explicit opt-in, not enabled by default.

#### Optional reusable CPU image buffers for PBO capture (2026-11-10)

`XYO_SHOT_SERIES_ASYNC=1` previously allocated a new
`std::vector<uint8_t>` for every completed GPU PBO readback
before copying the mapped bytes. The worker released the vector
after completing each PPM file, causing repeated allocations and
free operations under long-running capture.

`XYO_SHOT_ASYNC_BUFFER_POOL=1` enables an experimental
CPU-side buffer pool. After the writer closes a file, it returns
ownership of the image's vector to a mutex-protected free list.
The main GL thread acquires a free buffer before copying the next
completed PBO; when no free buffer is available, it still allocates.
Reuse is restricted to at most **10** retained buffers: the normal
8 queued images, 1 active writer image and 1 producer staging
image remain independently bounded. The GL calls continue to
execute only on the renderer thread; the writer holds sole access
to its image until output is complete.

The existing `XYO_SHOT_ASYNC_STATS=1` log also reports:

```text
[async PPM] cpu_buffers reuse=1 allocations=3 reused=93 peak_pool=3
```

On RTX3070, a 1920x1440 / 96-frame RGB-PBO test used only
**3 new CPU image buffer allocations, versus 96 without recycling**
and returned **93 previously used buffers**. All 96 PPM images
were written, six sampled frames matched synchronous PPM byte
hashes, the single-frame exit flushed correctly, and forced
writer backlog did not drop any of the 48 test images.
Separate `xyo-dlss-offscreen-pbo-pool-pressure` (RGBA) and
`xyo-dlss-offscreen-pbo-rgb-pool-pressure` (RGB) CTests cover
both transfer formats. Existing Headless/PBO regression tests
remain unchanged; all **8/8** Headless/PBO CTests passed after
the pool was enabled in the new test configurations.

Interleaved `3D2.sb3` A/B comparisons (`--fps 1000`;
mean of each run's median draw_ms) showed **variable** results:

| Output and run window | No pool | Pool | Change |
|---|---:|---:|---:|
| 960x720 / 48 frames / 12 runs | 1.3951 ms | 1.3004 ms | -6.8% |
| 960x720 / 96 frames / 8 runs | 0.7631 ms | 0.7664 ms | +0.4% |
| 1920x1440 / 40 frames / 12 runs | 4.6543 ms | 4.6634 ms | +0.2% |
| 1920x1440 / 64 frames / 8 runs | 2.9981 ms | 2.2524 ms | -24.9% |

The peak RSS sampled by the play profiler in the latter two
8-run comparisons differed by roughly 1–2 MiB between modes,
well within the noise of whole-process measurements. Memory
pooling demonstrably removes CPU allocation churn, but its
frame-time and peak-RSS benefit is **not consistent** across
runs and likely depends on driver, allocator and shared GPU load.
It remains OFF by default and should not be claimed to always
accelerate capture. The GPU PBO transfer byte count is unchanged
by CPU pooling; the separate `XYO_SHOT_ASYNC_RGB=1` flag
reduces transfer bytes.

```sh
# With offscreen DLSS and XYO_SHOT_SERIES already configured:
export XYO_SHOT_SERIES_ASYNC=1
export XYO_SHOT_ASYNC_RGB=1
export XYO_SHOT_ASYNC_BUFFER_POOL=1
export XYO_SHOT_ASYNC_STATS=1

# Paired memory/draw-time comparison, from repository root:
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
python3 tests/render/dlss_offscreen_pbo_benchmark.py \
    "$HOME/.cache/xyo-dlss-build/play/xyo" \
    --compare-pool --frames 96 --fps 1000 --blocks 2 --scale 2
```

A separate 256-frame, 960x720 real-NGX/offscreen run
wrote all **256** PPM frames (530.85 MB of files) with
**2** CPU allocations and **254** buffer reuses. Peak sampled
RSS was **358.4 MiB**; mean RSS rose from 349.5 MiB over the
first 64 frames to 358.4 MiB over the last 64. This proves
bounded buffer counts and completion, not the absence of all
unrelated process memory growth. The forced backpressure
test now also compares SHA-256 hashes for six sampled frames,
so a frame-index swap under load is detectable.

Final validation after full `xyo-cli` build: **8/8**
targeted Headless/PBO CTests and the separate
`xyo-dlss-pen-gpu-quantized-parity` CTest passed (**9/9**).
The buffer pool is opt-in and no default behavior was changed.
No commit/push.

#### Experimental direct PBO-mapped writer (2026-11-10)

Fine-grained measurement of 64-frame real NGX screenshot runs
using `XYO_SHOT_ASYNC_STAGE_PROFILE=1` showed the GPU-to-CPU
*memcpy* to be a major cost even after using three-byte GL_RGB:
64 frames at 960x720 involved 132,710,400 copied bytes and
11.74 ms of cumulative CPU copy time; at 1920x1440 it
involved 530,841,600 bytes and 54.09 ms. The separate
writer took 43.06 ms and 175.82 ms, respectively, over
these 64 frames (different thread, not additive to draw_ms).
`glReadPixels` command/fence/flush calls accounted
for 2.37 ms and 4.98 ms cumulatively.

`XYO_SHOT_ASYNC_DIRECT_MAP=1` enables an *experimental*
zero-intermediate-copy path. Once a PBO fence signals, the
renderer maps the PBO on the GL thread and transfers the
read-only CPU mapping to the disk worker, which writes PPM
scanlines directly from it. The GPU buffer stays mapped until
the worker reports completion. Only the GL thread calls
`glUnmapBuffer` and returns the buffer to the pool;
mapped PBOs are never re-used by `glReadPixels`.
At most four pending/mapped PBOs are allowed. If all four
are held by the writer or queued frames, the renderer
waits for the writer to finish one, applying backpressure.
The capture flush joins the writer and then unmaps/deletes
remaining PBOs. File-write failures propagate to exit code 2.

This avoids both the full-frame CPU allocation and
`memcpy`, including when RGB readback is selected.
Unlike the CPU-image pool, `XYO_SHOT_ASYNC_BUFFER_POOL`
is unnecessary in direct-map mode. This is opt-in because
accessing mapped GL driver memory from a non-GL CPU thread
has different latency/bandwidth characteristics across
drivers, and may reduce performance on other GPU architectures.

RTX3070 interleaved A/B tests using `3D2.sb3`
(64 frames/run, 12 runs, `--fps 1000`):

| Output | RGB PBO with memcpy | Direct mapped RGB PBO | Draw reduction | Wall-time reduction |
|---|---:|---:|---:|---:|
| 960x720 | 0.8293 ms | 0.4796 ms | 42.17% | 2.04% |
| 1920x1440 | 2.5444 ms | 0.8833 ms | 65.28% | 5.11% |

These are mean run medians of CPU `draw_ms`, not isolated
GPU timestamp measurements. Process wall times include loading,
rendering and final PBO draining. The mapped writer can spend
more time accessing driver memory even though the render thread
gets faster. No cross-platform speedup is assumed.

```sh
# Run with the existing SDL EGL offscreen and real NGX env:
export XYO_SHOT_SERIES_ASYNC=1
export XYO_SHOT_ASYNC_RGB=1
export XYO_SHOT_ASYNC_DIRECT_MAP=1
export XYO_SHOT_ASYNC_STATS=1
# Optional CPU-stage diagnostics:
export XYO_SHOT_ASYNC_STAGE_PROFILE=1

# Reproduce the paired experiment:
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
python3 tests/render/dlss_offscreen_pbo_benchmark.py \
 "$HOME/.cache/xyo-dlss-build/play/xyo" \
 --compare-direct --frames 64 --fps 1000 --blocks 3 --scale 4
```

The native stress tests
`xyo-dlss-offscreen-pbo-direct-pressure` (RGBA) and
`xyo-dlss-offscreen-pbo-rgb-direct-pressure` (RGB) check
96 consecutive 1920x1440 frames, six synchronous SHA256
comparisons, a one-frame termination drain, frame-index
correctness under forced disk-writer delay, and error
propagation for an unwritable destination. The stage telemetry
also asserts `cpu_copy_bytes=0` for direct mode and validates
bounded mapped PBO count. Existing modes stay enabled only
through their own opt-in flags, and default capture is
unaffected.

After a full `xyo-cli` rebuild, the ten headless/capture
GPU CTests passed **10/10**, and
`xyo-dlss-pen-gpu-quantized-parity` passed independently,
giving **11/11** relevant RTX3070 GPU checks.
An additional 256-frame RGB direct-mapped PBO run produced all
256 960x720 images (530.85 MB), with **zero** CPU-side image
allocations, zero explicit CPU `memcpy` bytes, and a maximum of
three PBOs borrowed by the worker. The play profiler recorded a
peak RSS of 362.41 MiB. This does not prove there are no other
memory leaks or that the OpenGL driver performs no implicit
transfers. Default mode remains unchanged. No commit/push.

#### Mapped PBO writer failure race and lifecycle verification (2026-11-10)

Corrected a resource-ownership corner case in the experimental
`XYO_SHOT_ASYNC_DIRECT_MAP=1` mode. The writer may fail
**after** the GL thread maps a PBO and removes it from the
pending-fence queue, but **before** the mapped image is placed
on the writer's queue. The original error branch abandoned the
mapped PBO without either returning it to the free list or
sending it to the writer's completion queue. This was a GPU
resource lifetime leak, not a pixel parity difference on
successful runs.

On observing failure before enqueue, the GL thread now
unmaps the never-queued PBO itself, restores the prior pixel
pack binding, returns the PBO to the free list, and reduces
the outstanding borrowed count. This unmap is safe because
the image was never handed to the writer. Normal queued PBOs
still get unmapped only after explicit writer completion.

When `XYO_SHOT_ASYNC_STATS=1`, a new lifecycle log prints:

```text
[async PPM] lifecycle created=2 deleted=2 borrowed_remaining=0 completed_remaining=0 aborted_before_enqueue=1
```

At the end of `finish()`, a mismatch in GL buffer
creation/deletion counts or a remaining borrowed/completed
mapping is treated as a capture error. The new CTest
`xyo-dlss-offscreen-pbo-failure-race` runs with real
RTX3070 NGX, simulates a writer failure after one completed
frame, and inserts a **test-only** enqueue delay to force the
failure during the GL PBO handoff. It verifies, for RGB and
RGBA separately, exit code 2, explicit mapped-buffer abort,
zero outstanding mappings, and identical created/deleted counts.
Two new environment variables are for fault injection **only**:
`XYO_SHOT_ASYNC_TEST_FAIL_AFTER` (successfully saved frames)
and `XYO_SHOT_ASYNC_TEST_ENQUEUE_DELAY_MS` (GL-thread delay).
Neither should be used in actual capture or benchmarks.

After the full `xyo-cli` rebuild, all **11** targeted
headless/DLSS screenshot tests passed, and
`xyo-dlss-pen-gpu-quantized-parity` independently
passed (40.73 s), giving **12/12** relevant GPU
regression tests. The new fault-injection CTest also passed
five consecutive repetitions (five runs, with both RGB
and RGBA failure paths exercised in every run).
Default capture behavior is unchanged; direct-mapped PBOs
remain explicit opt-in. No commit/push.

#### GL failure injection and SDL context restoration (2026-11-10)

The optional PBO capture path now fails cleanly on failures of
`glReadPixels`, `glClientWaitSync` or
`glMapBufferRange`. In each case it logs a concrete
diagnostic, marks the capture as failed, stops submitting frames,
disposes of pending GL fences/buffers during `finish()`,
and returns exit code 2 through `xyo play`. A guard refuses
GL calls if no GL context is current at final cleanup.

`SdlRenderer::flushPPMAsync()` now restores its saved
SDL OpenGL context before PBO cleanup if a different context
is current or the original was temporarily unbound. Test-only
`XYO_SHOT_ASYNC_TEST_UNBIND_CONTEXT=1` simulates this
condition immediately before flush without destroying the
context. In real NVIDIA NGX/EGL runs, the eight saved frames
were byte-identical with/without unbinding in both RGBA
and RGB modes.

The new native test
`xyo-dlss-offscreen-pbo-gl-fault` covers six injected
GL failures (read/fence/map in RGBA and RGB modes) and
two context-unbind/rebind image comparisons. The test-only
`XYO_SHOT_ASYNC_TEST_FAIL_STAGE` accepts
`read`, `fence`, or `map`. No real GPU
driver failure is triggered.

The earlier `xyo-dlss-offscreen-pbo-failure-race`
test had an intermittent assumption that the writer failure
would necessarily occur *while* a mapped PBO was between
the GPU and writer queues; on one run it failed after all
PBOs had already been handed off, even though all GL
resources were released correctly. The test now checks
that real writer failure cleans up **regardless of timing**,
and separately forces the precise pre-enqueue failure with
`XYO_SHOT_ASYNC_TEST_ABORT_BEFORE_ENQUEUE=1`.
Both failure tests passed five consecutive repetitions
each on RTX 3070.

After a successful full `xyo-cli` build and final
renderer-only rebuild, all **12/12** Headless/DLSS
tests passed, plus the independent quantized GPU Pen
parity test (**13/13** relevant GPU checks). Actual
destroyed/irrecoverably lost GL contexts are different
from temporary unbinding: explicit `glDeleteBuffers`
cannot be promised without a valid context, and the
GPU-driver-reset scenario is **not verified**. These
failure injection variables are for regression tests,
not production use. The default capture and rendering
mode are unchanged.

#### PBO GL context ownership and foreign-context safety (2026-11-10)

The asynchronous PBO implementation now records the **actual SDL
OpenGL context** at the first successful capture setup. Later captures
refuse to touch PBO objects if `SDL_RenderFlush` leaves a
different GL context current. This prevents GPU object names from
silently referring to objects owned by a different context/share group.
`GlAsyncPpmCapture::finish()` also requires the same original
context: a non-null but foreign context is **not** sufficient to
safely delete GL buffers or unmap the worker's PBOs.

`SdlRenderer::flushPPMAsync()` now asks the capture for its
recorded context and rebinds that specific context, rather than
assuming the previously stored `sdlGlCtx_` handle is always
identical. Tests use the independent GL Pen context to check both
branches: with rebind enabled all eight captured frames are
byte-identical to normal captures, in both RGB and RGBA modes;
with rebind intentionally skipped the flush returns exit code
2, logs a refused foreign-context cleanup, and issues no GL
object deletion using the wrong context. A separate unbound
context test also refuses GL operations when no context is
available.

Test-only switches:
- `XYO_SHOT_ASYNC_TEST_FOREIGN_CONTEXT=1` makes the independent
  GL Pen context current immediately before flush
- `XYO_SHOT_ASYNC_TEST_SKIP_RESTORE=1` prevents re-binding the
  saved PBO context, testing fail-closed behavior

**Limitation:** On true context destruction or a failed rebind,
`finish()` cannot explicitly unmap/delete GPU PBOs; it
stops the disk worker and returns failure, leaving GPU resource
reclamation to the driver when the owning context/share group is
destroyed. A deliberately skipped restoration in tests is *not*
equivalent to a GPU device loss, and cannot establish safe
recovery from a real driver reset. The experiment does not alter
normal capture behavior.

The native `xyo-dlss-offscreen-pbo-gl-fault` regression
now covers context unbind/restore, foreign-context recovery,
foreign-context refusal, and contextless refusal for both pixel
formats, in addition to read/fence/map failures. After a complete
`xyo-cli` rebuild, the **12/12** target Headless/PBO GPU
CTests passed; quantized GPU Pen parity passed separately
(**13/13** total on RTX 3070, 2026-11-10). The extended
foreign-context GL-fault regression also passed three consecutive
repetitions, including both RGB and RGBA paths. No commit/push.

#### Atomic PPM publication on asynchronous capture (2026-11-10)

The asynchronous PBO worker normally opens each final screenshot
path directly for writing, which may leave a truncated or corrupted
`.ppm` file if the disk reports an error partway through a
frame. The experimental `XYO_SHOT_ASYNC_ATOMIC=1` path
prevents partially written *final* files. On Linux it creates an
exclusive unique sibling with `mkstemp` (for example
`frame-000042.ppm.part.xxxxxx`), writes the full P6 image,
closes it, and only then uses a same-directory `rename` to
publish the completed screenshot. If any row write, close, or
rename operation fails, it deletes the staging file and reports
capture failure (process exit code 2). An existing final image
is preserved when a replacement fails. The temporary filename
is chosen securely with O_EXCL and has mode 0600, which also
means successfully published *new* files have mode 0600 rather
than the ordinary create-mode/umask behavior.

```sh
# With an existing XYO_SHOT_SERIES=... and PBO async capture:
export XYO_SHOT_SERIES_ASYNC=1
export XYO_SHOT_ASYNC_RGB=1
export XYO_SHOT_ASYNC_DIRECT_MAP=1
export XYO_SHOT_ASYNC_ATOMIC=1
export XYO_SHOT_ASYNC_STATS=1
```

Statistics report the number of created staging files,
successful final renames, and discarded partial files:
`[async PPM] atomic_files enabled=1 staged=8 published=8 discarded=0`.
The new native
`xyo-dlss-offscreen-pbo-atomic` test verifies all four
combinations of RGBA/RGB and copied/direct PBO readback.
Every completed frame is byte-identical to non-atomic output
(8 frames/mode). It additionally injects failure after two
scanlines, preserving an existing final image and confirming
the partial staging file is deleted, then tests rename failure
when a destination is an existing directory. All test cases
returned correct status codes with no dangling PBO mappings.
The test passed three consecutive CTest runs before the
additional rename-failure checks, and a further stand-alone
GPU run with rename-failure coverage passed.

An interleaved RTX 3070 `3D2.sb3` comparison with direct
RGB PBO, using only short runs, measured:

| Output | Plain mean wall/run | Atomic mean wall/run | Atomic overhead |
|---|---:|---:|---:|
| 960x720, 64 frames, 12 runs | 1.488 s | 1.414 s | -5.0% |
| 1920x1440, 48 frames, 8 runs | 1.470 s | 1.450 s | -1.4% |

There was no reproducible *penalty* in these samples; the
observed speed differences are probably environmental noise.
Atomic rename does not improve GPU throughput or imply a
speedup. The path remains opt-in because publishing semantics
and file permissions differ from direct writing.

**Important limits:** This provides atomic **visibility** of a
complete final path on the same filesystem, *not* crash-proof
durability: no file/directory `fsync` is issued. A crash or
kill during a frame can leave a `.part.*` staging file
for later cleanup, but will not publish it as the final
`.ppm`. It does not make an already exported image
synchronous, and it is unrelated to Vulkan/NGX GPU context
recovery.

Reproduce the A/B benchmark:

```sh
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
python3 tests/render/dlss_offscreen_pbo_benchmark.py \
  "$HOME/.cache/xyo-dlss-build/play/xyo" \
  --compare-atomic --frames 64 --fps 1000 --blocks 3 --scale 2
```

#### Opt-in fsync durability for atomic PPM capture (2026-11-10)

`XYO_SHOT_ASYNC_DURABLE=1` requests file and directory
durability for each asynchronous PBO screenshot on Linux. It
implicitly enables `XYO_SHOT_ASYNC_ATOMIC=1` and uses
the existing temporary-sibling publication. The write order is:

1. `mkstemp(final.ppm.part.XXXXXX)`, write the complete P6
   image, `fflush` and `fsync(fileno(file))`
2. `fclose`, then rename the completed temporary file to the
   final image path in the same parent directory
3. Open the parent directory with
   `O_RDONLY|O_DIRECTORY|O_CLOEXEC`, `fsync` the
   directory, and close it

The file fsync occurs **before** rename, so any failed image
write, flush, file fsync or close leaves the old complete final
file intact. A successful rename is visible even if the
**subsequent directory fsync fails**; in this case the process
reports failure (exit 2) but does not attempt to roll back or
delete an already published image. On a disk crash, its
durability may be uncertain. Disk-writer failures propagate
through the normal PBO finish path, with mapped GPU PBOs
still reclaimed by the render thread.

```sh
# Requires existing XYO_SHOT_SERIES and non-display-server
# capture environment; this flag automatically enables atomic PPM.
export XYO_SHOT_SERIES_ASYNC=1
export XYO_SHOT_ASYNC_RGB=1
export XYO_SHOT_ASYNC_DIRECT_MAP=1
export XYO_SHOT_ASYNC_DURABLE=1
export XYO_SHOT_ASYNC_STATS=1
```

An additional diagnostic line reports:
`[async PPM] durability enabled=1 file_fsyncs=128 dir_fsyncs=128 sync_failures=0`.

New native GPU CTest
`xyo-dlss-offscreen-pbo-durable` covers four combinations
of copied/direct mapped PBO and RGBA/RGB: 8 final-frame SHA256
comparisons in each mode against ordinary PBO output; successful
file and directory sync counts; and deterministic failures in
file fsync and directory fsync. Test-only
`XYO_SHOT_ASYNC_TEST_FAIL_SYNC=file` rejects publication
and preserves a pre-existing final file;
`XYO_SHOT_ASYNC_TEST_FAIL_SYNC=dir` rejects completion
*after* publishing complete images. Both failure modes
return exit 2 without leaving partial files or GPU PBOs
outstanding. Do not enable either fault flag in production.

A 128-frame 960x720 RGB direct-PBO offscreen NGX run saved
**128/128** full images (265.42 MB), with file-fsync count
128, directory-fsync count 128, zero errors and zero orphan
tempfiles. PBO created/deleted counts were 4/4.

Paired `3D2.sb3` RTX3070 measurements (each 32 frames,
8 alternating runs per size; whole-process wall time):

| Output | Atomic only | Atomic + fsync | Difference |
|---|---:|---:|---:|
| 960x720 | 0.8321 s | 0.8446 s | +1.51% |
| 1920x1440 | 0.8862 s | 0.8820 s | -0.48% |

These short measurements do not establish a reliable
performance advantage. On slower or networked storage,
per-frame file and directory fsync may be expensive; keep
durability opt-in. You can reproduce the paired test with
`--compare-durable --frames 32 --fps 1000 --blocks 2 --scale 2`
on `tests/render/dlss_offscreen_pbo_benchmark.py`.

**Guarantee limitations:** `fsync` is a request to the
operating system and storage stack, not proof against faulty
hardware, filesystems, controller caches or all crash windows.
A kill/crash *before rename* can leave a `.part.*`
file; the normal successful/error-return paths remove these.
Rename and directory fsync do not make a set of multiple
frames transactional: only each individual PPM is published
atomically. New files inherit `mkstemp`'s restrictive
0600 permissions. Windows/macOS are not supported by this
Linux-only PBO implementation.

After a full `xyo-cli` build, 14/14 capture/headless
GPU CTests plus the independent quantized GPU Pen test
passed (**15/15** relevant RTX3070 checks). No commit/push.

#### Opt-in stale PPM temporary-file recovery (2026-11-10)

On a hard process termination, an atomic/durable PPM capture may
leave an unpublished `.part.XXXXXX` temporary file behind.
`XYO_SHOT_ASYNC_RECOVER_STALE=1` enables a conservative
cleanup started at the first asynchronous frame capture.
The scan is now incremental and continues over subsequent captured
frames (see the later bounded-recovery section below), not in the
background. Without this flag, no stale-file scan or removal occurs.

Recovery searches only the parent directory of the active
`XYO_SHOT_SERIES` and only exact names of the form
`PREFIX-000001.ppm.part.A1b2C3`, matching the active
capture prefix, six decimal frame digits and the six-character
alphanumeric suffix generated by `mkstemp`. It removes
files only if all of these are true:

- They are more than **24 hours old** (mtime).
- They are ordinary files owned by the current effective user
  with exactly one hard link, not symlinks/directories.
- The file descriptor and directory entry refer to the same
  inode, verified before and after a non-blocking exclusive
  `flock`; the age is also rechecked after taking the lock.
- The temporary file is **not** locked by an active writer.
  New atomic captures hold `flock(LOCK_EX|LOCK_NB)`
  until the temporary file is closed.

Files outside this pattern, recent or active files, links and
existing final `.ppm` images are not deleted.
The current directory component is opened with
`O_DIRECTORY|O_NOFOLLOW` and candidates with
`openat(...,O_NOFOLLOW)`, reducing accidental symlink
traversal. Use a **private, trusted output directory**:
these checks are not a security boundary against malicious
concurrent filename swaps in a shared writable directory,
and old writers not participating in the `flock` protocol
cannot be identified as reliably. The 24-hour age cutoff
is intentionally fixed; there is no aggressive purge mode.

```sh
# With XYO_SHOT_SERIES=... already set to a private directory:
export XYO_SHOT_SERIES_ASYNC=1
export XYO_SHOT_ASYNC_ATOMIC=1  # or XYO_SHOT_ASYNC_DURABLE=1
export XYO_SHOT_ASYNC_RECOVER_STALE=1
export XYO_SHOT_ASYNC_STATS=1
```

Example log:
`[async PPM] recovery checked=7 removed=2 recent=1 locked=1 unsafe=3 errors=0 dir_entries=22 scan_ms=0.110`

The real RTX3070 regression
`xyo-dlss-offscreen-pbo-stale-recovery`
covers both copied and direct RGB PBO readback, verifies
that disabled recovery keeps old files, checks that exactly
two old matching files are removed, and preserves recent,
locked, other-prefix, malformed, symlink, hardlink, directory
and completed image files. The eight new frames are still
published atomically and have successful per-frame file/parent
directory fsync counts. The new test passed three consecutive
CTest repetitions after the final identity/age rechecks.
In a representative RTX3070 run, a directory of 22 entries
was checked in **0.110 ms** in copied mode and **0.069 ms**
in direct mode. This is a small example, not a benchmark
of very large directories.

After rebuilding the full `xyo-cli` target, all
**15/15** targeted Headless/DLSS/PBO CTests passed,
plus the separate quantized GPU Pen parity CTest
(**16/16** total checks on RTX3070). The independent
Pen test completed in 42.51 seconds. No commit/push.

**Limitations:** Stale recovery triggers only when the next
matching async frame is actually captured, not at process
startup with zero captured frames. It does not scan nested
directories, remove arbitrary temporary files or clean
non-six-digit frame numbers. It is not a guarantee of cleanup
after sudden power loss; no `fsync` of the deletion
directory is performed. The feature is opt-in and does not
change the regular PBO, non-atomic or default save paths.

#### Real SIGKILL recovery test for incomplete atomic PPM (2026-11-10)

The stale-part recovery now has a real process-crash integration
test, `xyo-dlss-offscreen-pbo-sigkill-recovery`
(`tests/render/dlss_offscreen_pbo_sigkill_recovery_smoke.py`).
A test-only environment variable,
`XYO_SHOT_ASYNC_TEST_STAGE_PAUSE_MS=8000`, makes the
atomic PPM writer `fflush` its first four image scanlines
and pause **while keeping the file descriptor and exclusive
`flock` open**. The test observes the partial file on
disk, confirms the live writer lock, then sends an actual
`SIGKILL` to the `xyo` process (not a graceful exit
or an injected C++ exception). The process exits -9 and
the Linux kernel releases its file lock automatically.

On RTX3070 with actual NVIDIA NGX/SDL offscreen output,
the copied and direct-mapped RGB-PBO writer each left
an **11,535-byte** staged P6 PPM file. No incomplete final
`frame-000001.ppm` was published, and a pre-existing
final image was not modified. Restarting immediately with
`XYO_SHOT_ASYNC_RECOVER_STALE=1` leaves the newly
orphaned temporary file untouched: it is still younger
than the fixed 24-hour cutoff. The test then backdates
only the orphan's `mtime` to two days earlier (it
does **not** actually wait 24 hours) and starts a second
fresh `xyo` process. This run recovers exactly one old
orphan, preserves the completed image, leaves no partials
and successfully publishes eight durable PPM frames with
matching file and parent-directory fsync counts, in both
copied and direct mapped variants.

This confirms real `SIGKILL` termination, kernel
lock release, partial-file survival and the next run's
age-gated cleanup. It does not simulate power loss,
GPU driver reset, or filesystem corruption. The pause
variable is strictly test-only and must never be set
in normal capture. Cleanup still occurs only on the first
actual captured asynchronous frame of a later run,
not at startup with no rendered frames.

The real `SIGKILL` recovery CTest passed **three
consecutive runs** with both copied/direct variants in each
run. After the full `xyo-cli` rebuild, all **16/16**
targeted Headless/DLSS/PBO CTests passed; the independent
quantized GPU Pen parity test also passed (39.69 seconds),
for **17/17** relevant RTX3070 GPU checks. No commit/push.

#### Concurrent capture-process safety and per-series locking (2026-11-10)

Two real multi-process NVIDIA NGX tests now cover the remaining
cross-process collision and stale-file recovery cases.

1. **Concurrent stale recovery**:
   `xyo-dlss-offscreen-pbo-concurrent-recovery` launches an
   actual `xyo play` writer with a flushed, partially written
   `.part.*` file and its exclusive `flock` held.
   While the writer is alive, the test backdates the file's
   modification time by 48 hours and starts a *second* NGX
   capture process with `XYO_SHOT_ASYNC_RECOVER_STALE=1`.
   Recovery observes `checked=1 removed=0 locked=1`:
   the artificially old but active file is not unlinked.
   After the first process receives a real `SIGKILL`,
   the next capture finds `removed=1 locked=0` and
   completes 8 durable PPM frames. Both copied and directly
   mapped PBO paths pass.

2. **Optional exclusive per-prefix writer lock**:
   `XYO_SHOT_ASYNC_LOCK_SERIES=1` prevents two
   cooperating processes from writing
   `PREFIX-000001.ppm`, etc., into the same directory
   concurrently. The first capture opens a persistent
   `.PREFIX.xyo-capture.lock` file (mode 0600) in its
   output directory using `O_NOFOLLOW|O_CLOEXEC`,
   checks that the file is owned by the current user and
   has exactly one link, and acquires
   `flock(LOCK_EX|LOCK_NB)`. It holds the lock until
   the disk writer has been joined and the capture object
   is destroyed. Another process with the same opt-in
   mode and prefix reports
   `[async PPM] capture series already active` and
   exits with status 2 rather than overwriting frame
   files. **Different prefixes in the same directory
   remain independent.** The lockfile intentionally
   remains after exit: removing it would permit an
   inode-replacement race between cooperating processes.
   A true `SIGKILL` releases the advisory lock
   automatically and a new owner can safely acquire it
   and perform stale PPM cleanup. Existing symlinks
   used as lockfiles are rejected.

```sh
# Stronger opt-in protection for an exclusive capture series:
export XYO_SHOT_SERIES=/path/to/private/output/frame
export XYO_SHOT_SERIES_ASYNC=1
export XYO_SHOT_ASYNC_ATOMIC=1
export XYO_SHOT_ASYNC_RECOVER_STALE=1
export XYO_SHOT_ASYNC_LOCK_SERIES=1
export XYO_SHOT_ASYNC_STATS=1
```

The separate CTest
`xyo-dlss-offscreen-pbo-series-lock` verifies identical-prefix
conflict, independent-prefix success, real owner `SIGKILL`,
successful lock reacquisition and orphan recovery, and
symlink lockfile rejection in both copied and direct
RGB-PBO configurations. On an RTX3070 all scenarios passed.

**Limitations:** `flock` is a cooperative advisory lock.
A writer that does not opt in, or a process that directly
manipulates files, can still overwrite the same prefix.
Use a trusted output directory; a malicious process with
write access to the directory can replace/remove the
lock filename and defeat pathname-based coordination.
No guarantee is made for distributed/NFS lock semantics.
Neither series lock nor recovery is enabled by default.

After rebuilding `xyo-cli` (LLVM 23.1.4, real NGX),
both new two-process CTests passed three consecutive
repetitions each. The complete targeted
Headless/DLSS/PBO GPU suite passed **18/18**, and
the separate quantized GPU Pen parity test passed
(40.45 seconds): **19/19** relevant native RTX3070 checks.
The full build emitted existing LLVM deprecation and
make jobserver FIFO warnings but completed successfully.
No commit/push.

#### Bounded incremental stale-recovery scan (2026-11-10)

The previous opt-in stale `.part.*` recovery scanned an
**entire directory on the first captured frame**, which could
cause a large first-frame delay when thousands of unrelated
files are present. `XYO_SHOT_ASYNC_RECOVER_STALE=1`
now creates a single `DIR*` cursor and processes a
limited number of entries **per captured frame**:

- At most **128 directory entries** in a single capture call.
- A **soft target of 1 ms** of scanning per call. A single
  blocking filesystem syscall or candidate-file metadata
  operation can exceed this time; it is *not* a real-time
  deadline.
- The directory cursor is retained across frames, preserving
  progress without materializing filenames in memory.
- At end-of-directory, the scan closes its cursor. If a run
  finishes before scanning the entire directory, it closes
  the cursor on cleanup and marks the operation as
  **incomplete**. The next capture session starts a fresh
  scan (not a persistent bookmark).
- All existing checks remain in effect: matching prefix,
  6-digit frame ID and 6-character `mkstemp` suffix,
  24-hour minimum age, owner and link-count checks,
  symlink avoidance, inode rechecks and exclusive
  `flock` protection of active writers. The optional
  per-prefix writer lock is independent.

`XYO_SHOT_ASYNC_STATS=1` retains the earlier cumulative
`recovery checked/removed/recent/locked/unsafe/errors/dir_entries/scan_ms`
log and adds:

```text
[async PPM] recovery_progress complete=1 steps=26 max_step_entries=128 max_step_ms=0.218 pending=0
```

A new native test,
`xyo-dlss-offscreen-pbo-incremental-recovery`,
creates 3,200 unrelated files and three old matching
temporary files. In real NVIDIA NGX/RTX3070 tests:

| Metric | Copied RGB-PBO | Direct-mapped RGB-PBO |
|---|---:|---:|
| Single-frame directory entries | 128 | 128 |
| Entries in complete scan | 3,206 | 3,206 |
| Captured frames needed to finish scan | 26 | 26 |
| Maximum entries per step | 128 | 128 |
| Maximum observed step duration | 0.218 ms | 0.518 ms |
| Old PPM files recovered | 3/3 | 3/3 |
| PPM frames saved in long run | 48/48 | 48/48 |

The one-frame capture verifies `complete=0,pending=1`;
the 48-frame capture verifies eventual completion, zero
recovery errors, complete atomic PPM output and no deletion
of unrelated files. The test explicitly re-creates stale
fixtures between phases because `readdir` does not
guarantee directory order; even a partial scan may find
eligible stale files immediately. The test passed
three consecutive CTest repetitions after this fix.

The full `xyo-cli` build succeeded. Targeted Headless/
DLSS/PBO tests passed **19/19**, and the independent
quantized GPU Pen parity passed (38.86 s), yielding
**20/20** native RTX3070 regression tests.

**Limitations:** the scan runs only on frames with active
asynchronous PBO capture, not all render frames or an
unrelated background task. A short capture in a very large
directory can leave eligible stale files for the next run.
`readdir` is not a snapshot, so concurrent directory
mutations may be missed until a later run. Slow storage or
a single blocked syscall can still exceed the 1 ms target.
Default behavior with `XYO_SHOT_ASYNC_RECOVER_STALE`
unset is unchanged. No commit/push.

#### Allocation-free recovery filename filter (2026-11-10)

The incremental PPM recovery scan now avoids copying every
`readdir` filename into a new `std::string` object.
Each entry is examined as a non-owning `std::string_view`
within the lifetime of the `dirent` returned by
`readdir`. Candidate metadata lookups still use
the original NUL-terminated `d_name` pointer. Only
entries matching the strict series/frame/tempfile format
perform the wall-clock `time()` check and the preexisting
`fstatat/openat/flock/unlinkat` validation.
The 1-ms soft deadline's monotonic-clock check runs every
16 entries rather than on every unrelated filename. The
existing **128 directory entries per captured frame hard
cap**, error counters, 24-hour minimum age, inode/owner/link
validation and cooperative locking are unchanged.

After the full `xyo-cli` rebuild, the ordinary 3,200-file
GPU integration test processed 3,206 directory entries in
26 frames, deleted all three stale PPM temps and saved
48/48 frames in both copied and direct-PBO modes. The
test passed three consecutive CTest repetitions.
An optional larger regression path is reproducible with:

```sh
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
python3 tests/render/dlss_offscreen_pbo_incremental_recovery_smoke.py \
  "$HOME/.cache/xyo-dlss-build/play/xyo" --large-only
```

This test creates **25,000** unrelated files, captures
one frame with each PBO mode, and asserts that no more
than 128 entries are scanned per run. A representative
RTX3070 invocation measured 0.157 ms (copied PBO) and
0.145 ms (direct PBO); a later repeat measured 0.385 and
0.786 ms. All remained below 1 ms *in these runs*,
but filesystem stalls may exceed the target, and the
test does **not** demonstrate a repeatable A/B speedup
over the old allocation-heavy scanner.

The full targeted Headless/DLSS/PBO GPU suite passed
**19/19**, with the independent quantized Pen parity
test also passing (40.72 s), for **20/20** relevant
RTX3070 GPU checks. The optimization is active only
when opt-in `XYO_SHOT_ASYNC_RECOVER_STALE=1`
is already enabled. No commit/push.

#### Optional background-thread stale PPM recovery (2026-11-10)

The bounded incremental cleanup above remains the default whenever
`XYO_SHOT_ASYNC_RECOVER_STALE=1`. A new, **separate opt-in**
`XYO_SHOT_ASYNC_RECOVERY_THREAD=1` moves *all*
recovery `opendir/readdir/fstatat/openat/flock/unlinkat`
work onto one dedicated SDL thread. The render thread only
launches it once on the first asynchronous captured frame.
No OpenGL operations are performed by the recovery thread.
The existing exact prefix pattern, 24-hour mtime threshold,
owner/link-count/inode validation and live-writer `flock`
protection are unchanged. Both PBO-copy and direct-mapped
write paths are supported.

```sh
export XYO_SHOT_SERIES_ASYNC=1
export XYO_SHOT_ASYNC_ATOMIC=1  # Or XYO_SHOT_ASYNC_DURABLE=1
export XYO_SHOT_ASYNC_RECOVER_STALE=1
export XYO_SHOT_ASYNC_RECOVERY_THREAD=1
export XYO_SHOT_ASYNC_STATS=1
```

This is compatible with `XYO_SHOT_ASYNC_LOCK_SERIES=1`.
The capture-series lock is acquired **before** the recovery
thread starts. This avoids racing another cooperating writer.
The scanner uses the existing 128-entry/1-ms-soft-target
batches but runs these batches independently of the number
or cadence of rendered frames. If the run ends early,
`finish()` requests cancellation, joins the SDL thread,
closes its directory cursor and only then reads its counters.
Destruction also joins it. No recovery state is read on the
render thread before joining, so the worker's non-atomic
statistics/cursor are not accessed concurrently.

`XYO_SHOT_ASYNC_STATS=1` adds:

```text
[async PPM] recovery_thread enabled=1 started=1 joined=1 foreground_ms=0.070
```

The real RTX3070 test
`xyo-dlss-offscreen-pbo-background-recovery` covers
3,200 unrelated entries plus seven protected/eligible
candidates in both RGB copied- and direct-PBO modes.
The recovery thread scanned 3,215 entries, completed in
26 steps, removed exactly two old orphaned PPM files,
preserved one recent, one actively locked and three
unsafe candidates, while 48/48 atomic/durable frame
outputs succeeded. With a **test-only** simulated
250-ms directory-operation delay
(`XYO_SHOT_ASYNC_TEST_RECOVERY_PAUSE_MS=250`),
the time spent by the render thread on recovery changed:

| RGB PBO | Foreground recovery | Recovery in separate SDL thread |
|---|---:|---:|
| Copied PBO | 250.2 ms | 0.070 ms |
| Direct-mapped PBO | 250.2 ms | 0.165 ms |

These values are the accumulated render-thread time in
the recovery call path, **not** full frame wall-clock time.
The latter can still include waiting for the recovery
thread to stop during final flush. The artificial delay
is only a reproducible concurrency test, not a measured
performance improvement on real slow storage.
The test passed three consecutive CTest runs. It was
later extended with fault injection: an artificial GPU
readback failure while the recovery worker is active
must still exit with status 2, join the recovery thread
and leave no PBO lifecycle mismatch. This extended test
passed two consecutive additional CTest repetitions in
both copied/direct PBO modes.

The implementation uses `SDL_CreateThread` and
`SDL_WaitThread`, not C++ exception handling, because
this project is compiled with `-fno-exceptions`.
If the scanner cannot start, capture continues (with a
reported recovery error) rather than retrying every frame.
A live scanner's blocking filesystem syscall is not
interruptible by the cooperative stop flag: shutdown
may wait for the current operation, so no hard
real-time latency or filesystem-durability guarantee is
claimed. The background mode is off by default, and
the foreground recovery policy is still available.

After a full CMake `xyo-cli` rebuild, all **20/20**
targeted Headless/DLSS/PBO native tests passed. A final
one-line guard was then added to prevent repeated attempts
if recovery thread creation fails; that change was rebuilt
successfully, the background recovery test was repeated
three times, and the independent quantized GPU Pen parity
test passed (40.44 seconds). The extended GL-failure
version also passed two repeats. Changes remain
uncommitted/unpushed.

#### Measured recovery I/O contention during real NGX rendering (2026-11-10)

To distinguish the previous synthetic 250-ms pause from actual
filesystem contention, added
`tests/render/dlss_offscreen_pbo_recovery_contention_benchmark.py`.
It creates separate fresh output directories per trial, with a
configurable count of unrelated filenames and old matching
`frame-XXXXXX.ppm.part.XXXXXX` files (mtime set to 2 days
ago), then launches the same
`debug/projects/3D2.sb3` on real NVIDIA NGX/RTX3070.
All runs use RGB direct-mapped PBOs and atomic publication;
durability is separately opt-in. Three interleaved conditions
are measured:

- **off:** do not scan or delete old PPM temp files;
- **foreground:** bounded scan, at most 128 entries per captured frame;
- **background:** recover through the SDL worker with a joined
  shutdown.

The benchmark verifies the saved frame count, atomic PPM
publishing, and complete recovery of every eligible old file
in the two enabled modes. It reads `XYO_PLAY_PROFILE`
to report per-trial draw/step p50, p95, p99 and early-frame
p95, plus total process wall time, scan time and render-thread
recovery time. `--output-csv` preserves complete trial
statistics for later analysis, separately from any checked-in
source artifacts.

| Workload | Off step p95 | Foreground step p95 | Background step p95 |
|---|---:|---:|---:|
| 960x720, 64 frames, atomic, 600 old + 3,200 unrelated | 13.846 ms | 13.713 ms | 13.717 ms |
| 1920x1440, 48 frames, durable fsync, 600 old + 3,200 unrelated | 14.286 ms | 14.352 ms | 14.161 ms |
| 960x720, 96 frames, durable fsync, 3,500 old + 1,000 unrelated | 13.682 ms | 14.078 ms | 13.962 ms |

The first two rows are means of four interleaved runs per
condition; the high-deletion row has only two per condition.
Actual CPU-side recovery scanning took roughly 3.4–4.9 ms
total for 600 old files, and ~17–22 ms for 3,500 old
files. In background mode, the render thread spent
~0.08–0.10 ms total **launching** the recovery worker,
versus several or tens of milliseconds of foreground
scan work. This is **not** the same as total GPU time
or the complete process wall time.

All 600/3,500 eligible old staging files were removed
when recovery was enabled; the off runs deliberately
left them in place. Complete PPM output was validated
for every trial, including durable fsync where requested.
The latest measured 3,500-old-file workload saw a small
p95 increase vs recovery-off in both foreground and
background modes; these short and variable GPU runs do
not establish reliable performance penalties or speedups.
As of this measurement, further speculative I/O thread
priority changes are not justified by the evidence.

Reproduction (replace the binary location if needed):

```sh
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
python3 tests/render/dlss_offscreen_pbo_recovery_contention_benchmark.py \
  "$HOME/.cache/xyo-dlss-build/play/xyo" \
  --frames 64 --blocks 2 --stale 600 --irrelevant 3200 --scale 2 \
  --output-csv "$HOME/.cache/xyo-dlss-build/recovery-contention-960x720.csv"

# Same study with higher pixel data volume and durable fsync:
# --frames 48 --blocks 2 --scale 4 --durable
# High unlink volume:
# --frames 96 --blocks 1 --stale 3500 --irrelevant 1000 --durable
```

Raw per-trial CSVs were written locally to:

- `$HOME/.cache/xyo-dlss-build/recovery-contention-960x720.csv`
- `$HOME/.cache/xyo-dlss-build/recovery-contention-1920x1440-durable.csv`
- `$HOME/.cache/xyo-dlss-build/recovery-contention-3500-stale-durable.csv`

**Interpretation limits:** The profile's `step_ms` and
`draw_ms` are CPU-side engine measurements, not GPU
timer-query timestamps. Process wall time includes NGX setup,
startup/shutdown and final PBO flush. OS page cache, GPU
clocking and concurrent workstation activity were not fully
controlled; no statistically robust speedup is claimed.
The benchmark is manual only, not a timing-sensitive CTest.

#### Cooperative recovery cancellation inside directory batches (2026-11-10)

The opt-in SDL background stale-file scanner previously
checked its stop request only **between** batches of up to
128 `readdir` entries. A batch has a soft 1-ms deadline,
but the clock is checked every 16 entries, and each matching
candidate can involve multiple filesystem syscalls. Under slow
filesystem operations, `finish()` could therefore remain
blocked in `SDL_WaitThread` while the scanner carried on
processing unrelated entries after cancellation.

The worker now checks `recoveryStopRequested_` before
**each directory entry** and before opening an old candidate
for deletion. Cancellation immediately stops the remainder
of the batch; the recovery cursor is closed by the worker
before its thread exits. The foreground scanner remains
unaffected, and the filename/ownership/age/flock safety
requirements are unchanged. The stop signal uses atomic
release/acquire ordering; statistics are still read only
after joining the worker.

`XYO_SHOT_ASYNC_STATS=1` adds:

```text
[async PPM] recovery_shutdown joined_ms=19.270 cancel_observed=1
```

The GPU regression
`xyo-dlss-offscreen-pbo-background-recovery` now injects
a **test-only 20-ms per-entry delay**, controlled by
`XYO_SHOT_ASYNC_TEST_RECOVERY_ENTRY_DELAY_MS=20`.
It verifies an eight-frame durable PPM capture, a
pending background recovery scan, a cancellation
observed by the worker, no worker leak, and scanner
thread join under 150 ms. One actual RTX3070 run reported
19.27 ms join for copied RGB-PBO and 0.14 ms for
direct-mapped RGB-PBO, with only one directory entry
scanned in each case. The test correctly allows zero
entries when cancellation happens during the very first
simulated slow I/O operation. This was initially caught
as a test-assertion race during the full CTest suite;
the assertion was corrected and the dedicated test then
passed **five consecutive runs**.

After the correction, full native GPU validation completed:
**20/20** Headless/DLSS/PBO tests passed, and the
independent quantized GPU Pen parity test passed
(38.56 seconds), yielding **21/21** relevant RTX3070
tests. `xyo-cli` rebuilt successfully before these
tests. No commit/push.

**Limitations:** the scanner still cannot interrupt an
individual kernel filesystem operation that is already
blocked. The 150-ms threshold is a fault-injection
regression bound, **not** a production shutdown latency
guarantee. The new diagnostics and per-entry delay are
not enabled by default. Avoid setting the test-delay
environment variable during ordinary capture.

#### Real Btrfs/NVMe shutdown contention and early scanner cancellation (2026-11-10)

The background scanner's cooperative stop checkpoints were
previously in place, but `finish()` still signaled the
scanner **after waiting for the entire PPM disk-writer
queue** to drain. Under a slow writer, this allowed
unnecessary metadata/deletion traffic to continue while
the final screenshots were being saved.

`GlAsyncPpmCapture::finish()` now signals
`requestRecoveryStop()` immediately, *before*
draining final PBO readbacks; it then **joins the recovery
thread before joining the disk writer**. Destruction
and the no-valid-GL-context path also join the
recovery worker before the writer. Recovery state
continues to be read only after a join, and all PBO
unmaps/deletions still occur only after the disk writer
has finished borrowing mapped GPU buffers. The
cooperative cancellation checks and file validation
rules are unchanged.

`XYO_SHOT_ASYNC_STATS=1` now includes the number
of submitted screenshots not yet successfully saved
when the scanner stop was requested:

```text
[async PPM] recovery_shutdown joined_ms=4.220 cancel_observed=1 pending_images_at_stop=4
```

The extended background recovery regression
(`tests/render/dlss_offscreen_pbo_background_recovery_smoke.py`)
adds a 16-frame real RTX3070 NGX/DLSS durability case,
with test-only 80 ms per-frame writer delay and
5 ms per-entry scanner delay. It verifies that
cancellation is observed while unfinished images
are still queued, the recovery thread joins safely,
and every final PPM is published correctly:

| RGB PBO mode | Images not yet saved at scanner stop | Scanner join | Durable output |
|---|---:|---:|---:|
| Copied PBO | 10/16 | 0.01 ms | 16/16 PASS |
| Direct PBO | 4/16 | 4.22 ms | 16/16 PASS |

These are one representative run's measurements,
not a guarantee of thread scheduling or hard
shutdown latency.

A new manual real-disk benchmark,
`tests/render/dlss_offscreen_pbo_real_io_shutdown_benchmark.py`,
exercises actual concurrent file writes with
`fdatasync` and rapid file creation/deletion
on the **same NVMe Btrfs filesystem** as the
temporary screenshot files. It compares idle/stressed
capture, copied/direct RGB PBOs and the
`recovery_shutdown joined_ms` telemetry.
It is intentionally rate-limited and bounded:
two default worker threads, each capped at 512 MiB
logical writes (the experiment used 256 MiB per
writer), 8 MiB batches, and a 25 ms pause between
sync operations. This also avoids massive unintended
CoW write amplification. The stress workload uses
temporary directories and cleans them afterward.

An eight-trial, four-frame-per-trial RTX3070 run,
with 2,500 old matching temporary files and 800
unrelated entries per run, measured:

| PBO mode | Idle join mean | Stressed join mean | Stressed join maximum |
|---|---:|---:|---:|
| Copied | 0.005 ms | 0.004 ms | 0.004 ms |
| Direct | 0.003 ms | 0.003 ms | 0.004 ms |

The stressed runs issued ~304–336 MiB of real
`fdatasync` writes and ~59,000–62,000 metadata
create/unlink operations *per trial*. All 4 PPM
frames/trial were atomically published and fsynced;
the old-file scanner was **still pending** on
every run of this post-fix comparison. Its
shutdown wait did not meaningfully grow in this
experiment. A previous exploratory unbounded
disk-stress run is superseded by the capped version.

The full `xyo-cli` rebuild succeeded with
existing unrelated LLVM deprecation warnings.
Headless/DLSS/PBO tests passed **20/20**,
and independent quantized GPU Pen parity
passed (38.13 seconds), for **21/21** relevant
RTX3070 regression checks.

Reproduce the *capped* I/O contention experiment:

```sh
XYO_DLSS_FEATURE_DIR="$HOME/.cache/xyo-dlss-sdk/lib/Linux_x86_64/rel" \
python3 tests/render/dlss_offscreen_pbo_real_io_shutdown_benchmark.py \
  "$HOME/.cache/xyo-dlss-build/play/xyo" \
  --trials 2 --frames 4 --stale 2500 --unrelated 800 \
  --write-threads 2 --max-mib-per-writer 256 --io-pause-ms 25 \
  --output-csv "$HOME/.cache/xyo-dlss-build/recovery-real-io-shutdown-v2.csv"
```

**Limitations:** the benchmark generates genuine
write/sync and metadata load but does not simulate
power failure, uninterruptible device I/O, a
failed controller or saturated network storage.
Its short-run time differences are not predictive
tail-latency bounds. The scanner still cannot
cancel a kernel operation once it blocks.
All advanced capture/recovery options remain
opt-in. No commit/push.

#### Stop-aware stale candidate deletion at the final unlink boundary (2026-11-11)

The background stale PPM recovery scanner already checked a
cooperative cancellation flag before individual `readdir`
operations and before opening old candidate files. However, a
stop request could still arrive **after** inode/owner/mtime
verification and `flock` acquisition but **before**
`unlinkat`. In that narrow interval, the scanner could issue
one more unnecessary metadata deletion while the final
PPM writer was flushing output.

Recovery now checks the stop flag again **immediately before
`unlinkat`**. If cancellation is pending, it skips deletion,
releases the file descriptor and its exclusive lock, records
`recoveryPreUnlinkSkipped_`, and exits the current batch.
The next run can recover the unchanged old file. The new
check is only relevant to the opt-in background scanner;
foreground recovery still scans according to its existing
rules. Existing owner, hardlink, symlink, age, inode and lock
verification remains in effect.

The statistics line gains `preunlink_skipped`:

```text
[async PPM] recovery_shutdown joined_ms=0.010 cancel_observed=1 pending_images_at_stop=1 preunlink_skipped=1
```

A new fault-injection case inside
`tests/render/dlss_offscreen_pbo_background_recovery_smoke.py`
uses `XYO_SHOT_ASYNC_TEST_PRE_UNLINK_PAUSE_MS=2500`.
The worker reaches a real, locked stale candidate and
enters a test-only pause **after** validating the candidate
but before attempting deletion. The render thread waits
for this point only when the test flag is set, then
finishes its capture and requests cancellation.
The test asserts cancellation was observed,
`preunlink_skipped >= 1`, and that the old PPM part
still exists. A second real NVIDIA NGX process then
removes the old file normally. Both copied and direct
RGB-PBO configurations passed, saving 2/2 durable images
before stopping and 12/12 after restart.

After full `xyo-cli` rebuild, the extended background
recovery CTest passed **3 consecutive executions**.
All **20/20** targeted Headless/DLSS/PBO GPU regression
tests passed and the independent quantized GPU Pen test
passed (38.21 s), for **21/21** relevant RTX 3070 tests.

The test-only pause is disabled by default. This is
cooperative cancellation and **does not guarantee**
safety against a malicious file rename in an untrusted,
shared writable directory; it also cannot stop an
already-running kernel `unlinkat` operation.
No commit/push.

#### Crash-durable deletion of recovered PPM staging files (2026-11-11)

The original stale PPM recovery removed eligible `.part.*`
files via `unlinkat`, but did not synchronize the
**parent directory** after deleting them. As with creating
or renaming an entry, a successful unlink alone does not
provide a crash-durability guarantee. This matters when
`XYO_SHOT_ASYNC_DURABLE=1` is specifically requested.

Recovery now uses the existing open `DIR*` descriptor
to synchronize recovered removals **in batches of 64**:
`fsync(dirfd(recoveryDir_))` runs after each 64
successful unlinks, and once at directory cursor close
for any remaining removals, provided durable capture
is explicitly enabled. This
covers both normal end-of-directory and a partial scan
terminated by an early stop request. It avoids an
additional sync for every removed PPM and does not
change the foreground/worker selection or the per-frame
128-entry scan budget. No directory sync is attempted
if no file was deleted or durability was not requested.

Diagnostics with `XYO_SHOT_ASYNC_STATS=1` include:

```text
[async PPM] recovery_durability enabled=1 dir_fsyncs=1 synced_removals=2 sync_failures=0
```

A failed deletion-directory sync increments the dedicated
`sync_failures` and the generic recovery error counter,
but does not falsely claim `synced_removals`. It is
reported as a **best-effort cleanup error** rather than
a screenshot-write failure: the unlink has already
become visible and completed output PPMs remain valid.
Failure does *not* roll back a completed deletion.
A separate test-only
`XYO_SHOT_ASYNC_TEST_FAIL_RECOVERY_SYNC=1`
forces this error without affecting the existing per-frame
`XYO_SHOT_ASYNC_TEST_FAIL_SYNC` fault hooks.

The real RTX3070 integration regression
`xyo-dlss-offscreen-pbo-background-recovery` now verifies:

- With 3,200 unrelated filenames and two old orphaned
  PPMs, the full recovery scan unlinks both files and
  performs **one** deletion-directory `fsync`, while
  preserving recent/locked/link candidates.
- A controlled stop *after* `unlinkat` but before
  finishing the scan persists exactly one deletion
  with one directory sync on shutdown.
  `XYO_SHOT_ASYNC_TEST_POST_UNLINK_PAUSE_MS=2500`
  is a test-only synchronization hook.
- The legacy **foreground** incremental recovery test
  also removes two stale files and synchronizes their
  directory **once**. Running recovery again with no
  removable files produces **zero** extra directory fsyncs.
- A simulated deletion-directory sync failure is
  explicitly reported, records zero durable removals
  and does not corrupt or prevent complete PPM image
  publication.
- Both copied and directly mapped RGB-PBO paths pass,
  including 2/2 early-stop and 16/16 failure-injection
  durable frame outputs.

Full `xyo-cli` rebuild succeeded. The targeted
Headless/DLSS/PBO regression suite passed **20/20**,
and the independent quantized GPU Pen parity test
also passed (38.20 seconds), for **21/21** relevant
RTX3070 native checks.

**Limits:** The directory fsync occurs every 64 successful
unlinks and at completion/shutdown for a final partial
batch, not after every unlink. A sudden power loss
between successful fsyncs can leave up to 63 deletions
with unproven durability; a sync error can leave more.
Directory fsync is supported on the tested local
Linux/Btrfs filesystem, but behavior varies by
filesystem and mount. Syncing deleted entries can
block the calling thread (foreground mode) or the
background recovery worker during its final join.
No commit/push.

#### 64-unlink durability checkpoints and real SIGKILL (2026-11-11)

The stale PPM scanner now performs one parent-directory
`fsync` after each **64 successful** old-file unlinks
when `XYO_SHOT_ASYNC_DURABLE=1`. If the scanner
completes or is cancelled, remaining 1–63 deletions
are also synced. A sticky fsync failure is reported
once; subsequent visible deletions are never falsely
counted as durable. Atomic-only capture does not
perform these recovery syncs.

`XYO_SHOT_ASYNC_STATS=1` includes
`sync_batch=64` and `max_unsynced` alongside
`dir_fsyncs`, `synced_removals` and
`sync_failures`. On RTX3070, recovering 130 old
files used **3 syncs (64+64+2)**, with
`max_unsynced=64`; injected first-batch sync
failure caused one error and zero durably counted
removals. All normal PPM frames were still saved.

The native CTest
`xyo-dlss-offscreen-pbo-recovery-batch-sigkill`
uses test-only
`XYO_SHOT_ASYNC_TEST_AFTER_RECOVERY_BATCH_PAUSE_MS=8000`
to pause after the first successful 64-file fsync.
The controller confirms that exactly 66 of the
initial 130 old temporary files remain and sends
a real `SIGKILL`. On restart, 66/66 old files
are recovered with two further syncs (64+2), and
24/24 durable RGB PPM frames are saved. Both copied
and direct PBO paths pass. The CTest repeated
**5 consecutive times** without failure.

The full targeted Headless/DLSS/PBO suite passed
**21/21** after the full `xyo-cli` build; the
independent quantized Pen GPU test also passed in
38.41s, for **22/22** relevant RTX3070 checks.

This verifies a real process crash **after fsync**
and subsequent recovery, not actual power-loss
persistence. One earlier broad background test
exited with SIGSEGV after printing successful
capture statistics; the next three repetitions,
the extended 21-test suite and crash-checkpoint
repetitions passed. Its cause was not established,
so it is not claimed fixed. The pause hook and
all enhanced recovery options are off by default.
No commit/push.

#### JIT teardown SIGSEGV follow-up (2026-11-11)

The earlier transient JIT teardown crash was investigated using
**four** historical core dumps, all with the same
`std::shared_ptr::_M_release -> JitEngine::~JitEngine ->
Runtime::~Runtime` stack. The original core's binary
does not match the currently rebuilt ELF; the exact
invalid shared pointer remains unidentified.
A concrete ownership-order hazard was corrected in
`src/jit/jit.cpp`: stop/join compilation before
member cleanup and destroy compiled-code caches before
their LLVM backend and AOT slot owners. This should not
be described as a proven root-cause fix for the old
crashes.

Added two real RTX3070 NGX GPU CTests covering
JIT Off/HotSpot/Eager shutdown with confirmed native
compilation, also with concurrent durable async PBO
capture and orphan recovery. After full CLI rebuild,
the new CTests passed 144 and 24 subprocess invocations
respectively; existing Headless/DLSS/PBO regression
21/21 and independent quantized GPU Pen parity PASS,
for 24/24 focused GPU CTests. Detailed evidence,
limitations and reproduction commands are in
`docs/jit-teardown-lifetime-2026-11-11.md`.

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