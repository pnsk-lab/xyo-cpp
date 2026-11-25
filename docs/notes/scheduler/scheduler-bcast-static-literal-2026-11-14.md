# xyo-cpp: LLVMの静的broadcast-and-waitリテラル直接配送

2026-11-14 / `xyo-cpp` / 共有dirty worktree / 未コミット

## 結果

ScratchBench `sched_pingpong` の、ソースで文字列名が確定している
`event_broadcastandwait` を最適化した。
Scratch報告値を毎回 `cast::toString` して `std::u16string` にコピーする
従来経路から、JITのLLVM objectに保存した変更不可のUTF-16配列を
直接渡す専用C ABIへ切り替える。
**実際の受信Threadの開始順・待機・終了観測・join identity・GC閉包・
ユーザーの動的メッセージは変更しない。**

CPU16固定、Release/Eager、30fps、同じバイナリ/同じScratchBench SB3、
OFF/ON順を交互にした測定（`ops/ms`、大きいほど高速）：

| 測定 | workload | OFF中央値 | ON中央値 | 対応ペア速度比 ON/OFF | ON勝利 |
|---|---|---:|---:|---:|---:|
| 初回20組 | sched_pingpong | 427.8705 | 440.0880 | **1.02569倍** | **19/20** |
| yield修正後16組 | **sched_pingpong** | **429.7730** | **444.8915** | **1.04172倍** | **16/16** |
| yield修正後16組 | sched_fanwait | 875.9180 | 877.3430 | 1.01080倍 | 7/16 |
| yield修正後16組 | sched_restart（対照） | 1365.3400 | 1361.8810 | 1.00793倍 | 8/16 |
| yield修正後16組 | sched_fanout（対照） | 895.8430 | 899.0895 | 1.01625倍 | 10/16 |

最も再現性がある改善は `sched_pingpong` のみ。各runは起動と
LLVM生成を含む。別の測定組の速度比は掛け合わせない。
時間依存のscheduler checksumは各run間で一致を要求しないが、
ScratchBench kernel本体は一切変更していない。

原データ：
- `build-scheduler-rewrite-20261111/bcast-static-20261114/bench-paired-20/results.json`
- `build-scheduler-rewrite-20261111/bcast-static-20261114/bench-final-16/results.json`

## 実装

- `src/jit/codegen.cpp`: `emitInputOf` を通常どおり評価した後、
  **非AOT/定数i64/タグ付き文字列が現コンパイル中の
  `pinned.strings` に存在する**場合のみ、LLVMのimmutable
  `i16` グローバルと文字数を作って専用ヘルパへ渡す。
  アドレスの有効性を確認するまでGC管理外の任意ポインタを逆参照しない。
  動的 reporter、文字列以外、AOTは従来の `xyo_broadcast_wait_v` を使う。
- `src/jit/rt.cpp`: `broadcastWaitName` に既存のreceiver
  起動/Unicode canonicalization/yield/joinを共通化した。
  `xyo_broadcast_wait_lit(ctx, const char16_t*, uint32_t)` はJIT object
  内の不変配列を直接 `std::u16string_view` として渡す。
  動的経路は従来の所有する `std::u16string` を保持し、
  Unicode Stage名前解決時にも**追加のコピーを作らない**。
- `src/jit/backend_llvm.cpp`: 新C ABIをLLVM ORCのhost symbolへ登録。
- `src/jit/codegen.cpp::helperMayYield`、
  `src/jit/stackless_coro_lower.hpp`、
  `src/ir/rt_yield_edge_stackmap.hpp`: すべてに
  `xyo_broadcast_wait_lit` を**中断可能な境界**として追加。
  RT-001ではnested helperのyieldを「安全な直接StackMap」
  として誤認しない。

### 発見・修正した重大バグ

最初の実装は `sched_pingpong` のように同一スクリプト内に
他のyieldヘルパがある場合は動作した。しかし、
静的な `broadcast and wait` しかないスクリプトが
Fiberなしの同期実行に誤分類された。
Unicode Kelvin記号 U+212A の受信テストで
`xyo_fiber_switch` がnull destinationを受け取りSIGSEGVした。
gdbはptrace不可のため、独立した信号ハンドラと実行ファイルの
シンボル/逆アセンブルで `xyo_fiber_switch` のnull保存先を特定した。
**新C ABIの `helperMayYield` 登録漏れが根本原因**だった。
分類、コルーチン禁止リスト、RT-001のyield-edge分類を修正し、
静的のみのsourceも必ず `CompiledScript::mayYield=true` になるようにした。

該当の実験的クラッシュ版はユーザーの既定設定に採用していない。
最終版はUnicodeのEager JIT単独静的joinと動的文字列を含む
13件の集中テストで、OFF/ONとも0失敗。

## 回帰テスト

