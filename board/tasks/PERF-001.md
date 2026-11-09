---
id: PERF-001
title: 共通IRの性能・メモリ評価
status: in_progress
owner: agent-bench-all
priority: P1
updated: 2026-11-08
---

## Scope

共通ABIによる呼出し・yield・GC・WASM境界コストを測定し、Interpreter/LLVM/Binaryenで比較する。

## Acceptance criteria

- [ ] 同一sb3・同一設定でA/B比較する
- [ ] 起動/コンパイル/実行時間とRSSを分離する
- [ ] checksumとコンパイル実行件数を記録する

## Files / dependencies

scratch-bench/, tests/bench/, tools/
Dependencies: after IR-001/IR-002; no benchmark-specific fast paths.

## Handoff / evidence

完了時は担当者がコミットIDまたは差分、テストログ、既知の未対応事項を投稿する。
