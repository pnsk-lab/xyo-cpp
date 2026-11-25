# OSR-013 — 3階層の動的 counted repeat の LLVM 実 yield → Interpreter 復帰

2026-11-09

## 対象と非対象

OSR-011 は `外側 repeat → 内側 repeat`、OSR-012 は可変 `TIMES` と小数丸めに対応した。OSR-013 は **`選択された外側 repeat → 中間 repeat → 最深部 repeat` の3階層**で、最深部の native LLVM Fiber が本物の scheduler yield を行った後に source edit を検出し、3つの実カウンターから Interpreter を復帰する。中間・最深部の `TIMES` は副作用なしの変数参照・加減算が可能。

これは **各最深部ループ本体が完全に終わった後**の yield に限定される。任意の命令位置や部分計算済み SSA の復元、4階層以上、制御構造が変更された状態の継続を保証しない。

既定 OFF。OSR-011/012 の source proof と in-flight guard を含む設定に追加する。

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
export XYO_JIT_OSR_NESTED_INNER_DYNAMIC=1
export XYO_JIT_OSR_NESTED_TRIPLE_YIELD=1
```

## 静的ソース証明

`src/jit/backend_llvm.cpp` の `safeBoundedNativeInnerRepeat` を、単一の内側 counted repeat の検証から、設定された上限まで再帰的にソース制御列を追跡する検証へ拡張した。3階層経路では中間・最深部の source ID を immutable `CompiledScript` metadata に固定。深い階層、未知ブロック、循環、2個を超える内側 `repeat`、長い・複雑な副作用列は拒否する。

`TIMES` はOSR-012で許可した数値リテラル（1〜8192）または深さ4・最大12ノードの純粋な変数／加減算式だけ。深さ2の source body まで、直接の単純な数値変数増減・代入だけを認める。ネストした実行は常に正確な元の LLVM private loop counter に基づき、変数の値から現在回数を再計算しない。

4番目の `repeat` を追加した実際の SB3 では native OSR を拒否し、Interpreter とすべての変数値・実行ブロック数を比較して一致した。

## 保存 ABI（試験的・既定 OFF）

`src/jit/codegen.cpp` は、`curYieldSourceLoop` と `yieldStack` から native source path を構築する。

- 位相1：選択された外側 repeat の本体が終了。従来の外側カウンターのみ。
- 位相2：中間 repeat の本体が終了、外側は未完了。外側・中間の2カウンター。
- **位相3：最深部 repeat の本体が終了、中間・外側はまだ未完了。外側・中間・最深部の3カウンター。**

native LLVM のプライベートカウンターは各本体完了時にはまだ loop latch による減算前。各値から1を引いて渡す。 `src/jit/rt.cpp` の `xyo_rt_nested_osr_triple_yield_checkpoint` では、中間・最深部の `f64` を Scratch `repeat` の整数ラッチへ `floor(afterBody+0.5)` で正規化する。非有限値、過大値、未証明の位相は拒否し、古い checkpoint を誤用しない。

`JitCtx` に `nestedOsrDeoptDeepRemaining`、`CompiledScript` に `nestedOsrDeepInnerLoopId` を**末尾追加**し、既存 ABI オフセットを変更しない。`backend_llvm.cpp` に専用ネイティブシンボルを登録。

## source edit 後の復帰

`src/engine/sequencer.cpp` は編集による改訂番号または immutable source digest 変更を検出した後、停止済み Fiber から次の命令を実行しない。コンパイル時に固定した中間・最深部の source ID が**変更後のソースでも同じ外側→中間→最深部の経路に残っていること**を確認する。カウンターが有限・非負の整数であり、元の Interpreter 最外ループフレームが生存していることを確認してから、Fiber を破棄する。

元の Interpreter 外側フレームと GC-rooted 引数を保持し、上に中間・最深部の `StackFrame` を順に積み、保存済みの `ExecContext::loopCounter` を設定する。内側の実行済み加算、先行する中間ループの本体、外側の prefix を再生しない。制御構造が変更された場合や source counters が証明できない場合は、効果の二重実行を避けるため fail-closed でそのスレッドを終了する。

## 実際の回帰テスト

`tests/differential/jit_nested_warp_triple_yield_deopt.py` は Scratch SB3 を新規生成し、`tests/platform/nested_warp_triple_yield_deopt_smoke.cpp` が SDL2 無効の Runtime と本物の native Fiber yield を使用する。テスト専用の製品フックは追加しない。

主な形状：

```text
root repeat(3)
  warp procedure
    repeat(100000)
      times = 17
      repeat(times - 15)       # 中間は2回
        times = 17
        repeat(times)          # 最深部は17回
          change sum by 2
          times = 7
      scratch = ...
