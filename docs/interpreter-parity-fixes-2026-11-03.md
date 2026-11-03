# インタプリタParity修正（2026-11-03）

## 結果

ScratchBench parity（2,123件）を xyo-cpp インタプリタ専用経路で実行し、2121/2123 → **2123/2123（100%）** に改善。

- Block Yield Tests #8: 修正
- Data: Name Collisions #6: 修正

## 原因と修正

### 1. イベント起動で実行コンテキストが破壊される

背景切替のプリミティブ内で Runtime::startHats() が同期的に別スレッドの execute() を呼び出す。従来の execute() は thread_local BlockUtility g_util に現在のスレッドを上書きし、呼び出し元に戻る際に復元しなかった。

これにより looks_switchbackdroptoandwait の yield が呼び出し元ではなく最後に起動した背景イベントのスレッドへ作用し、呼び出し元が待機せず次ブロックを実行した。

src/engine/execute.cpp に ScopedBlockUtility を追加。再入時に g_util の直前の設定を保存し、正常終了・早期returnのいずれでも復元する。

再現用SB3の観測値：修正前の「背景をblankにして待つ」直後の共有変数は0、期待値は46。修正後は全Parity合格。

### 2. broadcast と scalar のID衝突

SB3には stage.variables と stage.broadcasts の異なる名前空間に同じID（data_ping）が存在できる。src/load/sb3.cpp は両方を一つの unordered_map に emplace していたため、通常変数が先に存在するとbroadcast定義が失われていた。

結果として、ID指定のbroadcastは偶然通常変数を利用して動作したが、動的な名前指定 DATA_PING は broadcast の型フィルタで見つからず、受信回数が1（期待値2）になった。

同じIDがすでに存在するbroadcastのみ collidingBroadcasts に分離して保管。ID/名前両方の検索と、SB3保存・再読み込みを対応。クローン時の保持も補完。

## 回帰テスト

tests/differential/interpreter_parity_regressions.cpp に以下を追加：

- scalar/broadcastの同一IDを持つSB3の読み込み、型・値の維持、大小文字を無視する動的broadcast名の検索、保存後の再読み込み
- pending broadcastの後に switch backdrop and wait を実行し、別イベントの同期起動を経ても元のスレッドがyieldし、変数snapshotに受信結果が反映されること

tests/CMakeLists.txt に xyo-interpreter-parity-regressions を追加し、XYO_JIT_MODE=off でCTest対象にした。

## 検証ログ

- build/interp-parity-fixed-20261103.log: interpreter 2123/2123
- build/interp-parity-fix-hotspot-20261103.log: HotSpot 2123/2123
- build/interp-parity-fix-eager-20261103.log: Eager 2123/2123
- build/interp-parity-fix-ctest-20261103.log: 既存CTest 6/6
- build/interp-parity-fix-performance-20261103.log: 47項目完走、決定論的39項目のchecksum一致

修正はローカルに適用済み。既存の未コミット変更には触れず、commit/pushは実施していない。
