# START-004：jemallocを用いたネイティブ起動高速化（2026-11-09）

## 目的と結論

START-003のAOT起動分析に続き、約2万5千件のScratchブロックを
展開・解析・登録する際の多数のヒープ確保に着目した。

`glibc malloc` と `jemalloc` / `tcmalloc` を
**同一バイナリへの `LD_PRELOAD`** で12組比較したところ、
jemallocが最も高速だった。
その結果を基に、CMakeからjemallocを明示的にリンクできる
オプションを追加した。既定はOFFで、他OS・静的ビルドには影響しない。

## ビルド方法

Linuxでjemallocの開発用ライブラリを導入してから：

```bash
cmake -S . -B build-jemalloc -DCMAKE_BUILD_TYPE=Release \
  -DXYO_JIT=ON -DXYO_LINK_JEMALLOC=ON
cmake --build build-jemalloc --target xyo-cli -j4
ldd build-jemalloc/xyo | grep jemalloc
```

`libjemalloc.so.2` が実行時にも必要になる。
CMakeで `-DXYO_LINK_JEMALLOC=OFF` とすれば従来どおり。
Windows/WASM/完全静的Linuxは対象外。
jemallocはBSD 2-Clause系ライセンスのオープンソース実装。
ライブラリはxyo-cppに同梱しない。

ビルド時のリンクは `--no-as-needed` の区間をjemallocのみに
限定している。実行時の `readelf -d` に
`NEEDED libjemalloc.so.2` があることを実物のReleaseビルドで確認。
リンカー状態の保存・復元には
`--push-state,--no-as-needed` と `--pop-state` を使用する。
`XYO_LINK_JEMALLOC=ON -DXYO_STATIC=ON` の構成は
CMake設定時に明示的にエラーとなることも検証した。

## 起動性能

対象：2656件・206グループのScratchBench Parity、AOTコンパイル済みオブジェクト、
SDL `offscreen`、音声 `dummy`、CPU 10固定。
一度の起動ごとに**最初のScratch出力まで**を計測。
AOTの事前コンパイル時間とキャッシュ生成時間は含まない。

### LD_PRELOAD比較（同一実行ファイル、交互12組）

| 確保器 | 起動中央値 | glibc比 |
| --- | ---: | ---: |
| glibc | 268.1 ms | 1.00倍 |
| tcmalloc | 249.7 ms | 1.06倍 |
| jemalloc | **230.1 ms** | **1.15倍** |

jemallocは12組中11組でglibcに勝った。

### CMakeの選択的リンク（2バイナリ、交互16組）

| バイナリ | 起動中央値 |
| --- | ---: |
| glibc従来リンク | 270.0 ms |
| jemallocリンク | **230.5 ms** |

起動時間**約14.6%減**。jemallocが16組中15組に勝ち、
ペア速度比の幾何平均は**1.163倍**。

### 互換性

`tests/differential/gui_scratch_parity_regression.py` にて、
AOTの実ロードとScratchの期待値を照合：

- glibc従来リンク：**2656/2656 PASS**
- jemallocリンク：**2656/2656 PASS**

両者とも `aot: 479 scripts precompiled` によるネイティブAOTロードを確認。
`--push-state/--pop-state` に変更してリンクし直した最終バイナリでも
Scratch Parity **2656/2656 PASS** を再確認した。
外部確保器のリンク可否はOS・ディストリビューションによるため、
CIで常時有効にはしていない。

実際に2656件を最後まで実行した各1回の計測（CPU10固定）：

| 項目 | glibc | jemalloc |
| --- | ---: | ---: |
| 全テスト完走時間 | 17.46秒 | 17.36秒 |
| 最大RSS | 364.45MB | **441.43MB** |

**起動は高速化するが、メモリ使用量は約21%増加**した。
メモリ制限が厳しい環境では既定のglibcを推奨する。

追加実験としてjemallocの `MALLOC_CONF=narenas:1` /
`narenas:2` をCPU10固定・交互12組で評価した。
元のjemalloc設定の起動中央値247.6msに対し、
1 arenaは235.8ms、2 arenaは243.4ms。ただし勝数は
それぞれ7/12、8/12にとどまり、安定した改善とまでは言えない。
全量実行時の最大RSSはそれぞれ415.9MB、409.3MBで、
glibcより依然として大きい。自動設定は採用しない。

## 却下したブロック読み込み最適化

比較はCPU 10固定・交互16組のAOT起動で実施した。

| 候補 | 従来起動中央値 | 候補起動中央値 | 結論 |
| --- | ---: | ---: | --- |
| `Blocks::createBlocks` 一括登録・キャッシュ無効化1回 | 271.9ms | 277.3ms | 16組中10組改善、総合優位なし |
| `Blocks::restoreState` ベースのマップ一括入替 | 286.3ms | 281.0ms | 16組中8組改善、優位なし |

これらの候補は**キャッシュ内の隔離されたソース複製のみ**で実装・測定。
共有作業ツリーには反映していない。

## 制限事項

本改善は大規模なAOT起動という特定の作業負荷に対して検証した結果。
小さなプロジェクトや異なるCPU/OSの速度改善を保証しない。
外部ライブラリの使用により依存関係が増えるため、
配布用の既定ビルドはglibcのままにする。

全て未コミット・未push。GitHub Actionsの新設定の実実行は未確認。
