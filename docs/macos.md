# macOS (Apple Silicon / Intel)

xyo-cpp は macOS の `arm64` (Apple Silicon) と `x86_64` (Intel) をネイティブでビルドする。
LLVM ORC HotSpot JIT、インタプリタ、AOT オブジェクト出力、SDL2 の `play` / `gui` を対象とする。

## Homebrew からビルド

Xcode Command Line Tools が必要:

```sh
xcode-select --install
brew install cmake ninja llvm sdl2 pkgconf
```

```sh
cmake -S . -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DXYO_LLVM_MODE=llvm-config \
  -DLLVM_CONFIG="$(brew --prefix llvm)/bin/llvm-config" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
  -DXYO_SDL2=ON -DXYO_REQUIRE_SDL2=ON \
  -DXYO_RSVG=OFF -DXYO_FULL_BUILD=ON
cmake --build build-macos --parallel 2
ctest --test-dir build-macos --output-on-failure
```

```sh
./build-macos/xyo run examples/play_demo.sb3 --frames 30
./build-macos/xyo play examples/play_demo.sb3
./build-macos/xyo aot examples/play_demo.sb3 --obj play.o
./build-macos/xyo run examples/play_demo.sb3 --aot play.o --frames 30
```

CPU 別のコンパイルが必要（ユニバーサルバイナリは作らない）。
`XYO_JIT_BACKEND=none` と `XYO_JIT=OFF` のビルドも可能。
GUI が不要なら `-DXYO_SDL2=OFF` を指定する。
`XYO_STATIC=ON` は macOS 非対応（システムの libc++ / dylib を静的リンクできない）。

## 依存機能と制約

- SDL2 ウィンドウ/音声は Homebrew の SDL2 に依存。SVG は既定の
  nanosvg で表示する。librsvg を入れた場合は `-DXYO_RSVG=ON` で利用可能。
- OpenGL ペン描画は Linux 専用。macOS は CPU ペン実装を使う。
- Vulkan の計算オフロードは任意機能。glslc と Vulkan-Headers が揃い、
  Vulkan loader または MoltenVK がロードできる場合にのみ有効。
  macOS の Metal を直接呼ぶバックエンドはまだ無い。
- リリースの macOS アーカイブは動的リンクの Homebrew 依存を含む。
  利用先でも `brew install llvm sdl2` が必要になる場合がある。
- 強化ランタイム (Hardened Runtime) で署名する場合は JIT 実行権限など、
  macOS のコード署名設定が別途必要になる場合がある。

GitHub Actions の `macos-15` (arm64) / `macos-15-intel` (x86_64) で
ビルド、JIT/AOT のスモークテストと主要なテストを行う。
