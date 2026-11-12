# RT-001：保存済みFiberRegsからのlibunwind検証（2026-11-10）

## 結論

Linux x86-64 SysVで中断中の実LLVMネイティブFiberのレジスタを`unw_context_t`へ復元し、`unw_init_local`と`unw_step`でJITフレームへ到達する実験を実装した。

結果は重要な**部分成功と未証明箇所の分離**：

- Eager JITの2本の生存Fiberそれぞれで、unwinderがJIT手続き内の`xyo_yield`からの実際の戻り先を特定した（関数相対オフセット1869）。
- 直前の実StackMapは1861、すでに終わった`xyo_rt_capture_llvm_ssa`の戻り先は1849。`llvm-objdump --reloc`による正確な再配置位置からも一致を確認した。生スタック走査で見つかった1849を誤って生存フレームと認定しない。
- **ただし、libunwindがJIT内のPCについて返すproc_info範囲は[1869,1870)の1バイトだけ**。これは正規のJIT用FDEを確認した証拠とはみなせず、代替的なframe-pointer unwind結果として厳格に区別した。
- `llvm-dwarfdump --eh-frame`では、実ORC ELFのJIT関数全体に対応する長いFDEが存在する。生成ELF 2個で、FDE範囲は [0x190,0xf5b) と [0x170,0xf3b)。unwind対象の実PCはこの領域内だが、ランタイム側libunwindは完全なFDEを参照していない。
- **ゆえに、実際のJITフレームの復帰先候補は再配置情報で確認できるが、実行時DWARF CFIの完全な所有権は未証明**。StackMapオフセットとJIT停止PCも一致していない。GCの有効化は認めない。

## 追加・更新ファイル

- `tests/platform/rt_native_fiber_cfi_unwind_smoke.cpp`：実Scratchの2スクリプトをLLVM Eagerでコンパイルし、ネイティブFiberを本当に中断させる。元の`FiberRegs`のRBX/RBP/R12-R15/RSP/RIPを`unw_context_t.uc_mcontext.gregs`に設定。実レジスタからlibunwindのcursorを初期化し、最大40フレームで境界つき巻き戻し。停止中のスタック範囲を越えないことを検査する。JIT範囲内の復帰アドレスが1個見えること、`unw_proc_info`の範囲が実FDEなら長さが1バイトより十分長いことを確認し、単一バイトproc_infoなら`one_byte_fallbacks=1`、`jit_cfi=0`、`machine_frame_verified=0`とする。フラグは診断用、GC許可は固定false。
- `tests/differential/rt_native_fiber_cfi_unwind.py`：実SB3を生成し、このC++ smokeを起動して本物のORC ELFをダンプ。`llvm-objdump --syms --reloc`でJIT関数先頭およびキャプチャ／yieldの正確な戻り先を導出。`llvm-readobj --stackmap`からStackMap地点を照合。`llvm-dwarfdump --eh-frame`で実ORC ELF内に長いDWARF FDEが存在しJITの復帰先を覆うことも検証する。各unwinderフレームの`jit_cfi`、合成1バイトFDE、認定フラグ、GC vetoを検査。
- `tests/CMakeLists.txt`：`xyo-rt001-native-fiber-cfi-unwind-smoke`を追加。Linux x86-64かつPython/llvm-readobj/llvm-objdump/llvm-dwarfdumpとlibunwind/libunwind-x86_64が存在する場合のみ登録。通常ビルドにlibunwind依存を追加しない。

## 実測（隔離LLVM23）

共有ツリーの既存検証済み`/home/kicky/.rt001-machine-stackmap-isolated/libxyo.a`を利用し、新しいC++テスト実行ファイルを別途ビルド・リンク。

| 項目 | O2 | O3 |
|---|---|---|
| 実2-Fiber の巻き戻し | 2/2 PASS | 2/2 PASS |
| 最終JIT return offset | 1869 | 1869 |
| 生スタックに残った古いcapture return | 1849 | 1849 |
| 最適化後StackMap位置 | 1861 | 1861 |
| libunwind の実JIT FDE確認 | **未証明（代替1バイト範囲）** | **未証明（代替1バイト範囲）** |
| ORC ELFの長い `.eh_frame` FDE | **確認済み** | **確認済み** |
| 中断中FiberのGC許可 | **禁止継続** | **禁止継続** |

既存ネイティブ復帰／失敗クローズのフル回帰955件は、同じ隔離ライブラリ由来のCLIで前段階で確認済み（`build/rt001-machine-stackmap-full.log`）。今回はランタイムやLLVM生成処理を変更していないため全回帰は再実行せず、新しい独立ハーネスでO2/O3を検証した。共有ツリー全体のCMake再生成・全CTestは未実施。コミット・pushなし。

## 次に必要な工程

1. ORCが生成した`.eh_frame`を**実行時libunwindのアドレス空間に正しく登録・公開**し、`unw_get_proc_info`が実際のJIT関数長のFDEを返すことを証明する。単なる1バイト幅の合成proc_infoは不可。
2. 保存されたFiberRegsから現在実行中のJIT call chainとCFA/フレームベースを誤りなく取得する。特に保存PCは`xyo_fiber_switch`のホスト復帰先であり、StackMap命令のPCではない。
3. LLVM側StackMapを**Fiberが実際に停止した命令**と対応付けるsafepointプロトコルを設計する。今回の捕捉直後StackMap位置1861と本当のyield戻り1869は別命令。
4. 対象となる全GCオブジェクト、boxされた`i64`のScratch Value、元ポインタおよびコンパイラ一時値の完全なルート提供を別途証明する。
5. 上記の証明が完成するまで、`FiberPool::unsafeCount()`によるサスペンド中FiberのGC禁止を維持する。
