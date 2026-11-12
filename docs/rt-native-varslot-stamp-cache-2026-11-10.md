# RT-001：世代付き Variable* キャッシュとネイティブ Fiber 再結合（2026-11-10）

## 目的

前段階の XYO_RT_GUARD_NATIVE_VAR_SLOTS=1 は、生成LLVMが Scratch 変数を使用するたびに xyo_varslot_checked() を呼び、Target::lookupOrCreateVariable / lookupOrCreateList によって生の Variable* を新たに解決する保守的モードだった。エディタが実際のネイティブFiber中断点で Target::variables から削除・再作成するケースは通ったが、全参照で文字列ID検索するため遅い。

今回、二重の世代検査で同じ所有権のままならキャッシュを再利用する追加実験モードを実装した。

設定：
- XYO_RT_GUARD_NATIVE_VAR_SLOTS=1：保護付き外部ヘルパをLLVM各変数参照へ生成。既定OFF。
- XYO_RT_GUARD_NATIVE_VAR_SLOTS_FAST=1：上記のガードモードでだけ、世代付きキャッシュを使用。既定OFF。FASTだけ指定しても通常JIT動作は変更しない。

**デフォルトの高速JIT、ネイティブGCのsweep禁止、既存JitCtxおよびSSA capture ABIは変更しない。**

## 実装

### 世代付きキャッシュ

src/engine/thread.hpp の既存JIT可視フィールドの末尾に、ネイティブ保護用の所有ターゲット、ステージターゲット、ローカル/ステージのVariableMap世代、validビット、キャッシュのhit/miss/失効回数を追記した。

src/jit/rt.cpp で resolveGuardedCachedVariable と refreshNativeVarCacheStamp を追加した。xyo_varslot_checked / xyo_jitVar は guard+fast 併用時に次を守る：

1. Scratchの現在の Target::variableBindingStamp() を得る。これはローカル世代、ステージ世代、ステージ所有者を含む。
2. キャッシュの Target identity、stage identity、localGeneration、stageGeneration **全て一致する場合にのみ**既存の JitVarCacheKey(idポインタ、nameポインタ、リスト種別) から Variable* を取得する。
3. どれか変化した場合は **一度も古いVariable*を逆参照せずに** jitVarCache.clear() でキャッシュを捨て、現在のstampを確保する。VariableMapの世代はグローバル一意なので同一アドレスのerase/recreate（ABA）でも失効する。
4. 未キャッシュのキーは現在のTargetから通常解決する。Scratch変数が存在せずlookupOrCreateで挿入された場合は **挿入後のstampを再取得** し、旧世代の他のキーを保守的に無効化してから新しい Variable* を挿入する。
5. xyo_varslot_checked は戻り値を Thread::jitVarSlots と JitCtx::varSlotData に更新する。生成LLVMは毎回副作用ありの外部ヘルパを呼ぶため、LLVMのCSE/LICMで古いVariable*を繰り返し使わない。

Thread::releaseFinished() は所有者と世代・回数を初期状態に戻す。他の既存のJitCtxフィールドのオフセットは変更していない。ただし Thread 自体の sizeof は増えるため、隔離バイナリでは Threadをnew/stack上に作成する runtime.cpp、hot_start.cpp、jit.cpp と thread.cpp を再コンパイルした。既存の静的ライブラリ内のコンパイル済みThread生成関数だけを流用することはヒープ境界違反になるので不可。

### Yieldをまたぐポインタ保持の監査

ガードモードでは既に hoistVarSlotPointers() と beginLoopPromotion() を無効にしていたが、追加監査でさらに：
- emitTransientRepeat / emitTransientMultiRepeat はループ開始時に varValuePtr を保持し、yield後に使用するため無効。
- emitWarpInduction は誘導変数と境界の Variable::value アドレスを emitSsaChunkScheduler() のyieldをまたいで保持するため無効。
- GPUループ、および par-for のworker経路を無効（Target::variables は並列編集と互換でない）。
- ループ不変pen setterの移動を無効。エディタ変更で不変と判断したreporterの実体が変わる危険を除く。

いずれも保護付きJITモード限定で、通常のJIT最適化には影響しない。スケジューラ本体も保護付きモードでは XYO_PAR=1 指定下でもワーカー並列を無効化する。

## 独立した実行テスト

