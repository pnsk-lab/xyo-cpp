# SList storage A/B — 2026-11-07

## 概要

`SList` の既存 ABI（`len`, `cap`, `monitorDirty`, `numericOnly`,
`items` の位置）と連続した `Value[]` 表現を保持しつつ、
環境変数 `XYO_LIST_STORAGE` で切り替え可能な4方式を追加した。

| mode | storage | resize |
|---|---|---|
| `flat` | malloc + realloc（既定） | 容量 2 倍 |
| `sbo4` | 4要素以内は GC オブジェクトの末尾に格納。超過時にヒープへ移行 | 容量 2 倍 |
| `sbo8` | 8要素以内は GC オブジェクトの末尾に格納。超過時にヒープへ移行 | 容量 2 倍 |
| `flat15` | malloc + realloc | 容量 1.5 倍 |

単に `std::deque` / B+ tree に置き換えると、JIT が `SList::items` を
直接インデックス参照する ABI を壊す。今回はデータ構造を分割する方式は
実装していない（別途 JIT lowering の抽象化が必要）。

GC はインラインの場合に `items` を外部ヒープとして `free`・二重計上
しないよう変更した。スピル後のバッファは従来の `malloc/realloc/free`。
また `realloc` 成功確認前に元のポインタを破棄しないようにした（OOM は abort）。

## 測定

- 2026-11-07
- `scratch-bench` の `performance` 47 workloads
- 同一バイナリ `build/xyo`、HotSpot JIT、各モード2回
- `--offline --no-variants`
- 結果: `scratch-bench/results/list-storage-20261107/`
- 集計: `scratch-bench/results/list-storage-20261107/summary.json`
- 速度比は同じワークロードに対する `ops_per_ms` 中央値の比を幾何平均。
  サイトの重み付き score とは異なる。

| mode | all vs flat | lists vs flat | startup median | peak RSS median |
|---|---:|---:|---:|---:|
| flat | 1.0000 | 1.0000 | 329.15 ms | 241.10 MB |
| sbo4 | 1.0010 | 0.9876 | 328.75 ms | 229.45 MB |
| sbo8 | 0.9963 | 0.9839 | 324.20 ms | 241.05 MB |
| flat15 | 0.9992 | 0.9971 | 327.15 ms | 239.45 MB |

各 list workload の flat 比:

| mode | append | read | replace | insdel | search |
|---|---:|---:|---:|---:|---:|
| flat | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| sbo4 | 0.982 | 0.999 | 0.994 | 0.967 | 0.996 |
| sbo8 | 0.948 | 0.992 | 0.994 | 0.986 | 1.002 |
| flat15 | 0.995 | 0.999 | 0.985 | 1.016 | 0.991 |

4方式・各2回とも完走。各方式の最初の run を比較したところ、
47項目中39項目の checksum は一致した。差がある残り8項目はいずれも
`sched_*` の時間依存項目である。

## 検証

`cmake --build build -j4 --target xyo xyo-tests`: 成功

`XYO_JIT_MODE=eager build/tests/xyo-tests`: 成功
（`test_list_limit` 308 checks / 0 failures、`list codegen bounds` 
3497 checks / 0 failures、`GC pool` 10943 checks / 0 failures）。

## 再測定

```sh
cmake --build build -j4 --target xyo
python3 tests/bench/list_storage_scratchbench.py --runs 3
```

`XYO_LIST_STORAGE=sbo4 build/xyo run project.sb3` などで個別指定。

## 判断

この `scratch-bench` では基準 `flat` が list スコア最高。
`sbo4` は RSS の中央値を 4.8% 下げたが、list スコアでは
1.2% 程度の低下。2回測定なので小さな速度差はノイズの可能性あり。
既定値の変更は行わない。

今後、新しいストレージの必要性を検証するなら、
大規模リストのランダムな途中挿入・削除に焦点を当てる必要がある。
`SList::items` を必須とする現在の JIT を抽象化した上で、
別途 chunked B+ sequence 等を比較する。
