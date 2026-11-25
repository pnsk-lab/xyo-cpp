# Mega Scheduler β — source-certified 単独Threadの事前分類 (2026-11-14)

状態：実験機能、既定OFF、未コミット。他エージェントの既存変更を保持。

## 設計

前回までの単独Thread早期経路 (`XYO_JIT_MEGA_SINGLE_FAST_SCAN`) は
`tryMegaScalarBatch` 内で7種類のsource metadata配列の範囲・条件を毎回検証していた。
実行後のソース形状は変わらない（変化した場合は `Blocks::revision()` で
コンパイル結果そのものが拒否される）ので、ソース証明をLLVMコンパイル時に一度だけ行う。

`MegaDispatchCode::singleFastSourceKinds` にsource slotごとの分類を保存：

- 0: 単独高速経路非対応（分岐、2変数加算、戻り先suffix等）
- 1: 1変数の単発加算（source-certified `Scalar`）
- 2: 1変数のカウント付き反復加算（source-certified `Repeat`）

`XYO_JIT_MEGA_SINGLE_PREPROVE=1` が有効な場合、
C++側の1件特化経路はこの分類だけから静的なsource適格性を取り出す。
旧来の7種類の metadata スキャンはフラグOFF時用に残し、交互A/B比較を可能にした。

重要：コンパイル時分類 **だけ** では実行を許可しない。
source revision、Thread状態、JitCtx/Fiber/OSR不在、スケジューラ順序、
PCの偶数・奇数、残反復数（`multiPcRepetitions`）、変数のlocal/Stage世代、
ターゲット同一性、数値の型・有限性等は毎回検証。
不適合時は既存の一般走査経路へ移り、Scratchの型変換を維持する。

## 実行方法

```sh
XYO_JIT_MODE=eager \
XYO_JIT_MEGA_BATCH_YIELD=1 \
XYO_JIT_MEGA_DIRECT_SCALAR=1 \
XYO_JIT_MEGA_SCALAR_BATCH=1 \
XYO_JIT_MEGA_MULTI_PC_BATCH=1 \
XYO_JIT_MEGA_SINGLE_DISPATCH=1 \
XYO_JIT_MEGA_SINGLE_SPECIALIZE=1 \
XYO_JIT_MEGA_SINGLE_FAST_SCAN=1 \
XYO_JIT_MEGA_SINGLE_PREPROVE=1 \
./build-scheduler-rewrite-20261111/xyo run example.sb3 --fps 0
```

ブロードキャスト受信者の融合が必要なら
`XYO_JIT_MEGA_BCAST_RECEIVERS=1` を追加。

## 対応ペア性能測定

`tests/bench/algos/bench_mega_single_dispatch.py --preprove`
で、固定CPU16、同一凍結Releaseバイナリ、同一SB3、
Eager JIT O1、起動・JITコンパイル込みの壁時計時間を測定。
対象：反復300,000回 + 1回のgreen flagスクリプト。
他の全フラグは両条件で同じ。24組でON/OFF実行順を交互に入れ替える。

| 指標 | 事前分類OFF | 事前分類ON |
|---|---:|---:|
| 実行時間中央値 | 178.802 ms | 176.788 ms |
| 対応ペア速度比 (幾何平均) | 1.000 | **1.00670566** |
| ON勝利数 | — | 16/24 |
| LLVM batch呼出し | 300001 | 300001 |
| 処理Thread slot | 300003 | 300003 |

生データ：
`build-scheduler-rewrite-20261111/mega-multi-pc/bench-single-preprove-300k/results.json`

この結果は約0.67%の小幅改善。環境差・計測ノイズと比較して
優位性はまだ限定的であり、実プロジェクトでの再現性は未検証。
Scratchの実行passやYield回数は変わらない。

## 安全性テスト

- Release `xyo-cli`、`xyo-tests` ビルド成功。
- 集中テスト：PREPROVE ON/OFFいずれも **31/31 PASS**。
- source分類検証：混合Scalar/Repeatは `{2,2,2,1,2}`。
  条件分岐系は `{0,0,0,0}`。単純な2スクリプトは `{2,2}`。
- 途中で変数 map から `s` を消して再作成：世代の不一致を検出して
  旧バインディングを触らず通常経路で再束縛し、次パスから高速経路に戻る。
- キャッシュ世代が変わらないまま値を数値から文字列に変更：
  早期経路を拒否し、通常のScratch数値変換後の最終値が一致。
- `git diff --check` とベンチPython構文検証PASS。

## 結論

測定できた改善幅は小さいため、既定OFFのまま。
既存未コミット変更のreset、commit、pushはしていない。

全体回帰ログ：
`build-scheduler-rewrite-20261111/mega-multi-pc/single-preprove-full-on.out` と
`single-preprove-full-off.out`。

## 最終全体回帰 (Eager JIT, 同一ビルド)

- PREPROVE ON: exit=0、Scheduler **592/592 PASS**、その他全テスト群で失敗0。
- PREPROVE OFF: exit=0、Scheduler **592/592 PASS**、その他全テスト群で失敗0。
- Fiber / GC deferral / multi-Fiber GC barrier smoke: **3/3 PASS**。
- WAIT_WAKE_REGISTRY=1: fast source proofを持つ構成でも通常経路に
  フォールバックし、最終値 v0=300000、v1=2、exit=0。
- Python構文チェック / git diff --check: PASS。

専用化フラグは既定OFF。ベンチ以外の実Scratch作品に対する
性能改善や、未知の入力全体への安全性を証明するものではない。
