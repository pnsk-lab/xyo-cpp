# RT-001：RAX上のyieldカウンターをLLVM SSA構造で識別（2026-11-11）

## 対象と結論

実際の2-Fiber Scratchプロジェクト `ssa_budget_cross_fiber_two_hats` で、yield直前の `llvm.experimental.stackmap` に格納された `i64` 値のうち一部がcaller-savedレジスタ `RAX` に配置されていた。その値は、**Scratch文字列の管理ポインタではなく、JIT内部のスケジューラ用 `JitCtx::yieldCheckN` カウンターの増分**だった。

確認した最適化後LLVM IR（`build/rt001-yield-ssa/source-id198/source-id198-optimized-ir.log`）：

```llvm
%ycnt.0141 = phi i64 [ %9, %cmpok.done ], [ %121, %214 ]
...
%121 = add i64 %ycnt.0141, 1
```

初期値 `%9` は `JitCtx*` 引数からの `getelementptr i8, ptr %0, i64 104` 経由の `load i64` で、同じフィールドへカウンターが書き戻される。ソース `src/jit/codegen.cpp` の `beginLoopYieldScope` は `offsetof(JitCtx, yieldCheckN)` から初期化し、`emitLoopYield` は `add(load(curYieldCnt), 1)` を生成する。 `src/jit/jit.hpp` では `yieldCheckN` は `uint64_t` と宣言され、ScratchのGC管理 `Value` ではない。

**LLVM命令IDや機械レジスタ番号には依存しない**。この対象のコンパイルでは別のSSA位置がID `198` や `204` と表示される場合があったため、識別器は番号を使用せず次の構造すべてを確認する。

- 元値が `i64 add` の2項演算であり、一方の項が厳密に定数1、他方が `i64 PHI`。
- そのPHIは2つのincoming valueを持ち、1つは当該加算の戻り、残り1つは `i64 load` の初期値。
- loadのアドレスが**関数第1引数の信頼済み`JitCtx*`**から、単一の定数バイトオフセット `offsetof(JitCtx, yieldCheckN)` で得られること。
- これ以外のadd、別フィールド、別アドレス、未知のPHIは **unclassified** のまま。

この認証された整数カウンターに限り、実験用 `XYO_RT_YIELD_SSA_FILTER=i64_no_tick` のStackMap location一覧から除外する。ほかの `i64` 値はそのまま維持する。

## 変更箇所

- `src/ir/rt_yield_ssa_machine_stackmap.hpp`：
  - `isNativeYieldTickIncrement(...)` でLLVM IRの構造と正しいABIオフセットを検証。
  - `YieldSsaOperandSource::semantic` に `scheduler_tick / unclassified` を保持。
  - `YieldSsaSelection::I64WithoutSchedulerTicks` では**証明済みのnumeric tickだけ**をStackMapから除外し、除外件数を別管理。
- `src/jit/backend_llvm.cpp`：
  - 実行時に `offsetof(JitCtx,yieldCheckN)` を上の検査へ渡す。
  - `XYO_RT_YIELD_SSA_TRACE_OPERANDS=1` の追加opt-inで機械位置→SSA定義・意味をログに記録。
  - `[rt001-yield-ssa-numeric-excluded]` で実際の除外件数を報告。常に `ownership_proven=0 machine_roots_proven=0 gc_safe=0`。
- `tests/platform/rt_yield_ssa_machine_stackmap_smoke.cpp`：
  - 正規PHI＋`add 1`を受理、別ABIオフセット・別関数引数・定数2を足す計算・load単体・未知コンテキスト等の偽装を拒否。
- `tests/differential/rt_yield_tick_exclusion.py`：
  - 実際のLLVM23 ORC Eager/Lazy ELFの `i64` と `i64_no_tick` を比較し、各直接yield地点で1件だけ機械locationが減ることと、Scriptの最終変数がInterpreterと一致することを必須とする。
- `tests/differential/rt_yield_ssa_spill_bounds.py`：
  - 実2-Fiber CFIフレーム・`llvm-readobj --stackmap`・LLVM SSAのsource manifestを突合し、`i64_no_tick` モードでは除外済みのnumeric tickが依然としてmachine locationとして残ることを禁止。GC安全を認可することはない。

## 実測

- 隔離されたLLVM23の現行ソースビルド `/home/kicky/.rt001-orc-fde-cmake`：コンパイル・リンク成功。
- LLVM O3関連CTest **6/6 PASS**（`build/rt001-tick-semantic/ctest-current.log`）。
- LLVM O2関連CTest **6/6 PASS**（`build/rt001-tick-semantic/ctest-o2.log`）。
- `i64_no_tick` と通常 `i64` の比較ではEager/Lazyとも実Scratchの4地点に対して**各1件のnumeric tickだけ**を削除し、Interpreter結果が一致。
- 実際の中断中2-Fiberを巻き戻した比較では、`i64`側の148 i64候補のうち4地点のnumeric tickを除外後、locationの総数は **152→148**。caller-savedレジスタの復元不能なlocationは **2→0**。残りのスタックスロット136、callee-savedレジスタ12については場所を確認できたが、GC所有権はまだ証明していない。
- `XYO_RT_YIELD_EDGE_STACKMAP_PROBE=1 XYO_RT_YIELD_LIVE_SSA_STACKMAP=1 XYO_RT_YIELD_SSA_FILTER=i64_no_tick` の状態で、ネイティブ復帰と不正復帰拒否の差分回帰 **955/955 PASS** （`build/rt001-tick-semantic/full-no-tick.log`）。

## 安全性の留保

数値のスケジューラカウンターはGCルートではないと判定できる一方、**残りのi64すべてがGCルートというわけでも、ルートの完全性が証明されたわけでもない**。

- Tagged `Value` と生のポインタ、float・整数、スタックアロケーションを機械位置／所有権ごとに分類する必要がある。
- StackMapは呼び出し直前のレジスタ配置を記述する。Fiberはホスト内で停止し、その時点のレジスタ状態は変わりうる。
- 待機ヘルパ内部など、直接yield以外の中断経路はまだ証明されていない。
- `FiberPool::unsafeCount()`に基づくnative FiberのGC禁止、他の安全ゲートは変更していない。
- 全リポジトリCTestは未実施。他担当の未コミット変更には触れず、コミット・pushはしていない。

## 次の調査

残りの148 locationsのうち、GC-managed boxed `i64`、数値としての `i64`、一時的な演算用ビット列・関数引数・不確定値をLLVMの出所で分類する。安全性が証明できない値は必ず未分類として残す。呼出し前のルート候補と**実停止中のルート値**が一致することを保証するまではGCを解放しない。
