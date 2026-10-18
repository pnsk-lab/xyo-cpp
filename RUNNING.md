# 実行時依存と使い方

## Linux x86-64

Linux 版は 2 種類あります。どちらも **LLVM は静的リンク**なので
`libLLVM` のインストールは不要です。

### 1. `xyo-linux-x86_64-static.tar.gz` — 完全静的（推奨）

同梱: `xyo`（LLVM JIT + ランチャー以外の CLI。**動的依存ゼロ**）

```sh
./xyo run <project.sb3> --frames N   # ヘッドレス実行
./xyo test <fixture.sb3|dir>         # フィクスチャ検証
./xyo aot <project.sb3> -o <app>     # 単体実行ファイルを書き出し
```

- `ldd`/`readelf -d` で NEEDED が 0（glibc も静的）。どの x86-64 Linux でも動きます。
- `play` / `gui`（ウィンドウ表示）は **含みません** — SDL2 を使うためです。
- Vulkan compute は `libvulkan.so.1` があれば実行時に読み込んで使います（無くても動作）。

### 2. `xyo-linux-x86_64.tar.gz` — ウィンドウ表示つき

同梱: `xyo`（JIT + SDL2 レンダラ + ランチャー GUI + Vulkan compute）

動的リンクの依存（LLVM は静的リンク済み）:

| ライブラリ | パッケージ例（Ubuntu 24.04） |
|---|---|
| `libSDL2-2.0.so.0` | `libsdl2-2.0-0` |
| `librsvg-2.so` / cairo / pango / glib | `librsvg2-2` |
| `libGL.so.1`（GPU ペン層） | `libgl1` |

```sh
./xyo play <project.sb3>             # ウィンドウで再生
./xyo                                # （引数なし）ランチャー GUI
```

## Windows x86-64（xyo-windows-x86_64.zip）

同梱: `xyo.exe`（インタプリタ + Vulkan compute、ヘッドレス）

```sh
xyo.exe run <project.sb3> --frames N
xyo.exe test <dir|file>
xyo.exe play <project.sb3>           # SDL2 付きビルドでのみ利用可
```

- Vulkan（`vulkan-1.dll`）があれば GPU 並列ループが有効になります。
- JIT は LLVM for Windows が必要なため、このビルドでは無効（interp）です。
- UTF-8 のパス・コンソール出力に対応しています（日本語ファイル名の sb3 も可）。

## ビルド

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```
