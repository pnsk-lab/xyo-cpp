---
title: テストと性能測定
---
## テスト

ビルド設定でテストを有効にして `ctest` を実行します。

```sh
cmake -S . -B build -G Ninja -DXYO_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Scratch との互換性

`tests/differential/` には Scratch VM との出力比較ハーネスがあります。独立した比較対象を使って実行結果と副作用を照合してください。

## ベンチマーク

`scratch-bench/` と `tests/bench/` に複数の測定方法があります。必ず同一の SB3、実行時間条件、描画条件、JIT モードをそろえて比較してください。平均速度だけでなく、個々の処理系の弱点を観測することが重要です。

- [Scratch Bench 検証記録](/xyo-cpp/reference/scratch-bench/)
- [互換性に関する既知の差分](/xyo-cpp/reference/scratch-divergences/)
- [研究・実験記録](/xyo-cpp/research/algo-bench/)
