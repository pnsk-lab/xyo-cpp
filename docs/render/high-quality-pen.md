# High Quality Pen (TurboWarp-compatible)

TurboWarp's High Quality Pen draws the persistent pen layer at the stage's
display resolution instead of its logical stage resolution. This includes
points, lines, stamps, transparency and redraws, with the Scratch-coordinate
system and pen diameter kept unchanged.

Enable it with:

```sh
./build/xyo play project.sb3 --scale 2 --high-quality-pen
# Or enable it for the player and embedded editor:
XYO_HIGH_QUALITY_PEN=1 ./build/xyo play project.sb3
```

The equivalent `--hq-pen` alias is also accepted. Off by default (vanilla
Scratch compatibility). The rendering API also exposes
`SdlRenderer::setHighQualityPen(bool)`, `highQualityPen()`, and
`penResolution()`. Switching the mode at runtime or resizing a window
resamples the existing premultiplied pen image instead of erasing it.

A 480×360 stage displayed at 960×720 uses a 960×720 pen buffer in HQ mode.
On HiDPI screens, the SDL renderer's output-to-window ratio is taken into
account. Window resizing recalculates pen dimensions; a maximum of 8192
pixels per dimension and SDL texture-size limits avoid unbounded allocations.
The CPU rasterizer, both OpenGL stroke paths, GPU compute stroke rendering,
and GPU/CPU stamping use the same scaled physical pen geometry.

Be aware that higher pen resolution increases VRAM and system memory use and
makes drawing and full-screen rendering more expensive. On resource-limited
systems, disable HQ pen for fastest rendering.

Tests: `ctest --test-dir build -R xyo-render-tests --output-on-failure`.
