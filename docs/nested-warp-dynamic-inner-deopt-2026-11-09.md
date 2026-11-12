# OSR-012 — 動的 `TIMES` を使う内側 warp repeat の実 yield 後デオプト

2026-11-09

## 要点

OSR-011 では単一の内側 counted `repeat` に対して**固定整数の `TIMES`**（1〜8192）だけを許可していた。OSR-012 は、副作用のないソース式から `TIMES` を評価する場合も、LLVM の内側ループ本体が完了して実際の Fiber yield を行った境界から Interpreter へ復帰できるよう拡張する。

従来のソース改竄拒否、GC 管理の手続き引数、外側の親ループの実カウンター保存と復帰、位相2（内側本体完了）を継承する。**任意の SSA 命令・副作用途中位置からの復帰は対象外**。

既定 OFF。以下のフラグを OSR-011 の設定に追加する：

```bash
export XYO_JIT_OSR_NESTED_INNER_YIELD_DEOPT=1
export XYO_JIT_OSR_NESTED_SOURCE_PROOF=1
export XYO_JIT_OSR_NESTED_INFLIGHT_DEOPT=1
export XYO_JIT_OSR_NESTED_INNER_DYNAMIC=1
```

`INNER_DYNAMIC` 単独では有効にならない。`INNER_BOUNDED` の yield 抑制モードとも独立し、両方指定時は OSR-011/012 の **実 inner Fiber yield** が優先する。

## 静的ソース検証

`src/jit/backend_llvm.cpp` の `safeDynamicInnerTimes` を追加。次の純粋 reporter の AST のみを許可する：

- `data_variable`（実際の `VARIABLE.id` があり、入力・NEXT を持たない）
- `math_number` / `math_integer` / `math_whole_number` / `math_positive_number`（定数は有限値）
- `operator_add` / `operator_subtract`（`NUM1`、`NUM2` がそれぞれ上記の AST）

式には少なくとも1個の変数参照が必要。深さ最大4、節点数最大12、循環や共有節点・不正な入力・未知 reporter は拒否する。さらに OSR-011 の安全な内側本体（単一の内側 `repeat`、小さな直接ステートメント集合）と、`safeNestedLoopBody` の既存の純粋 reporter 検証を要求する。

この動的モードでは、内部の `data_changevariableby` に加えて `data_setvariableto` も認める。ソースの `TIMES` に用いた変数を**内側の実行中に書き換える**テストが可能になる。固定内側ループの通常経路を広げたり、他の拡張・時間・乱数 reporter を許可したりしない。

`operator_multiply` は純粋であっても**本実験で未対応の reporter 形状として拒否**する。任意式を一般に許可するとは主張しない。

## 実行状態の不変条件

LLVM が内側 `TIMES` を初回に評価したあと、プライベートな浮動小数点ループカウンターを保持する。ループ本体でその変数を変更しても、**実行中の同じ counted repeat の回数を再初期化しない**。次の新しい inner repeat に進む場合だけ、更新後の変数を評価する。

`XYO_BLOCK_COUNT=1` 時の Scratch reporter 再評価は行うが、実行中のカウンターを変更しない。

ネイティブ内側本体を完了した直後、直前の Scratch 副作用が反映され、Fiber が yield する前に `outerAfterBody` と `innerAfterBody` を記録。Source proof が変わると native Fiber は次の命令を実行せず破棄される。元の Interpreter の outer フレームを保持し、新たに inner `StackFrame` と保存された counted latch を復元する。

### 小数の `TIMES` に対する修正

Scratch Interpreter の `control_repeat` は初回 `TIMES` を **Math.round 相当（0.5 は正方向）** に丸めてから反復回数を設定する。従来 LLVM は `remaining >= 0.5` の浮動小数点カウンターで同じ実行回数を表していたため、`TIMES=17.5` のネイティブ本体を1回終了すると、未減算状態の private 値から計算した `innerAfterBody=16.5` が整数ではなくなる。

