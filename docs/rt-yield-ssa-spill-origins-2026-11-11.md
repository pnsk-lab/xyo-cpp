# RT-001：中断中FiberのLLVM i64 StackMap機械配置・SSA出所監査（2026-11-11）

## 結論

LLVM23のORC Eager JITが実際に生成した`.llvm_stackmaps`の**全てのi64単独診断location**について、LLVM SSAの定義IDとx86-64の機械語配置を対応付け、中断中の2本のFiberを正規DWARF FDEで巻き戻して、スタック上の配置が実際にFiberの有効なスタック領域内にあるか検証した。

この証明は**スタックアドレスの範囲**と**一部callee-savedレジスタの復元可能性**までであり、値の**GC所有権、boxed i64が参照か数値か、メモリ内容が保持されていること、全ポインタのrootの完備性**を保証しない。`FiberPool::unsafeCount()`によるGC禁止は変更しない。

### 実測

実Scratch fixture `ssa_budget_cross_fiber_two_hats`、LLVM O2/O3。2本の実中断中Fiberを対象に、各手続き2箇所、全4箇所の診断StackMapを調査。

| 指標 | 結果 |
| --- | ---: |
| StackMapの位置 | 4箇所 |
| i64値のLLVM liveness候補 | 148（1手続き当たり34＋40、2手続き） |
| 呼び出し引数ポインタ | 4 |
| StackMap locationの合計 | 152 |
| RBP相対スタックスロット | 138 |
| Callee-saved機械レジスタ | 12 |
| 実中断地点から復元不能なcaller-savedレジスタ | **2** |
| 中断中JITフレームの有効な正規DWARF FDE | 2/2 |
| RBPのstack topからの距離 | 96バイト（2本とも） |
| Fiber保存RSPのstack topからの距離 | 1,488バイト（2本とも） |
| Fiberの予約スタック容量 | 1,048,576バイト |
| RBP+オフセットで得た138のスタックアドレス | すべて保存RSPより上／有効なFiberスタック内 |
| すべてのGC管理値を復元できるという証明 | **なし** |
| GC許可 | **なし** |

**重要な不足の具体例**：最初の直接yieldのStackMapの10番目の機械locationは`Register R#0`、つまり`RAX`。x86-64 SysVで`RAX`はcaller-savedなので、`xyo_yield`の内部で中断している状態から、そのレジスタにあった値を復元できることは**保証されない**。2本のJIT Fiberで発生した計2箇所を`volatile_unrecoverable`として分類した。

LLVM SSAの**コンパイル時の定義ID 198**に対応し、最適化後のopcodeは`add`。この出所は新しいコンパイラoperand manifestで、1-based location番号→LLVM SSA定義ID→opcodeを逆引きした結果。元のScratchの具体的な式・ループカウンターという意味付けは未証明。

## 実装と検証

- `tests/platform/rt_native_fiber_cfi_unwind_smoke.cpp`
  - opt-in `XYO_RT001_YIELD_SPILL_BOUNDS_PROBE=1`で、実JITフレームを巻き戻した時点のDWARF`RBP`、実Fiber保存`RSP`、スタック容量の**相対距離**だけを記録。生のアドレスは出力しない。正規FDEと中断中のスタック範囲外ならframe不正とする。
- `tests/differential/rt_yield_ssa_spill_bounds.py`
  - C++ハーネスで実Scratchの2本のFiberを中断、最適化後のLLVM SSA StackMapを有効化した実ORC ELFを出力。
  - 既存の厳格なcall-edge検証器で、StackMap→直接yield call命令→実Fiber JIT return PCを照合し、実正規FDEの成立を要求。
  - `llvm-readobj --stackmap`の**各location**を解析して、`Indirect [R#6 + negative_offset]`のみ実スタックスロット候補として認め、8バイト整列・有効スタック領域内・保存SPより上を必須とする。レジスタ直置きではR3/R6/R12–R15をcallee-saved、R0等を**明示的な未復元値**として数える。Direct/Constantや不明なbaseレジスタは推測せず拒否。
  - コンパイラに新しく出力するoperand manifestと、`llvm-readobj`の実際の位置番号を全152件で1対1に照合。出所／opcodeを明示し、機械位置だけで管理対象オブジェクトと断定する処理は行わない。
- `src/ir/rt_yield_ssa_machine_stackmap.hpp`
  - `YieldSsaOperandSource`に1-based location、分類`helper_arg/i64_instruction/i64_argument/pointer`、LLVMのsource instruction ID、opcodeを保持。実StackMapの順序と常に一致させる。
- `src/jit/backend_llvm.cpp`
  - `XYO_RT_YIELD_SSA_TRACE_OPERANDS=1`の**追加opt-in**で、定義ID／opcodeだけをコンパイラログに記録。未証明のownership、GCルート、管理対象アドレスの生値は出力しない。`gc_safe=0`を固定。
- `tests/platform/rt_yield_ssa_machine_stackmap_smoke.cpp`
  - LLVM O3の実SSA producer、`i64 mul`の存在、StackMap location番号の一意・昇順・source分類の一致をアサート。
- `tests/differential/rt_yield_ssa_spill_contract.py`
  - 16件の不正なstackmap/CFI/GC証拠（volatileをroot扱い、Direct/Constant、符号・整列・range違反、FDE欠落、偽のGC許可等）を拒否する独立テスト。
- `tests/CMakeLists.txt`
  - `xyo-rt001-yield-ssa-paused-spill-bounds-smoke`（正規FDEプラグインとLinux x86-64がある時のみ）と`xyo-rt001-yield-ssa-spill-contract-smoke`を登録。

## 実行結果

- LLVM23隔離ビルド：`/home/kicky/.rt001-orc-fde-cmake`。
- LLVM O2/O3の実停止Fiberでの4 StackMap・152 locations・138 spills・12 callee-saved・2 volatile **いずれもPASS**。
- 関連CTestの`xyo-rt001-yield-ssa-*` **4/4 PASS**（`build/rt001-yield-ssa/ctest-spill-final.log`）。
- `XYO_RT_YIELD_EDGE_STACKMAP_PROBE=1 XYO_RT_YIELD_LIVE_SSA_STACKMAP=1 XYO_RT_YIELD_SSA_FILTER=i64`で、ネイティブ復帰／不正復帰拒否の**955/955 PASS**（`build/rt001-yield-ssa/full-i64-regression.log`）。この全回帰はoperand provenanceログを追加する直前の同一診断条件で実行しており、追加後は対応するLLVM単体・実Fiber CFI検証を再実行した。
- 混合i64＋pointer、およびpointer-onlyは過去のUnicode GCストレスでScratch変数の不一致を再現したため、本番実行経路では**挿入拒否**を継続。`i64`単独が955件PASSしたこととGC rootsを安全に回収できることは**別の主張**。

コミット・pushなし。GCの安全ゲートは一切変更していない。全リポジトリCTestは実行していない。

## 次に必要なこと

1. `RAX`に置かれたSSA ID198の`add`が実際のScratchどの構文に由来するか（変数、一時式、ループカウンター）をコンパイラの元構文との紐付けから分類する。
2. Yield直前SSAの呼出し規約を跨ぐ際、caller-saved値の保持先（spill先、値の再計算、再構築手段）を、機械コードの正確なCFG／レジスタ割付から証明する。StackMap直前に位置が記録されただけでは十分ではない。
3. `i64`のタグ・所有権／生ポインタの根本的な参照元を照合し、GC管理値のみを識別する。
4. Nested helperの中断など未マップ経路を網羅してもなお、全ルート証明なしにnative FiberのGC制限を解除しない。
