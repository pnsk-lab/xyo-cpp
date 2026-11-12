# RT-001：LLVM機械語StackMapの生成・解析実験（2026-11-10）

## 目的

前回の最適化後SSA監査で、ネイティブJITの実行中にはSource Checkpointに残るLLVM pointer SSAが存在し、従来のORC ELFオブジェクトには `.llvm_stackmaps` セクションが無いことを確認した。

今回はLLVM23の `llvm.experimental.stackmap` を **O2/O3の完了後**に生成し、ネイティブ機械語へ低下した際のレジスタ／スタックスロット位置情報が実際に出力されるかを実証する。

```sh
XYO_RT_MACHINE_STACKMAP_PROBE=1
```

既定OFF、**診断専用**。このフラグはGCの回収許可、Fiberの中断・復帰、Scratch Valueのルート管理、既存SSAキャプチャABIを一切変更しない。

## 実装

- `src/ir/rt_postopt_ssa_audit.hpp`
  - 既存の最適化後 `auditPostoptSsaCaptures()` に、その時点の `xyo_rt_capture_llvm_ssa` 呼出し、未証明pointerのLLVM Value*候補（診断時のみ借用）を記録する。
  - 候補命令は従来の `ExtraLiveSsaReport::unknownPointerDetails` の**最適化後の命令ID集合**で復元する。引数には上位ビットの別ID空間を使い、普通の命令ではBB/命令を順に数えて厳密に照合する。候補IDの集合が一致しなければ解析失敗とし、StackMapを作らない。
  - `stackmapProbeId()` はLLVM Function名とソースチェックポイントIDからFNV-1aの64ビット識別子を作る。1モジュール内の識別子衝突を検出したら該当地点は挿入拒否。
  - `emitExperimentalPostoptStackmaps()` は、有効な元の17引数キャプチャABI・コンパイラ報告に限り、`llvm.experimental.stackmap(i64 id, i32 shadow_bytes=0, ptr JitCtx*, ptr unknown_0, ...)` を元の `xyo_rt_capture_llvm_ssa` の直後に挿入する。
  - 全Unknownポインタを列挙する。64件より多い場合は切り捨てるのではなく**挿入拒否**する。どのケースでも「完全なGCルート証明」という状態は返さない。
  - これらはポインタ**値をGC管理オブジェクトとして検証する操作ではなく**、その機械語位置を記録するだけ。マシンレジスタの番号やstack offsetは実行環境のCPU/ABIと最適化tierに依存する。

- `src/jit/backend_llvm.cpp`
  - EagerとORC lazyの両方でO2/O3パス完了後、`emitRt001MachineStackmapProbe()` を呼ぶ。
  - `[rt001-machine-stackmap]` はコンパイル関数名、source checkpoint、64bit StackMap ID、Unknown数、追加JitCtx数、挿入可否、gc_safe=0を示す。
  - `[rt001-machine-stackmap-total]` はモジュールごとに挿入数・拒否数と `machine_roots_proven=0 gc_safe=0` を常に記録する。

- `tests/platform/rt_postopt_ssa_audit_smoke.cpp`：LLVM O3の実IRを用い、キャプチャ後にも実際に使うUnknown pointerが含まれた正しいサイトはStackMapを出力し、第一JitCtx*引数を偽装した17引数呼出しは**挿入拒否**されることを実証。挿入後も `llvm::verifyModule()` がPASS。
- 新規 `tests/differential/rt_machine_stackmap_probe.py`：実Scratchの2スクリプト×2地点をEager／Lazyで実行し、生成されたORC ELFを `llvm-readobj --stackmap` でデコード。コンパイラ出力のStackMap IDと実ELF内のIDが全件一致し、各IDのlocation数が`1 + postopt_unknown_pointer_count`（JitCtx1件を含む）に一致することを確認。Scratch結果がInterpreterと一致すること、GC許可=0も検証する。
- `tests/CMakeLists.txt` に `xyo-rt001-machine-stackmap-eager-lazy-smoke` CTestを追加。LLVM readobjとPythonが利用可能な環境でのみテストを登録する。

## 実機結果：x86-64 / LLVM23

`ssa_budget_cross_fiber_two_hats` のScratch JITで、非warpの2スクリプトが同時に生存する環境を検証した。両方とも最終状態はInterpreterと一致（y=48005）。

最適化後O3の各スクリプトには2つのStackMapが出力された。各StackMapに登録されたoperand数は次のとおり。

| checkpoint | postopt Unknown pointer値 | JitCtx* | 実機stackmap locations |
| --- | ---: | ---: | ---: |
| 1 | 3 | 1 | 4 |
| 2 | 4 | 1 | 5 |

生成ログのStackMap ID例：
- xyo_script_0_p0 site1: 18281173708092752603
- xyo_script_0_p0 site2: 11584227487189984376
- xyo_script_1_p0 site1: 1743975969463278940
- xyo_script_1_p0 site2: 8440922190366047167

ORCが生成した`obj-0.o`（9384B）、`obj-1.o`（9248B）の両方に `.llvm_stackmaps` セクションが出現した。`llvm-readobj --stackmap`では両者とも LLVM StackMap Version 3、1 function、2 callsite records を確認。

