# Scratch 向けリスト格納構造の検証（2026-11-07）

## 今回の実装

SList の `adaptive` 格納方式を改善。JIT の連続配列 ABI と既定 `flat` を維持。

- `src/core/list_storage.hpp`: `assignFlat` と `copyFlat` を追加。従来の各要素への virtual insert/get 呼出しを一括コピーで置換。
- `src/core/list_storage.cpp`: Gap Buffer は編集位置に gap を確保し、前後2区間を memcpy。C++20 `std::make_unique_for_overwrite<Value[]>` で未使用スロットのゼロ初期化を省く。Chunked Sequence は128要素のブロック単位でコピーし、読み戻しの二分探索をなくす。未使用ブロックスロットの初期化も省く。
- `src/core/slist.hpp`: 昇格、降格、adaptive の複製に一括コピーを使用。
- `tests/unit/test_list_limit.cpp`: n=0,1,2,127,128,129,1000、移行位置 0/中央/末尾、数値/boolean/空文字を含む正確な値・境界の検証、clone・編集後の検証。
- `tests/bench/list_migration_bench.cpp`: 同じリストを使って編集24回による昇格、読取り64回による降格を各30回計測。

## 測定値

CPU 8に固定。各サイズ30サイクル×3プロセスの中央値。単位 ms / 昇格・降格。速度比は各処理の旧/新。

| 要素数 | 変換 | 旧 | 新 | 高速化 |
|---:|---|---:|---:|---:|
| 10,000 | Flat→Gap | 0.02752 | 0.02469 | 1.11倍 |
| 10,000 | Gap→Flat | 0.01488 | 0.00186 | 8.00倍 |
| 10,000 | Flat→Chunked | 0.03371 | 0.01684 | 2.00倍 |
| 10,000 | Chunked→Flat | 0.05243 | 0.00274 | 19.17倍 |
| 100,000 | Flat→Gap | 0.26692 | 0.14189 | 1.88倍 |
| 100,000 | Gap→Flat | 0.12239 | 0.02592 | 4.72倍 |
| 100,000 | Flat→Chunked | 0.36425 | 0.18477 | 1.97倍 |
| 100,000 | Chunked→Flat | 0.70189 | 0.03166 | 22.17倍 |
| 200,000 | Flat→Gap | 0.57166 | 0.30347 | 1.88倍 |
| 200,000 | Gap→Flat | 0.24676 | 0.06594 | 3.74倍 |
| 200,000 | Flat→Chunked | 0.87922 | 0.52504 | 1.67倍 |
| 200,000 | Chunked→Flat | 1.52208 | 0.07803 | 19.51倍 |

生データ：`build-list-read-ir/migration-pre/results.txt`、`build-list-read-ir/migration-after-final.txt`、`build-list-read-ir/migration-comparison.json`。

47項目 scratch-bench は同一の更新バイナリで `flat` と `adaptive` を各2回：全体の幾何平均0.9848倍、リスト5項目1.3602倍、append 1.395、insert/delete 3.896、read 0.944、replace 0.939、search 0.966。保存先：`scratch-bench/results/list-bulk-20261107/summary.json`。変換コストの単体改善を全体の速度改善と混同しない。2回の結果はノイズに敏感。

## Scratch の操作パターンと推奨データ構造

| パターン | 候補 | 主な計算量・制約 |
|---|---|---|
| item / replace / append / 順次走査 | Flat（既定） | O(1) read、償却 O(1) push。JIT 直接アクセス可。途中の挿入は O(n)。 |
| 同じ位置近辺で insert/delete | Gap Buffer（adaptive） | gap の移動距離に比例。局所的編集なら償却定数時間。離れた位置へ飛ぶ場合は O(n)。 |
| 分散した insert/delete | Chunked Sequence（adaptive）、Tiered Vector（研究候補） | 現行 Chunked は128要素ブロック。rank索引 O(log(n/128))、編集の再索引 O(n/128)。 |
| append＋delete(1) の FIFO キュー | Circular Vector（**未実装**） | head offset＋2の冪容量で両端 O(1)、インデックス O(1)。現行 adaptive は先頭32位置を昇格判定から除くため、このパターンに弱い。 |
| contains / indexof の反復検索 | 更新世代と連動するハッシュ索引（**未実装**） | Scratch の数値文字列・大文字小文字・最初の出現位置の意味論を維持する必要がある。無条件ハッシュ化は危険。 |
| 巨大リストで大量の分散中間編集と頻繁な参照 | Tiered Vector（**未実装**） | 定数時間 rank access と n の分数冪に依存する編集コストの両立が理論的に可能。ポインタ間接参照が JIT の負担。 |
| 巨大リストで編集多、参照少 | Counted B+ Tree（**未実装**） | O(log n) の位置操作。追加メモリ、GC とアクセスのランダム性が難点。 |

研究資料：
- Goodrich & Kloss, *Tiered Vector: An Efficient Dynamic Array for JDSL* (1998), https://cs.brown.edu/cgc/jdsl/papers/tiered-vector.pdf
- Bille et al., *Fast Dynamic Arrays*, ESA 2017, DOI 10.4230/LIPIcs.ESA.2017.16, https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.ESA.2017.16
- Counted B-Trees, https://www.chiark.greenend.org.uk/~sgtatham/algorithms/cbtree.html

## 次に試すこと

1. Circular Vector を独立した backend として作り、FIFO の閾値を測る。任意位置参照は O(1) を保つ。
2. Tiered Vector を分散編集＋参照の混合ワークロードで比較する。格納方式を JIT で直接参照するには新しいガード設計が必要。
3. Gap や Chunked を2区間/ブロックの連続スキャンとして読めるAPIを作り、contains/indexof/contents で無駄な flat 化を減らす。
4. 読取り64回で一律降格する現在の適応しきい値を、list長と測定した変換コストに基づいて見直す。

再現：

```sh
cmake --build build-list-epoch -j 5 --target xyo xyo-list-migration-bench xyo-list-backend-smoke xyo-list-epoch-smoke
ctest --test-dir build-list-epoch -R '^(xyo-list-backend-|xyo-list-epoch-jit)' --output-on-failure
taskset -c 8 build-list-epoch/tests/xyo-list-migration-bench 100000 30
XYO_LIST_STORAGE=adaptive build-list-epoch/xyo run project.sb3
```

既定は `flat` のまま。`adaptive` は実験的。未使用の物理スロットは未初期化だが、論理 `len` 内の値は全て書き込まれており、その範囲だけアクセスする。
