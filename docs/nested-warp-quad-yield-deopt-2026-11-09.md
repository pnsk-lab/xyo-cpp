# OSR-014：4階層の動的 repeat における実 native yield 後のデオプト

2026-11-09

## 実装範囲

OSR-013 の3階層 source counted-repeat 継続を、**選択済み外側 repeat＋3つの内側 repeat の4階層**に拡張した。中間の `TIMES` は副作用のない変数／定数／加算／減算の式から評価可能。各ループは入力の初回評価後に LLVM の private counter を保持し、最深部の本体内で `TIMES` の変数を変更しても実行中カウンターを再初期化しない。

新しい経路は既定 OFF。以下は必要な条件の主要部分であり、従来のネストした warp OSR 用の親フラグも必要。

```bash
export XYO_JIT_OSR_NESTED_SOURCE_PROOF=1
export XYO_JIT_OSR_NESTED_INFLIGHT_DEOPT=1
export XYO_JIT_OSR_NESTED_INNER_YIELD_DEOPT=1
export XYO_JIT_OSR_NESTED_INNER_DYNAMIC=1
export XYO_JIT_OSR_NESTED_TRIPLE_YIELD=1
export XYO_JIT_OSR_NESTED_QUAD_YIELD=1
```

`QUAD_YIELD=1` だけでは有効にならず、前提のフラグとソース証明が揃わなければ4階層の LLVM OSR には移行しない。従来のOSR-011/012/013経路では、同じビットフラグを有効にする必要はない。

## 安全性とコードの流れ

`src/jit/backend_llvm.cpp` の `safeBoundedNativeInnerRepeat` に追加の最大深さ（1, 2, 3）を渡し、厳密に対応する数のネストされた repeat と期待する直列ステートメントのソースグラフのみ許可する。SOURCE_PROOF 下で各 static ID をコンパイル時に凍結。第4の `control_repeat` を1個だけ追加した source 形状は、**必ず4階層を明示的に有効にした場合にだけ選択**される。5階層や複数分岐、動的な副作用つき reporter は拒否する。

`src/jit/codegen.cpp`：最深部 counted repeat の本体が1回完了し、共有変数の書込みを公開した直後の、本物の native Fiber yield に限定して `xyo_rt_nested_osr_quad_yield_checkpoint` を呼び出す。3つの未完了祖先ループの private counter と、最深部のカウンターをまとめて渡す。中間ループの完了を装って先行処理や副作用を再実行することはない。

`src/jit/rt.cpp`：`JitCtx` の追加フィールド `nestedOsrDeoptFourthRemaining` を利用。**4つの counter が同時に妥当な場合のみ**継続可能な phase=4 を公開し、途中で検証に失敗した場合は古い phase=1/2/3 を再利用しない。中間・深部・最深部の浮動小数点カウンターは `floor(afterBody+0.5)` で Scratch Interpreter の整数ループラッチ単位に変換し、有効な有限・非負整数だけを保存。コンパイル済みコードが凍結した source ID のみを使用する。

`src/engine/sequencer.cpp`：共有ソース改訂や内容ダイジェストが変更された場合、**停止している LLVM Fiber を再開せず先に破棄**。親→子→孫の3つの control SUBSTACK 到達関係を別個に再検証し、保存された4つのカウンター・元 Interpreter の選択外側ループフレーム・GC-rooted 引数を確認する。その後、元の外側 loopCounter を上書きし、その上に3つの inner `StackFrame` を元の source ID 順に構築。最深部から本来の Interpreter latch で再開し、途中の副作用、既に実行した inner body、後続の caller suffix は重複実行しない。

通常の LLVM 実行時・ソース変更なしの処理はこれまでどおり。任意の LLVM SSA 命令途中PCではなく、最深部の本体完了後に実際の Fiber scheduler yield が起こった境界限定。

## 実測テスト

`tests/differential/jit_nested_warp_quad_yield_deopt.py` は、root `repeat(3)` → warp 手続き → outer `repeat(40000)` → middle `repeat(times−15)` → deep `repeat(times−15)` → leaf `repeat(times)` を作る。各 middle・deep の本体先頭で `times=17`、leaf 本体で `times=7` と変更するため、元の private loop counter を維持できなければ全体の加算総数と最後の変数状態がずれる。変数・文字列・GC管理の引数と後続処理も Interpreter 基準値と比較。

実 native phase-4 yield の1回目と4回目で、エディタ相当の `Blocks::resetCache()` により leaf の `data_changevariableby` 対象を `sum` から `branchCount` に変更する。**最終 sum == 編集直前 sum**、**branchCount == 全加算数 − 編集直前 sum**、全その他の Scratch 変数とブロック計数が Interpreter と一致することを必須条件とする。

| 入力 | 期待加算量 | 最深部回数 |
|---|---:|---:|
| 整数：`times−15`・`times` | 16,320,000 | 17 |
| 小数：`times−15.5`・`times+0.5` | 17,280,000 | 18 |

明示的フラグ無効の拒否、旧 triple フラグをONにし quad のみOFFにした拒否、5階層の拒否をいずれも Interpreter 完全一致で検証する。

```bash
ctest --test-dir board/.osr-silent-edit-headless \
  -R '^xyo-nested-warp-quad-yield-(deopt|deopt-fast|fractional|fractional-fast)$' \
  --output-on-failure
```

対応ファイル：

- `src/jit/backend_llvm.cpp`
- `src/jit/codegen.cpp`
- `src/jit/rt.cpp`
- `src/jit/jit.hpp`
- `src/engine/sequencer.cpp`
- `tests/platform/nested_warp_quad_yield_deopt_smoke.cpp`
- `tests/differential/jit_nested_warp_quad_yield_deopt.py`
- `tests/CMakeLists.txt`

SDL が無効の headless ビルドでは、並行開発中の `src/render/sdl_renderer.hpp` で `render::DlssOpaqueCoverage` が見つからない不整合があるため、**対象ビルドディレクトリの Ninja コマンドにだけ必要なヘッダーを `-include` する**一時回避を適用した。製品側の描画ソースは編集していない。この限定的な回避で OSR-014 の全ソースを再コンパイルしてリンクし、実行検証している。

## 制限

1. 最大4階層、単一路線の直列ステートメントと副作用のない指定 reporter のみ。5階層以上や任意の分岐・再帰的 loop は未対応。
2. ブロックを評価中の LLVM SSA PC・レジスタ状態・GC 汎用デオプトは未実装。各 source loop の**1回の本体完了後の実 yield**に限定。
3. 省略不能な source 編集後の階層構造確認に失敗した場合は fail-closed であり、実行中のプログラムを黙って再実行しない。
4. 通常 GUI の全 Parity と、競合する独立エディタスレッドからの更新はまだ確認していない。

この作業ではコミット・プッシュせず、他担当の IR-001／RT-001 および DLSS の共有作業を上書きしない。
