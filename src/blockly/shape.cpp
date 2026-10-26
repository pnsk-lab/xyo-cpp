// xyo-cpp: scratch-blocks 準拠のブロック描画の実装。
#include "shape.hpp"

#if defined(XYO_HAVE_SDL2) && !defined(__EMSCRIPTEN__)

#include <SDL.h>
#include "ui/draw.hpp"

#include <algorithm>
#include <cmath>

#include "label.hpp"
#include "model/blocks.hpp"

namespace xyo {
namespace blockly {

namespace {

constexpr float R = kCornerR;
constexpr float NH = kNotchH;      // 8
constexpr float NS = kNotchX;      // 12 (NOTCH_START_PADDING)
constexpr float EDGE = kArmW;      // 16 (STATEMENT_INPUT_EDGE_WIDTH)
constexpr float INS = 8;           // STATEMENT_INPUT_INNER_SPACE

/// (x,y) から右へ NOTCH_PATH_LEFT（切り欠き、深さ NH）。
void notchL(Path& p, float x, float y) {
    // 'c 2,0 3,1 4,2  l 4,4  c 1,1 2,2 4,2  h 12  c 2,0 3,-1 4,-2  l 4,-4
    //  c 1,-1 2,-2 4,-2'
    p.cubicTo(x + 2, y, x + 3, y + 1, x + 4, y + 2, 4);
    p.lineTo(x + 8, y + 6);
    p.cubicTo(x + 9, y + 7, x + 10, y + 8, x + 12, y + 8, 4);
    p.lineTo(x + 24, y + 8);
    p.cubicTo(x + 26, y + 8, x + 27, y + 7, x + 28, y + 6, 4);
    p.lineTo(x + 32, y + 2);
    p.cubicTo(x + 33, y + 1, x + 34, y, x + 36, y, 4);
}
/// (x,y) から左へ NOTCH_PATH_RIGHT（張り出し、深さ NH）。
void notchR(Path& p, float x, float y) {
    p.cubicTo(x - 2, y, x - 3, y + 1, x - 4, y + 2, 4);
    p.lineTo(x - 8, y + 6);
    p.cubicTo(x - 9, y + 7, x - 10, y + 8, x - 12, y + 8, 4);
    p.lineTo(x - 24, y + 8);
    p.cubicTo(x - 26, y + 8, x - 27, y + 7, x - 28, y + 6, 4);
    p.lineTo(x - 32, y + 2);
    p.cubicTo(x - 33, y + 1, x - 34, y, x - 36, y, 4);
}

/// 上辺（ブロック左上から時計回り開始）。
void edgeTop(Path& p, const LBlock& lb) {
    float w = lb.w;
    if (lb.defineHat) {
        // procedures_definition: TOP_LEFT/RIGHT_CORNER_DEFINE_HAT（R=20）
        // 上辺が y=-20 まで盛り上がった大きな角丸四角形。
        const float DR = 20;
        p.moveTo(0, 0);
        p.arcTo(DR, -DR, DR, 180, 270, 10);
        p.lineTo(w - DR, -DR);
        p.arcTo(w - DR, 0, DR, -90, 0, 10);
        return;
    }
    if (lb.hat) {
        p.moveTo(0, 0);
        // 'c 25,-22 71,-22 96,0'（ドームは y=0 の上に張り出す）
        p.cubicTo(25, -22, 71, -22, 96, 0, 12);
        p.lineTo(w, 0);
        // 右上は直角（startHat_ のとき角丸無し…Scratch はドーム後も角丸）
        p.cornerTo(w, 0, R, w, R);
        return;
    }
    if (lb.shape == edit::Shape::Reporter ||
        lb.shape == edit::Shape::Boolean) {
        float e = lb.h / 2;         // edgeShapeWidth
        p.moveTo(e, 0);
        p.lineTo(w - e, 0);
        // 右側 edge shape（下向き半分）
        if (lb.shape == edit::Shape::Reporter)
            p.arcTo(w - e, e, e, -90, 90, 12);
        else {
            p.lineTo(w, e);
            p.lineTo(w - e, lb.h);
            p.lineTo(e, lb.h);
            p.lineTo(0, e);
            p.close();
            return;   // hexagon はここで閉じる
        }
        p.lineTo(e, lb.h);
        p.arcTo(e, e, e, 90, 270, 12);
        return;
    }
    // stack / end / c-block
    p.moveTo(0, R);
    p.arcTo(R, R, R, 180, 270, 4);
    p.lineTo(NS, 0);
    notchL(p, NS, 0);
    p.lineTo(w - R, 0);
    p.arcTo(w - R, R, R, -90, 0, 4);
}

/// 下辺（最終行から左へ）。右下円弧 → next バンプ → 左下円弧。
/// 呼出し時にカーソルは (lb.w, y)（右辺下端）に置く。
void edgeBottom(Path& p, const LBlock& lb, float y) {
    // BOTTOM_RIGHT_CORNER: 'a R,R 0 0,1 -R,R'（(w,y-R)→(w-R,y)、中心 (w-R,y-R)）
    p.arcTo(lb.w - R, y - R, R, 0, 90, 4);
    if (!lb.noBump) {
        // NOTCH_PATH_RIGHT は上ノッチと同じ帯域 [NS, NS+36] 上を右→左に描く
        // （NOTCH_WIDTH(32)+NOTCH_START_PADDING(12)+CORNER_RADIUS(4) に揃える）
        p.lineTo(NS + 36, y);
        notchR(p, NS + 36, y);
    }
    p.lineTo(R, y);
    p.arcTo(R, y - R, R, 90, 180, 4);
    p.lineTo(0, R);
}

} // namespace

void notchDown(Path& p, float x, float y) { notchL(p, x, y); }
void notchDownRev(Path& p, float x, float y) { notchR(p, x + 36, y); }

Path roundRectPath(float x, float y, float w, float h, float rad) {
    Path p;
    rad = std::min(rad, std::min(w, h) / 2);
    p.moveTo(x + rad, y);
    p.lineTo(x + w - rad, y);
    p.arcTo(x + w - rad, y + rad, rad, -90, 0);
    p.lineTo(x + w, y + h - rad);
    p.arcTo(x + w - rad, y + h - rad, rad, 0, 90);
    p.lineTo(x + rad, y + h);
    p.arcTo(x + rad, y + h - rad, rad, 90, 180);
    p.lineTo(x, y + rad);
    p.arcTo(x + rad, y + rad, rad, 180, 270);
    return p;
}

/// 口の天井（右端から左へ）: BOTTOM_RIGHT_CORNER + h + NOTCH_RIGHT + inner arc
static void mouthCeiling(Path& p, float mouthRightX, float headerBot) {
    // right edge から headerBot に来た点。corner を描き左へ
    p.cornerTo(mouthRightX, headerBot, R, mouthRightX - R, headerBot);
    p.lineTo(EDGE + NS + 36, headerBot);            // ノッチ右端 64
    notchR(p, EDGE + NS + 36, headerBot);           // 下向きバンプ → 28
    p.lineTo(EDGE + R, headerBot);                  // h -INS → 20
    // INNER_TOP_LEFT_CORNER: 'a R,R 0 0,0 -R,R' → (EDGE, headerBot+R)
    // （角の外側 (EDGE+R, headerBot+R) を中心とする凹フィレット）
    p.arcTo(EDGE + R, headerBot + R, R, -90, -180, 4);
}

/// 口の床（左内角から右へ）: INNER_BOTTOM_LEFT + notch + H rightEdge-R
static void mouthFloor(Path& p, float mouthRightX, float floorY, bool notch) {
    // INNER_BOTTOM_LEFT_CORNER: 'a R,R 0 0,0 R,R' → (EDGE+R, floorY)
    // （角の外側 (EDGE+R, floorY-R) を中心とする凹フィレット）
    p.arcTo(EDGE + R, floorY - R, R, 180, 90, 4);
    if (notch) {
        p.lineTo(EDGE + R + INS, floorY);           // h +INS → 28
        notchL(p, EDGE + R + INS, floorY);          // 上向きバンプ [28,64]
    }
    p.lineTo(mouthRightX - R, floorY);
}

Path blockOutline(const LBlock& lb) {
    Path p;
    if (lb.shape == edit::Shape::Reporter ||
        lb.shape == edit::Shape::Boolean) {
        edgeTop(p, lb);   // 自力で閉じる
        return p;
    }
    if (lb.cblock == 0) {
        edgeTop(p, lb);
        p.lineTo(lb.w, lb.h - R);
        edgeBottom(p, lb, lb.h);
        p.close();
        return p;
    }
    // ---- C ブロック ----
    edgeTop(p, lb);
    const float headerBot = lb.mouths.empty() ? lb.h : lb.mouths[0].y - lb.y;
    const float rightX = lb.w;
    // 右辺を header 下端まで下げる
    p.lineTo(rightX, headerBot - R);
    // ---- 口 1 ----
    mouthCeiling(p, rightX, headerBot);
    float floor1 = headerBot + lb.mouths[0].innerH;
    p.lineTo(EDGE, floor1 - R);
    mouthFloor(p, rightX, floor1, true);
    // ここでカーソルは (rightX - R, floor1)
    if (lb.cblock >= 2) {
        // else バー: 右辺を elseBarH 下げる
        float elseBot = floor1 + kStackH;
        p.cornerTo(rightX, floor1, R, rightX, floor1 + R);
        p.lineTo(rightX, elseBot - R);
        // ---- 口 2 ----
        mouthCeiling(p, rightX, elseBot);
        float floor2 = elseBot + lb.mouths[1].innerH;
        p.lineTo(EDGE, floor2 - R);
        mouthFloor(p, rightX, floor2, true);
        // キャップへ
        p.cornerTo(rightX, floor2, R, rightX, floor2 + R);
        p.lineTo(rightX, lb.h - R);
        edgeBottom(p, lb, lb.h);
    } else {
        // キャップ: 床から右辺へ戻って下端へ
        p.cornerTo(rightX, floor1, R, rightX, floor1 + R);
        p.lineTo(rightX, lb.h - R);
        edgeBottom(p, lb, lb.h);
    }
    p.close();
    return p;
}

// ---- 描画 -------------------------------------------------------------------

static Color withAlpha(Color c, float a) {
    c.a = (uint8_t)std::lround(c.a * a);
    return c;
}

void drawGlow(SDL_Renderer* r, const Path& p, float ox, float oy, float s,
              float alpha) {
    Path t = p;
    for (auto& q : t.pts) { q.x += ox; q.y += oy; }
    strokePath(r, t, s, withAlpha(Color{0xFF, 0xF2, 0x00, 255}, alpha),
               4 * s + 1.5f);
}

void arrowDown(SDL_Renderer* r, float cx, float y, float w, float h, Color c) {
    tri(r, cx - w / 2, y, cx + w / 2, y, cx, y + h, c);
}

/// フィールド箱（白いピル/角丸）+ 文字。枠線は親ブロックの tertiary
/// （scratch-blocks: shadow ブロックの stroke = parent.getColourTertiary()）。
static void fieldBox(SDL_Renderer* r, float x, float y, float w, float h,
                     bool pill, const std::string& text, Color ink, Color edge,
                     float s, const TextRenderer& tr, float alpha) {
    Color fill{255, 255, 255, 255};
    rrFill(r, x * s, y * s, w * s, h * s, pill ? h * s / 2 : 4 * s,
           withAlpha(fill, alpha));
    rrOutline(r, x * s, y * s, w * s, h * s, pill ? h * s / 2 : 4 * s,
              withAlpha(edge, alpha), std::max(1.f, s));
    // 文字中央（field.js: text-anchor=middle, 中線=中央+FIELD_TOP_PADDING）
    int tw = tr.width(text, kFieldTextPx);
    float tx = x + (w - tw) / 2;
    tr.textMid(tx * s, (y + h / 2 + 2) * s, text, kFieldTextPx, false,
               withAlpha(ink, alpha));
}

/// ドロップダウン箱（tertiary 色の角丸 + ▼ + 白文字）。
/// scratch-blocks field_dropdown.js: box_ は rect rx=CORNER_RADIUS(=4) —
/// フルピルではなく角丸四角形。
static void dropdownBox(SDL_Renderer* r, float x, float y, float w, float h,
                        const std::string& text, Color box, Color ink,
                        float s, const TextRenderer& tr, float alpha) {
    rrFill(r, x * s, y * s, w * s, h * s, 4 * s, withAlpha(box, alpha));
    // field.js render_: text は矢印域を除いた中央（(w-arrowW)/2）
    int tw = tr.width(text, kFieldTextPx);
    float zoneW = w - (kArrowW + kArrowPad);
    float tx = x + (zoneW - tw) / 2;
    tr.textMid(tx * s, (y + h / 2 + 2) * s, text, kFieldTextPx, false,
               withAlpha(ink, alpha));
    // ▼（arrowY=11、高さ12: 中心は h/2+1）
    float ax = (x + w - 24) * s;
    float acy = (y + h / 2 + 1) * s;
    tri(r, ax, acy - 3 * s, ax + kArrowW * s, acy - 3 * s,
        ax + kArrowW * s / 2, acy + 3 * s, withAlpha(ink, alpha));
}

/// 空ソケットの穴（tertiary 色の輪郭 + 中身はブロック色）。
/// scratch-blocks: HEXAGONAL=六角、SQUARE=角丸四角(R=4)、ROUND=ピル。
static void emptySocket(SDL_Renderer* r, float x, float y, float w, float h,
                        bool hex, bool square, Color tertiary, float s,
                        float alpha) {
    if (hex) {
        hexFill(r, x * s, y * s, w * s, h * s, h * s / 2,
                withAlpha(tertiary, alpha));
    } else {
        rrFill(r, x * s, y * s, w * s, h * s,
               square ? 4 * s : h * s / 2, withAlpha(tertiary, alpha));
    }
}

/// 埋め込み実 SVG ピクセル（tools/embed_icons.py 生成、48x48 RGBA8）。
#include "icons_rgba.inc"

/// アイコン名 → テクスチャ（レンダラ毎に 1 セット生成してキャッシュ）。
static SDL_Texture* iconTex(SDL_Renderer* r, const std::string& kind) {
    static std::map<std::string, SDL_Texture*> cache;
    static SDL_Renderer* owner = nullptr;
    if (owner != r) {
        for (auto& [k, t] : cache) SDL_DestroyTexture(t);
        cache.clear();
        owner = r;
    }
    if (!r) return nullptr;
    // kind（opcode や "flag"/"tab_*"/"logo" 等）からアイコン名を引く。
    // 1) まず登録名と完全一致を試す（名前指定アイコン）
    for (const auto& ic : kIconPxs) {
        if (kind == ic.name) {
            auto it0 = cache.find(kind);
            if (it0 != cache.end()) return it0->second;
            SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                               SDL_TEXTUREACCESS_STATIC, ic.w, ic.h);
            if (!t) return nullptr;
            SDL_UpdateTexture(t, nullptr, ic.px, ic.w * 4);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            cache[kind] = t;
            return t;
        }
    }
    const char* name = nullptr;
    if (kind.rfind("tab_", 0) == 0 || kind == "logo" || kind == "globe")
        name = kind.c_str();
    else if (kind.find("flag") != std::string::npos) name = "flag";
    else if (kind.find("turnright") != std::string::npos) name = "turnright";
    else if (kind.find("turnleft") != std::string::npos) name = "turnleft";
    else if (kind.find("repeat") != std::string::npos ||
             kind.find("forever") != std::string::npos ||
             kind.find("while") != std::string::npos ||
             kind.find("wait") != std::string::npos ||
             kind.find("loop") != std::string::npos) name = "repeat";
    else if (kind.rfind("pen", 0) == 0) name = "pen";
    else if (kind.rfind("music", 0) == 0) name = "music";
    if (!name) return nullptr;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    const IconPx* def = nullptr;
    for (const auto& ic : kIconPxs)
        if (std::string(name) == ic.name ||
            kind.find(ic.name) != std::string::npos) { def = &ic; break; }
    if (!def) return nullptr;
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STATIC, def->w, def->h);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, def->px, def->w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    cache[name] = t;
    return t;
}

