# RT-001：SSA ID198の意味解析と数値スケジューラカウンターの除外（2026-11-11）

## 結論

実際に中断中の2本のScratch native Fiberで、yield直前LLVM StackMapの`RAX`に配置されていた`i64`値の出所を特定した。LLVM SSA ID198の`add`はScratch変数`y`でもboxed文字列参照でもなく、**Codegen::emitLoopYield()が生成する内部`curYieldCnt` / `JitCtx::yieldCheckN`の反復カウンター**である。

`XYO_JIT_DUMP2=1`で確認した最適化後IR（この診断モードではID204になる；LLVM命令IDはビルド・フラグ局所の番号であり固定識別子ではない）：

```llvm
%8 = getelementptr i8, ptr %0, i64 104
%9 = load i64, ptr %8
; ...
%ycnt.0141 = phi i64 [ %9, %cmpok.done ], [ %121, %214 ]
; ...
%121 = add i64 %ycnt.0141, 1
```

`JitCtx`のABI定義`src/jit/jit.hpp`では`yieldCheckN`はoffset 104（64-bit SysV）。`src/jit/codegen.cpp`の`beginLoopYieldScope`は`JitCtx::yieldCheckN`を`ycnt`にロードし、`emitLoopYield()`は反復ごとに`b.CreateAdd(load(curYieldCnt), 1)`する。このため値は**GC管理ポインタではなく内部の整数位相カウンター**。ただし他の`i64`の所有権はまだ不明。

## 実装

- `src/ir/rt_yield_ssa_machine_stackmap.hpp`に`isNativeYieldTickIncrement`を追加：
  - `i64 add(PHI, 1)`であること。
  - `PHI`の2 incomingは当該`add`からのループ戻りと`load i64`の初期値であること。
  - 初期値のアドレスは第1JIT ABI引数`JitCtx*`を起点とする**単一byte GEP**であり、フィールドのoffsetはbackendが渡した`offsetof(JitCtx, yieldCheckN)`と完全一致すること。
  - 不正なGEP、別offset、変形された加算、偽造されたctxなどは**unclassified**。LLVMの変数名`ycnt`には依存しない。
  - マシンStackMap operand manifestに`semantic=scheduler_tick`を追加。生のポインタは出力しない。
- `src/jit/backend_llvm.cpp`は追加の診断フラグ`XYO_RT_YIELD_SSA_TRACE_OPERANDS=1`で、構造上証明されたときのみ`[rt001-yield-ssa-semantic]`を記録。生成位置・元SSA ID・`root_kind=numeric_not_managed`・`gc_safe=0`を出す。
- `tests/platform/rt_yield_ssa_machine_stackmap_smoke.cpp`にPHI/ADD/host ctx offsetの正負分類テストを追加。2や違うoffsetで加算したり、コンテキストが欠落／不一致なら分類拒否。実LLVM23/O3アサート有効でPASS。

## 実停止中FiberのSSA・機械位置検査

`tests/differential/rt_yield_ssa_spill_bounds.py`はSSA locationの順序付きorigin manifestと実ORC ELFの1-based machine locationを厳密に一致させて、実CFIが示すJITフレームからスタック範囲を確認する。

変更前の`i64`専用診断（4 machine StackMap）：

| machine location | 個数 |
| --- | ---: |
| `Indirect [RBP + negative]`（8バイト整列、Fiberスタック内） | 138 |
| `RBX/R12–15`などcallee-saved | 12 |
| `RAX` volatile（直接yield内部での復元不可） | **2** |
| 合計 | **152** |

2つのRAX locationは双方ともLLVM SSA ID198、opcode `add`に由来し、`scheduler_tick`の構造的分類に成功。元のLLVM IRではこの値の少なくとも一時的なコピーが`movq %r12,%rax`と`movq %r12,-0x50(%rbp)`として呼び出し前に見えており、呼び出し後にはスタックコピーの参照がある。ただしこのバイトコード形状は最適化・フラグ依存であり一般のGC証明には利用できない。

## 新しい任意の数値除外モード

`XYO_RT_YIELD_SSA_FILTER=i64_no_tick`は、従来の`i64`だけの診断モードから、**上のABI+PHI recurrenceで証明できた純数値カウンターだけを除外**する。証明不能な`i64`は**すべて従来通り残す**。除外数は`[rt001-yield-ssa-numeric-excluded]`に明示し、`ownership_proven=0 machine_roots_proven=0 gc_safe=0`を固定。

実停止Fiberを正規DWARF FDEで巻き戻した結果（LLVM O2/O3、2 Fiber・4地点）：

| machine location | i64 mode | i64_no_tick mode |
| --- | ---: | ---: |
| RBP相対スタック | 138 | **136** |
| callee-saved registers | 12 | **12** |
| volatile（復元不能） | 2 | **0** |
| 合計 | 152 | **148** |

LLVM実JIT Eager/Lazyの通常CLIでも4地点それぞれ**ちょうど1件を除外**、Interpreterの結果と一致。ただし通常CLIでは元からvolatileが0件の場合があり、**中断中Fiberのハーネスを使った比較と区別**する。

`tests/differential/rt_yield_tick_exclusion.py`でEager/Lazy各4件の元SSA manifest、除外ログ、ELF machine recordsを比較。`rt_yield_ssa_spill_bounds.py --filter i64_no_tick`では2本の実Fiber CFIと4件のmachine recordsを再検査し、**volatileが1つでも復活したら拒否**する。このfixture限定の判定であり、GCを許可するものではない。

## 検証

- 本体の隔離LLVM23ビルド`/home/kicky/.rt001-orc-fde-cmake/xyo`でビルド成功。
- O2/O3両方、実停止中2-Fiberで計4地点の`i64_no_tick`機械語SSAマップ照合PASS。148 location、136 spills、12 callee-saved、0 volatileを確認。
- `xyo-rt001-yield-tick-exclusion-machine-smoke`および`xyo-rt001-yield-tick-paused-fiber-smoke`をCTestに追加。旧yield-SSA系含むO2/O3両方 **6/6 PASS**。ログ`build/rt001-yield-ssa/tick-no-root-ctest-o3.log`、`...-o2.log`。
- 実フルネイティブ回帰`XYO_RT_YIELD_SSA_FILTER=i64_no_tick` **955/955 PASS**。`build/rt001-yield-ssa/full-no-tick-regression.log`。文字列・GC延期・ソース復帰・不正SSA情報拒否・並列2-Fiberを含む。
- ユーザーの並列開発変更は巻き戻していない。コミット・pushなし。

## 未解決

純数値カウンターをStackMapに載せる必要がなくなっただけ。残った148件の候補にはJitCtx ABIポインタ1件×4、boxed i64、ループ一時値などが含まれうる。これらの**内容・GC所有権・未捕捉のpointer SSA・xyo_wait_yield内部の中断・異なるABI経路**は依然として未証明。ルート完全性が成立するまで`FiberPool::unsafeCount()`によるnative Fiber GC禁止は維持する。全リポジトリCTestは未実施。
