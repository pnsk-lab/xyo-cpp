---
title: アーキテクチャ
---

xyo-cpp は SB3 ローダ、ブロックモデル、IR、中間表現最適化、インタプリタ、LLVM ORC JIT、共通ランタイム、ファイバスケジューラから構成されます。

```text
SB3 → Loader → Blocks → IR
                       ├→ Interpreter ─┐
                       └→ LLVM JIT ────┤
                                      ↓
                             Runtime helpers
                                      ↓
                            Sequencer / Fibers
                                      ↓
                               Render / Audio
```

独立したスクリプトを依存解析により並列実行する仕組みも備えます。詳しくは[設計書](/xyo-cpp/reference/design/)を参照してください。
