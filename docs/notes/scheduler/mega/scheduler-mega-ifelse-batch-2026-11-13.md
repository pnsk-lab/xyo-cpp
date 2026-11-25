# Mega Scheduler β — 有限数値 if/else のLLVM一括実行

日付: 2026-11-13
状態: 既定OFF、opt-in、共有作業ツリーで未コミット。

## 実装仕様

対応範囲は次のScratch処理だけ。

    if (scalar_variable > | < | = finite_literal):
        change then_variable by finite_literal
    else:
        change else_variable by finite_literal

これを一度だけ実行する受信ハット、または固定回数repeatの本体に置ける。
条件変数と書込先は同じIDでも別IDでもよい。ソースは番号のみのリテラル、
data_variable、control_if_else、operator_gt/lt/equals、
各armの定数加算1命令であることを静的に検査する。
procedure戻りsuffix、ifの次の命令、ネストした分岐、動的レポータ、
外部副作用・GCを伴うステートメントは拒否する。

認定を有効化する独立フラグは XYO_JIT_MEGA_IF_ELSE_BATCH=1。
省略・OFF時は古いJITを使い、この新しい条件分岐は認定しない。

主な変更:

- src/jit/mega_yield_source.hpp: 条件・分岐先の型、グラフの終端、
  比較対象のID、有限数値リテラルを静的に証明
- src/jit/backend_llvm.cpp: 条件付きsourceについて
  IEEE754 fcmp ogt/olt/oeqとthen/else fadd/storeを生成。
  PCとremainingの更新は両分岐の合流後に実施。
  1Thread用Mega経路では既存のxyo_var_getとxyo_gt/lt/eqを呼び、
  Scratchのcast::compareと型変換を厳密に保持
- src/jit/jit.hpp: MultiPcItemにconditionVariableを追加し、
  フィールドのオフセットと構造サイズをstatic_assert する。
  多重PC群の各スロットにconditionalメタデータを追加
- src/engine/sequencer.cpp: then、else、conditionの3本のVariable*を
  ソース版、現在のTarget、実行可能なThread状態と照合する。
  すべてfinite numberでなければ通常のMega/JIT継続経路へ戻す
- src/jit/jit.cpp: green flag / broadcast受信者のインストールにおいて
  条件変数とelse変数の存在も事前チェック
- tests/unit/test_scheduler.cpp: 5受信者・gt/lt/eq、
  単発とrepeat(4,3,0,2)の混在。独立の書込先と条件変数のalias、
  numeric-stringと非数値文字列、変数削除→再作成、
  ORCキャッシュ再使用を検査

従来の一括バッチ条件を維持し、reverse待機通知の
XYO_WAIT_WAKE_REGISTRY=1のときはmultiPC一括コードを生成しない。
初期版が扱わない分岐などはすべて通常JITへ回す。

## 実行例

    XYO_JIT_MODE=eager \
    XYO_JIT_MEGA_BATCH_YIELD=1 \
    XYO_JIT_MEGA_BCAST_RECEIVERS=1 \
    XYO_JIT_MEGA_DIRECT_SCALAR=1 \
    XYO_JIT_MEGA_SCALAR_BATCH=1 \
    XYO_JIT_MEGA_MULTI_PC_BATCH=1 \
    XYO_JIT_MEGA_IF_ELSE_BATCH=1 \
    ./build-scheduler-rewrite-20261111/xyo run project.sb3 --fps 0

任意の一括実行集計: XYO_JIT_MEGA_BULK_STATS=1。

## 回帰検証

testMegaIfElseBatchの各scheduler pass直後の共有変数結果を確認する。
数値x=3からの最終値は x=0, a=112, b=4。
入力を数値文字列2に変えた場合もx=1, a=2, b=103に到達。
非数値文字列abcの場合はScratchの文字列比較を用いる。
変数を同IDで削除・再作成しても現在のVariableを再解決。

## 実測（2026-11-13）

8本のgreen flagスクリプト、それぞれrepeat(40000)。
各反復は if v_i > 0 then change v_i -1 else change v_i +3。
初期値が3、周期が4のため、全8変数の最終値は3になる。
同一バイナリ/SB3、CPU16固定、12組の交互A/B、
プロセス起動とLLVM JITコンパイルも含む壁時計時間。

| 指標 | 通常JIT | 条件分岐一括ON |
|---|---:|---:|
| 中央値 | 445.625ms | 386.301ms |
| 対応ペア速度比（OFF/ON幾何平均） | 1.0x | 1.1993x |
| ONの勝利 | — | 11/12 |

先行した6組交互測定では1.2037x、5/6勝利。
小標本で外れ値もあるため、他のScratchBenchの一般性能とはしない。

生成器: tests/bench/algos/build_mega_ifelse_fixture.py
測定器: tests/bench/algos/bench_mega_ifelse.py
結果: build-scheduler-rewrite-20261111/mega-ifelse/bench-repeat-12/results.json
先行結果: build-scheduler-rewrite-20261111/mega-ifelse/bench-40000x8/results.json

## 未対応

汎用if/else、動的Reporter、短絡論理、リスト・文字列の
ネイティブ一括比較、関数呼び出しの混在、独自ブロック引数、
イベント生成、GCを伴う副作用など。
この試験で既定値は変更しない。

## 最終回帰・互換性確認

2026-11-13の同一ソース、同一xyo-testsで次を確認した。

- 条件分岐一括ON、通常の待機通知レジストリOFF:
  Scheduler 551 checks, 0 failures、全テストPASS、終了コード0
- 条件分岐フラグOFF、旧Mega実行経路:
  Scheduler 536 checks, 0 failures、全テストPASS、終了コード0
- 条件分岐ON + XYO_WAIT_WAKE_REGISTRY=1:
  Scheduler 509 checks, 0 failures、全テストPASS、終了コード0
- 数値文字列2と非数値文字列abcによる比較を厳密な期待値で検証
- 変数を同じIDで削除・再作成した後のORCコード再利用でも検証
- C++ xyo-tests、xyo-cli両ターゲットをビルド成功
- Python benchmark/generator: py_compile PASS
- git diff --check PASS

出力ログ:
build-scheduler-rewrite-20261111/mega-ifelse/test-final-on.out
build-scheduler-rewrite-20261111/mega-ifelse/test-final-off.out
build-scheduler-rewrite-20261111/mega-ifelse/test-final-wake.out
