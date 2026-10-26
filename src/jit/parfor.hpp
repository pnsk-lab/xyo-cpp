// xyo-cpp: 並列 append ループ（par-for）の検出。
//
//   repeat <n>
//     change <i> by <整数k>        （0 回以上・無条件）
//     add <式> to <L1>
//     ...
//
// この形は「反復ごとに各リストへちょうど 1 個ずつ append し、カウンタ <i> は
// 整数の等差」なので、**索引書き込みに privatize すると反復独立**になる。
// append 順 = 反復順なので、出来上がるリストは逐次実行と 1 要素も変わらない。
//
// JIT は本体を `(JitCtx*, iter, ctr, SList**, i64*)` の別関数へ emit し、
// ワーカープールで反復範囲を分割実行する（`xyo_par_for`）。カウンタは
// 反復ごとのローカルへ差し替え、ループ後に `init + trip*delta` を書き戻す。
//
// 判定は保守側: 本体に制御構文・手続き呼出し・カウンタ以外の変数書き込み・
// append 先リストの読み取りが 1 つでもあれば諦める（逐次ループへ）。
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace xyo {

struct Block;
struct BlockField;
struct Blocks;

namespace jit {

struct ParForPlan {
    const Block* loop = nullptr;
    Block* bodyHead = nullptr;
    /// 反復カウンタ（VARIABLE フィールド）。v1 は 1 本だけ。
    const BlockField* counterField = nullptr;
    /// 1 反復あたりのカウンタ増分（整数・1 以上）。
    double delta = 1.0;
    /// append 先リスト（VARIABLE ではなく LIST フィールド）。重複なし。
    std::vector<const BlockField*> lists;
};

/// `loop` が par-for 化できる `control_repeat` なら plan を返す。
/// 不可なら nullopt と理由（why 非 null のとき）。
std::optional<ParForPlan> analyzeParFor(Blocks& blocks, const Block* loop,
                                        std::string* why);

}  // namespace jit
}  // namespace xyo
