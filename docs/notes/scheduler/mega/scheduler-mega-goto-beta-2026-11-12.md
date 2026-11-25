# Mega Dispatch β — LLVM単一関数への自作ブロック統合

日付: 2026-11-12
状態: 実験機能、既定OFF、未コミット

## 使い方

- XYO_JIT_MEGA_DISPATCH=1: β版を有効化
- XYO_JIT_MEGA_TRACE=1: 対象スクリプトごとの判定と統合した呼出し数を出力
- XYO_JIT_MEGA_MAX_IR=512: 展開予測IRの上限（既定512、100〜100000に設定可）
- XYO_JIT_MODE=eager: 通常の同期LLVM ORC経路で利用

例:

    XYO_JIT_MODE=eager XYO_JIT_MEGA_DISPATCH=1 \
      ./build-scheduler-rewrite-20261111/xyo run project.sb3

## 実装範囲

現段階は **1つのトップレベルScratchスクリプトから到達可能な、非再帰の自作ブロック群** のみを対象とする。

src/jit/mega_goto.hpp はLLVM IRに対し、Scratchの手続き呼出しグラフを事前検査し、LLVM InlineFunctionで自作ブロック呼出しをBasicBlock間のジャンプ（goto相当）に変換する。トップレベルからの全呼出しが消えた後、独立した手続き関数を内部化してLLVMの通常パイプラインで削除する。

src/jit/backend_llvm.cpp から同期コンパイル時に呼び出し、完全融合した場合は遅延ORCプロシージャ解決を行わない。

再帰、上限を超えるIR、アドレスの流出、間接呼出し等は従来の関数呼出しにフォールバックする。引数評価、CallFrame、warp、yieldは生成済みIRを保存する。

**重要な制限:** これは静的インライン化されたgoto型制御フローであり、プロジェクト全体の全スレッドを一つのswitch(pc)で切り替える機能ではない。手続き本体の複製でコードが増える可能性がある。

## 正しさの検証

通常のSB3で P → Q → Stage変数を1増やす処理を反復した。
- 20,000回×Qを2回: Stage s=40000（正確）
- XYO_JIT_INLINE_LIMIT=0での診断:
  [mega-goto] script=hat fused=1 calls=3 estimated_ir=110 remaining=0 reason=fused
- 最適化後IRには define void @xyo_script_0 が1つ、_pN手続き呼出しは0件
- 再帰するP→Q→P: reason=recursive、通常経路に戻り最終s=6
- Qが30個の変数更新を持つ場合: 既定上限512では size-or-callsite、最終s=120000
- Mega Dispatch ONの完全xyo-tests: PASS（47.85秒）

追加テスト: tests/unit/test_scheduler.cpp の testMegaGotoProcedureFusion。
生成器: tests/bench/algos/build_mega_goto_fixture.py。
A/B計測: tests/bench/algos/bench_mega_goto.py。

## CPU16固定・同一バイナリ交互A/B（8組）

指標はプロセス総壁時計時間（コンパイル・起動費用を含む）。
速度比は対応ペアのOFF時間÷ON時間の幾何平均。

| 条件 | OFF中央値 | ON中央値 | ON/OFF速度比 | ON勝利 |
| --- | ---: | ---: | ---: | ---: |
| P→Q×2、2万反復、明示inlineを無効 | 28.847ms | 26.594ms | 1.1284x | 7/8 |
| 同上、従来inline上限220 | 28.479ms | 26.836ms | 1.0876x | 6/8 |
| Q本体が30命令（強制統合） | 31.367ms | 40.437ms | 0.8055x | 1/8 |

長い手続きの強制統合は約19%悪化するため、既定のコードサイズ予算を512へ縮小した。

生データ:
- build-scheduler-rewrite-20261111/mega-goto/ab-no-inline/results.json
- build-scheduler-rewrite-20261111/mega-goto/ab-default-inline/results.json
- build-scheduler-rewrite-20261111/mega-goto/ab-long-body/results.json

## ScratchBench sched_* 全8項目への効果

6組交互A/Bでの既定上限512のON/OFFスループット比:
- sched_looptick 0.9933x
- sched_pingpong 1.0112x
- sched_restart 0.9982x
- sched_clonechurn 1.0014x
- sched_threads 0.9969x
- sched_fanout 0.9837x
- sched_fanwait 0.9835x
- sched_waituntil 0.9898x

この条件では7コンパイル対象がnot-applicable、残る1つがsize-or-callsiteで、**実際にMega Dispatchを適用したsched_*スクリプトはない**。上記の差を高速化の根拠にしてはいけない。

上限を12000へ引き上げると、script=~Mv は26呼出し、推定IR=8878で統合できた。しかし4組A/Bではsched_clonechurn約0.980x、pingpong約0.993x、fanwait約0.991xで、明確な改善は見られなかった。

生データ:
- build-scheduler-rewrite-20261111/mega-goto/sched-ab/results.json
- build-scheduler-rewrite-20261111/mega-goto/sched-ab-force/results.json

## 次の段階

ユーザーが提案した真の単一関数スケジューラには、スレッド単位のPC、呼出し戻り先、待機状態、保存するSSA値、ブロードキャスト受信者、再帰、GCルート、動的ソース変更を明示的に扱う継続ABIが必要。現在のLLVMスタックレス・コルーチン実験と統合し、実際のScratchBench性能とParityを確認する。

全て既定OFFで未コミット。他エージェントの変更はリセットしない。
