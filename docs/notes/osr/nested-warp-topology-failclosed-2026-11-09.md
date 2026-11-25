# OSR-015 — ネイティブ yield 中の制御グラフ変更を fail-closed で拒否する検証

2026-11-09

## 対象

OSR-014 は4階層 counted repeat に対しネイティブ Fiber yield で4つの source counter を保存し Interpreter のループフレームへ復帰する。OSR-015 は、ソース編集によって保存済みフレームの制御ノードが現在のプログラムから到達不能になった場合を検証する。正解は正常継続ではなく、古い LLVM も Interpreter も再生せずにスレッドを安全に終了する fail-closed。

## 変更するソース経路

実行中の LLVM Fiber が位相4の最深部 yield で停止した後、CompiledScript にフリーズ済みの source ID を使って3組の親子関係を個別に切断する。外側→中間、中間→深部、深部→最深部の3つを、各々最初と4回目の native yield で試す。

具体的には、子 repeat の直前にある data_setvariableto の next を空にしてその repeat を source chain から切り離す。Blocks::resetCache() でソース改訂し、revision 増加、ScratchCompilationUnit::capture の有効性、immutable source digest の変化を確認する。旧ノードと確定済み変数副作用は破壊しない。

## 正確性の不変条件

- 編集直前の native committed sum は正で、予定加算総量に未到達
- 終了時 sum は committed sum と厳密一致。古い LLVM の続行・Interpreter での実行済み効果の再生は禁止
- branchCount は元の型付き文字列0のまま。親の後続処理や root done は実行しない
- Runtime のスレッドが終了し、危険なフレームを復帰成功と認めない
- 計数 ON の編集後ブロック数は、編集なし Interpreter 全実行より厳密に少ない
- 同じ正常ソースで native OSR が実際に起きる positive control を確認してから実施

## テスト

- tests/platform/nested_warp_topology_edit_failclosed_smoke.cpp — 実 Runtime::step と停止済み Fiber でソースを切断
- tests/differential/jit_nested_warp_topology_failclosed.py — 3か所×最初/4回目 yield の6ケース
- tests/CMakeLists.txt — xyo-nested-warp-topology-failclosed と xyo-nested-warp-topology-failclosed-fast

再現: ctest --test-dir board/.osr-silent-edit-headless -R '^xyo-nested-warp-topology-failclosed(-fast)?$' --output-on-failure

短い入力では非同期 LLVM コンパイルより先に処理が終了することがあるため、outer repeat を40,000から100,000へ増やした。40,000版は計数ON 6/6 PASS、計数OFF 5/6 PASS（最後は LLVM 移行前に終了）。100,000版の計数OFFは6/6 PASS。最新CTestは両モードとも100,000で検証する。

## 制限

シリアルなエディタ操作を native Fiber が停止した safe point で行う。別スレッドからの並行書換え、SSA・副作用の途中位置、別の意味論へ安全に継続する機能は対象外。OSR-014 / IR-001 / RT-001 の製品コードは変更せず、コミット・プッシュなし。

## 最終結果

- CTest xyo-nested-warp-topology-failclosed: **PASS（75.51秒）**。最初と4回目の実ネイティブyield、3箇所のNEXT切断、合計6ケース。
- CTest xyo-nested-warp-topology-failclosed-fast: **PASS（76.90秒）**。ブロック計数なし、同じ6ケース。
- いずれもソースrevisionとimmutable digestの変更、確定sumの完全保持、旧LLVMの実行拒否、危険なInterpreterフレームの実行拒否、root suffix未完了を確認。
- テストに用いたフリーズ済み実行ファイル: board/osr-topology-failclosed-verified-smoke。SHA256: 51805c447206b590a2aed2f2ec6a672e8e130ee5e01e8abf486930b5fa0f0dac。正式CTestで使用した実行ファイルとハッシュ一致。
- 最新GUI ParityとOSR全体の一括回帰はこのタスクでは未実施。OSR-014は並行開発中であり、同担当の製品コードは変更していない。
