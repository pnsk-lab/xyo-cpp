# Mega Scheduler β — 部分的LLVM自作ブロック融合

日付: 2026-11-12
状態: β / 明示的にON / デフォルトOFF / 未コミット

## 背景

ScratchBench performance.sb3 は4ターゲット、2,310ブロック、
緑旗ハット1本、procedures_callが239個の大型ソース。
Stageから実行するsched_pingpongやsched_fanwaitは自作ブロックの中にあり、
既存のXYO_JIT_MEGA_DISPATCH=1の全体融合は
IRサイズ512、3,000、12,000の全てでStageメインスクリプトに適用できなかった。

今回実装したXYO_JIT_MEGA_PARTIAL=1は、
局所的なLLVM InlineFunctionによって既存の自作ブロックの直接呼び出しを
少数だけ基本ブロック内へ統合するもの。
完全なMega Scheduler、すなわちスレッド選択も含む
一つのネイティブswitch(pc)はまだ実現していない。

## 構成

新規: src/jit/mega_partial.hpp
接続: src/jit/backend_llvm.cpp
診断: CompiledScript::megaPartialInlined（実際にinlinedした呼出し件数）

制約:
- 対象はproccodeがsched_で始まる、実際に定義があるScratch手続きだけ。
- callerごとに最大8箇所、callee IR最大384、追加IR予算最大1200命令。
- callerへの再帰経路が存在する場合は拒否。
- musttail、間接呼び出し、予算超過は元の呼び出しを維持する。
- LLVM verifierが失敗した場合、コンパイルを拒否する。
- 完全融合に失敗した場合だけ実行する。部分融合後も
  手続きのORC関数シンボルとJitEngine管理マップを維持する。
- 手続き固有のCallFrame、Scratchのyield規則などは変更しない。
- 計装・JIT CallFrame実験・ブロックカウント有効時は非適用。
- 既定OFF、コンパイル時のみ実行され、元のSB3を変更しない。

有効化:

    XYO_JIT_MODE=eager \
    XYO_JIT_MEGA_PARTIAL=1 \
    XYO_JIT_MEGA_PARTIAL_SCOPE=pingpong,restart \
    XYO_JIT_MEGA_TRACE=1 \
    ./build-scheduler-rewrite-20261111/xyo run performance.sb3

XYO_JIT_MEGA_PARTIAL_SCOPEはカンマ区切りで厳密一致。
省略またはallで全sched_*が対象。pingpong,restartなら該当2つのみ。
XYO_JIT_MEGA_PARTIAL_SITES=1を指定すると
各インライン展開の呼出し先とサイズをログ出力する。

## 実際のScratchBenchでのコード生成

    [mega-partial] script=~Mv selected=10 inlined=16 rejected_cycle=0
    rejected_size=0 valid=1

Stageのsched_*手続き10個が選択され、16箇所の実際のLLVM
CallInstが変換された。以前の全体融合で同じStageの統合数は0。
ScratchBenchのsched_*全8項目の出力とチェックサムを照合した。

## 性能測定

同一バイナリ固定、CPU16固定、6組の交互A/B、eager JIT、
XYO_YIELD_QMODE=off。変数の最終値とスイートのチェックサムを確認。
下記の数値はスループットのペア速度比（ON/OFF、幾何平均）。

| ScratchBench | scope=all | scope=pingpong,restart |
| --- | ---: | ---: |
| sched_looptick | 1.024x | 1.011x |
| sched_pingpong | 1.059x | 1.015x |
| sched_restart | 1.049x | 1.017x |
| sched_clonechurn | 0.984x | 0.985x |
| sched_threads | 0.974x | 1.022x |
| sched_fanout | 0.917x | 1.019x |
| sched_fanwait | 0.955x | 1.012x |
| sched_waituntil | 0.931x | 1.038x |

初回にみられたpingpong +5.9%は対象を限定すると+1.5%となり、
再現する大幅改善としては確認できていない。
また対象外のベンチマークにも数%の変動がある。
結論として、実際のLLVMコード統合は成功したが、
sched_*の一貫した性能改善は確認できていない。

生データ:
- build-scheduler-rewrite-20261111/mega-partial-ab/results.json
- build-scheduler-rewrite-20261111/mega-partial-ab-scope-ping-restart/results.json

## 正しさ

tests/unit/test_scheduler.cppに独立したLLVM IRテストを追加。
通常のScratchコンパイラが小さい自作ブロックを
事前にインライン化する場合もあるため、
テスト内では呼び出しを含むLLVM Moduleを直接構成して検証。
- sched_partial -> partial_helperを正確に1箇所インライン化。
- helper自体のORC公開シンボルを維持。
- 相互再帰する呼び出しを変更しない。
- 通常のScratchデータ変更・コンパイルキャッシュでParityを検証。

## 次の候補

1. 節約した実呼出しの数ではなく、JITのネイティブ命令／
   フレームスイッチ回数で評価する。
2. ScratchBenchの動的repeat-untilの中にある
   broadcast-and-waitに対して、正しい受信者集合を維持しながら
   スタックレスの継続を使えるようにする。
3. Stageの単一緑旗スクリプトから自作ブロックに入り、
   Thread所有のPCと戻り先スタックを管理する。
4. 遅延JITでも妥当なソース証明／安全なフォールバックを設計する。

他エージェントの無関係な変更は保持。ソースは未コミット。

## 最終回帰テスト

- XYO_JIT_MEGA_PARTIAL=1、scope=all: xyo-tests PASS（51.68秒）
- XYO_JIT_MEGA_PARTIAL=0: xyo-tests PASS（52.56秒）
- 合成LLVM Moduleの1箇所の直接呼び出し除去・元関数保持をPASS。
- 再帰を持つ関数のインライン拒否をPASS。
- Scratchスクリプトからの実行・緑旗再実行をPASS。
- 既定OFF。全変更未コミット。
