# RT-001：ネイティブ変数スロットの安全な再結合（2026-11-10）

## 結果
実験フラグ XYO_RT_GUARD_NATIVE_VAR_SLOTS=1 を追加。実行中のLLVM Fiberをyieldによるスケジューラ境界で中断し、TargetのScratch変数をdeleteVariable後に再作成して値200に設定してから復帰させた。2400回のループは正しく完了。実例では編集時y=518、最終x=2082、y=2400で、x=200+(2400-518)と一致した。

既存ネイティブ復帰・fail-closed回帰は従来951から953実行へ拡張し、953/953 PASS。Guardモードの通常変数操作と2-Fiber共有変数/Unicode計算がInterpreterと一致。

## 実装
- src/jit/rt.cpp / jit.hpp：xyo_varslot_checkedというC ABIランタイムヘルパを追加。JitCtx/Threadを受け、毎回現在のTargetへID・名前・リスト種別でlookupOrCreateを行ってVariable*を再解決。削除済みの生ポインタを逆参照せず、Thread::jitVarSlotsとJitCtx::varSlotDataを更新。実験モード中は従来のxyo_jitVarキャッシュも迂回する。
- src/jit/codegen.cpp：実験モードでは直接のvarSlotDataスロットロードを生成せず、副作用ありの外部xyo_varslot_checked呼出しを各変数アクセスに生成。hoistVarSlotPointers、beginLoopPromotion、list-pointer巻き上げ用メタデータをguardモードだけ無効化してyieldを跨ぐ古いVariable*の保持を防ぐ。
- src/jit/backend_llvm.cpp：ORC JITの明示ランタイムシンボル表にxyo_varslot_checkedを追加。未登録時には実際にJIT session symbols not foundが出たが修正し再テスト。
- src/jit/nojit_stub.cpp：非JIT用の到達不能スタブを追加。
- src/engine/sequencer.cpp：構造変更がthread-safeでないVariableMapの保護のため、guardモードではXYO_PAR=1が明示指定されても並列workerを使わない。実テスト内でSequencer::parallelEnabled()==falseを検証。
- Thread/JitCtx/Gcのオブジェクトレイアウトやnative SSA capture ABIの変更なし。GCのnative Fiber sweep禁止は緩和しない。

## 新規・拡張テスト
- tests/platform/rt_jitctx_varslot_gc_root_smoke.cpp：実Runtime/Target/Thread/JitCtxのスロット所有権、Runtime::markRootsによるGCヘッダへの正確なmark、rehash・削除・再作成・再削除、リスト変数、ステージ変数とスプライトローカル変数の同名・同IDシャドーおよび復帰を確認。guardフラグOFF/ON両モードで2/2 PASS。
- tests/platform/rt_native_varslot_editor_rebind_smoke.cpp：本物のLLVM Native FiberをEager JITで起動。短いスケジューラtickにより実際のFiber中断を確認してから、ホスト側でv_xを削除・再作成し値200を設定。再開後にy=2400、x=200+(2400-y_at_edit)を確認。XYO_PAR=1も指定し、guardによるserial強制を検証。PASS。
- tests/differential/rt_native_varslot_editor_rebind.py：上記テスト用SB3を生成しテスト実行ファイルを起動。CTest相当の単独実行PASS。
- tests/differential/native_simple_resume.py：fixture varslot_guard_editor_mutation と、guardモードでの2件のJIT/Interpreter一致試験を追加。全953/953 PASS。
- tests/CMakeLists.txt：新規CTest xyo-rt001-native-varslot-editor-rebind-smokeを登録。ただし共有作業ツリーのCMake再生成と全CTestは未実施。

## 実行と検証
隔離LLVM23バイナリ：
/home/kicky/.rt001-guarded-varslot-serial-isolated/xyo
SHA256 f8d4138aaff51ada804a0507c742ae922e72cf60edd203197bf114da49cbe0b2

全回帰ログ：build/rt001-guarded-varslot-serial-final.log （953/953 PASS）。
Python構文検査と対象ファイルのgit diff --checkもPASS。
既存の検証済みLLVM23アーカイブをコピーし、codegen.cpp、rt.cpp、backend_llvm.cpp、sequencer.cppのみ再コンパイル・置換・再リンクした隔離ビルド。共有ツリーの全ソースを再コンパイルしたものではない。
commit/pushは行っていない。他担当の未コミット変更は保持。

## 注意と今後
このモードは安全性検証用で、アクセスのたびの再解決と最適化の一部停止により遅くなる。次の段階として、Target::variableBindingStampの世代を保持して一致時のみ既存スロットを利用する高速経路を検討できるが、ABA再使用・シャドー・クローン寿命・編集時点の検証が必要。

現時点ではエディタ側の構造変更がシリアルなFiber中断点で起きる場合に限る。未同期の外部ホストスレッドやネイティブFiber内で再入する編集コールバックへの安全性は証明していない。変数の所有権は一部検証できてもLLVM最適化後の生存SSA参照やGC管理String/Listの一時値をすべてルート公開した証明にはならないため、途中のNative Fiberに対するGC回収禁止は続ける。通常の高速JITは既定動作のまま。
