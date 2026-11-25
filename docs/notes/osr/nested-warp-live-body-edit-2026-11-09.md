# OSR-009：実行中の Scratch ループ本体の変更を LLVM デオプトで反映

2026-11-09

## 目的

OSR-007 / 008 では、native Fiber の yield 前後にソース変更を検出・復帰することを確認したが、変更対象は**すでに実行を終えた初期化命令**であり、実行後のプログラム意味論が同じだった。

OSR-009 は、LLVM が実際に実行中の **最内側 counted `repeat` 本体の書込命令**を変更する。旧 LLVM を誤って再開すると結果が実際に変わるため、source proof と bounded deopt が変更後の Scratch 命令を実行することを観測できる。

## 検証方法

- `tests/platform/nested_warp_inflight_body_edit_smoke.cpp` は `Runtime::step()` からホストを駆動し、実行中の native JIT Fiber が **`xyo_yield` で停止した後**だけソースを編集する。公開 API の `warpTimeMs=3ms` で停止点を作り、ランタイムコードにはテスト用の改竄フックを入れない。
- `data_changevariableby` の実行対象 `fields["VARIABLE"].id` を **`v_sum` から `v_branchCount`** に変更。今度は実際のエディタ相当の `Blocks::resetCache()` を呼び、改訂番号と source digest の双方が更新されることを確認する。
- `v_sum` は、編集直前に LLVM が確定した値を `sumAtEdit` として厳密な整数で記録する。
- **ガード ON：** LLVM を再開せず、元の Interpreter の `ExecContext::loopCounter` に戻す。変更後の命令を用いるため、最終 `sum == sumAtEdit`、`branchCount == 48,000,000 - sumAtEdit`。
- **ガード OFF：** ソース変更を知らないまま古い LLVM の body が実行される。最終 `sum > sumAtEdit`。ただし次の外側ループは新しい Interpreter に戻り、書込先が変更される。
- 編集を最初の native yield と **4回目の yield** の両方で発生させ、同じ `JitCtx` が複数回の yield / native 再開を経た場合もカウンターが正しく更新されることを確認する。
- 10個の Scratch 変数、元の root/procedure の suffix、動的 TIMES（初期3→17へ途中変化）、日本語・絵文字を含む手続き引数と文字列連結、全ブロック数を比較する。文字列として設定された初期変数 `branchCount="0"` は型タグを保存したまま扱う。

## 実測

元の Interpreter の全加算数は (3 × 8,000,000 × 2 = 48,000,000)。
変更後の `sum + branchCount` は **必ず48,000,000**、`XYO_BLOCK_COUNT=1` での全ブロック数は全方式とも **144,000,043**。

| モード | 実 native OSR | 境界デオプト | 例：最終 sum | 例：最終 branchCount |
|---|---:|---:|---:|---:|
| positive（編集なし、ガードあり） | 3 | 0 | 48,000,000 | "0"（文字列） |
| unguarded（最初のyieldで編集） | 1 | 0 | 16,000,000 | 32,000,000 |
| guarded（最初のyieldで編集） | 1 | 1 | 279,418 | 47,720,582 |
| unguarded-late（4回目のyieldで編集） | 1 | 0 | 16,000,000 | 32,000,000 |
| guarded-late（4回目のyieldで編集） | 1 | 1 | 980,708 | 47,019,292 |

注：`sumAtEdit` は実際の warp yield のタイミング（CPU 負荷等）に依存するので、上表の sum は再実行で変わりうる。テストは固定値ではなく **実際に保存された sumAtEdit と正確に一致**することを確認する。

変更後に未実行分を再生すると、変更先の `branchCount` だけが増え、以前の `sum` は全く増えない。失われた反復・重複した反復・キャッシュの古いフィールド再利用はいずれも検出できる。

## 新規テスト

- `tests/platform/nested_warp_inflight_body_edit_smoke.cpp`
- `tests/differential/jit_nested_warp_inflight_body_edit.py`
- `tests/CMakeLists.txt`：ヘッドレス CTest `xyo-nested-warp-inflight-body-edit`

```bash
# SDL2 に依存しない実行中ソース編集の回帰テスト
ctest --test-dir board/.osr-silent-edit-headless \
  -R xyo-nested-warp-inflight-body-edit --output-on-failure

# 整合ビルド済みのフリーズされた smoke 実行ファイルでも実行可能
python3 tests/differential/jit_nested_warp_inflight_body_edit.py \
  --harness board/osr-active-body-edit-verified-smoke \
  --out board/.osr-active-body-edit-recheck
```

ビルド整合性ハッシュ：

```text
board/osr-active-body-edit-verified-smoke
Source digest: edcbd181ef412a7fbae533d0ae41c2cc1adc66e8b07d5436920066d7e124265f
Binary SHA256: 82b210b04ff68f9f48ed8e4a2f7efcc12145a8796f60bc4cd04ef2aff9ddae67
```

## 制限

今回は OSR-008 のデオプト本体を変更していない。デオプトの対象はあくまで `native yield` に到達した、元 Interpreter の CallFrame が生きている **flat counted repeat の完了済み本体境界**。

同時に異なるスレッドからソースグラフを改造するケース、実行中の machine PC / SSA ローカル / 一部実行済み副作用からの一般的な復帰、内側のネスト repeat の途中からの復帰は未対応である。

共有ワークツリーの他エージェント変更は保持し、コミット・プッシュは行わない。

## 追加：通常の高速経路（ブロック数計数なし）

ブロック数計数は一部のネイティブ scalar / repeat 最適化を無効にするため、
`--without-block-count` でも同じ 5 モードの実 native Fiber テストを実行した。
計数なしの高速経路でも **positive / unguarded / guarded / unguarded-late / guarded-late がすべて PASS**。
通常（編集なし）は Interpreter と全変数が一致し、実行中編集のガードありは
`sum == sumAtEdit` と `branchCount == 48000000 - sumAtEdit` が厳密に一致した。
ガードなしでは元 LLVM が古い `sum` へ書き込むことを対照実験で実測。

追加 CTest `xyo-nested-warp-inflight-body-edit-fast` は同一バイナリで
`--without-block-count` を実行する。

```bash
python3 tests/differential/jit_nested_warp_inflight_body_edit.py \
  --harness board/osr-active-body-edit-verified-v2-smoke \
  --without-block-count --out board/.osr-active-body-edit-fast-recheck
```

本体変更を扱う新規 CTest と以前の source proof 系 2 テスト、合計 3/3 PASS（33.88s、56.39s、58.30s）。