OSR-012 では `src/jit/rt.cpp` の source checkpoint helper に限り、この値を `floor(innerAfterBody + 0.5)` で **Interpreter の整数ラッチ単位**へ変換して保存する。例えば `16.5` は `17`、最後の本体で `-0.5` は `0`。有限・下限 `-0.5`・上限 `2^40` の条件に違反すれば、実行継続可能な checkpoint を発行せず fail-closed。

計数を有効にするかどうかで整数化の結果は変わらない。

## 回帰テスト

`tests/differential/jit_nested_warp_dynamic_inner_deopt.py` が単独で SB3 を生成する。root `repeat(3)` の各親呼出し前に `times` を 17 に設定する。warp 手続きの外側 `repeat(350000)` 内にある新しい inner `repeat` は次の `TIMES` 式で実行：

- `times`：最初17回、以後7回
- `times + 1`：最初18回、以後8回
- `times - 1`：最初16回、以後6回
- `times + 0.5`：Scratch 丸めで最初18回、以後8回

内側本体の1つ目のステートメントは引数 `increment=2` を `sum` へ加算する。2つ目のステートメントは **`times = 7`** で、最初の内側の反復がまだ続いている間に `TIMES` の評価値が変わる。日本語・絵文字を含む引数や GC を使う後続文字列処理も維持する。

実際の native Fiber が **inner yield（位相2）** に到達したあと、ソース `data_changevariableby` の対象を `sum` → `branchCount` に変更し、`Blocks::resetCache` で通常の編集相当の無効化を行う。

正しい結果は `sum == sumAtEdit`、`branchCount == 全加算量 - sumAtEdit`、全変数の型タグ・親の後続処理は Interpreter と一致。最初と4回目の実 inner yield を両方検証する。通常経路・高速経路（`XYO_BLOCK_COUNT=0`）を別々の CTest に登録。

対応しない `TIMES = times * 1` も明示的に拒否することを検証する。また `INNER_DYNAMIC=0` では変数 `TIMES` からネイティブ OSR しない。

期待加算総数（`times` / `times+1` / `times-1` / `times+0.5`）：

- `14,700,060`
- `16,800,060`
- `12,600,060`
- `16,800,060`

```bash
ctest --test-dir board/.osr-silent-edit-headless \
  -R '^xyo-nested-warp-dynamic-inner-deopt(-fast)?$' \
  --output-on-failure

# 再現用の凍結バイナリ
python3 tests/differential/jit_nested_warp_dynamic_inner_deopt.py \
  --harness board/osr-dynamic-inner-verified-smoke \
  --out board/.osr-dynamic-inner-recheck
```

新規テストと変更点：

- `src/jit/backend_llvm.cpp` — 限定 reporter source proof、動的 `TIMES` オプション
- `src/jit/rt.cpp` — 小数カウンターから Scratch source latch への正規化
- `tests/platform/nested_warp_dynamic_inner_deopt_smoke.cpp` — 実 native Fiber yield 後の source edit、変数 TIMES 書き換え状態の検証
- `tests/differential/jit_nested_warp_dynamic_inner_deopt.py`
- `tests/CMakeLists.txt` — 通常／高速 CTest

## 限界

- AST は意図的に狭い。乗除算、複数階層の内側 counted loop、再帰的手続き、任意の拡張 reporter は未対応。
- AST が純粋でも `data_variable` の**実行時の値は静的には上限を証明できない**。ソースを途中で変更した場合、checkpoint に保存できるのは有限かつ安全な範囲内だけ。証明できない checkpoint は fail-closed（その場合の「処理を続けて完走する」保証はない）。
- 任意の LLVM SSA・作用途中 PC、異なる編集スレッドからの同時書換え、通常 GUI Parity の完全検証は対象外。
- 対象は独立した LLVM Fiber の**本体完了後の scheduler yield** に限る。値の途中状態まで復元するものではない。

共有ツリーの他担当者の IR-001 / RT-001 変更は保持し、コミット・プッシュは行わない。