void releaseIcons() { iconTex(nullptr, ""); }

bool drawBlockIcon(SDL_Renderer* r, const std::string& name,
                   float x, float y, float w, float h, float alpha) {
    SDL_Texture* t = iconTex(r, name);
    if (!t) return false;
    ui::DrawList* dl = drawList();
    if (!dl) return false;
    int a = std::min(255, (int)(alpha * 255));
    dl->AddImage((ui::TextureID)(uintptr_t)t, {x, y}, {x + w, y + h},
                 {0, 0}, {1, 1}, ui::rgba(255, 255, 255, a));
    return true;
}

/// ブロック内アイコン（実 SVG をラスタ化して埋め込んだ 48x48 画像を描く）。
static void icon(SDL_Renderer* r, const std::string& kind, float x, float y,
                 float s, float alpha) {
    Color white{255, 255, 255, 255};
    if (SDL_Texture* t = iconTex(r, kind)) {
        if (ui::DrawList* dl = drawList()) {
            int a = std::min(255, (int)(alpha * 255));
            dl->AddImage((ui::TextureID)(uintptr_t)t, {x * s, y * s},
                         {(x + 24) * s, (y + 24) * s}, {0, 0}, {1, 1},
                         ui::rgba(255, 255, 255, a));
        }
        return;
    }
    if (kind.find("stop") != std::string::npos ||
        kind.find("execute") != std::string::npos) {
        // stop: 白い八角形（八近似的に円でも可）
        discFill(r, (x + 12) * s, (y + 12) * s, 9 * s,
                 withAlpha(white, alpha));
        return;
    }
    // 既定: 白い菱形
    Path p;
    p.moveTo(x + 12, y + 4);
    p.lineTo(x + 20, y + 12);
    p.lineTo(x + 12, y + 20);
    p.lineTo(x + 4, y + 12);
    fillPath(r, p, s, withAlpha(white, alpha));
}

