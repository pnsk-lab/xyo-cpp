# RT-001：二段以上のLLVMポインタロード経路監査（2026-11-10）

## 目的

前回の`rt-native-unknown-pointer-load-audit-2026-11-10.md`では、2本のネイティブFiberを使用する実Scratch `ssa_budget_cross_fiber_two_hats`について、各LLVMチェックポイントの未証明ポインタ6件・7件がすべて`load ptr`の結果であることが分かった。ただしロード時に使われる「アドレス自体」がロードされた値だったため、単発の`PointerRegion`分類では6件が`load_unknown`としか分からなかった。

今回は`load ptr, ptr p`の`p`をさらに遡り、GEP/BitCast/ConstantExprを経由しつつ多段ロードとその起点を特定する。**ロード先の割当領域のGCルート所有権を証明するものではない。**

## 実装

- `src/jit/rt_ssa_extra_capture.hpp`に`PointerLoadChain`・`PointerLoadTerminal`・`tracePointerLoadChain()`を追加。
  - 後方に最大64ノードを走査し、`LoadInst`を検出するたびに参照段数`dereferences`を加算する。
  - `GetElementPtrInst`・`BitCastInst`・GEP/BitCast `ConstantExpr`は「アドレス派生演算」として回数`addressDerivations`を記録。
  - 起点は`AllocaInst`、明示した`JitCtx*` ABI引数、`GlobalVariable`、未証明引数、コール戻り値、整数/アドレス空間変換、PHI/SELECT、循環、深さ上限、その他に分類。
  - PHI/SELECTは全arm一致でもロード経路については**混合経路とみなして停止**。ロード結果は一律GCルートの証明なし、深さ上限でもfail-closed。
  - 各UnknownがLoad系の原因の場合、元のLLVMコンパイラ命令ID・`dereferences`・`addressDerivations`・`terminal`を診断専用の`unknownLoadChains`へ保存する。原因別Load数と診断数が一致しなければ解析報告`valid=false`。
- `src/jit/codegen.cpp`に`[rt001-ssa-pointer-load-chain]`ログを追加。**LLVM SSA→RuntimeのキャプチャABI、CallFrame、Gc、FiberPoolおよびGC回収条件は一切変更しない。** LLVMアドレスの実際の値も出力しない。
- `tests/platform/rt_pointer_region_smoke.cpp`に、実LLVM23 IRBuilderで2段ロード・3段ロード・スタック/Host/Static/Foreign/Call/Integer起点・混合PHI・循環PHI・80段の深さ上限・全Unknown Load数照合を追加。

## 実Scratchで確認できた経路

LLVM23の隔離ネイティブCLI `/home/kicky/.rt001-load-chain-isolated/xyo`で緑の旗2つから同じ非warp手続き（Unicode文字列結合、各24,000回）を実行。両方のLLVMスクリプトに同じ2種類のsource checkpointがある。

| チェックポイント | LLVM命令ID | 参照段数 | アドレス派生操作数 | 最初のアドレスの出自 |
|---|---|---:|---:|---|
| #1 | 3, 5, 7 | 2 | 各2 | JitCtx |
| #1 | 281, 284, 287 | 2 | 各3 | JitCtx |
| #2 | 5, 7, 9 | 2 | 各2 | JitCtx |
| #2 | 273 | 1 | 1 | JitCtx |
| #2 | 521, 524, 527 | 2 | 各3 | JitCtx |

重要：既存の`load_unknown`6件は、**2段以上遡ったアドレスの起点をJitCtxまで追跡できた**。例えば概念的には`load ptr, ptr(gep(load ptr, ptr(gep(JitCtx*, ...)), ...))`のように、間に1回ポインタを読み出してから次のアドレスを作っている。**ロードされた中間ポインタは依然としてUnknownであり、GCヒープを指す可能性は排除できない**。値の生存SSAを根付きとみなすのは誤り。

実際の2-Fiber GC安全性の挙動も以前どおりで、最初のFiberのInterpreter復帰では`collected=0,pending=1,active_fibers=1`、最後のFiberの復帰では`collected=1,pending=0,active_fibers=0`。最終変数はInterpreterと一致（`y=48005`）。

## 検証と制限

- LLVM23単体テスト直接ビルド・実行：`PASS RT-001 pointer region: stack=3 host=4 static=1 unknown=14 total=22; loaded/mixed/invalid/cycle/deep refused`。
- 実Scratch回帰は`tests/differential/native_simple_resume.py`の既存Pointer Region監査ケースを拡張。原因別のUnknown Load ID集合と新しいload-chain IDの多重集合が一致すること、ロード段数と起点が期待値どおりであること、全件`gc_safe=0`でGCが生存Fiberを回収しないことを検証する。追加の差分実行数は変わらず951件。
- 新しいバイナリは直前の検証済みLLVM23静的ライブラリの隔離コピーへ、現在の`src/jit/codegen.cpp`のみ再コンパイルして再リンクしたもの。現在の共有ツリー全体の完全な再ビルドではない。
- 隔離ビルド実行中に、別担当の未コミット`src/ir/llvm_source_reporter_trace.hpp`に一時的な型不一致があったが、後に同ファイルが更新され`SourceResumePhase`を`uint64_t`へ明示変換した。RT-001では**同ファイルを変更していない**。
- `JitCtx`を起点とした参照連鎖が分かっても、ロード結果の割当元・寿命・GC rootの所有権・LLVM最適化後レジスタlivenessは未証明。保守的なネイティブFiber GCバリアを維持。
- CMake全体の再構成・全CTest、ASanビルド、並列worker停止契約は未検証。コミット・pushは行わず、他担当の編集を保全。

## 最終検証（2026-11-10）

- ネイティブ差分テスト: **951/951 PASS**。
  - コマンド: `python3 tests/differential/native_simple_resume.py --native /home/kicky/.rt001-load-chain-isolated/xyo --out build/rt001-load-chain-final`
  - ログ: `build/rt001-load-chain-final.log`
- 2つの実ネイティブFiberを同時に生存させるケースでも、2つのsource checkpoint×2スクリプトの未知ロードID・段数・起点と全GC回収保留の条件を確認。
- LLVM23 IRBuilder単体テスト: `build/rt001-pointer-region/rt-pointer-region-smoke` PASS。2/3段のポインタロード、ロード元Host/Stack/Static/Foreign/Call/IntCast、PHI、循環、80段の深さ制限を検査。結果: 22生存ポインタうち14件Unknown。
- `python3 -m py_compile tests/differential/native_simple_resume.py` PASS。
- `git diff --check`対象実装・テストPASS。
- 隔離ネイティブCLI: `/home/kicky/.rt001-load-chain-isolated/xyo`、SHA256 `dd2ec5bfd9227cf55165a20cd956ffb122624ae8d70b512eefdc934381918ff0`。
- 実際のGCを許可する条件は変更なし。`gc_safe=0`を全ロード経路について維持。