実際の機械位置：
- site1: `Register R#3`, `Register R#15`, `Register R#14`, `Indirect [R#6 - 56]`（いずれも8バイト）
- site2: `Register R#3`, `Register R#14`, `Register R#13`, `Indirect [R#6 - 56]`, `Indirect [R#6 - 120]`（いずれも8バイト）

この出力は**実際のmachine stack/レジスタ位置**であり、ソース上のLLVM命令名でもGCメモリのアドレスでもない。R#番号はDWARFレジスタ番号として解釈する必要がある。

### 回帰・対照実験

- O3 Eager：4地点全件StackMap ID一致、4/5か所のlocation件数一致、Scratch Interpreter一致、PASS
- O3 Lazy：同様に4/4 PASS
- O2 Eager：4/4 PASS
- O2 Lazy：4/4 PASS
- `XYO_RT_MACHINE_STACKMAP_PROBE=0` 対照：同じ隔離CLIで実行した2つのELFオブジェクトのどちらにも `.llvm_stackmaps` セクションなし。Scratch状態一致。
- LLVM23単体O3の正規呼出し/不正キャプチャ：PASS。

## GC安全性の制限

現時点でStackMapが示すのは、`xyo_rt_capture_llvm_ssa`の**直後**の機械語プログラムカウンター位置における、一部のLLVM SSAポインタ値の格納位置だけである。

GCを安全に実行するには、少なくとも以下が未実装：
1. スケジューラで中断中のFiberが**実際にそのStackMapの機械語位置で停止している**ことの証明／PC対応。
2. GCが他のFiberに対してstop-the-worldし、各実行スタック／callee-saved registersと、そのStackMapの正確なframe baseを特定する仕組み。
3. StackMapがカバーする未知ポインタだけでなく、`i64`にboxされたScratch Value/文字列/リスト・ホスト保持値・別の一時値のroot所有権まで含めて**完全性を証明**すること。たとえば最適化後の104個のstack由来候補は今回個別StackMap operandとして公開していない。
4. 実際の保存位置からGC-managedオブジェクトかどうかを安全に判定してmarkするための型／ハンドル証明。
5. サスペンド中にStackMapで指し示された参照先が再割当・移動・破棄されない寿命契約。

以上が揃うまで、`FiberPool::unsafeCount()`を使ったnative FiberのGC禁止は**絶対に解除しない**。実験モードは安全性の仕組みを調査する段階であり、デフォルトの実行経路ではStackMapを出力しない。

## 検証環境・ファイル

- 検証CLI：`/home/kicky/.rt001-machine-stackmap-isolated/xyo`
- 以前のLLVM23ライブラリの隔離コピーに、現在の`backend_llvm.cpp`と、他担当が追加したシンボルを整合させるための`rt.cpp`を再コンパイルして差し替えた。**共有作業ツリー全体のクリーンビルドではない**。
- 生成ORC ELF：`/home/kicky/.rt001-machine-stackmap-isolated/machine-objects/`
- Python生成・検証：`python3 tests/differential/rt_machine_stackmap_probe.py --native /home/kicky/.rt001-machine-stackmap-isolated/xyo --out build/rt001-machine-stackmap-validation`
- O2手動追加検証：`build/rt001-machine-stackmap-o2/`
- 全ネイティブ回帰ログ：`build/rt001-machine-stackmap-full.log`（**955/955 PASS**）。
- コミット・pushは行わず、他の担当者の共有ツリー変更は保持。CMake/CTest全体の再生成・実行は未実施。

## 最終検証記録

- 隔離LLVM23 CLI SHA256：`a81115b29071eaba00836a09239a0ad1e98a04ca768fc0946ae535031bdade18`。
- 実2-Fiber ScratchのO3 Eager、O3 Lazy、O2 Eager、O2 Lazyは**4/4構成PASS**。各構成の実ORCオブジェクトから機械StackMapレコード4件を抽出し、コンパイラsite IDとlocation数を照合。
- O3 Omit (実験フラグOFF)の独立実行では生成ELF2個にStackMapセクションなし、Scratch Interpreter一致。
- LLVM O3 IRBuilder smoke：正規captureはstackmap挿入成功、不正captureはfail-close、最終module verifierもPASS。
- 既存native resume/fail-close full regression：**955/955 PASS**、`build/rt001-machine-stackmap-full.log`。
- Python `-m py_compile`・対象ファイルの`git diff --check`：PASS。
- `tests/CMakeLists.txt`にCTestを登録（`llvm-readobj`・Python・`xyo-cli`が利用可能な時のみ）。共有ツリー全体のCMake再生成・CTest全体は未実施。

**さらに重要な制限：** `llvm.experimental.stackmap`に候補ポインタを引数として追加すると、それ自体がLLVMの追加の使用地点となり、当該変数の生存期間・レジスタ割当を変える可能性がある。これは**プローブ付きの機械コードにおける位置を記録した**証拠であり、プローブなしの元コードの機械レベルlivenessをそのまま観測した証拠ではない。GC許可を与える前に、プローブによるコード変化も含めた完全な安全点契約を別途証明する。