void drawSeg(SDL_Renderer* r, const LSeg& g, const BlockStyle& st,
             float bx, float by, float s, const TextRenderer& tr,
             float alpha) {
    float x = bx + g.x, y = by + g.y;
    switch (g.kind) {
    case SegKind::Text:
        tr.textMid(x * s, (by + g.y + g.h / 2 + 2) * s, g.text,
                   kTextPx, false,
                   withAlpha(Color{255, 255, 255, 255}, alpha));
        break;
    case SegKind::NumField:
        fieldBox(r, x, y, g.w, g.h, true, g.text, Color{0x57, 0x5E, 0x75, 255},
                 st.tertiary, s, tr, alpha);
        break;
    case SegKind::StrField:
        fieldBox(r, x, y, g.w, g.h, false, g.text, Color{0x57, 0x5E, 0x75, 255},
                 st.tertiary, s, tr, alpha);
        break;
    case SegKind::Dropdown:
        dropdownBox(r, x, y, g.w, g.h, g.text, st.tertiary,
                    Color{255, 255, 255, 255}, s, tr, alpha);
        break;
    case SegKind::Variable:
        dropdownBox(r, x, y, g.w, g.h, g.text,
                    g.list ? Color{0xFF, 0x66, 0x1A, 255} : st.tertiary,
                    Color{255, 255, 255, 255}, s, tr, alpha);
        break;
    case SegKind::Socket:
        emptySocket(r, x, y, g.w, g.h, g.hex, g.square, st.tertiary, s, alpha);
        break;
    case SegKind::Icon:
        icon(r, g.text, x, y, s, alpha);
        break;
    case SegKind::Swatch: {
        // 白枠 + 色本体（Scratch のカラー入力は角丸のスウォッチ）
        Color sw = Color{255, 255, 255, 255};
        if (g.text.size() == 7 && g.text[0] == '#') {
            sw = rgb((uint32_t)std::strtoul(g.text.c_str() + 1, nullptr, 16));
        }
        float mx = x * s, my = y * s, mw = g.w * s, mh = g.h * s;
        rrFill(r, mx, my, mw, mh, 4 * s, Color{255, 255, 255, 255});
        rrFill(r, mx + 1.5f * s, my + 1.5f * s, mw - 3 * s, mh - 3 * s, 3 * s,
               withAlpha(sw, alpha));
        break;
    }
    default:
        break;   // Input/BoolInput は子ブロックとして描く
    }
}

