# OSR-011 — Native LLVM の内側 counted repeat yield から Interpreter へ実復帰

2026-11-09

## 対象

OSR-008 は選択済み外側 counted repeat の「本体完了後」のみ復帰でき、OSR-010 は短い内側 repeat の yield を抑制して完了境界を作っていた。OSR-011 は **内側 `repeat` 自身が本物の native Fiber yield を行い、外側反復の本体がまだ完了していない位置**でも、安全なケースに限って LLVM→Interpreter の状態引き継ぎを可能にする。

機能は既定 OFF：

```bash
export XYO_JIT_OSR=1
export XYO_JIT_OSR_NESTED=1
export XYO_JIT_OSR_NESTED_DEEP=1
export XYO_JIT_OSR_NESTED_PARENT_REPEAT=1
export XYO_JIT_OSR_NESTED_ARG_READS=1
export XYO_JIT_OSR_NESTED_DIRECT_ARGS=1
export XYO_JIT_OSR_NESTED_HOIST_ARGS=1
export XYO_JIT_OSR_NESTED_SOURCE_PROOF=1
export XYO_JIT_OSR_NESTED_INFLIGHT_DEOPT=1
export XYO_JIT_OSR_NESTED_INNER_YIELD_DEOPT=1
```

`INNER_BOUNDED` の「内側 yield を省略する」実験とは別経路。両方を設定しても内側 yield 実復帰を優先し、同じソースループを別途一括実行しない。

## 厳密に許可するソース形状

`src/jit/backend_llvm.cpp` がコンパイル前にソース構造を検証し、次のケースのみ採用する。

- 選択済み warp 手続き内部の counted repeat の本体に **正確に1個** の内側 counted repeat。
- 内側 TIMES は数値プリミティブの**固定整数1〜8192**。動的 TIMES、不正な数値、過大回数は拒否。
- 内側本体は `data_changevariableby` のみで高々4命令、外側直接の先行・後続命令は `data_changevariableby` / `data_setvariableto` に限る。ループ以外のネスト制御、未知ブロック、循環や長すぎるソースを拒否。
- 既存の `safeNestedLoopBody` による reporter 無副作用検証も通す。完全なソース内容ダイジェストによる provenance 証明を必須とする。

コンパイル済み `CompiledScript` に内側 source ID を固定する。移行中のエディタ編集後に現在のソースから任意の「次に実行するループ」を選び直さない。

## ネイティブ実行中のスナップショット

`src/jit/codegen.cpp` は、選択外側ループ本体内の内側 counted repeat の個別 `emitLoopYield` を保持する。計数 OFF でも内側で一括実行・GPU・par-for・strip-mining 等が yield を消さないよう、この検証経路だけ該当最適化を抑制する。その他の JIT モードは変更しない。

実際の `xyo_yield` 直前、昇格済み変数の書込みを反映した後に以下を保存する。両方とも native LLVM の private counter を持ち、現在の Scratch 本体を完了したが loop latch の減算が未実行なので1を引いた値を送る。

- `outerAfterBody = outerNativeCounter - 1`
- `innerAfterBody = innerNativeCounter - 1`

`JitCtx::nestedOsrDeoptPhase` は **1 = 外側本体完了**、**2 = 内側本体完了（外側未完了）** を表す。古い外側の値を誤って「最後の yield」として採用しない。正常なダブルカウンターを保存できない場合は復帰不可として安全終了する。

`src/jit/rt.cpp` に限定的な `xyo_rt_nested_osr_inner_yield_checkpoint` を追加。`src/jit/backend_llvm.cpp` に外部シンボルを登録。

## source 変更時の復帰

`src/engine/sequencer.cpp` では、ネイティブ Fiber が停止中で、ソース改訂番号または immutable digest が変化した時だけデオプトを試す。

1. 停止済み Fiber を次の LLVM 命令へ進めない。
2. 元の Interpreter outer repeat の `ExecContext::loopCounter` を outerAfterBody に更新する。
3. 復帰位相2なら、コンパイル時に固定した内側 repeat source ID が現在も外側本体に属しており、両カウンターが有限の非負整数であることを検証。新しい Interpreter `StackFrame` をひとつ積み、`isLoop=true`、`hasLoopCounter=true`、`loopCounter=innerAfterBody` を設定する。
4. Native Fiber と JitCtx を破棄し、元の手続きフレームと GC 管理の引数マップは保持したまま Interpreter の内側 loop latch から再開する。すでに実行した副作用、外側前処理、内側の最終実行済み本体を再実行しない。
5. 位相不明、ソース制御構造消滅、検証できないカウンターは fail-closed（native を再開しない）。

外側の本体が完了した source yield は従来 OSR-008 の位相1で復帰する。

## 実テスト

