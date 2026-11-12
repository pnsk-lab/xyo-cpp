# RT-001：LLVM O2/O3最適化後の生存SSA・GCルート監査（2026-11-10）

## 結論

実際のネイティブLLVM JITを用い、O3最適化が終了した後のLLVM IRでSSAキャプチャ呼出し xyo_rt_capture_llvm_ssa の生存ポインタを計数する監査を、Eagerおよび遅延ORC JITの双方に実装した。

**この監査は「GCしても安全」の証明を与えない。** 最適化後LLVM IRのポインタ生存性は機械レジスタ割付後の正確なGCルート位置と異なる。また従来のSSAキャプチャは一部の数値をi64のdescriptorへ保存するが、ポインタ型SSAの生存値は一律root化されていない。従って実験用の生存Fiber GC禁止を維持する。

使用する診断フラグ：

```bash
XYO_RT_POSTOPT_SSA_AUDIT=1
```

本フラグは既定OFF。診断専用で、サスペンド・GC許可・ソース復帰・生成ABIの挙動は一切変更しない。

## 変更

- 新規 `src/ir/rt_postopt_ssa_audit.hpp`：
  - LLVM Moduleから正確な `xyo_rt_capture_llvm_ssa` の CallBase を選択する。
  - 17引数のhelper署名、呼出し側の第1 JitCtx* とhelper第1引数の同一性、site ID (i64)、元の未解決ポインタ数 (i32)、descriptor数 (i32)を検証。不正または非定数の情報は `signatureValid=false` → `reportValid=false`とする。
  - 元の `analyzeExtraLiveSsa` / `analyzeLlvmCheckpointLiveness` を**最適化後のIR**のhelper call地点へ再実行。スタック/HostContext/StaticGlobal/Unknownのポインタ候補、数値SSA、その他の件数を返す。
  - 返却値に `gcSafe` を設けず、診断結果がすべてvalidでもGC許可の状態遷移は起こらない。生ポインタ値、ヒープアドレス、実際のレジスタ値は出力・収集しない。
- `src/jit/backend_llvm.cpp`：
  - `auditRt001PostoptSsa()` を追加。
  - 通常Eagerの `mpm.run(*mod,mam)` 完了直後と、遅延ORCのIR transform layerの `mpm.run(module,mam)` 完了直後へ同じ診断を接続。
  - `[rt001-postopt-ssa]` で各地点の pre_unresolved、captured、post_live_ptr、post_unknown、post_stack、post_host、post_static、post_numeric、post_other、valid、**gc_safe=0**を出力。
  - `[rt001-postopt-ssa-total]` で各最適化単位の件数と **machine_roots_proven=0, gc_safe=0** を明示。
- 新規 `tests/platform/rt_postopt_ssa_audit_smoke.cpp`：
  - LLVM23 IRBuilderで生のポインタを取得し、checkpoint helperの後で使用する関数を2つ生成。
  - 同じmoduleを実際にO3で最適化してから監査。正常な17引数の呼出しは生存Unknownポインタありと診断し、不正な第1引数である「偽のJitCtx」は `reportValid=false` で拒否。
  - 直接LLVM23ビルド・実行 PASS。
- 新規 `tests/differential/rt_postopt_ssa_audit.py`：
  - `ssa_budget_cross_fiber_two_hats` 実ScratchでEagerと遅延JITの独立プロセスをそれぞれ実行。
  - 2本の緑の旗スクリプトでそれぞれ2 site ID、計4のcaputure地点を検証。全て post_unknown>=1、各地域件数の合計=post_live_ptr、全て gc_safe=0、合計ログは machine_roots_proven=0。最終Scratch状態はInterpreterと同一（y=48005）。
  - Eager PASS、遅延JIT PASS。
- `tests/CMakeLists.txt`：LLVM23単体テストとEager+遅延JIT差分テストをそれぞれCTestへ登録。共有ツリーのCMake全体の再生成と全CTestは未実施。

## 実際の生存SSA件数

検証用の2本のスクリプトは同じ非warp処理に対して次の結果を出した。各スクリプトについて同一の結果が観測され、Eagerと遅延JITの双方で一致した。

