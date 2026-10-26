// xyo-cpp: Blockly 形状ジオメトリ（float ポリライン + 三角形分割）。
//
// scratch-blocks (Blockly fork) の block_render_svg_vertical.js の
// パスを SDL2 の SDL_RenderGeometry で再現するための最小エンジン。
// すべて論理座標（Blockly gridUnit=4 基準）。描画時に s（zoom）倍する。
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

struct SDL_Renderer;
namespace xyo::ui { class DrawList; }

namespace xyo {
namespace blockly {

struct Color { uint8_t r, g, b, a; };

inline Color rgb(uint32_t v, uint8_t a = 255) {
    return Color{uint8_t((v >> 16) & 0xFF), uint8_t((v >> 8) & 0xFF),
                 uint8_t(v & 0xFF), a};
}

// ---- パス構築 -----------------------------------------------------------
/// ポリラインで近似したパス。moveTo は新しい副パスを開始する
/// （fill は各副パスを別ポリゴンとして扱う）。
struct Path {
    struct Pt { float x, y; };
    std::vector<Pt> pts;
    std::vector<int> sub;   // 各副パスの開始インデックス（pts 内）

    void clear() { pts.clear(); sub.clear(); }
    void moveTo(float x, float y) {
        sub.push_back((int)pts.size());
        pts.push_back({x, y});
    }
    void lineTo(float x, float y) {
        // ほぼ重複点は潰す（arc→line の継ぎ目の浮動誤差が
        // triangulator の退化三角形・耳誤判定を起こす）
        if (!pts.empty() &&
            std::fabs(pts.back().x - x) < 1e-3f &&
            std::fabs(pts.back().y - y) < 1e-3f) return;
        pts.push_back({x, y});
    }
    /// 三次ベジェを seg 分割で近似して追加（制御点 c1,c2 → 終点 e）。
    void cubicTo(float c1x, float c1y, float c2x, float c2y,
                 float ex, float ey, int seg = 10);
    /// 円弧: 中心 (cx,cy) 半径 r、角度 a0 から a1（度、y 下向き）まで seg 分割。
    void arcTo(float cx, float cy, float r, float a0deg, float a1deg,
               int seg = 6);
    /// 角丸コーナー: 現在位置から (ex,ey) へ向かう前に (cx,cy) を支点に
    /// 半径 r の円弧を置く（線→弧→線）。SVG の 'a' 相当の簡易版。
    void cornerTo(float cx, float cy, float r, float ex, float ey);
    void close() {}  // ポリラインは暗黙で閉じる
};

// ---- 塗り / 輪郭 ---------------------------------------------------------
/// 凹ポリゴン対応の塗り（ear clipping で三角形化）。scale は描画倍率。
void fillPath(SDL_Renderer* r, const Path& p, float s, Color c);
/// 多角形 1 枚（凹 OK）。
void fillPoly(SDL_Renderer* r, const float* xy, int n, float s, Color c);
/// 線分チェーン（輪郭線）。t = 線の太さ（描画座標）。閉路は close=true。
void strokePath(SDL_Renderer* r, const Path& p, float s, Color c, float t,
                bool close = true);

// ---- プリミティブ（スクリーン座標、倍率適用後）--------------------------
void fill(SDL_Renderer* r, float x, float y, float w, float h, Color c);
void outline(SDL_Renderer* r, float x, float y, float w, float h, Color c);
/// 角丸矩形（両側 semicircle 可能な pill まで）。
void rrFill(SDL_Renderer* r, float x, float y, float w, float h, float rad,
            Color c);
void rrOutline(SDL_Renderer* r, float x, float y, float w, float h, float rad,
               Color c, float t = 1.0f);
/// 六角形（横向き、左右の先端）。n = 先端の飛び出し幅。
void hexFill(SDL_Renderer* r, float x, float y, float w, float h, float n,
             Color c);
void hexOutline(SDL_Renderer* r, float x, float y, float w, float h, float n,
                Color c, float t = 1.0f);
void tri(SDL_Renderer* r, float x0, float y0, float x1, float y1,
         float x2, float y2, Color c);
void line(SDL_Renderer* r, float x0, float y0, float x1, float y1, Color c,
          float t = 1.0f);

/// 円（塗り）。
void discFill(SDL_Renderer* r, float cx, float cy, float rad, Color c);
/// 円環（輪郭）。
void ring(SDL_Renderer* r, float cx, float cy, float rad, float t, Color c);

/// 描画先の ui::DrawList を設定（フレーム先頭で 1 回呼ぶ）。
/// 全プリミティブはここへ描く。SDL_Renderer 引数は互換のため残置。
void setDrawList(ui::DrawList* dl);
/// 現在の描画先（画像 blit 用。未設定なら nullptr）。
ui::DrawList* drawList();

} // namespace blockly
} // namespace xyo
