# Interpreter optimization follow-up — 2026-11-03

## 実装

1. src/blocks/core.cpp: operator_contains 内で文字列を小文字化して2つの SStr を毎回新規確保していた処理を削除し、既存の rt::strContains に直接渡す。Unicodeの変換とASCII用の高速検索は共通実装を使う。
2. src/model/blocks.{hpp,cpp}: BlockCached::parentValueSlot を追加。通常の reporter 値の書き込み先をキャッシュ構築時に特定する。std::map のノードは後からキーを追加しても移動しない。BROADCAST_INPUT の IdName 経路はそのまま。実行関数のない入力には新規キーを生成しない。
3. src/engine/execute.cpp: 通常実行、非同期再開、既報告値の復元で書き込み関数を共通化する。

## 実測

環境: ローカル Linux、既存 ScratchBench performance.sb3、インタプリタ専用、30fps。性能比較は時間依存のscheduler 8項目を除く39項目。

- operator_contains 単独: str_contains 226.772 → 957.237 ops/ms (**4.22倍**)。
- slotキャッシュ追加: slot → baseline → baseline → slot の交互 A/B。全39項目でチェックサム一致、幾何平均1.0983倍。**38/39項目が改善**。math_compare 約1.20倍、math_arith 約1.15倍、app_raycast 約1.14倍。
- Scratch VM インタプリタに対して、最終版は幾何平均 **1.268倍**、38/39項目で高速。以前の版は1.138倍、32/39項目。Scratch比較は単一の過去実行の値なので僅差は有意ではない。

## 回帰検証

- CTest 6/6 PASS。
- ScratchBench非スケジューラ39項目: checksum 39/39一致。
- インタプリタ GUI Parity: 2121/2123。失敗した Block Yield Tests #8 と Data: Name Collisions #6 は、変更前バイナリでも同じ。変更前バイナリはさらに Motion: If On Edge Bounce #2 に失敗（並行して進行した別作業の修正の影響）。本作業による新しい失敗は観測されず。
- git diff --check PASS。
- 他の未コミット変更を保存。今回はcommit/pushしていない。

## ログ

- build/scratchbench-interp-final-20261103.log
- build/scratchbench-interp-contains-20261103.log
- build/interp-ab-{base,slot}-{1,2}.log
- build/interp-slot-ctest-20261103.log
- build/interp-slot-parity-20261103.log
- build/interp-contains-baseline-parity-20261103.log
- build/scratchbench-scratch-interp-20261103.log
