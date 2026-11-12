# RT-001：中断中Fiberの保存PCと機械語StackMap位置の照合（2026-11-10）

## 結論

**実際に中断した2本のネイティブLLVM Fiberで、保存された`FiberRegs.rip`はJIT内のStackMap命令ではなくホスト側`xyo_fiber_switch`の復帰先を指すことを検証した。**

2本の独立Scratchスクリプトが、非warp文字列連結手続きをLLVMで実行中に中断した状態で測定した。双方とも`NativeRtSsaCapture`が作成済みで最後のsource checkpoint IDは1だった。

- 保存PC（`FiberRegs.rip`）：ELFのホスト実行ファイルに属する。JIT生成コードには属さない。
- LLVM JITの`CompiledScript::entry`：ORCが確保した実行可能メモリ内にあり、ホストELFの`dladdr`所有範囲外。
- 停止中Fiberの使用済みスタック区間を、**検証されたメモリ境界の範囲内に限って**読み、`CompiledScript::procedures`の既知関数エントリから32KiB内にある生ワードを探索したところ、両FiberでJIT戻りアドレスの**候補**が2件ずつ見つかった。
- 候補のうち先頭の関数先頭からのオフセット：**1849**。
- 同じLLVM生成手続きのsite1の実際のORC ELFの`llvm-readobj --stackmap`命令オフセット：**1861**。
- 差：**12バイト**（両Fiber、O2/O3とも一致）。
- `machine_frame_verified=0`, `stackmap_pc_matched=0`, `gc_safe=0`を強制している。

**さらにELF再配置情報と逆アセンブリの検証で、先頭の候補1849は、すでに呼び出しが終了している `xyo_rt_capture_llvm_ssa` の戻りアドレスと厳密に一致することが分かった。** 実物の機械語では関数先頭から1844バイトでこの関数へcallし、1849バイトで戻り、StackMapが1861バイト、`xyo_yield` のcallが1864バイトでyield-callの戻り先は1869バイトにある。

したがって、そのスタック上の1849という生ワードは**現在の生存フレームの戻りアドレスだという証拠にはならない**。既に戻ったSSA capture呼出しの古い戻りアドレスがスタック内に残っている反例として解釈すべきであり、ただ「JITコードのアドレス範囲に入っている」「StackMapから12バイト」と判定する仕組みは危険である。

## 技術的な根拠

`src/engine/fiber_x86_64_sysv.S`の`xyo_fiber_switch`は、Fiberを停止する際に

```asm
leaq 8(%rsp), %rax
movq %rax, 48(%rdi)
movq (%rsp), %rax
movq %rax, 56(%rdi)
```

によって、スイッチ関数の呼出し元に戻るための`rsp`と`rip`を保存する。JIT関数が`xyo_yield`などのランタイムヘルパを呼び、その中から`Fiber::yield()`、さらに`xyo_fiber_switch`を呼んだ場合、最初に復帰するのはJIT命令ではなくホスト関数のフレームである。

一方、LLVMの`llvm.experimental.stackmap`はコンパイルしたJIT関数中のその疑似命令に対する正確な**機械語位置の診断**を作るが、異なるPCで実際に停止したFiberのroot register一覧を提供するものではない。保存PC=StackMap命令PCという誤った仮定ではGCを実行できない。

## 今回追加した検証

