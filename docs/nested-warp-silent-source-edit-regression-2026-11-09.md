# OSR-007：改訂番号を変えない実ソース編集のネイティブ移行拒否

2026-11-09

## 目的

OSR-006 が追加した `XYO_JIT_OSR_NESTED_SOURCE_PROOF=1` のソース内容識別子検証を、LLVM のコンパイル完了後に **実行中の `Blocks` を直接編集する**対照実験で検証する。

既存の `Blocks::revision()` 検査だけでは、`Block` のメンバーを書き換えても `resetCache()` を呼ばない場合、LLVM に埋め込まれたソースと実際のソースの相違を検出できない。

今回は製品コードにテストフックを一切加えず、テスト用の独立した C++ Runtime ドライバから `JitEngine::pollAsync()` と `Runtime::step()` を制御した。

## 改竄のタイミングと中身

1. `Runtime::greenFlag()` で実際の Scratch プロジェクトを開始。
2. Interpreter が root の `set sum = 0` を実行済みであることを `entered >= 1` で確認。
3. `JitEngine::pollAsync()` で完成済み LLVM コードが公開され、`nestedOsrEntry` とコンパイル時のソース改訂番号が一致することを確認。
4. 次の `Runtime::step()` **より前**に、元の root の `data_setvariableto` ブロックの `fields["VARIABLE"].id` を `v_sum` から別の有効な変数 ID `v_done` に変更。**`resetCache()` は呼ばない**。
5. 編集前後の `ScratchCompilationUnit` と `scratchSourceDigest` を実測比較し、ソース内容識別子は変わり、改訂番号は同じことを確認。改訂番号を明示的に上げる対照試験も実施。
6. 続くスケジューラ実行でネイティブ OSR を採用したか検査。最後に Scratch の**全10変数**を、編集なし Interpreter 実行と型タグを含む正規化値で比較。

変更したブロックは、すでに実行を終えた root の初期化であるため、現在進行中のループと残りの Scratch 実行結果は編集によって変わらない。古い LLVM 実行を拒否できるかだけを切り離して測定できる。

## 追加ファイル・登録

- `tests/platform/nested_warp_silent_edit_smoke.cpp`：実行中の Runtime、公開済み LLVM、実ソース直接編集を制御。製品コードへのフック追加なし。
- `tests/differential/jit_nested_warp_silent_edit.py`：実プロジェクト作成と5条件の実移行・復帰・全変数比較。
- `tests/CMakeLists.txt`：`xyo-nested-warp-silent-edit-smoke` を構築、SDL2 不要の **CTest `xyo-nested-warp-silent-edit`** を登録。ヘッドレスのため `SDL2_FOUND` に依存させない。

## 実測結果

8,000,000回の内側 counted repeat を外側 `repeat(3)` から呼ぶ入力。初期 root の変数 `sum` は1度だけ初期化され、ループ3回と内部変更によって `sum=48,000,000`、`entered=3`、`done=1`、回数指定変数 `times=17` となる。

| モード | source proof | ソース直接編集 | revision | 実 LLVM 移行 | 実復帰 | ソース証明受理 | 全10変数 |
|---|---|---|---|---:|---:|---:|---|
| baseline | OFF | なし | 元 | なし | なし | なし | 一致 |
| positive | ON | なし | 元 | **3** | **3** | **3** | 一致 |
| unguarded | OFF | あり | 同一 | **3** | **3** | 0 | 一致 |
| guarded | **ON** | **あり** | **同一** | **0** | **0** | **0** | **一致** |
| versioned | ON | あり | 変更 | **0** | **0** | **0** | 一致 |

`unguarded` と `guarded` の変更元、タイミング、ソース改訂番号が同一である点が重要。新しい proof ON 経路だけが source digest の差異で既存 LLVM コードの採用を阻止した。改訂番号検査だけでは阻止できない。

新規 CTest **1/1 PASS（27.60秒）**、凍結バイナリへの独立再実行でも4モード全部 PASS。

## ビルド

共有ツリーの別エージェントによる SDL レンダラ変更に `realDlssEnabled_` 未宣言の独立ビルドエラーがあったため、そのコードには手を触れず、`-DXYO_SDL2=OFF` の独立ヘッドレス構成を使用した。

```bash
cmake -S . -B board/.osr-silent-edit-headless -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DXYO_JIT=ON -DXYO_JIT_BACKEND=llvm \
  -DXYO_SDL2=OFF -DXYO_BUILD_TESTS=ON
cmake --build board/.osr-silent-edit-headless \
  --target xyo-nested-warp-silent-edit-smoke --parallel 3
ctest --test-dir board/.osr-silent-edit-headless \
  -R xyo-nested-warp-silent-edit --output-on-failure
```

凍結・整合検証済み C++ テスト実行ファイル：

```text
board/osr-silent-edit-verified-smoke
Binary SHA256: bc8a50d1c4a055d08d6a2e9dba6be0f0207aca0fe2cd26fb61c4d3fbdf0049bd
Source SHA256 (src/CMake fingerprint): 02f98a30868578888064cc4e051825bbc875bf7ed79766c8e6f97fa0535c37a0
```

`tools/consistent-build.ts` は `--artifact tests/xyo-nested-warp-silent-edit-smoke` で C++ テストバイナリも凍結できる。既定の `xyo` バイナリを対象にしたままだと、今回のテスト専用ターゲットではコピー元パスが異なるためエラーになる点に注意。

## 未解決

この検証は「LLVM 採用前にソースが変化した」ケースだけを対象とする。**LLVM 実行開始後の編集**を途中で検出して元の source PC / SSA / effect state に戻す汎用デオプト機構はまだない。IR-001 / RT-001 の CallFrame・副作用復元統合は引き続き必要。

IR-001・RT-001 所有のソース、既存 OSR の本体コードは今回変更していない。コミット・プッシュなし。
