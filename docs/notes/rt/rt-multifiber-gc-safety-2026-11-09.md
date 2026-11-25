# RT-001：同時中断中の複数LLVM FiberとGCルート安全性（2026-11-09）

## 問題と再現

`XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1` / `XYO_RT_GC_SOFT_BUDGET_MB=1` の実験経路では、単一のネイティブFiberが復帰した時点で `Gc::collect()` を実行していた。しかし、**別のネイティブFiberがまだ中断中でも、そちらのコンパイラ活性化が保持するSSA値の完全なルート証明が存在するとは限らない**。

`Runtime::markRoots()` は全Threadの中断済みFiberスタック、保存されたcallee-savedレジスタ、Interpreterの引数等を保守的に走査している。ただし任意のLLVM生存SSA値・ポインタ・SIMDレジスタや一時値まで捕捉したという証明にはならない。

実験用Scratch `ssa_budget_cross_fiber_two_hats` は緑の旗2本が同一の非warp手続きを別引数で各24,000回実行し、Unicode文字列結合・条件分岐・共有変数への副作用を繰り返す。各ネイティブFiberは元々の非warpループで交互にyieldするので、GC予算に達した時点で複数のNative Fiberが生存する。

従来の隔離LLVMビルドでは両方のGC予算復帰が `collected=1` と報告された。最終変数はInterpreterと一致するが、その事実だけでは最初のGCが他方のネイティブSSAを保護できた証明にはならない。

## 実装（opt-in限定）

`src/engine/fiber.hpp`、`src/engine/fiber.cpp`:
- `FiberPool::activeCount()` を追加。プールの待機Fiberは数えず、`acquire` 済みかつ `release` 前の**実際のlease数**を原子的に管理する。複数Runtimeに跨るGc共有にも対応。
- ASanモードで中断中のスタックを意図的にリークしても、到達不能になったleaseは必ずカウントから減らす。GCの型・ABI・ポインタ表現は変更なし。

`src/core/gc.cpp`:
- 起動時に `XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1` を明示している場合、`Gc::collect()` のsweep前に `FiberPool::activeCount()` を確認。
- 生存leaseが1本以上なら**マーク・スイープを行わず** `pendingExplicitCollect_` に保留。明示的・自動閾値双方の回収経路を保護する。通常モードでは従来どおり。
- これはGCを合法化する新しいSSAルート証明ではなく、未証明の実行活性化が残る間の**回収禁止ゲート**。

`src/engine/sequencer.cpp`:
- `stepThreads()` の後、保留GCがあれば `collectIfNeeded()` で再試行。残存Native Fiberがある間は引き続き拒否し、最後のleaseが解放されて初めて回収する。
- 移行成功後の `native-gc-soft-budget`、復帰拒否後の `native-gc-soft-budget-finished` は `Gc::collectCount()` で**実際の回収の有無**を記録。保留中なのに `collected=1` と出すことを修正。
- ログへ `pending` と `active_fibers` を追加。

## 検証

新しい実Scratchケース（`tests/differential/native_simple_resume.py`）:

| 段階 | GC | 残存Native Fiber | GC管理バイト |
|---|---|---:|---:|
| 1本目のLLVM→Interpreter復帰 | collected=0 / pending=1 | 1 | 1,048,662 |
| 2本目のLLVM→Interpreter復帰 | collected=1 / pending=0 | 0 | 43,046 |

双方が正しく処理を完了し、Interpreterと最終変数完全一致（`y=48005`）。

**改変SSA（fingerprint再計算）**：両方 `adopted=0`、中断中は `collected=0`。1本目終了後は `pending=1, active_fibers=1`、最終Fiber終了後に `collected=1, pending=0, active_fibers=0`、約3.50 MiB → 約43 KiB。Interpreterと完全一致。

**無圧力・無root所有者**：8 MiB予算またはroot保護無効では予算移行は発生せず、Interpreterと完全一致。

- 新規差分実行4件。以前の946件を含む全ネイティブ差分テスト **950/950 PASS**。ログ `build/rt001-cross-fiber-full.log`。
- `tests/platform/rt_native_gc_deferral_smoke.cpp` にFiberPoolの2本lease取得・解放と保留GC/最終回収の単体テストを追加。`tests/CMakeLists.txt` に `xyo-rt001-native-gc-multifiber-barrier-smoke` を追加し、`XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1` で実行。
- 隔離LLVM23バイナリにリンクした単体テストを、保護モードOFF/ONそれぞれで直接実行し、両方PASS。
- Python構文検査・関連ソースのdiff check実施。

隔離バイナリ：`/home/kicky/.rt001-cross-fiber-gc-isolated/xyo`。既存の検証済みLLVM23 `libxyo.a` の複製に対し、変更した`gc.cpp`・`fiber.cpp`・`sequencer.cpp`の3オブジェクトを再コンパイル、同名アーカイブ要素を置換して再リンク。現時点の全共有ツリーからの完全な再ビルドと主張しない。

## 未対応・制限

- **実験用フラグが必要で、既定OFF**。通常ランタイムのGC挙動に新たな制限を加えない。
- 未終了Fiberが無期限に残ればGCも延期される。SOFT予算はRSS/物理メモリのハードキャップではなく、恒久的な長寿命Fiberには別の安全なルート公開・中断・回収設計が必要。
- 本対応は「**全部のnative Fiberが解放されてから回収する**」保守的な対策。生存中のLLVM SIMD/pointer SSAを個別に回収可能と証明したわけではない。
- 並列workerのstop-the-world契約、ネイティブ以外のスレッドが保有するFFI値、任意SSA/副作用位置の一般的なデオプト、プロジェクト編集中のsource世代変更は別課題。
- `Gc`、`FiberPool` のオブジェクト配置ABI・LLVM CallFrame ABIは変更していない。

RT-001引き続きin_progress。他担当の未コミット変更は保持し、コミット・pushは行っていない。
