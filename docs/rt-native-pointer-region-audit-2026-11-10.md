# RT-001: LLVM生存ポインタの出自分類（2026-11-10）

## 対象

RT-001の実験用GCソフト予算（`XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1`、`XYO_RT_GC_SOFT_BUDGET_MB=1`）では、ネイティブFiberを継続可能なままGCすると、まだルート公開されていないLLVM SSAポインタの指すGCオブジェクトを回収する恐れがある。このため以前の変更では開始済みのネイティブFiberが1本でも残る間、実際のGCを保留した。

今回の目的は、従来ひとまとめにしていたLLVM生存ポインタの出自を区分し、**将来どの証明が追加で必要かを可視化すること**。危険なGC許可の緩和はしない。

## 実装

`src/jit/rt_ssa_extra_capture.hpp` に `PointerRegion` と `classifyPointerRegion` を追加。LLVMのチェックポイントにおける生存ポインタを次の4種類に分類する。

- `NativeStack`: 実際のネイティブ関数内の**同一のAllocaInst**に到達することを、既存の`nativeStackPointerRoot()`で証明済み。PHI/SELECTでスタック領域が混在した場合も安全な復帰ポインタとはみなさない。
- `HostContext`: 明示的に指定された最初のABI引数 `JitCtx*`、そのGEP/BitCast派生、同種の全armが一致するPHI/SELECT。
- `StaticGlobal`: LLVMのGlobalVariableまたは静的なGEP/BitCast ConstantExpr派生、同種の全armが一致するPHI/SELECT。
- `Unknown`: ロード結果、関数呼び出し結果、`IntToPtr`、`AddrSpaceCast`、未証明引数、混合PHI/SELECT、循環したPHI、深すぎるポインタ演算、その他。

`analyzeExtraLiveSsa` にコンパイル時診断用 `pointerRegions` 配列と、任意の`trustedContext`（該当LLVM関数の引数0、ポインタ型であることを独立に検証）を追加。全4分類の合計が従来の`unresolvedPointer`と一致し、`NativeStack`が従来の`stackDerivedPointers`と完全一致しなければ`valid=false`。**LLVM→Runtimeの17引数のcapture ABIと`NativeRtSsaCapture`の構造を変えず**、診断情報だけを追加した。

`src/jit/codegen.cpp` から引数0を明示して解析し、`[rt001-ssa-pointer-regions] ... gc_safe=0` を表示する。解析は既存のLLVM SSA spill前のIRに対して実行し、誤認を避けるため結果は新しいroot handleでもGC許可でも復帰認可でもない。

重要: NativeStackが参照するスタック内や、HostContextの各フィールドに**GC管理ポインタが格納されている可能性は残る**。ポインタ値自体が非GC領域を指していることと、そこから到達可能なGC値すべてのrootが保持されたことは別問題。

## 実測（本物のScratch/JIT）

同一の2-green-flag hatプロジェクト `ssa_budget_cross_fiber_two_hats` で、各Script内に2種類のLLVM source checkpointを確認した。各Scriptで同じ分類が繰り返された。

| 生存ポインタ | NativeStack | HostContext | StaticGlobal | Unknown | 合計 |
|---|---:|---:|---:|---:|---:|
| checkpoint 1 | 14 | 1 | 0 | 6 | 21 |
| checkpoint 2 | 16 | 2 | 0 | 7 | 25 |

元のGC安全性挙動は維持: 最初のLLVM→Interpreter移行時は他方のFiberが生存しているためGCを保留（`collected=0 pending=1 active_fibers=1`）、2本目のFiber解放後に回収（`collected=1 pending=0 active_fibers=0`）。実行結果の `y=48005` はInterpreterと一致。

## 単体テスト

新規 `tests/platform/rt_pointer_region_smoke.cpp` を作成。LLVM23の実IRBuilderから、JitCtx引数、異種未証明引数、stack alloca、静的global、静的GEP ConstantExpr、host/stack GEP、ロードしたポインタ、IntToPtr、PHI/SELECT、自己循環、深さ80のGEPを作って解析する。正しい分類だけを承認し、ロード/混合/循環/深さ超過/不正なtrustedContextを拒否する。実LLVM23の直接ビルド+実行では `PASS ... stack=3 host=4 static=1 unknown=6 total=14`。

`tests/CMakeLists.txt` に `xyo-rt001-pointer-region-smoke` を追加。CTestからも実行可能な定義だが、新CMake構成全体はまだ未実行。

`tests/differential/native_simple_resume.py` に、実Scratchの2-Fiber結果とLLVM元の生存ポインタ総数・スタック由来数・分類合計が一致すること、未証明参照が残ること、`gc_safe=0`とGC保留を維持することを確認する差分実行を追加。元の保護ロジックは変更しない。

## 残課題

- 生存SSAの完全な型・ルート所有権の証明ではない。特にUnknown6/7件の元になるポインタロードと派生GEP、GC指向のポインタ、外部コールの戻り値を追跡する必要がある。
- `JitCtx`やネイティブスタック自体が非GC領域であるという事実から、それらの*内容*のルート到達性まで演繹してはいけない。
- LLVM最適化後の実機レジスタの生存状態、ポインタ・alias/FFI経路、GCとのstop-the-world、Source効果・復帰証明は別途必要。コンパイル前IRの診断はGC許可の根拠にならない。
- モード既定OFF、コミット・pushなし。他担当による未コミット変更は保持。

## 検証環境

既存の確認済みLLVM23アーカイブの**隔離コピー**へ、現在の`codegen.cpp`を再コンパイルして同名オブジェクトを置換し、CLIを再リンク。前回の`gc.cpp`、`fiber.cpp`、`sequencer.cpp`変更はコピー元に含まれる。現在の共有ツリー全体を再ビルドしたものではない。

- バイナリ `/home/kicky/.rt001-pointer-region-isolated/xyo`。
- ベンチ兼回帰ログ `build/rt001-pointer-region-full.log`。

## 検証確定結果

- isolated native CLI SHA256: `a6a81adb3587d3ded14d81b82393f4e814e8714669c5dea65e5557e0215a61de`
- `python3 tests/differential/native_simple_resume.py --native /home/kicky/.rt001-pointer-region-isolated/xyo --out build/rt001-pointer-region-full`: **951 / 951 PASS**
- ログ: `build/rt001-pointer-region-full.log`
- LLVM 23 直接ビルドした `build/rt001-pointer-region/rt-pointer-region-smoke`: PASS（NativeStack 3件、HostContext 4件、StaticGlobal 1件、Unknown 6件）
- `python3 -m py_compile tests/differential/native_simple_resume.py`: PASS
- `git diff --check`（対象実装・テスト）: PASS
- `tests/CMakeLists.txt` にはCTEST登録済み。ただし共有ビルドのCMake再生成およびCTest全体は未実施。
- 不変の安全境界: `Gc::collect()` は引き続き実際に中断中のNative Fiberがある間、実験モードでは`FiberPool::unsafeCount()`で回収を止める。新診断はこの条件に一切接続していない。
