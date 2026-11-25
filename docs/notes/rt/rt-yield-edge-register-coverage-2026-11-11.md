# RT-001：中断中JITフレームのStackMapレジスタ値検証と未対応yield分類（2026-11-11）

## 結論

前段階で得た「LLVM O2/O3後のyield呼び出し直前のStackMap位置 ↔ x86-64機械語call ↔ Fiberの実際の復帰先PC」の対応を一歩進め、**StackMapの引数レジスタに保存されている値**を、正規DWARF FDEで巻き戻したライブJITフレームと照合した。

実際の2つのScratch非warp Fiberでは、最初のyield-edge StackMapが`Register R#13, size: 8`を報告。保存済み`FiberRegs`から`libunwind`の正規JIT FDE（`jit_cfi=1`、1バイト合成FDEではない）を使って復元したR13値が、両Fiberの`Thread::jitCtx`ポインタと**ビット単位で一致**した。LLVM O2とO3の両方で2/2 PASS。

**ここから証明できるのは1つのABI引数ポインタの値の一致だけ。Scratch Valueのboxed i64、可動GCオブジェクト、リスト、文字列、関数引数・SSA一時値の完全ルート性は全く保証しない。現在のnative Fiber GC禁止を解除しない。**

さらに、直接`xyo_yield`呼び出しとは別に、**ヘルパの内部**でFiberを切り替えるケースを明確に未対応として数える診断を追加し、「一部のcall-edgeが一致したから全中断地点がGC安全」という誤判定を防止した。

## 実装

