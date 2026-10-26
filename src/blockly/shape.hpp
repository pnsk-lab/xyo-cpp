// xyo-cpp: scratch-blocks 準拠のブロック描画。
// 形状は block_render_svg_vertical.js のパス定義を Path（ポリライン近似）で
// 再現し、SDL_RenderGeometry で塗る。
#pragma once

#include <cstdint>
#include <string>

#include "geom.hpp"
#include "layout.hpp"

struct SDL_Renderer;

namespace xyo {

class Blocks;
struct Block;

namespace blockly {

/// lb の外形パス（ブロック左上 = (0,0) 相対、論理座標）。
/// drawBlock が lb.x/lb.y と zoom を掛けて実座標へ展開する。
Path blockOutline(const LBlock& lb);
/// ノッチ（上の切り欠き）を (x, y) 起点に追加（左→右、深さ kNotchH）。
void notchDown(Path& p, float x, float y);
/// バンプ（下の張り出し）を (x, y) 起点に追加（右→左、深さ kNotchH）。
/// 左→右で引く天井バンプ版（C ブロックの口の天井）。
void notchDownRev(Path& p, float x, float y);

/// 1 ブロックを描く（lb は ws 論理座標。ox,oy = 画面上の原点、s = zoom）。
/// sel: 選択アウトライン、glow: 実行中の黄色グロウ、alpha: 不透明度。
/// 名前指定でブロックアイコン（埋め込み SVG 48px）を描画する。
/// name は "flag"/"repeat"/"turnright"/"turnleft"/"pen"/"music"/
/// "tab_code"/"tab_costumes"/"tab_sounds" または opcode。
/// 対応するアイコンが無ければ false。
bool drawBlockIcon(SDL_Renderer* r, const std::string& name,
                   float x, float y, float w, float h, float alpha = 1.0f);

// Call before destroying the SDL renderer (launcher can reopen the editor).
void releaseIcons();

void drawLBlock(SDL_Renderer* r, const LBlock& lb, Blocks* blocks,
                const TextRenderer& tr, const BlockStyle& st,
                float ox, float oy, float s, bool sel, bool glow,
                float alpha);
/// ブロック上の 1 区間（テキスト/フィールド箱/ソケット/アイコン）を描く。
/// 入れ子ブロック（Input/BoolInput）は呼び出し側が子 LBlock で描く。
void drawSeg(SDL_Renderer* r, const LSeg& g, const BlockStyle& st,
             float bx, float by, float s, const TextRenderer& tr,
             float alpha);
/// コメントの黄色い吹き出しをブロック右上へ描く。
void drawComment(SDL_Renderer* r, const std::string& text, float bx,
                 float by, float s, const TextRenderer& tr);
/// 実行中グロウ（黄色の太いアウトライン）。
void drawGlow(SDL_Renderer* r, const Path& p, float ox, float oy, float s,
              float alpha);

// ---- 補助 ------------------------------------------------------------------
/// 角丸（半径 rad）で角を削った四角形パス（x,y,w,h。上端ノッチ無し）。
Path roundRectPath(float x, float y, float w, float h, float rad);
/// 下向き三角（▼）。cx は中心 x、y は上端。
void arrowDown(SDL_Renderer* r, float cx, float y, float w, float h, Color c);

} // namespace blockly
} // namespace xyo
