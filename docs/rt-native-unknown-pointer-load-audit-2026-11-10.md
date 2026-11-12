# RT-001：LLVM未証明ポインタのロード経路診断（2026-11-10）

## 目的と安全境界

以前の `PointerRegion` 分類では、2本の緑の旗スクリプトのネイティブFiberが同時に動く場合、LLVM生存ポインタ21/25件のうち6/7件が`Unknown`であると分かった。今回は **Unknownになった命令が何を行い、ロードであればどの領域のアドレスから値を取得したか**を静的に分類した。

これは安全なGCルートを証明するものではない。特に
`load ptr, ptr <JitCtx由来のアドレス>`の**ロード先アドレス**がHostContextでも、返される`ptr`はGC管理オブジェクトを指す可能性がある。`LoadStack`と`LoadStatic`も同様。どれも引き続き`PointerRegion::Unknown`であり、現在の`FiberPool::unsafeCount()>0`に基づくGC延期を解除しない。

## 実装

`src/jit/rt_ssa_extra_capture.hpp`:
- `UnknownPointerCause`を追加。ロード元の`PointerRegion`が`NativeStack`/`HostContext`/`StaticGlobal`/`Unknown`の4種類、`CallReturn`、`IntOrAddressCast`、`ForeignArgument`、`PhiSelect`、`Other`の合計9種類。GEP/BitCastで派生したUnknownも最大深さ64まで根をたどり、循環を保守的に`Other`として扱う。PHI/SELECTは同一分類に見えても保守的に別分類してルート証明と誤認しない。
- `ExtraLiveSsaReport`にコンパイル専用`unknownPointerCauses`と`unknownPointerDetails`（元LLVM命令ID、原因enumのみ）を追加。総数が`pointerRegions[Unknown]`と一致しなければ`valid=false`。ネイティブSSAキャプチャABIや`Gc`/CallFrameオブジェクトは不変。実行時のポインタビットやアドレスは収集・保存・表示しない。

`src/jit/codegen.cpp`:
- 元IRのlivenessを取得する地点で`[rt001-ssa-pointer-unknown]`の原因別カウントと`gc_safe=0`を表示。
- `[rt001-ssa-pointer-unknown-id]`でコンパイラ命令IDと原因のみを出力する。GC回収許可・復帰可否・生存SSA root handleに接続していない。

`tests/platform/rt_pointer_region_smoke.cpp`:
- 実LLVM23 IRBuilderから、スタック/HostContext/静的global/未証明アドレスからのロード、ロード結果の派生GEP、関数戻り値、非定数IntToPtr、未証明引数、混合PHI/SELECTを作り、原因を個別に照合。全Unknown原因件数と元のUnknown領域数の整合も検査。さらにロード元領域がどれであっても戻り値を`PointerRegion::Unknown`と判定することを確認。
- 実行結果：`PASS RT-001 pointer region: stack=3 host=4 static=1 unknown=12 total=20; loaded/mixed/invalid/cycle/deep refused`。

`tests/differential/native_simple_resume.py`:
- 既存の実Scratch `ssa_budget_cross_fiber_two_hats` のコンパイラ元SSAポインタ数、4領域の総数、新しい9原因の総数、命令ID別原因の集計を照合し、GC保留のfail-closedを継続検証する。追加の単体試行ではなく既存の「コンパイラ起源監査」実行を強化。

## 実Scratchの発見

`ssa_budget_cross_fiber_two_hats` では各スクリプトが同じ2種類のLLVMチェックポイントを持ち、次の結果が得られた。

| チェックポイント | 全生存ポインタ | 非証明pointer値 | 未証明アドレスからのロード | HostContextアドレスからのロード |
| --- | ---: | ---: | ---: | ---: |
| #1 | 21 | 6 | 6 | 0 |
| #2 | 25 | 7 | 6 | 1 |

それぞれ`[rt001-ssa-pointer-regions]`ではNativeStack=14/16、HostContext=1/2、StaticGlobal=0、Unknown=6/7である。

第1チェックポイントのコンパイラ命令ID：3, 5, 7, 281, 284, 287（6件とも`load_unknown`）。
第2チェックポイントのID：5, 7, 9, 273, 521, 524, 527。273のみ`load_host`、それ以外は`load_unknown`。

両スクリプトで同じ形が出現し、実行結果`y=48005`はInterpreterと一致。最初のネイティブFiber終了時には`collected=0,pending=1,active_fibers=1`で、最後のFiber終了時にのみGCを実行した。

## 制限

- `load_unknown`のロード元アドレスがロード・PHI・GEPのどれに由来するか、そのさらに上流の保守的追跡は未完了。
- LLVMコンパイラ最適化**前**のSSA診断であり、レジスタ割当・最適化後live状態・GC Rootsの完全性を証明しない。
- GCを中断中Fiberに許可するにはロード結果の元となるメモリ内容・alias・GC管理ポインタの所有権およびstop-the-worldを含む別の証明が必要。
- RT-001の実験フラグは既定OFFで変更なし。コミット・pushなし。他の共有作業ツリー変更を保存。

## 検証環境

検証用CLI：`/home/kicky/.rt001-unknown-pointer-isolated/xyo`。既存検証済みLLVM23静的ライブラリの隔離コピーに、現在の`codegen.cpp`を再コンパイルして同名オブジェクトを置換・リンク。共有ツリー全体の完全再ビルドではない。

- SHA256 `caa69f51decfcf318ab63b9323f861ea3be5ccd2b3cce3ef1912e0fabe018b1e`
- フル回帰：`build/rt001-unknown-pointer-full.log`（**951/951 PASS**）

## 最終検証

- `python3 tests/differential/native_simple_resume.py --native /home/kicky/.rt001-unknown-pointer-isolated/xyo --out build/rt001-unknown-pointer-full`：**951/951 PASS**
- 新しい原因別集計では、全Unknownポインタ件数と命令IDごとの9分類の集計結果が厳密に一致することを確認。前回実装したLLVMポインタ領域分類の集計も維持。
- `build/rt001-pointer-region/rt-pointer-region-smoke`：**PASS**。LLVM23の実IRでロード4種類、混合、関数戻り値、整数変換、外部引数の原因分類とサイクル/深さ制限を確認（20ポインタ、12件Unknown）。
- `python3 -m py_compile tests/differential/native_simple_resume.py`：PASS。
- `git diff --check`（対象の新規診断コード・回帰テスト）：PASS。
- `tests/CMakeLists.txt` のCTest登録は前回から継続。今回はLLVM23の直接ビルド・直接実行と全ネイティブ差分テストを実施し、共有CMake全体の再構成・CTest全体は未実施。