- tests/platform/rt_jitctx_varslot_gc_root_smoke.cpp：Runtime / Target / Thread / JitCtxの実体を用いる。Variable::valueのGC管理文字列が Runtime::markRoots でマークされる。rehash、変数erase/recreate、リストerase/recreate、ステージ変数のローカルシャドーとシャドー解除、**ステージ自体でのerase/recreate**まで検査する。世代キャッシュのhit/miss/失効件数を直接観察。fast OFF/ON 2/2 PASS。
- tests/platform/rt_native_varslot_editor_rebind_smoke.cpp：Eager LLVM JITで実際に2400回のnonwarp Scratch変数更新を走らせ、Fiberがサスペンドしたことを確認してからホストがv_xをdeleteVariable→recreate、value=200に変更。再開後にy=2400、x=200+(2400-y_at_edit)を検証。FASTモードではコンパイル済みコードが事前にnativeVarCacheHitsとMissesを増やしたことをassert。FAST OFF/ONともPASS。
- tests/differential/rt_native_varslot_editor_rebind.py：--fastを指定できるようにし、独立プロセスの環境変数を制御。tests/CMakeLists.txtに既存通常ガード版と新FAST版の2つのCTestを登録。今回の直接実行はPASS。CMake全体再生成・CTest全体の実行は未実施。
- tests/differential/native_simple_resume.py：フラグ制御を追加し、通常のvariable editor fixtureと2 concurrent native Fiber Unicode fixtureの双方で、slow guardおよびfast stamped guardのInterpreter等価性をそれぞれ独立プロセスで検査。既存の951実行に計4実行追加。

## 実測：ネイティブヘルパの呼出し性能

実際のRuntime + Stage + Sprite + JitCtx、4つの実変数を用いる独立C++プログラムを隔離ディレクトリ /home/kicky/.rt001-varstamp-fast-isolated/varstamp_bench.cpp に作成した。

guard slowおよびguard fastを別プロセスで実行、各プロセス120万回の連続変数解決、5組の交互A/Bで計測。

- 毎回Targetを検索：中央値23.298ns/call
- stamped cacheを利用：中央値8.647ns/call
- 呼出し速度比：2.694倍
- FASTモードでは120万callとウォームアップ2万callの合計でhit=1,219,997、miss=7、invalidations=4。最初の変数挿入時はstampが変化するため失効回数が増える。steady-stateでは毎回のID文字列解決を回避できる。

注意：これは **Variable*解決ヘルパ単体**のmicrobenchmark。LLVMコード生成・GC・Scratchスケジューラ全体の性能比ではなく、CPU固定や統計的有意性の検定も未実施。Guardedモードは他のLLVM最適化を停止しているため、通常JITより高速とは限らない。

## 制限

今回の安全条件はシリアルなScratchスケジューラの安全なyield境界においてホストが構造変更を実施すること。呼び出し中にホストが別スレッドから未同期でTargetを編集すること、変数アドレスを取得してから再入コールバックが構造を変更することまでは許可しない。LLVMの他の特殊経路については追加の静的監査が必要。

変数所有権の検証によりGC管理の全てのLLVM live SSAや機械レジスタの一時値のroot所有が証明されるわけではない。従来のネイティブFiber GC延期は維持する。ソースBlockの動的変更に対するdeoptとは別課題。

コミット・pushなし。他担当の共有ツリー編集は保持。

## 最終検証

ネイティブ差分フルテスト：build/rt001-varstamp-fast-full.log（**955/955 PASS**）。従来951 + ガードslow通常/2-Fiber 2件 + fast世代付き通常/2-Fiber 2件をすべて検査。

隔離LLVM23 CLI：/home/kicky/.rt001-varstamp-fast-isolated/xyo。既存の検証済みstatic archiveのコピーに、rt.cpp、codegen.cpp、thread.cpp、runtime.cpp、hot_start.cpp、jit.cppを現在のソースから差し替え、共有作業ツリーで同時変更中のTranslationService ABIにも対応するためtranslate_service.cppを再コンパイル。現在の全ソースのクリーンリビルドではない。


検証結果（2026-11-10）：
- 実ネイティブFiberのエディタ変数削除・再作成試験：guard slow、guard fastともPASS。スケジューラの並列実行要求 XYO_PAR=1 でもguard側のserialバリアを検証。スローモード実例はy_edit=554、x=2046、y=2400、ファストモード実例はy_edit=652、x=1948、y=2400。
- Runtime / GC / stage shadow変数所有権smoke：slow/fast双方でPASS。FASTのhit>0/miss>0、ローカル変数のerase/recreateによる invalidations増加に加え、ステージマップのerase/recreateだけでスプライト側のキャッシュが失効することを直接確認。
- LLVM23ネイティブ復帰と失敗クローズの差分回帰：955/955 PASS、build/rt001-varstamp-fast-full.log。
- tests/differential/native_simple_resume.py と rt_native_varslot_editor_rebind.py のpython3 -m py_compile：PASS。
- git diff --check 対象コード・テスト：PASS。
- 隔離CLI SHA256：a6ca647d613f1b364565f79d137f3dc370f26ed3344e168490884e74872b5bbf。
- ベンチマークの10プロセス実測ログは隔離ベンチスクリプト実行結果に記録。slowの中央値23.298ns、fastの中央値8.647ns、2.694倍。
