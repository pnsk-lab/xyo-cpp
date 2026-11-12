# RT-001：実停止中Fiberの戻り先とnative yield-edge StackMapの対応（2026-11-11）

## 概要

LLVM JITの`xyo_rt_capture_llvm_ssa`直後にStackMapを発行しても、中断中のFiberでは保存RIPがホスト`xyo_fiber_switch`の戻り先になる。DWARF CFIで見えるJITフレームのPCは`xyo_yield`からの戻り先であり、capture直後のStackMapとは別位置である。

今回、**最適化後O2/O3 IRに現れる実際の`xyo_yield`または`xyo_yield_fast`の直接呼び出し直前**に、独立した診断用 `llvm.experimental.stackmap` を挿入する。

さらにLLVM23 ORCが生成したELFを`llvm-readobj --stackmap`、`llvm-objdump --reloc --disassemble --syms`で検査し、StackMap命令の機械位置・その直後の引数レジスタ設定・yield callの命令位置・x86-64のreturn PCを厳密に照合した。実際に停止しているFiberでは、保存済みSysVレジスタから`libunwind`で真のJIT呼び出しフレームまで巻き戻し、その復帰PCを上記のreturn PCと一致させた。

**この実験は呼び出し地点とStackMapの対応関係を検証するものであり、GCルートの完全性やサスペンド中のレジスタ値をStackMapが保存することは証明しない。**

## 実装

- `src/ir/rt_yield_edge_stackmap.hpp`（新規）：
  - `emitExperimentalYieldEdgeStackmaps(llvm::Module&)`は最適化後の`xyo_script_*`関数内にある直接の`xyo_yield`、`xyo_yield_fast`呼び出しだけを対象とする。
  - `xyo_yield`では必ずその関数の第1引数のJitCtxを渡していることを要求。helperが指定した引数1件・戻りvoidのABIに一致しないもの、musttail、重複IDは拒否する。
  - 各呼び出しの前に`llvm.experimental.stackmap(i64 hash(function, ordinal), i32 0, ptr helper_arg)`を挿入。識別子は既存のSSAキャプチャや非同期レポータとは異なるFNVドメイン`rt001.yield.edge`。
  - 実際のGCオブジェクトの全ライブ値を載せたわけではない。StackMap operandは**呼び出しに渡すポインタ1件のみ**。ベンチマーク実行の意味論は変えず、`machine_roots_proven=0 gc_safe=0`を常に出す。
- `src/jit/backend_llvm.cpp`：既存のO2/O3パイプライン後のEagerとLazyの両方で、`XYO_RT_YIELD_EDGE_STACKMAP_PROBE=1`のときだけ挿入・診断ログ出力。
- `tests/platform/rt_yield_edge_stackmap_smoke.cpp`：実LLVM23 O3 IRで有効な直接呼び出し2件（通常とfast）に挿入し、JitCtxを偽装した呼び出し1件には挿入しないこと、生成IRを`llvm::verifyModule`が受理すること、**元のyield呼び出しの直前**にpseudo instructionがあることをアサート。`-UNDEBUG`を付けて実際にアサートを有効化して確認。
- `tests/differential/rt_yield_edge_fiber_pc.py`：実Scratchプロジェクトの2本のLLVM Fiberが中断中のC++ハーネスを実行。ORC ELFのStackMap IDs／offset／location数と、コード内`R_X86_64_PLT32 xyo_yield-0x4`等の直接call再配置情報を合わせ、StackMapからcallの間に**mov/lea/nop以外の機械命令が存在しないこと**を逆アセンブラで確認する。call長が5バイト、返るPCがcall直後であることを必須とし、`libunwind`により得た実JIT戻りPCをこれに照合する。
  - オプション`--require-fde`は前段階のORC/libunwind FDEプラグインを使い、2本とも実際の長いFDEで認証できること（1バイト合成FDE拒否）を必須とする。
  - 常に`machine_roots_proven=0`、`gc_safe=0`を要求。
- `tests/differential/rt_yield_edge_machine_probe.py`：実xyo CLIでScratch結果をInterpreterと比較し、**Eager / ORC Lazy**の生成ELFすべてについて上記の機械語対応を検査する。Lazyの追加支援ELFは明示的に判別し、Scratch本体2関数のStackMap4件は必ず機械語照合させる。実験OFFの2つのELFの`.llvm_stackmaps`不在も確認。`--fast`はCallFrame OFFでCLIを直接実行し、`xyo_yield_fast(Fiber*)`の実際の手続きも同じ機械語oracleで検証する。
- `tests/CMakeLists.txt`：LLVM単体、Eager/Lazyの機械語、停止中Fiberの通常／正規FDE必須の4テストを追加（Linux x86-64限定のものを含む）。

## 実測例

`ssa_budget_cross_fiber_two_hats`：非warp文字列連結を2スクリプトが並列実行する実際のScratchプロジェクト。

保存RIPはホストのFiber switch復帰先であり、そのままJITの機械命令を指すものではない。保存されたRSP/RBP/RBX/R12-R15/RIPから独立にunwindした場合、実JITのyield復帰先を特定できる。

既存のSSAキャプチャ直後StackMapも同時ONのO3 CFIハーネスでは：

