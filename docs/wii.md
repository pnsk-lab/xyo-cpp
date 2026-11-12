# Nintendo Wii port (experimental)

This first version builds **interpreter-only** for the Wii Homebrew Channel.
It loads a Scratch 3 `.sb3` file from FAT SD/USB, runs the Scratch VM and
prints `say`/`think` results on the Wii text console.

**Not implemented:** Scratch stage drawing (sprites, PNG/SVG costumes,
pen), sound, editing, LLVM JIT, AOT, and WASM JIT. Therefore graphical
projects will *execute logic* without displaying their stage.

## Toolchain and build

Install devkitPro, Wii development packages and zlib, e.g.:

```sh
sudo dkp-pacman -S --needed wii-dev ppc-zlib
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
cmake -S wii -B build-wii \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Wii.cmake" \
  -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build-wii -j4
```

This configuration forces `XYO_JIT=OFF`, `XYO_SDL2=OFF` and disables
the desktop CLI and desktop tests. Native fiber switches deliberately
abort if invoked: the PowerPC fiber backend has not been implemented.
Expected result, once cross-linking succeeds: `build-wii/boot.dol`.

Place the files on the SD card:

```text
sd:/apps/xyo-cpp/boot.dol
sd:/apps/xyo-cpp/meta.xml
sd:/apps/xyo-cpp/project.sb3
```

Alternatively, the loader looks for `sd:/xyo/project.sb3` or
`usb:/apps/xyo-cpp/project.sb3`. Compressed projects are capped at
12 MiB; decoded asset and VM memory are additional to that.

Controls: **A** restarts the project, **HOME** quits, D-pad sends
Scratch arrow keys, **1** sends Enter and **2** sends Space.

The VM runs every other vertical refresh (30 ticks/sec on 60 Hz,
25 ticks/sec on 50 Hz). This is a logic/architecture test, not
a full graphical runtime.

## Current status and technical risks

The devkitPPC cross-compiler is not installed in the connected build
environment; the Wii target has **not been cross-compiled or run on Wii**.

- Scratch `Value` uses 64-bit NaN boxing while Wii pointers are 32-bit.
  CPU endianness, GC pointer tracing and 32-bit compilation must be
  validated on PowerPC.
- 24 MiB MEM1 + 64 MiB MEM2 is shared with IOS and OS allocations.
  Project size, GC heap, threads, and decompression need real-device
  memory measurements.
- Existing engine code uses parts of the standard C++ threading API;
  compatibility and available heap arenas depend on devkitPro/libogc.
- For full Scratch visuals: implement a Wii GX display backend, costume
  decoding and pen compositing; use a preprocessing strategy for SVG.
- A native JIT would require a PowerPC code-generation backend, cache
  synchronization and fiber context-switch implementation. Desktop LLVM
  JIT cannot simply be enabled.

This port is intentionally separate from existing desktop renderer/JIT
work. Do not interpret a successful desktop no-JIT compile as proof that
a `.dol` runs.
