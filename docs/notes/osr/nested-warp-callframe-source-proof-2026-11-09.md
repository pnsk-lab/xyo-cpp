# OSR-006：Interpreter warp 中断点と共通 CallFrame ソース位置の照合

2026-11-09

## スコープ

IR-001 は LLVM Fiber の呼出し状態を `SourceFrameSymbols`、`SourceReturnSite`、`SourceLoopResume`、`CallFrameInfo` に記録する。一方、OSR-005 は元の Interpreter の StackFrame を生かしておき、最も内側の counted `repeat` の未実行部分だけ LLVM へ移行する。

今回の `XYO_JIT_OSR_NESTED_SOURCE_PROOF=1` は、**既存のネスト warp OSR が採用する直前に、Interpreter の実中断点が IR-001 の不変なソース PC 辞書と完全に対応するか検証する**オプションである。通常は OFF。

これは同じ共通形式への**読取専用の射影**であり、ネイティブの機械 PC や SSA の任意途中位置を再構築する機能ではない。任意の非 warp yield、再帰の完全デオプト、汎用 LLVM→Interpreter 復帰をこのフラグで有効化したわけではない。

## 変更内容

- `src/jit/backend_llvm.cpp`：非同期 LLVM の準備中、同じ Blocks 改訂番号から `ScratchCompilationUnit` を捕捉する。IR-001 の既存 `NativeProcedureFrameSession` を使用して不変な `SourceFrameSymbols` を構築し、`CompiledScript` に共有所有で保持。準備に失敗した場合は proof 必須である以上、native OSR 採用を拒否する。
- `src/jit/jit.hpp`：`nestedOsrSourceProofRequired` と `nestedOsrSourceSymbols` を追加。共通 CallFrame の ABI レイアウトには変更を加えない。
- `src/jit/nested_warp_callframe_projection.hpp`（新規）：`InterpreterOsrSnapshot` のすべての実行中フレームを元のソース PC に変換する。PC→ブロック名は IR-001 が凍結した名前表と照合する。各呼出しは `SourceReturnSite` の return PC、呼出し元手続き ID、引数数、`callerLoopPath`、`branchPath` を比較し、`validReturnTarget` を通す。外側 counted repeat の**実カウンター**は `Value::num(...).bits` の `SourceLoopResume` 形式で保存し、所有手続きと入れ子 loop PC を比較する。末端の repeat も同様に検証する。結果は `NestedWarpSourceWitness` の数値ソース情報で、実行権限は持たない。
- `src/engine/runtime.cpp`：OSR 採用直前に `projectNestedWarpSource` を呼び出す。さらに source revision だけでは見落とす **Block::resetCache を通さない直接編集**に備え、`ScratchCompilationUnit` のソース内容識別子を採用時に再計算し、コンパイル元の `ScratchSourceDigest` と照合する。どれか一つでも不一致なら native OSR を採用しない。

すべてオプトイン。通常経路のループ本体・引数読出し・GC・スケジューラ・共通 CallFrame セッションの ABI は変更しない。内容識別子の再計算はソースグラフ全体を走査するため、proof 有効時だけの追加コストを持つ。

## 設定

```bash
XYO_JIT_MODE=hotspot \
XYO_JIT_ASYNC=1 \
XYO_JIT_OSR=1 \
XYO_JIT_OSR_NESTED=1 \
XYO_JIT_OSR_NESTED_DEEP=1 \
XYO_JIT_OSR_NESTED_PARENT_REPEAT=1 \
XYO_JIT_OSR_NESTED_BRANCH=1 \
XYO_JIT_OSR_NESTED_ARG_READS=1 \
XYO_JIT_OSR_NESTED_DIRECT_ARGS=1 \
XYO_JIT_OSR_NESTED_HOIST_ARGS=1 \
XYO_JIT_OSR_NESTED_SOURCE_PROOF=1 \
XYO_JIT_OSR_NESTED_TRACE=1 \
  ./board/osr-callframe-proof-v2-xyo play project.sb3
```

