# RT-001: Native LLVM SSA → JitCtxフィールドの正確な経路とGCルート所有権（2026-11-10）

## 対象と結論

2つの緑の旗スクリプトが独立にJIT非warp手続きを動かす`ssa_budget_cross_fiber_two_hats`について、前回のLLVMポインタロード追跡を**実際の`JitCtx`のフィールドオフセット**まで拡張した。

- チェックポイント1: LLVM live不明ポインタ6/21件のすべてが `JitCtx::varSlotData` を起点とする2段ロード。実オフセットは96バイト。
- チェックポイント2: 6/7件が同じ `JitCtx::varSlotData` 起点の2段ロード。残る1/7件が `JitCtx::sampledWarpClockUs` を起点とする1段ロード。オフセット120バイト。
- 両スクリプトで結果は一致し、最終状態`y=48005`はInterpreterと一致。

**これをもって生存中FiberのGCを許可してはいけない。** フィールドの名前・アドレスはその中身にあるGC-managed `Value`の完全なルート証明とは異なる。実験用の `Gc::collect()` の`FiberPool::unsafeCount()>0`に対するsweep禁止は継続する。

## 実装内容

`src/jit/rt_ssa_extra_capture.hpp`:

- `constantHostContextOffset(address, trustedContext)`を追加し、LLVMの単一インデックス定数GEPとBitCastだけを最大64段遡る。GEPの要素型が`i8`なら1バイト単位、`ptr`なら`sizeof(void*)`単位でオフセットを計算する。
- GEPインデックスが負/動的、構造体型や想定外の要素型、多次元GEP、加算オーバーフロー、循環・深すぎる連鎖、`inttoptr`などは`std::nullopt`にする。事前にポインタがJitCtxの第1 ABI引数であることは既存`analyzeExtraLiveSsa`側でチェック済み。
- `tracePointerLoadChain()`が検出した「逆順で最後のLoad＝ABI起点から最初のLoad」のアドレスにだけこの関数を適用し、`PointerLoadChain.hostContextFieldOffset`に保存。コンパイラ診断専用の数値で、実行時ポインタ/GCアドレスは出力しない。

`src/jit/codegen.cpp`:

- `offsetof(JitCtx, field)`で実ABIのフィールド開始位置を照合し、完全一致したフィールド名のみを`ctx_field`として表示。
- Thread/Target/Runtime/Sequencer/Fiber/args/compatEc/varSlotData/joinRefs/sample時刻/ブロックカウンターなどのポインタフィールドを対象とし、未知・内部オフセットは「unresolved」「interior_or_unmapped」で拒否。元の`[rt001-ssa-pointer-load-chain]`ログに`ctx_field`と`ctx_offset`を追加。
- GCルート・ソース復帰の認可ロジック、LLVM SSA→Runtime 17引数のABI、実際のFiber/GC処理は変更していない。

`tests/platform/rt_pointer_region_smoke.cpp`:

- LLVM23の直接IRBuilderで実際のJitCtx第1ポインタ引数から、定数`i8` GEPおよび`ptr` GEPを追跡するテスト。
- 2段/3段ロード、`hostContextFieldOffset==8`、スタック由来はnull、動的GEP・負インデックス・未知要素型・偽のtrustedContextを拒否する。既存の原因別分類・GC安全条件の不変性も継続。

`tests/differential/native_simple_resume.py`:

- 実Scratchで未知ロードごとの原因分類、LLVM命令ID多重集合、ロード回数とアドレス派生回数、`ctx_field`および非ゼロオフセットを照合。
- 2段ロードは`varSlotData`、1段ロードは`sampledWarpClockUs`、各フィールドの実オフセットが一致することを検査。すべて`gc_safe=0`で、生存中FiberがあるとGC保留されることを確認する。

## 実際のフィールドの所有権調査

コードの実データフロー:

`JitCtx::varSlotData` → `Thread::jitVarSlots.data()` → `Variable*` → `Target::variables`内の`Variable` → `Variable::value`または`Variable::list`。

`xyo_varslot_init`は引数の`JitCtx::thread`の`jitVarSlots`に変数参照を格納し、`ctx->varSlotData=t->jitVarSlots.data()`へ更新する。LLVMコードの`Codegen::varSlot`は`offsetof(JitCtx,varSlotData)`からポインタ配列を取得し、`Variable*`をロードする。`Variable*`自体はGCオブジェクトではなく、C++コンテナ内の要素。`Runtime::markRoots`はその`Target::variables`全体を訪れ、各`Variable::value`を`Gc::markValue`、`Variable::list`を`Gc::markPtr`で保持する。

`JitCtx::sampledWarpClockUs`は`std::atomic<int64_t>*`で、スケジューラのワッチドッグ時計の値を参照する。GC管理のScratch `Value`ではない。