- 新規`tests/platform/rt_native_fiber_pc_stackmap_smoke.cpp`：実際のRuntime／2 Native FiberをEager LLVMで動かす。Fiber2本がstarted&&!finished、SSAキャプチャが存在、原始的GC安全ゲート`FiberPool::unsafeCount()>=2`の状態で、`FiberRegs.rip`と`CompiledScript::entry`の異なる実行領域を`dladdr`で確認する。後段ではsuspendedSpからstackBaseまでの上限1MiB内にある既知JIT procedureアドレス範囲のワードを診断する。**候補を保存PCとして認定しない。**
- 新規`tests/differential/rt_native_fiber_pc_stackmap.py`：Pythonが本物のScratch SB3を生成し、実際の停止中Fiberテストを起動、同じプロセスからダンプされたORC ELF2ファイルを`llvm-readobj --stackmap`で解析。さらに`llvm-objdump --syms --reloc`で実手続き開始オフセット、`xyo_rt_capture_llvm_ssa`と`xyo_yield`の呼び出し再配置アドレスを取得。x86-64の相対call（再配置オフセット+4=戻り先）に基づいて、スタック上で見つかった1849が既に終わったキャプチャ呼び出しの戻り位置と一致し、site1 StackMap1861とyield-call戻り1869の間に関係があることを両Fiberで厳密に検証する。**このELF照合でさえDWARFのスタック巻き戻しを代替しない**。常に`machine_frame_verified=0`、`gc_safe=0`を要求する。
- `tests/CMakeLists.txt`：Linux x86-64 ELFかつPython＋llvm-readobjが利用可能な時のみ、新規CTest`xyo-rt001-native-fiber-pc-stackmap-smoke`を登録。C++実行ファイルはxyoと`CMAKE_DL_LIBS`にリンク。

## 検証結果

実験環境は以前確認済みの隔離LLVM23バイナリ：

```
/home/kicky/.rt001-machine-stackmap-isolated/xyo
```

新C++ harnessはこの隔離`libxyo.a`とリンクした。共有ツリーの全ソースクリーンビルドではない。

- O3、Eager LLVM JIT、実2-Fiber suspend／SSA capture／ORC StackMap：**PASS**。
- O2、Eager LLVM JIT、同一の検証：**PASS**。
- 2本とも`last_source_site=1`、保存RIPはホスト、関数戻り候補offset1849、StackMap命令offset1861、距離12バイト。
- 過去の同じ隔離CLI全体回帰955/955 PASSについては`build/rt001-machine-stackmap-full.log`を参照。今回の新規C++ハーネスはLLVM/GC実装を変更しておらず、既存ネイティブ実行バイナリと同じ。
- `python3 -m py_compile tests/differential/rt_native_fiber_pc_stackmap.py`と対象ファイルの`git diff --check`：PASS。
- O3・O2の両方で再配置を解決した呼出し戻りアドレスとの一致（捕捉済みの古いSSA capture callが1849、StackMapが1861、yield callが1869）を**2/2 Fibers PASS**。Pythonで再配置先の正規の呼出し命令を参照しているため、単なる近傍推測ではなく、実際に機械コード内に存在した古い戻り位置を証明する。
- 対象ソースは新規2つのテストファイルと`tests/CMakeLists.txt`のみ。GC・Fiber・JITの実装コードの変更なし。コミット・pushも行っていない。

## 次の段階

GC安全性の実証には、たとえばLinux DWARF CFI（JITの`.eh_frame`を含む）を使い、**保存されたFiberRegsを開始点として本当にスタック巻き戻し**を行う必要がある。その結果がORCの関数別code address／StackMapのPCと整合すること、StackMapの位置で停止させる強制的なsafepoint、そしてScratchのboxされたi64 ValueなどすべてのGC rootの完全性を別々に証明すべきである。

今回の実験ではGCは**絶対に**サスペンド中Fiberに対して回収しない。

## リスクと次の段階（再配置検証後）

生のFiberスタックを走査してJIT範囲内のアドレスを見つけただけで「生存しているフレームを復元した」とみなすのは不正確。今回、**すでに終了したキャプチャ関数の戻りアドレスが候補として残存する反例を実際の機械語再配置と一致させた**。したがって、単純なスタックワード検索でGC rootを認可することはできない。

次の実装は正規のunwind規則（Linux x86-64 DWARF CFIとFiberRegsのcallee-saved GPR、実JIT `.eh_frame`）の取得、実際の呼出しフレーム同定、各PCに対応するStackMap safepointの停止確認に限定すべきである。LLVM StackMapに載せていないbox済みScratch Valueやネイティブ一時変数のroot完全性も、別途必要。