| 機械位置（関数相対バイト） | 内容 |
| ---: | --- |
| 1897 | **新しいyield-edge StackMap**と引数レジスタ設定 `mov %rbx,%rdi` |
| 1900 | x86-64 direct `call xyo_yield` |
| 1905 | **中断中Fiberから正規CFIで巻き戻したJIT復帰先PC** |

既存の捕捉StackMapをOFFにした通常CLIでは、同じ命令列でもレイアウトが異なり、最初のyield-edge StackMapは1840、その後のcallは1843、戻りは1848だった。**オフセットはそのビルド内でのみ有効**であり、最適化・レジスタ割付・プローブ挿入によって変わる。比較は毎回その実際のORC ELFで行う。

`XYO_RT_ORC_FDE_REGISTRATION=1`を有効化した正規FDEモードでは、2本のFiberとも`jit_cfi=1 one_byte_fallbacks=0`、正確なJIT return offset=1905で一致。検証用プラグインは正規FDEをORCコードの生存期間だけ公開する。

## 結果

- O2 Eager CLI：4/4 StackMap↔native call対応、Interpreter一致、PASS
- O2 Lazy CLI：4/4、Interpreter一致、PASS
- O3 Eager CLI：4/4、Interpreter一致、PASS
- O3 Lazy CLI：4/4、Interpreter一致、PASS
- O2・O3実2-Fiberの中断中状態：両FiberでJIT return offset=1905がnative call returnと一致、PASS
- O2・O3とも正規ORC FDEを要求した追加モード：両Fiberで`jit_cfi=1`、PASS
- 実LLVM23 O3単体テスト：直接呼出し2件挿入・偽装引数1件拒否・module verifier、PASS
- 隔離ビルド`/home/kicky/.rt001-orc-fde-cmake`でxyo本体とCFIハーネスを含む対象を再コンパイル・リンク後、**yield-edge関連CTest 5/5 PASS**。ログ`build/rt001-yield-edge/ctest-final.log`。前回のFDE Eager/Lazy動作との組合せを含む拡張関連CTest 6/6も`build/rt001-yield-edge/ctest-yield-edge.log`で確認済み。
- **実際の`xyo_yield_fast(Fiber*)`経路**も、CallFrames OFFで実Scratch CLIを実行して確認。2 script × 2 yield地点の一部に`xyo_yield_fast`が現れ、O2/O3・Eager/Lazyの各4/4実機StackMap↔ELF direct-call再配置対応、Interpreter最終結果一致でPASS。上記`xyo_yield(JitCtx*)`のみの試験と区別し、`--fast`オプションを追加。
- `XYO_RT_YIELD_EDGE_STACKMAP_PROBE=0`の**実ORC ELF対照実験**では2ファイルとも`.llvm_stackmaps`セクション不在、JIT結果Interpreter一致、コンパイラのyield-edge診断ログなし。
- 新StackMap診断ONのまま`tests/differential/native_simple_resume.py`全量：**955/955 PASS**。ログ`build/rt001-yield-edge/native-resume-full.log`。従来のNative CallFrame復帰・GC安全性のfail-closed条件が保たれている。

重要：実行時にはyieldのcallの中からさらにホスト関数を経て`xyo_fiber_switch`で中断する。`llvm.experimental.stackmap`の命令位置はcall直前であり、保存済みFiber状態の実レジスタ・ローカル変数の値がその位置にあるという証明ではない。登録されたのもJitCtx/Fiber引数だけで、boxed i64 Scratch ValueやGC-managed文字列/リスト全体のrootについては不完全。

## ビルド上の制限

同時に別担当の未コミット変更が加わったため、途中の共有ツリーCMakeビルドで`src/jit/codegen.cpp`内のLLVM23 API呼出しが不整合になり一時的にビルド停止した。別担当ファイルを巻き戻さず、前回成功したLLVM23隔離ビルドのlibxyo.aに新しいbackend_llvmとrt_blocksだけを差し替えた隔離CLI`/home/kicky/.rt001-yield-edge-isolated/xyo`でも全4構成を検証した。

その後ソース側の変更が整合し、共有ソースからの隔離CMake`/home/kicky/.rt001-orc-fde-cmake`で**再ビルド成功**。実CTestも6/6 PASS。ユーザーの共有作業ツリーの他担当変更は保持した。コミット・pushなし。

## 今後の安全性課題

1. 現在のStackMapは**yield callの直前**にあり、実際のFiber中断はさらに呼び出しの深い場所で起きる。機械語call前のspill/callee-save規則とunwindの実フレームから、当該StackMapのlocationを**同じ値に対応付けられるか**別途証明が必要。単にPCが隣接しているだけではmarkルートの使用を許可できない。
2. source SSAのポインタ値とnative boxed `i64` Scratch Value・関数引数・一時式・GCオブジェクト所有権を網羅し、命令ごとの厳密なroot集合を作る必要がある。
3. `xyo_yield`以外のyield可能ヘルパ（wait/broadcast/ask/停止など）もある。現在のedge診断対象は`xyo_yield`と`xyo_yield_fast`のみ。他の中断先では未対応として拒否すべき。
4. これらの証明が揃うまで、`FiberPool::unsafeCount()`によるネイティブFiberのGC禁止は必ず維持する。