`tests/platform/nested_warp_inner_yield_deopt_smoke.cpp` は独立した本物の `Runtime::step()` ドライバであり、製品側へテスト専用割込みフックを入れない。warp budget はテストランタイムのみ 0ms に設定して native 内側ループでの Fiber yield を安定再現する。

`tests/differential/jit_nested_warp_inner_yield_deopt.py` は SB3 を生成。root `repeat(3)` → warp 手続き → 外側 `repeat(40,000)` → 内側 `repeat(128)`（加算）を実行する。絵文字/日本語を含む手続き引数と文字列後続処理を含む。

native の位相2 yield 後に内側 `data_changevariableby` の `VARIABLE.id` を `sum` から `branchCount` に書換え、通常の `Blocks::resetCache()` を実行する。Interpreter への引継ぎ後は厳密に **`sum == sumAtEdit`**、**`branchCount == 30,720,000 - sumAtEdit`**。全変数の型タグ・親手続き後続処理・外側ループの実カウンターを基準 Interpreter と比較する。編集は最初と4回目の実 inner yield の2点で検証する。

数え上げ有効時のブロック実行総数は正確に **46,680,043**。非計数時も同じ変数状態で一致する。内側 `repeat(9000)` は source proof により拒否する。

```bash
ctest --test-dir board/.osr-silent-edit-headless \
  -R '^xyo-nested-warp-inner-yield-deopt(-fast)?$' \
  --output-on-failure
```

追加ファイル：

- `tests/platform/nested_warp_inner_yield_deopt_smoke.cpp`
- `tests/differential/jit_nested_warp_inner_yield_deopt.py`
- `tests/CMakeLists.txt`

## 未対応

これは **固定 source 形状の二重 counted-loop、かつ1つの内側の本体を完了した直後の scheduler yield** に限定。引数や副作用を評価している最中の SSA の途中状態、動的・複数・再帰的な内側ループ、異なるソース編集スレッドとの競合は対象外。IR-001 / RT-001 の汎用デオプト・SSA・CallFrame ABI とは別の実験的経路であり、同一機能と主張しない。

コミット・プッシュはしない。共有ツリーの別エージェントが所有する IR/RT の変更は保護する。

## 最終検証結果

- 新規 CTest: `xyo-nested-warp-inner-yield-deopt` **1/1 PASS（23.12秒）**、`xyo-nested-warp-inner-yield-deopt-fast` **1/1 PASS（23.44秒）**。
- OSR-007〜010 の旧回帰 CTest は **6/6 PASS**。各テストは source edit 28.09s、inflight 39.42s、active body 45.81s、active body fast 46.32s、bounded inner 70.44s、bounded inner fast 73.38s。6件計303.47s。
- **合計 8/8 PASS**。新規の実 native inner-loop yield テストは、手続き引数が GC-rooted 日本語・絵文字文字列のまま残り、元 caller suffix と外側の動的 TIMES が復元されることも確認した。
- 新規2件とは別に、source digest で整合性を検証した凍結バイナリから数え上げ有効／無効の2モードを再実行し、全ケース PASS。最新状態のブロック数ONの全モードで **46,680,043**。

凍結バイナリと再実行コマンド：

```text
board/osr-inner-yield-verified-smoke
Binary SHA256: 8d057971b198e0266e4c0270959ab396d18c43cd3e2bc73915e479407e3cb7c5
Source digest: 6aa025ea03fb56baefcb978c2451de79f72c6b06cff2e32d4775563837bdfb9a
```

```bash
python3 tests/differential/jit_nested_warp_inner_yield_deopt.py \
  --harness board/osr-inner-yield-verified-smoke \
  --out board/.osr-inner-yield-final-count
python3 tests/differential/jit_nested_warp_inner_yield_deopt.py \
  --harness board/osr-inner-yield-verified-smoke \
  --out board/.osr-inner-yield-final-fast --without-block-count
```

注意：今回の検証は headless Runtime＋LLVM であり、これを取り込んだ SDL GUI Parity 2123件はまだ実施していない。LLVM 非計数モードで手続き呼出し3回の全てが native 移行することは保証せず、実際に移行した回数と対応する正常復帰の件数が一致することを検証する。

## 旧 CTest のウォームアップ安定化（2026-11-09）

OSR-012 を含む最新ビルドで10件の OSR 回帰 CTest を実施したところ、旧 OSR-011 の計数ありテスト1件だけが、非同期コンパイル前に短いソース実行を完了してしまい `native LLVM never yielded` で失敗した。LLVM 実復帰自体の失敗と混同しないよう、OSR-011 のデフォルト外側反復を `40,000` → **`120,000`** に増加した。これにより native 移行が実際に発生した状態の復帰を検証する。

修正版 CTest 計数 ON は **PASS（84.01秒）**、native 3/3 promotion/restoration、実行ブロック数 **140,040,043**。旧 40,000 回の fixture のブロック数 46,680,043 は、履歴として保持される値であって新しい CTest の期待値ではない。