```

各ソース変数は現在の inner `TIMES` を変化させるが、アクティブな LLVM loop counter はリセットしない。最深部の本物の位相3 native yield を観測してから `data_changevariableby` の書込み先を `sum` → `branchCount` に変更し、`Blocks::resetCache()` で改訂番号とソースダイジェストを更新する。

最初と4回目の yield で編集して、`sum == sumAtEdit`、`branchCount == 20400000-sumAtEdit`、全 Scratch 変数（日本語・絵文字引数、後続 suffix 等）を Interpreter と比較する。実行ブロック数ONでは **57,000,043**、加算総量 **20,400,000** が一致する。

追加の小数テスト：中間 `times - 15.5`（1.5→2回）、最深部 `times + 0.5`（17.5→18回）。両カウンターが正規化されて復帰できることを計数ON/OFFで比較する。

4階層目を追加した条件は、実際の Interpreter と同じ最終状態になる一方、LLVM OSR を**一度も起動しない**ことを確認する。

新規ヘッドレス CTest は以下の4件。

| CTest | 結果 |
|---|---|
| `xyo-nested-warp-triple-yield-deopt` | PASS（29.34秒） |
| `xyo-nested-warp-triple-yield-deopt-fast` | PASS（28.80秒） |
| `xyo-nested-warp-triple-yield-fractional` | PASS（33.42秒） |
| `xyo-nested-warp-triple-yield-fractional-fast` | PASS（31.96秒） |

```bash
ctest --test-dir board/.osr-silent-edit-headless \
  -R '^xyo-nested-warp-triple-yield-(deopt|fractional)(-fast)?$' \
  --output-on-failure
```

## 整合ビルド

`tools/consistent-build.ts` が再コンパイルとソースダイジェストの固定を確認した実行ファイル：

- `board/osr-triple-yield-verified-smoke`
- バイナリ SHA256: `b4456adacebc8fdf62f406345855aa6034e30dd446e5653a3944128fbbe50777`
- ソース SHA256: `56255883f466c9980554dc490eda1c969a76325d2b1ae41eac45e59c76e2a809`
- `board/osr-triple-yield-verified-smoke.manifest.json`

ビルド時、並行開発中の SDL/DLSS ヘッダーが `DlssOpaqueCoverage` を直接参照するため、**ヘッドレスの `build.ninja` の該当ターゲットのみ**にソースを変更しない `-include src/render/dlss_opaque_coverage.hpp` を一時設定。SDL描画の製品ソースは変更していない。CMake再構成でこのローカル設定は上書きされる点に注意。

## 制限

- 任意の命令途中の SSA 値の再構成や effect-PC デオプトではない。OSR-013 は3階層の確定した内側本体境界だけ。
- 現在の動的式 AST は限定的。実際に巨大な数値や無限大が入力される場合、checkpoint が証明不能なら fail-closed。
- SDL GUIの 2123/2123 Parity は今回未検証。ヘッドレスの OSR 旧回帰については、同じ `libxyo.a` に再リンクして検証する。

コミット・プッシュなし。IR-001/RT-001 が所有する共通フレーム保存 ABI は変更せず、OSR専用の局所的な実験フラグに限定する。