採用が成功すると、`[jit-osr-source-proof] calls=N parent-loops=M branches=K leaf-pc=X revision=R` と記録する。ログがあるだけでなく `[jit-osr-nested] promoted` と `restored caller` が対応することをテストで確認する。

## テスト

新規 `tests/differential/jit_nested_warp_source_proof.py`、CTest `xyo-nested-warp-source-proof`。すべて元の Interpreter の最終変数全体と比較し、proof と実際の native promotion が一致することを要求する。

- ルートの動的外側 `repeat(3)`、TIMES 変数を途中で17に変更：同じ root counted loop の PC と値を検証する。
- 二重 root counted `repeat(2) × repeat(2)`：入れ子 `SourceLoopResume` 2件を照合する。
- warp 手続き内の外側 repeat：実手続き owner の相違を検証する。
- root `repeat` 下の IF、then/else × then/else：arm と returnPc を検証する。
- 最大8個の warp 呼出し：実ネイティブ移行時に8件の呼出し・return PC を検証する。
- DEEP 無効の従来2フレーム root→warp：旧経路の互換性を検証する。
- 日本語・絵文字引数を含む強制GC：rooted Interpreter 引数に干渉しないことを確認する。

独立整合ビルド `board/osr-callframe-proof-v2-xyo` の SHA256 は
`cdabde4b691fe6d52df8bd6f6c2e2510934f7827f730cdecd47f347957ccc965`、
ソースダイジェストは
`3d3cb940635a6d1caf1dc7c3ba064eb368a167e02dbe482792776ca98f136e4d`。

## 残る統合

この witness は新しい LLVM CallFrame 実行フレームを作らず、外側カウンターの所有権も移さない。IR-001 / RT-001 の本来の作業である、一般的な native SSA/effect 状態の捕捉、ネイティブ yield 時の元ソース PC への実行可能な移行、再帰に跨がる任意復帰は未実装。今回の照合は、そのために必要な **同じ source-PC 証明規則**を、限定 OSR の実採用時に用いた段階である。

共有の作業ツリーを保持するため、コミット・プッシュは行っていない。

## 検証実績（実行済み）

- 新規 CTest #59 `xyo-nested-warp-source-proof` **1/1 PASS、63.10秒**。実際の `promoted`、`restored caller`、`[jit-osr-source-proof]` が1対1で対応。
- root動的TIMES親ループ：canonical calls=1 / parent-loops=1 / branch=0 が3回。
- root二重ループ：parent-loops=2 が4回。warp内親ループ：calls=2 / parent-loops=1 が3回。
- root repeat+IF：calls=1/parent-loops=1/branches=1 が3回。IF-ELSE×IF-ELSE：calls=2/branches=2 が1回。
- 8段 warp 呼出し：calls=8 が1回。legacy direct warp、UTF16+GC を含めて CTest 全件成功。
- ソース証明フラグを有効にした GUI parity：off / hotspot / eager 各 **2,123 / 2,123**。

数値のソースフレームは読取専用であり、SSA・effect の途中値が採用可能になったわけではない。コンテンツ digest 照合の実装は存在するが、同一 revision の実行中 Block 直接改竄を注入する個別動作試験はこのテストには含まれない。IR-001 の原本改竄に対する既存の C++ 証明検査を併用する必要がある。

## 旧経路の回帰（同一整合バイナリ）

- 親 `repeat`：動的TIMES、2重ループ、warp-owned、IF、全ブロック数120000043、GC1033、PASS。
- IF/ELSE：4組・単一arm・拒否ケース、全ブロック数40000030、GC341、PASS。
- 最大8呼出し：深さ3/5/7/8・拒否ケース、全ブロック数40000058、GC343、PASS。
- 新規CTest #59 **1/1 PASS、63.10秒**。GUI parity off/hotspot/eager は各 **2123/2123**。

同一 revision の実行途中 Block 直接改竄を発生させる個別負例は未実施。ソース内容ダイジェストの再検証は実装済みだが、この特殊な動作の専用試験は次段階で追加する。