| checkpoint | pre unresolved | captured words | post live pointer candidates | post unknown | post stack | post JitCtx | post static | post numeric |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 21 | 24 | 113 | 3 | 104 | 5 | 1 | 39 |
| 2 | 25 | 31 | 114 | 4 | 104 | 5 | 1 | 57 |

注意：この2時点は**同一の命令集合ではない**。最適化前のカウントは元のsource checkpoint付近で得たもので、その後に人工的なSSA descriptor、volatile store/load、allocaなどを挿入してからO3最適化している。最適化後の監査は合成された `xyo_rt_capture_llvm_ssa` の直前で行う。したがって21→113の差をLLVM最適化の安全性悪化と解釈することも、Unknown6→3をGCルートの安全性改善と解釈することもできない。すべて保守的な候補数であり、**確実な生存値の必要十分条件を提供しない**。

## ORC機械語オブジェクトの確認

上記の2本のネイティブスクリプトを実際に `XYO_JIT_DUMP_OBJECT=/home/kicky/.rt001-postopt-ssa-isolated/machine-objects/obj` でJIT実行し、ORCが作成したELFオブジェクトファイルをreadelf -SWで調べた。

- `obj-0.o`：9272バイト
- `obj-1.o`：9160バイト
- 両方で **.llvm_stackmaps セクションなし**

これはその2オブジェクトから正確なLLVM stackmap root情報を抽出できないことを意味する。しかし`readelf`の結果だけでコンパイラ/ランタイムのあらゆるルート機構の不存在を証明するものではない。現在のランタイムはFiberのスタック・退避済みレジスタの保守的scanも持つが、これだけではレジスタSSAの全値が保持される保証がない。

GC禁止を安全に取り除くには、LLVM機械コードと中断点を同期させた明示的なstackmap/GC statepoint、または機械レベルで保持位置が検証されたシャドースタックが別途必要。

## 実行・検証

- 隔離CLI：`/home/kicky/.rt001-postopt-ssa-isolated/xyo`。以前の検証済みLLVM23アーカイブをコピーし、現在の `backend_llvm.cpp` を再コンパイル・差し替え・リンクしたバイナリ（共有未コミットツリー全体のクリーンビルドではない）。
- 直接単体テスト：`build/rt001-pointer-region/rt-postopt-ssa-audit-smoke` PASS。
- `python3 tests/differential/rt_postopt_ssa_audit.py --native /home/kicky/.rt001-postopt-ssa-isolated/xyo --out build/rt001-postopt-ssa-regression` Eager PASS / Lazy PASS。
- 全ネイティブ差分回帰ログ：`build/rt001-postopt-ssa-full.log`、**955/955 PASS**。
- 実験フラグなしでは追加のLLVMモジュール監査は実行しない。
- GCのsweepバリア／ソース復帰／ランタイムのLLVMSSA型付きdescriptor仕様は変更なし。コミット・pushなし。他エージェントの共有ツリー編集を保存。

## 次の実装候補

1. LLVM stackmapの明示的なpatchpoint、またはGC statepointが保証するrooted boxed Valueハンドルへの書き出しを実験モードで試す。
2. キャプチャ地点とスケジューラのFiber中断点を1対1に対応させ、各機械レジスタ/stack slotの位置をGCに知らせる。
3. 実GCを走らせる前に、全Fiberの機械level safepoint到達・登録ルートの完全性・ルートの生存期間を検証する。どれかが不明なら引き続きGCを延期する。


## 最終検証

- 隔離CLI SHA256：`6226f5b51add9ef01032248203b012fedcf1376868c727eda3e99b3573d6e602`。
- O3純粋LLVM IRBuilderテスト：PASS。不正なキャプチャ関数の第1引数を拒否。
- 実2-Fiber Scratch × Eager / Lazy 両方の最適化後監査：**2/2 PASS**、それぞれ4地点、GC許可なし、Interpreter同一。
- 従来のすべてのネイティブ差分・失敗クローズ回帰：**955/955 PASS**。
- `python3 -m py_compile` と `git diff --check`：PASS。
- セクション確認：実ORC ELF 2個とも`.llvm_stackmaps`がない（readelf -SW）。
- 共有ソース全体のクリーン再ビルドおよびCTest全実行は未実施。新規2件のCTest登録は済み。
