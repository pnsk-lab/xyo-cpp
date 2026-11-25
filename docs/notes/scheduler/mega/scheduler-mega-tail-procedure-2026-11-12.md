# Mega Scheduler β — 末尾の自作ブロック呼び出しをまたぐ再開

日付: 2026-11-12
状態: 実験機能・既定OFF・未コミット

## 変更点

src/jit/mega_yield_source.hpp に、引数なし・非warpの
末尾 procedures_call を証明する経路を追加。
緑旗スクリプトの唯一の命令が手続き呼び出しである場合だけ、
最初に解決される procedures_definition の本体を検査する。

本体が、従来のMega-Yieldで既に安全性を検証していた
- 非warp repeat(整数リテラル) { 変数を有限定数だけ加算 }
- 固定ASCIIのbroadcast and wait → 変数を有限定数だけ加算
- 非warp repeat(整数リテラル) { 固定ASCIIのbroadcast and wait }
  → 変数を有限定数だけ加算
のいずれかに該当する場合に限り、LLVMの共通switch(pc)へ直接入れる。

Source.tailProcedure は診断用ソース証明メタデータ。
LLVMコードにはC++呼出しスタック、手続きFrameや戻りアドレスを
残さない。今回は末尾呼び出しなので、手続きが完了すると
Scratchスクリプト全体も完了する。一般的なネストした手続きの
戻り先スタックを実装したわけではない。

src/jit/jit.cpp の選択時に、
本体が厳密に証明できる場合に限り、
従来の自作ブロック用CompiledScript::proceduresが非空でも
Mega-Yieldへ組み込めるようにした。
無関係な非yieldの同期Mega Batchは従来の制約を維持する。

### フォールバック条件

- 引数がある／引数定義が一致しない
- warp付き、自作ブロックがreporterである
- 緑旗スクリプトの呼び出しに後続命令がある
- 解決対象のdefinition/prototypeがない・不一致
- 手続き本体がサポートされていない
- AOT/WASM、GUI、非eager経路、途中実行中のThread

いずれも通常のコンパイル経路を維持する。
Source revisionや変数binding generationの安全検査も従来通り。

## 実行と検証

有効化:

    XYO_JIT_MODE=eager XYO_JIT_MEGA_BATCH_YIELD=1 \
      XYO_JIT_MEGA_BATCH_TRACE=1 \
      ./build-scheduler-rewrite-20261111/xyo run project.sb3

再現可能なScratchプロジェクト生成:
tests/bench/algos/build_mega_tailproc_fixture.py

入力SB3の緑旗スクリプトを、
「緑旗→専用の自作ブロックcall」に置き換え、
元の本体をargument-free/nonwarpのdefinitionの下へ移動する。

正しい対象条件で確認した実際の生成ログ:

    [mega-yield] slots=3 definitions=1 states=6

数値repeatを持つ自作ブロック3本:
v0=3、v1=4、v2=0（OFF/ON一致）。

繰り返しbroadcast-and-waitを持つ自作ブロック2本:
rx0=6、rx1=12、v0=10、v1=20（OFF/ON一致）。

一回broadcast-and-waitを持つ自作ブロック2本:
rx0=3、rx1=4、v0=10、v1=20（OFF/ON一致）。

引数あり、warp付き、末尾以外のcallの3変種では
無効になった1スクリプトだけを対象から外し、
残り2スクリプトだけを統合した（slots=2）。
他のエージェントの作業はリセットしていない。

testMegaTailProcedureResumeをtests/unit/test_scheduler.cppに追加。
2本の実際のScratch自作ブロックを一つのLLVM関数に統合し、
各反復の残り回数とPCの独立性、Fiber未使用、
緑旗の2回目のキャッシュ利用、効果の正確な再実行を検査。

## 8組交互A/B、CPU16固定、同一バイナリ

eager JIT。オフの通常コンパイル＋Fiber経路と
Mega-Yieldオンを比較。コンパイルと起動を含むプロセス総壁時計。

| プロジェクト | OFF中央値 | ON中央値 | ペア速度比 | ON勝利 |
| --- | ---: | ---: | ---: | ---: |
| 自作ブロック3本、各10,000回の数値repeat | 133.137ms | 129.921ms | 1.0134x | 5/8 |
| 自作ブロック2本、150回・200回のbroadcast-and-wait | 75.944ms | 80.074ms | 0.9624x | 2/8 |

結果:
build-scheduler-rewrite-20261111/mega-tailproc/ab-3x/results.json
build-scheduler-rewrite-20261111/mega-tailproc/ab-bcast/results.json

数値の改善は小さく、broadcast-and-waitでは約3.8%悪化。
ScratchBenchのStageは単純な末尾自作ブロックではないため、
今回の経路は依然としてsched_pingpong/sched_fanwaitには適用できない。

## 取り下げた探索省略実験

XYO_BCAST_JOIN_DEAD_FASTで完了受信者へのmembership scanを
省略する実験も行ったが、sched_fanwaitペア速度比0.970xで悪化。
コードはこのターン内に取り下げた。バイナリの既定動作は不変。

## 次の段階

- 任意のprocedures_call後続処理を保存する明示的return PC
- 複数の手続き呼び出しで生存する引数、GC root、CallFrameの検証
- 時間制限付きrepeat untilの中でbroadcast-and-waitを再開
- イベント受信側にも共有dispatcherを適用
- ScratchBenchの実際のsched_*コードへの適用後に性能とParityを測定

機能は既定OFF。今の限定範囲では一般Scratchの既定機能として
有効化するべきではない。

## 最終ビルド回帰テスト

- XYO_JIT_MEGA_BATCH_YIELD=1：xyo-tests PASS（50.24秒）
- XYO_JIT_MEGA_BATCH_YIELD=0：xyo-tests PASS（43.40秒）
- 単体テストではThreadごとのPC/残反復数・Fiber未使用・
  緑旗再実行のキャッシュ再利用・値の重複加算なしを確認。
- src/jit/rt.cpp と src/engine/sequencer.cpp に入れた
  成績不良のXYO_BCAST_JOIN_DEAD_FAST実験は削除済み。
