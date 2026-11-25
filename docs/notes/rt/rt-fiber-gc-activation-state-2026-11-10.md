# RT-001: GC安全性の範囲を未開始・完了Fiberへ限定して改善（2026-11-10）

## 背景と見つかった制約

2026-11-09に導入した RT-001 の保守的GCバリアは、`XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1` で全 `FiberPool::activeCount()`（確保済み・返却前のFiber数）が0になるまでGCを止めていた。しかし **Fiberのlease** と **再開可能なネイティブSSAの存在** は異なる。

- プールから確保されただけの、まだ実行されていないFiberはLLVMのSSA状態を保持できない。
- 元のentryがすでに戻ったFiberは、まだプールへ返していなくても、そのLLVM SSAを再開できない。
- 一度実行を開始して `yield` したFiberは、GCルート未証明のSSAを保有する可能性があり、引き続き回収を禁止する必要がある。

LLVMコンパイラ出力の実測：

`ssa_budget_cross_fiber_two_hats` の各native checkpointで、生成文字列は **1件がexact GC rootへ登録済み**。一方で未解決のlive LLVM pointerは**21件**（arg 1、alloca 12、GEP/cast 5、load 3、その他0；うちstack由来14、distinct alloca12）存在する。`unresolvedOther=0`。これは **「文字列SSAだけがrootedだからGC可能」と判断してはいけない**ことを示す。全live SSA・pointer ownershipの証明は未完成。

## 実装内容

`src/engine/fiber.hpp`、`src/engine/fiber.cpp`：

- 既存の `Fiber::started_` を `Fiber::run()` で実行開始時に設定し、公開読み取りAPI `started()` を追加。構造体のフィールド追加なし。
- 新たに `FiberPool::unsafeCount()` を追加。実際にnative entryを開始していて、そのentryがまだ戻っていないFiberだけを原子的に数える。**yield中もカウント継続**。
- `Fiber::run()` のentry終了でカウントを下げる。終了前に `FiberPool::release` された中断中のFiber（ASan意図的リークを含む）はrelease時にカウントを下げる。プール再利用時の `reset()` は `started_` を初期化済み。
- 追加の原子的カウンター操作は `XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1` の場合に限定する。通常モードはコードパスが変わらない。

`src/core/gc.cpp`：

- 以前の `FiberPool::activeCount()!=0` を `FiberPool::unsafeCount()!=0` に置換。
- GCは **未実行/完了だけが存在する場合** に許可され、未証明の中断中native Fiberがある場合は従来通り `pendingExplicitCollect_` に回収を保留する。
- GCのオブジェクトレイアウト・LLVM ABI・mark/sweepロジックには変更を加えない。

## 検証

`tests/platform/rt_native_gc_deferral_smoke.cpp` の本物のmmap Fiberを使用：

1. Fiberを2つ `acquire` しただけで `activeCount()==2` / `unsafeCount()==0`、GCが実行可能。
2. 両方 `resume` して実際の `yield` で中断すると `unsafeCount()==2`、GC拒否・pending保持。
3. 片方のentryが `return` しても、もう一方が中断中ならGC禁止（`unsafeCount()==1`）。
4. 最後のentryが `return` すれば、プールへ返す前でもGCが実行可能（`activeCount()==1` / `unsafeCount()==0`）。
5. プール再利用後、以前の `started` 状態は残らず、中断中に破棄したFiberのGC禁止状態も解消する。
6. フラグなしの通常モードで実際に中断したFiberがあっても、新しい`unsafeCount`は0（余分なatomic加減算なし）。

GC smokeは OFF/ON両モードで PASS。隔離LLVM23バイナリを作成して、実Scratchの`ssa_budget_cross_fiber_two_hats`（正常移行とSSA改変による拒否）、`ssa_budget_warp_two_hats_writer`の両経路でInterpreterとの最終状態一致・GC延期/回収時刻を再確認。

隔離ビルド：`/home/kicky/.rt001-fiber-state-gc-isolated/xyo`。以前の検証済みLLVM23アーカイブを複製し、`fiber.cpp`・`gc.cpp`・`sequencer.cpp`のみ再コンパイル・リンクしている。全未コミットツリーの完全な再ビルドではない。

- SHA-256: `2d1f1369a1b56e1b5e8070293ecd120f2fbdf4819658b8688e47ad19633ea9ed`
- フル差分回帰: **950/950 PASS**。コマンド `python3 tests/differential/native_simple_resume.py --native /home/kicky/.rt001-fiber-state-gc-isolated/xyo --out build/rt001-fiber-state-gc-final`。ログ `build/rt001-fiber-state-gc-final.log`。
- 旧状態カウンターの初期版についても950/950 PASS、新しい「実験フラグONだけatomic更新」の最終バイナリで改めて950/950 PASS。
- 単体テスト `/home/kicky/.rt001-fiber-state-gc-isolated/rt_native_gc_multifiber_smoke` は `XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1` と無指定の各1回、合計2/2 PASS。
- `git diff --check`（対象実装）とPython構文検査PASS。実測は隔離Releaseビルドであり、ASanによる全体検証・共有ツリーの完全再ビルドは未実施。

## 未解決・次の範囲

- **まだ中断中のnative Fiberに対するGCは許可していない**。正確なルート証明を得るには21件の未解決pointerに対し、stack-only、JitCtx、モデル/変数の寿命管理、GC heap内ポインタを区別する静的検証、さらにコンパイラ最適化後のlive値との整合性が必要。
- 通常時のGCを遅延させない改善は限定的。長寿命のsuspended Fiberが一つでもある場合、soft GC予算はハードメモリ上限にはならない。
- 複数workerによる同時GCは別のstop-the-world課題。実験経路ではシリアルスケジューラを要求。
- `Gc` / `Fiber` のオブジェクト構造ABIを変えず、実験フラグ既定OFF。
- 他担当の共有作業ツリーを保持し、commit/pushなし。RT-001 in_progress。
