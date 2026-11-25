# Mega Scheduler β — スタックレス複数スクリプト継続

2026-11-12 / xyo-cpp / experimental (opt-in, default OFF, uncommitted)

## 機能

新フラグ: XYO_JIT_MEGA_BATCH_YIELD=1

同一Targetの2〜8本の独立した緑旗Scratchスクリプトで、
「イベントハット → 非warp control_repeat(整数リテラル) →
data_changevariableby(数値リテラル)」だけを含むものを対象にする。

全スクリプトは **ただ一つのLLVM関数** にコンパイルされる。
LLVM switch(pc)により各スクリプトの初回PC（偶数）と再開PC（奇数）を
区別し、反復回数はThread::megaRemaining、再開位置はThread::megaPc
に保存する。UINT32_MAXが終了状態。
各LLVM呼出しは最大1回の通常repeat本体を処理し、C++スケジューラ
へ戻る。ネイティブFiberやコルーチンがC++スタックを保存することはない。

この段階では**C++が次に実行するScratch Threadを選ぶ**。
全Threadの選択まで一回のLLVM呼出しに移す「完全グローバルgoto」
とは区別する。

使い方:

    XYO_JIT_MODE=eager XYO_JIT_MEGA_BATCH_YIELD=1 \
      XYO_JIT_MEGA_BATCH_TRACE=1 \
      ./build-scheduler-rewrite-20261111/xyo run project.sb3

旧XYO_JIT_MEGA_BATCH=1（同期スクリプトの単一関数化）と
XYO_JIT_MEGA_DISPATCH=1（自作ブロックの関数統合）は独立し、同時にON可能。

## 安全性と対象制約

- 緑旗ハット直下に、後続のないcontrol_repeatが1本。
- TIMESは非負の整数定数（最大2,147,483,647）。
- SUBSTACKは後続のない変数加算で、VALUEは有限数値リテラル。
- reporter、再帰、自作ブロック、wait、broadcast、clones、
  GPU、AOT、WASM、OSRは現在の対象外。
- Eager LLVM/JIT、headless、無計装の未実行Threadに限定。
- 元のScratch Thread同一性、順序、終了状態は保持。
- Source Blocksの所有者とrevisionを検証し、同一ソースの
  緑旗再実行はコンパイル済みLLVM関数を再利用。
- 実行開始前のソース変更はインタプリタで再実行。
  一部の反復を書き込んだ後のソース変更は再実行せずThreadを終了。
- 中断中にScratch変数が削除・再作成された場合は、
  Stage/Targetの変数binding generationを再確認してjitVarCacheを
  クリアし、新しい変数を解決。古いVariable*は読まない。
- 変数書き込みには既存のxyo_var_changeを使い、Scratchの
  数値変換とnotifyWaitVariableWriteを維持する。
- 未対応・コンパイル失敗のスクリプトは既存経路へ戻す。
- 既定OFF。作業ツリーは共有・未コミットでリセットなし。

## 機能検証

3本の独立repeat（3×+1、2×+2、0×+4）の場合:

    [mega-yield] slots=3 definitions=1 states=6
    [mega-batch] installed=3 kind=yield

全Scratch変数は通常方式と一致。専用unit testは次を確認:
- 1回のscheduler selectionでrepeat本体を1回だけ実行し、
  各ThreadのPCと残反復数が独立している
- 0回repeatは書込みなしで終了
- 最終和は7、2回目の緑旗でコンパイル済み関数を再利用し14
- ネイティブFiberのactive countに増加なし
- 変数マップ再作成後のスレッドが再バインドして最終106
- 一部書込後のソース改変は終了し、他の未実行Threadは安全に解釈
  （再実行なし、テスト最終値19）

4スクリプト、リピート0,1,9,2、増分-1,0.5,-2.5,0の場合:
Stage変数 v0=0, v1=0.5, v2=-22.5, v3=0 をOFF/ONで確認。
この場合も4スクリプトが1LLVM関数・8状態に融合。

同期スクリプト2本＋yieldスクリプト3本を一つのSB3で実行:
両β版を有効化し2個のdispatcher（同期2slot、yield3slot）が生成。
全5変数がOFFと一致。

## 性能測定（CPU16固定、同一バイナリ、交互A/B）

Eager JIT、LLVM O1、XYO_YIELD_QMODE=off。
計測はプロセスの総壁時計時間（起動・JITコンパイル時間を含む）。
全ペアでScratch変数の最終値を照合。

| 条件 | OFF中央値 | Mega ON中央値 | 対応ペア速度比 | ON勝利 |
|---|---:|---:|---:|---:|
| 3本 × 10,000反復 | 120.452ms | 118.597ms | 1.0377x | 6/8 |
| 8本 × 15,000反復 | 284.871ms | 273.248ms | 1.0426x | 5/6 |

性能生データ:
- build-scheduler-rewrite-20261111/mega-yield/ab-3x/results.json
- build-scheduler-rewrite-20261111/mega-yield/ab-8x/results.json

この約4%の改善は厳密なソース条件を満たす単純反復に限る。
一般のScratchBench sched_*やbroadcast and waitの高速化は
まだ確認していない。ベータ機能をデフォルトONにしない。

## 主な実装ファイル

- src/jit/mega_yield_source.hpp: 完全なソース条件の検査
- src/jit/backend_llvm.cpp: 1LLVM関数、2PC/Thread、保存した反復数の利用
- src/jit/backend.hpp, src/jit/jit.hpp: 明示的な継続ABI
- src/jit/jit.cpp: 対象選択、compiled batchキャッシュ、ソース検査
- src/engine/thread.hpp/.cpp: ThreadごとのPC・残反復数・binding stamp
- src/engine/sequencer.cpp: Fiberを使わず実行して元のスケジューラへ復帰
- tests/unit/test_scheduler.cpp: interleaving、キャッシュ、ソース編集、
  変数再作成、Fiber未使用の回帰テスト
- tests/bench/algos/build_mega_yield_fixture.py: SB3生成
- tests/bench/algos/bench_mega_yield.py: 正しさと対応A/B測定

## 次の課題

1. 1スクリプトに複数repeatがある場合のPC遷移
2. wait untilや時間待ちの継続条件／停止状態
3. broadcast and waitの受信者一覧と正確なjoin順序
4. 自作ブロック引数、再帰return PC、warp、GC root
5. LLVM関数内でThread全体を巡回する単一goto型スケジューラ本体

## 最終検証（同一ターン）

- すべてのMega関連βフラグをONにしたxyo-tests CTest:
  **PASS（最終ABI修正後45.09秒）**。
- 3フラグすべてOFF: **PASS（44.69秒）**（ABI宣言の整合前）。
- 一時停止中のVariableマップ削除・再作成、Source revisionの変更、
  2回目の緑旗、0回repeat、独立PC/残回数はすべてPASS。
- Mixed project: 同期2slot＋yield3slotの共存成功。
- ScratchBench sched_*全8項目: exit=0で8行出力。
  今回の限定適格性には該当せず、mega-yieldのLLVM関数は生成されなかった。
  したがってsched_*への速度改善は主張しない。
- LLVM ORCの従来のxyo_var_get/set/changeのforward declarationが
  実装ABIの引数数と一致していなかったため修正。
- git diff --checkおよびPython生成器・ベンチマークの構文確認PASS。
- 全変更は未コミット。他エージェントの作業領域は変更しない。
