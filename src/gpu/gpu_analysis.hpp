// xyo-cpp: GPU 並列 VM バイトコードの CFG 解析。
// gpu_compile が emit したバイトコード列に対し、構造化構文ではなく
// 実ジャンプを辿る worklist 固定点解析で反復間独立性を証明する
// （早期 return・内側ループ・合流で飛ばされる書き込みを正確に扱う）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xyo::gpu {

struct ExecRequest;

/// CPU-only GL allocation plan. Zero hardware limits mean unspecified.
/// hostOutputBytes is the full legacy laneOut readback (capped at 512 MiB).
/// chunkBufferBytes includes fixed buffers and scratchBytes, but not host memory.
struct GlChunkPlan {
    bool ok = false;
    uint32_t lanesPerChunk = 0;
    uint64_t hostOutputBytes = 0;
    uint64_t chunkBufferBytes = 0;
};

/// Preserve a fitting single dispatch (512 MiB); otherwise use bounded chunks
/// (256 MiB including scratch reserve). forcedChunk is a maximum, not a minimum.
/// This checks sizes only; callers must still validate bytecode and pointers.
GlChunkPlan planGlChunks(const ExecRequest& req, uint64_t ssboLimit,
                         uint32_t groupLimit, uint32_t forcedChunk = 0,
                         bool allowChunks = true, uint64_t scratchBytes = 0);

/// コンパイル時の事前走査結果（slot id ではなく変数 id ベースで
/// 集めて compile 側が slot へ写像する）
struct BodyScan {
    std::vector<bool> slotWritten;   // 本体が書くスロット（emit 後に確定）
    bool writtenX = false, writtenY = false;
    bool writtenAttr[5] = {};        // 0=size 1=hue 2=sat 3=bri 4=transp
    bool hasPenOps = false;          // penUp/penDown が本体に存在
};

/// バイトコードの静的検証:
/// 未知 op / オペランド範囲 / ジャンプ先 / 合流・分岐で一致する
/// スタック高さ / 最大スタック <= 128。
bool validateBytecode(const uint32_t* code, size_t nInstr,
                      uint32_t nSlots, uint32_t nGlobals, uint32_t nWb,
                      std::string* why);

struct CfgResult {
    bool ok = false;
    std::string why;
    /// 書き込まれるが反復内で「読み取り前に必ず代入済み」を証明
    /// できなかったスロット — induction（ affine 増分）候補。
    std::vector<uint32_t> needsRecurrence;

};

/// lanes>0 の入口ペン状態は penUp 固定。carried penDown（pdSet で
/// ない時点の移動/打刻）を観測した場合、全到達 HALT の
/// carriedMayDown が偽（= 終端ペン状態が反復間 carried に依存
/// しない）ときのみ受理 — 終端 down のまま観測する body は、
/// 観測前に反復内でペン状態を上書き済みの場合に限り安全。

/// 反復間独立性の CFG 固定点解析。
/// initiallyDefined には induction 証明済みスロットを渡す
/// （preheader で初期化されるため入口で definite 扱い）。
/// - LOADV は definite スロットのみ
/// - POSX/POSY は「反復内設定」か「本体が一度も書かない（一様）」
/// - CHANGEX/Y は対応する位置定義必須
/// - 移動/PENDOWN で stroke が起こり得る点は位置（反復内設定 or
///   本体未書込みで一様）+ 属性（反復内設定 or 未書込みで一様 or
///   全終了経路で一様定数＝lanes>0 初期値として dispatch）を要求
/// - PENSIZED/PENCOLPB は対応属性が反復内で設定済み必須
/// - 本体が down で終わり得る場合、carried penDown を観測する
///   stroke は「全 HALT down 確定（lanes>0 を down 開始）」でのみ
///   受理（それ以外は従来通り全 HALT penUp 必須）
CfgResult analyzeCfg(const uint32_t* code, size_t nInstr,
                     uint32_t nSlots, const BodyScan& scan,
                     const std::vector<uint32_t>& initiallyDefined = {});

/// affine 再帰の証明結果（proven 時のみ有効）
struct Recurrence {
    bool proven = false;
    int64_t step = 0;
    int64_t minOff = 0, maxOff = 0;   // carried からの観測オフセット幅
};

/// スロットの抽象値を SelfPlus(0) で seed した symbolic CFG dataflow。
/// 全到達 HALT で同じ step の SelfPlus なら再帰として受理。
/// 内側ループ・条件付き増分・非線形蓄積は merge/widening で
/// Unknown になり自動的に落ちる。
Recurrence analyzeRecurrence(const uint32_t* code, size_t nInstr,
                             uint32_t nSlots, uint32_t slot,
                             const std::vector<uint32_t>* roundedPushes =
                                 nullptr);

/// レーン内レコードの論理順序 id（GPU 完了後に CPU で逐次順へ並べる）。
/// upper32 = レーン（論理反復）、lower32 = レーン内ストローク列。
using PenOrderId = uint64_t;
constexpr PenOrderId penOrderPack(uint32_t lane, uint32_t stroke) {
    return (uint64_t(lane) << 32) | uint64_t(stroke);
}
constexpr uint32_t penOrderLane(PenOrderId id) {
    return uint32_t(id >> 32);
}
constexpr uint32_t penOrderStroke(PenOrderId id) {
    return uint32_t(id & 0xFFFFFFFFu);
}

/// counts[]（レーン毎レコード数）の排他的接頭和。
/// 返り値 false = 容量超過等で無効（counts 内容が信頼できない場合に備え
/// maxPerLane を超える要素も拒否）。
bool penOrderOffsets(const uint32_t* counts, uint32_t lanes,
                     uint32_t maxPerLane, uint32_t* offsetsOut,
                     uint64_t* totalOut);

} // namespace xyo::gpu