## 追加回帰：移行**後**のネイティブ内側 `TIMES` 変化

最初の18・17回という反復数から7へ変化する処理が Interpreter の OSR ウォームアップ時に完了すると、LLVM 実行中のカウンター更新を検証できない。このテスト穴を塞ぐため、`--stress-active-times` を追加した。

- 選択した外側 `repeat` の**毎回の本体先頭**で `times=17` に戻し、その直後に **`repeat(times)`** を開始する。
- 内側本体の最初の1回が終わると変数 `times=7` に変化する。しかし、LLVM 内側の private counter は17反復を維持しなければならない。次の外側反復の `times=17` は新しい counted repeat の初回評価にだけ適用する。
- 生きている LLVM Fiber の位相2 yield で sum→branchCount のソース編集を実施し、元の native private countdown と Interpreter 側の復帰カウンターを比較する。
- 外側 150,000 反復×各内側17×親3×加算2＝総加算 **15,300,000**。計数 ON/OFF の実行も完全な最終状態と一致させる。

専用 CTests：`xyo-nested-warp-dynamic-inner-active` と `xyo-nested-warp-dynamic-inner-active-fast`。

```bash
python3 tests/differential/jit_nested_warp_dynamic_inner_deopt.py --harness board/osr-dynamic-inner-verified-smoke --out board/.osr-active-times-recheck --outer-count 150000 --stress-active-times
```

## 最終検証記録（2026-11-09）

- `xyo-nested-warp-dynamic-inner-deopt`：PASS（87.54秒）。`times` / `times+1` / `times-1` / `times+0.5` の全てで、実行中 source edit と inner-phase デオプト、完全な Scratch 状態一致を確認。
- `xyo-nested-warp-dynamic-inner-deopt-fast`：PASS（87.65秒）。`XYO_BLOCK_COUNT=0` の高速経路も同じ結果。
- `xyo-nested-warp-dynamic-inner-active`：PASS（19.10秒）。LLVM へ移行した後の各 inner loop で TIMES が 17→7 に変わる入力を追加。加算 15,300,000、実行ブロック数 **41,400,043**、変更後のターゲット変数・root/caller suffix 全て一致。
- `xyo-nested-warp-dynamic-inner-active-fast`：PASS（18.24秒）。計数なしの高速経路で同じ副作用・継続状態一致。
- 以前の OSR-007〜010 の旧回帰6件は同じ製品ビルドで全て PASS。OSR-011 計数ありは非同期コンパイル完了より先に短い入力が終わる不安定性のため最初の試験で失敗。fixture のデフォルト外側反復数を 40,000 → 120,000 に増やして再試験し、計数 ON **PASS（84.01秒、140,040,043ブロック）**、OFF **PASS（77.14秒）**。
- **注意：** 一括10件の最初の実行は9/10（前述のOSR-011のウォームアップ条件で1件失敗）。OSR-011を修正して2件を再実行し、いずれもPASS。新規の追加2件もPASS。12件を一度にまとめた単一の12/12 CTest実行は行っていない。
- 一貫ビルドの凍結バイナリ `board/osr-dynamic-inner-verified-smoke` の SHA256：`4a0201222368eeca74e96b3348d90457851c9e8de62a1f6a6af2dde16b746eea`。ソース digest：`3cf9fe266c0413b520a67b97a65f53bf49b52f7a83826119da076b95510d47b8`。
- OSR-012 と強化テストの検証には、凍結バイナリと**SHA256が同じ** headless CTest 実行ファイルを使った。最新の共有ツリーの再ビルドは別担当の DLSS 型 `render::DlssOpaqueCoverage` 未宣言エラーで停止。描画ソースには手を加えず、ビルド済み実行ファイルで追加テストを成功させた。今回のGUI Parityは未実施。

```bash
ctest --test-dir board/.osr-silent-edit-headless -R '^xyo-nested-warp-dynamic-inner-(deopt|active)(-fast)?$' --output-on-failure
```
