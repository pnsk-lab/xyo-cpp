---
title: 実行モードと JIT
---
xyo-cpp はインタプリタ、LLVM JIT、AOT を組み合わせる実行基盤です。

| モード | 設定 | 動作 |
| --- | --- | --- |
| HotSpot | `--jit=hotspot` | まず解釈実行し、ホットなスクリプトをコンパイル。通常の既定 |
| Eager | `--jit=eager` | スクリプトを早期に JIT コンパイル |
| Interpreter | `--jit=off` | JIT を無効化 |
| AOT | `xyo aot` | 実行前にネイティブコードを生成 |

```sh
XYO_HOT_STATS=1 ./xyo run project.sb3
XYO_JIT_TIMING=1 ./xyo run project.sb3 --jit=hotspot
```

## HotSpot と遅延手続きコンパイル

`--jit-lazy=auto|on|off` は手続きの LLVM 最適化・機械語生成を初回呼び出しまで遅延させるか制御します。既定の `auto` は HotSpot 中で静的な基準により選択され、すべてのプロジェクトで最速になるわけではありません。

```sh
./xyo run project.sb3 --jit=hotspot --jit-lazy=on
./xyo run project.sb3 --jit=hotspot --jit-lazy=off
```

詳細な閾値と制約は [実行手順](/xyo-cpp/reference/running/) および [設計書](/xyo-cpp/reference/design/) を参照してください。
