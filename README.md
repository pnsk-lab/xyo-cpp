# xyo-cpp

[![CI](https://github.com/pnsk-lab/xyo-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/pnsk-lab/xyo-cpp/actions/workflows/ci.yml)
[![Documentation](https://github.com/pnsk-lab/xyo-cpp/actions/workflows/docs.yml/badge.svg)](https://github.com/pnsk-lab/xyo-cpp/actions/workflows/docs.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

マルチエージェント共同開発: [掲示板・タスク一覧](BOARD.md) / [運用ルール](board/README.md)。

**Scratch プロジェクトを C++ で高速に実行するランタイム。**

xyo-cpp は Scratch の `.sb3` プロジェクトを読み込み、インタプリタ、LLVM ベースの JIT（実行時コンパイル）、AOT（事前コンパイル）で実行するオープンソースプロジェクトです。SDL2 によるプレイヤー、プロジェクトランチャー、エディタも備えています。

> **開発中のプロジェクトです。** Scratch / TurboWarp との完全互換性や、すべてのプラットフォームでの動作を保証するものではありません。最適化・GPU・WebAssembly 関連の機能には実験的なものがあります。

## 主な機能

- **HotSpot JIT** — まずインタプリタで実行し、頻繁に使われるスクリプトを LLVM でネイティブコード化。全スクリプトをコンパイルする eager モードも選べます。
- **AOT** — Scratch プロジェクトを事前コンパイルして、実行ファイルまたはオブジェクトとして出力できます。
- **プレイヤー / エディタ / ランチャー** — SDL2 ベースのウィンドウ表示と、`.sb3` の再生・編集・管理。
- **ペン描画** — CPU 描画と、対応環境での OpenGL FBO 描画。macOS は OpenGL 4.1 対応、OpenGL 4.3 の計算シェーダは非対応です。
- **分析・変換ツール** — スクリプト構造の解析、`.sb3` の縮小、書き換え、動作検証。
- **GPU / WebAssembly の研究開発** — GPU 実行経路や WASM 出力バックエンドを開発中。利用可能な構成と制限は個別のドキュメントを参照してください。

## クイックスタート

[Releases](https://github.com/pnsk-lab/xyo-cpp/releases) から対応するビルドを取得するか、下記の手順でソースからビルドします。

```sh
# ヘッドレス実行（30フレーム）
./build/xyo run examples/play_demo.sb3 --frames 30

# ウィンドウで再生（SDL2 対応ビルド）
./build/xyo play examples/play_demo.sb3

# プロジェクトランチャー
./build/xyo gui examples

# スクリプトの構造を解析
./build/xyo analyze examples/play_demo.sb3

# Scratch プロジェクトを縮小
./build/xyo minify examples/play_demo.sb3
```

既定の JIT は HotSpot 方式です。動作を比較する場合は `--jit=eager` または `--jit=off` を指定できます。

## ソースからビルド

CMake **3.24 以降**、C++20 対応のコンパイラが必要です。LLVM JIT を使うビルドには LLVM と `llvm-config`（または LLVM の CMake 設定）が必要です。SDL2 はウィンドウ表示時のみ必要です。

### Linux

以下は LLVM、SDL2、zlib などの開発ライブラリがインストール済みの環境を想定した例です。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DXYO_JIT=ON -DXYO_SDL2=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

SDL2 がないサーバーでは `-DXYO_SDL2=OFF` でヘッドレスビルドできます。LLVM がない場合は `-DXYO_JIT=OFF` でインタプリタ主体のビルドにできます。

### macOS (Apple Silicon / Intel)

Xcode Command Line Tools と Homebrew を使用します。

```sh
xcode-select --install
brew install cmake ninja llvm sdl2 pkgconf

cmake -S . -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DXYO_LLVM_MODE=llvm-config \
  -DLLVM_CONFIG="$(brew --prefix llvm)/bin/llvm-config" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
  -DXYO_SDL2=ON -DXYO_REQUIRE_SDL2=ON -DXYO_RSVG=OFF
cmake --build build-macos --parallel
ctest --test-dir build-macos --output-on-failure
```

詳しくは [macOS ビルドガイド](docs/macos.md) を参照してください。

### Windows

Windows は MSYS2 / MinGW-w64 でのビルドに対応しています。必要な依存パッケージ、静的リンク版の配布物、実行時の注意点については [実行・配布ガイド](RUNNING.md) と [CI 設定](.github/workflows/ci.yml) を参照してください。

## CLI

| コマンド | 用途 |
| --- | --- |
| `xyo run project.sb3 --frames 120` | ヘッドレス実行 |
| `xyo play project.sb3` | ウィンドウで再生 |
| `xyo gui [directory]` | プロジェクトランチャー |
| `xyo edit project.sb3` | エディタを開く |
| `xyo aot project.sb3 -o app` | 事前コンパイル |
| `xyo dump project.sb3` | プロジェクト情報を出力 |
| `xyo analyze project.sb3` | スクリプト構造を解析 |
| `xyo minify project.sb3` | プロジェクトを縮小 |
| `xyo rewrite project.sb3` | ブロック構造を書き換え |
| `xyo test fixture.sb3` | 検証用フィクスチャを実行 |

JIT は `--jit=hotspot|eager|off` で切り替えられます。各コマンドの追加オプションや環境変数は [RUNNING.md](RUNNING.md) を参照してください。

## ドキュメント

- [実行方法・配布バイナリの依存関係](RUNNING.md)
- [macOS 対応](docs/macos.md)
- [プロジェクトランチャー](docs/launcher.md)
- [ランタイム設計](docs/design.md)
- [WASM / Emscripten](docs/wasm.md)
- [文字列処理の性能](docs/string-performance.md)
- [スケジューラの性能](docs/scheduler-performance.md)
- [ベンチマーク](docs/algo-bench.md)
- [テストと差分検証](tests/differential/README.md)

## 開発・テスト

```sh
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# 描画テストのみ
ctest --test-dir build -R '^xyo-render-tests$' --output-on-failure
```

ベンチマークの結果は CPU、JIT モード、入力プロジェクト、ビルドオプションで変化します。性能を比較するときは条件を揃えて計測してください。

## ライセンス

オリジナルの xyo-cpp コードは [MIT License](LICENSE) です。リポジトリに含まれるサードパーティ製コンポーネントには、それぞれのライセンスが適用されます。

Scratch は Scratch Foundation のプロジェクトです。本リポジトリは Scratch Foundation の公式ランタイムではありません。
