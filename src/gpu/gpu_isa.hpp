// xyo-cpp: GPU 並列実行用ミニ VM の ISA・プログラム構造。
// control_repeat の本体が「数値計算 + ペン描画のみ・反復間独立」と
// 静的に証明できるとき、JIT が本 ISA のバイトコードへコンパイルし、
// compute shader 上で 1 反復 = 1 レーンとして並列実行する
// （詳細は gpu_compile.cpp — 変数は「読む前に必ず書かれる」、
// 位置/ペン状態も「stroke/read 前に反復内で設定される」ことが必要）。
// VM は f32 値のみを扱い、文字列・リスト・yield・動的ディスパッチは
// 対象外（コンパイル不可なら CPU ループへフォールバックする）。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xyo::gpu {

/// VM 命令（op + operand の 8B 単位）。
enum Op : uint32_t {
    OP_HALT = 0,
    OP_PUSH,      // operand = f32 bits → stack
    OP_ITER,      // push 反復 index (0..lanes-1)
    OP_LOADV,     // push lane var[operand]
    OP_STOREV,    // pop → lane var[operand]（temp/arg スロット用）
    OP_STOREVM,   // operand = slot | wbIdx<<16: store + wbMask[wbIdx] セット
    OP_LOADG,     // push globals[operand]（dispatch 時スナップショット）
    OP_POSX,      // push 現在位置 x（lane ローカル）
    OP_POSY,      // push 現在位置 y
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_LT, OP_GT, OP_EQ, OP_LE, OP_GE, OP_NEQ,
    OP_AND, OP_OR, OP_NOT, OP_NEG,
    OP_MATH,      // operand = MathKind（単項）
    OP_RAND,      // pop to,from → random(from,to)（両方整数値なら int pick）
    OP_JMP,       // pc = operand
    OP_JMPIF0,    // pop cond; ==0 なら pc = operand
    OP_GOTOXY,    // pop y,x: フェンス内へ clamp。penDown なら線レコード出力。
    OP_SETX,      // pop x
    OP_SETY,      // pop y
    OP_CHANGEX,   // pop dx
    OP_CHANGEY,   // pop dy
    OP_PENDOWN,   // penDown=1 + 現位置にドット出力（size/color 現在値）
    OP_PENUP,     // penDown=0
    OP_PENSIZE,   // pop → clamp(1,1200)
    OP_PENSIZED,  // pop d → size = clamp(size+d, 1, 1200)
    OP_PENCOLN,   // pop → 0xRRGGBB 整数 → rgbToHsv → hsv 更新 + t=0 + shade=v/2
    OP_PENCOLP,   // operand: 0=color 1=sat 2=bri 3=trans; pop → パラメータ set
    OP_PENCOLPB,  // operand: 同上; pop → パラメータ += value
    _OP_MAX
};

/// OP_MATH の operand
enum MathKind : uint32_t {
    M_ABS = 0, M_FLOOR, M_CEIL, M_SQRT,
    M_SIN, M_COS, M_TAN, M_ASIN, M_ACOS, M_ATAN,
    M_LN, M_LOG, M_EXP, M_EXP10, M_ROUND,
    _M_MAX
};

/// 読み取り専用グローバル変数（dispatch 時に値をスナップショット）
struct GlobalVar {
    std::string id;
    std::u16string name;
};

/// writeback 変数 — 最終反復で「実際に書かれた」最後のレーンの値を戻す
struct WbVar {
    std::string id;
    std::u16string name;
    uint32_t slot = 0;
    uint32_t wbIdx = 0;
};

/// 外側ループのドメイン（実行側が lanes の導出に使用）
enum class LoopMode : uint32_t { Repeat = 0, ForEach, While };

/// affine 再帰（carried 変数の反復毎一定ステップ増分）。
/// preheader で slot = globals[globalIdx] + iter*step に初期化。
struct InductionVar {
    uint32_t globalIdx = 0;   // 初期値スナップショットの LOADG index
    uint32_t slot = 0;
    float step = 0;           // 整数ステップ（実行時 exact 検査）
    float minOff = 0, maxOff = 0;  // SelfPlus オフセットの観測範囲
};

/// コンパイル済み並列ループ プログラム。
struct Program {
    std::vector<uint32_t> code;              // {op,operand} のフラット列
    std::vector<GlobalVar> globals;          // globals[i] ← LOADG i
    std::vector<WbVar> wbVars;               // writeback 変数一覧
    uint32_t nSlots = 0;                     // lane ローカル変数スロット数
    uint32_t maxPerLane = 64;                // レコードバッファ容量/レーン
    /// 本体に penUp/penDown が存在する。false ならペン状態は
    /// 反復を通じて不変 — dispatch 時 penDown==true は逐次依存のため
    /// 実行側が拒否する（全ストロークが前反復終端位置に依存する）。
    bool hasPenOps = false;

