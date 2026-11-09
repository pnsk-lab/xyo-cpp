# 正確な Scratch ブロック実行回数の計測

`xyo run project.sb3 --block-count` または
`XYO_BLOCK_COUNT=1 xyo run project.sb3` で有効化する。
`xyo play` でも同じフラグを使える。終了時に標準エラーへ出力する。

```text
[block-count] executed=10 elapsed=0.001234567s blocks/s=8100.00 (Scratch interpreter/native JIT)
```

平均 `blocks/s` は `executed / elapsed` で、`elapsed` は
緑旗実行開始から最後の実行フレームまでの実時間（秒）。
プロジェクトの読み込み・実行終了後のファイル保存時間は除外する。
フレーム待ち・明示的な `--fps` による待機・描画は経過時間に含むので、
CPU だけの演算速度とは異なる。カウント 0 または経過時間 0 なら
`blocks/s=0`。表示例の数字は形式の説明用で実測ではない。

## 何を数えるか

実際に実行された Scratch のプリミティブ（スタックブロックと
計算・真偽値のリポータ）を、呼び出しごとに1回加算する。
`BlockCached::ops` 内で実際に呼ばれた `blockFunction` が計数点で、
リテラル値／ドロップダウンの shadow・実行されていない分岐・
未評価の入力は数えない。ハットの起動や未呼び出しの手続き定義は含めない。手続きが実際に呼ばれた場合、`procedures_definition` の実行は1回数える。

ループの条件・制御ブロックは再評価のたびに加算する。例えば
`repeat (3)` は本文が3回、制御ブロック自体は終了時の判定を
含めて4回実行される。条件式のリポータも評価した回数だけ数える。
待機で再試行したブロックの実際の再呼び出しも加算対象。
値の結果が同じでも加算を省略しない。

## 正確性と実行速度

`--block-count --jit=eager` / `--block-count --jit=hotspot` は
ネイティブ LLVM JIT で計数する。LLVM IR は各ブロックで `JitCtx` 内の
非 atomic `pendingBlockCount` を加算する。各スレッドは専用の
`JitCtx` を所有し、yield 前とコンテキスト解放前（正常終了・停止・
中断したスレッドの破棄）に限って共有の
`Runtime::executedBlocks` へ `fetch_add(relaxed)` でまとめて反映する。
複数回 flush しても二重計数しない。コンパイル済みの手続きからも同じ
ローカルカウンターに加算し、`procedures_definition` も計数対象。

コンパイル済みのループは各反復と終了時に制御ブロックを数え、
`repeat` の TIMES リポータも各反復の条件再評価時に実際に評価する。
JIT のループ融合・ループ本文コピーの先読み、並列/GPU 化、
リスト append の並列化、文字列 transient 集約、pen setter の
ループ外移動など、計数位置が消える最適化は計数モードに限り停止する。
LLVM による通常の型・算術最適化は残る。
`blocks/s` は**計数処理込みの速度**で、通常の JIT の性能ではない。
初回 JIT コンパイル時間も `elapsed` に含む。

`--jit=off` はインタプリタ実行として計数する。
Browser WASM/Binaryen JIT はまだ計数用コードを生成しないため、
この計数モードではインタプリタへフォールバックする（JIT速度は測れない）。
`--aot` や組込み AOT 実行ファイルは正確な計数を保証できないため
エラーにする。

C++ からは `Runtime::executedBlocks.load()` で累積値を読み、
`exchange(0)` で測定区間をリセットできる。計数モードはスクリプト
開始前に設定し、プロジェクト内の全スレッド・クローンを合計する。

検証: `python3 tests/differential/block_execution_count.py build/xyo`。