**重要なライフタイム障害条件:** `Target::variables`は世代付き`VariableMap`（`std::unordered_map`基底）。通常の挿入・rehashは既存要素参照を安定に保つが、`Target::deleteVariable`は要素を消去する。JITの`Thread::jitVarSlots`および`jitVarCache`に格納した`Variable*`は、ターゲットの変数削除時には自動的にclearされない。古いポインタを直接dereferenceするのはUAFの可能性がある。世代番号`Target::variableBindingStamp()`は削除・再作成前後で変化するので、再利用を許可するには必ず世代確認または再解決が必要。**この状態では中断中FiberのGCを安全と断定できない。**

`tests/platform/rt_jitctx_varslot_gc_root_smoke.cpp`（新規）:

- 実際の`Runtime`・`Target`・`Thread`・`JitCtx`・`xyo_varslot_init`を用いる。
- `varSlotData==thread.jitVarSlots.data()`と、`Variable*`が実際の`Target::variables`要素と同一であることを確認。
- `Variable::value`にGC管理`SStr`を置き、`Runtime::markRoots(gc)`を直接呼び出して**実際のGcHeaderのmarkフラグが0から1へ変わることを確認**。その後明示的GCを2回行い保持を確認。単なる「生存テスト」ではC++スタックの保守的スキャンで偶然保持される可能性があるため、独立に正確なroot-providerの到達性を確かめる。
- 80個の追加変数の挿入・rehashで既存map要素のアドレスは維持され、世代番号は変化することを確認。
- その変数を削除すると`jitVarSlots`内の生ポインタが残存するが、実マップには存在しなくなり、所有権世代は変化することを確認。**削除済みポインタは絶対にdereferenceしない。** 再作成後も世代番号が元に戻らず、ABAでの誤受理を防ぐ性質も確認。
- SSA実験フラグOFFとONの双方で実行し、いずれもPASS。

## 制限・次の調査

今回の分類はLLVM最適化**前**のIRのコンパイラ診断であり、`varSlotData`の指す生ポインタがエディタ上の変数削除・clone破棄・ランタイム編集の間も生存するという保証ではない。 `Target::variables`の構造変更に対するJITキャッシュの世代ガードが別途必要。さらにGC管理の文字列・リスト値がLLVMレジスタ・変数の一時コピーに保持されるケースを独立にルート公開する必要がある。

実験フラグは既定OFF。今回もGCのsweep条件を緩和していない。共有作業ツリーには他担当の未コミット変更があるため、RT-001所有の診断・テスト以外を変更せず、コミット・pushは行わない。

## 検証と実行方法

隔離LLVM23 CLI: `/home/kicky/.rt001-ctx-field-isolated/xyo`。前回確認済みのstatic libraryの隔離コピーに`codegen.cpp`のみ再コンパイルして同名アーカイブメンバーを置換、再リンクした。共有ツリー全体のCMake再ビルドは未実施。

- `/home/kicky/.rt001-ctx-field-isolated/rt_jitctx_varslot_gc_root_smoke`：実際のRuntime/Target/Thread/JitCtxを用いた単体テスト。フラグOFF/ONともPASS。
- LLVM23純粋IRテスト `build/rt001-pointer-region/rt-pointer-region-smoke`：PASS。
- ネイティブ差分回帰ログ `build/rt001-ctx-field-final.log`（**951/951 PASS**）。

### 最終検証記録

- 隔離バイナリ`/home/kicky/.rt001-ctx-field-isolated/xyo`のSHA-256は`f21056e181ea06b5425c0911f463f0572c13a6b8c185394ac3b4298826cb2ce8`。
- `python3 tests/differential/native_simple_resume.py --native /home/kicky/.rt001-ctx-field-isolated/xyo --out build/rt001-ctx-field-final`：**951/951 PASS**。ログ`build/rt001-ctx-field-final.log`。
- 新規の実Runtimeルート所有権smoke：`/home/kicky/.rt001-ctx-field-isolated/rt_jitctx_varslot_gc_root_smoke`。実験フラグOFF、ONの**2/2 PASS**。GCの正確なmarkフラグ、rehash、erase、世代番号もテスト。
- LLVM23の純粋IR smoke：`build/rt001-pointer-region/rt-pointer-region-smoke` PASS。動的・負数・想定外要素型のGEPでオフセット判定を拒否するテストも追加。
- `python3 -m py_compile tests/differential/native_simple_resume.py` PASS、`git diff --check`（対象実装・テスト）PASS。
- `tests/CMakeLists.txt`に`xyo-rt001-jitctx-varslot-gc-root-smoke`を追加。ただし**共有ツリー全体のCMake再構成とCTest全体実行は行っていない**。実テストは既存LLVM23の隔離static libraryと独立にコンパイル/リンクして検証した。
- コミット・pushなし。GC許可条件は変更なし。`Target::variables`から要素が削除された場合はキャッシュ済み`Variable*`をGC安全性の根拠として使用できない。
