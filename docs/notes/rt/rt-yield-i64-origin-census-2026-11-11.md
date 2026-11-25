# RT-001：yield時に生存するi64 SSA値の出所分類（2026-11-11）

## 結論

最適化後のLLVM SSAに対し、実際のORC ELFの `.llvm_stackmaps` の各i64 locationと、その定義元のSSA命令を1対1で照合する出所分類を追加した。これは**GC参照としての所有権証明ではない**。一部の出所が既知の文字列生成や手続き引数であることは、値がGCによりマーク済み、または停止中Fiberから復元可能であることを意味しない。

実行中は `XYO_RT_YIELD_EDGE_STACKMAP_PROBE=1`、`XYO_RT_YIELD_LIVE_SSA_STACKMAP=1`、`XYO_RT_YIELD_SSA_FILTER=i64_no_tick`、`XYO_RT_YIELD_SSA_TRACE_OPERANDS=1` を指定。通常実行ではこれらは無効。

### 実測（Eager・Lazyとも同数）

実Scratchプロジェクト `ssa_budget_cross_fiber_two_hats` のそれぞれ2手続き×2 yield地点＝4地点。

| LLVM出所 | i64 locations | 扱い |
| --- | ---: | --- |
| 手続きi64引数からのビット不変別名 | 4 | `candidate_argument_alias`、タグと所有権は未検証 |
| 実ホスト`xyo_join`などの生成結果 | 4 | `candidate_generated_string`、寿命未検証 |
| i1/i32→i64のゼロ拡張 | 28 | `no_tagged_string_zext`。高位32bitが0なので文字列タグ`0x7FF9`を構築不可。ただし**生ポインタとしての安全性は未証明** |
| LLVM関数のローカル`alloca`からのi64ロード | 90 | `unknown_load_stack`。アドレスは現在のネイティブフレーム内と追跡できるが、ロード値はboxed Valueや管理参照を含む可能性あり |
| LLVM PHI / Selectの合流値 | 16 | `unknown_merge`、全分岐の値源は未証明 |
| double→i64 bitcast | 12 | `unknown_float_bits`。NaNビットなどがあり数値であると早計に扱わない |
| i64ビット演算（and） | 2 | `unknown_bit_arithmetic`。参照タグを維持しないか未証明 |
| **合計** | **156** | **全件所有権未証明、gc_safe=0** |

同じ4地点についてi64候補のうちLLVM scheduler `yieldCheckN` tickは既に構造的に同定して診断対象から除外されている。今回の変更は**i64 locationを一切追加・削除せず、出所メタデータのみ追加**する。

## 実装

- `src/ir/rt_yield_ssa_machine_stackmap.hpp`：`classifyYieldI64Origin(value, context, varSlotDataOffset, variableValueOffset, yieldCheckOffset)`を追加。既存の厳格な`sameBoxedArgumentRoot`、`trustedGeneratedStringSource`、`classifyNativeVariableValueLoad`、`trustedAsyncReporterValueSource`を利用し、いずれも証明できなければ保守的にunknownへ分類。LLVMの`ZExtInst`は元ビット幅が32bit以下に限り`no_tagged_string_zext`。根拠は`src/core/value.hpp`の `val::kTagStr=0x7FF9000000000000`。未知のi64 Loadは、その**アドレス**に対する`classifyPointerRegion`のみで `unknown_load_stack/context/static/unresolved` を区別。ロードされた**値**の由来ではない。
- `src/jit/backend_llvm.cpp`：`offsetof(JitCtx,varSlotData)`と`offsetof(Variable,value)`を常に実際のABIから渡す。`XYO_RT_YIELD_SSA_TRACE_OPERANDS=1`の場合のみ`[rt001-yield-ssa-origin]`診断を各StackMapオペランドに出力。生のGCポインタは記録せず、所有権も許可しない。
- `tests/platform/rt_yield_ssa_machine_stackmap_smoke.cpp`：LLVM単体で正しい手続き引数、結合、変数スロット、checked resolver、レポータを認識し、偽のヘルパ名・オフセット・整数ビット演算・任意ロードを不正に昇格させない。i1/i32ゼロ拡張とi48ゼロ拡張・符号拡張、double bitcastを区別。LLVM verifierも実行。
- `tests/differential/rt_yield_i64_origin_census.py`：Eager・Lazy双方の実Scratch結果をInterpreterと比較し、4つのStackMap ID、全156件のlocation番号、SSA IDとopcode、出所、実ELF machine location数の全件同一性を検査。出所欠落・重複・source ID偽装・別JIT tier混入・偽のGC許可・machine record欠落など**8種類の改変証拠**を拒否。
- `tests/CMakeLists.txt`：実Eager/Lazy検証を`xyo-rt001-yield-i64-origin-census-smoke`として追加。

## 試験

- 更新後の隔離CMake環境：`/home/kicky/.rt001-orc-fde-cmake/xyo`、LLVM23、Linux x86-64。
- Eager実ORC ELF：4/4 yield sites、156/156 i64位置の出所照合PASS。
- Lazy実ORC ELF：4/4 yield sites、156/156 PASS。
- 改変された証拠8種：Eager・Lazyともすべて拒否。
- 関連CTest：LLVM O3 **7/7 PASS**、O2 **7/7 PASS**。`build/rt001-i64-origins/ctest-o3.log`および`ctest-o2.log`。
- 個別生成ログ：`build/rt001-i64-origins/final-eager-lazy.log`、opcode別内訳は`final-eager-lazy/eager/opcode-breakdown.txt`。
- ネイティブ復帰・fail-closed 955件は `i64_no_tick` かつsource originログONで実行中（完了後に結果追記）。

## 未解決

1. 90件の`unknown_load_stack`の保存元の`store`チェーンを追跡し、現在のScratch`Value`が元のどの変数・文字列・一時値に由来するか（phi経由、mem2reg失敗等）を正確に知る必要がある。スタックアドレスは所有権証明ではない。
2. 16件の合流値について、すべてのPHI/Select incomingがboxed安全値であるか、任意の管理参照が含まれうるかを分岐ごとに解析する必要がある。
3. 生の文字列・リスト参照、浮動小数点値、async helper内部の停止点の網羅、およびJIT callee-save機械レジスタの寿命証明。
4. **Native Fiberが生存中のGCは禁止を維持する**。GC gateは変更していない。コミット・pushなし。全リポジトリCTestは未実施。
