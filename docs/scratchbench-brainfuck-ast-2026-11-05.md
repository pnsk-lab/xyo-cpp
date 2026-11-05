# ScratchBench app_brainfuck / app_ast JIT高速化（2026-11-05）

## 変更

- `src/jit/codegen.cpp` `eqAsciiWord` / `emitEqAsciiWord`：非数値の短いASCII文字列リテラルに記号を許す。従来 `a..z` に限定されていたためBrainfuckの `+ - < > [ ]` はすべて汎用 `xyo_eq` に落ちていた。文字列の1文字比較はUTF-16長・ASCIIコード・大小文字をJITで直接検査し、非文字列、非ASCII、連結文字列などは従来ヘルパに戻す。
- `canElideWarpRecursiveFrames`：完全にLLVMへloweringでき、全てwarpで、明示的なyield/compatやインタプリタスタック観測を含まない再帰手続きグラフに対し、リスト操作・文字列操作・warpループもフレーム不要と証明する。warpループに挿入される500msタイマーはネイティブfiber自体を中断・再開するため、インタプリタ用手続きフレームを要求しない。`XYO_JIT_RECURSIVE_FRAME_ELIDE=0`で新しい判定のみ無効化できる。
- `operator_contains`：第1引数が固定文字列 `"0123456789"` のとき、第2引数が単一ASCII UTF-16文字であることを実行時ガードして数字範囲判定を行う。それ以外は `xyo_strcontains` へ戻す。`XYO_JIT_DIGIT_CONTAINS=0`で無効化できる。

## 実測（i7-12700、Release、JIT、30fps、4回の交互実行の中央値）

ScratchBench元の `performance/project/Stage/apps.fractch` から生成した単独SB3で、計時はプロジェクト内の約600msの実時間窓。どの条件でもチェックサム一致。

| 試験 | 変更前 ops/ms | 変更後 ops/ms | 倍率 | checksum |
|---|---:|---:|---:|---:|
| `app_brainfuck` | 33,831 | 136,629 | **4.04x** | `931125` |
| `app_ast`（再帰フレーム省略のみ） | 7,568 | 25,269 | **3.34x** | `403` |
| `app_ast`（再帰フレーム省略＋数字集合contains） | 7,451 | 32,275 | **4.33x** | `403` |

ASTの分離実験（後半の同一測定系列）では、フレーム省略のみ 25,196、数字containsのみ 9,481、両方 32,275 ops/ms（各4回中央値）。最適化無効のASTは 7,451 ops/ms。

プロファイラ付きのAST実行では、`xyo_proc_push` は約786万回から7回に減少。Brainfuckの `xyo_eq` は約1460万回からホットパス上では呼ばれない状態になった。プロファイラを有効にすると処理回数が変わるため、絶対コストの比較は通常計測のみを使用する。

## 検証

- `tests/differential/jit_ascii_fastpaths.py`：記号/英字/Unicode/空文字/数字型との等値比較、数字集合contains、リストとwarpループを伴う121段の再帰。インタプリタ、eager JIT、HotSpot、各フォールバックを照合。
- 既存CTest：`xyo-tests`、`xyo-interpreter-parity-regressions`、`xyo-jit-lazy-integration`、`xyo-jit-transient-strings`、`xyo-jit-ascii-fastpaths` が成功（追加のUnicode文字列高速化は別の未コミット作業であり、このリリースには含めない）。
- 独立測定ログ `build/diagnose-brainfuck-ast/paired-benchmark.json`、`digits-ab.json`、プロファイラ出力 `final-*.err`。

ベンチの計測期間中に並行したユーザー作業が存在しうるため、さらに厳密なスコア比較にはCPU固定・実行順乱択・複数セッションを推奨する。機能の一般的な速度保証ではない。
