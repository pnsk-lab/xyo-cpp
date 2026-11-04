---
title: ビルドとインストール
---
## 配布バイナリを利用

最新の配布物は [GitHub Releases](https://github.com/pnsk-lab/xyo-cpp/releases) を参照してください。Linux の完全静的版はヘッドレス専用です。画面表示には SDL2 対応ビルドが必要です。

## ソースコードから

C++20 コンパイラ、CMake、Ninja、LLVM の開発環境を用意します。必要なライブラリと CMake オプションは環境によって異なります。

```sh
git clone https://github.com/pnsk-lab/xyo-cpp.git
cd xyo-cpp
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target xyo -j
./build/xyo --help
```

SDL2 を使用しない場合は `-DXYO_SDL2=OFF` を指定してください。構成により LLVM / GUI の依存パッケージが必要です。エラーが発生したら `CMakeLists.txt` の依存条件を確認してください。

WebAssembly 用の手順は [WASM](/xyo-cpp/architecture/wasm/) にあります。