    LoopMode mode = LoopMode::Repeat;
    std::vector<InductionVar> induction;
    /// While/Unil のみ: induction[] 内のカウンタ index と正規化済み
    /// 継続条件（counter <op> bound: 1='<',2='<=',3='>',4='>='）。
    /// 0 = while ドメインでない。
    int32_t whileInd = -1;
    uint32_t whileCmp = 0;
    /// globals 末尾 nBoundGuards 個は while 境界式が参照する
    /// 変数の実行時型ガード（VM は LOADG しない）。
    uint32_t nBoundGuards = 0;

    /// 全終了経路が pen down と証明された — 逐次では lanes>0 が
    /// 前レーン終端の down を継承するので、VM の lanes>0 も
    /// down 開始にする必要がある（実行側へ伝搬）。
    bool endMustDown = false;
    /// 全終了経路で一様な定数と証明されたペン属性
    /// {size,hue,sat,bri,transp}（bit k 有効）— lanes>0 の
    /// 初期属性。carried 観測の直列一致に必要。
    uint32_t endAttrMask = 0;
    float endAttr[5] = {};
};

/// laneOut（SSBO）の 1 レーンあたりフラット float レイアウト。
/// [0]=flags(u32bits) [1]=endX [2]=endY [3]=penSize
/// [4]=h [5]=s [6]=v [7]=t  [8..8+MW)=wbMask(u32bits)  [8+MW..)=wb 値
/// [8+MW+nWb]=shade（PENCOLN 実行時点の brightness/2）
/// flags: bit0 posDirty, bit1 penAttrDirty, bit2 pdDirty, bit3 penDown,
///        bit4 PENCOLN 実行あり(shade 更新用), bit5 命令ガード超過,
///        bit6 X, bit7 Y, bit8 size, bit9 hue, bit10 saturation,
///        bit11 brightness, bit12 transparency （成分別 dirty —
///        集約 writeback が他レーンの古い成分で上書きするのを防ぐ）
constexpr uint32_t kLaneOutFixed = 8;
constexpr uint32_t kLaneFlagX = 1u << 6, kLaneFlagY = 1u << 7,
                   kLaneFlagSize = 1u << 8, kLaneFlagHue = 1u << 9,
                   kLaneFlagSat = 1u << 10, kLaneFlagBri = 1u << 11,
                   kLaneFlagTransp = 1u << 12;
inline uint32_t laneOutShadeOffset(uint32_t nWb) {
    return kLaneOutFixed + (nWb + 31) / 32 + nWb;
}
inline uint32_t laneOutStride(uint32_t nWb) {
    return laneOutShadeOffset(nWb) + 1;
}

/// RendererAPI::gpuExecParallel への実行要求。
/// code/globals/wbOut は要求中のみ有効。
struct ExecRequest {
    const uint32_t* code = nullptr;
    size_t codeLen = 0;                      // u32 個数
    const float* globals = nullptr;
    uint32_t nGlobals = 0;
    uint32_t nSlots = 0;
    uint32_t lanes = 0;
    uint32_t maxPerLane = 64;
    uint32_t nWb = 0;
    const uint32_t* wbSlots = nullptr;   // nWb 個 — laneOut 書き戻し元スロット

    // フェンス矩形（scratch 座標、呼出し側で target から計算済み）
    float fenceL = -240, fenceR = 240, fenceB = -180, fenceT = 180;
    // lane 初期状態（dispatch 時点の実状態を全レーンへ複製）
    float startX = 0, startY = 0;
    float penSize = 1;
    float penHSV[4] = {0, 100, 100, 0};      // h(0-100),s,v,transparency
    bool penDown = false;
    /// lanes>0 の初期 penDown（Program::endMustDown — 本体が全経路
    /// down 終了と証明された場合のみ真。lane0 は常に penDown）。
    bool penDownAll = false;
    /// lanes>0 の初期ペン属性（Program::endAttrMask/endAttr —
    /// 全終了経路で一様と証明された属性のみ）。bit k 未対応の
    /// 属性は dispatch 時点値を使う。
    uint32_t endAttrMask = 0;
    float endAttr[5] = {};
    bool miscLimits = true;                  // runtime->miscLimits（ペン幅 clamp）
    uint32_t baseSeed = 1;                   // 乱数 seed — dispatch 毎に変える
                                           // （固定だと全 dispatch で同一の
                                           // 乱数列になり描画が静止する）

    // 結果（executed=true のとき有効）: laneOut フラット配列
    // laneOutStride(nWb) × lanes — 呼出し側が走査して書き戻す。
    const float* laneOut = nullptr;          // GPU 側で readback 済み領域
    uint32_t nRecords = 0;                   // 描画された総ストローク数
    bool overflow = false;                   // レコード溢れ（→CPU へ）
    bool executed = false;
};

} // namespace xyo::gpu
