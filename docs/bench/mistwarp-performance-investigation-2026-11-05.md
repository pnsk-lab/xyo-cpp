# MistWarpとxyo-cppのループ性能調査（2026-11-05）

## 実装上の違い

MistWarpの [3d898e6](https://github.com/MistWarp/scratch-vm/commit/3d898e6cbef95ae25ff8354a6dbbfcb9753a50c9) は安全なwarpループ内のScratch変数をJavaScriptローカル変数へ昇格し、V8レジスタ保持を実現した。コミット本文の改善率（変数更新14.7倍など）はMistWarp自身の変更前との比較であり、xyo-cppとの直接比較ではない。

MistWarpの `src/engine/runtime.js` は `compilerOptions.warpTimer: false` が既定。`src/compiler/jsgen.js` はwarp中かつwarpTimerが無効ならループの時間監視を省略する。一方xyo-cppは64反復ごとに時間監視する。これらの実行条件を合わせずに単純な倍率比較はできない。

xyo-cppにも `src/jit/codegen.cpp` の `beginLoopPromotion` と `spillPromoted`/`reloadPromoted` があり、MistWarpと同系統のループ内SSA変数昇格は実装済み。

## 同一バイナリでの実測

ビルド済みの `build/xyo`、CPU12固定、`--jit eager --fps 30`、`XYO_GPU=0 XYO_PARFOR=0`。ScratchBenchの実ワークロードを切り出した既存SB3を使用した。数値はops/ms。

- loops_until: 展開OFF 1.530/1.493/1.477M、展開ON 1.553/1.490/1.518M。明確な改善なし。
- vars_setchange: 交互2組でOFF 2.031/2.032M、ON 2.318/2.341M。約14〜15%改善。チェックサム一致。
- loops_repeat: 交互2組でOFF 2.350/2.337M、ON 2.356/2.329M。明確な改善なし。チェックサム一致。
- 47項目の全体ScratchBench SB3ではOFFは約34秒で47/47完走。正式スコアv2で59,482（本環境限定）。
- `XYO_REPEAT_UNROLL=1` の全47項目は140秒経っても最初の結果を出せずタイムアウト。生成IR増大によるコンパイル費用が大きい可能性。現状の一律ONは不適切。

ログは `build/mist-compare-*`、`build/mist-full-default.out`、`build/mist-full-unroll.out`。

## LLVM IR調査

`build/mist-loops-ir.ll` にloops_untilの生成IRを保存。内部変数はSSAへ昇格するが、外側の時間判定にsensing_dayssince2000があり、外側は昇格対象外。内側のホットループにも値の型判定と64回に1回のwarp時計監視が残る。

warp手続き内のisWarp定数伝播を試作したが、ビルド環境がアセンブラ一時ファイルをENOSPCで拒否したため実行検証できず、**試作差分は撤回**した。現時点でコードの追跡対象ファイルへの変更はない。

## 未完了と次の作業

同一マシン上のMistWarpをbun installしようとしたが、書込可能TMPDIR指定でもENOSPCで停止。ホストは約57GiB空いていたがsandbox側の一時ディレクトリ作成が失敗。したがって**MistWarpとxyo-cppの直接実測はまだない**。

次はビルド・一時ディレクトリの書込環境を修復し、両者のwarpタイマー設定を揃えて交互実測する。その上で、warp状態の静的証明による不要分岐削減と、コード予算制限付きの少数回ループ展開を、パリティを保持しながら検証する。