// xyo-cpp: エディタの配色と寸法（scratch-gui 単一ウィンドウ準拠）。
#pragma once

#include <cstdint>

#include "blockly/geom.hpp"

namespace xyo {
namespace theme {

using Color = blockly::Color;
constexpr Color rgb(uint32_t v) {
    return Color{uint8_t((v >> 16) & 0xFF), uint8_t((v >> 8) & 0xFF),
                 uint8_t(v & 0xFF), 0xFF};
}

// ---- scratch-gui 配色（css/colors.css 実値） ----
// menu-bar.css: .menu-bar background は $looks-secondary（#855CD6）
// Native edition: graphite chrome, violet accents, familiar block colors.
inline constexpr Color kHeader = rgb(0x20283F);
inline constexpr Color kHeaderDark = rgb(0x151C30);
inline constexpr Color kUiPrimary = rgb(0xE5F0FF);   // $ui-primary #E5F0FF (body-wrapper)
inline constexpr Color kUiSecondary = rgb(0xE9F1FC); // $ui-secondary #E9F1FC
inline constexpr Color kUiTertiary = rgb(0xD9E3F2);  // $ui-tertiary #D9E3F2
inline constexpr Color kTabBg = rgb(0xD9E3F2);       // 非アクティブタブ $ui-tertiary
inline constexpr Color kTabActive = rgb(0xFFFFFF);
inline constexpr Color kBody = rgb(0xE5F0FF);        // $ui-primary
inline constexpr Color kPanel = rgb(0xFFFFFF);
inline constexpr Color kBorder = Color{0, 0, 0, 38}; // rgba(0,0,0,.15) $ui-black-transparent
inline constexpr Color kFg = rgb(0x575E75);          // $text-primary
inline constexpr Color kDim = rgb(0x8691A7);
inline constexpr Color kSel = rgb(0xDBEAFF);
inline constexpr Color kAccent = rgb(0x4C97FF);      // $motion-primary
inline constexpr Color kLooksSecondary = rgb(0x855CD6);
inline constexpr Color kWarn = rgb(0xE64C4C);
inline constexpr Color kToolboxSel = rgb(0xE9EEF2);  // $toolboxSelected
inline constexpr Color kMenuHover = Color{0, 0, 0, 38}; // 15% 黒（menu hover/メニュー区切り）

// ---- Blockly（scratch-blocks css.js/colours.js） ----
inline constexpr Color kWsBg = rgb(0xF9F9F9);        // COLORS.workspace
inline constexpr Color kWsGrid = rgb(0xDDDDDD);      // グリッド #ddd
inline constexpr Color kFlyoutBg = rgb(0xF9F9F9);    // COLORS.flyout（opacity .8 で描画）
inline constexpr Color kOnBlock = rgb(0xFFFFFF);
inline constexpr Color kTextField = rgb(0xFFFFFF);
inline constexpr Color kTextFieldText = rgb(0x575E75);
inline constexpr Color kGlow = rgb(0xFFF200);        // stackGlow
inline constexpr Color kInsertion = Color{0, 0, 0, 51}; // insertionMarker 20% 黒

// ---- stage 列 ----
inline constexpr Color kStageMenu = rgb(0xE5F0FF);
inline constexpr Color kGreenFlag = rgb(0x4CBF56);
inline constexpr Color kGreenFlagDark = rgb(0x45993D);
inline constexpr Color kStopRed = rgb(0xEC5959);

// ---- 寸法（units.css / css.js 実値） ----
inline constexpr int kMenuH = 48;        // $menu-bar-height 3rem
inline constexpr int kTabH = 44;         // $stage-menu-height
inline constexpr int kTabListW = 250;    // tab-list width
inline constexpr int kToolboxW = 60;     // .scratchCategoryMenu width
inline constexpr int kFlyoutW = 250;     // flyout 実効幅（.tab-list と同幅）
inline constexpr int kStageMenuH = 44;
inline constexpr int kSpriteInfoH = 96;  // $sprite-info-height 6rem
inline constexpr int kBackpackBarH = 30;  // backpack-header: margin-top .5rem + padding .25rem + 13.6px
inline constexpr int kGap = 8;           // $space

} // namespace theme
} // namespace xyo
