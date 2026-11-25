# Adaptive List JIT の世代キャッシュ最適化（2026-11-07）

## 目的と方式

初期の adaptive モードでは LLVM JIT のループ内リストヘッダ昇格を無効化していた。リストごとの連続配列を安全に再利用するため、SList に 64-bit storageEpoch を追加した。

世代は確保容量の増加による再配置、flat→gap/chunked、gap/chunked→flat、非連続状態の clear→flat で増加する。通常の値の上書きでは増加しない。

LLVM は昇格可能なループの入り口で len / items / numericOnly / storageEpoch を取得し、yield から戻った際にも更新する。scanPromotable が append・insert・erase・clear・不透明な手続きを除外するため、通常の連続配列上で実行される中断のない区間では cached items を再利用できる。これは同一スケジューラ内の協調実行を前提とし、他スレッドから SList を直接同時変更する動作は対象外。

キャッシュされた items が null の場合だけ遅い経路へ入り、storageEpoch が変わっていたら len / items / numericOnly / storageEpoch を再取得する。格納方式がまだ非連続なら従来の Scratch helper に分岐する。helper が flat へ復帰させたときは次の呼び出しで再取得する。既存のフラット系に新しい JIT ガードは発生しない。

## 47項目の scratch-bench A/B

各構成につき同じバイナリで flat と adaptive を2回ずつ計測。LLVM HotSpot JIT、performance suite、47項目、offline。数値は flat の中央値 ops_per_ms で正規化した速度比（高いほど速い）。

| 指標 | 変更前 | 全アクセス世代検査 | 改訂版（冷経路のみ検査） |
|---|---:|---:|---:|
| 全47項目の幾何平均 | 1.0097 | 1.0112 | **1.0200** |
| リスト5項目の幾何平均 | 1.3709 | 1.3405 | **1.4152** |
| Append | 1.441 | 1.365 | **1.549** |
| Read | 0.860 | 0.849 | **0.890** |
| Replace | 0.913 | 0.905 | **0.958** |
| Insert/Delete | **4.385** | 4.092 | 4.023 |
| Search | 0.976 | 1.008 | **1.069** |
| Peak RSS 中央値 | 220.5 MB | 229.25 MB | 226.3 MB |

初回の世代検査は毎回の照合が読み取りを悪化させた。改訂版は中断のないループ内での照合を除外したため、Read が初回版比約4.8%、変更前比約3.5%改善した。一方、Read は flat よりまだ約11%遅い。Insert/Delete の改善幅は変更前より小さくなった。いずれも2回の測定であり、差の有意性は未検証。

測定の保存先:

- 変更前: scratch-bench/results/list-epoch-before-20261107/summary.json
- 全アクセス検査版: scratch-bench/results/list-epoch-after-20261107/summary.json
- 改訂版: scratch-bench/results/list-epoch-hot-20261107/summary.json

それぞれのフォルダに生の performance-xyo-cpp.json と console.log がある。39個の決定論的な workload は flat と checksum が一致した。残る8個の sched_* は実時間依存で一致しない。総合スコア（サイトの重み付きスコア）と表の幾何平均は異なる。

## テスト

- ctest の xyo-list-backend-* 5件と xyo-list-epoch-jit 1件が成功。
- 新 JIT テストは非 warp の Scratch リスト読み取りを yield で止め、別経路で flat→gap を実行、GC 後に再開、読み取りが続くと flat へ復帰するところまでインタプリタと比較。
- SList のユニットテストは再確保・昇格・復帰・clear と世代増加、clone、ランダム挿入削除を検証。
- 既定 flat の xyo-tests は既存一式を回帰検証。

## 実行方法

    XYO_LIST_STORAGE=adaptive build-list-epoch/xyo run project.sb3

    ctest --test-dir build-list-epoch -R 'xyo-list-(backend-|epoch-jit)' --output-on-failure

    python3 tests/bench/list_storage_scratchbench.py --binary build-list-epoch/xyo --runs 3 --modes flat,adaptive --out scratch-bench/results/list-epoch-repeat

## 今後

通常用途の既定値は flat のまま。adaptive は実験的な選択肢。読取遅延の残りは、ガード・分岐・値の型情報・JIT 最適化パスの影響を IR で調べる余地がある。Eager/HotSpot・より広い実プロジェクトのパリティと、サンプル数を増やした再計測が必要。
