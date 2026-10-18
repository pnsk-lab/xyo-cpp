// xyo-cpp: control_repeat/for_each/while 本体の GPU 並列実行向け
// サブセットコンパイラ。
//
// 対象（いずれかを外れる構成はコンパイル不可 → CPU ループへ）:
//   - 数値式のみ（文字列・リスト・真偽値以外の型変換を要するものは不可）
//   - 文: data_setvariableto/changevariableby, motion_gotoxy/setx/sety/
//     changexby/changeyby, pen_penDown/penUp/setPenSizeTo/
//     setPenColorToColor/setPenColorParamTo/changePenColorParamBy,
//     control_if/if_else/repeat/while/repeat_until/for_each/
//     all_at_once, procedures_call（インライン展開）
//   - 変数: 反復内で「読む前に必ず書かれる」変数のみ lane ローカル化可。
//     本体で一度も書かれない変数は dispatch 時スナップショット（読み専用）。
//     反復内で書き込み前に読む変数は affine 再帰（一定整数ステップ増分）
//     と静的に証明できる場合のみ preheader で再構成して受理。
//   - ペン/スプライト状態はレーン毎に dispatch 時点の値で初期化。
//   - 独立性は emit 済みバイトコード上の CFG 固定点解析で証明する
//     （gpu_analysis.cpp — 早期 return や内側ループを正確に扱う）。
#pragma once

#include <optional>
#include <string>

#include "../model/blocks.hpp"
#include "gpu_isa.hpp"

namespace xyo::gpu {

/// compileParallelLoop の結果。limitParent/limitInput は while/until
/// の境界式を JIT が評価するためのメタデータ（repeat/for_each では
/// 既に評価済みの n をそのまま渡し、これらは未使用）。
struct ParallelLoop {
    Program program;
    const Block* limitParent = nullptr;
    std::string limitInput;
};

/// SUBSTACK 先頭ブロック id → 並列実行プログラム（repeat 相当）。
/// 不可なら nullopt（why に理由を入れる）。
std::optional<Program> compileParallelRepeat(Blocks* blocks,
                                             const std::string& headId,
                                             std::string* why = nullptr);

/// ループブロック（control_repeat/for_each/while/repeat_until）
/// 全体 → 並列実行プログラム + ドメインメタデータ。
/// 不可なら nullopt（why に理由を入れる）。
std::optional<ParallelLoop> compileParallelLoop(Blocks* blocks,
                                              const Block* loop,
                                              std::string* why = nullptr);

} // namespace xyo::gpu
