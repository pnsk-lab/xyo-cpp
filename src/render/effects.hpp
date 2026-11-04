// xyo-cpp: Scratch グラフィックエフェクト（scratch-render sprite.frag の CPU 移植）。
// uv 変換（mosaic→pixelate→whirl→fisheye）→ サンプル → color/brightness → ghost。
#pragma once

#include <cstdint>
#include "../model/graphic_effects.hpp"
#include <string>
#include <vector>

namespace xyo {

/// エフェクトが 1 つでも非ゼロなら true（raw スキンをそのまま使える判定）。
bool hasActiveEffects(const GraphicEffects& effects);

/// エフェクト適用済み RGBA バッファを生成（out は w*h*4）。
/// src は straight-alpha RGBA8。uniform 変換・HSV 色相・premult 補正は
/// scratch-render の sprite.frag + ShaderManager.EFFECT_INFO に準拠。
void applyEffects(const uint8_t* src, int w, int h,
                  const GraphicEffects& effects,
                  std::vector<uint8_t>& out);

/// キャッシュキー用文字列（エフェクト値の正準化タプル）。
std::string effectKey(const GraphicEffects& effects);

} // namespace xyo
