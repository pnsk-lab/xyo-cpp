# Multi-Agent Board — xyo-cpp

[運用手順と投稿コマンド](board/README.md)

タスク状態の正本は board/tasks/ 以下です。この一覧の状態は自動更新されません。
リアルタイムの担当・進捗は次のコマンドで確認してください。

    bun tools/board.ts list

| ID | 優先度 | タスク | 初期状態 |
|---|---|---|---|
| [IR-001](board/tasks/IR-001.md) | P0 | 共通 CallFrame / Return PC | open / unassigned |
| [RT-001](board/tasks/RT-001.md) | P0 | yield / deopt 継続状態の統合 | open / unassigned |
| [IR-002](board/tasks/IR-002.md) | P0 | 共通Effect / ランタイム関数ABI | open / unassigned |
| [VER-001](board/tasks/VER-001.md) | P0 | LLVM / WASM / Binaryen パリティハーネス | open / unassigned |
| [IR-003](board/tasks/IR-003.md) | P1 | Scratch命令の共通CFG lowering拡張 | open / unassigned |
| [PERF-001](board/tasks/PERF-001.md) | P1 | 共通IRの性能・メモリ評価 | open / unassigned |

## エージェントへの開始指示

1. AGENTS.md と board/README.md を読む。
2. タスクをclaimして、自分のagent ID・編集予定ファイル・方針を投稿する。
3. 他のタスクと競合するファイルは作業前に合意する。
4. 完了時に差分・テストログ・未対応事項を記録しreviewに移す。

全体連絡: board/messages/general/ （bun tools/board.ts post general ...）

スコープは共通IR/実行ABIの統合。個々のBinaryen専用パッチより、LLVMと
Binaryenで共通利用できる実装と厳格なパリティ検証を優先する。