void drawLBlock(SDL_Renderer* r, const LBlock& lb, Blocks* blocks,
                const TextRenderer& tr, const BlockStyle& st,
                float ox, float oy, float s, bool sel, bool glow,
                float alpha) {
    Path p = blockOutline(lb);
    Path tp = p;
    for (auto& q : tp.pts) { q.x += lb.x; q.y += lb.y; }
    Path scr = tp;
    for (auto& q : scr.pts) { q.x += ox; q.y += oy; }

    // 本体
    fillPath(r, scr, s, withAlpha(st.primary, alpha));
    // 周縁 1px 線（block_svg.js updateColour: stroke = getColourTertiary()）
    strokePath(r, scr, s, withAlpha(st.tertiary, alpha), 1.f);

    // else ラベル
    if (!lb.sub2Label.empty() && lb.cblock >= 2) {
        tr.textMid((lb.x + ox + kLabelPadX) * s,
                   (lb.y + oy + lb.sub2LabelY + kStackH / 2 + 2) * s,
                   lb.sub2Label, kTextPx, false,
                   withAlpha(Color{255, 255, 255, 255}, alpha));
    }

    // 区間（フィールド/ソケット/文字）
    for (const auto& g : lb.segs) {
        if (g.kind == SegKind::Input || g.kind == SegKind::BoolInput) {
            // 中身のブロックは LBlock として別描画。空なら穴を出す
            // （measure 時点で中身ありなのでここは空に見えるはずがない）
            continue;
        }
        drawSeg(r, g, st, lb.x + ox, lb.y + oy, s, tr, alpha);
    }

    if (glow) drawGlow(r, tp, ox, oy, s, alpha);
    if (sel) {
        strokePath(r, scr, s, withAlpha(Color{0x4C, 0x97, 0xFF, 255}, alpha),
                   2.5f * s);
    }
    (void)blocks;
}

void drawComment(SDL_Renderer* r, const std::string& text, float bx,
                 float by, float s, const TextRenderer& tr) {
    // scratchCommentRect/Body: fill #fef49c・枠 #bcA903・radius 4・
    // テキスト 12pt weight 400 黒（css.js 準拠）
    Color bg{0xFE, 0xF4, 0x9C, 255};
    Color edge{0xBC, 0xA9, 0x03, 255};
    Color ink{0x00, 0x00, 0x00, 255};
    int tw = tr.width(text, 11);
    float w = std::max(60.f, (float)tw * s + 16 * s);
    float h = 22 * s;
    float x = bx + 8 * s, y = by - h - 4 * s;
    rrFill(r, x, y, w, h, 4 * s, bg);
    rrOutline(r, x, y, w, h, 4 * s, edge, std::max(1.f, s));
    tr.text(x + 8 * s, y + (h - 11 * s) / 2, text, 11, false, ink);
}

} // namespace blockly
} // namespace xyo

#endif  // XYO_HAVE_SDL2
