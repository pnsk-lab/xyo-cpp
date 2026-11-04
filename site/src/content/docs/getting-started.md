---
title: クイックスタート
description: 最初の Scratch プロジェクトを実行する
---

## 1. xyo を用意する

[GitHub Releases](https://github.com/pnsk-lab/xyo-cpp/releases) から使用環境向けのリリースを入手するか、[ソースからビルド](/xyo-cpp/installation/)します。

Linux ではヘッドレス専用の完全静的版と、画面表示に対応した通常版があります。

## 2. SB3 を実行する

```sh
./xyo run project.sb3 --frames 300
```

ウィンドウで再生する場合は SDL2 対応版を使います。

```sh
./xyo play project.sb3
```

## 3. 実行モードを選ぶ

```sh
./xyo run project.sb3 --jit=hotspot
./xyo run project.sb3 --jit=eager
./xyo run project.sb3 --jit=off
```

通常の `run` では HotSpot が既定です。必要に応じて [実行モードの解説](/xyo-cpp/guide/execution/)を参照してください。

## 次に読む

- [CLI リファレンス](/xyo-cpp/guide/cli/)
- [AOT コンパイル](/xyo-cpp/guide/aot/)
- [既存の詳細な実行手順](/xyo-cpp/reference/running/)
