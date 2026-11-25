# Non-contiguous list algorithms — 2026-11-07

## 実装した3種類

`XYO_LIST_STORAGE=deque|gap|chunked` を追加。

- `deque`: `std::deque<Value>`（分割配列、インデックス参照は O(1)）
- `gap`: Gap Buffer（連続バッファ中の空き領域を最後の編集位置へ移動）
- `chunked`: 128 要素の固定長ブロック + prefix sum による rank 二分探索。
  ブロック内更新は O(128)、prefix 更新は O(ブロック数)。
  **B+ tree ではない**。木構造の再平衡は行わない。
- `flat` / `sbo4` / `sbo8` / `flat15` は従来方式で変更なし。

既存の `SList::items` は LLVM の fast-path、WASM、par-for、描画、
GC、GPU の一部で直接参照されるので、新しいモードでは `items` を nullptr
とし `ListStore` インターフェース `get/set/insert/erase` に転送する。
JIT の add/item/replace は非連続モードのとき**生成時に helper に lowering**
する（flat 系の JIT fast-path は維持）。`par-for` は直接 slot 書き込みの
競合があるため非連続モードで非使用とする。GC は各リストの実データを走査し、
解放時には `ListStore` を delete する。

**注意:** 同じコード生成・同じアクセス手段で比較したわけではない。
非連続モードはヘルパー関数呼び出しの費用も含む。
総合性能を採用判断するには storage・JIT lowering の共同最適化が必要。

## ネイティブデータ構造ベンチマーク

`tests/bench/list_algorithms_native.cpp` は独立したストレージ実装のみを比較。
20,000 要素 → 20万回ランダム読み取り → 8,000回ランダム
insert/delete (交互) → clear → 20万回 append。操作後全体ハッシュを照合。

例示の単発測定（実行ごとの変動に注意）:

| mode | append ms | random get ms | middle edit ms |
|---|---:|---:|---:|
| flat | 2.158 | 0.088 | 13.359 |
| deque | 1.759 | 0.497 | 19.170 |
| gap | 2.257 | 0.272 | 8.416 |
| chunked | 1.065 | 7.233 | 0.824 |

実行:

```sh
g++ -O3 -std=c++20 -Isrc src/core/list_storage.cpp \
  tests/bench/list_algorithms_native.cpp -o build/list_algorithms_native
build/list_algorithms_native
```

## Scratch-bench 2 run（HotSpot JIT）

- 47 workloads、同じ `build/xyo` バイナリ、同じ performance.sb3
- データ: `scratch-bench/results/list-algorithms-20261107/`
- 集計: `summary.json`
- 47/47 workloads、各2回の測定が成功
- checksum は39決定論的ケースで flat と一致。残り8項目は時間依存の
  `sched_*` で、実行時間に依存するため一致しない
- 数値は同一操作の median `ops_per_ms` の比であり、スコア自体は重み付け有り。

| backend | all geomean vs flat | list geomean vs flat | overall site score | list site score | RSS median MB |
|---|---:|---:|---:|---:|---:|
| flat | 1.000 | 1.000 | 81,700 | 62,313 | 235.25 |
| deque | 0.728 | 0.375 | 56,315 | 23,364 | 212.30 |
| gap | 0.813 | 0.714 | 62,979 | 44,463 | 221.55 |
| chunked | 0.712 | 0.276 | 54,382 | 17,222 | 210.60 |

| mode | append | read | replace | insdel | search |
|---|---:|---:|---:|---:|---:|
| deque | 0.696 | 0.375 | 0.303 | 0.201 | 0.465 |
| gap | 0.844 | 0.423 | 0.317 | **3.142** | 0.520 |
| chunked | 0.769 | 0.082 | 0.055 | **2.123** | 0.220 |

再実行:

```sh
cmake --build build -j4 --target xyo
python3 tests/bench/list_storage_scratchbench.py --runs 3 \
  --modes flat,deque,gap,chunked --out scratch-bench/results/list-algorithms-repeat
```

## テスト / 制限

```sh
cmake --build build --target xyo-list-backend-smoke
ctest --test-dir build -R '^xyo-list-backend-' --output-on-failure
```

4/4 通過。最新ビルドの `XYO_LIST_STORAGE=flat XYO_JIT_MODE=eager build/tests/xyo-tests` も全テスト成功（`test_list_limit` 10055 checks / 0 failures、`GC pool` 10943 checks / 0 failures）。

各モードで `test_list_limit_main` の基本操作、clone、limit、
および非連続モードでは3000回のランダム挿入・削除と参照配列の順序照合を実施。

既存の `xyo-tests` の複数テストは内部構造 `list->items[i]` を直接
参照しているため、非連続方式での実行に非対応であり、`deque` モードの
通常 `xyo-tests` はセグメンテーション違反となる。専用ターゲットを
別に設けた。今後既存テストを `get0` / `replace0` に移行する必要あり。

**判断:** 既定は `flat` のままとする。`gap` は Scratch の
insert/delete だけ約3.1倍速いが他のアクセスで遅い。`chunked` は
大規模でランダムな位置編集が頻発する workload を救えるが、
`item` と `replace` に大きな回帰がある。
動的昇格を実装するなら、プロフィールの編集比率とリスト長に加え、
JIT の高速アクセス経路の切り替えを再設計すること。