- `tests/platform/rt_native_fiber_cfi_unwind_smoke.cpp`：
  - opt-in `XYO_RT001_YIELD_REGISTER_PROBE=1`時のみ、実JITフレームを`libunwind`で復元している位置でR13, R14, RBX(DWARF #3), R12を取得。
  - `Thread::jitCtx`の正確なアドレスと各レジスタを比較し、`[rt001-yield-register]`として真偽値を記録。スレッドの復帰先相対オフセットと`genuine_fde`も報告。raw pointer addressはログに出さない。`machine_roots_proven=0 gc_safe=0`を強制。
- `tests/differential/rt_yield_edge_fiber_pc.py`：
  - `llvm-readobj --stackmap`の第1locationが`Register R#N, size: 8`であることを各StackMap ID・命令オフセットに結びつける。第1locationを推定で`R13`固定にしない。実JITのyield callの戻り先と位置を突き合わせる。
  - 新`--require-register`は`--require-fde`とセットでのみ許可。2つのFiberの正規CFIログと照合し、対象戻り先でレジスタ候補が一意、callee-saved GPR `R#3/12/13/14`のいずれかであること、指定GPRを`libunwind`で正しく復元でき、その実値が`Thread::jitCtx`と一致することを必須とする。
  - volatileレジスタ、異なる2つのRegister候補、存在しないFDE、未取得レジスタ、異なるThread ID、GC許可の混入はすべて拒否。
- `tests/differential/rt_yield_register_contract.py`：この判定を独立した純粋な関数としてテスト。R13・R14の有効な記録2種を受理し、12種類の不正記録（欠落・重複・偽装レジスタ・偽装JitCtx・FDE欠落・誤ったGC許可等）がすべて拒否されることを確認。
- `src/ir/rt_yield_edge_stackmap.hpp`：
  - `knownPotentiallyYieldingHelper`はCodegenで既知の25種類の中断可能なヘルパと、バックエンドが直接挿入しうる`xyo_rt_async_reporter_rooted`を保持。
  - `auditUnmappedYieldEdges`は最適化後の各`xyo_script_*`の直接callから、認証可能な`xyo_yield`/`xyo_yield_fast`の直前にStackMapがない箇所、wait系・broadcast系・非同期系など**ヘルパ内部のyield**、間接call、入れ子JIT関数呼び出しを独立に数える。
  - これらについては実yield地点のレジスタとStackMapの対応を証明しておらず、すべて未対応・GC不許可と記録。
- `src/jit/backend_llvm.cpp`：`XYO_RT_YIELD_EDGE_STACKMAP_PROBE=1`のopt-in時に限り、`[rt001-yield-edge-unmapped]`および集計`[rt001-yield-edge-coverage]`を診断。標準実行時は何も追加しない。
- `tests/differential/rt_yield_helper_catalog.py`：生成器の`helperMayYield`をソースから解析し、診断側の一覧との一致（＋rooted async reporter）を検証。今後のCodegenへのヘルパ追加が見逃された場合は回帰テストで失敗。
- `tests/platform/rt_yield_edge_stackmap_smoke.cpp`：LLVM O3後の本物のIRで、正規の直接yield2件だけにStackMapを挿入し、偽装JitCtx、wait helper、間接call、入れ子JIT callの4つの未対応要因を明示して拒否。アサート有効ビルドで検証。
- `tests/differential/rt_yield_wait_unmapped.py`：実Scratch `control_wait_until`と別スクリプトのwriterを用い、`XYO_WAIT_DEP_FAST=0/1`と`XYO_EVENT_SCHED=0/1`で、通常`xyo_wait_yield`・イベント待機`xyo_wait_event_yield`の双方を確実に生成。Eager指定・Lazy指定どちらもInterpreterの最終状態と一致し、**待機ヘルパは必ず未対応として検出される**ことを確認。Lazy要求時もその待機スクリプトはEagerで生成され、別のwriterがLazyで生成されたため、テストは実際のコンパイルtierを参照している。
- `tests/CMakeLists.txt`：追加の3独立テスト（レジスタ一致、12偽装拒否、ヘルパ一覧のドリフト）とwait-untilの実検証をCTestに登録。

## 測定例

`ssa_budget_cross_fiber_two_hats`。LLVM O3/正規FDEプラグインとyield-edge StackMapを同時に有効化。

| 指標 | Fiber b0017 | Fiber b0021 |
|---|---|---|
| 実JIT戻り先関数内オフセット | 1905 | 1905 |
| 対応するyield-edge StackMap | 1897 | 1897 |
| 実x86-64 callオフセット | 1900 | 1900 |
| StackMapのlocation | DWARF R#13 | DWARF R#13 |
| FDEが認証したJITフレーム | 正規 | 正規 |
| JITフレームでunwindしたR13と`Thread::jitCtx` | 一致 | 一致 |
| Machine rootの完全性 | **未証明** | **未証明** |
| GC許可 | **なし** | **なし** |

`R13`や関数オフセットはO2/O3や別のSSA probeを有効化した場合に変化しうる。比較の度に**実際に出力された**機械StackMapとELF再配置、最新のFiberRegsを用いる。レジスタがR13と決め打ちされるコードではない。

## テスト

- `xyo-rt001-yield-edge-paused-register-smoke`：本物のFiber 2/2でCFIレジスタとStackMapのJitCtx値を照合。O2・O3両方PASS。
- `xyo-rt001-yield-register-contract-smoke`：有効R13/R14 witnessと12種類の偽造証拠拒否、PASS。
- `xyo-rt001-yield-helper-catalog-smoke`：Codegen 25種類+rooted reporterの一覧照合、PASS。
- `xyo-rt001-yield-wait-unmapped-smoke`：Eager/Lazy要求 × 通常待機/イベント待機、4ケースPASS。
- `xyo-rt001-yield-edge-stackmap-smoke`：O3 IR＋4種の未対応ケース、PASS。
- 既存Eager/Lazy・fast yield・CFI／Machine ELF testsを含め、隔離CMake（`/home/kicky/.rt001-orc-fde-cmake`）で**関連CTest 9/9 PASS**。ログ`build/rt001-yield-edge/ctest-register-final.log`。
- 実コードは`-DXYO_RT001_ORC_FDE_PLUGIN=ON`の隔離xyoを使用。yield-edge診断自体は実行時フラグOFFで標準実行経路に入らない。

### 2026-11-11 最終再検証

- 更新後の全ソースを隔離`/home/kicky/.rt001-orc-fde-cmake`でコンパイル・リンク成功。
- `XYO_RT_YIELD_EDGE_STACKMAP_PROBE=1`で実際のネイティブ復帰・安全性拒否差分回帰 **955/955 PASS**。ログ：`build/rt001-yield-edge/full-register-census.log`。二重Fiber・SSA・GC延期・不正な再開記録の拒否を含む。
- yield-edge/CFI/レジスタ/待機ヘルパ関連CTestはLLVM O3/O2いずれも **9/9 PASS**。ログ：`build/rt001-yield-edge/ctest-register-final.log`、`build/rt001-yield-edge/ctest-register-o2.log`。
- 全リポジトリのCTestは未実施。既存のGC許可判定は一切変更していない。コミット・pushなし。

## なお残る未証明事項

1. 今回比較したのは**直接のyield呼び出しが引数として持つ1つのポインタ**のみ。Scratch`Value`の`i64`値、native string/list、spill済みGC参照の完全ルート性を証明していない。
2. `xyo_wait_yield`や非同期レポータ等はヘルパ内部で中断し、通常のyield-edgeとは異なる呼出し先のCFIを経由する。必要な中断地点の全集合と各ヘルパ内のルートを整理する必要がある。
3. 失効したコードやStackMap、異なるLLVM最適化段階を混ぜてレジスタの安全性を判断してはならない。
4. すべてのルートと機械位置が証明されるまでは、`FiberPool::unsafeCount()`によるnative FiberのGC保護を維持する。

本作業では他の開発者が触れたソースを巻き戻さず、変更は実験用診断・テスト・文書のみに限定した。共有ツリーへのコミット・pushは行わない。
