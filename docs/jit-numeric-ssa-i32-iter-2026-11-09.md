# MATH-018 — 数値SSA反復カウンターの32bit化（2026-11-09）

## 対象・安全性

対象: src/jit/numeric_repeat_ssa.inc の emitNumericRepeatChunk。

ネイティブ反復は chunk = min(remaining32, 64 - (ticks & 63)) により、常に64回以内に制限される。このため rep.numeric.iter のPHI、加算、比較をi64からi32に縮小できる。一方、schedulerのtick、chunk、yieldおよびカウンターの同期処理は旧コードのまま維持する。比較部分でのみchunkをi32へ変換する。

既定ON。XYO_JIT_NUMERIC_REPEAT_I32_ITER=0 で旧64bitカウンターに戻せる。

## LLVM IR

評価用バイナリ: build/math018-iter-xyo。
- Arithmetic: phi i64 → phi i32、比較i64→i32。ネイティブhead IR 1298文字→1298文字。
- vars_many: phi i64 → phi i32、比較i64→i32。ネイティブhead IR 4057文字→4088文字。
- チェックサムはArithmetic 620203352、vars_many 1588830、ON/OFF一致。
- ログ: build/math018-arith-ir-{0,1}.err、build/math018-vars_many-ir-{0,1}.err。

## 交互性能測定

CPU12固定、同一バイナリ8組ABBA/BAAB、組内のドリフト8%以下を安定と定義。

- Arithmetic: 安定8/8組、ON/OFF速度比中央値1.0013549倍。
- vars_many: 安定6/8組、中央値1.0083165倍（+0.83%）。
- 全測定でチェックサム一致。
- 生データ: build/math018-iter-abba.json、build/math018-iter-abba.log。

改善は小さく、ハードウェア・ホスト負荷で差が変わる可能性がある。

## 正確性・回帰

- 整数幅をまたぐ境界値770観測値×10構成: Interpreterと全件一致。build/math018-u32-diff.log。
- 旧numeric_repeat、numeric_until、repeat_copy差分テスト: PASS。repeat(65)や64境界を含む。
- Scratch-bench parity: HotSpot / Eager / Interpreter / AOT、各2123/2123。build/math018-parity.log。
- CTest xyo-tests: 1/1 PASS、44.70秒。build/math018-ctest.log。
- 47項目の性能プロジェクトをOFF/ON双方で完走。非schedulerチェックサム相違0。
  ログ: build/math018-full-off.log、build/math018-full-on.log。
  全項目の連続実行ではmath/vars速度変化は小さく、これを改善倍率の根拠とはしない。

## 採否

採用（既定ON）。i32チャンクカウンターは最大64回の上界から安全性が明白であり、追加変更が小さい。パリティ、CTest、全47項目の結果を保ち、交互測定ではわずかに有利だった。機能を簡単にOFFへ切り替えられる。

なおMATH-017の遅延ボックス化は、生成IRを削減したが24script×8変数の8組ABBAでプロセスCPU時間新/旧比1.02856（約2.86%悪化）だったため撤回済。記録はdocs/jit-numeric-lazy-box-evaluation-2026-11-09.md。

他エージェントのcodegen.cpp、RT/IR、scheduler、rendererは変更しておらず、未commit・未push。