`testNativeStaticBroadcastWait()` を追加：
- 1本のLLVM JITスクリプトで静的 `PING` と
  変数レポータによる動的 `PING` を順に送信し、
  受信側の値を2回加算する。
- 動的変数を `UNKNOWN` に変更後、静的 `PING` のみが
  正しく受信者を起動する。
- 変数を `PING` に戻して再実行し、重複配送がない。
- 各実行後に未返却Fiberが増加しない。
- U+212A のUnicode Stage放送名を用い、ASCII fast pathが
  禁止される状態で、静的 `broadcast and wait` が
  **必ずFiberを確保し**、正しいイベントを配送する。

集中テストは `XYO_TEST_ONLY_STACKLESS_CORO=1` と
`XYO_TEST_ONLY_STATIC_BCAST=1` を指定し、
`XYO_JIT_BCAST_STATIC_LITERAL=0/1` の双方で13/13 PASS。

修正後のEager JIT全体回帰：
- ON: Scheduler **651/651 PASS**、E2E **99/99 PASS**、exit 0。
- OFF: Scheduler **651/651 PASS**、E2E **99/99 PASS**、exit 0。
- ログ: `build-scheduler-rewrite-20261111/bcast-static-20261114/full-{on,off}.{out,err}`。
- 最終の既定ON版を改めてEager JITで全量実行：
  Scheduler **651/651 PASS**、E2E **99/99 PASS**、exit 0。
  ログ：`build-scheduler-rewrite-20261111/bcast-static-20261114/full-final-default.{out,err}`。
- Fiber smoke / GC deferred-collection smoke / multifiber barrier smoke は
  同じ最終Releaseビルドで **3/3 PASS**。
- 最終既定ON、明示OFFとも静的+動的+Unicodeの集中テスト
  **13/13 PASS**。`git diff --check`もPASS。

## 設定

効果の再現性とON/OFF回帰合格を受けて
**`XYO_JIT_BCAST_STATIC_LITERAL` を既定ON**にした。
従来の経路が必要なら `XYO_JIT_BCAST_STATIC_LITERAL=0`。
ビルド時/コード生成時だけ有効なログ：
`XYO_JIT_BCAST_STATIC_TRACE=1` は
`[jit-bcast-static] block=<id> len=<UTF16文字数>` をstderrに出力。

```sh
# 既定のstatic literal高速化
XYO_JIT_MODE=eager ./build-scheduler-rewrite-20261111/xyo run project.sb3 --fps 30

# 従来方式との比較
XYO_JIT_MODE=eager XYO_JIT_BCAST_STATIC_LITERAL=0 \
  ./build-scheduler-rewrite-20261111/xyo run project.sb3 --fps 30
```

## 棄却した同時実験

`restartThread()` の `threadMap` の既存キー再利用も試した。
新旧のThreadでキーを使い回せるかを慎重に判定する実装で、
Stage Target IDのホスト変更、外部map差し替え、
連続再起動を7件で検証し、ON/OFFとも合格したが、
ScratchBench 16組交互では `sched_pingpong=1.00094倍`（9/16勝）、
`sched_fanwait=1.00159倍`（6/16勝）、
`sched_restart=0.99320倍`（5/16勝）で改善がなく、
**ソースから実験コードとテストを撤回した**。
生データは
`build-scheduler-rewrite-20261111/restart-keep-key-20261114/bench-paired-16/results.json`。

この変更の主眼はbroadcastの定数文字列変換だけである。
未検証のjoin省略・Fiber切替回数削減・同一receiverの
インライン同期実行は行っていない。


## 既定HotSpot JITでの交互測定（最終ビルド）

既定ONへ変更後の同一Release実行ファイル、元のScratchBench
`sched_*`カーネル本体を変更しない凍結SB3、CPU16固定、
30fps、HotSpot、`XYO_PAR=0`、`XYO_YIELD_QMODE=off`。
`XYO_JIT_BCAST_STATIC_LITERAL=0`と`=1`を16組で交互実行した。

| workload | OFF中央値 (ops/ms) | ON中央値 (ops/ms) | 対応ペア幾何平均 ON/OFF | ON勝利 |
|---|---:|---:|---:|---:|
| **sched_pingpong** | **555.8735** | **574.7330** | **1.03573倍** | **13/16** |
| sched_fanwait | 1146.4640 | 1153.2000 | 0.99391倍 | 10/16 |
| sched_restart | 1338.3270 | 1328.4055 | 0.99190倍 | 8/16 |

生データ: `build-scheduler-rewrite-20261111/bcast-static-20261114/bench-hotspot-final-16/results.json`

Eager（修正後16組: 1.04172倍、16/16勝）だけでなく、
**HotSpot（1.03573倍、13/16勝）でもpingpongの改善が確認できた**。
他のケースの変動はこの最適化に帰属させず、全47項目への
改善を主張しない。値は個別の交互試験であり、異なる試験間の速度比は乗算しない。
