// xyo-cpp: Native batched editor (SDL2, no external UI framework).
// scratch-gui（Scratch 3.0）の単一ウィンドウ構成を完全再現する:
//   青いメニューバー + Code/Costumes/Sounds タブ
//   左: カテゴリ列 + フライアウト（実ブロック形状のテンプレ）
//   中央: 無限ワークスペース（ドットグリッド・パン/ズーム・自由配置）
//   右: ステージ + スプライト情報 + スプライト選択
//   下端: バックパック
#include "editor.hpp"

#include <cstdio>
#include <vector>

#if defined(XYO_HAVE_SDL2) && !defined(__EMSCRIPTEN__)

#include <SDL.h>
#include <SDL_opengl.h>

#include "ui/draw.hpp"
#include "ui/font.hpp"
#include <cfloat>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "blockly/catalog.hpp"
#include "blockly/geom.hpp"
#include "blockly/label.hpp"
#include "blockly/layout.hpp"
#include "blockly/ops.hpp"
#include "blockly/shape.hpp"
#include "core/cast.hpp"
#include "core/fileio.hpp"
#include "core/json.hpp"
#include "core/slist.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "load/sb3.hpp"
#include "load/sb3_save.hpp"
#include "load/zip.hpp"
#include "model/blocks.hpp"
#include "model/target.hpp"
#include "render/sdl_renderer.hpp"
#include "render/text.hpp"
#include "shell/theme.hpp"

#ifdef XYO_HAVE_EMBEDDED_FONT
// ビルド時に tools/embed_font.py が生成（Liberation Sans, OFL）
#include "xyo_fonts.inc"
#endif

#ifdef XYO_HAVE_DEFAULT_PROJECT
// scratch-gui src/lib/default-project 由来の最小プロジェクト（File→New）。
// 白背景 + Scratch Cat（costume1/2）+ pop/meow 音を assets/default.sb3 から
// ビルド時に埋め込む。
#include "xyo_default_project.inc"
#endif

using namespace xyo;
using xyo::blockly::Color;
using xyo::blockly::Path;
using namespace xyo::theme;
namespace bl = xyo::blockly;

namespace {

static std::string utf16ToUtf8(const std::u16string& s) { return SStr::toUtf8(s); }

struct FRect { float x, y, w, h; };
static bool hit(const FRect& r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

} // anon

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------
struct Editor {
    Runtime* rt = nullptr;
    SdlRenderer ren;
    SDL_Window* win = nullptr;
    SDL_Renderer* r = nullptr;
    std::string path;

    // ---- UI 矩形（毎フレーム layoutUi() が更新）----
    struct Ui {
        FRect menuBar{0, 0, 0, 48};
        FRect tabs;         // Code/Costumes/Sounds
        FRect toolbox;      // カテゴリ列
        FRect flyout;       // フライアウト
        FRect ws;           // ワークスペース
        FRect stageMenu;    // 緑旗/stop
        FRect stage;        // ステージ（SdlRenderer が描く）
        FRect spriteInfo;
        FRect spriteSel;
        FRect stageSel;     // 右下のステージ選択カード
        FRect bpBar;        // バックパック開閉バー
        FRect bpPanel;      // バックパックパネル（開いているとき）
        FRect body;         // タブ以下の左列全体（Costumes/Sounds で使用）
        FRect zoomPlus, zoomMinus, zoomReset, trash;
        int tabX[3];        // タブの x 開始
        int tabW[3];
        FRect catRow[16];   // カテゴリ行
    } ui;
    int W = 0, H = 0;

    // ---- タブ / カテゴリ ----
    int tab = 0;            // 0=Code 1=Costumes 2=Sounds
    int catSel = 0;
    float flyScroll = 0;
    std::vector<edit::CategoryDef> cats_;   // kCategories + 使用中の拡張
    void buildCategories() {
        cats_ = edit::kCategories;
        if (!rt) return;
        auto uses = [&](const std::string& prefix) {
            for (Target* t : rt->targets)
                if (t->blocks)
                    for (auto& [bid, b] : t->blocks->allBlocks())
                        if (b.opcode.rfind(prefix, 0) == 0) return true;
            return false;
        };
        // My Blocks は常時（Scratch 既定のツールボックス末尾カテゴリ）
        cats_.push_back(edit::CategoryDef{"more", "My Blocks", 0xFF6680, {}});
        for (const auto& c : edit::kExtensionCategories)
            if (uses(c.id + "_")) cats_.push_back(c);
    }
    const edit::CategoryDef& catAt(int i) const { return cats_[i]; }
    int catCount() const { return (int)cats_.size(); }

    // ---- ワークスペース ----
    float panX = 0, panY = 0;      // ws 論理座標 → 画面へのオフセット
    bool panInit = false;
    float zoom = 1.0f;
    float wsScale() const { return bl::kWsZoom * zoom; }
    /// ワークスペースコンテンツの論理 bounds（原点を含む）。
    void wsContentBounds(float& minX, float& minY, float& maxX,
                         float& maxY) const {
        minX = std::min(0.f, -240.f);
        minY = std::min(0.f, -180.f);
        maxX = 240.f;
        maxY = 180.f;
        for (const auto& lb : wsLayout_.blocks) {
            minX = std::min(minX, lb.x);
            minY = std::min(minY, lb.y);
            maxX = std::max(maxX, lb.x + lb.w);
            maxY = std::max(maxY, lb.y + lb.h);
        }
    }
    /// スクロールバー（scratch-blocks: svgBackground=11px 全体、
    /// handle=6px で中央に描く・色 #CECDCE・右下に配置）。
    /// which: 0=horizontal, 1=vertical。トラック矩形を返す（要らなければ {}）。
    FRect wsScrollTrack(int which) {
        float s = wsScale();
        float minX, minY, maxX, maxY;
        wsContentBounds(minX, minY, maxX, maxY);
        float cw = (maxX - minX) * s, ch = (maxY - minY) * s;
        if (which == 0) {
            if (cw <= ui.ws.w) return {0, 0, 0, 0};
            return {ui.ws.x + 2, ui.ws.y + ui.ws.h - 11, ui.ws.w - 4 - 11,
                    11};
        }
        if (ch <= ui.ws.h) return {0, 0, 0, 0};
        return {ui.ws.x + ui.ws.w - 11, ui.ws.y + 2, 11,
                ui.ws.h - 4 - 11};
    }
    /// スクロールバーの thumb 矩形（handle = トラック 11 − 5 = 6px、中央）。
    FRect wsScrollThumb(int which) {
        FRect tr2 = wsScrollTrack(which);
        if (tr2.w <= 0) return tr2;
        float s = wsScale();
        float minX, minY, maxX, maxY;
        wsContentBounds(minX, minY, maxX, maxY);
        if (which == 0) {
            float cw = (maxX - minX) * s;
            float frac = ui.ws.w / cw;
            float th = tr2.w * frac;
            // 画面左端が見ている論理位置
            float lo = (ui.ws.x - panX) / s - minX;
            float f = lo / (maxX - minX - ui.ws.w / s);
            f = std::clamp(f, 0.f, 1.f);
            return {tr2.x + f * (tr2.w - th), tr2.y + 2.5f, th, 6};
        }
        float ch = (maxY - minY) * s;
        float frac = ui.ws.h / ch;
        float th = tr2.h * frac;
        float lo = (ui.ws.y - panY) / s - minY;
        float f = lo / (maxY - minY - ui.ws.h / s);
        f = std::clamp(f, 0.f, 1.f);
        return {tr2.x + 2.5f, tr2.y + f * (tr2.h - th), 6, th};
    }
    /// スクロールバー位置 → pan 更新（クリック/ドラッグ）。mx は画面座標。
    void wsScrollTo(int which, float pos) {
        FRect tr2 = wsScrollTrack(which);
        if (tr2.w <= 0) return;
        float s = wsScale();
        float minX, minY, maxX, maxY;
        wsContentBounds(minX, minY, maxX, maxY);
        FRect th2 = wsScrollThumb(which);
        if (which == 0) {
            float f = (pos - tr2.x - th2.w / 2) / (tr2.w - th2.w);
            f = std::clamp(f, 0.f, 1.f);
            float lo = minX + f * (maxX - minX - ui.ws.w / s);
            panX = ui.ws.x - lo * s;
        } else {
            float f = (pos - tr2.y - th2.h / 2) / (tr2.h - th2.h);
            f = std::clamp(f, 0.f, 1.f);
            float lo = minY + f * (maxY - minY - ui.ws.h / s);
            panY = ui.ws.y - lo * s;
        }
    }
    int scrollDrag_ = 0;   // 0=なし 1=H 2=V（マウス位置は drag.mx/my で追う）

    // ---- テキスト描画（キャッシュ済みフォント: Liberation Sans 埋め込み）----
    // ベイク済みフォントは editorMain が fonts_ へ登録する。
    // [0]=regular 32px, [1]=bold 32px。要求 px はスケールで出す
    // （32px ベイクからの縮小は 12–22px 域で十分に鮮明）。
    bool gpuUi_ = false;
    ui::Font* fontReg_ = nullptr;
    ui::Font* fontBold_ = nullptr;
    ui::Font* fontAt(bool bold) {
        ui::Font* f = bold && fontBold_ ? fontBold_ : fontReg_;
        return f;
    }
    ui::DrawList* dl() { return bl::drawList(); }

    void drawText(float x, float y, const std::string& s, int px, Color c,
                  bool bold = false) {
        ui::DrawList* d = dl();
        if (!d || s.empty()) return;
        ui::Font* f = fontAt(bold);
        d->AddText(f, (float)px, {x, y},
                   ui::rgba(c.r, c.g, c.b, c.a), s.c_str());
    }
    int textW(const std::string& s, int px, bool bold = false) {
        ui::Font* f = fontAt(bold);
        // Font metrics are available immediately, before the first draw.
        return (int)std::lround(
            f->CalcTextSizeA((float)px, FLT_MAX, 0.f, s.c_str()).x);
    }
    static int measureThunk(void* ctx, const std::string& s, int px, bool bold) {
        return static_cast<Editor*>(ctx)->textW(s, px, bold);
    }
    float trScale_ = 1.0f;   // tr().scale — drawThunk が拾う
    static void drawThunk(void* ctx, float x, float y, const std::string& s,
                          int px, bool bold, Color c) {
        Editor* e = static_cast<Editor*>(ctx);
        // 描画スケールに合わせてネイティブ解像度でラスタ化（縮小ボケを防ぐ）
        int npx = std::max(4, (int)std::lround(px * e->trScale_));
        e->drawText(x, y, s, npx, c, bold);
    }
    // 中線基準テキスト（field.js: y=size.height/2+FIELD_TOP_PADDING(2),
    // dominant-baseline=middle → 中線を box 中央+2 に合わせる）。
    // yMid はスケール済み座標。xHeight は Liberation Sans の 0.52em。
    static void drawMidThunk(void* ctx, float x, float yMid,
                             const std::string& s, int px, bool bold,
                             Color c) {
        Editor* e = static_cast<Editor*>(ctx);
        int npx = std::max(4, (int)std::lround(px * e->trScale_));
        ui::Font* f = e->fontAt(bold);
        float fs = f->FontSize > 0 ? f->FontSize : 32.f;
        float sc = (float)npx / fs;
        float asc = f->Ascent * sc;
        float xh = 0.52f * (float)npx;
        float y0 = yMid + xh * 0.5f - asc;
        e->drawText(x, y0, s, npx, c, bold);
    }
    bl::TextRenderer tr(float s = -1.f) {
        bl::TextRenderer t;
        t.ctx = this;
        t.measure = &Editor::measureThunk;
        t.draw = &Editor::drawThunk;
        t.drawMid = &Editor::drawMidThunk;
        t.scale = s > 0 ? s : wsScale();
        trScale_ = t.scale;
        return t;
    }

    /// SDL_Texture を ui::DrawList に貼る（スプライトサムネイル等）。
    void blitTex(SDL_Texture* tex, float x, float y, float w, float h,
                 float alpha = 1.0f) {
        ui::DrawList* d = dl();
        if (!d || !tex) return;
        int a = std::min(255, (int)(alpha * 255));
        d->AddImage((ui::TextureID)(uintptr_t)tex, {x, y}, {x + w, y + h},
                    {0, 0}, {1, 1}, ui::rgba(255, 255, 255, a));
    }
    void pushClip(const FRect& rc) {
        ui::DrawList* d = dl();
        if (!d) return;
        d->PushClipRect({rc.x, rc.y}, {rc.x + rc.w, rc.y + rc.h}, true);
    }
    void popClip() {
        ui::DrawList* d = dl();
        if (d) d->PopClipRect();
    }
    /// 点線（dashed border 用）。dash/間隔は CSS の 1px dashed に近似。
    void dashLine(float x1, float y1, float x2, float y2, Color c) {
        ui::DrawList* d = dl();
        if (!d) return;
        ui::PackedColor u = ui::rgba(c.r, c.g, c.b, c.a);
        float dx = x2 - x1, dy = y2 - y1;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.f) return;
        float ux = dx / len, uy = dy / len;
        const float on = 3.f, off = 2.f;
        for (float p = 0; p < len; p += on + off) {
            float e = std::min(p + on, len);
            d->AddLine({x1 + ux * p, y1 + uy * p}, {x1 + ux * e, y1 + uy * e},
                       u, 1.f);
        }
    }
    /// 角丸矩形の点線枠（title-field の dashed border 用）。
    void dashOutline(float x, float y, float w, float h, Color c) {
        dashLine(x + 4, y, x + w - 4, y, c);
        dashLine(x + w, y + 4, x + w, y + h - 4, c);
        dashLine(x + 4, y + h, x + w - 4, y + h, c);
        dashLine(x, y + 4, x, y + h - 4, c);
    }

    // ---- 選択ターゲット ----
    int selTarget = 0;
    int soundSel_ = 0;   // Sounds タブの選択インデックス
    int paintTool_ = 0;  // mode-selector の選択ツール（0=select）
    FRect paintToolRects_[9] = {};  // ツールボタンの矩形（hit 用）
    Target* selected() {
        if (!rt || rt->targets.empty()) return nullptr;
        selTarget = std::clamp(selTarget, 0, (int)rt->targets.size() - 1);
        return rt->targets[selTarget];
    }

    // ---- undo/redo ----
    struct Snapshot {
        std::unordered_map<std::string, Block> blocks;
        std::vector<std::string> scripts;
    };
    std::vector<Snapshot> undoStack, redoStack;
    static constexpr size_t kUndoLimit = 64;
    Snapshot takeSnapshot() {
        Snapshot s;
        Target* t = selected();
        if (t && t->blocks) {
            s.blocks = t->blocks->allBlocks();
            s.scripts = t->blocks->getScripts();
        }
        return s;
    }
    void pushUndo() {
        Target* t = selected();
        if (!t || !t->blocks) return;
        undoStack.push_back(takeSnapshot());
        if (undoStack.size() > kUndoLimit) undoStack.erase(undoStack.begin());
        redoStack.clear();
    }
    void popUndo() {
        if (!undoStack.empty()) undoStack.pop_back();
    }
    void undo() {
        Target* t = selected();
        if (!t || !t->blocks || undoStack.empty()) return;
        redoStack.push_back(takeSnapshot());
        Snapshot s = std::move(undoStack.back());
        undoStack.pop_back();
        t->blocks->restoreState(std::move(s.blocks), std::move(s.scripts));
        selBlock.clear();
        edit_.active = false;
    }
    void redo() {
        Target* t = selected();
        if (!t || !t->blocks || redoStack.empty()) return;
        undoStack.push_back(takeSnapshot());
        Snapshot s = std::move(redoStack.back());
        redoStack.pop_back();
        t->blocks->restoreState(std::move(s.blocks), std::move(s.scripts));
        selBlock.clear();
        edit_.active = false;
    }
    void clearHistory() { undoStack.clear(); redoStack.clear(); }

    // ---- ブロック操作 ------------------------------------------------------
    std::string selBlock;
    int blkCounter = 0;
    std::string newBlockId() {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "xyo_blk_%d", ++blkCounter);
        return buf;
    }
    std::string makeShadowBlock(Blocks* blocks, const std::string& opcode,
                                const std::string& field,
                                const std::string& val,
                                const std::string& parent) {
        std::string id = newBlockId();
        Block b;
        b.id = id;
        b.opcode = opcode;
        b.shadow = true;
        b.parent = parent;
        if (!field.empty()) {
            if (opcode == "text" || field == "TEXT") {
                b.fields[field] = BlockField{Value::string(SStr::fromUtf8(val))};
            } else {
                char* end = nullptr;
                double d = std::strtod(val.c_str(), &end);
                if (end != val.c_str() && *end == 0)
                    b.fields[field] = BlockField{Value::num(d)};
                else
                    b.fields[field] = BlockField{Value::string(SStr::fromUtf8(val))};
            }
        } else {
            // メニュー shadow（touchingobjectmenu 等）は自身の dropdown 既定値を持つ
            setupBlockDefaults(b, blocks);
            bindVariableField(b, selected());
            if (opcode == "colour_picker" && b.fields.empty())
                b.fields["COLOUR"] =
                    BlockField{Value::string(SStr::fromUtf8("#9966FF"))};
        }
        blocks->createBlock(b);
        return id;
    }
    void setupBlockDefaults(Block& b, Blocks* blocks) {
        const edit::BlockDef* d = edit::findBlock(b.opcode);
        if (!d) return;
        for (const auto& a : d->args) {
            if (a.kind == edit::ArgKind::Value) {
                BlockInput bi;
                if (!a.shadow.empty()) {
                    // INPUT_SAME_BLOCK_SHADOW: block == shadow
                    bi.block = makeShadowBlock(blocks, a.shadow, a.shadowField,
                                               a.shadowDefault, b.id);
                    bi.shadow = bi.block;
                }
                b.inputs.set(a.name, bi);
            } else if (a.kind == edit::ArgKind::Number ||
                       a.kind == edit::ArgKind::Angle) {
                char* end = nullptr;
                double dv = std::strtod(a.dflt.c_str(), &end);
                if (end != a.dflt.c_str() && *end == 0)
                    b.fields[a.name] = BlockField{Value::num(dv)};
                else
                    b.fields[a.name] = BlockField{Value::string(SStr::fromUtf8(a.dflt))};
            } else if (a.kind == edit::ArgKind::Dropdown) {
                std::string v = a.dflt.empty()
                                    ? (a.options.size() >= 2 ? a.options[1] : "")
                                    : a.dflt;
                b.fields[a.name] = BlockField{Value::string(SStr::fromUtf8(v))};
            }
        }
        if (d->cblock >= 1 && !b.inputs.has("SUBSTACK"))
            b.inputs.set("SUBSTACK", BlockInput{});
        if (d->cblock >= 2 && !b.inputs.has("SUBSTACK2"))
            b.inputs.set("SUBSTACK2", BlockInput{});
    }
    void bindVariableField(Block& b, Target* t) {
        const edit::BlockDef* d = edit::findBlock(b.opcode);
        if (!d) return;
        for (const auto& a : d->args) {
            if (a.kind != edit::ArgKind::Variable) continue;
            // 対象タイプ: LIST=リスト変数、BROADCAST_OPTION=ブロードキャスト（ステージ変数）
            const char* wantType = "";   // ""=スカラー変数
            bool typed = false;
            if (a.name == "LIST") { wantType = Variable::kListType; typed = true; }
            else if (a.name == "BROADCAST_OPTION") {
                wantType = Variable::kBroadcastType; typed = true;
            }
            const Target* search = t;
            if (typed && wantType == Variable::kBroadcastType && rt) {
                for (Target* x : rt->targets)
                    if (x->isStage) { search = x; break; }
            }
            if (search) {
                const std::string* pickId = nullptr;
                const Variable* pickV = nullptr;
                for (auto& [vid, v] : search->variables)
                    if (typed ? v.type == wantType
                              : v.type != Variable::kListType &&
                                    v.type != Variable::kBroadcastType) {
                        pickId = &vid; pickV = &v; break;
                    }
                if (!pickV && !search->variables.empty() && !typed) {
                    pickId = &search->variables.begin()->first;
                    pickV = &search->variables.begin()->second;
                }
                if (pickV) {
                    b.fields[a.name] =
                        BlockField{Value::string(SStr::fromUtf8(utf16ToUtf8(pickV->name)))};
                    b.fields[a.name].id = *pickId;
                } else if (typed && wantType == Variable::kBroadcastType) {
                    b.fields[a.name] =
                        BlockField{Value::string(SStr::fromUtf8("message1"))};
                } else {
                    b.fields[a.name] = BlockField{Value::string(SStr::fromUtf8(a.dflt))};
                }
            } else {
                b.fields[a.name] = BlockField{Value::string(SStr::fromUtf8(a.dflt))};
            }
        }
    }
    /// flyout 由来の値を持つブロックを作る（opcode + 任意のフィールド初期値）。
    std::string instBlock(Blocks* blocks, const std::string& opcode,
                          const std::map<std::string, BlockField>& initFields,
                          float wx, float wy, bool top,
                          const Mutation* mut = nullptr,
                          const std::map<std::string, std::string>* argTexts =
                              nullptr) {
        Block b;
        b.id = newBlockId();
        b.opcode = opcode;
        setupBlockDefaults(b, blocks);
        bindVariableField(b, selected());
        for (auto& [k, v] : initFields) b.fields[k] = v;
        if (mut) {
            b.mutation = *mut;
            b.mutation->parsed = false;
        }
        b.topLevel = top;
        b.x = wx;
        b.y = wy;
        std::string id = b.id;
        blocks->createBlock(b);
        if (argTexts) {
            for (auto& [inm, txt] : *argTexts) {
                if (txt.empty()) continue;   // bool 引数は空ソケット
                std::string sid = makeShadowBlock(blocks, "text", "TEXT", txt, id);
                if (Block* nb = blocks->getBlock(id)) {
                    BlockInput bi;
                    bi.block = sid;
                    bi.shadow = sid;
                    nb->inputs.set(inm, bi);
                }
            }
        }
        return id;
    }
    void deleteBlockChain(const std::string& id) {
        Target* t = selected();
        if (!t || !t->blocks || id.empty()) return;
        Blocks* blocks = t->blocks;
        pushUndo();
        // 前ブロックの next を切ってチェーンごと消す
        std::string prev = bl::prevOf(blocks, id);
        std::string parent;
        if (Block* b = blocks->getBlock(id)) {
            parent = b->parent;
            if (!prev.empty()) {
                if (Block* p = blocks->getBlock(prev)) p->next.clear();
            } else if (!parent.empty()) {
                if (Block* p = blocks->getBlock(parent)) {
                    for (auto& [nm, in] : p->inputs.items)
                        if (in.block == id) in.block.clear();
                }
            }
            if (b->topLevel) { blocks->deleteScript(id); }
        }
        blocks->deleteBlock(id);
        blocks->resetCache();
        if (selBlock == id) selBlock.clear();
    }
    /// 単一ブロック削除（next 側を残す）
    void deleteSelectedBlock() {
        if (selBlock.empty()) return;
        Target* t = selected();
        if (!t || !t->blocks) return;
        pushUndo();
        Blocks* blocks = t->blocks;
        const std::string delId = selBlock;
        Block* sb = blocks->getBlock(delId);
        if (sb) {
            const std::string nxt = sb->next;
            const bool wasTop = sb->topLevel;
            std::string parentId = bl::prevOf(blocks, delId);
            sb->next.clear();
            if (!parentId.empty()) {
                if (Block* pb = blocks->getBlock(parentId)) pb->next = nxt;
            }
            if (!nxt.empty()) {
                if (Block* nb = blocks->getBlock(nxt)) {
                    nb->parent = parentId;
                    if (wasTop) { nb->topLevel = true; blocks->addScript(nxt); }
                }
            }
            blocks->deleteBlock(delId);
        }
        selBlock.clear();
    }
    // ---- 複製 ----
    std::string dupRec(Blocks* blocks, const std::string& srcId,
                       std::unordered_map<std::string, std::string>& map,
                       bool head) {
        auto it = map.find(srcId);
        if (it != map.end()) return it->second;
        Block* sp = blocks->getBlock(srcId);
        if (!sp) return "";
        Block nb = *sp;
        std::string oldNext = nb.next;
        std::vector<std::pair<std::string, std::string>> ins;
        for (auto& [name, in] : nb.inputs.items)
            ins.push_back({in.block, in.shadow});
        nb.id = newBlockId();
        map[srcId] = nb.id;
        nb.next.clear();
        nb.parent.clear();
        nb.topLevel = head;
        std::string newNext =
            oldNext.empty() ? "" : dupRec(blocks, oldNext, map, false);
        std::vector<std::pair<std::string, std::string>> newIns;
        for (auto& [ob, os] : ins)
            newIns.push_back({ob.empty() ? "" : dupRec(blocks, ob, map, false),
                              os.empty() ? "" : dupRec(blocks, os, map, false)});
        for (size_t i = 0; i < nb.inputs.items.size() && i < newIns.size(); ++i) {
            nb.inputs.items[i].second.block = newIns[i].first;
            nb.inputs.items[i].second.shadow = newIns[i].second;
        }
        nb.next = newNext;
        std::string id = nb.id;
        blocks->createBlock(nb);
        return id;
    }
    void duplicateBlock(const std::string& id) {
        Target* t = selected();
        if (!t || !t->blocks || id.empty()) return;
        Blocks* blocks = t->blocks;
        pushUndo();
        std::unordered_map<std::string, std::string> map;
        std::string nid = dupRec(blocks, id, map, true);
        blocks->resetCache();
        if (!nid.empty()) selBlock = nid;
    }

    // ---- フィールド編集（dropdown / variable / 数値・テキスト shadow）-------
    void advanceField(Target* t, Block* b, int argIdx) {
        const edit::BlockDef* d = edit::findBlock(b->opcode);
        if (!d || argIdx < 0 || argIdx >= (int)d->args.size()) return;
        const edit::ArgDef& a = d->args[argIdx];
        if (a.kind == edit::ArgKind::BooleanField) {
            bool cur = false;
            auto it = b->fields.find(a.name);
            if (it != b->fields.end()) cur = it->second.value.asBool();
            pushUndo();
            b->fields[a.name] = BlockField{Value::boolean(!cur)};
            t->blocks->resetCache();
        }
    }
    /// ドロップダウンを実メニューで開く（Dropdown の選択肢 / Variable の変数名）。
    void openDropdownMenu(const std::string& blockId, const bl::LSeg& g,
                          float sx, float sy) {
        Target* t = selected();
        if (!t || !t->blocks) return;
        Block* b = t->blocks->getBlock(blockId);
        if (!b) return;
        menu_.items.clear();
        menu_.blockId = blockId;
        menu_.fieldName = g.field;
        menu_.variable = false;
        if (g.kind == bl::SegKind::Variable) {
            menu_.variable = true;
            for (auto& [vid, v] : t->variables) {
                bool isList = v.type == Variable::kListType;
                if (isList != g.list) continue;
                menu_.items.push_back(utf16ToUtf8(v.name));
                menu_.ids.push_back(vid);
            }
        } else {
            const edit::BlockDef* d = edit::findBlock(b->opcode);
            if (!d || g.arg < 0 || g.arg >= (int)d->args.size()) return;
            const edit::ArgDef& a = d->args[g.arg];
            for (size_t i = 0; i < a.options.size(); i += 2)
                menu_.items.push_back(a.options[i]);
            for (size_t i = 0; i < a.options.size(); i += 2)
                menu_.ids.push_back(a.options.size() > i + 1 ? a.options[i + 1]
                                                           : a.options[i]);
        }
        menu_.open = true;
        menu_.x = sx;
        menu_.y = sy;
        menu_.hover = -1;
        // blocklyDropDownDiv: 背景=ブロック primary、枠=tertiary
        bl::BlockStyle st = bl::styleOf(t->blocks, b);
        menu_.bg = st.primary;
        menu_.border = st.tertiary;
        menu_.checked = -1;
        // 現在値を強調
        std::string cur = bl::fieldStr(b, g.field);
        std::string curId = b->fields.count(g.field) ? b->fields.at(g.field).id : "";
        for (size_t i = 0; i < menu_.ids.size(); ++i) {
            if (menu_.variable ? menu_.ids[i] == curId
                              : menu_.ids[i] == cur) {
                menu_.checked = (int)i;
                break;
            }
        }
    }
    void runMenuItem(int i) {
        if (!menu_.open || i < 0 || i >= (int)menu_.items.size()) return;
        Target* t = selected();
        if (!t || !t->blocks) { menu_.open = false; return; }
        Block* b = t->blocks->getBlock(menu_.blockId);
        if (b) {
            pushUndo();
            if (menu_.variable) {
                // 変数名は display、id は variable id
                std::string nm;
                for (auto& [vid, v] : t->variables)
                    if (vid == menu_.ids[i]) { nm = utf16ToUtf8(v.name); break; }
                BlockField f{Value::string(SStr::fromUtf8(nm))};
                f.id = menu_.ids[i];
                b->fields[menu_.fieldName] = f;
            } else {
                b->fields[menu_.fieldName] =
                    BlockField{Value::string(SStr::fromUtf8(menu_.ids[i]))};
            }
            t->blocks->resetCache();
        }
        menu_.open = false;
    }
    /// 数値/テキスト shadow をインライン編集開始。
    void startEditingInputOf(Block* b) {
        if (!b) return;
        Target* t = selected();
        if (!t || !t->blocks) return;
        Blocks* blocks = t->blocks;
        for (auto& [iname, bi] : b->inputs.items) {
            if (bi.block.empty()) continue;
            Block* sh = blocks->getBlock(bi.block);
            if (!sh || !sh->shadow) continue;
            std::string fn;
            bool isNum = false;
            if (bl::shadowIsField(sh, &fn, &isNum)) {
                edit_.active = true;
                edit_.blockId = sh->id;
                edit_.field = fn;
                edit_.numeric = isNum;
                edit_.buf = bl::fieldStr(sh, fn);
                SDL_StartTextInput();
                return;
            }
        }
        // 自身が shadow の math_* / text
        std::string fn;
        bool isNum = false;
        if (b->shadow && bl::shadowIsField(b, &fn, &isNum)) {
            edit_.active = true;
            edit_.blockId = b->id;
            edit_.field = fn;
            edit_.numeric = isNum;
            edit_.buf = bl::fieldStr(b, fn);
            SDL_StartTextInput();
        }
    }
    void startEditingField(Block* b, const bl::LSeg& g) {
        if (!b) return;
        // フィールド箱の中身（shadow または自身の field）
        Target* t = selected();
        if (!t || !t->blocks) return;
        // 対象ブロックを決める: input 名があればその shadow、無ければ自身
        Block* target = b;
        if (!g.input.empty()) {
            const BlockInput* in = b->inputs.find(g.input);
            if (in && !in->block.empty()) {
                Block* sh = t->blocks->getBlock(in->block);
                if (sh && sh->shadow) target = sh;
            }
        }
        std::string fn;
        bool isNum = false;
        if (target->shadow) {
            if (!bl::shadowIsField(target, &fn, &isNum)) return;
        } else {
            fn = g.field;
            if (fn.empty()) return;
            const edit::BlockDef* d = edit::findBlock(target->opcode);
            if (d)
                for (const auto& a : d->args)
                    if (a.name == fn)
                        isNum = (a.kind == edit::ArgKind::Number ||
                                 a.kind == edit::ArgKind::Angle);
        }
        edit_.active = true;
        edit_.blockId = target->id;
        edit_.field = fn;
        edit_.numeric = isNum;
        edit_.buf = bl::fieldStr(target, fn);
        SDL_StartTextInput();
    }
    void applyEdit() {
        Target* t = selected();
        if (t && t->blocks && edit_.active) {
            if (Block* b = t->blocks->getBlock(edit_.blockId)) {
                pushUndo();
                if (edit_.numeric) {
                    char* end = nullptr;
                    double dv = std::strtod(edit_.buf.c_str(), &end);
                    if (end != edit_.buf.c_str() && *end == 0)
                        b->fields[edit_.field] = BlockField{Value::num(dv)};
                    else
                        b->fields[edit_.field] =
                            BlockField{Value::string(SStr::fromUtf8(edit_.buf))};
                } else {
                    b->fields[edit_.field] =
                        BlockField{Value::string(SStr::fromUtf8(edit_.buf))};
                }
                t->blocks->resetCache();
            }
        }
        edit_.active = false;
        edit_.buf.clear();
        SDL_StopTextInput();
    }

    // ---- コメント ----
    void startEditingComment(const std::string& id) {
        Target* t = selected();
        if (!t || !t->blocks) return;
        Block* b = t->blocks->getBlock(id);
        if (!b) return;
        cmt_.active = true;
        cmt_.blockId = id;
        cmt_.buf = b->comment;
        SDL_StartTextInput();
    }
    void applyCommentEdit() {
        Target* t = selected();
        if (t && t->blocks) {
            if (Block* b = t->blocks->getBlock(cmt_.blockId)) {
                pushUndo();
                b->comment = cmt_.buf;
                t->blocks->resetCache();
            }
        }
        cmt_.active = false;
        cmt_.buf.clear();
        cmt_.blockId.clear();
    }

    // ---- バックパック ----
    struct BpItem { std::string json, label; };
    std::vector<BpItem> backpack;
    bool backpackReadOnly = false;
    bool bpOpen = false;
    std::string backpackPath() const {
        if (const char* p = std::getenv("XYO_BACKPACK")) return p;
        const char* h = std::getenv("HOME");
        return h ? (std::string(h) + "/.xyo-backpack.json") : std::string();
    }
    void backpackSave() {
        if (backpackReadOnly) return;
        std::string p = backpackPath();
        if (p.empty()) return;
        JsonArray arr;
        for (auto& it : backpack) {
            JsonObject o;
            o["label"] = JsonValue(it.label);
            o["json"] = JsonValue(it.json);
            arr.push_back(JsonValue(std::move(o)));
        }
        std::string s = jsonStringify(JsonValue(std::move(arr)));
        FILE* f = fopenUtf8(p.c_str(), "wb");
        if (!f) return;
        std::fwrite(s.data(), 1, s.size(), f);
        std::fclose(f);
    }
    void backpackLoad() {
        std::string p = backpackPath();
        if (p.empty()) return;
        FILE* f = fopenUtf8(p.c_str(), "rb");
        if (!f) return;
        std::string s;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        std::fclose(f);
        JsonValue v = jsonParse(s);
        if (!v.isArray()) return;
        for (const auto& e : v.asArray()) {
            BpItem it;
            it.label = e.get("label").asString();
            it.json = e.get("json").asString();
            if (!it.json.empty()) backpack.push_back(std::move(it));
        }
    }
    void backpackAdd(const std::string& id) {
        Target* t = selected();
        if (!t || !t->blocks || id.empty()) return;
        Block* b = t->blocks->getBlock(id);
        if (!b) return;
        BpItem it;
        it.json = jsonStringify(saveBlocksFragment(*t->blocks, id));
        const edit::BlockDef* d = edit::findBlock(b->opcode);
        it.label = d ? bl::shortBlockLabel(d) : b->opcode;
        backpack.push_back(std::move(it));
        backpackSave();
    }
    void backpackPaste(int i) {
        Target* t = selected();
        if (!t || !t->blocks) return;
        if (i < 0 || i >= (int)backpack.size()) return;
        JsonValue frag = jsonParse(backpack[i].json);
        if (!frag.isObject()) return;
        pushUndo();
        std::string head;
        loadBlocksFragment(*t->blocks, frag, &head);
        if (head.empty()) return;
        if (Block* h = t->blocks->getBlock(head)) {
            h->topLevel = true;
            h->parent.clear();
            // ドロップ位置はビュー中央
            float wx = (ui.ws.x + ui.ws.w / 2 - panX) / wsScale();
            float wy = (ui.ws.y + ui.ws.h / 2 - panY) / wsScale();
            h->x = wx;
            h->y = wy;
            t->blocks->addScript(head);
        }
        t->blocks->resetCache();
        selBlock = head;
    }
    int backpackAt(float mx, float my) {
        float x = ui.bpPanel.x + 8;
        float y = ui.bpPanel.y + 8;
        for (int i = 0; i < (int)backpack.size(); ++i) {
            if (mx >= x && mx < x + 64 && my >= y && my < y + 72) return i;
            x += 64 + 8;
        }
        return -1;
    }

    // ---- メニュー / コンテキスト / テキスト編集 ------------------------------
    struct Menu {
        bool open = false;
        float x = 0, y = 0;
        int hover = -1;
        std::string blockId, fieldName;
        bool variable = false;
        std::vector<std::string> items;   // 表示名
        std::vector<std::string> ids;     // 実値（variable id / option value）
        int w = 180;
        int itemH = 28;
        int checked = -1;                 // 現在値（チェックマーク）
        Color bg{255, 255, 255, 255};     // ブロック primary
        Color border{120, 120, 120, 255}; // ブロック tertiary
    } menu_;

    // ---- メニューバーの File/Edit ドロップダウン -----------------------------
    struct TopMenu {
        bool open = false;
        int which = -1;              // 0=File 1=Edit 2=Add Extension
        float x = 0, y = 0;
        int hover = -1;
        int w = 220;
        int itemH = 30;
    } topMenu_;
    std::vector<std::string> topMenuList(int which) {
        // file-menu.jsx / edit-menu.jsx（canManageFiles）準拠
        if (which == 0)
            return {"New", "Load from your computer",
                    "Save to your computer"};
        if (which == 1)
            return {"Restore deleted",
                    (rt && rt->turboMode) ? "Turn off Turbo Mode"
                                          : "Turn on Turbo Mode"};
        if (which == 2)
            return {"Pen", "Music"};
        if (which == 3)
            return {"Choose a Sprite", "Paint", "Surprise", "Upload"};
        if (which == 4)
            return {"Choose a Backdrop", "Paint", "Surprise", "Upload"};
        if (which == 6)
            return {"Choose a Costume", "Paint", "Surprise", "Upload"};
        if (which == 7)
            return {"Choose a Sound", "Upload"};
        if (which == 5)
            // settings-menu.jsx: Language / Theme / Color Mode の
            // サブメニュー（本実装は英語のみ・テーマ変更なし）
            return {"Language ▶", "Theme ▶", "Color Mode ▶"};
        return {"Undo", "Redo"};
    }
    FRect addExtRect_{0, 0, 0, 0};   // ツールボックス下部の Add Extension
    FRect menuBtnFile, menuBtnEdit, menuBtnGear, menuBtnSave; // drawMenuBar で埋める
    bool newRequested_ = false;        // File→New でデフォルトプロジェクトに切替
    bool reloadRequested_ = false;

    void openTopMenu(int which, float x, float y) {
        topMenu_.open = true;
        topMenu_.which = which;
        topMenu_.x = x;
        topMenu_.y = y;
        topMenu_.hover = -1;
    }
    void runTopMenuItem(int i) {
        int which = topMenu_.which;
        topMenu_.open = false;
        if (which == 0) {
            if (i == 0) {   // New — デフォルトプロジェクトへ
                newRequested_ = true;
            } else if (i == 1) {   // Load from your computer
                reloadRequested_ = true;
            } else if (i == 2) {   // Save to your computer
                if (rt && !path.empty()) {
                    bool ok = saveSb3File(*rt, path.c_str());
                    std::fprintf(stderr, "editor: saved %s (%s)\n",
                                 path.c_str(), ok ? "ok" : "FAILED");
                }
            }
        } else if (which == 1) {
            if (i == 1 && rt) rt->turboMode = !rt->turboMode;
            // i==0 Restore deleted は実装外（現状 no-op）
        } else if (which == 2) {
            // Add Extension — カテゴリをツールボックスへ恒常追加
            const char* id = i == 0 ? "pen" : "music";
            bool exists = false;
            for (const auto& c : cats_)
                if (c.id == id) { exists = true; break; }
            if (!exists) {
                for (const auto& c : edit::kExtensionCategories)
                    if (c.id == id) cats_.push_back(c);
                flyDirty_ = true;
            }
        } else if (which == 3) {
            // スプライト追加メニュー（Choose/Paint/Surprise/Upload → 新規スプライト）
            addSprite();
        } else if (which == 4) {
            // 背景追加メニュー（Choose/Paint/Surprise/Upload → 新規背景）
            addBackdrop();
        } else if (which == 6) {
            addCostume();
        } else if (which == 7) {
            addSound();
        }
    }

    struct EditBox {
        bool active = false;
        std::string blockId, field, buf;
        bool numeric = false;
    } edit_;

    struct CmtEdit {
        bool active = false;
        std::string blockId, buf;
    } cmt_;

    struct SpriteEdit {
        bool active = false;
        int which = -1;   // 0=name 1=x 2=y 3=dir 4=size
        std::string buf;
    } spEdit_;

    /// コスチューム/サウンド名フィールド（fixed-tools.jsx / sound-editor.jsx
    /// の <Input>）。which: 6=costume 7=sound。
    struct NameEdit {
        bool active = false;
        int which = -1;
        int idx = -1;
        std::string buf;
    } nmEdit_;
    FRect nmNameRect_{0, 0, 0, 0};

    /// スプライトタイルの reorder ドラッグ（sprite-selector-item.jsx の
    /// DragRecognizer）。ti は rt->targets の index。
    struct SpriteDrag {
        bool pending = false, active = false;
        int ti = -1;
        float sx = 0, sy = 0;      // 掴んだ点
        float mx = 0, my = 0;      // 現在位置
        int insert = -1;           // 非 stage スプライト順の挿入 index
    } sdr_;

    struct DirectionPickerDlg {
        bool open = false;
        float x = 0, y = 0; // ポップオーバー左上
        float w = 150, h = 175;
        float dialCx = 0, dialCy = 0, dialR = 48;
        bool dragging = false;
        FRect btnAll, btnLr, btnNo;
    } dirPicker_;

    // Scratch paint の color-picker.jsx: HSB スライダ3本 + nofill/
    // eyedropper スウォッチ + (fill のみ) gradient 4 種選択。
    struct ColorPickerDlg {
        bool open = false;
        int which = 0;        // 0=Fill 1=Stroke
        float x = 0, y = 0;
        float w = 220, h = 0;
        float hue = 0, sat = 0, bri = 0;   // 0..100
        float hue2 = 0, sat2 = 0, bri2 = 0;
        int gradient = 0;     // 0 solid 1 horz 2 vert 3 radial
        int colorIndex = 0;
        bool dragging = false;
        int dragSlider = -1;
        FRect slH, slS, slB;
        FRect btnSolid, btnHorz, btnVert, btnRadial;
        FRect sw1, sw2, btnSwap;
        FRect noFillR, eyeR;
    } cp_;
    FRect cpFillRect_{0,0,0,0}, cpStrokeRect_{0,0,0,0};

    // scratch-gui ActionMenu 展開状態（action-menu.css/jsx 準拠）:
    // 44px main-button の直上に 36px more-buttons を縦積み
    // （$looks-secondary-dark の縦ピル、角丸 top、左に白 tooltip）。
    struct ActionMenuDlg {
        bool open = false;
        int which = -1;   // topMenu which と同じ（3/4/6/7）
        float cx = 0;     // 列の中心 x（main-button と揃う）
        float mainY = 0;  // main-button 上端
        std::vector<FRect> items;
        int hover = -1;
    } am_;

    /// HSV(0..100,0..100,0..100) → Color（scratch-paint slider グラデ用）
    static Color hsb(float h, float s, float v) {
        h = std::fmod(h, 100.f) / 100.f * 360.f;
        s = std::clamp(s, 0.f, 100.f) / 100.f;
        v = std::clamp(v, 0.f, 100.f) / 100.f;
        float c = v * s, x2 = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f));
        float m = v - c, rr = 0, gg = 0, bb = 0;
        if (h < 60)      { rr = c;  gg = x2; bb = 0;  }
        else if (h < 120){ rr = x2; gg = c;  bb = 0;  }
        else if (h < 180){ rr = 0;  gg = c;  bb = x2; }
        else if (h < 240){ rr = 0;  gg = x2; bb = c;  }
        else if (h < 300){ rr = x2; gg = 0;  bb = c;  }
        else             { rr = c;  gg = 0;  bb = x2; }
        return Color{(uint8_t)((rr + m) * 255), (uint8_t)((gg + m) * 255),
                     (uint8_t)((bb + m) * 255), 255};
    }

    struct Ctx {
        bool open = false;
        float x = 0, y = 0;
        int hover = -1;
        std::string blockId;
        int spriteIdx = -1;              // >=0 ならスプライトタイルのメニュー
        int assetKind = 0, assetIdx = -1; // 1=コスチューム/2=サウンドタイル
        std::vector<std::string> items;
        std::vector<bool> enabled;
        std::vector<bool> danger;        // true は hover 時 error-primary(#FF8C1A)
        float wx = 0, wy = 0;   // ws 座標（paste 位置）
        int w = 170, itemH = 24;
    } ctx_;

    // ---- フライアウト ---------------------------------------------------------
    // テンプレを専用 Blocks に実体化して本物の形状でレイアウト/描画する。
    struct FlyItem {
        enum Kind { BlockT, Header, Button, VarRow, ListRow, Divider } kind = BlockT;
        std::string opcode;
        std::string text;                   // Header/Button/VarRow の文字
        std::string varId;                  // VarRow/ListRow
        bool list = false;
        std::map<std::string, BlockField> initFields;  // BlockT の初期フィールド
        std::optional<Mutation> mutation;   // procedures_call
        std::map<std::string, std::string> argTexts; // 引数 input → 表示文字列
        bool makeBlock = false;             // Button: Make a Block
        bl::LBlock lb;                      // BlockT: テンプレレイアウト
        bl::BlockStyle st;
        float y = 0, h = 0;                 // flyout 内の y/高さ（論理）
    };
    std::vector<FlyItem> flyItems_;
    std::string flyCat_;
    int flyTarget_ = -1;
    bool flyDirty_ = true;

    /// flyout のテンプレ用にブロック実体を作る（永続しないローカル Blocks）。
    void makeFlyItem(Blocks* tmp, const std::string& opcode,
                     const std::map<std::string, BlockField>& init,
                     FlyItem& fi) {
        fi.kind = FlyItem::BlockT;
        fi.opcode = opcode;
        fi.initFields = init;
        Block b;
        b.id = "fly_" + opcode + "_" + std::to_string(tmp->size());
        b.opcode = opcode;
        b.topLevel = true;
        setupBlockDefaults(b, tmp);
        bindVariableField(b, selected());
        for (auto& [k, v] : init) b.fields[k] = v;
        tmp->createBlock(b);
        bl::BlockSize sz = bl::measureBlock(tmp, tmp->getBlock(b.id), tr());
        fi.lb.id = b.id;
        fi.lb.block = tmp->getBlock(b.id);
        fi.lb.w = sz.w;
        fi.lb.h = sz.h;
        fi.lb.headerH = sz.headerH;
        fi.lb.segs = sz.segs;
        fi.lb.shape = fi.st.shape = bl::styleOf(tmp, fi.lb.block).shape;
        fi.st = bl::styleOf(tmp, fi.lb.block);
        fi.lb.cblock = fi.st.cblock;
        fi.lb.hat = fi.st.shape == edit::Shape::Hat;
        fi.lb.noBump = fi.st.shape == edit::Shape::End;
        fi.h = sz.h + (fi.lb.noBump ? 0 : bl::kNotchH);
        // C-block の口（レイアウトに依存するので再構成は不要 — 空で描画）
        if (fi.st.cblock >= 1) {
            bl::LMouth m;
            m.branch = 0;
            m.x = bl::kArmW;
            m.y = sz.headerH;
            m.w = sz.w - bl::kArmW;
            m.innerH = bl::kMouthHMin;
            fi.lb.mouths.push_back(m);
            if (fi.st.cblock >= 2) {
                fi.lb.sub2Label = fi.st.sub2Label;
                fi.lb.sub2LabelY = sz.headerH + bl::kMouthHMin;
                bl::LMouth m2;
                m2.branch = 1;
                m2.x = bl::kArmW;
                m2.y = sz.headerH + bl::kMouthHMin + bl::kStackH;
                m2.w = sz.w - bl::kArmW;
                m2.innerH = bl::kMouthHMin;
                fi.lb.mouths.push_back(m2);
            }
        }
    }

    /// 選択中カテゴリ/ターゲットのフライアウト項目を再構成する。
    void rebuildFlyout() {
        flyItems_.clear();
        if (catSel < 0 || catSel >= catCount()) return;
        const auto& cat = catAt(catSel);
        Blocks tmp(rt);
        float y = 12;   // flyout MARGIN=12
        auto& items = flyItems_;
        Target* t = selected();
        if (cat.id == "data") {
            // 変数: Make a Variable / チェックボックス + 変数記者
            FlyItem mkv;
            mkv.kind = FlyItem::Button;
            mkv.text = "Make a Variable";
            mkv.list = false;
            mkv.y = y; mkv.h = 40;   // blocklyFlyoutButton 高さ 40
            items.push_back(mkv);
            y += 52;
            if (t) {
                for (auto& [vid, v] : t->variables) {
                    if (v.type == Variable::kListType) continue;
                    FlyItem fi;
                    fi.kind = FlyItem::VarRow;
                    fi.varId = vid;
                    fi.text = utf16ToUtf8(v.name);
                    fi.y = y; fi.h = 34;
                    items.push_back(fi);
                    y += 40;
                }
            }
            for (const auto& op : cat.items) {
                const edit::BlockDef* d = edit::findBlock(op);
                if (!d) continue;
                if (d->shape == edit::Shape::Reporter) {
                    // data_variable / data_listcontents は VarRow で代替
                    bool isListRep = op == "data_listcontents";
                    bool hasVar = false;
                    if (t) for (auto& [vid, v] : t->variables)
                        if ((v.type == Variable::kListType) == isListRep)
                            hasVar = true;
                    if (!hasVar) continue;
                    // 変数記者は VarRow のブロック部分で描くのでスキップ
                    continue;
                }
                FlyItem fi;
                makeFlyItem(&tmp, op, {}, fi);
                fi.y = y;
                items.push_back(fi);
                y += fi.h + 12;   // GAP_Y=12
            }
            // リスト
            FlyItem dv;
            dv.kind = FlyItem::Divider;
            dv.y = y; dv.h = 12;
            items.push_back(dv);
            y += 18;
            FlyItem mkl;
            mkl.kind = FlyItem::Button;
            mkl.text = "Make a List";
            mkl.list = true;
            mkl.y = y; mkl.h = 40;
            items.push_back(mkl);
            y += 52;
            if (t) {
                for (auto& [vid, v] : t->variables) {
                    if (v.type != Variable::kListType) continue;
                    FlyItem fi;
                    fi.kind = FlyItem::ListRow;
                    fi.varId = vid;
                    fi.text = utf16ToUtf8(v.name);
                    fi.y = y; fi.h = 34;
                    items.push_back(fi);
                    y += 40;
                }
            }
            flyCat_ = cat.id;
            flyTarget_ = selTarget;
            return;
        }
        if (cat.id == "more") {
            // My Blocks: Make a Block ボタン + 定義済み手続きの呼出し
            FlyItem mkb;
            mkb.kind = FlyItem::Button;
            mkb.text = "Make a Block";
            mkb.makeBlock = true;
            mkb.y = y; mkb.h = 40;
            items.push_back(mkb);
            y += 52;
            if (t && t->blocks) {
                // 一意の proccode でソートした手続き一覧
                std::vector<const Block*> protos;
                for (auto& [bid, pb] : t->blocks->allBlocks())
                    if (pb.opcode == "procedures_prototype" && pb.mutation &&
                        pb.mutation->has("proccode"))
                        protos.push_back(&pb);
                std::sort(protos.begin(), protos.end(),
                          [](const Block* a, const Block* b) {
                              return a->mutation->attr("proccode") <
                                     b->mutation->attr("proccode");
                          });
                for (const Block* proto : protos) {
                    FlyItem fi;
                    fi.kind = FlyItem::BlockT;
                    fi.opcode = "procedures_call";
                    fi.mutation = *proto->mutation;
                    fi.mutation->parsed = false;
                    // 引数入力 → 引数名（%b は空 = 六角ソケット）
                    fi.mutation->parseArgs();
                    int i = 0;
                    // proccode の %s/%b を順に辿って bool を判定
                    const std::string& pc = proto->mutation->attr("proccode");
                    for (size_t k = 0; k + 1 < pc.size() &&
                                     i < (int)fi.mutation->argumentIds.size();
                         ++k) {
                        if (pc[k] != '%' ||
                            (pc[k+1] != 's' && pc[k+1] != 'b'))
                            continue;
                        bool isB = pc[k+1] == 'b';
                        const std::string& inm = fi.mutation->argumentIds[i];
                        if (!isB && i < (int)fi.mutation->argumentNames.size())
                            fi.argTexts[inm] = SStr::toUtf8(
                                fi.mutation->argumentNames[i]);
                        ++i;
                    }
                    // テンプレ block を flyout Blocks に作って測量
                    Block b;
                    b.id = "fly_proc_" + std::to_string(tmp.size());
                    b.opcode = "procedures_call";
                    b.topLevel = true;
                    b.mutation = fi.mutation;
                    for (auto& [inm, txt] : fi.argTexts) {
                        if (txt.empty()) continue;
                        std::string sid =
                            makeShadowBlock(&tmp, "text", "TEXT", txt, b.id);
                        BlockInput bi;
                        bi.block = sid; bi.shadow = sid;
                        b.inputs.set(inm, bi);
                    }
                    tmp.createBlock(b);
                    bl::BlockSize sz =
                        bl::measureBlock(&tmp, tmp.getBlock(b.id), tr());
                    fi.lb.id = b.id;
                    fi.lb.block = tmp.getBlock(b.id);
                    fi.lb.w = sz.w;
                    fi.lb.h = sz.h;
                    fi.lb.headerH = sz.headerH;
                    fi.lb.segs = sz.segs;
                    fi.st = bl::styleOf(&tmp, fi.lb.block);
                    fi.lb.shape = fi.st.shape;
                    fi.lb.cblock = 0;
                    fi.h = sz.h + bl::kNotchH;
                    fi.y = y;
                    items.push_back(fi);
                    y += fi.h + 12;
                }
            }
            flyCat_ = cat.id;
            flyTarget_ = selTarget;
            return;
        }
        for (const auto& op : cat.items) {
            const edit::BlockDef* d = edit::findBlock(op);
            if (!d) continue;
            FlyItem fi;
            makeFlyItem(&tmp, op, {}, fi);
            fi.y = y;
            items.push_back(fi);
            y += fi.h + 12;   // GAP_Y=12
        }
        flyCat_ = cat.id;
        flyTarget_ = selTarget;
        flyDirty_ = false;
    }
    float flyContentH() const {
        float m = 0;
        for (auto& f : flyItems_) m = std::max(m, f.y + f.h);
        return m + 20;
    }
    FlyItem* flyItemAt(float mx, float my) {
        for (auto& f : flyItems_) {
            float sy = ui.flyout.y + f.y * bl::kWsZoom - flyScroll;
            if (my < sy || my >= sy + f.h * bl::kWsZoom) continue;
            if (f.kind == FlyItem::BlockT) {
                float bx = ui.flyout.x + 10;
                if (mx >= bx && mx < bx + f.lb.w * bl::kWsZoom) return &f;
            } else {
                if (mx >= ui.flyout.x && mx < ui.flyout.x + ui.flyout.w)
                    return &f;
            }
        }
        return nullptr;
    }

    // ---- ワークスペースレイアウト ------------------------------------------
    bl::ScriptLayout wsLayout_;
    int wsTarget_ = -1;
    bool wsDirty_ = true;
    void rebuildWs() {
        Target* t = selected();
        if (!t || !t->blocks) { wsLayout_.blocks.clear(); return; }
        wsLayout_ = bl::layoutWorkspace(t->blocks, tr());
        wsTarget_ = selTarget;
        wsDirty_ = false;
    }
    bl::LBlock* wsBlockAt(float mx, float my) {
        bl::LBlock* best = nullptr;
        const float s = wsScale();
        for (auto& lb : wsLayout_.blocks) {
            if (lb.shadow) continue;
            const float lx = (mx - panX) / s - lb.x;
            const float ly = (my - panY) / s - lb.y;
            if (lx < 0 || lx >= lb.w) continue;
            bool inside = ly >= 0 && ly < lb.fullH;
            // Hat domes extend ABOVE the logical row origin. Test the actual
            // outline there so grabbing the visible dome preserves its offset.
            if (!inside && lb.hat && ly < 0 && ly >= -48) {
                const Path path = bl::blockOutline(lb);
                if (path.pts.size() >= 3) {
                    for (size_t i = 0, j = path.pts.size() - 1;
                         i < path.pts.size(); j = i++) {
                        const auto& a = path.pts[i];
                        const auto& b = path.pts[j];
                        if ((a.y > ly) != (b.y > ly) &&
                            lx < (b.x - a.x) * (ly - a.y) / (b.y - a.y) + a.x)
                            inside = !inside;
                    }
                }
            }
            if (inside && (!best || lb.depth > best->depth)) best = &lb;
        }
        return best;
    }

    // ---- ドラッグ -----------------------------------------------------------
    enum class DragKind { None, Chain, ValueInput, Substack, TopLevel };
    struct Drag {
        bool active = false;
        bool pending = false;
        bool fromFlyout = false;
        FlyItem* flyItem = nullptr;
        std::string opcode;
        std::map<std::string, BlockField> initFields;
        std::optional<Mutation> mutation;          // procedures_call 用
        std::map<std::string, std::string> argTexts;
        std::string headId, tailId;
        DragKind kind = DragKind::None;
        std::string prevId, tailNext, prevParent, prevInput;
        bool wasTop = false;
        edit::Shape shape = edit::Shape::Stack;
        float mx = 0, my = 0;        // ws 論理
        float offX = 0, offY = 0;    // 掴んだ位置 − head の ws 座標
        std::string targetId;
        bool before = false, valueTarget = false, substackTarget = false;
        int subBranch = 0;
        std::string valueInput;
        bool hasSnap = false;
        float snapX = 0, snapY = 0;
        float snapHeadX = 0, snapHeadY = 0; // head pose matching the snapped connector
        float chainH = 0, headH = 0;
        std::vector<bl::LBlock> previewLayout; // origin-relative geometry, built once
        bool connectionsReady = false, hasPrevious = true, hasNext = true;
        bool snapshotTaken = false;
        // flyout ドラッグの描画用テンプレ
        bl::LBlock tmpl;
        bl::BlockStyle tmplSt;
    } drag;

    edit::Shape blockShape(const std::string& id) {
        Target* t = selected();
        return bl::blockShapeOf(t ? t->blocks : nullptr, id);
    }

    void detachDrag() {
        Target* t = selected();
        if (!t || !t->blocks) return;
        Blocks* blocks = t->blocks;
        Block* b = blocks->getBlock(drag.headId);
        if (!b) return;
        drag.shape = blockShape(drag.headId);
        drag.connectionsReady = false;
        bool isReporter = drag.shape == edit::Shape::Reporter ||
                          drag.shape == edit::Shape::Boolean;
        drag.wasTop = b->topLevel;
        drag.prevId.clear(); drag.tailNext.clear();
        drag.prevParent.clear(); drag.prevInput.clear();

        auto substackBranch = [&](Block* p) -> int {
            if (!p) return -1;
            for (int br = 0; br < 2; br++) {
                const char* nm = br == 0 ? "SUBSTACK" : "SUBSTACK2";
                const BlockInput* in = p->inputs.find(nm);
                if (in && in->block == drag.headId) return br;
            }
            return -1;
        };
        drag.tailId = bl::chainTail(blocks, drag.headId);
        Block* tl = blocks->getBlock(drag.tailId);
        drag.tailNext = tl ? tl->next : "";

        if (isReporter) {
            if (!b->parent.empty()) {
                drag.kind = DragKind::ValueInput;
                drag.prevParent = b->parent;
                if (Block* p = blocks->getBlock(drag.prevParent)) {
                    for (auto& [iname, bi] : p->inputs.items)
                        if (bi.block == drag.headId) {
                            drag.prevInput = iname;
                            bi.block = bi.shadow;
                            break;
                        }
                }
            } else {
                drag.kind = DragKind::TopLevel;
                if (drag.wasTop) { blocks->deleteScript(drag.headId); b->topLevel = false; }
            }
            b->parent.clear();
        } else {
            drag.prevId = bl::prevOf(blocks, drag.headId);
            if (!drag.prevId.empty()) {
                drag.kind = DragKind::Chain;
                if (Block* px = blocks->getBlock(drag.prevId)) px->next = drag.tailNext;
            } else if (!b->parent.empty() &&
                       substackBranch(blocks->getBlock(b->parent)) >= 0) {
                drag.kind = DragKind::Substack;
                drag.prevParent = b->parent;
                drag.prevInput = substackBranch(blocks->getBlock(b->parent)) == 0
                                     ? "SUBSTACK" : "SUBSTACK2";
                if (Block* p = blocks->getBlock(b->parent)) {
                    BlockInput* in = p->inputs.find(drag.prevInput);
                    if (in) in->block = drag.tailNext;
                }
            } else {
                drag.kind = DragKind::TopLevel;
                if (drag.wasTop) { blocks->deleteScript(drag.headId); b->topLevel = false; }
            }
            if (tl) tl->next.clear();
            if (!drag.tailNext.empty())
                if (Block* g = blocks->getBlock(drag.tailNext))
                    g->parent = drag.prevId.empty() ? b->parent : drag.prevId;
            b->parent.clear();
        }
        blocks->resetCache();
        wsDirty_ = true;
    }

    // 接続点候補（ws 論理座標）
    struct Conn {
        int kind = 0;      // 0=next(下端) 1=prev(上端) 2=value 3=statement
        std::string blockId, inputName;
        int branch = 0;
        float x = 0, y = 0;
    };
    void collectConnections(Blocks* blocks, const std::set<std::string>& exclude,
                            std::vector<Conn>& out) {
        if (wsDirty_) rebuildWs();
        for (auto& lb : wsLayout_.blocks) {
            if (exclude.count(lb.id) || lb.shadow) continue;
            Block* b = blocks->getBlock(lb.id);
            if (!b) continue;
            bool isRep = lb.shape == edit::Shape::Reporter ||
                         lb.shape == edit::Shape::Boolean;
            if (!isRep) {
                if (!lb.noBump && (lb.shape == edit::Shape::Stack ||
                                      lb.shape == edit::Shape::Hat))
                    out.push_back({0, lb.id, "", 0, lb.x + 32, lb.y + lb.h});
                if (lb.shape != edit::Shape::Hat)
                    out.push_back({1, lb.id, "", 0, lb.x + 32, lb.y});
                for (auto& m : lb.mouths)
                    out.push_back({3, lb.id, "", m.branch, m.x + 32, m.y});
            }
            for (auto& g : lb.segs) {
                // value input の接続点: スロット中央。reporter の入力
                // （例: join の左右）も受け口になる
                if (g.input.empty() || g.kind == bl::SegKind::Text) continue;
                if (!b->inputs.has(g.input)) continue;
                // renderDrawRight_: connection はスロット左端＋行中央
                out.push_back({2, lb.id, g.input, 0,
                               lb.x + g.x, lb.y + g.y + g.h / 2});
            }
        }
    }
    void updateDropTarget() {
        drag.targetId.clear();
        drag.before = drag.valueTarget = drag.substackTarget = false;
        drag.subBranch = 0;
        drag.valueInput.clear();
        drag.hasSnap = false;
        Target* t = selected();
        if (!t || !t->blocks) return;
        Blocks* blocks = t->blocks;
        bool isReporter = drag.shape == edit::Shape::Reporter ||
                          drag.shape == edit::Shape::Boolean;
        std::set<std::string> exclude;
        if (!drag.fromFlyout && !drag.headId.empty())
            bl::collectSubtree(blocks, drag.headId, exclude);
        std::vector<Conn> cands;
        collectConnections(blocks, exclude, cands);
        if (!drag.connectionsReady) {
            // A hat has only an outgoing (bottom) connector. A stack dragged
            // before another script connects with its LAST block, not its head.
            drag.hasPrevious = !isReporter && drag.shape != edit::Shape::Hat;
            drag.hasNext = !isReporter && drag.shape != edit::Shape::End;
            if (drag.fromFlyout) {
                drag.headH = drag.tmpl.h;
                drag.chainH = drag.tmpl.h;
                if (drag.opcode == "control_forever") drag.hasNext = false;
            } else if (blocks->getBlock(drag.headId)) {
                drag.previewLayout.clear();
                drag.chainH = bl::layoutChain(blocks, drag.headId, 0, 0, tr(),
                                              drag.previewLayout, 0);
                for (const auto& lb : drag.previewLayout)
                    if (lb.id == drag.headId) { drag.headH = lb.h; break; }
                if (Block* tail = blocks->getBlock(bl::chainTail(blocks, drag.headId))) {
                    drag.hasNext = !isReporter &&
                        bl::blockShapeOf(blocks, tail->id) != edit::Shape::End &&
                        tail->opcode != "control_forever";
                }
            }
            drag.connectionsReady = true;
        }
        float radius = isReporter ? 64 : 56;
        float bestD2 = radius * radius;
        int best = -1;
        for (int i = 0; i < (int)cands.size(); ++i) {
            const Conn& c = cands[i];
            bool compat = isReporter ? (c.kind == 2)
                                     : ((c.kind == 0 || c.kind == 3)
                                            ? drag.hasPrevious
                                            : c.kind == 1 && drag.hasNext);
            // A hat can head a script, but cannot become a nested/next block.
            if (compat && drag.shape == edit::Shape::Hat) {
                Block* target = blocks->getBlock(c.blockId);
                compat = c.kind == 1 && target && target->topLevel;
            }
            if (!compat) continue;
            float srcX = drag.mx + (isReporter ? 0.f : 32.f);
            float srcY = drag.my + (isReporter ? drag.headH / 2.f
                                               : c.kind == 1 ? drag.chainH : 0.f);
            float dx = c.x - srcX, dy = c.y - srcY;
            float d2 = dx * dx + dy * dy;
            if (d2 < bestD2) { bestD2 = d2; best = i; }
        }
        if (best >= 0) {
            const Conn& c = cands[best];
            drag.targetId = c.blockId;
            drag.snapX = c.x; drag.snapY = c.y;
            drag.snapHeadX = c.x - (isReporter ? 0.f : 32.f);
            drag.snapHeadY = c.y - (isReporter ? drag.headH / 2.f
                                              : c.kind == 1 ? drag.chainH : 0.f);
            drag.hasSnap = true;
            if (c.kind == 2) { drag.valueTarget = true; drag.valueInput = c.inputName; }
            else if (c.kind == 0) drag.before = false;
            else if (c.kind == 1) drag.before = true;
            else { drag.substackTarget = true; drag.subBranch = c.branch; }
            return;
        }
        // Hats must never fall back to being inserted after/inside a block.
        if (drag.shape == edit::Shape::Hat) return;
        // フォールバック: ブロック上への直接ドロップ
        bl::LBlock* lb = wsBlockAt((drag.mx + 32) * wsScale() + panX,
                                   (drag.my) * wsScale() + panY);
        if (!lb || exclude.count(lb->id)) return;
        drag.targetId = lb->id;
        if (isReporter) { drag.targetId.clear(); return; }
        float wsy = (lb->y - drag.my);
        if (lb->cblock >= 1 && !lb->mouths.empty()) {
            for (auto& m : lb->mouths)
                if (drag.my >= m.y - 24 && drag.my < m.y + m.innerH + 24) {
                    drag.substackTarget = true;
                    drag.subBranch = m.branch;
                    return;
                }
        }
        drag.before = wsy > 0;
    }
    void doDrop() {
        if (!drag.active) return;
        Target* t = selected();
        if (!t || !t->blocks) { drag.active = false; return; }
        Blocks* blocks = t->blocks;
        std::string id;
        if (drag.fromFlyout) {
            id = instBlock(blocks, drag.opcode, drag.initFields,
                           drag.mx, drag.my, false,
                           drag.mutation ? &*drag.mutation : nullptr,
                           drag.argTexts.empty() ? nullptr : &drag.argTexts);
            if (id.empty()) { drag.active = false; return; }
        } else {
            id = drag.headId;
        }
        if (std::getenv("XYO_DEBUG"))
            std::fprintf(stderr, "editor: drop opcode=%s id=%s at %.0f,%.0f\n",
                         drag.opcode.c_str(), id.c_str(), drag.mx, drag.my);
        bool isRep = drag.shape == edit::Shape::Reporter ||
                     drag.shape == edit::Shape::Boolean;
        // Derive the tail from THIS chain, never from an earlier gesture.
        const std::string tail = bl::chainTail(blocks, id);
        // insertBefore may promote this head to a script root (notably hats).
        // Persist its current snapped pose before rewiring, otherwise layout
        // reads the old stored x/y and the hat jumps back after mouse release.
        if (Block* head = blocks->getBlock(id)) {
            head->x = drag.hasSnap ? drag.snapHeadX : drag.mx;
            head->y = drag.hasSnap ? drag.snapHeadY : drag.my;
        }
        if (drag.valueTarget && !drag.targetId.empty()) {
            bl::attachToValue(blocks, id, drag.targetId, drag.valueInput);
        } else if (drag.substackTarget && !drag.targetId.empty() && !isRep) {
            bl::attachToSubstack(blocks, id, tail,
                                 drag.targetId, drag.subBranch);
        } else if (!drag.targetId.empty() && !isRep) {
            if (drag.before)
                bl::insertBefore(blocks, id,
                                 tail,
                                 drag.targetId);
            else
                bl::insertAfter(blocks, id,
                                tail,
                                drag.targetId);
        } else {
            if (Block* b = blocks->getBlock(id)) { b->x = drag.mx; b->y = drag.my; }
            bl::makeTopLevel(blocks, id);
        }
        blocks->resetCache();
        wsDirty_ = true;
        selBlock = id;
        drag.active = false;
        drag.pending = false;
        drag.snapshotTaken = false;
        drag.connectionsReady = false;
    }
    void cancelDrag() {
        if (drag.snapshotTaken) { popUndo(); drag.snapshotTaken = false; }
        Target* t = selected();
        if (t && t->blocks && !drag.fromFlyout) {
            Blocks* blocks = t->blocks;
            if (drag.kind == DragKind::ValueInput) {
                if (!drag.prevParent.empty() && !drag.prevInput.empty()) {
                    if (Block* p = blocks->getBlock(drag.prevParent)) {
                        BlockInput* in = p->inputs.find(drag.prevInput);
                        if (in) in->block = drag.headId;
                    }
                    if (Block* h = blocks->getBlock(drag.headId)) h->parent = drag.prevParent;
                }
            } else if (drag.kind == DragKind::Substack) {
                if (!drag.prevParent.empty() && !drag.prevInput.empty()) {
                    if (Block* p = blocks->getBlock(drag.prevParent)) {
                        BlockInput* in = p->inputs.find(drag.prevInput);
                        if (in) in->block = drag.headId;
                    }
                    if (Block* h = blocks->getBlock(drag.headId)) h->parent = drag.prevParent;
                }
                if (Block* tl = blocks->getBlock(drag.tailId)) tl->next = drag.tailNext;
                if (!drag.tailNext.empty())
                    if (Block* g = blocks->getBlock(drag.tailNext)) g->parent = drag.tailId;
            } else if (drag.kind == DragKind::Chain) {
                if (!drag.prevId.empty()) {
                    if (Block* px = blocks->getBlock(drag.prevId)) px->next = drag.headId;
                    if (Block* h = blocks->getBlock(drag.headId)) h->parent = drag.prevId;
                } else if (drag.wasTop) {
                    if (Block* h = blocks->getBlock(drag.headId)) {
                        h->topLevel = true;
                        blocks->addScript(drag.headId);
                    }
                }
                if (Block* tl = blocks->getBlock(drag.tailId)) tl->next = drag.tailNext;
                if (!drag.tailNext.empty())
                    if (Block* g = blocks->getBlock(drag.tailNext)) g->parent = drag.tailId;
            } else if (drag.kind == DragKind::TopLevel) {
                if (drag.wasTop) bl::makeTopLevel(blocks, drag.headId);
            }
            blocks->resetCache();
        }
        drag.active = false;
        drag.pending = false;
        wsDirty_ = true;
    }

    // ---- スプライト ---------------------------------------------------------
    /// スプライト削除（タイル右上の ×）。オリジナル + クローンを廃棄する。
    void deleteSprite(int ti) {
        if (!rt || ti < 0 || ti >= (int)rt->targets.size()) return;
        Target* t = rt->targets[ti];
        if (!t || t->isStage) return;
        Sprite* spr = t->sprite;
        // 全クローンを廃棄（先にコピー — disposeTarget が clones を変更する）
        std::vector<Target*> all = spr->clones;
        for (Target* c : all) {
            auto it = std::find(rt->targets.begin(), rt->targets.end(), c);
            if (it != rt->targets.end()) rt->disposeTarget(c);
        }
        rt->ownedSprites.erase(
            std::remove_if(rt->ownedSprites.begin(), rt->ownedSprites.end(),
                           [spr](const std::unique_ptr<Sprite>& p) {
                               return p.get() == spr;
                           }),
            rt->ownedSprites.end());
        if (selTarget >= (int)rt->targets.size())
            selTarget = (int)rt->targets.size() - 1;
        if (selTarget >= 0 && rt->targets[selTarget]->isStage)
            for (size_t i = 0; i < rt->targets.size(); ++i)
                if (!rt->targets[i]->isStage) { selTarget = (int)i; break; }
        clearHistory();
        wsDirty_ = true;
        flyDirty_ = true;
    }
    /// 新規コスチューム（生成 SVG — 実 GUI の Paint→空キャンバス相当）。
    void addCostume() {
        Target* t = selected();
        if (!t || !t->sprite) return;
        int n = (int)t->sprite->costumes.size() + 1;
        Costume c;
        c.name = utf8ToU16("costume" + std::to_string(n));
        c.dataFormat = "svg";
        static const char* cols[] = {"#855CD6", "#0FBD8C", "#FF8C1A",
                                      "#4C97FF", "#CF63CF", "#FF6680"};
        static int hue = 0;
        std::string svg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"80\" "
            "height=\"60\" viewBox=\"0 0 80 60\">"
            "<ellipse cx=\"40\" cy=\"32\" rx=\"34\" ry=\"26\" fill=\"";
        svg += cols[hue++ % 6];
        svg += "\"/>"
               "<circle cx=\"30\" cy=\"24\" r=\"7\" fill=\"#fff\"/>"
               "<circle cx=\"50\" cy=\"24\" r=\"7\" fill=\"#fff\"/>"
               "<circle cx=\"31\" cy=\"25\" r=\"3\" fill=\"#000\"/>"
               "<circle cx=\"51\" cy=\"25\" r=\"3\" fill=\"#000\"/>"
               "</svg>";
        c.data.assign(svg.begin(), svg.end());
        c.assetId = "gen" + std::to_string(SDL_GetTicks());
        c.rotationCenterX = 40;
        c.rotationCenterY = 30;
        t->sprite->costumes.push_back(c);
        t->setCostume((double)t->sprite->costumes.size() - 1);
    }
    /// 新規サウンド（440Hz ビープ WAV 生成 — Upload 代替）。
    void addSound() {
        Target* t = selected();
        if (!t || !t->sprite) return;
        int n = (int)t->sprite->sounds.size() + 1;
        Sound s2;
        s2.name = utf8ToU16("pop" + std::to_string(n));
        s2.dataFormat = "wav";
        s2.format = "";
        // 16bit PCM 22050Hz 0.12s 正弦
        const int RATE = 22050, N = RATE * 12 / 100;
        std::vector<uint8_t> wav(44 + N * 2);
        auto w32 = [&](size_t o, uint32_t v) { std::memcpy(&wav[o], &v, 4); };
        auto w16 = [&](size_t o, uint16_t v) { std::memcpy(&wav[o], &v, 2); };
        std::memcpy(&wav[0], "RIFF", 4);
        w32(4, 36 + N * 2);
        std::memcpy(&wav[8], "WAVEfmt ", 8);
        w32(16, 16);
        w16(20, 1); w16(22, 1); w32(24, RATE);
        w32(28, RATE * 2); w16(32, 2); w16(34, 16);
        std::memcpy(&wav[36], "data", 4);
        w32(40, N * 2);
        for (int i = 0; i < N; ++i) {
            double env = 1.0 - (double)i / N;
            int16_t v = (int16_t)(std::sin(i * 440.0 * 6.2831853 / RATE) *
                                  30000 * env);
            std::memcpy(&wav[44 + i * 2], &v, 2);
        }
        s2.data = std::move(wav);
        s2.assetId = "gen" + std::to_string(SDL_GetTicks());
        s2.soundId = n - 1;
        t->sprite->sounds.push_back(std::move(s2));
        soundSel_ = (int)t->sprite->sounds.size() - 1;
    }
    /// 選択コスチュームを削除（1 個以下なら残す — Scratch も最後は消せない）。
    void deleteCostume() {
        Target* t = selected();
        if (!t || !t->sprite) return;
        deleteCostumeAt(std::clamp(t->currentCostume, 0,
                        (int)t->sprite->costumes.size() - 1));
    }
    void deleteCostumeAt(int i) {
        Target* t = selected();
        if (!t || !t->sprite || t->sprite->costumes.size() <= 1) return;
        if (i < 0 || i >= (int)t->sprite->costumes.size()) return;
        t->sprite->costumes.erase(t->sprite->costumes.begin() + i);
        if (t->currentCostume >= (int)t->sprite->costumes.size())
            t->setCostume((double)t->sprite->costumes.size() - 1);
    }
    /// 選択サウンドを削除。
    void deleteSound() {
        deleteSoundAt(std::clamp(soundSel_, 0,
            (int)(selected() && selected()->sprite
                      ? selected()->sprite->sounds.size() : 0) - 1));
    }
    void deleteSoundAt(int i) {
        Target* t = selected();
        if (!t || !t->sprite || t->sprite->sounds.empty()) return;
        if (i < 0 || i >= (int)t->sprite->sounds.size()) return;
        t->sprite->sounds.erase(t->sprite->sounds.begin() + i);
        soundSel_ = std::clamp(soundSel_, 0,
                               (int)t->sprite->sounds.size() - 1);
    }

    /// スプライトタイルの hit（targets index を返す、無ければ -1）。
    int spriteTileAt(float mx, float my) {
        if (!rt) return -1;
        float tw = std::min(96.f, std::max(64.f,
                        (ui.spriteSel.w - 8 - 4 * 5) / 5.f));
        const float th = tw, gap = 4;
        float x = ui.spriteSel.x + 4, y = ui.spriteSel.y + 4;
        int cols = std::max(1, (int)((ui.spriteSel.w - 8) / (tw + gap)));
        int i = 0;
        for (size_t ti = 0; ti < rt->targets.size(); ++ti) {
            Target* t = rt->targets[ti];
            if (t->isStage) continue;
            if (i > 0 && i % cols == 0) { x = ui.spriteSel.x + 4; y += th + gap; }
            if (mx >= x && mx < x + tw && my >= y && my < y + th)
                return (int)ti;
            x += tw + gap;
            i++;
        }
        return -1;
    }
    /// タイル矩形（spriteTileAt と同じレイアウト）。非 stage の
    /// visual index n から (x,y) を返す。
    FRect spriteTileRect(int visIdx) {
        float tw = std::min(96.f, std::max(64.f,
                        (ui.spriteSel.w - 8 - 4 * 5) / 5.f));
        int cols = std::max(1, (int)((ui.spriteSel.w - 8) / (tw + 4)));
        return {ui.spriteSel.x + 4 + (visIdx % cols) * (tw + 4),
                ui.spriteSel.y + 4 + (visIdx / cols) * (tw + 4),
                tw, tw};
    }
    /// 非 stage スプライトの visual index を返す（target index → 並び番号）。
    int spriteVisIndex(int ti) {
        int v = 0;
        for (size_t i = 0; i < rt->targets.size(); ++i) {
            if (rt->targets[i]->isStage) continue;
            if ((int)i == ti) return v;
            ++v;
        }
        return -1;
    }
    /// 非 stage スプライトの個数。
    int spriteCount() {
        int n = 0;
        for (Target* t : rt->targets)
            if (!t->isStage) ++n;
        return n;
    }
    /// ドラッグ位置から挿入 index（0..spriteCount）を計算。
    int spriteInsertIndex(float mx, float my) {
        int n = spriteCount();
        if (n == 0) return 0;
        float tw = std::min(96.f, std::max(64.f,
                        (ui.spriteSel.w - 8 - 4 * 5) / 5.f));
        int cols = std::max(1, (int)((ui.spriteSel.w - 8) / (tw + 4)));
        float relY = my - (ui.spriteSel.y + 4);
        float relX = mx - (ui.spriteSel.x + 4);
        int row = std::clamp((int)((relY + (tw + 4) / 2) / (tw + 4)), 0,
                             (n - 1) / cols + 1);
        int rowFirst = row * cols;
        int rowLast = std::min(n, rowFirst + cols);
        if (rowFirst >= n) return n;
        int ins = rowFirst;
        for (int i = rowFirst; i < rowLast; ++i) {
            FRect tr = spriteTileRect(i);
            if (mx < tr.x + tr.w / 2.f) { ins = i; break; }
            ins = i + 1;
        }
        return std::clamp(ins, 0, n);
    }
    /// 非 stage visual index → targets index。
    int spriteTiFromVis(int visIdx) {
        int v = 0;
        for (size_t i = 0; i < rt->targets.size(); ++i) {
            if (rt->targets[i]->isStage) continue;
            if (v == visIdx) return (int)i;
            ++v;
        }
        return -1;
    }
    /// targets / executableTargets を reorder（orderAt.js 相当: 全 target の
    /// 並び順をスプライトタイル順に揃える。stage は targets 先頭固定）。
    void spriteReorder(int ti, int ins) {
        if (!rt) return;
        // 非 stage の配列を作り、ti を抜いて ins へ挿入
        std::vector<Target*> spr;
        Target* moved = nullptr;
        for (Target* t : rt->targets)
            if (!t->isStage) {
                if (t == rt->targets[ti]) moved = t;
                else spr.push_back(t);
            }
        if (!moved) return;
        ins = std::clamp(ins, 0, (int)spr.size());
        spr.insert(spr.begin() + ins, moved);
        // targets = [stage...][spr 順]、executableTargets = 同順（clone 除く）
        std::vector<Target*> nt, ne;
        for (Target* t : rt->targets) if (t->isStage) nt.push_back(t);
        for (Target* t : spr) nt.push_back(t);
        for (Target* t : nt)
            if (std::find(rt->executableTargets.begin(),
                          rt->executableTargets.end(), t) !=
                rt->executableTargets.end())
                ne.push_back(t);
        // executableTargets には clone も残っている — 順序は親の後へ追従
        for (Target* t : rt->executableTargets)
            if (std::find(ne.begin(), ne.end(), t) == ne.end())
                ne.push_back(t);
        // clone を親の直後へ集める
        rt->targets = nt;
        rt->executableTargets = ne;
        selTarget = (int)(std::find(nt.begin(), nt.end(), moved) - nt.begin());
    }
    /// スプライトタイル右クリック（sprite-selector-item.jsx:
    /// duplicate / export / delete、delete は DangerousMenuItem）。
    void openSpriteMenu(float mx, float my, int ti) {
        ctx_.x = mx; ctx_.y = my; ctx_.hover = -1;
        ctx_.items.clear();
        ctx_.enabled.clear();
        ctx_.danger.clear();
        ctx_.blockId.clear();
        ctx_.spriteIdx = ti;
        ctx_.assetKind = 0; ctx_.assetIdx = -1;
        ctx_.items = {"duplicate", "export", "delete"};
        ctx_.enabled = {true, true, true};
        ctx_.danger = {false, false, true};
        ctx_.open = true;
    }
    /// アセット（コスチューム/サウンド）タイルの hit。
    int assetTileAt(float mx, float my) {
        for (auto& at : assetTiles_)
            if (mx >= at.r.x && mx < at.r.x + at.r.w &&
                my >= at.r.y && my < at.r.y + at.r.h)
                return at.index;
        return -1;
    }
    /// アセットタイル右クリック（asset-panel selector も
    /// sprite-selector-item と同じ duplicate/export/delete）。
    void openAssetMenu(float mx, float my, int kind, int idx) {
        ctx_.x = mx; ctx_.y = my; ctx_.hover = -1;
        ctx_.items.clear();
        ctx_.enabled.clear();
        ctx_.danger.clear();
        ctx_.blockId.clear();
        ctx_.spriteIdx = -1;
        ctx_.assetKind = kind; ctx_.assetIdx = idx;
        ctx_.items = {"duplicate", "export", "delete"};
        ctx_.enabled = {true, true, true};
        ctx_.danger = {false, false, true};
        ctx_.open = true;
    }
    /// コスチューム複製（duplicateCostume: 名前重複回避つきコピー）。
    void duplicateCostume(int i) {
        Target* t = selected();
        if (!t || !t->sprite ||
            i < 0 || i >= (int)t->sprite->costumes.size()) return;
        Costume c = t->sprite->costumes[i];
        // unusedName
        std::string bn = utf16ToUtf8(c.name);
        while (!bn.empty() && std::isdigit((unsigned char)bn.back()))
            bn.pop_back();
        int n = 2;
        auto exists = [&](const std::string& nm) {
            for (auto& cc : t->sprite->costumes)
                if (utf16ToUtf8(cc.name) == nm) return true;
            return false;
        };
        while (exists(bn + std::to_string(n))) ++n;
        c.name = utf8ToU16(bn + std::to_string(n));
        t->sprite->costumes.insert(t->sprite->costumes.begin() + i + 1, c);
        t->setCostume((double)(i + 1));
    }
    /// サウンド複製。
    void duplicateSound(int i) {
        Target* t = selected();
        if (!t || !t->sprite ||
            i < 0 || i >= (int)t->sprite->sounds.size()) return;
        Sound snd = t->sprite->sounds[i];
        std::string bn = utf16ToUtf8(snd.name);
        while (!bn.empty() && std::isdigit((unsigned char)bn.back()))
            bn.pop_back();
        int n = 2;
        auto exists = [&](const std::string& nm) {
            for (auto& ss : t->sprite->sounds)
                if (utf16ToUtf8(ss.name) == nm) return true;
            return false;
        };
        while (exists(bn + std::to_string(n))) ++n;
        snd.name = utf8ToU16(bn + std::to_string(n));
        t->sprite->sounds.insert(t->sprite->sounds.begin() + i + 1, snd);
        soundSel_ = i + 1;
    }
    /// コスチュームを .svg/.png へ、サウンドを .wav へ書き出す。
    bool exportAsset(int kind, int idx) {
        Target* t = selected();
        if (!t || !t->sprite) return false;
        std::string p = path;
        auto dot = p.find_last_of('.');
        if (dot != std::string::npos) p = p.substr(0, dot);
        const char* ext = "bin";
        std::string nm;
        const std::vector<uint8_t>* data = nullptr;
        if (kind == 1 && idx < (int)t->sprite->costumes.size()) {
            const Costume& c = t->sprite->costumes[idx];
            ext = c.dataFormat == "svg" ? "svg" : "png";
            nm = utf16ToUtf8(c.name);
            data = &c.data;
        } else if (kind == 2 && idx < (int)t->sprite->sounds.size()) {
            const Sound& snd = t->sprite->sounds[idx];
            ext = "wav";
            nm = utf16ToUtf8(snd.name);
            data = &snd.data;
        } else return false;
        if (!data || data->empty()) return false;
        p += "-" + nm + "." + ext;
        FILE* f = fopen(p.c_str(), "wb");
        if (!f) return false;
        size_t n = std::fwrite(data->data(), 1, data->size(), f);
        std::fclose(f);
        return n == data->size();
    }
    /// スプライト複製（sprite.js duplicate: 名前重複回避・コスチューム/
    /// サウンド/ブロック/変数をディープコピー）。
    void duplicateSprite(int ti) {
        if (!rt || ti < 0 || ti >= (int)rt->targets.size()) return;
        Target* src = rt->targets[ti];
        if (!src || !src->sprite) return;
        Sprite* ss = src->sprite;
        auto spr = std::make_unique<Sprite>(rt);
        Sprite* sp = spr.get();
        rt->ownedSprites.push_back(std::move(spr));
        // unusedName: 同名なら末尾数字を除き "name2"、"name3"…
        {
            std::string bn = utf16ToUtf8(ss->name);
            auto exists = [&](const std::string& nm) {
                for (Target* t : rt->targets)
                    if (utf16ToUtf8(t->sprite->name) == nm) return true;
                return false;
            };
            if (exists(bn)) {
                while (!bn.empty() &&
                       std::isdigit((unsigned char)bn.back()))
                    bn.pop_back();
                int i = 2;
                while (exists(bn + std::to_string(i))) ++i;
                bn += std::to_string(i);
            }
            sp->name = utf8ToU16(bn);
        }
        sp->costumes = ss->costumes;
        sp->sounds = ss->sounds;
        sp->soundBank = ss->soundBank;
        sp->blocks = std::make_unique<Blocks>(rt);
        if (ss->blocks)
            sp->blocks->restoreState(ss->blocks->allBlocks(),
                                     std::vector<std::string>(
                                         ss->blocks->getScripts()));
        Target* nt = sp->createClone();
        nt->x = src->x; nt->y = src->y; nt->direction = src->direction;
        nt->visible = src->visible; nt->size = src->size;
        nt->draggable = src->draggable;
        nt->rotationStyle = src->rotationStyle;
        nt->currentCostume = src->currentCostume;
        nt->volume = src->volume;
        nt->variables = src->variables;
        nt->effects = src->effects;
        nt->soundEffects = src->soundEffects;
        rt->addTarget(nt);
        // 元の直下へ（goBehindOther — 直前の兄弟の後ろ = 一覧順は末尾）
        selTarget = (int)rt->targets.size() - 1;
        clearHistory();
        wsDirty_ = true;
        flyDirty_ = true;
    }
    /// スプライト書き出し（.sprite3 = sprite.json + アセットの zip）。
    bool exportSprite(int ti) {
        if (!rt || ti < 0 || ti >= (int)rt->targets.size()) return false;
        Target* t = rt->targets[ti];
        if (!t || !t->sprite) return false;
        std::string p = path;
        auto dot = p.find_last_of('.');
        if (dot != std::string::npos) p = p.substr(0, dot);
        p += "-" + utf16ToUtf8(t->sprite->name) + ".sprite3";
        return saveSprite3File(*rt, *t, p.c_str());
    }

    void addSprite() {
        if (!rt) return;
        auto spr = std::make_unique<Sprite>(rt);
        Sprite* sp = spr.get();
        rt->ownedSprites.push_back(std::move(spr));
        int n = 1;
        for (Target* t : rt->targets)
            if (!t->isStage) n++;
        sp->name = utf8ToU16("Sprite" + std::to_string(n));
        // 新規スプライトへ生成コスチューム（塗りつぶし円 + 目）を付ける
        {
            Costume c;
            c.name = u"costume1";
            c.dataFormat = "svg";
            static int hue = 0;
            static const char* cols[] = {"#855CD6", "#0FBD8C", "#FF8C1A",
                                          "#4C97FF", "#CF63CF", "#FF6680"};
            const char* col = cols[hue++ % 6];
            std::string svg =
                "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"80\" "
                "height=\"60\" viewBox=\"0 0 80 60\">"
                "<ellipse cx=\"40\" cy=\"32\" rx=\"34\" ry=\"26\" fill=\"";
            svg += col;
            svg += "\"/>"
                   "<circle cx=\"30\" cy=\"24\" r=\"7\" fill=\"#fff\"/>"
                   "<circle cx=\"50\" cy=\"24\" r=\"7\" fill=\"#fff\"/>"
                   "<circle cx=\"31\" cy=\"25\" r=\"3\" fill=\"#000\"/>"
                   "<circle cx=\"51\" cy=\"25\" r=\"3\" fill=\"#000\"/>"
                   "</svg>";
            c.data.assign(svg.begin(), svg.end());
            c.assetId = "sprite" + std::to_string(n) + "_c1";
            c.rotationCenterX = 40;
            c.rotationCenterY = 30;
            sp->costumes.push_back(c);
        }
        Target* tgt = sp->createClone();
        rt->addTarget(tgt);
        selTarget = (int)rt->targets.size() - 1;
        clearHistory();
    }
    /// ステージへ生成背景を追加（add-backdrop ボタン）。
    void addBackdrop() {
        if (!rt) return;
        Target* stage = nullptr;
        for (Target* t : rt->targets)
            if (t->isStage) { stage = t; break; }
        if (!stage || !stage->sprite) return;
        int stageIdx = 0;
        for (size_t i = 0; i < rt->targets.size(); ++i)
            if (rt->targets[i] == stage) { stageIdx = (int)i; break; }
        int n = (int)stage->sprite->costumes.size() + 1;
        Costume c;
        c.name = utf8ToU16("backdrop" + std::to_string(n));
        c.dataFormat = "svg";
        static int hue = 0;
        static const char* cols[] = {"#7DDBDC", "#E9F1FC", "#F9F3E9",
                                      "#B8E4B8", "#FFD9D9", "#D9E3F2"};
        const char* col = cols[hue++ % 6];
        std::string svg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"480\" "
            "height=\"360\" viewBox=\"0 0 480 360\">"
            "<rect width=\"480\" height=\"360\" fill=\"";
        svg += col;
        svg += "\"/><circle cx=\"380\" cy=\"70\" r=\"34\" "
               "fill=\"#FFBF00\"/></svg>";
        c.data.assign(svg.begin(), svg.end());
        c.assetId = "backdrop_auto_" + std::to_string(n);
        c.rotationCenterX = 240;
        c.rotationCenterY = 180;
        stage->sprite->costumes.push_back(c);
        stage->setCostume((double)(stage->sprite->costumes.size() - 1));
        selTarget = stageIdx;
    }
    void addVariable(bool list) {
        Target* t = selected();
        if (!t) return;
        int n = 1;
        for (auto& [vid, v] : t->variables)
            if ((v.type == Variable::kListType) == list) n++;
        std::string name = list ? "my list" : "my variable";
        if (n > 1) name += std::to_string(n);
        Variable v = Variable::create("xyo_var_" + std::to_string(++blkCounter),
                                      utf8ToU16(name),
                                      list ? Variable::kListType
                                           : Variable::kScalarType,
                                      false);
        t->variables.emplace(v.id, std::move(v));
        flyDirty_ = true;
    }
    // ---- Make a Block ダイアログ（Scratch の custom-procedures 再現）----
    struct MakeBlockDlg {
        enum PartKind { Text = 0, StrInput = 1, BoolInput = 2 };
        struct Part { PartKind kind; std::string text; };
        bool open = false;
        std::vector<Part> parts;
        bool warp = false;
        int editPart = -1;          // テキスト編集中の part index
        std::string buf;            // 編集中バッファ
        int selPart = -1;           // 選択中の part
        FRect addStr, addBool, addLabel, okBtn, cancelBtn, warpBox;
        std::vector<FRect> partRects;
        FRect editBox;              // 編集フィールド
        int strCount = 1, boolCount = 1;
        void reset() {
            open = true; warp = false; editPart = -1; selPart = -1;
            buf.clear(); strCount = boolCount = 1;
            parts.clear();
            parts.push_back({Text, "block name"});
            partRects.clear();
        }
        std::string proccode() const {
            std::string pc;
            for (const Part& p : parts) {
                if (!pc.empty()) pc += ' ';
                if (p.kind == Text) pc += p.text;
                else pc += p.kind == StrInput ? "%s" : "%b";
            }
            return pc;
        }
    } mb_;

    /// Make a Block — proccode/mutation 付きで定義（prototype + hat）を生成。
    void makeProcedure() {
        Target* t = selected();
        if (!t || !t->blocks) return;
        Blocks* blocks = t->blocks;
        pushUndo();
        // proccode を parts から合成（被りは番号付け）
        std::string proccode = mb_.proccode();
        if (proccode.empty()) proccode = "my block";
        {
            std::string base = proccode;
            bool dup = true;
            int n = 1;
            while (dup) {
                dup = false;
                for (auto& [bid, pb] : blocks->allBlocks())
                    if (pb.opcode == "procedures_prototype" && pb.mutation &&
                        pb.mutation->attr("proccode") == proccode) {
                        proccode = base + std::to_string(++n);
                        dup = true;
                        break;
                    }
            }
        }
        // mutation 用 JSON 列 + 引数 reporter shadow（SB3 仕様）
        std::string ids = "[", names = "[", dflts = "[";
        struct ArgShadow { std::string id; bool isBool; std::string name; };
        std::vector<ArgShadow> argShadows;
        int ai = 0;
        for (const auto& p : mb_.parts) {
            if (p.kind == MakeBlockDlg::Text) continue;
            if (ai) ids += ",", names += ",", dflts += ",";
            std::string aid = "arg" + std::to_string(ai);
            ids += "\"" + aid + "\"";
            names += "\"" + p.text + "\"";
            bool isBool = p.kind == MakeBlockDlg::BoolInput;
            dflts += isBool ? "false" : "\"\"";
            argShadows.push_back({aid, isBool, p.text});
            ai++;
        }
        ids += "]"; names += "]"; dflts += "]";

        Block proto;
        proto.id = newBlockId();
        proto.opcode = "procedures_prototype";
        proto.shadow = true;
        Mutation m;
        m.tagName = "mutation";
        m.attrs["proccode"] = proccode;
        m.attrs["argumentids"] = ids;
        m.attrs["argumentnames"] = names;
        m.attrs["argumentdefaults"] = dflts;
        m.attrs["warp"] = mb_.warp ? "true" : "false";
        m.parsed = false;
        proto.mutation = m;
        std::string protoId = proto.id;

        Block def;
        def.id = newBlockId();
        def.opcode = "procedures_definition";
        def.topLevel = true;
        // Scratch: 新規定義はワークスペース中央上付近へ出す
        def.x = (ui.ws.x + ui.ws.w * 0.35f - panX) / wsScale();
        def.y = (ui.ws.y + 80 - panY) / wsScale();
        BlockInput in;
        in.block = protoId;
        in.shadow = protoId;
        def.inputs.set("custom_block", in);
        proto.parent = def.id;
        // 各引数に対応する argument_reporter_* shadow を prototype の input に配置
        for (const auto& as : argShadows) {
            Block arg;
            arg.id = newBlockId();
            arg.opcode = as.isBool ? "argument_reporter_boolean"
                                   : "argument_reporter_string_number";
            arg.shadow = true;
            arg.parent = proto.id;
            arg.fields["VALUE"] =
                BlockField{Value::string(SStr::fromUtf8(as.name))};
            BlockInput ai2;
            ai2.block = arg.id;
            ai2.shadow = arg.id;
            proto.inputs.set(as.id, ai2);
            blocks->createBlock(arg);
        }
        blocks->createBlock(def);
        blocks->createBlock(proto);
        if (Block* dp = blocks->getBlock(def.id)) {
            dp->topLevel = true;
            blocks->addScript(def.id);
        }
        blocks->resetCache();
        wsDirty_ = true;
        flyDirty_ = true;
    }
    void toggleMonitor(const std::string& varId, const std::string& name) {
        if (!rt) return;
        Target* t = selected();
        MonitorRecord& rec = rt->monitorState.getOrCreate(varId);
        if (rec.id.empty()) {
            rec.id = varId;
            rec.opcode = "data_variable";
            rec.params["VARIABLE"] = utf8ToU16(name);
            rec.targetId = t ? t->id : "";
        }
        rec.visible = !rec.visible;
        rt->monitorState.dirty = true;
        if (Block* b = rt->monitorBlocks->getBlock(varId))
            b->isMonitored = rec.visible;
        rt->monitorBlocks->resetCache();
    }

    // ---- 実行中スクリプト（グロウ）-------------------------------------------
    std::set<std::string> runningHeads() {
        std::set<std::string> s;
        if (!rt) return s;
        Target* t = selected();
        for (Thread* th : rt->threads)
            if (th && th->target == t && !th->topBlock.empty())
                s.insert(th->topBlock);
        return s;
    }
    void clickRun(const std::string& id) {
        Target* t = selected();
        if (!t || !t->blocks || !rt) return;
        // トップブロックを探して toggleScript
        std::string top = t->blocks->getTopLevelScript(id);
        if (top.empty()) top = id;
        rt->toggleScript(top, t, true);
    }

    // ---- 状態 ----------------------------------------------------------------
    bool quit = false;
    bool panning = false;
    // Camera panning: middle drag anywhere, left drag on empty canvas, or
    // space + left drag anywhere. Left-drag on empty canvas matches the way
    // most node/canvas editors let you move the view with a plain mouse.
    Uint8 panButton = 0;
    bool panMoved = false;
    bool spaceDown = false;
    SDL_Cursor* panCursor = nullptr;
    SDL_Cursor* panPrevCursor = nullptr;
    float panStartX = 0, panStartY = 0, panPX = 0, panPY = 0;
    void beginPan(Uint8 button, float mx, float my) {
        if (std::getenv("XYO_DEBUG"))
            std::fprintf(stderr, "editor: pan start button=%u at %.0f,%.0f\n",
                         (unsigned)button, mx, my);
        panning = true;
        panButton = button;
        panMoved = false;
        panStartX = mx; panStartY = my;
        panPX = panX; panPY = panY;
        if (!panCursor)
            panCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEALL);
        if (panCursor) {
            panPrevCursor = SDL_GetCursor();
            SDL_SetCursor(panCursor);
        }
    }
    void endPan() {
        if (panning && std::getenv("XYO_DEBUG"))
            std::fprintf(stderr, "editor: pan end at %.1f,%.1f\n", panX, panY);
        if (panning && panCursor) SDL_SetCursor(panPrevCursor);
        panning = false;
        panButton = 0;
        panPrevCursor = nullptr;
    }
    std::string shotPath;
    bool shotDone = false;
    int lastSel = -1;
    FRect flagRect{0,0,0,0}, stopRect{0,0,0,0};
    bool stageLarge_ = false;   // ステージフルスクリーン（ui 列のみ表示）
    bool stageSmall_ = false;   // 小ステージモード（240×180 = 0.5x）

    // ---- レイアウト ----------------------------------------------------------
    void layoutUi() {
        SDL_GetWindowSize(win, &W, &H);
        ui.menuBar = {0, 0, (float)W, (float)kMenuH};
        // gui.css: .tab-list は幅 250px 固定（toolbox 列とは無関係）。
        // 右側は body 背景のみ。タブ矩形は drawTabs で配置する。
        ui.tabs = {0, (float)kMenuH, (float)kTabListW, (float)kTabH};
        if (stageLarge_) {
            // 拡大モード（stage-wrapper.css .full-screen: position fixed,
            // padding 0.1875rem=3px、中央配置でアスペクト 4:3 を維持）
            ui.stageMenu = {0, (float)kMenuH, (float)W, (float)kStageMenuH};
            float aw = (float)W - 6.f;
            float ah = (float)(H - kMenuH - kStageMenuH) - 6.f;
            float sc2 = std::min(aw / 480.f, ah / 360.f);
            float sw2 = 480.f * sc2, sh2 = 360.f * sc2;
            ui.stage = {(W - sw2) / 2.f,
                        (float)(kMenuH + kStageMenuH) + 3.f +
                            (ah - sh2) / 2.f,
                        sw2, sh2};
            ui.toolbox = ui.flyout = ui.ws = ui.body = ui.tabs = {};
            ui.spriteInfo = ui.spriteSel = ui.bpBar = ui.bpPanel = {};
            float fy = ui.stageMenu.y + 6;
            flagRect = {ui.stageMenu.x + 8, fy, 32, 32};
            stopRect = {ui.stageMenu.x + 40, fy, 32, 32};
            ui.trash = {};
            return;
        }
        // 右列（stage）。480×360 通常 / 240×180 小ステージ（scale=0.5）。
        // stage-and-target-wrapper の左右 8px padding 込みで列幅 +16。
        float stageW = stageSmall_ ? 240 : 480;
        float stageH = stageSmall_ ? 180 : 360;
        float rightX = W - kGap - stageW;
        ui.stageMenu = {rightX, (float)kMenuH, stageW, (float)kStageMenuH};
        ui.stage = {rightX, ui.stageMenu.y + ui.stageMenu.h, stageW, stageH};
        ui.spriteInfo = {rightX, ui.stage.y + ui.stage.h, stageW,
                         (float)kSpriteInfoH};
        float selTop = ui.spriteInfo.y + ui.spriteInfo.h + kGap;
        float selH = H - selTop - kGap;
        float stageSelW = 72.f;
        float sprSelW = stageW - stageSelW - 4.f;   // sprite-selector margin-right 4px
        ui.spriteSel = {rightX, selTop, sprSelW, std::max(80.f, selH)};
        ui.stageSel = {rightX + sprSelW + 4.f, selTop, stageSelW,
                       std::max(80.f, selH)};
        // 左列
        float leftW = rightX - kGap;
        ui.bpBar = {0, (float)H - kBackpackBarH, leftW, (float)kBackpackBarH};
        float bpH = bpOpen ? 160.f : 0.f;
        ui.bpPanel = {0, ui.bpBar.y - bpH, leftW, bpH};
        float bodyBot = ui.bpPanel.y;
        float colTop = ui.tabs.y + ui.tabs.h;
        ui.toolbox = {0, colTop, (float)kToolboxW, bodyBot - colTop};
        ui.flyout = {(float)kToolboxW, colTop, (float)(kFlyoutW - kToolboxW),
                     bodyBot - colTop};
        ui.ws = {ui.flyout.x + ui.flyout.w, colTop, leftW - kFlyoutW,
                 bodyBot - colTop};
        ui.body = {0, colTop, leftW, bodyBot - colTop};
        if (!panInit) {
            panInit = true;
            panX = ui.ws.x + 40;
            panY = ui.ws.y + 40;
        }
        // ツールボックスのカテゴリ行（item: padding .375rem×2 + bubble 20 +
        // margin 2 + ラベル ≈ 47px）
        float cy = ui.toolbox.y + 12;
        for (int i = 0; i < catCount() && i < 16; ++i) {
            ui.catRow[i] = {ui.toolbox.x, cy, ui.toolbox.w, 46};
            cy += 46;
        }
        // ズームコントロール（scratch-blocks: 36px×3、右下 12px、間隔 8px）
        float zx = ui.ws.x + ui.ws.w - 12 - 36;
        float zy = ui.ws.y + ui.ws.h - 12 - 124;
        ui.zoomPlus = {zx, zy, 36, 36};
        ui.zoomMinus = {zx, zy + 44, 36, 36};
        ui.zoomReset = {zx, zy + 88, 36, 36};
        // Scratch 3 にはゴミ箱が無い（trashcan:false）。hit 判定のみ残し描画はしない。
        ui.trash = {};
        // ステージメニューの flag/stop（controls.jsx: 32×32 ボタン、padding 6）
        float fy = ui.stageMenu.y + 6;
        flagRect = {ui.stageMenu.x + 8, fy, 32, 32};
        stopRect = {ui.stageMenu.x + 40, fy, 32, 32};
    }

    // ---- 描画: 各領域 --------------------------------------------------------
    void fillR(FRect rr, Color c) { fill(r, rr.x, rr.y, rr.w, rr.h, c); }
    void outlineR(FRect rr, Color c) { outline(r, rr.x, rr.y, rr.w, rr.h, c); }

    void drawMenuBar() {
        // menu-bar.css: background $looks-secondary、3rem、12px bold 白
        fillR(ui.menuBar, kHeader);
        // Scratch ロゴ（scratch-logo.svg、高さ 1.6rem=25.6px）
        rrFill(r, 12, 10, 29, 28, 8, Color{139, 116, 255, 255});
        drawText(18, 13, "X", 22, Color{255, 255, 255, 255}, true);
        drawText(47, 11, "XYO", 17, Color{255, 255, 255, 255}, true);
        drawText(48, 30, "NATIVE", 8, Color{174, 187, 215, 255}, true);
        fill(r, 0, 47, (float)W, 1, Color{139, 116, 255, 180});
        // Settings（ギア）メニュー項目
        {
            FRect rr{88.f, 0.f, 44.f, 48.f};
            menuBtnGear = rr;
            bool open = topMenu_.open && topMenu_.which == 5;
            if (open) fill(r, rr.x, rr.y, rr.w, rr.h, kMenuHover);
            bl::drawBlockIcon(r, "gear", rr.x + 12, 14, 20, 20, 0.95f);
        }
        // File / Edit（アイコン + ラベル + caret）
        float x = 136;
        const char* menus[2] = {"File", "Edit"};
        const char* icons[2] = {"menu_file", "menu_edit"};
        for (int i = 0; i < 2; i++) {
            float mw = 24 + 8 + textW(menus[i], 12, true) + 8 + 8 + 12;
            FRect rr{x, 0, mw, 48};
            if (i == 0) menuBtnFile = rr; else menuBtnEdit = rr;
            bool open = topMenu_.open && topMenu_.which == i;
            if (open) fill(r, rr.x, rr.y, rr.w, rr.h, kMenuHover);
            bl::drawBlockIcon(r, icons[i], x + 12, 14, 20, 20, 0.95f);
            drawText(x + 24 + 8, 17, menus[i], 12,
                     Color{255, 255, 255, 255}, true);
            bl::drawBlockIcon(r, "caret",
                              x + 24 + 8 + textW(menus[i], 12, true) + 8, 21,
                              8, 6, 0.95f);
            x += mw + 4;
        }
        // divider.css: border-right 1px dashed rgba(0,0,0,.15) +
        // menu-bar.css の margin 8×2・高さ 34px。Tutorials/Debug は
        // fileGroup（help/debug icon + ラベル）。
        x += 8;
        dashLine(x, 7, x, 41, Color{0, 0, 0, 38});
        x += 8;
        {
            const char* extras[2] = {"tutorials", "debug"};
            const char* labels[2] = {"Tutorials", "Debug"};
            for (int i = 0; i < 2; ++i) {
                bl::drawBlockIcon(r, extras[i], x, 14, 20, 20, 0.95f);
                x += 24;
                float lw = textW(labels[i], 12, true);
                drawText(x, 17, labels[i], 12, Color{255, 255, 255, 255},
                         true);
                x += lw + 16;
            }
        }
        // プロジェクト名（titleFieldGrowable: 左右グループ間いっぱいに伸びる。
        // 点線 border + 25% 白、8px padding、bold 12.8px）
        {
            float sw = textW("Save", 12, true) + 24;
            float boxX = x + 8;
            float boxW = (float)W - sw - 8 - 8 - boxX - 78;
            if (boxW < 160.f) { boxX = W / 2.f - 110; boxW = 220; }
            // title-field.css: bg rgba(255,255,255,.25)、1px dashed
            // rgba(0,0,0,.15)、radius 4、padding .5rem、テキストは
            // 左寄せの入力フィールド（.8rem bold 白、span に ellipsis）
            rrFill(r, boxX, 9, boxW, 30, 7, Color{255, 255, 255, 18});
            rrOutline(r, boxX, 9, boxW, 30, 7, Color{255, 255, 255, 24});
            float bx = (float)W - sw - 84;
            rrFill(r, bx, 14, 68, 22, 11, Color{255, 255, 255, 16});
            discFill(r, bx + 11, 25, 3, gpuUi_ ? Color{83, 229, 183, 255}
                                                            : Color{255, 197, 92, 255});
            drawText(bx + 19, 19, gpuUi_ ? "GPU UI" : "CPU UI", 10,
                     Color{212, 223, 245, 255}, true);
            pushClip({boxX + 8, 9, boxW - 16, 30});
            drawText(boxX + 8, 15, pathFromUtf8(path).filename().string(), 13,
                     Color{255, 255, 255, 255}, true);
            popClip();
        }
        // 右: Save 項目（save-status: 通常の menuBarItem テキスト）
        {
            float sw = textW("Save", 12, true) + 24;
            FRect rr{(float)W - sw - 8, 0, sw, 48};
            menuBtnSave = rr;
            float tw = textW("Save", 12, true);
            drawText(rr.x + (rr.w - tw) / 2, 17, "Save", 12,
                     Color{255, 255, 255, 255}, true);
        }
    }
    void drawTabs() {
        // gui.css: .tab-list は高さ 44px・幅 250px、タブは下揃えで 80%/90% 高、
        // 左から重なり（margin-left -8px、selected が最前面）。
        fill(r, ui.tabs.x, ui.tabs.y, ui.tabs.w, ui.tabs.h, kBody);
        const char* names[3] = {"Code", "Costumes", "Sounds"};
        static const char* tabIcons[3] = {"tab_code", "tab_costumes",
                                          "tab_sounds"};
        // .tab: flex-grow:1 + min-width:auto → 内容幅が基準（padding 20×2 +
        // icon 22 + margin-right 2 + 12px テキスト）。margin-left:-8 で重なり、
        // 合計は tab-list(250) からはみ出して ≈toolbox+flyout 幅になる。
        float tws[3], txs[3];
        for (int i = 0; i < 3; ++i)
            tws[i] = 40 + 24 + textW(names[i], 12, true);
        float tx = ui.tabs.x;
        for (int i = 0; i < 3; ++i) {
            txs[i] = tx;
            ui.tabX[i] = (int)tx;
            ui.tabW[i] = (int)tws[i];
            tx += tws[i] - 8.f;   // margin-left:-8
        }
        // z-index: 3,2,1（選択は 4 = 最前面）→ 非選択を右から描き、選択を最後に。
        static const char* tabIconsC[3] = {"tab_code_c", "tab_costumes_c",
                                           "tab_sounds_c"};
        auto drawTab = [&](int i) {
            bool sel = tab == i;
            float tw = tws[i];
            float th = ui.tabs.h * (sel ? 0.90f : 0.80f);
            float ty = ui.tabs.y + ui.tabs.h - th;
            tx = txs[i];
            // border-radius:1rem 1rem 0 0 — 下辺は隠れるため矩形に潰す
            rrFill(r, tx, ty, tw, th, 10, sel ? kTabActive : kTabBg);
            fill(r, tx, ui.tabs.y + ui.tabs.h - 10, tw, 10,
                 sel ? kTabActive : kTabBg);
            rrOutline(r, tx, ty, tw, th + 1, 4, kBorder, 1);
            // タブアイコン 22px。非選択はグレースケール、選択はカラー。
            float cw = 24 + textW(names[i], 12, true);
            float cx0 = tx + (tw - cw) / 2.f;
            float iy = ty + (th - 22) / 2;
            bl::drawBlockIcon(r, sel ? tabIconsC[i] : tabIcons[i], cx0, iy,
                              22, 22, 1.0f);
            drawText(cx0 + 24, ty + (th - 13) / 2, names[i], 12,
                     sel ? kLooksSecondary : kDim, true);
        };
        for (int i = 2; i >= 0; --i) if (i != tab) drawTab(i);
        drawTab(tab);
    }
    void drawToolbox() {
        // .scratchCategoryMenu: 60px・白・#575E75・6px 上下 padding
        fillR(ui.toolbox, kPanel);
        for (int i = 0; i < catCount() && i < 16; ++i) {
            const auto& c = catAt(i);
            bool sel = tab == 0 && catSel == i;
            if (sel) fillR(ui.catRow[i], kToolboxSel);
            // scratchCategoryItemBubble: 20px 円 + 1px secondary 境界
            float bx = ui.catRow[i].x + ui.toolbox.w / 2;
            bool isExt = c.name == "Pen" || c.name == "Music";
            if (isExt && bl::drawBlockIcon(
                    r, c.name == "Pen" ? "pen" : "music",
                    bx - 10, ui.catRow[i].y + 6, 20, 20)) {
                // アイコン描画済み
            } else {
                discFill(r, bx, ui.catRow[i].y + 16, 10, rgb(c.primary));
                uint32_t ter = c.id == "more" ? 0xFF3355u : 0;
                if (!ter && !c.items.empty()) {
                    if (const edit::BlockDef* d =
                            edit::findBlock(c.items[0]))
                        ter = d->tertiary;
                }
                if (ter)
                    ring(r, bx, ui.catRow[i].y + 16, 10, 1.f, rgb(ter));
            }
            // カテゴリラベル 11.2px（.7rem）。hover は $toolboxHover #4C97FF。
            float tw = textW(c.name, 11, false);
            drawText(ui.catRow[i].x + (ui.toolbox.w - tw) / 2,
                     ui.catRow[i].y + 29, c.name, 11, kFg, false);
        }
        // toolbox 右境界（toolbox border-right）
        line(r, ui.toolbox.x + ui.toolbox.w - 1, ui.toolbox.y,
             ui.toolbox.x + ui.toolbox.w - 1, ui.toolbox.y + ui.toolbox.h,
             kBorder, 1);
        // Add Extension（extension-button-container: 60×52、$looks-secondary、
        // 中央に 28px ＋アイコン）
        {
            float ey = ui.toolbox.y + ui.toolbox.h - 52;
            fill(r, ui.toolbox.x, ey, ui.toolbox.w, 52, kLooksSecondary);
            float cx = ui.toolbox.x + ui.toolbox.w / 2;
            float cy = ey + 26;
            line(r, cx - 10, cy, cx + 10, cy, Color{255, 255, 255, 255}, 2.5f);
            line(r, cx, cy - 10, cx, cy + 10, Color{255, 255, 255, 255}, 2.5f);
            addExtRect_ = {ui.toolbox.x, ey, ui.toolbox.w, 52};
        }
    }
    void drawFlyout() {
        fillR(ui.flyout, kFlyoutBg);
        // .blocklyFlyoutBackground: #F9F9F9 @80% opacity + 右 1px 境界
        fill(r, ui.flyout.x, ui.flyout.y, ui.flyout.w, ui.flyout.h,
             Color{kFlyoutBg.r, kFlyoutBg.g, kFlyoutBg.b, 204});
        line(r, ui.flyout.x + ui.flyout.w - 1, ui.flyout.y,
             ui.flyout.x + ui.flyout.w - 1, ui.flyout.y + ui.flyout.h, kBorder,
             1);
        if (flyDirty_ || flyCat_ != catAt(catSel).id ||
            flyTarget_ != selTarget) {
            flyDirty_ = true;
            rebuildFlyout();
        }
        // クリップ
        pushClip(ui.flyout);
        for (auto& f : flyItems_) {
            float sy = ui.flyout.y + f.y * bl::kWsZoom - flyScroll;
            if (sy > ui.flyout.y + ui.flyout.h ||
                sy + f.h * bl::kWsZoom < ui.flyout.y)
                continue;
            switch (f.kind) {
            case FlyItem::BlockT: {
                float bx = ui.flyout.x + 10;
                bl::LBlock lb = f.lb;
                lb.x = 0; lb.y = 0;
                bl::drawLBlock(r, lb, nullptr, tr(bl::kWsZoom), f.st,
                               bx / bl::kWsZoom, sy / bl::kWsZoom,
                               bl::kWsZoom, false, false, 1.f);
                break;
            }
            case FlyItem::Header:
                // .blocklyFlyoutLabelText: Helvetica 14pt bold #575E75
                drawText(ui.flyout.x + 12, sy + 2, f.text, 19, kFg, true);
                break;
            case FlyItem::Button: {
                // blocklyFlyoutButton: rx4、fill 無し + #c6c6c6 ストローク、
                // 高さ 40px、文字 #575E75 bold（Make a Variable 等）
                float bw = (float)textW(f.text, 13, true) + 80;
                if (bw > ui.flyout.w - 24) bw = ui.flyout.w - 24;
                float bx = ui.flyout.x + (ui.flyout.w - bw) / 2;
                rrOutline(r, bx, sy, bw, 40, 4, rgb(0xC6C6C6), 1);
                float tw = textW(f.text, 13, true);
                drawText(bx + (bw - tw) / 2, sy + 13, f.text, 13, kFg, true);
                break;
            }
            case FlyItem::VarRow:
            case FlyItem::ListRow: {
                // チェックボックス + 変数記者ピル
                bool mon = false;
                auto it = rt ? rt->monitorState.records.find(f.varId)
                             : rt->monitorState.records.end();
                if (it != rt->monitorState.records.end()) mon = it->second.visible;
                Color col = f.list ? rgb(0xFF661A) : rgb(0xFF8C1A);
                // check
                float cx = ui.flyout.x + 14;
                rrOutline(r, cx, sy + 10, 14, 14, 3, kDim, 1.5f);
                if (mon) {
                    fill(r, cx + 3, sy + 13, 8, 8, col);
                    drawText(cx + 3.5f, sy + 11, "✓", 9,
                             Color{255,255,255,255});
                }
                // ピル
                float px = cx + 22;
                float pw = textW(f.text, 11, true) + 20;
                rrFill(r, px, sy + 6, pw, 22, 11, col);
                drawText(px + 10, sy + 11, f.text, 11,
                         Color{255,255,255,255}, true);
                break;
            }
            case FlyItem::Divider:
                line(r, ui.flyout.x + 8, sy + 4,
                     ui.flyout.x + ui.flyout.w - 8, sy + 4, kBorder, 1);
                break;
            }
        }
        // スクロールバー（.blocklyFlyout .blocklyScrollbarHandle: #bbb、
        // トラック 11px・handle=6px 中央・スクロール可能時のみ）
        float ch = flyContentH() * bl::kWsZoom;
        if (ch > ui.flyout.h) {
            float barH = ui.flyout.h * ui.flyout.h / ch;
            float barY = ui.flyout.y + flyScroll * (ui.flyout.h - barH) /
                                            (ch - ui.flyout.h);
            rrFill(r, ui.flyout.x + ui.flyout.w - 8.5f, barY, 6, barH, 3,
                   Color{0xBB, 0xBB, 0xBB, 255});
        }
        popClip();
    }

    void drawWsGrid() {
        fillR(ui.ws, kWsBg);
        // ドットグリッド: ws 論理 32px 毎（UI の円で真のドットに）
        ui::DrawList* d = dl();
        if (!d) return;
        float s = wsScale();
        float step = 32 * s;
        ui::PackedColor dot = ui::rgba(kWsGrid.r, kWsGrid.g, kWsGrid.b, 255);
        float x0 = ui.ws.x + std::fmod(panX - ui.ws.x, step);
        float y0 = ui.ws.y + std::fmod(panY - ui.ws.y, step);
        for (float sy = y0; sy < ui.ws.y + ui.ws.h; sy += step)
            for (float sx = x0; sx < ui.ws.x + ui.ws.w; sx += step)
                d->AddCircleFilled({sx, sy}, 1.2f, dot);
    }
    void drawWorkspace() {
        drawWsGrid();
        if (wsDirty_ || wsTarget_ != selTarget) { wsDirty_ = true; rebuildWs(); }
        Target* t = selected();
        Blocks* blocks = t ? t->blocks : nullptr;
        auto heads = runningHeads();
        pushClip(ui.ws);
        float s = wsScale();
        // Traverse the dragged chain once, not once per workspace block.
        std::set<std::string> dragged;
        if (drag.active && !drag.fromFlyout)
            bl::collectSubtree(blocks, drag.headId, dragged);
        for (auto& lb : wsLayout_.blocks) {
            if (dragged.count(lb.id)) continue;
            // Skip path building and text work for offscreen scripts. Comments
            // may extend beyond their owner, so retain owners with comments.
            const float bx = panX + lb.x * s, by = panY + lb.y * s;
            if ((!lb.block || lb.block->comment.empty()) &&
                (bx + lb.w * s < ui.ws.x - 12 ||
                 by + lb.fullH * s < ui.ws.y - 12 ||
                 bx > ui.ws.x + ui.ws.w + 12 ||
                 by - (lb.hat ? (lb.defineHat ? 40.f : 17.f) * s : 0.f) >
                            ui.ws.y + ui.ws.h + 12)) continue;
            bl::BlockStyle st = bl::styleOf(blocks, lb.block);
            bool glow = heads.count(lb.id) > 0 ||
                        (lb.block && !lb.block->topLevel &&
                         heads.count(lb.block->id));
            // トップブロックのグロウのみ（Blockly はスタック全体を光らせる）
            bool isHead = lb.block && lb.block->topLevel;
            bl::drawLBlock(r, lb, blocks, tr(s), st, panX / s, panY / s, s,
                           lb.id == selBlock, isHead && heads.count(lb.id),
                           1.f);
            if (lb.block && !lb.block->comment.empty())
                bl::drawComment(r, lb.block->comment,
                                panX + lb.x * s, panY + lb.y * s, s, tr(s));
        }
        // スナップのハイライト（接続点へ黄丸）
        if (drag.active && drag.hasSnap) {
            discFill(r, panX + drag.snapX * s, panY + drag.snapY * s, 5 * s,
                     kGlow);
            ring(r, panX + drag.snapX * s, panY + drag.snapY * s, 7 * s,
                 1.5f * s, kGlow);
        }
        // ドラッグ中のゴースト
        if (drag.active) {
            if (drag.fromFlyout) {
                bl::LBlock g = drag.tmpl;
                g.x = drag.mx;
                g.y = drag.my;
                bl::drawLBlock(r, g, nullptr, tr(s), drag.tmplSt, panX / s,
                               panY / s, s, false, false, 0.85f);
            } else if (blocks) {
                // Translate the cached relative layout instead of remeasuring
                // the entire dragged script on each rendered frame.
                for (const auto& lb : drag.previewLayout) {
                    const float bx = panX + (drag.mx + lb.x) * s;
                    const float by = panY + (drag.my + lb.y) * s;
                    if (bx + lb.w * s < ui.ws.x - 12 ||
                        by + lb.fullH * s < ui.ws.y - 12 ||
                        bx > ui.ws.x + ui.ws.w + 12 ||
                        by - (lb.hat ? (lb.defineHat ? 40.f : 17.f) * s : 0.f) >
                            ui.ws.y + ui.ws.h + 12) continue;
                    bl::BlockStyle st = bl::styleOf(blocks, lb.block);
                    bl::drawLBlock(r, lb, blocks, tr(s), st,
                                   panX / s + drag.mx, panY / s + drag.my,
                                   s, false, false, 0.85f);
                }
            }
        }
        popClip();
        // スクロールバー（scratch-blocks: 11px #CECDCE、コンテンツが
        // はみ出すときだけ表示）
        {
            Color sbC{0xCE, 0xCD, 0xCE, 0xFF};
            FRect htr = wsScrollTrack(0);
            if (htr.w > 0) {
                FRect th = wsScrollThumb(0);
                rrFill(r, th.x, th.y, th.w, th.h, th.h / 2.f, sbC);
            }
            FRect vtr = wsScrollTrack(1);
            if (vtr.w > 0) {
                FRect th = wsScrollThumb(1);
                rrFill(r, th.x, th.y, th.w, th.h, th.w / 2.f, sbC);
            }
        }
        // ズームコントロール（scratch-blocks: 白丸 36px アイコン3連、右下 12px）
        bl::drawBlockIcon(r, "zoom_in", ui.zoomPlus.x, ui.zoomPlus.y, 36, 36,
                          1.f);
        bl::drawBlockIcon(r, "zoom_out", ui.zoomMinus.x, ui.zoomMinus.y, 36, 36,
                          1.f);
        bl::drawBlockIcon(r, "zoom_reset", ui.zoomReset.x, ui.zoomReset.y, 36,
                          36, 1.f);
        // Scratch 3 にはゴミ箱が無い（scratch-blocks trashcan:false）。
        // ドラッグ中だけ「この辺でドロップすると削除」の赤い枠をワークスペース
        // 右下に出す。
        if (drag.active && !drag.fromFlyout) {
            FRect tr2{ui.ws.x + ui.ws.w - 160, ui.ws.y + ui.ws.h - 56, 56, 44};
            rrFill(r, tr2.x, tr2.y, tr2.w, tr2.h, 8, Color{0xE6, 0x4C, 0x4C, 0x88});
            ui.trash = tr2;
            rrOutline(r, tr2.x, tr2.y, tr2.w, tr2.h, 8, kBorder);
            float cx = tr2.x + tr2.w / 2, cy = tr2.y + 10;
            fill(r, cx - 10, cy, 20, 3, Color{255, 255, 255, 255});
            fill(r, cx - 4, cy - 3, 8, 3, Color{255, 255, 255, 255});
            Path bucket;
            bucket.moveTo(cx - 8, cy + 3);
            bucket.lineTo(cx + 8, cy + 3);
            bucket.lineTo(cx + 5, cy + 24);
            bucket.lineTo(cx - 5, cy + 24);
            fillPath(r, bucket, 1.f, Color{255, 255, 255, 255});
        }
    }

    void drawStageMenu() {
        // controls.jsx: flag/stop は 32×32 ボタン（padding 6）連続配置。
        // stage-header.css: 右側に small/large 切替 + fullscreen（34×34 白
        // ボタン、1px rgba(0,0,0,.15)、radius 4）。選択は looks-transparent。
        bool running = rt && !rt->threads.empty();
        // green-flag.css: is-active = $looks-transparent（15%）背景・
        // radius 4。stop-all.css: 既定 opacity .5 / is-active 1。
        if (running)
            rrFill(r, flagRect.x, flagRect.y, 32, 32, 4,
                   Color{0xAD, 0x5C, 0xD6, 0x59});
        bl::drawBlockIcon(r, "gf_button", flagRect.x + 6, flagRect.y + 6, 20, 20,
                          1.0f);
        bl::drawBlockIcon(r, "stop_button", stopRect.x + 6, stopRect.y + 6, 20,
                          20, running ? 1.0f : 0.5f);
        // turbo-mode.jsx: turbo 中のみ 雷アイコン + 10px bold "Turbo Mode"
        if (rt && rt->turboMode) {
            float tx = stopRect.x + 40;
            bl::drawBlockIcon(r, "turbo_icon", tx, stopRect.y + 6, 20, 20,
                              1.f);
            drawText(tx + 24, stopRect.y + 11, "Turbo Mode", 10,
                     Color{0xFF, 0xAB, 0x19, 0xFF}, true);
        }
        // 右側: ToggleButtons [small][large]（34×34、連結・共有境界・
        // 端のみ radius 4、選択=looks-light-transparent 背景 + カラーアイコン
        // / 非選択=grayscale+opacity .5）+ fullscreen（stage-button 34×34 白）
        float bx = ui.stageMenu.x + ui.stageMenu.w - 34 - 8;
        float by = ui.stageMenu.y + 5;
        stageSmallRect_ = {bx - 4 - 34 - 34 + 1, by, 34, 34};
        stageLargeRect_ = {bx - 4 - 34, by, 34, 34};
        for (int i = 0; i < 2; ++i) {
            FRect tr = i == 0 ? stageSmallRect_ : stageLargeRect_;
            bool on = (i == 0) ? stageSmall_ : !stageSmall_;
            rrFill(r, tr.x, tr.y, tr.w, tr.h, 4, Color{255, 255, 255, 255});
            if (on)
                fill(r, tr.x, tr.y, tr.w, tr.h, Color{0xF3, 0xE9, 0xFF, 255});
            // 共有境界: i==0 は右辺を描かない（隣が担当）
            rrOutline(r, tr.x, tr.y, tr.w + (i == 0 ? 1 : 0), tr.h, 4,
                      kBorder, 1);
            bl::drawBlockIcon(r, i == 0 ? "stage_small" : "stage_large",
                              tr.x + 9, tr.y + 9, 16, 16, on ? 1.0f : 0.45f);
        }
        // fullscreen（34×34 白ボタン、gap 4）
        rrFill(r, bx, by, 34, 34, 4, Color{255, 255, 255, 255});
        rrOutline(r, bx, by, 34, 34, 4, kBorder, 1);
        bl::drawBlockIcon(r, stageLarge_ ? "unfullscreen" : "fullscreen",
                          bx + 9, by + 9, 16, 16, 1.0f);
        fullScrRect_ = {bx, by, 34, 34};
    }
    FRect stageSmallRect_{0, 0, 0, 0}, stageLargeRect_{0, 0, 0, 0};
    FRect fullScrRect_{0, 0, 0, 0};
    // ステージのオフスクリーンテクスチャ（UI image batch で表示するため）
    SDL_Texture* stageTex_ = nullptr;
    int stageTexW_ = 0, stageTexH_ = 0;
    float stageTexScale_ = 2.0f;   // 表示解像度の 2 倍でレンダリング
    /// ウィンドウ座標のマウスイベントをステージテクスチャ座標へ変換して
    /// SdlRenderer へ渡す（ステージをオフスクリーンに描いているため）。
    void renEvent(const SDL_Event& ev) {
        SDL_Event e2 = ev;
        auto mapXY = [&](int& x, int& y) {
            x = (int)((x - ui.stage.x) * stageTexScale_);
            y = (int)((y - ui.stage.y) * stageTexScale_);
        };
        switch (e2.type) {
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            mapXY(e2.button.x, e2.button.y);
            break;
        case SDL_MOUSEMOTION:
            mapXY(e2.motion.x, e2.motion.y);
            break;
        }
        ren.handleEvent(e2);
    }
    void drawStage() {
        // ステージはオフスクリーンテクスチャへ描いてから UI image batch で
        // 表示する（角丸クリップ・オーバーレイ重なり・イベント座標変換が自由）。
        // 全画面モードは stage-wrapper.css の fixed オーバーレイ相当のため、
        // テクスチャを介さずウィンドウへ直接 ren.draw() する（4:3 中央配置）。
        if (stageLarge_) {
            SDL_Rect wr{(int)ui.stage.x, (int)ui.stage.y,
                        (int)ui.stage.w, (int)ui.stage.h};
            SDL_RenderSetClipRect(r, &wr);
            IRect sr{wr.x, wr.y, wr.w, wr.h};
            ren.setStageRect(sr);
            ren.draw();
            SDL_RenderSetClipRect(r, nullptr);
            rrOutline(r, ui.stage.x, ui.stage.y, ui.stage.w, ui.stage.h,
                      8, kBorder, 1);
            return;
        }
        // 大ステージではテクスチャを拡大しすぎない（ソフトウェア
        // レンダラの上限対策: 面積が 960×720 相当を超えたらスケール低下）
        float tsc = stageTexScale_;
        if (ui.stage.w * tsc > 960.f)
            tsc = 960.f / ui.stage.w;
        int tw = (int)(ui.stage.w * tsc);
        int th = (int)(ui.stage.h * tsc);
        if (tw <= 0 || th <= 0) return;
        if (!stageTex_ || stageTexW_ != tw || stageTexH_ != th) {
            if (stageTex_) SDL_DestroyTexture(stageTex_);
            stageTex_ = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_TARGET, tw, th);
            stageTexW_ = tw; stageTexH_ = th;
        }
        if (!stageTex_) return;
        SDL_Texture* prevTgt = SDL_GetRenderTarget(r);
        SDL_SetRenderTarget(r, stageTex_);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderClear(r);
        // ステージ内座標 (0,0)-(tw,th) で描く
        IRect sr{0, 0, tw, th};
        ren.setStageRect(sr);
        ren.draw();
        SDL_SetRenderTarget(r, prevTgt);
        // 表示
        blitTex(stageTex_, ui.stage.x, ui.stage.y, ui.stage.w, ui.stage.h);
        // stage.css: radius 8px + 1px rgba(0,0,0,.15)
        rrOutline(r, ui.stage.x, ui.stage.y, ui.stage.w, ui.stage.h, 8,
                  kBorder, 1);
    }
    /// スプライト情報パネルの 6 フィールド（sprite-info.css:
    /// コンテナ padding 12、行1 = Sprite + name 128px | x icon34 + input48 |
    /// y icon34 + input48、行2 = Show + 34×34 トグル | Size + input64 |
    /// Direction + input64。入力は高さ 32・半径 16 のピル）
    FRect spFieldRect(int which) {
        float px = ui.spriteInfo.x + 12;
        float pr = ui.spriteInfo.x + ui.spriteInfo.w - 12;
        float y1 = ui.spriteInfo.y + 12, y2 = ui.spriteInfo.y + 52;
        if (stageSmall_) {
            // STAGE_DISPLAY_SIZES.small: row1=名前のみ・row2=x/y のみ
            // （Show/Size/Direction は描かない。Label "x"/"y" 付き
            // input-small 48px、space-between）。
            switch (which) {
            case 0: return {px, y1, pr - px, 32};   // small: ラベル無し・全幅
            case 1: return {px + 26, y2, 48, 32};
            case 2: return {pr - 48, y2, 48, 32};
            }
            return {0, 0, 0, 0};
        }
        switch (which) {
        case 0: return {px + 46, y1, 128, 32};           // name
        case 1: return {pr - 8 - 82 - 48, y1, 48, 32};   // x input
        case 2: return {pr - 48, y1, 48, 32};            // y input
        case 3: return {px + 46, y2, 68, 32};            // Show toggle group
        case 4: return {pr - 48, y2, 48, 32};            // Direction(3rem)
        case 5: return {px + 190, y2, 64, 32};           // Size (4rem)
        }
        return {0, 0, 0, 0};
    }
    /// Show のアイコン半分（0=show 左、1=hide 右、-1=該当なし）
    int spShowHalf(float mx, float my) {
        FRect fr = spFieldRect(3);
        if (!hit(fr, mx, my)) return -1;
        return mx < (fr.x + fr.w / 2.f) ? 0 : 1;
    }
    int spFieldAt(float mx, float my) {
        for (int i = 0; i < 6; ++i) {
            FRect fr = spFieldRect(i);
            if (fr.w > 0 && hit(fr, mx, my)) return i;
        }
        return -1;
    }
    void drawSpriteInfo() {
        // sprite-info.css: 白・padding .75rem・border-top-left/right
        // radius $space（8px、上側だけ）・下 1px 境界
        rrFill(r, ui.spriteInfo.x, ui.spriteInfo.y, ui.spriteInfo.w,
               ui.spriteInfo.h, 8, kPanel);
        // 下 8px は角を立てる（下側は隣パネルと直線で接する）
        fill(r, ui.spriteInfo.x, ui.spriteInfo.y + ui.spriteInfo.h - 8,
             ui.spriteInfo.w, 8, kPanel);
        outline(r, ui.spriteInfo.x, ui.spriteInfo.y + ui.spriteInfo.h - 1,
                ui.spriteInfo.x + ui.spriteInfo.w,
                ui.spriteInfo.y + ui.spriteInfo.h - 1, kBorder);
        Target* t = selected();
        if (!t) return;
        // ステージ選択時はパネル全体を無効表示（scratch-gui:
        // sprites[selectedId] が無い → spriteInfoDisabled=true）
        bool disabled = t->isStage;
        // sprite-info.jsx: "Sprite" ラベル + name 入力、x/y はアイコン +
        // 小入力。行2 は Show トグル + Size + Direction（全てラベル左）。
        Color labelC = disabled ? Color{0x57, 0x5E, 0x75, 0x88} : kFg;
        float px = ui.spriteInfo.x + 12;
        float pr = ui.spriteInfo.x + ui.spriteInfo.w - 12;
        float y1 = ui.spriteInfo.y + 12, y2 = ui.spriteInfo.y + 52;
        if (!stageSmall_)
            drawText(px, y1 + 10, "Sprite", 10, labelC, true);
        if (!stageSmall_)
            drawText(px, y2 + 10, "Show", 10, labelC);
        // x/y: sprite-info.jsx は標準ビューで <Label text="x/y">（10px
        // opacity .8 のテキスト）。アイコン（icon--x/y.svg）は large
        // stage のみの icon-wrapper。
        FRect xr = spFieldRect(1), yr = spFieldRect(2);
        float xyLabelY = stageSmall_ ? y2 : y1;
        if (stageLarge_) {
            bl::drawBlockIcon(r, "spr_x", xr.x - 34 + 9, xyLabelY + 8, 16,
                              16, disabled ? 0.4f : 0.9f);
            bl::drawBlockIcon(r, "spr_y", yr.x - 34 + 9, xyLabelY + 8, 16,
                              16, disabled ? 0.4f : 0.9f);
        } else {
            Color lbl = labelC; lbl.a = (uint8_t)(lbl.a * 0.8f);
            drawText(xr.x - 12, xyLabelY + 10, "x", 10, lbl, true);
            drawText(yr.x - 12, xyLabelY + 10, "y", 10, lbl, true);
        }
        // Size / Direction ラベル（入力の左 8px）
        if (!stageSmall_) {
            FRect szr = spFieldRect(5), drr = spFieldRect(4);
            float swl = textW("Size", 10);
            drawText(szr.x - swl - 8, y2 + 10, "Size", 10, labelC);
            float dwl = textW("Direction", 10);
            drawText(drr.x - dwl - 8, y2 + 10, "Direction", 10, labelC);
        }

        std::string vals[6] = {utf16ToUtf8(t->getName()), fmtNum(t->x),
                               fmtNum(t->y), "",
                               fmtNum(t->direction), fmtNum(t->size)};
        if (disabled)
            for (auto& v : vals) v.clear();
        for (int i = 0; i < 6; ++i) {
            FRect fr = spFieldRect(i);
            if (fr.w <= 0) continue;
            bool foc = spEdit_.active && spEdit_.which == i;
            std::string dispVal = vals[i];
            if (foc) {
                dispVal = spEdit_.buf;
            } else if (i == 4 && dirPicker_.open) {
                dispVal = fmtNum(t->direction);
            }
            if (i == 3) {
                // Show/Hide トグル（34×34、境界折り畳み、選択=looks-transparent）
                bool showSel = !disabled && t->visible;
                bool hideSel = !disabled && !t->visible;
                float bw = fr.w / 2.f;
                FRect bL{fr.x, fr.y, bw, fr.h};
                FRect bR{fr.x + bw, fr.y, bw, fr.h};
                fill(r, bL.x, bL.y, bL.w, bL.h,
                     showSel ? Color{133, 92, 214, 38}
                             : Color{255, 255, 255, 255});
                fill(r, bR.x, bR.y, bR.w, bR.h,
                     hideSel ? Color{133, 92, 214, 38}
                             : Color{255, 255, 255, 255});
                outline(r, fr.x, fr.y, fr.w, fr.h, kBorder);
                line(r, fr.x + bw, fr.y, fr.x + bw, fr.y + fr.h, kBorder, 1);
                bl::drawBlockIcon(r, "spr_show", bL.x + (bw - 18) / 2.f,
                                  bL.y + (fr.h - 18) / 2.f, 18, 18,
                                  showSel ? 1.0f : 0.5f);
                bl::drawBlockIcon(r, "spr_hide", bR.x + (bw - 18) / 2.f,
                                  bR.y + (fr.h - 18) / 2.f, 18, 18,
                                  hideSel ? 1.0f : 0.5f);
            } else {
                // ピル入力（半径 16、10px bold #575E75、focus=紫 ring）
                rrFill(r, fr.x, fr.y, fr.w, fr.h, 16,
                       Color{255, 255, 255, 255});
                rrOutline(r, fr.x, fr.y, fr.w, fr.h, 16,
                          foc ? kLooksSecondary : kBorder, foc ? 1.5f : 1.f);
                if (foc)
                    rrOutline(r, fr.x - 2, fr.y - 2, fr.w + 4, fr.h + 4, 16,
                              Color{133, 92, 214, 89}, 2.f);
                // input.css: input-form padding 0 .75rem（左寄せ）。
                // input-small は text-align:center（x/y/Direction/Size）。
                // 名前入力(.sprite-input)は小さくないので左寄せ。
                bool centered = (i == 1 || i == 2 || i == 4 || i == 5);
                float tw = textW(dispVal, 10, true);
                float txx = centered ? fr.x + (fr.w - tw) / 2.f
                                     : fr.x + 12;
                drawText(txx, fr.y + 10, dispVal, 10, labelC, true);
                if (foc)  // カーソル（input caret）
                    fill(r, txx + tw + 1, fr.y + 8, 1, fr.h - 16, kFg);
            }
        }
        // 上下の区切り: コンテナは白・下 1px 境界（スプライトセレクタへ続く）
        line(r, ui.spriteInfo.x, ui.spriteInfo.y + ui.spriteInfo.h - 1,
             ui.spriteInfo.x + ui.spriteInfo.w,
             ui.spriteInfo.y + ui.spriteInfo.h - 1, kBorder, 1);
    }
    std::string fmtNum(double v) {
        char buf[32];
        if (std::floor(v) == v && std::fabs(v) < 1e15)
            std::snprintf(buf, sizeof buf, "%lld", (long long)v);
        else
            std::snprintf(buf, sizeof buf, "%.4g", v);
        return buf;
    }
    void drawSpriteSel() {
        fillR(ui.spriteSel, kUiSecondary);
        outlineR(ui.spriteSel, kBorder);
        // スプライトタイル（sprite-selector.css: width=calc(w/5−8px)
        // clamp 64..96px 正方形、margin 4px、border 2px、radius 8、
        // 画像 max 32px、下部 sprite-info ストリップ（padding 4、10px、
        // 選択時 #855CD6 背景+白）、番号 top 0.15rem left 0.15rem、
        // delete-button top/right −10px）
        float tw = std::min(96.f, std::max(64.f,
                        (ui.spriteSel.w - 8 - 4 * 5) / 5.f));
        const float th = tw, gap = 4;
        float x = ui.spriteSel.x + 4, y = ui.spriteSel.y + 4;
        int cols = std::max(1, (int)((ui.spriteSel.w - 8) / (tw + gap)));
        int i = 0;
        for (size_t ti = 0; ti < rt->targets.size(); ++ti) {
            Target* t = rt->targets[ti];
            if (t->isStage) continue; // Stage は専用カードで描画
            if (i > 0 && i % cols == 0) { x = ui.spriteSel.x + 4; y += th + gap; }
            bool sel = (int)ti == selTarget;
            bool dragging = sdr_.active && sdr_.ti == (int)ti;
            // ドラッグ中タイルは薄く（tile が半透明で残る）
            rrFill(r, x, y, tw, th, 8,
                   dragging ? Color{0xFA, 0xFC, 0xFF, 120}
                            : (sel ? kPanel
                                   : Color{0xFA, 0xFC, 0xFF, 255}));
            rrOutline(r, x, y, tw, th, 8,
                      dragging ? Color{0x85, 0x5C, 0xD6, 120}
                               : (sel ? kLooksSecondary : kBorder),
                      2.f);
            if (sel) {
                // box-shadow 0 0 0 4px $looks-transparent
                rrOutline(r, x - 2, y - 2, tw + 4, th + 4, 10,
                          Color{0x85, 0x5C, 0xD6, 0x33}, 4.f);
            }
            // サムネイル（max 32×32、画像域の中央）
            double lw = 0, lh = 0, rcx = 0, rcy = 0;
            if (SDL_Texture* tex = ren.spriteTexture(t, &lw, &lh, &rcx, &rcy)) {
                float sc = std::min(32.f / (float)lw, 32.f / (float)lh);
                blitTex(tex, x + tw / 2.f - (float)lw * sc / 2.f,
                        y + (th - 22.f) / 2.f - (float)lh * sc / 2.f,
                        (float)lw * sc, (float)lh * sc);
            }
            // 下部 sprite-info（padding 4、font 10px、選択=紫背景+白文字）
            FRect nb{x + 2, y + th - 24, tw - 4, 20};
            if (sel)
                rrFill(r, nb.x, nb.y, nb.w, nb.h, 4, kLooksSecondary);
            std::string nm = utf16ToUtf8(t->getName());
            int nw = textW(nm, 10);
            drawText(x + (tw - nw) / 2.f, y + th - 18, nm, 10,
                     sel ? Color{255, 255, 255, 255} : kFg);
            // 順番番号（左上、10px bold）
            drawText(x + 4, y + 2, std::to_string(i + 1), 10, kFg, true);
            // 削除バッジ（delete-button.css: 28px 円、top/right −0.625rem
            // → 中心はタイル右上の外側 +10px）
            if (sel) {
                float dx = x + tw + 10 - 4, dy = y - 10 + 4;
                discFill(r, dx, dy, 14, kLooksSecondary);
                line(r, dx - 4.5f, dy - 4.5f, dx + 4.5f, dy + 4.5f,
                     Color{255, 255, 255, 255}, 2.f);
                line(r, dx + 4.5f, dy - 4.5f, dx - 4.5f, dy + 4.5f,
                     Color{255, 255, 255, 255}, 2.f);
                spriteDelRect_ = {dx - 14, dy - 14, 28, 28};
            }
            x += tw + gap;
            i++;
        }
        // ドラッグ中: 挿入位置に 2px 紫線 + カーソル位置に半透明タイル
        if (sdr_.active && sdr_.insert >= 0) {
            int n = spriteCount();
            int ins = sdr_.insert;
            FRect slot;
            if (ins < n) {
                slot = spriteTileRect(ins);
            } else {
                slot = spriteTileRect(n - 1);
                slot.x += slot.w + 4;   // 末尾 = 最後のタイルの右
            }
            line(r, slot.x - 2, slot.y, slot.x - 2, slot.y + slot.h,
                 kLooksSecondary, 2.f);
            FRect gr = spriteTileRect(spriteVisIndex(sdr_.ti));
            float gx = sdr_.mx - gr.w / 2.f, gy = sdr_.my - gr.h / 2.f;
            rrFill(r, gx, gy, gr.w, gr.h, 8, Color{0xFA, 0xFC, 0xFF, 160});
            rrOutline(r, gx, gy, gr.w, gr.h, 8, kLooksSecondary, 2.f);
            Target* dt = (sdr_.ti >= 0 &&
                          sdr_.ti < (int)rt->targets.size())
                             ? rt->targets[sdr_.ti]
                             : nullptr;
            if (dt) {
                double lw = 0, lh = 0, rcx = 0, rcy = 0;
                if (SDL_Texture* tex =
                        ren.spriteTexture(dt, &lw, &lh, &rcx, &rcy)) {
                    float sc =
                        std::min(32.f / (float)lw, 32.f / (float)lh);
                    blitTex(tex, gx + gr.w / 2.f - (float)lw * sc / 2.f,
                            gy + (gr.h - 22.f) / 2.f - (float)lh * sc / 2.f,
                            (float)lw * sc, (float)lh * sc);
                }
            }
        }
        // + スプライト追加（action-menu.css: main-button 44px
        // $looks-secondary、main-icon 28px）
        float bx = ui.spriteSel.x + ui.spriteSel.w - 48;
        float by = ui.spriteSel.y + ui.spriteSel.h - 48;
        discFill(r, bx + 22, by + 22, 22, kLooksSecondary);
        bl::drawBlockIcon(r, "addsprite", bx + 8, by + 8, 28, 28, 1.0f);
        addSpriteRect_ = {bx, by, 44, 44};

        // ---- 右側 Stage 選択カード (ui.stageSel) ----
        Target* stageTgt = nullptr;
        for (Target* tg : rt->targets) if (tg->isStage) { stageTgt = tg; break; }
        if (stageTgt) {
            bool stageSel = (selTarget >= 0 && (size_t)selTarget < rt->targets.size() &&
                             rt->targets[selTarget]->isStage);
            // stage-selector.css: 白・1px 境界（下なし・上 8px 丸、
            // is-selected は 10px 丸）・選択時は border-color 紫 +
            // box-shadow 0 0 0 4px $looks-transparent、ヘッダー 42px 紫。
            fillR(ui.stageSel, kPanel);
            outline(r, ui.stageSel.x, ui.stageSel.y, ui.stageSel.w,
                    ui.stageSel.h, stageSel ? kLooksSecondary : kBorder);
            if (stageSel)
                rrOutline(r, ui.stageSel.x - 4, ui.stageSel.y - 4,
                          ui.stageSel.w + 8, ui.stageSel.h + 8, 14,
                          Color{133, 92, 214, 89}, 4.f);
            // ヘッダー "Stage"（42px、下 1px 境界、選択時は紫背景・白文字）
            FRect hRect{ui.stageSel.x, ui.stageSel.y, ui.stageSel.w, 42};
            if (stageSel)
                fill(r, hRect.x, hRect.y, hRect.w, hRect.h, kLooksSecondary);
            line(r, hRect.x, hRect.y + hRect.h - 1, hRect.x + hRect.w,
                 hRect.y + hRect.h - 1,
                 stageSel ? kLooksSecondary : kBorder, 1);
            float thw = textW("Stage", 10, true);
            drawText(hRect.x + (hRect.w - thw) / 2.f,
                     hRect.y + (42 - 10) / 2.f - 2, "Stage", 10,
                     stageSel ? Color{255, 255, 255, 255} : kFg, true);
            // サムネイル（最大 64×48、1px 境界・radius 4）
            double lw = 0, lh = 0, rcx = 0, rcy = 0;
            if (SDL_Texture* tex = ren.spriteTexture(stageTgt, &lw, &lh, &rcx, &rcy)) {
                float sc = std::min(64.f / (float)lw, 48.f / (float)lh);
                float bw2 = (float)lw * sc, bh2 = (float)lh * sc;
                float tx = ui.stageSel.x + (ui.stageSel.w - bw2) / 2.f;
                float ty = ui.stageSel.y + 48.f;
                blitTex(tex, tx, ty, bw2, bh2);
                // costume-canvas: 1px 境界 + radius 4 + inset 0 0 4px
                // rgba(0,0,0,.15) の内側影（4辺に薄いグラデ線で近似）
                rrOutline(r, tx, ty, bw2, bh2, 4, kBorder, 1);
                rrOutline(r, tx + 1, ty + 1, bw2 - 2, bh2 - 2, 3,
                          Color{0, 0, 0, 20}, 1.5f);
                rrOutline(r, tx + 3, ty + 3, bw2 - 6, bh2 - 6, 2,
                          Color{0, 0, 0, 10}, 1.5f);
            }
            // "Backdrops" 9.6px + 枚数
            const auto* cList = stageTgt->getCostumes();
            int nBg = cList ? (int)cList->size() : 1;
            drawText(ui.stageSel.x + (ui.stageSel.w - textW("Backdrops", 10)) / 2.f,
                     ui.stageSel.y + 106, "Backdrops", 10, kFg);
            drawText(ui.stageSel.x + (ui.stageSel.w - textW(std::to_string(nBg), 10, true)) / 2.f,
                     ui.stageSel.y + 122, std::to_string(nBg), 10, kFg, true);
            // + 背景追加（ActionMenu 44px、bottom .75rem）
            float bbx = ui.stageSel.x + (ui.stageSel.w - 44) / 2.f;
            float bby = ui.stageSel.y + ui.stageSel.h - 12 - 44;
            discFill(r, bbx + 22, bby + 22, 22, kLooksSecondary);
            bl::drawBlockIcon(r, "addbackdrop", bbx + 8, bby + 8, 28, 28,
                              1.0f);
            addBackdropRect_ = {bbx, bby, 44, 44};
        }
    }
    FRect addSpriteRect_{0, 0, 0, 0};
    FRect addBackdropRect_{0, 0, 0, 0};
    FRect spriteDelRect_{0, 0, 0, 0};  // 選択スプライトの削除バッジ

    // ---- 方向ピッカーポップオーバー (Scratch DirectionPicker 再現) ----
    void drawDirectionPicker() {
        if (!dirPicker_.open) return;
        Target* t = selected();
        if (!t) return;
        float px = dirPicker_.x, py = dirPicker_.y;
        float pw = dirPicker_.w, ph = dirPicker_.h;
        // ポップオーバー外枠 + 白背景
        rrFill(r, px, py, pw, ph, 8, Color{255, 255, 255, 255});
        rrOutline(r, px, py, pw, ph, 8, kBorder, 1.5f);

        // ダイアル面 (dial_face アイコン 96x96)
        float dcx = px + pw / 2.f;
        float dcy = py + 58.f;
        dirPicker_.dialCx = dcx;
        dirPicker_.dialCy = dcy;
        dirPicker_.dialR = 46.f;
        bl::drawBlockIcon(r, "dial_face", dcx - 48, dcy - 48, 96, 96, 1.0f);

        // ダイアル針 / ハンドル (icon--handle.svg: 矢印付きハンドル)
        // Scratch 方向角: 0=上, 90=右, 180=下, -90/270=左
        // 画面上での角: rad = (t->direction - 90) * (M_PI / 180.0)
        double rad = ((double)t->direction - 90.0) * (M_PI / 180.0);
        float hx = dcx + (float)(std::cos(rad) * 38.0);
        float hy = dcy + (float)(std::sin(rad) * 38.0);
        bl::drawBlockIcon(r, "dial_handle", hx - 16, hy - 16, 32, 32, 1.0f);

        // 回転スタイル切替ボタン行 (全回転 / 左右 / 回転しない)
        float by = py + ph - 38;
        float bw = 32, bh = 28;
        float bStart = px + (pw - (bw * 3 + 8)) / 2.f;
        dirPicker_.btnAll = {bStart, by, bw, bh};
        dirPicker_.btnLr = {bStart + bw + 4, by, bw, bh};
        dirPicker_.btnNo = {bStart + bw * 2 + 8, by, bw, bh};

        auto drawRotBtn = [&](FRect br, const char* icName, bool active) {
            rrFill(r, br.x, br.y, br.w, br.h, 4,
                   active ? kSel : Color{255, 255, 255, 255});
            rrOutline(r, br.x, br.y, br.w, br.h, 4,
                      active ? kLooksSecondary : kBorder, active ? 1.5f : 1.f);
            bl::drawBlockIcon(r, icName, br.x + (bw - 18) / 2.f,
                              br.y + (bh - 18) / 2.f, 18, 18,
                              active ? 1.0f : 0.5f);
        };
        drawRotBtn(dirPicker_.btnAll, "rot_all", t->rotationStyle == "all around");
        drawRotBtn(dirPicker_.btnLr, "rot_lr", t->rotationStyle == "left-right");
        drawRotBtn(dirPicker_.btnNo, "rot_no", t->rotationStyle == "don't rotate");
    }

    // ---- ActionMenu 展開（action-menu.css/jsx 準拠）-------------------
    /// which 別の (icon, label) リスト（実ソースの moreButtons 順、
    /// 上→下 = Upload/Surprise/Paint|Record/Library）。
    static void amEntries(int which, const char* (&icons)[4],
                          const char* (&labels)[4], int& n) {
        switch (which) {
        case 3: n = 4;
            icons[0] = "am_upload";    labels[0] = "Upload";
            icons[1] = "am_surprise";  labels[1] = "Surprise";
            icons[2] = "am_paint";     labels[2] = "Paint";
            icons[3] = "am_search";    labels[3] = "Choose a Sprite";
            break;
        case 4: n = 4;
            icons[0] = "am_upload";    labels[0] = "Upload";
            icons[1] = "am_surprise";  labels[1] = "Surprise";
            icons[2] = "am_paint";     labels[2] = "Paint";
            icons[3] = "am_search";    labels[3] = "Choose a Backdrop";
            break;
        case 6: n = 4;
            icons[0] = "am_upload";    labels[0] = "Upload";
            icons[1] = "am_surprise";  labels[1] = "Surprise";
            icons[2] = "am_paint";     labels[2] = "Paint";
            icons[3] = "am_costume";   labels[3] = "Choose a Costume";
            break;
        case 7: n = 4;
            icons[0] = "am_upload";    labels[0] = "Upload";
            icons[1] = "am_surprise";  labels[1] = "Surprise";
            icons[2] = "am_record";    labels[2] = "Record";
            icons[3] = "am_sound";     labels[3] = "Choose a Sound";
            break;
        default: n = 0; break;
        }
    }
    /// 展開列の外枠（main-button の直上・水平中心は main-button に揃う）。
    FRect amPanelRect() {
        int n = 4;
        return {am_.cx - 18, am_.mainY - n * 36 + 22 - n * 36 - 22,
                36, 0};
    }
    void drawActionMenu() {
        if (!am_.open) return;
        const char* icons[4]; const char* labels[4]; int n = 0;
        amEntries(am_.which, icons, labels, n);
        if (!n) return;
        const float bw = 36;
        float x = am_.cx - bw / 2;
        // more-buttons-outer: 36px 幅・上角丸・bottom が main-button の
        // 中心に重なる（padding-bottom = main/2 = 22）
        float top = am_.mainY + 22 - n * bw - 22;
        float bot = am_.mainY + 22;
        // ピル本体（上角丸 36/2、下は main-button に隠れる）
        float bh = bot - top;
        if (bh > 0) {
            // 簡易: 上部角丸 rect + 下部 rect で pill 形状
            rrFill(r, x, top, bw, 36, 18, Color{0x71, 0x4E, 0xB6, 255});
            fill(r, x, top + 18, bw, bh - 18, Color{0x71, 0x4E, 0xB6, 255});
        }
        am_.items.clear();
        for (int i = 0; i < n; ++i) {
            FRect b{x, top + i * bw, bw, bw};
            am_.items.push_back(b);
            bl::drawBlockIcon(r, icons[i], b.x + 8, b.y + 8, 20, 20, 1.f);
            if (i == am_.hover) {
                // tooltip place="left"（ReactTooltip 既定）: 白・黒文字・
                // 4px radius・左矢印（▶ は手前の小三角形）
                float tw = textW(labels[i], 11) + 16;
                float tx = b.x - 6 - tw, ty = b.y + (bw - 22) / 2;
                rrFill(r, tx, ty, tw, 22, 4, Color{255, 255, 255, 255});
                rrOutline(r, tx, ty, tw, 22, 4, kBorder, 1);
                drawText(tx + 8, ty + 5, labels[i], 11, kFg);
            }
        }
    }
    int amItemAt(float mx, float my) {
        for (int i = 0; i < (int)am_.items.size(); ++i)
            if (hit(am_.items[i], mx, my)) return i;
        return -1;
    }

    // ---- カラーピッカーポップオーバー（scratch-paint color-picker.jsx）----
    void drawColorPicker() {
        if (!cp_.open) return;
        float px = cp_.x, py = cp_.y;
        bool grad = (cp_.which == 0);
        cp_.w = 232;
        // Popover-body: 白・1px #ddd・radius 4・padding 4・
        // box-shadow 0 0 8px rgba(0,0,0,.3)。preferPlace below。
        float ch = 4;
        if (grad) { ch += 40; if (cp_.gradient != 0) ch += 48; }
        ch += 38 * 3 + 4 + 32 + 4;
        cp_.h = ch;
        rrFill(r, px + 2, py + 4, cp_.w, cp_.h, 4, Color{0, 0, 0, 40});
        rrFill(r, px, py, cp_.w, cp_.h, 4, Color{255, 255, 255, 255});
        rrOutline(r, px, py, cp_.w, cp_.h, 4,
                  Color{0xDD, 0xDD, 0xDD, 255}, 1);
        float cy = py + 4;
        auto hline = [&](float yy) {
            line(r, px + 8, yy, px + cp_.w - 8, yy,
                 Color{0xDD, 0xDD, 0xDD, 255}, 1);
        };
        if (grad) {
            // gradient-picker-row: solid/horz/vert/radial 24px アイコン
            // （非選択は saturate(0%) → グレー）
            float gx = px + (cp_.w - 4 * 24 - 3 * 8) / 2;
            const char* gi[4] = {"cp_solid", "cp_horz", "cp_vert",
                                 "cp_radial"};
            FRect* gr[4] = {&cp_.btnSolid, &cp_.btnHorz, &cp_.btnVert,
                            &cp_.btnRadial};
            for (int i = 0; i < 4; ++i) {
                *gr[i] = {gx + i * 32, cy + 8, 24, 24};
                bl::drawBlockIcon(r, gi[i], gx + i * 32, cy + 8, 24, 24,
                                  cp_.gradient == i ? 1.f : 0.4f);
            }
            cy += 40;
            hline(cy); cy += 0;
            // 非 SOLID 時: グラデ端色 2 スウォッチ + swap
            if (cp_.gradient != 0) {
                float sy = cy + 8;
                cp_.sw1 = {px + 40, sy, 32, 32};
                cp_.btnSwap = {px + cp_.w / 2 - 22, sy + 6, 44, 20};
                cp_.sw2 = {px + cp_.w - 72, sy, 32, 32};
                auto sw = [&](FRect r2, Color c, bool act) {
                    rrFill(r, r2.x, r2.y, r2.w, r2.h, 4, c);
                    rrOutline(r, r2.x, r2.y, r2.w, r2.h, 4,
                              act ? kLooksSecondary
                                  : Color{0xDD, 0xDD, 0xDD, 255}, 1);
                    if (act)
                        rrOutline(r, r2.x - 3, r2.y - 3, r2.w + 6,
                                  r2.h + 6, 6,
                                  Color{0xAA, 0x4D, 0xC0, 0x40}, 2);
                };
                sw(cp_.sw1, hsb(cp_.hue, cp_.sat, cp_.bri),
                   cp_.colorIndex == 0);
                sw(cp_.sw2, hsb(cp_.hue2, cp_.sat2, cp_.bri2),
                   cp_.colorIndex == 1);
                bl::drawBlockIcon(r, "cp_swap", cp_.btnSwap.x + 12,
                                  cp_.btnSwap.y + 2, 20, 16, 1.f);
                cy += 48;
            }
        }
        // HSB スライダ 3 本（row-header 10.4px + 読み出し、
        // slider: 150×22 radius 11、ハンドル 26px 白丸 + 4px リング）
        struct SlRow { const char* name; float val; int idx; };
        SlRow rows[3] = {{"Color", cp_.hue, 0},
                         {"Saturation", cp_.sat, 1},
                         {"Brightness", cp_.bri, 2}};
        FRect* sre[3] = {&cp_.slH, &cp_.slS, &cp_.slB};
        for (int i = 0; i < 3; ++i) {
            float ry = cy + 8;
            drawText(px + 8, ry + 4, rows[i].name, 10, kFg, true);
            char vb[8];
            std::snprintf(vb, sizeof vb, "%d", (int)std::lround(rows[i].val));
            drawText(px + 8 + textW(rows[i].name, 10, true) + 10, ry + 4,
                     vb, 10, kFg);
            float sx = px + cp_.w - 8 - 150;
            *sre[i] = {sx, ry, 150, 22};
            // グラデ（h: 虹 / s: 現色 → 白 | 彩度 0..100 / b: 黒→現色）
            const int N = 60;
            for (int k = 0; k < N; ++k) {
                Color cc;
                if (i == 0) cc = hsb(k * 100.f / N, 100, 100);
                else if (i == 1) cc = hsb(cp_.hue, k * 100.f / N, 100);
                else           cc = hsb(cp_.hue, cp_.sat, k * 100.f / N);
                fill(r, sx + k * 2.5f, ry, 2.5f, 22, cc);
            }
            rrOutline(r, sx, ry, 150, 22, 11,
                      Color{0, 0, 0, 0x30}, 1);
            float hx = sx + rows[i].val / 100.f * 150;
            discFill(r, hx, ry + 11, 13, Color{255, 255, 255, 255});
            ring(r, hx, ry + 11, 13, 4, Color{0, 0, 0, 38});
            cy += 38;
        }
        cy += 4;
        // swatch-row: nofill（左）/ eyedropper（右）24px
        cp_.noFillR = {px + 8, cy, 24, 24};
        cp_.eyeR = {px + cp_.w - 32, cy, 24, 24};
        rrOutline(r, cp_.noFillR.x, cy, 24, 24, 4,
                  Color{0xDD, 0xDD, 0xDD, 255}, 1);
        bl::drawBlockIcon(r, "cp_nofill", px + 8, cy, 24, 24, 1.f);
        rrOutline(r, cp_.eyeR.x, cy, 24, 24, 4,
                  Color{0xDD, 0xDD, 0xDD, 255}, 1);
        bl::drawBlockIcon(r, "tool_eyedrop", cp_.eyeR.x + 2, cy + 2, 20,
                          20, 1.f);
    }

    void drawBackpack() {
        // backpack.css: ヘッダーは白・1px rgba(0,0,0,.15)・上右 8px 丸・
        // 13.6px #575E75。リストは #E5F0FF 上に 64×72 白タイル。
        if (bpOpen) {
            fillR(ui.bpPanel, kBody);
            outline(r, ui.bpPanel.x + ui.bpPanel.w - 1, ui.bpPanel.y, 1,
                    ui.bpPanel.h, kBorder);
            float x = ui.bpPanel.x + 8, y = ui.bpPanel.y + 8;
            for (int i = 0; i < (int)backpack.size(); ++i) {
                rrFill(r, x, y, 64, 72, 4, kPanel);
                rrOutline(r, x, y, 64, 72, 4, kBorder, 1);
                drawText(x + 4, y + 6, backpack[i].label, 9, kFg);
                x += 64 + 8;
            }
            if (backpack.empty()) {
                // backpack.jsx statusMessage: "Backpack is empty" 中央・
                // .85rem=13.6px・$text-primary
                const char* msg = "Backpack is empty";
                drawText(ui.bpPanel.x + (ui.bpPanel.w - textW(msg, 13)) / 2.f,
                         ui.bpPanel.y + ui.bpPanel.h / 2.f - 7, msg, 13,
                         kFg);
            }
        }
        // backpack-header: 白・全面 1px rgba(0,0,0,.15)・右上 8px 丸のみ・
        // margin-top .5rem・13.6px #575E75 中央（chevron 無し — backpack.jsx
        // は FormattedMessage のみ）。
        fillR(ui.bpBar, kPanel);
        outline(r, ui.bpBar.x, ui.bpBar.y, ui.bpBar.x + ui.bpBar.w,
                ui.bpBar.y + ui.bpBar.h, kBorder);
        // 右上だけ radius 8（上辺右端を body 色で欠き取る）
        rrFill(r, ui.bpBar.x + ui.bpBar.w - 2, ui.bpBar.y - 2, 11, 11, 0,
               kBody);
        // 欠き取った部分を含む右上の円弧枠は省略（1px 差異以下）
        drawText(ui.bpBar.x + ui.bpBar.w / 2 - textW("Backpack", 13) / 2,
                 ui.bpBar.y + (ui.bpBar.h - 13) / 2 + 2, "Backpack", 13,
                 kFg);
    }
    // ---- AssetPanel（Costumes/Sounds 共通、asset-panel.css 準拠）------------
    // 左: #D9E3F2 幅150px セレクタ（80×80 タイル縦積み + 下部 ＋ActionMenu）
    // 右: 白 detail（左 1px 境界）— costume: name + プレビュー / sound: 編集 UI
    struct AssetTile { FRect r; int index; };
    std::vector<AssetTile> assetTiles_;
    FRect assetDelRect_{0, 0, 0, 0};
    FRect assetAddBtn_{0, 0, 0, 0};
    FRect soundPlayRect_{0, 0, 0, 0};

    /// WAV の rate と総サンプルフレーム数（チャンネル共通）を返す。
    static bool wavPcmInfo(const Sound& snd, uint32_t& rate,
                           uint32_t& frames) {
        rate = 0; frames = 0;
        const uint8_t* d = snd.data.data();
        size_t n = snd.data.size();
        if (n < 44 || std::memcmp(d, "RIFF", 4) ||
            std::memcmp(d + 8, "WAVE", 4)) return false;
        uint16_t fmt = 0, ch = 0, bits = 0;
        size_t off = 12;
        uint32_t pcmLen = 0;
        while (off + 8 <= n) {
            uint32_t sz;
            std::memcpy(&sz, d + off + 4, 4);
            if (!std::memcmp(d + off, "fmt ", 4) && sz >= 16) {
                std::memcpy(&fmt, d + off + 8, 2);
                std::memcpy(&ch, d + off + 10, 2);
                std::memcpy(&rate, d + off + 12, 4);
                std::memcpy(&bits, d + off + 22, 2);
            } else if (!std::memcmp(d + off, "data", 4))
                pcmLen = std::min<uint32_t>(sz, (uint32_t)(n - off - 8));
            off += 8 + sz + (sz & 1);
        }
        if (!pcmLen || !ch || (fmt != 1 && fmt != 3) ||
            (bits != 16 && bits != 32)) return false;
        frames = pcmLen / (bits / 8) / ch;
        return rate > 0;
    }
    /// WAV (PCM 16bit/float, RIFF) → per-bucket 振幅包絡（0..1）。失敗 false。
    static bool wavEnvelope(const Sound& snd, std::vector<float>& out,
                            int buckets) {
        const uint8_t* d = snd.data.data();
        size_t n = snd.data.size();
        if (n < 44 || std::memcmp(d, "RIFF", 4) ||
            std::memcmp(d + 8, "WAVE", 4)) return false;
        uint16_t fmt = 0, ch = 0, bits = 0;
        uint32_t rate = 0;
        const uint8_t* pcm = nullptr;
        uint32_t pcmLen = 0;
        size_t off = 12;
        while (off + 8 <= n) {
            uint32_t sz;
            std::memcpy(&sz, d + off + 4, 4);
            if (!std::memcmp(d + off, "fmt ", 4) && sz >= 16) {
                std::memcpy(&fmt, d + off + 8, 2);
                std::memcpy(&ch, d + off + 10, 2);
                std::memcpy(&rate, d + off + 12, 4);
                std::memcpy(&bits, d + off + 22, 2);
            } else if (!std::memcmp(d + off, "data", 4)) {
                pcm = d + off + 8;
                pcmLen = std::min<uint32_t>(sz, (uint32_t)(n - off - 8));
            }
            off += 8 + sz + (sz & 1);
        }
        if (!pcm || !ch || (fmt != 1 && fmt != 3)) return false;
        int bps = bits / 8;
        if (bps != 2 && bps != 4) return false;
        size_t frames = pcmLen / (bps * ch);
        if (!frames) return false;
        out.assign(buckets, 0.f);
        for (int b = 0; b < buckets; ++b) {
            size_t f0 = frames * b / buckets, f1 = frames * (b + 1) / buckets;
            float mx = 0;
            for (size_t f = f0; f < f1; ++f) {
                const uint8_t* sp = pcm + f * bps * ch;
                float v = 0;
                if (fmt == 1) {
                    int16_t s16;
                    std::memcpy(&s16, sp, 2);
                    v = s16 / 32768.f;
                } else {
                    float f32;
                    std::memcpy(&f32, sp, 4);
                    v = f32;
                }
                mx = std::max(mx, std::fabs(v));
            }
            out[b] = mx;
        }
        return true;
    }

    /// セレクタ列（タイル + add ボタン）。item 数は呼び出し側で決める。
    void drawAssetSelector(float x, float y, float h) {
        fill(r, x, y, 150, h, Color{0xD9, 0xE3, 0xF2, 0xFF});
    }
    /// selector.css: .new-buttons はタイルの上に 100px の
    /// linear-gradient(rgba(232,237,241,0)→1) フェードを敷き、その上に
    /// ＋ActionMenu を置く（タイルの後に呼ぶ）。
    void drawAssetNewButtons(float x, float h) {
        // フェード（256 段で線形グラデーション、タイル側のみ）
        for (int i = 0; i < 64; ++i) {
            float fy = h - 100 + i * (100.f / 64);
            fill(r, x, fy, 150, 100.f / 64 + 1,
                 Color{0xE8, 0xED, 0xF1, (uint8_t)(i * 255 / 63)});
        }
        // ActionMenu main-button: 44px 紫・icon 28px（costume ＋ 猫、
        // sound ＋ 音符 — selector.jsx の main button に専用 icon）
        float bx = x + 75 - 22, by = ui.body.y + h - 52;
        assetAddBtn_ = {bx, by, 44, 44};
        discFill(r, bx + 22, by + 22, 22, kLooksSecondary);
        bl::drawBlockIcon(r, tab == 1 ? "addsprite" : "addsound",
                          bx + 8, by + 8, 28, 28, 1.f);
    }

    /// 1 タイル（SpriteSelectorItem: 80×80、選択=2px 紫 + 4px リング +
    /// 紫ネームストリップ、左上に番号、右上に削除バッジ）。
    void drawAssetTile(float x, float y, int idx, bool sel,
                       const std::u16string& name,
                       std::function<void(float, float, float, float)> thumb,
                       const std::string& details = "") {
        rrFill(r, x, y, 80, 80, 8, sel ? kPanel : Color{0xD9, 0xE3, 0xF2, 0xFF});
        if (sel) {
            rrOutline(r, x, y, 80, 80, 8, kLooksSecondary, 2);
            rrOutline(r, x - 2, y - 2, 84, 84, 10,
                      Color{0x85, 0x5C, 0xD6, 0x59}, 2);
        } else {
            rrOutline(r, x, y, 80, 80, 8, kBorder, 1);
        }
        if (thumb) thumb(x + 8, y + 8, 64, 48);
        // ネームストリップ
        std::string nm = utf16ToUtf8(name);
        rrFill(r, x, y + 60, 80, 20, 4,
               sel ? kLooksSecondary : Color{0xD9, 0xE3, 0xF2, 0xFF});
        fill(r, x, y + 60, 80, 8,
             sel ? kLooksSecondary : Color{0xD9, 0xE3, 0xF2, 0xFF});
        int tw2 = textW(nm, 10, true);
        if (!details.empty()) {   // 詳細があるとき名前を上寄せ
            drawText(x + (80 - tw2) / 2, y + 61, nm, 10,
                     sel ? Color{255, 255, 255, 255} : kFg, true);
            int dw = textW(details, 8);
            drawText(x + (80 - dw) / 2, y + 71, details, 8,
                     sel ? Color{255, 255, 255, 255} : kFg);
        } else
            drawText(x + (80 - tw2) / 2, y + 65, nm, 10,
                     sel ? Color{255, 255, 255, 255} : kFg, true);
        // 番号（左上）
        drawText(x + 3, y + 2, std::to_string(idx + 1), 10, kFg, true);
        // 削除バッジ（選択時）
        if (sel) {
            assetDelRect_ = {x + 80 - 14, y - 14, 28, 28};
            float cx = x + 80 - 14 + 14, cy = y - 14 + 14;
            discFill(r, cx, cy, 14, kLooksSecondary);
            line(r, cx - 4, cy - 4, cx + 4, cy + 4,
                 Color{255, 255, 255, 255}, 2);
            line(r, cx - 4, cy + 4, cx + 4, cy - 4,
                 Color{255, 255, 255, 255}, 2);
        }
    }

    void drawCostumes() {
        Target* t = selected();
        // wrapper: 白 + 1px 境界、右側だけ 8px 丸
        rrFill(r, ui.body.x, ui.body.y, ui.body.w, ui.body.h, 8, kPanel);
        assetTiles_.clear();
        assetDelRect_ = {};
        soundPlayRect_ = {};
        const std::vector<Costume>* cs = t ? t->getCostumes() : nullptr;
        // セレクタ
        drawAssetSelector(ui.body.x, ui.body.y, ui.body.h);
        float tx = ui.body.x + 35, ty = ui.body.y + 8;
        if (cs) {
            for (size_t i = 0; i < cs->size(); ++i) {
                const Costume& c = (*cs)[i];
                assetTiles_.push_back({{tx, ty, 80, 80}, (int)i});
                const Costume* cp = &c;
                // formatCostumeDetails: ceil(size/bitmapResolution) "W x H"
                std::string det;
                {
                    double lw0 = 0, lh0 = 0;
                    if (SDL_Texture* tt = ren.costumeTexture(cp, &lw0, &lh0)) {
                        (void)tt;
                        double br = c.bitmapResolution > 0
                                        ? c.bitmapResolution : 1;
                        det = std::to_string((int)std::ceil(lw0 / br)) +
                              " x " +
                              std::to_string((int)std::ceil(lh0 / br));
                    }
                }
                // sprite-selector-item.css: .sprite-image は最大 32px
                drawAssetTile(tx, ty, (int)i, (int)i == t->currentCostume,
                              c.name,
                              [&](float ix, float iy, float iw, float ih) {
                        (void)iw; (void)ih;
                        double lw = 0, lh = 0;
                        if (SDL_Texture* tex =
                                ren.costumeTexture(cp, &lw, &lh)) {
                            float sc = std::min(32.f / (float)lw,
                                                32.f / (float)lh);
                            blitTex(tex,
                                    ix + 32 - (float)lw * sc / 2,
                                    iy + 26 - (float)lh * sc / 2 + 8,
                                    (float)lw * sc, (float)lh * sc);
                        }
                    }, det);
                ty += 88;
            }
        }
        drawAssetNewButtons(ui.body.x, ui.body.h);
        // 詳細エリア（paint-editor.jsx 準拠）—
        // 上段: 名前 pill + undo/redo + group/ungroup + send + flip +
        // copy/paste/delete、下段: mode-selector 列 + キャンバス +
        // canvasControls（Convert ボタン + zoom ボタングループ）。
        float dx = ui.body.x + 150;
        line(r, dx, ui.body.y, dx, ui.body.y + ui.body.h, kBorder, 1);
        if (cs && !cs->empty()) {
            const Costume& sel = (*cs)[std::clamp(
                t->currentCostume, 0, (int)cs->size() - 1)];
            float ex = dx + 12, ey = ui.body.y + 12;
            // fixed-tools.jsx row1:
            //   [Label "Costume" + 名前入力 8rem] [undo/redo button-group]
            //   |破線| [Group/Ungroup] |破線| [Forward/Backward]
            //   [Front/Back]（全て LabeledIconButton: icon 24px + 10px
            //   ラベル下、min-width 48px、padding 4/5）
            // 名前ラベル + 入力
            drawText(ex, ey + 11, "Costume", 10, kFg, true);
            ex += textW("Costume", 10, true) + 8;
            nmNameRect_ = {ex, ey, 128, 32};
            {
                bool foc = nmEdit_.active && nmEdit_.which == 6;
                // 実 Input: focus = 2px $motion-primary
                rrOutline(r, ex, ey, 128, 32, 16,
                          foc ? Color{0x4C, 0x97, 0xFF, 0xFF} : kBorder,
                          foc ? 2.f : 1.f);
                std::string nm =
                    foc ? nmEdit_.buf : utf16ToUtf8(sel.name);
                float tx = ex + 64 - textW(nm, 10, true) / 2;
                if (foc) tx = ex + 8;   // input は左寄せ padding 8
                drawText(tx, ey + 10, nm, 10, kFg, true);
                if (foc) {
                    float cx2 = tx + textW(nm, 10, true) + 2;
                    fill(r, cx2, ey + 8, 1, 16, kFg);
                }
            }
            ex += 128 + 16;
            // undo/redo（border 1px #D9D9D9、padding .35rem、icon 20px、
            // 連結ボタン 32px 高）
            {
                rrOutline(r, ex, ey, 32, 32, 4, kBorder, 1);
                bl::drawBlockIcon(r, "pe_undo", ex + 6, ey + 6, 20, 20,
                                  1.f);
                ex += 31;
                rrOutline(r, ex, ey, 32, 32, 4, kBorder, 1);
                bl::drawBlockIcon(r, "pe_redo", ex + 6, ey + 6, 20, 20,
                                  1.f);
                ex += 32 + 16;
            }
            // LabeledIconButton 群（icon 24 + ラベル 10px 下、破線区切り）
            auto dashedSep = [&](float sx) {
                for (float yy = ey - 4; yy < ey + 48; yy += 6)
                    line(r, sx, yy, sx, yy + 3, Color{0xD9, 0xD9, 0xD9, 255}, 1);
            };
            auto iconBtn = [&](float bx, const char* icon,
                               const char* label, bool disabled) {
                float lw = std::max(48.f, (float)textW(label, 10) + 10.f);
                bl::drawBlockIcon(r, icon, bx + lw / 2 - 12, ey + 2,
                                  24, 24, disabled ? 0.3f : 1.f);
                drawText(bx + lw / 2 - textW(label, 10) / 2, ey + 30,
                         label, 10,
                         disabled ? Color{0x57, 0x5E, 0x75, 0x70} : kFg);
                return bx + lw;
            };
            ex += 0;   // [Group/Ungroup]（mod-dashed-border）
            ex = iconBtn(ex, "pe_group", "Group", true);
            ex = iconBtn(ex, "pe_ungroup", "Ungroup", true);
            dashedSep(ex + 16);   // グループ右端の破線 + padding-right 16
            ex += 32;
            ex = iconBtn(ex, "pe_send_forward", "Forward", true);
            ex = iconBtn(ex, "pe_send_backward", "Backward", true);
            dashedSep(ex + 16);
            ex += 32;
            ex = iconBtn(ex, "pe_send_front", "Front", true);
            iconBtn(ex, "pe_send_back", "Back", true);
            // 2行目（mod-labeled-icon-height 2.85rem≈46px）:
            // 左の破線区切りグループに Fill/Stroke（Label 10px bold +
            // ColorButton 48×32: 色スウォッチ 32px + ▾ 16px）と
            // ストローク幅の丸入力（input-small-range 64×32）、
            // 右の ModeTools（mod-mode-tools margin-left 16）
            {
                float fy = ey + 56;   // row1 の LabeledIconButton 高(≈46)+8
                auto colorBtn = [&](float bx, const char* label,
                                    Color c, bool outline) {
                    drawText(bx, fy + 11, label, 10, kFg, true);
                    bx += textW(label, 10, true) + 16;
                    (label[0] == 'F' ? cpFillRect_ : cpStrokeRect_) =
                        {bx, fy, 48, 32};
                    // .color-button 3rem×2rem、スウォッチ 32 + ▾ 16
                    rrFill(r, bx, fy, 32, 32, 0, c);
                    // 左上側だけ radius 4
                    rrFill(r, bx, fy, 4, 32, 4, c);
                    if (outline) {   // .outline-swatch: 中央に白四角
                        fill(r, bx + 10, fy + 10, 12, 12,
                             Color{255, 255, 255, 255});
                        rrOutline(r, bx + 10, fy + 10, 12, 12, 0,
                                  Color{0, 0, 0, 0x40}, 1);
                    }
                    rrOutline(r, bx, fy, 32, 32, 4, Color{0, 0, 0, 0x40}, 1);
                    // ▾ 部分
                    rrOutline(r, bx + 32, fy, 16, 32, 4,
                              Color{0, 0, 0, 0x40}, 1);
                    // .color-button-arrow: 12px ▾ （グリフ依存を避け手描き）
                    { bl::Path tp; tp.moveTo(bx + 35, fy + 13); tp.lineTo(bx + 45, fy + 13); tp.lineTo(bx + 40, fy + 20); tp.close(); bl::fillPath(r, tp, 1.f, Color{0x57, 0x5E, 0x75, 255}); }
                    return bx + 48;
                };
                float bx = dx + 12;
                bx = colorBtn(bx, "Fill", Color{0x99, 0x66, 0xFF, 255},
                              false);
                bx = colorBtn(bx + 16, "Stroke", Color{0, 0, 0, 255},
                              true);
                // ストローク幅（initialState=4、input-small-range 4rem）
                bx += 16;
                rrOutline(r, bx, fy, 64, 32, 16, Color{0xE9, 0xEE, 0xF2, 255}, 1);
                drawText(bx + 32 - textW("4", 12, true) / 2, fy + 9,
                         "4", 12, kFg, true);
                // グループ右の縦破線（mod-dashed-border、padding-right 16）
                float sepX = bx + 64 + 16;
                for (float yy = fy - 8; yy < fy + 42; yy += 6)
                    line(r, sepX, yy, sepX, yy + 3,
                         Color{0xD9, 0xD9, 0xD9, 255}, 1);
                // ModeTools（SELECT モード = mode-tools.jsx:
                // [Copy/Paste] |破線| [Delete] |破線| [Flip H/Flip V]、
                // 全て LabeledIconButton）
                float mx2 = sepX + 16;
                auto iconBtn2 = [&](const char* icon, const char* label,
                                    bool disabled) {
                    float lw = std::max(
                        48.f, (float)textW(label, 10) + 10.f);
                    bl::drawBlockIcon(r, icon, mx2 + lw / 2 - 12,
                                      fy + 2, 24, 24,
                                      disabled ? 0.3f : 1.f);
                    drawText(mx2 + lw / 2 - textW(label, 10) / 2, fy + 30,
                             label, 10,
                             disabled ? Color{0x57, 0x5E, 0x75, 0x70}
                                      : kFg);
                    mx2 += lw;
                };
                iconBtn2("pe_copy", "Copy", true);
                iconBtn2("pe_paste", "Paste", true);
                mx2 += 16;
                for (float yy = fy - 8; yy < fy + 42; yy += 6)
                    line(r, mx2 - 8, yy, mx2 - 8, yy + 3,
                         Color{0xD9, 0xD9, 0xD9, 255}, 1);
                iconBtn2("pe_delete", "Delete", true);
                mx2 += 16;
                for (float yy = fy - 8; yy < fy + 42; yy += 6)
                    line(r, mx2 - 8, yy, mx2 - 8, yy + 3,
                         Color{0xD9, 0xD9, 0xD9, 255}, 1);
                iconBtn2("pe_flip_h", "Flip Horizontal", true);
                iconBtn2("pe_flip_v", "Flip Vertical", true);
            }
            // 2行の下に破線（editor-container-top padding-bottom 16）
            line(r, dx + 12, ey + 116,
                 ui.body.x + ui.body.w - 12, ey + 116,
                 kBorder, 1);
            // mode-selector（paint-editor.jsx 順、max-width 7.5rem で
            // 2 列 wrap。tool-select-base: img 2rem=32px + padding 4 + margin 4）
            float tx2 = dx + 20, ty2 = ey + 156;
            for (int i = 0; i < 9; ++i)
                paintToolRects_[i] = {tx2 + (i % 2) * 40,
                                      ty2 + (i / 2) * 40, 40, 40};
            const char* tools[9] = {"tool_select", "tool_reshape",
                                    "tool_brush", "tool_eraser",
                                    "tool_fill",  "tool_text",
                                    "tool_line",  "tool_oval",
                                    "tool_rect"};
            for (int i = 0; i < 9; ++i) {
                float ix = tx2 + (i % 2) * 40 + 4;
                float iy = ty2 + (i / 2) * 40 + 4;
                if (paintTool_ == i)
                    rrFill(r, ix - 4, iy - 4, 40, 40, 4, kLooksSecondary);
                bl::drawBlockIcon(r, tools[i], ix, iy, 32, 32, 1.f);
            }
            // キャンバス（白 + 1px #e8edf1 + radius 4）
            float px = dx + 100, py = ey + 140;
            float pw = ui.body.x + ui.body.w - px - 12;
            float ph = ui.body.y + ui.body.h - py - 60;
            if (pw > 40 && ph > 40) {
                rrFill(r, px, py, pw, ph, 4, Color{255, 255, 255, 255});
                rrOutline(r, px, py, pw, ph, 4,
                          Color{0xE8, 0xED, 0xF1, 0xFF}, 1);
                double lw = 0, lh = 0;
                if (SDL_Texture* tex = ren.costumeTexture(&sel, &lw, &lh)) {
                    float sc = std::min((pw - 40) / (float)lw,
                                        (ph - 60) / (float)lh);
                    if (sc > 4.f) sc = 4.f;
                    pushClip({px, py, pw, ph});
                    blitTex(tex, px + pw / 2 - (float)lw * sc / 2,
                            py + ph / 2 - (float)lh * sc / 2,
                            (float)lw * sc, (float)lh * sc);
                    popClip();
                }
            }
            // canvasControls（高さ36、左=Convert ボタン、右=zoom グループ）
            float cy = py + ph + 4;
            bool isSvg = sel.dataFormat == "svg";
            const char* conv = isSvg ? "Convert to Bitmap"
                                     : "Convert to Vector";
            float cw = textW(conv, 10, true) + 40;
            rrFill(r, px, cy, cw, 36, 5, kLooksSecondary);
            bl::drawBlockIcon(r, "pe_bitmap", px + 8, cy + 8, 20, 20, 1.f);
            drawText(px + 32, cy + 12, conv, 10,
                     Color{255, 255, 255, 255}, true);
            float zx = px + pw - 96;
            rrOutline(r, zx, cy, 32, 32, 4, kBorder, 1);
            bl::drawBlockIcon(r, "pe_zoom_out", zx + 6, cy + 6, 20, 20, 1.f);
            rrOutline(r, zx + 31, cy, 32, 32, 0, kBorder, 1);
            bl::drawBlockIcon(r, "pe_zoom_reset", zx + 37, cy + 6, 20, 20,
                              1.f);
            rrOutline(r, zx + 63, cy, 32, 32, 4, kBorder, 1);
            bl::drawBlockIcon(r, "pe_zoom_in", zx + 69, cy + 6, 20, 20, 1.f);
        }
    }

    void drawSounds() {
        Target* t = selected();
        rrFill(r, ui.body.x, ui.body.y, ui.body.w, ui.body.h, 8, kPanel);
        assetTiles_.clear();
        assetDelRect_ = {};
        soundPlayRect_ = {};
        drawAssetSelector(ui.body.x, ui.body.y, ui.body.h);
        float tx = ui.body.x + 35, ty = ui.body.y + 8;
        static const std::vector<Sound> kNoSounds;
        auto& sounds = t && t->sprite ? t->sprite->sounds : kNoSounds;
        int selIdx = std::clamp(soundSel_, 0,
                                (int)std::max<size_t>(sounds.size(), 1) - 1);
        for (size_t i = 0; i < sounds.size(); ++i) {
            assetTiles_.push_back({{tx, ty, 80, 80}, (int)i});
            std::string det;
            {
                uint32_t rate = 0, frames = 0;
                double sec = (sounds[i].rate > 0 && sounds[i].sampleCount > 0)
                                 ? sounds[i].sampleCount / sounds[i].rate
                                 : (wavPcmInfo(sounds[i], rate, frames)
                                        ? (double)frames / rate : 0);
                if (sec > 0) {
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "%.2f", sec);
                    det = buf;
                }
            }
            drawAssetTile(tx, ty, (int)i, (int)i == selIdx,
                          sounds[i].name,
                          [&](float ix, float iy, float iw, float ih) {
                    (void)iw; (void)ih;
                    bl::drawBlockIcon(r, "snd_icon", ix + 16,
                                      iy + 10, 48, 48, 1.f);
                }, det);
            ty += 88;
        }
        drawAssetNewButtons(ui.body.x, ui.body.h);
        // 詳細（sound-editor.css: パディング 16、行構成）
        float dx = ui.body.x + 150;
        line(r, dx, ui.body.y, dx, ui.body.y + ui.body.h, kBorder, 1);
        if (t && t->sprite && !sounds.empty()) {
            const Sound& snd = sounds[std::clamp(
                selIdx, 0, (int)sounds.size() - 1)];
            // sound-editor.jsx row1: inputGroup[名前 Label+Input,
            // button-group undo/redo] | inputGroup[Copy,Paste,Copy to New
            // の tool-button（icon+10px ラベル、幅 60）] | Delete
            // input-group は右側に 16px padding + 1px 破線。
            float ex = dx + 16, ey = ui.body.y + 16;
            std::string nm = utf16ToUtf8(snd.name);
            drawText(ex, ey + 3, "Sound", 10, kFg, true);
            nmNameRect_ = {ex + 46, ey, 160, 32};
            {
                bool foc = nmEdit_.active && nmEdit_.which == 7;
                rrOutline(r, ex + 46, ey, 160, 32, 16,
                          foc ? Color{0x4C, 0x97, 0xFF, 0xFF} : kBorder,
                          foc ? 2.f : 1.f);
                std::string d = foc ? nmEdit_.buf : nm;
                float tx = ex + 46 + 80 - textW(d, 10, true) / 2;
                if (foc) tx = ex + 46 + 8;
                drawText(tx, ey + 10, d, 10, kFg, true);
                if (foc) {
                    float cx2 = tx + textW(d, 10, true) + 2;
                    fill(r, cx2, ey + 8, 1, 16, kFg);
                }
            }
            // button-group margin-left 16（32px×2、連結:
            // 左端/右端だけ radius 4、共有辺は 1px 仕切り線）
            float ux = ex + 46 + 160 + 16;
            rrOutline(r, ux, ey, 64, 32, 4, kBorder, 1);
            line(r, ux + 32, ey, ux + 32, ey + 32, kBorder, 1);
            bl::drawBlockIcon(r, "snd_undo", ux + 6, ey + 6, 20, 20,
                              undoStack.empty() ? 0.25f : 1.f);
            bl::drawBlockIcon(r, "snd_redo", ux + 38, ey + 6, 20, 20,
                              redoStack.empty() ? 0.25f : 1.f);
            ux += 64;
            // グループ間の破線（input-group: padding-right 16）
            auto dashSep = [&](float sx) {
                for (float yy = ey - 6; yy < ey + 44; yy += 6)
                    line(r, sx, yy, sx, yy + 3, kBorder, 1);
            };
            dashSep(ux + 16);
            // tool-button: img 4rem×1.5rem（64×24）+ 10px ラベル下、
            // flex-basis 60px（img は中央配置で はみ出し 2px 両側）
            auto toolBtn = [&](float bx, const char* icon,
                               const char* label, bool dis) {
                float a = dis ? 0.5f : 1.f;
                bl::drawBlockIcon(r, icon, bx + 10, ey + 4, 40, 24, a);
                drawText(bx + 30 - textW(label, 10) / 2, ey + 30, label,
                         10, dis ? Color{0x57, 0x5E, 0x75, 0x80} : kFg);
                return bx + 60;
            };
            float tx3 = ux + 32;   // 2 番目の input-group
            tx3 = toolBtn(tx3, "snd_copy", "Copy", false);
            tx3 = toolBtn(tx3, "snd_paste", "Paste", true);
            tx3 = toolBtn(tx3, "snd_copynew", "Copy to New", false);
            dashSep(tx3 + 16);
            toolBtn(tx3 + 32, "snd_delete", "Delete", true);
            // row2: 波形（waveform-container: hsla(300,53%,60%,.15) +
            // 1px 境界 + radius 5、高さ 160）
            float wy = ey + 56;
            float ww = ui.body.x + ui.body.w - dx - 32;
            float wh = std::min(160.f, ui.body.y + ui.body.h - wy - 96);
            rrFill(r, dx + 16, wy, ww, wh, 5,
                   Color{0xAD, 0x5C, 0xD6, 0x59});
            rrOutline(r, dx + 16, wy, ww, wh, 5, kBorder, 1);
            std::vector<float> env;
            const int NB = 120;
            bool ok = wavEnvelope(snd, env, NB);
            pushClip({dx + 16, wy, ww, wh});
            float bw = ww / NB;
            for (int i = 0; i < NB; ++i) {
                float v = ok ? env[i] : (i % 2 ? 0.05f : 0.15f);
                float bh = std::max(2.f, v * (wh - 16));
                fill(r, dx + 16 + i * bw + bw * 0.2f,
                     wy + wh / 2 - bh / 2, bw * 0.6f, bh,
                     Color{0xAA, 0x4D, 0xC0, 0x66});
            }
            popClip();
            // row3: [round Play ボタン | 破線 | エフェクト9個]
            // （DOM 順は play inputGroup → faster..robot; row-reverse は
            //   RTL 専用で LTR は左寄せ）
            float ry = wy + wh + 16;
            // round-button: 48px 円、border 4px ui-white-dim(75%)
            soundPlayRect_ = {dx + 16 + 4, ry, 56, 56};
            discFill(r, dx + 16 + 28, ry + 28, 28,
                     Color{0xFF, 0xFF, 0xFF, 0xC0});
            discFill(r, dx + 16 + 28, ry + 28, 24, kLooksSecondary);
            bl::drawBlockIcon(r, "snd_play", dx + 16 + 16, ry + 16, 24, 24,
                              1.f);
            for (float yy = ry + 2; yy < ry + 58; yy += 6)
                line(r, dx + 16 + 64, yy, dx + 16 + 64, yy + 3,
                     kBorder, 1);
            // effect-button: icon 20px + 10px ラベル、flex-basis 60px
            const char* fx[9] = {"Faster", "Slower", "Louder", "Softer",
                                 "Mute", "Fade In", "Fade Out",
                                 "Reverse", "Robot"};
            const char* fxi[9] = {"fx_faster", "fx_slower", "fx_louder",
                                  "fx_softer", "fx_mute", "fx_fadein",
                                  "fx_fadeout", "fx_reverse", "fx_robot"};
            float fx0 = dx + 16 + 64 + 16;
            for (int i = 0; i < 9; ++i) {
                // effect-button: 枠無し、icon 20px + 10px ラベル
                if (fx0 + 60 > dx + 16 + ww) { fx0 = dx + 16 + 80;
                    ry += 56; }
                bl::drawBlockIcon(r, fxi[i], fx0 + 20, ry + 4, 20, 20,
                                  1.f);
                drawText(fx0 + 30 - textW(fx[i], 10) / 2, ry + 28,
                         fx[i], 10, kFg);
                fx0 += 60;
            }
        }
    }
    // ---- テキスト編集中の入力ボックス ----------------------------------------
    /// 編集中フィールドの画面上の矩形（shadow は親ブロックの seg を探す）。
    bool editSegRect(FRect& out) {
        Target* t = selected();
        if (!t || !t->blocks || !edit_.active) return false;
        Blocks* blocks = t->blocks;
        Block* eb = blocks->getBlock(edit_.blockId);
        if (!eb) return false;
        const bl::LBlock* plb = nullptr;
        const bl::LSeg* seg = nullptr;
        if (eb->shadow) {
            Block* par = blocks->getBlock(eb->parent);
            if (!par) return false;
            std::string iname;
            for (auto& [nm, bi] : par->inputs.items)
                if (bi.block == eb->id) { iname = nm; break; }
            for (auto& l : wsLayout_.blocks)
                if (l.id == par->id) plb = &l;
            if (plb)
                for (auto& g : plb->segs)
                    if (g.input == iname &&
                        (g.kind == bl::SegKind::NumField ||
                         g.kind == bl::SegKind::StrField ||
                         g.kind == bl::SegKind::Input))
                        seg = &g;
        } else {
            for (auto& l : wsLayout_.blocks)
                if (l.id == eb->id) plb = &l;
            if (plb)
                for (auto& g : plb->segs)
                    if (g.field == edit_.field) seg = &g;
        }
        if (!plb || !seg) return false;
        float s = wsScale();
        out = {panX + (plb->x + seg->x) * s, panY + (plb->y + seg->y) * s,
               seg->w * s, seg->h * s};
        return true;
    }
    void drawTextEdit() {
        float s = wsScale();
        if (edit_.active) {
            FRect fr;
            if (editSegRect(fr)) {
                // 白箱 + 枠 + バッファ + カーソル
                float w = std::max(fr.w, textW(edit_.buf, 11) * s + 12.f);
                rrFill(r, fr.x - 2, fr.y - 2, w + 4, fr.h + 4, fr.h / 2,
                       Color{255, 255, 255, 255});
                rrOutline(r, fr.x - 2, fr.y - 2, w + 4, fr.h + 4, fr.h / 2,
                          kAccent, 2);
                drawText(fr.x + 4, fr.y + fr.h / 2 - 6, edit_.buf, 11, kFg);
                float tw = textW(edit_.buf, 11);
                if ((SDL_GetTicks() / 500) % 2 == 0)
                    fill(r, fr.x + 5 + tw, fr.y + 5, 2, fr.h - 10, kFg);
            }
            return;
        }
        if (cmt_.active) {
            // コメント編集: ブロック右上に入力ボックス
            for (auto& lb : wsLayout_.blocks) {
                if (lb.id != cmt_.blockId) continue;
                float bx = panX + lb.x * s, by = panY + lb.y * s;
                float w = std::max(80.f, textW(cmt_.buf, 11) + 16.f);
                rrFill(r, bx + lb.w * s + 8, by - 8, w, 24, 5,
                       Color{0xFF, 0xF3, 0xB0, 255});
                rrOutline(r, bx + lb.w * s + 8, by - 8, w, 24, 5,
                          Color{0xC9, 0xA0, 0x30, 255}, 2);
                drawText(bx + lb.w * s + 16, by - 2, cmt_.buf, 11,
                         Color{0x33, 0x2B, 0x00, 255});
                float tw = textW(cmt_.buf, 11);
                if ((SDL_GetTicks() / 500) % 2 == 0)
                    fill(r, bx + lb.w * s + 17 + tw, by - 4, 2, 16,
                         Color{0x33, 0x2B, 0x00, 255});
                break;
            }
            return;
        }
    }

    // ---- メニュー / コンテキスト描画 -----------------------------------------
    void drawMenu() {
        if (!menu_.open) return;
        // blocklyDropDownDiv: padding 4、radius 4、1px tertiary 枠、
        // 背景=ブロック primary（field_dropdown.js → setColour）。
        // item は goog-menuitem（bold 白 13px、padding-left 28、
        // min-height 24）、hover=rgba(0,0,0,.2)、checked は左に白 ✓。
        float mw = 0;
        for (auto& it : menu_.items)
            mw = std::max(mw, (float)textW(it, 13, true));
        menu_.w = (int)mw + 28 + 24;
        menu_.itemH = 28;
        int mh = (int)menu_.items.size() * menu_.itemH + 8;
        rrFill(r, menu_.x + 1, menu_.y + 3, (float)menu_.w, (float)mh, 4,
               Color{0, 0, 0, 50});
        rrFill(r, menu_.x, menu_.y, (float)menu_.w, (float)mh, 4, menu_.bg);
        rrOutline(r, menu_.x, menu_.y, (float)menu_.w, (float)mh, 4,
                  menu_.border, 1);
        for (int i = 0; i < (int)menu_.items.size(); ++i) {
            float iy = menu_.y + 4 + i * menu_.itemH;
            if (i == menu_.hover)
                fill(r, menu_.x + 1, iy, menu_.w - 2, (float)menu_.itemH,
                     Color{0, 0, 0, 51});
            if (i == menu_.checked) {
                Path chk;
                chk.moveTo(menu_.x + 8, iy + 15);
                chk.lineTo(menu_.x + 13, iy + 20);
                chk.lineTo(menu_.x + 22, iy + 9);
                strokePath(r, chk, 1.f, Color{255, 255, 255, 255}, 2.f, false);
            }
            drawText(menu_.x + 28, iy + 7, menu_.items[i], 13,
                     Color{255, 255, 255, 255}, true);
        }
    }

    void drawCtx() {
        if (!ctx_.open) return;
        // 2 系統のメニュー:
        // ・guiMenu（sprite-selector / asset-panel）= context-menu.css:
        //   白・padding 5px 0・1px rgba(0,0,0,.15)・radius 4・
        //   shadow 0 0 5px 1px rgba(0,0,0,.15)。item 25px・13px
        //   $text-primary・hover $looks-secondary 白・danger #FF8C1A。
        // ・ワークスペース = goog-menu（css.js）: 白・
        //   border 1px solid（上左 #ccc、下右 #666）・padding 4px 0・
        //   radius 4（.blocklyContextMenu）・影無し。item padding 4px、
        //   noicon 時 padding-left 12px・hover #d6e9f8・disabled #ccc。
        bool guiMenu = ctx_.spriteIdx >= 0 || ctx_.assetKind > 0;
        int w = 0;
        for (auto& it : ctx_.items)
            w = std::max(w, (int)textW(it, 13) + (guiMenu ? 52 : 32));
        ctx_.w = w;
        ctx_.itemH = guiMenu ? 25 : 21;   // gui: height 25 / goog: 4+13+4
        int pad = guiMenu ? 5 : 4;
        int mh = (int)ctx_.items.size() * ctx_.itemH + pad * 2;
        if (guiMenu) {
            // box-shadow 0 0 5px 1px rgba(0,0,0,.15)（2 段リングで近似）
            rrOutline(r, ctx_.x - 1, ctx_.y - 1, (float)ctx_.w + 2,
                      (float)mh + 2, 5, Color{0, 0, 0, 0x18}, 2.f);
            rrOutline(r, ctx_.x - 3, ctx_.y - 3, (float)ctx_.w + 6,
                      (float)mh + 6, 7, Color{0, 0, 0, 0x0A}, 3.f);
            rrFill(r, ctx_.x, ctx_.y, (float)ctx_.w, (float)mh, 4,
                   Color{0xFF, 0xFF, 0xFF, 255});
            rrOutline(r, ctx_.x, ctx_.y, (float)ctx_.w, (float)mh, 4,
                      kBorder, 1);
        } else {
            // goog-menu: 4 辺色違い 1px（上左 #ccc、下右 #666）、radius 4
            rrFill(r, ctx_.x, ctx_.y, (float)ctx_.w, (float)mh, 4,
                   Color{0xFF, 0xFF, 0xFF, 255});
            line(r, ctx_.x + 3, ctx_.y, ctx_.x + ctx_.w - 3, ctx_.y,
                 Color{0xcc, 0xcc, 0xcc, 255}, 1);
            line(r, ctx_.x, ctx_.y + 3, ctx_.x, ctx_.y + mh - 3,
                 Color{0xcc, 0xcc, 0xcc, 255}, 1);
            line(r, ctx_.x + 3, ctx_.y + mh - 1, ctx_.x + ctx_.w - 3,
                 ctx_.y + mh - 1, Color{0x66, 0x66, 0x66, 255}, 1);
            line(r, ctx_.x + ctx_.w - 1, ctx_.y + 3,
                 ctx_.x + ctx_.w - 1, ctx_.y + mh - 3,
                 Color{0x66, 0x66, 0x66, 255}, 1);
        }
        for (int i = 0; i < (int)ctx_.items.size(); ++i) {
            float iy = ctx_.y + pad + i * ctx_.itemH;
            bool en = i >= (int)ctx_.enabled.size() || ctx_.enabled[i];
            bool dgr = i < (int)ctx_.danger.size() && ctx_.danger[i];
            bool hov = i == ctx_.hover && en;
            if (hov) {
                Color hc = guiMenu
                               ? (dgr ? Color{0xFF, 0x8C, 0x1A, 255}
                                      : kLooksSecondary)
                               : Color{0xd6, 0xe9, 0xf8, 255};
                fill(r, ctx_.x, iy, (float)ctx_.w, (float)ctx_.itemH, hc);
            }
            drawText(ctx_.x + 12, iy + (guiMenu ? 6 : 4), ctx_.items[i], 13,
                     !en ? Color{0xcc, 0xcc, 0xcc, 255}
                         : (hov && guiMenu ? Color{255, 255, 255, 255}
                                           : Color{0x57, 0x5E, 0x75, 255}));
        }
    }
    void drawTopMenu() {
        if (!topMenu_.open) return;
        auto items = topMenuList(topMenu_.which);
        bool menubar = topMenu_.which <= 1;   // File/Edit は menu.css（紫）
        int itemH = menubar ? 34 : 30;
        topMenu_.itemH = itemH;
        int mw = menubar ? 186 : 220;
        for (auto& s : items)
            mw = std::max(mw, textW(s, menubar ? 12 : 11, menubar) + 20);
        topMenu_.w = mw;
        int mh = (int)items.size() * itemH + 4;
        if (menubar) {
            // menu.css: $looks-secondary 背景、下 8px 丸・上辺四角、
            // 1px rgba(0,0,0,.15) 境界 + 影、hover は 15% 黒、12px bold 白。
            rrFill(r, topMenu_.x, topMenu_.y, (float)mw, (float)mh, 8,
                   kLooksSecondary);
            fill(r, topMenu_.x, topMenu_.y, (float)mw, 10, kLooksSecondary);
            rrOutline(r, topMenu_.x, topMenu_.y, (float)mw, (float)mh, 4,
                      kBorder, 1);
            for (int i = 0; i < (int)items.size(); ++i) {
                float iy = topMenu_.y + 2 + i * itemH;
                // セクション区切り（menu-section の 1px rgba(0,0,0,.15)）:
                // File は New | Load | Save → Load と Save の前に線。
                // Edit は Restore | Turbo → i==1 のみ。
                if ((topMenu_.which == 0 && (i == 1 || i == 2)) ||
                    (topMenu_.which != 0 && i == 1))
                    line(r, topMenu_.x, iy, topMenu_.x + mw, iy, kMenuHover,
                         1);
                if (i == topMenu_.hover)
                    fill(r, topMenu_.x, iy, (float)mw, (float)itemH,
                         kMenuHover);
                bool dimmed = topMenu_.which == 1 && i == 0;  // Restore deleted
                drawText(topMenu_.x + 10, iy + (itemH - 12) / 2, items[i], 12,
                         Color{255, 255, 255,
                               dimmed ? (uint8_t)150 : (uint8_t)255},
                         true);
            }
        } else {
            // 拡張/スプライト/背景のアクションメニュー（白）
            rrFill(r, topMenu_.x, topMenu_.y, (float)mw, (float)mh, 6,
                   Color{0xFF, 0xFF, 0xFF, 252});
            rrOutline(r, topMenu_.x, topMenu_.y, (float)mw, (float)mh, 6,
                      kBorder, 1);
            for (int i = 0; i < (int)items.size(); ++i) {
                float iy = topMenu_.y + 2 + i * itemH;
                if (i == topMenu_.hover)
                    rrFill(r, topMenu_.x + 2, iy, (float)mw - 4, (float)itemH,
                           4, kSel);
                drawText(topMenu_.x + 12, iy + 8, items[i], 11, kFg);
            }
        }
    }
    int topMenuItemAt(float mx, float my) {
        if (!topMenu_.open) return -1;
        int itemH = topMenu_.which <= 1 ? 34 : 30;
        if (mx < topMenu_.x || mx >= topMenu_.x + topMenu_.w) return -1;
        int rel = (int)(my - (topMenu_.y + 2));
        if (rel < 0) return -1;
        int i = rel / itemH;
        return i < (int)topMenuList(topMenu_.which).size() ? i : -1;
    }
    int ctxItemAt(float mx, float my) {
        if (!ctx_.open) return -1;
        if (mx < ctx_.x || mx >= ctx_.x + ctx_.w) return -1;
        bool guiMenu = ctx_.spriteIdx >= 0 || ctx_.assetKind > 0;
        int rel = (int)(my - (ctx_.y + (guiMenu ? 5 : 4)));
        if (rel < 0) return -1;
        int i = rel / ctx_.itemH;
        return i < (int)ctx_.items.size() ? i : -1;
    }
    /// 「Delete N Blocks」（ワークスペース全削除 — top-level を列挙して remove）
    void deleteAllBlocks() {
        Target* t = selected();
        Blocks* blocks = t ? t->blocks : nullptr;
        if (!blocks) return;
        pushUndo();
        for (const auto& sid : blocks->getScripts()) blocks->deleteBlock(sid);
        blocks->resetCache();
        wsDirty_ = true;
        selBlock.clear();
    }
    /// 「Clean up Blocks」— 全スクリプトを x=0 で縦に整列（scratch-blocks
    /// の cleanup: 左上から chain 高さ + マージンで積む）。
    void cleanUpBlocks() {
        Target* t = selected();
        Blocks* blocks = t ? t->blocks : nullptr;
        if (!blocks) return;
        pushUndo();
        double y = 0;
        for (const auto& sid : blocks->getScripts()) {
            Block* b = blocks->getBlock(sid);
            if (!b) continue;
            b->x = 0;
            b->y = y;
            y += bl::chainHeight(blocks, sid, tr()) + 32;
        }
        wsDirty_ = true;
    }

    void runCtxItem(int i) {
        if (i < 0 || i >= (int)ctx_.items.size()) return;
        if (i < (int)ctx_.enabled.size() && !ctx_.enabled[i]) return;
        std::string item = ctx_.items[i];
        std::string bid = ctx_.blockId;
        int spIdx = ctx_.spriteIdx;
        int aKind = ctx_.assetKind, aIdx = ctx_.assetIdx;
        float wx = ctx_.wx, wy = ctx_.wy;
        ctx_.open = false;
        if (spIdx >= 0) {   // スプライトタイル: duplicate / export / delete
            if (item == "duplicate") duplicateSprite(spIdx);
            else if (item == "export") exportSprite(spIdx);
            else if (item == "delete") deleteSprite(spIdx);
            return;
        }
        if (aKind > 0) {    // アセットタイル: duplicate / export / delete
            if (item == "duplicate") {
                if (aKind == 1) duplicateCostume(aIdx);
                else duplicateSound(aIdx);
            } else if (item == "export") {
                exportAsset(aKind, aIdx);
            } else if (item == "delete") {
                if (aKind == 1) deleteCostumeAt(aIdx);
                else deleteSoundAt(aIdx);
            }
            return;
        }
        if (item == "Duplicate") duplicateBlock(bid);
        else if (item.rfind("Delete", 0) == 0) {
            if (!bid.empty()) deleteBlockChain(bid);
            else deleteAllBlocks();
        }
        else if (item == "Clean up Blocks") cleanUpBlocks();
        else if (item == "Copy to Backpack") backpackAdd(bid);
        else if (item == "Undo") undo();
        else if (item == "Redo") redo();
        else if (item == "Paste") {
            if (!backpack.empty()) backpackPaste(0);
        } else if (item == "Add Comment") {
            startEditingComment(bid);
        } else if (item == "Remove Comment") {
            Target* t = selected();
            if (t && t->blocks) {
                if (Block* b = t->blocks->getBlock(bid)) {
                    pushUndo();
                    b->comment.clear();
                    t->blocks->resetCache();
                }
            }
        }
        (void)wx; (void)wy;
    }

    // ---- 全体描画（専用 draw list へ記録）------------------------------------
    void draw() {
        layoutUi();
        drawMenuBar();
        if (!stageLarge_) {
            drawTabs();
            if (tab == 0) {
                // injectionDiv: 左列は白コンテナ（1px rgba(0,0,0,.15)、右 8px 丸）。
                // 先に外枠を敷いてから toolbox/flyout/ws を重ねる。
                rrFill(r, ui.body.x, ui.body.y, ui.body.w, ui.body.h, 8,
                       kPanel);
                drawToolbox();
                drawFlyout();
                drawWorkspace();
                // コンテナ外枠（右・下辺の 8px 丸角境界）
                rrOutline(r, ui.body.x - 1, ui.body.y, ui.body.w + 1,
                          ui.body.h + 1, 8, kBorder, 1);
            } else if (tab == 1) {
                drawCostumes();
            } else {
                drawSounds();
            }
        }
        drawStageMenu();
        drawStage();
        if (!stageLarge_) {
            drawSpriteInfo();
            drawSpriteSel();
            drawBackpack();
        }
        drawTextEdit();
        drawMenu();
        drawCtx();
        drawTopMenu();
        drawDirectionPicker();
        drawColorPicker();
        drawActionMenu();
        drawMakeBlock();
    }

    // ---- Make a Block ダイアログ（custom-procedures 再現）-------------------
    /// パーツ列を proccode 風にプレビュー描画する。
    void mbDrawPreview(float x, float y, float s) {
        // ピンクのピル: 部分ごとにテキスト / 角丸入力箱 / 六角箱
        Color pink{0xFF, 0x66, 0x80, 255};
        Color pinkD{0xF2, 0x46, 0x64, 255};
        mb_.partRects.clear();
        float cx = x;
        float h = 30 * s;
        for (size_t i = 0; i < mb_.parts.size(); i++) {
            const auto& p = mb_.parts[i];
            float pw;
            if (p.kind == MakeBlockDlg::Text)
                pw = textW(p.text, 11, true) + 14 * s;
            else
                pw = std::max(46.f, textW(p.text, 10) + 16 * s);
            FRect pr{cx, y, pw, h};
            mb_.partRects.push_back(pr);
            bool sel = mb_.selPart == (int)i;
            if (p.kind == MakeBlockDlg::Text) {
                // ラベル: ピンク塗り + 白文字
                rrFill(r, pr.x, pr.y, pr.w, pr.h, h / 2, pink);
                if (sel) rrOutline(r, pr.x, pr.y, pr.w, pr.h, h / 2,
                                   Color{255, 255, 255, 255}, 2);
                drawText(pr.x + 7 * s, pr.y + h / 2 - 5, p.text, 11,
                         Color{255, 255, 255, 255}, true);
            } else if (p.kind == MakeBlockDlg::StrInput) {
                // string/number 入力: 白い角丸（文字はグレー）
                rrFill(r, pr.x, pr.y, pr.w, pr.h, h / 2, pink);
                rrFill(r, pr.x + 3, pr.y + 3, pr.w - 6, pr.h - 6,
                       (h - 6) / 2, Color{255, 255, 255, 255});
                if (sel)
                    rrOutline(r, pr.x, pr.y, pr.w, pr.h, h / 2,
                              Color{255, 255, 255, 255}, 2);
                drawText(pr.x + (pr.w - textW(p.text, 10)) / 2,
                         pr.y + h / 2 - 5, p.text, 10, kDim);
            } else {
                // boolean 入力: 白い六角
                Path hex;
                hex.moveTo(pr.x + 10, pr.y);
                hex.lineTo(pr.x + pr.w - 10, pr.y);
                hex.lineTo(pr.x + pr.w, pr.y + h / 2);
                hex.lineTo(pr.x + pr.w - 10, pr.y + h);
                hex.lineTo(pr.x + 10, pr.y + h);
                hex.lineTo(pr.x, pr.y + h / 2);
                hex.close();
                fillPath(r, hex, 1.f, Color{255, 255, 255, 255});
                if (sel)
                    rrOutline(r, pr.x, pr.y, pr.w, pr.h, h / 2,
                              Color{255, 255, 255, 255}, 2);
                drawText(pr.x + (pr.w - textW(p.text, 10)) / 2,
                         pr.y + h / 2 - 5, p.text, 10, kDim);
            }
            cx += pw + 6 * s;
        }
        (void)pinkD;
    }
    void mbApplyPartEdit() {
        if (mb_.editPart >= 0 && mb_.editPart < (int)mb_.parts.size()) {
            if (!mb_.buf.empty())
                mb_.parts[mb_.editPart].text = mb_.buf;
            else
                mb_.parts.erase(mb_.parts.begin() + mb_.editPart);
        }
        mb_.editPart = -1;
        mb_.buf.clear();
        mb_.selPart = -1;
    }
    /// ダイアログ内クリック。全て消費する。
    void mbClick(float mx, float my) {
        // 部品クリック → 編集開始
        for (int i = 0; i < (int)mb_.partRects.size(); i++)
            if (hit(mb_.partRects[i], mx, my)) {
                mb_.selPart = i;
                mb_.editPart = i;
                mb_.buf = mb_.parts[i].text;
                SDL_StartTextInput();
                return;
            }
        if (hit(mb_.editBox, mx, my)) { SDL_StartTextInput(); return; }
        if (hit(mb_.addStr, mx, my)) {
            mb_.parts.push_back(
                {MakeBlockDlg::StrInput,
                 "num" + std::to_string(mb_.strCount++)});
            return;
        }
        if (hit(mb_.addBool, mx, my)) {
            mb_.parts.push_back(
                {MakeBlockDlg::BoolInput,
                 "bool" + std::to_string(mb_.boolCount++)});
            return;
        }
        if (hit(mb_.addLabel, mx, my)) {
            mb_.parts.push_back({MakeBlockDlg::Text, "label"});
            return;
        }
        if (hit(mb_.warpBox, mx, my)) { mb_.warp = !mb_.warp; return; }
        if (hit(mb_.cancelBtn, mx, my)) {
            mb_.open = false;
            SDL_StopTextInput();
            return;
        }
        if (hit(mb_.okBtn, mx, my)) {
            if (mb_.editPart >= 0) mbApplyPartEdit();
            makeProcedure();
            mb_.open = false;
            SDL_StopTextInput();
            return;
        }
        // それ以外: ダイアログ外クリックはキャンセル
        FRect dlg{W / 2 - 240.f, H / 2 - 160.f, 480, 320};
        if (!hit(dlg, mx, my)) { mb_.open = false; SDL_StopTextInput(); }
    }
    void drawMakeBlock() {
        if (!mb_.open) return;
        // modal.css: overlay は $ui-modal-overlay = hsla(215,100%,65%,.9)
        // （motion-primary の 90% 透過 = ほぼ不透明の明るい青）。
        // custom-procedures.css: content は width 700px・margin 50px auto・
        // border 4px $ui-white-transparent・radius $space。
        fill(r, 0, 0, (float)W, (float)H, Color{0x5C, 0x80, 0xFF, 0xE6});
        float dw = std::min(700.f, (float)W - 40.f);
        float dh = std::min(430.f, (float)H - 100.f);
        FRect dlg{(W - dw) / 2.f, 50.f, dw, dh};
        rrFill(r, dlg.x, dlg.y, dlg.w, dlg.h, 8, Color{255, 255, 255, 255});
        rrOutline(r, dlg.x, dlg.y, dlg.w, dlg.h, 8,
                  Color{255, 255, 255, 0x26}, 4.f);
        // 左カラム（ピンク）: プレビュー
        FRect left{dlg.x, dlg.y, 210, dlg.h};
        rrFill(r, left.x, left.y, left.w, left.h, 8, Color{0xFF, 0x66, 0x80, 255});
        // 左カラムの右角を四角に（上塗り）
        fill(r, left.x + left.w - 8, left.y, 8, left.h,
             Color{0xFF, 0x66, 0x80, 255});
        mbDrawPreview(left.x + 16, left.y + 24, 1.f);
        // 右カラム
        float rx = left.x + left.w + 16;
        float ry = dlg.y + 16;
        drawText(rx, ry, "Make a Block", 14, kFg, true);
        ry += 34;
        // 入力部品の追加ボタン
        auto addBtn = [&](FRect& rr, const char* label, const char* sub) {
            rr = {rx, ry, 128, 42};
            rrFill(r, rr.x, rr.y, rr.w, rr.h, 4, Color{0xE9, 0xF1, 0xFC, 255});
            rrOutline(r, rr.x, rr.y, rr.w, rr.h, 4,
                      Color{0x4C, 0x97, 0xFF, 255}, 1);
            drawText(rr.x + 10, rr.y + 5, label, 10, kFg, true);
            drawText(rr.x + 10, rr.y + 22, sub, 9, kDim);
            ry += 50;
        };
        addBtn(mb_.addStr, "Add an input", "number or text");
        addBtn(mb_.addBool, "Add an input", "boolean");
        addBtn(mb_.addLabel, "Add a label", "");
        ry += 2;
        // 選択部品の編集フィールド
        mb_.editBox = {rx, ry, 200, 24};
        if (mb_.editPart >= 0) {
            rrFill(r, mb_.editBox.x, mb_.editBox.y, mb_.editBox.w,
                   mb_.editBox.h, 3, Color{255, 255, 255, 255});
            rrOutline(r, mb_.editBox.x, mb_.editBox.y, mb_.editBox.w,
                      mb_.editBox.h, 3, kAccent, 1.5f);
            drawText(mb_.editBox.x + 6, mb_.editBox.y + 6, mb_.buf, 11, kFg);
            float cw = textW(mb_.buf, 11) + 6;
            fill(r, mb_.editBox.x + cw + 1, mb_.editBox.y + 5, 1,
                 mb_.editBox.h - 10, kAccent);
        } else {
            rrFill(r, mb_.editBox.x, mb_.editBox.y, mb_.editBox.w,
                   mb_.editBox.h, 3, Color{0xF4, 0xF6, 0xFA, 255});
            drawText(mb_.editBox.x + 6, mb_.editBox.y + 6,
                     mb_.selPart >= 0 ? "click part to edit"
                                      : "(select a part)",
                     10, kDim);
        }
        ry += 36;
        // Run without screen refresh
        mb_.warpBox = {rx, ry, 16, 16};
        rrOutline(r, rx, ry, 16, 16, 3, kDim, 1.5f);
        if (mb_.warp) {
            fill(r, rx + 3, ry + 3, 10, 10, kAccent);
            drawText(rx + 3.5f, ry + 3, "✓", 9, Color{255, 255, 255, 255});
        }
        drawText(rx + 24, ry + 3, "Run without screen refresh", 10, kFg);
        // ボタン行
        float by = dlg.y + dlg.h - 46;
        mb_.cancelBtn = {dlg.x + dlg.w - 200, by, 90, 30};
        mb_.okBtn = {dlg.x + dlg.w - 104, by, 90, 30};
        rrFill(r, mb_.cancelBtn.x, mb_.cancelBtn.y, mb_.cancelBtn.w,
               mb_.cancelBtn.h, 4, Color{0xE9, 0xF1, 0xFC, 255});
        drawText(mb_.cancelBtn.x + 28, mb_.cancelBtn.y + 9, "Cancel", 11,
                 kFg, true);
        rrFill(r, mb_.okBtn.x, mb_.okBtn.y, mb_.okBtn.w, mb_.okBtn.h, 4,
               kAccent);
        drawText(mb_.okBtn.x + 37, mb_.okBtn.y + 9, "OK", 11,
                 Color{255, 255, 255, 255}, true);
    }


    // ---- イベント -----------------------------------------------------------
    void openContextMenu(float mx, float my) {
        ctx_.x = mx; ctx_.y = my; ctx_.hover = -1;
        ctx_.wx = (mx - panX) / wsScale();
        ctx_.wy = (my - panY) / wsScale();
        ctx_.items.clear();
        ctx_.enabled.clear();
        ctx_.danger.clear();
        ctx_.spriteIdx = -1;
        ctx_.assetKind = 0; ctx_.assetIdx = -1;
        Target* t = selected();
        Blocks* blocks = t ? t->blocks : nullptr;
        if (bl::LBlock* lb = wsBlockAt(mx, my)) {
            // scratch-blocks BlockSvg.showContextMenu_:
            // Duplicate / Add|Remove Comment / Delete N Blocks（+ Help 省略）
            ctx_.blockId = lb->id;
            selBlock = lb->id;
            bool hasCmt = lb->block && !lb->block->comment.empty();
            int n = 0;
            if (blocks) {
                for (Block* cur = blocks->getBlock(lb->id); cur;
                     cur = cur->next.empty() ? nullptr
                             : blocks->getBlock(cur->next)) {
                    if (!cur->shadow) ++n;
                }
            }
            ctx_.items.push_back("Duplicate");
            ctx_.items.push_back(hasCmt ? "Remove Comment" : "Add Comment");
            ctx_.items.push_back(n <= 1 ? "Delete Block"
                                       : "Delete " + std::to_string(n) + " Blocks");
            ctx_.items.push_back("Copy to Backpack");
            ctx_.enabled.assign(4, true);
            ctx_.danger.assign(4, false);
        } else {
            // WorkspaceSvg.showContextMenu_:
            // Undo / Redo / Clean up Blocks / Paste(独自) / Delete N Blocks
            ctx_.blockId.clear();
            ctx_.items.push_back("Undo");
            ctx_.enabled.push_back(!undoStack.empty());
            ctx_.items.push_back("Redo");
            ctx_.enabled.push_back(!redoStack.empty());
            ctx_.items.push_back("Clean up Blocks");
            int tops = blocks ? (int)blocks->getScripts().size() : 0;
            ctx_.enabled.push_back(tops > 0);
            if (!backpack.empty()) {
                ctx_.items.push_back("Paste");
                ctx_.enabled.push_back(true);
            }
            int total = 0;
            if (blocks) {
                for (auto& [id, b] : blocks->allBlocks())
                    if (!b.shadow) ++total;
            }
            ctx_.items.push_back(total <= 1 ? "Delete Block"
                                 : "Delete " + std::to_string(total) + " Blocks");
            ctx_.enabled.push_back(total > 0);
        }
        ctx_.open = true;
    }

    /// ワークスペース上のクリック: ブロック選択・フィールド・ドラッグ開始。
    void wsClick(float mx, float my) {
        bl::LBlock* lb = wsBlockAt(mx, my);
        if (!lb) { selBlock.clear(); return; }
        selBlock = lb->id;
        Target* t = selected();
        Blocks* blocks = t ? t->blocks : nullptr;
        if (!blocks) return;
        // フィールド箱のクリック（dropdown/variable/編集）
        float wx = (mx - panX) / wsScale() - lb->x;
        float wy = (my - panY) / wsScale() - lb->y;
        for (auto& g : lb->segs) {
            if (wx < g.x || wx >= g.x + g.w || wy < g.y || wy >= g.y + g.h)
                continue;
            pushUndo();
            bool consumed = false;
            switch (g.kind) {
            case bl::SegKind::Dropdown:
                openDropdownMenu(lb->id, g, mx, my + 8);
                consumed = true;
                break;
            case bl::SegKind::Variable:
                openDropdownMenu(lb->id, g, mx, my + 8);
                consumed = true;
                break;
            case bl::SegKind::NumField:
            case bl::SegKind::StrField:
                startEditingField(lb->block, g);
                consumed = true;
                break;
            default:
                break;
            }
            if (consumed) { popUndo(); return; }  // 編集系は pushUndo を戻す
            break;
        }
        // ハット/ブロッククリックで実行（空白以外はドラッグも可）
        // ここではドラッグ開始の pending のみ立てる
    }

    bool handleEvent(const SDL_Event& ev) {
        switch (ev.type) {
        case SDL_QUIT: return false;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_CLOSE) return false;
            // A release outside the window must not leave the camera stuck.
            if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                scrollDrag_ = 0;
                spaceDown = false;
            }
            break;
        case SDL_MOUSEBUTTONDOWN: {
            float mx = (float)ev.button.x, my = (float)ev.button.y;
            menu_.open = false;
            endPan();   // a fresh press can never leave the camera captured
            if (ev.button.button == SDL_BUTTON_RIGHT) {
                if (hit(ui.ws, mx, my)) { openContextMenu(mx, my); break; }
                int sti = spriteTileAt(mx, my);
                if (sti >= 0) { openSpriteMenu(mx, my, sti); break; }
                if ((tab == 1 || tab == 2)) {
                    int ai = assetTileAt(mx, my);
                    if (ai >= 0) { openAssetMenu(mx, my, tab, ai); break; }
                }
                break;
            }
            if (ev.button.button == SDL_BUTTON_MIDDLE) {
                beginPan(SDL_BUTTON_MIDDLE, mx, my);
                break;
            }
            if (ctx_.open) {
                int i = ctxItemAt(mx, my);
                if (i >= 0) { runCtxItem(i); break; }
                ctx_.open = false;
            }
            // Make a Block ダイアログはモーダル（全クリックを消費）
            if (mb_.open) { mbClick(mx, my); break; }
            // 方向ピッカーポップオーバーのクリック処理
            if (dirPicker_.open) {
                FRect pRect{dirPicker_.x, dirPicker_.y, dirPicker_.w, dirPicker_.h};
                if (hit(pRect, mx, my)) {
                    Target* t = selected();
                    if (t) {
                        if (hit(dirPicker_.btnAll, mx, my)) {
                            t->rotationStyle = "all around";
                        } else if (hit(dirPicker_.btnLr, mx, my)) {
                            t->rotationStyle = "left-right";
                        } else if (hit(dirPicker_.btnNo, mx, my)) {
                            t->rotationStyle = "don't rotate";
                        } else {
                            // ダイアル面クリック・ドラッグ開始
                            float dx = mx - dirPicker_.dialCx;
                            float dy = my - dirPicker_.dialCy;
                            if (std::hypot(dx, dy) <= dirPicker_.dialR + 12.f) {
                                dirPicker_.dragging = true;
                                double angle = std::atan2((double)dy, (double)dx) * (180.0 / M_PI);
                                double deg = std::round(angle + 90.0);
                                if (deg > 180.0) deg -= 360.0;
                                t->direction = deg;
                            }
                        }
                    }
                    break;
                } else {
                    dirPicker_.open = false; // 外クリックで閉じる
                }
            }
            // カラーピッカー（Scratch paint color-picker ポップオーバー）
            if (cp_.open) {
                FRect pr{cp_.x, cp_.y, cp_.w, cp_.h};
                if (hit(pr, mx, my)) {
                    FRect* sre[3] = {&cp_.slH, &cp_.slS, &cp_.slB};
                    for (int i = 0; i < 3; ++i) {
                        if (hit(*sre[i], mx, my)) {
                            cp_.dragging = true;
                            cp_.dragSlider = i;
                            float v = std::clamp(
                                (mx - sre[i]->x) / sre[i]->w * 100.f,
                                0.f, 100.f);
                            (i == 0 ? cp_.hue : i == 1 ? cp_.sat : cp_.bri) = v;
                            break;
                        }
                    }
                    if (!cp_.dragging) {
                        if (cp_.which == 0) {
                            FRect* gr[4] = {&cp_.btnSolid, &cp_.btnHorz,
                                            &cp_.btnVert, &cp_.btnRadial};
                            for (int i = 0; i < 4; ++i)
                                if (hit(*gr[i], mx, my)) cp_.gradient = i;
                            if (cp_.gradient != 0) {
                                if (hit(cp_.sw1, mx, my)) cp_.colorIndex = 0;
                                else if (hit(cp_.sw2, mx, my)) cp_.colorIndex = 1;
                                else if (hit(cp_.btnSwap, mx, my)) {
                                    std::swap(cp_.hue, cp_.hue2);
                                    std::swap(cp_.sat, cp_.sat2);
                                    std::swap(cp_.bri, cp_.bri2);
                                }
                            }
                        }
                    }
                    break;
                }
                cp_.open = false;
            }
            if (tab == 1 &&
                (hit(cpFillRect_, mx, my) ||
                 hit(cpStrokeRect_, mx, my))) {
                bool fill2 = hit(cpFillRect_, mx, my);
                cp_.open = true;
                cp_.which = fill2 ? 0 : 1;
                FRect fr = fill2 ? cpFillRect_ : cpStrokeRect_;
                cp_.x = fr.x + fr.w / 2.f - 116;
                cp_.y = fr.y + fr.h + 8;
                // 既定色: Fill #9966FF → HSV 約 (73,60,100)、
                // Stroke #000000 → (0,0,0)、2 番目の端色は白
                if (fill2) { cp_.hue = 73; cp_.sat = 60; cp_.bri = 100; }
                else       { cp_.hue = 0;  cp_.sat = 0;  cp_.bri = 0;   }
                cp_.hue2 = 0; cp_.sat2 = 0; cp_.bri2 = 100;
                cp_.gradient = 0; cp_.colorIndex = 0;
                break;
            }
            // ActionMenu 展開列のクリック
            if (am_.open) {
                int ai = amItemAt(mx, my);
                am_.open = false;
                if (ai >= 0) {
                    int w = am_.which;
                    if (w == 3) addSprite();
                    else if (w == 4) addBackdrop();
                    else if (w == 6) addCostume();
                    else if (w == 7) addSound();
                    break;
                }
            }
            // メニューバー
            if (topMenu_.open) {
                int i = topMenuItemAt(mx, my);
                if (i >= 0) { runTopMenuItem(i); break; }
                topMenu_.open = false;   // メニュー外クリックで閉じる（以降の処理は続行）
            }
            if (hit(ui.menuBar, mx, my)) {
                if (hit(menuBtnFile, mx, my)) {
                    openTopMenu(0, menuBtnFile.x + 8,
                                ui.menuBar.y + ui.menuBar.h - 2);
                } else if (hit(menuBtnEdit, mx, my)) {
                    openTopMenu(1, menuBtnEdit.x + 8,
                                ui.menuBar.y + ui.menuBar.h - 2);
                } else if (hit(menuBtnGear, mx, my)) {
                    // Settings メニューは言語切替のみ（単一言語のため装飾）
                } else if (hit(menuBtnSave, mx, my)) {
                    bool ok = saveSb3File(*rt, path.c_str());
                    std::fprintf(stderr, "editor: saved %s (%s)\n", path.c_str(),
                                 ok ? "ok" : "FAILED");
                }
                break;
            }
            // タブ
            for (int i = 0; i < 3; i++)
                if (my >= ui.tabs.y && my < ui.tabs.y + ui.tabs.h &&
                    mx >= ui.tabX[i] && mx < ui.tabX[i] + ui.tabW[i]) {
                    if (std::getenv("XYO_DEBUG"))
                        std::fprintf(stderr, "editor: tab=%d\n", i);
                    tab = i;
                    break;
                }
            // ステージメニュー
            if (hit(flagRect, mx, my)) { rt->greenFlag(); break; }
            if (hit(stopRect, mx, my)) { rt->stopAll(); break; }
            if (hit(stageSmallRect_, mx, my)) { stageSmall_ = true; break; }
            if (hit(stageLargeRect_, mx, my)) { stageSmall_ = false; break; }
            if (hit(fullScrRect_, mx, my)) { stageLarge_ = !stageLarge_; break; }
            // ステージ本体 → ren へ転送（座標をステージテクスチャ座標へ）
            if (hit(ui.stage, mx, my)) { renEvent(ev); break; }
            // ステージ選択カード (右下)
            if (hit(ui.stageSel, mx, my)) {
                if (hit(addBackdropRect_, mx, my)) {
                    am_.open = true; am_.which = 4;
                    am_.cx = addBackdropRect_.x + 22;
                    am_.mainY = addBackdropRect_.y;
                    break;
                }
                for (size_t ti = 0; ti < rt->targets.size(); ++ti) {
                    if (rt->targets[ti]->isStage) {
                        selTarget = (int)ti;
                        clearHistory();
                        wsDirty_ = true;
                        flyDirty_ = true;
                        break;
                    }
                }
                break;
            }
            // スプライト選択
            if (hit(ui.spriteSel, mx, my)) {
                if (hit(addSpriteRect_, mx, my)) {
                    am_.open = true; am_.which = 3;
                    am_.cx = addSpriteRect_.x + 22;
                    am_.mainY = addSpriteRect_.y;
                    break;
                }
                if (spriteDelRect_.w > 0 && hit(spriteDelRect_, mx, my)) {
                    deleteSprite(selTarget);
                    spriteDelRect_ = {};
                    break;
                }
                int sti = spriteTileAt(mx, my);
                if (sti >= 0) {
                    selTarget = sti;
                    clearHistory();
                    wsDirty_ = true;
                    flyDirty_ = true;
                    sdr_.pending = true;
                    sdr_.ti = sti;
                    sdr_.sx = sdr_.mx = mx;
                    sdr_.sy = sdr_.my = my;
                    sdr_.insert = -1;
                }
                break;
            }
            // スプライト情報フィールド（ステージ選択時は無効）
            if (hit(ui.spriteInfo, mx, my)) {
                Target* t = selected();
                if (t && !t->isStage) {
                    int which = spFieldAt(mx, my);
                    if (which == 3) {
                        int half = spShowHalf(mx, my);
                        if (half == 0) t->visible = true;
                        else if (half == 1) t->visible = false;
                        else t->visible = !t->visible;
                        break;
                    }
                    if (which == 4) { // Direction: ポップオーバーを開く
                        FRect fr = spFieldRect(4);
                        dirPicker_.open = true;
                        dirPicker_.w = 150;
                        dirPicker_.h = 175;
                        dirPicker_.x = fr.x + fr.w / 2.f - dirPicker_.w / 2.f;
                        dirPicker_.y = fr.y - dirPicker_.h - 8.f;
                    }
                    if (which >= 0) {
                        spEdit_.active = true;
                        spEdit_.which = which;
                        switch (which) {
                        case 0: spEdit_.buf = utf16ToUtf8(t->getName()); break;
                        case 1: spEdit_.buf = fmtNum(t->x); break;
                        case 2: spEdit_.buf = fmtNum(t->y); break;
                        case 4: spEdit_.buf = fmtNum(t->direction); break;
                        case 5: spEdit_.buf = fmtNum(t->size); break;
                        }
                        SDL_StartTextInput();
                    }
                }
                break;
            }
            // バックパック
            if (hit(ui.bpBar, mx, my)) { bpOpen = !bpOpen; break; }
            if (bpOpen && hit(ui.bpPanel, mx, my)) {
                int i = backpackAt(mx, my);
                if (i >= 0) backpackPaste(i);
                break;
            }
            // ズーム/ゴミ箱
            if (hit(ui.zoomPlus, mx, my)) { zoom = std::min(2.5f, zoom * 1.2f); break; }
            if (hit(ui.zoomMinus, mx, my)) { zoom = std::max(0.3f, zoom / 1.2f); break; }
            if (hit(ui.zoomReset, mx, my)) { zoom = 1.0f; break; }
            // Add Extension ボタン（ツールボックス下部）
            if (tab == 0 && hit(addExtRect_, mx, my)) {
                openTopMenu(2, ui.toolbox.x + ui.toolbox.w + 4,
                            ui.toolbox.y + ui.toolbox.h - 90);
                break;
            }
            // カテゴリ列
            for (int i = 0; i < catCount() && i < 16; ++i)
                if (hit(ui.catRow[i], mx, my)) {
                    catSel = i;
                    flyScroll = 0;
                    flyDirty_ = true;
                    break;
                }
            // フライアウト
            if (tab == 0 && hit(ui.flyout, mx, my)) {
                if (FlyItem* fi = flyItemAt(mx, my)) {
                    if (fi->kind == FlyItem::Button) {
                        if (fi->makeBlock) { mb_.reset(); SDL_StartTextInput(); }
                        else addVariable(fi->list);
                        break;
                    }
                    if (fi->kind == FlyItem::VarRow || fi->kind == FlyItem::ListRow) {
                        // チェックボックス部でなければドラッグ開始
                        float cx = ui.flyout.x + 14;
                        if (mx >= cx && mx < cx + 14 &&
                            my >= ui.flyout.y + fi->y * bl::kWsZoom - flyScroll + 10 &&
                            my < ui.flyout.y + fi->y * bl::kWsZoom - flyScroll + 24) {
                            toggleMonitor(fi->varId, fi->text);
                            break;
                        }
                        // Every gesture owns its own connection/chain state.
                        drag = Drag{};
                        // 変数記者としてドラッグ
                        drag.pending = true;
                        drag.fromFlyout = true;
                        drag.flyItem = fi;
                        drag.opcode = fi->list ? "data_listcontents" : "data_variable";
                        drag.initFields.clear();
                        // 変数 field を束縛
                        Target* t = selected();
                        std::string fname = fi->list ? "LIST" : "VAR";
                        if (t) {
                            for (auto& [vid, v] : t->variables)
                                if (vid == fi->varId) {
                                    BlockField f{Value::string(
                                        SStr::fromUtf8(utf16ToUtf8(v.name)))};
                                    f.id = vid;
                                    drag.initFields[fname] = f;
                                }
                        }
                        drag.shape = edit::Shape::Reporter;
                        drag.offX = 0;
                        drag.offY = 0;
                        drag.mx = (mx - panX) / wsScale();
                        drag.my = (my - panY) / wsScale();
                        // テンプレ描画用
                        Blocks tmp(rt);
                        FlyItem tf;
                        makeFlyItem(&tmp, drag.opcode, drag.initFields, tf);
                        drag.tmpl = tf.lb;
                        drag.tmplSt = tf.st;
                        break;
                    }
                    if (fi->kind == FlyItem::BlockT) {
                        drag = Drag{};
                        drag.pending = true;
                        drag.fromFlyout = true;
                        drag.flyItem = fi;
                        drag.opcode = fi->opcode;
                        drag.initFields = fi->initFields;
                        drag.mutation = fi->mutation;
                        drag.argTexts = fi->argTexts;
                        drag.shape = fi->st.shape;
                        drag.tmpl = fi->lb;
                        drag.tmplSt = fi->st;
                        // Preserve the grabbed point when crossing into the workspace.
                        drag.offX = (mx - (ui.flyout.x + 10)) / bl::kWsZoom;
                        drag.offY = (my - (ui.flyout.y + fi->y * bl::kWsZoom -
                                          flyScroll)) / bl::kWsZoom;
                        drag.mx = (mx - panX) / wsScale() - drag.offX;
                        drag.my = (my - panY) / wsScale() - drag.offY;
                        break;
                    }
                }
                break;
            }
            // Costumes/Sounds タブ: セレクタのタイル + ＋ + 削除/Play
            if ((tab == 1 || tab == 2) && hit(ui.body, mx, my)) {
                Target* t = selected();
                if (t && hit(nmNameRect_, mx, my)) {
                    nmEdit_.active = true;
                    nmEdit_.which = (tab == 1 ? 6 : 7);
                    nmEdit_.buf = utf16ToUtf8(
                        tab == 1
                            ? t->sprite->costumes[std::clamp(
                                  t->currentCostume, 0,
                                  (int)t->sprite->costumes.size() - 1)]
                                  .name
                            : t->sprite->sounds[std::clamp(
                                  soundSel_, 0,
                                  (int)t->sprite->sounds.size() - 1)]
                                  .name);
                    SDL_StartTextInput();
                    break;
                }
                if (hit(assetAddBtn_, mx, my)) {
                    am_.open = true;
                    am_.which = (tab == 1 ? 6 : 7);
                    am_.cx = assetAddBtn_.x + 22;
                    am_.mainY = assetAddBtn_.y;
                    break;
                }
                // paint-editor のツール列（選択のみ — 実編集は未実装）
                if (tab == 1)
                    for (int i = 0; i < 9; ++i)
                        if (paintToolRects_[i].w > 0 &&
                            hit(paintToolRects_[i], mx, my)) {
                            paintTool_ = i;
                            break;
                        }
                if (assetDelRect_.w > 0 && hit(assetDelRect_, mx, my)) {
                    if (tab == 1) deleteCostume();
                    else deleteSound();
                    break;
                }
                if (tab == 2 && soundPlayRect_.w > 0 &&
                    hit(soundPlayRect_, mx, my)) {
                    if (t && t->sprite && t->sprite->soundBank) {
                        int i = std::clamp(soundSel_, 0,
                                           (int)t->sprite->sounds.size() - 1);
                        if (i >= 0)
                            t->sprite->soundBank->playSound(
                                t, t->sprite->sounds[i].soundId);
                    }
                    break;
                }
                for (auto& at : assetTiles_)
                    if (hit(at.r, mx, my)) {
                        if (tab == 1) {
                            if (t) t->setCostume((double)at.index);
                        } else {
                            soundSel_ = at.index;
                        }
                        break;
                    }
                break;
            }
            // ワークスペース
            if (tab == 0 && hit(ui.ws, mx, my)) {
                // ズームコントロール（Blockly: scale×1.3 / ÷1.3 / reset）
                if (hit(ui.zoomPlus, mx, my)) {
                    zoom = std::min(3.f, zoom * 1.3f);
                    wsDirty_ = true;
                    break;
                }
                if (hit(ui.zoomMinus, mx, my)) {
                    zoom = std::max(0.2f, zoom / 1.3f);
                    wsDirty_ = true;
                    break;
                }
                if (hit(ui.zoomReset, mx, my)) {
                    // scrollCenter: コンテンツ中心をビュー中央へ
                    zoom = 1.f;
                    float cbx = 0, cby = 0, cbw = 0, cbh = 0;
                    wsContentBounds(cbx, cby, cbw, cbh);
                    if (cbw > 0) {
                        panX = ui.ws.x + ui.ws.w / 2 -
                               (cbx + cbw / 2) * wsScale();
                        panY = ui.ws.y + ui.ws.h / 2 -
                               (cby + cbh / 2) * wsScale();
                    } else {
                        panX = ui.ws.x + ui.ws.w / 2;
                        panY = ui.ws.y + ui.ws.h / 2;
                    }
                    wsDirty_ = true;
                    break;
                }
                // スクロールバー thumb ドラッグ
                FRect hth = wsScrollThumb(0), vth = wsScrollThumb(1);
                if (hth.w > 0 && hit(hth, mx, my)) { scrollDrag_ = 1; break; }
                if (vth.w > 0 && hit(vth, mx, my)) { scrollDrag_ = 2; break; }
                // Space + left drag pans from anywhere, including over blocks.
                if (spaceDown) { beginPan(SDL_BUTTON_LEFT, mx, my); break; }
                if (bl::LBlock* lb = wsBlockAt(mx, my)) {
                    // ドラッグかクリックかは移動量で決める
                    drag = Drag{};
                    drag.pending = true;
                    drag.fromFlyout = false;
                    drag.headId = lb->id;
                    drag.offX = (mx - panX) / wsScale() - lb->x;
                    drag.offY = (my - panY) / wsScale() - lb->y;
                    drag.mx = lb->x;
                    drag.my = lb->y;
                } else {
                    // Empty canvas: plain left drag moves the camera.
                    selBlock.clear();
                    beginPan(SDL_BUTTON_LEFT, mx, my);
                }
                break;
            }
            break;
        }
        case SDL_MOUSEMOTION: {
            float mx = (float)ev.motion.x, my = (float)ev.motion.y;
            if (sdr_.pending || sdr_.active) {
                sdr_.mx = mx; sdr_.my = my;
                if (sdr_.pending &&
                    (std::abs(mx - sdr_.sx) > 4.f ||
                     std::abs(my - sdr_.sy) > 4.f)) {
                    sdr_.pending = false;
                    sdr_.active = true;
                }
                if (sdr_.active)
                    sdr_.insert = spriteInsertIndex(mx, my);
            }
            if (menu_.open) {
                int rel = (int)(my - (menu_.y + 2));
                menu_.hover = (rel >= 0) ? rel / menu_.itemH : -1;
                if (menu_.hover >= (int)menu_.items.size()) menu_.hover = -1;
            }
            if (ctx_.open) ctx_.hover = ctxItemAt(mx, my);
            if (topMenu_.open) topMenu_.hover = topMenuItemAt(mx, my);
            if (am_.open) am_.hover = amItemAt(mx, my);
            if (cp_.open && cp_.dragging) {
                FRect* sre[3] = {&cp_.slH, &cp_.slS, &cp_.slB};
                int i = cp_.dragSlider;
                if (i >= 0) {
                    float v = std::clamp(
                        (mx - sre[i]->x) / sre[i]->w * 100.f, 0.f, 100.f);
                    (i == 0 ? cp_.hue : i == 1 ? cp_.sat : cp_.bri) = v;
                }
                break;
            }
            if (dirPicker_.open && dirPicker_.dragging) {
                Target* t = selected();
                if (t) {
                    float dx = mx - dirPicker_.dialCx;
                    float dy = my - dirPicker_.dialCy;
                    double angle = std::atan2((double)dy, (double)dx) * (180.0 / M_PI);
                    double deg = std::round(angle + 90.0);
                    if (deg > 180.0) deg -= 360.0;
                    t->direction = deg;
                    if (spEdit_.active && spEdit_.which == 4) {
                        spEdit_.buf = fmtNum(deg);
                    }
                }
                break;
            }
            if (scrollDrag_) {
                wsScrollTo(scrollDrag_ == 1 ? 0 : 1,
                           scrollDrag_ == 1 ? mx : my);
                break;
            }
            if (panning) {
                panX = panPX + (mx - panStartX);
                panY = panPY + (my - panStartY);
                if (!panMoved &&
                    (std::fabs(mx - panStartX) > 2.f ||
                     std::fabs(my - panStartY) > 2.f))
                    panMoved = true;
                break;
            }
            if (drag.pending && !drag.active) {
                // 4px 以上の移動でドラッグ確定
                float dx = mx - (panX + drag.mx * wsScale() + drag.offX * wsScale());
                float dy = my - (panY + drag.my * wsScale() + drag.offY * wsScale());
                if (std::hypot(dx, dy) > 4) {
                    pushUndo();
                    drag.snapshotTaken = true;
                    if (!drag.fromFlyout) detachDrag();
                    drag.active = true;
                }
            }
            if (drag.active) {
                drag.mx = (mx - panX) / wsScale() - drag.offX;
                drag.my = (my - panY) / wsScale() - drag.offY;
                updateDropTarget();
            }
            break;
        }
        case SDL_MOUSEBUTTONUP: {
            scrollDrag_ = 0;
            if (sdr_.pending || sdr_.active) {
                // reorder: ドラッグ中のみ（pending は単クリック）
                if (sdr_.active && sdr_.insert >= 0) {
                    int vi = spriteVisIndex(sdr_.ti);
                    int ins = sdr_.insert;
                    if (ins > vi) ins--;   // 自分を抜いた後の index
                    if (ins != vi && rt && sdr_.ti >= 0 &&
                        sdr_.ti < (int)rt->targets.size()) {
                        pushUndo();
                        spriteReorder(sdr_.ti, ins);
                    }
                }
                sdr_.pending = sdr_.active = false;
                sdr_.ti = -1; sdr_.insert = -1;
                break;
            }
            if (panning && (ev.button.button == panButton ||
                            ev.button.button == SDL_BUTTON_MIDDLE)) {
                endPan();
                break;
            }
            if (dirPicker_.dragging) { dirPicker_.dragging = false; }
            if (cp_.dragging) { cp_.dragging = false; }
            float mx = (float)ev.button.x, my = (float)ev.button.y;
            if (hit(ui.stage, mx, my)) renEvent(ev);
            if (drag.pending && !drag.active) {
                // クリック（ドラッグ無し）
                if (drag.fromFlyout) {
                    // flyout クリック: ワークスペース中央へ追加
                    Target* t = selected();
                    if (t && t->blocks) {
                        float wx = (ui.ws.x + ui.ws.w / 2 - panX) / wsScale();
                        float wy = (ui.ws.y + ui.ws.h / 2 - panY) / wsScale();
                        pushUndo();
                        std::string id = instBlock(t->blocks, drag.opcode,
                                                   drag.initFields, wx, wy, true);
                        selBlock = id;
                        t->blocks->resetCache();
                        wsDirty_ = true;
                    }
                } else {
                    // ブロッククリック → フィールド or 実行
                    wsClick(mx, my);
                    // ハット/ブロック本体は実行（フィールドクリックでなかったとき）
                    if (!menu_.open && !edit_.active) {
                        if (bl::LBlock* lb = wsBlockAt(mx, my)) {
                            if (lb->block && lb->block->topLevel)
                                clickRun(lb->id);
                        }
                    }
                }
                drag.pending = false;
            } else if (drag.active) {
                // The final button position can differ from the last motion event.
                drag.mx = (mx - panX) / wsScale() - drag.offX;
                drag.my = (my - panY) / wsScale() - drag.offY;
                updateDropTarget();
                // ドロップ: ツールボックス/フライアウト/ゴミ箱上は削除
                bool del = hit(ui.toolbox, mx, my) || hit(ui.flyout, mx, my) ||
                           hit(ui.trash, mx, my);
                if (del) {
                    if (!drag.fromFlyout) {
                        Target* t = selected();
                        if (t && t->blocks) {
                            blocks_deleteChain(t->blocks, drag.headId);
                        }
                    }
                    drag.snapshotTaken = false;
                    drag.active = false;
                    drag.pending = false;
                    wsDirty_ = true;
                } else {
                    doDrop();
                }
            }
            break;
        }
        case SDL_MOUSEWHEEL: {
            // Prefer the position carried by the wheel event: SDL_GetMouseState
            // can be stale (and under some backends reports window-relative 0).
            float fx = 0, fy = 0;
#if SDL_VERSION_ATLEAST(2, 0, 26)
            fx = (float)ev.wheel.mouseX;
            fy = (float)ev.wheel.mouseY;
#else
            int mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            fx = (float)mx; fy = (float)my;
#endif
            float dx = (float)ev.wheel.x, dy = (float)ev.wheel.y;
            if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { dx = -dx; dy = -dy; }
            if (hit(ui.flyout, fx, fy)) {
                flyScroll = std::max(0.f, flyScroll - dy * 30.f);
            } else if (hit(ui.ws, fx, fy)) {
                SDL_Keymod m = SDL_GetModState();
                if ((m & KMOD_CTRL) && dy != 0) {
                    // Ctrl+wheel zooms about the pointer.
                    float z0 = zoom;
                    zoom = std::clamp(zoom * (dy > 0 ? 1.1f : 0.9f), 0.3f, 2.5f);
                    panX -= (fx - panX) * (zoom / z0 - 1);
                    panY -= (fy - panY) * (zoom / z0 - 1);
                } else if (m & KMOD_SHIFT) {
                    // Shift+wheel pans horizontally, like most canvases.
                    panX += (dy != 0 ? dy : dx) * 30;
                } else {
                    panY += dy * 30;
                    panX += dx * 30;
                }
            }
            break;
        }
        case SDL_TEXTINPUT: {
            if (mb_.open && mb_.editPart >= 0) mb_.buf += ev.text.text;
            else if (edit_.active) edit_.buf += ev.text.text;
            else if (cmt_.active) cmt_.buf += ev.text.text;
            else if (spEdit_.active) spEdit_.buf += ev.text.text;
            else if (nmEdit_.active) nmEdit_.buf += ev.text.text;
            else ren.handleEvent(ev);
            break;
        }
        case SDL_KEYDOWN: {
            SDL_Keymod mod = (SDL_Keymod)ev.key.keysym.mod;
            SDL_Keycode sym = ev.key.keysym.sym;
            if (!edit_.active && !cmt_.active && !spEdit_.active &&
                !nmEdit_.active && !mb_.open && sym == SDLK_SPACE)
                spaceDown = true;
            // Make a Block ダイアログ中のキー
            if (mb_.open) {
                if (sym == SDLK_ESCAPE) {
                    mb_.open = false;
                    SDL_StopTextInput();
                } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                    if (mb_.editPart >= 0) mbApplyPartEdit();
                    else { makeProcedure(); mb_.open = false;
                           SDL_StopTextInput(); }
                } else if (sym == SDLK_BACKSPACE && mb_.editPart >= 0) {
                    while (!mb_.buf.empty() && (mb_.buf.back() & 0x80) &&
                           !(mb_.buf.back() & 0x40))
                        mb_.buf.pop_back();
                    if (!mb_.buf.empty()) mb_.buf.pop_back();
                }
                break;
            }
            if (edit_.active || cmt_.active || spEdit_.active ||
                nmEdit_.active) {
                if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_TAB) {
                    if (edit_.active) applyEdit();
                    else if (cmt_.active) applyCommentEdit();
                    else if (spEdit_.active) applySpriteEdit();
                    else applyNameEdit();
                } else if (sym == SDLK_ESCAPE) {
                    edit_.active = false;
                    cmt_.active = false;
                    spEdit_.active = false;
                    nmEdit_.active = false;
                    SDL_StopTextInput();
                } else if (sym == SDLK_BACKSPACE) {
                    std::string* buf =
                        edit_.active  ? &edit_.buf
                        : cmt_.active ? &cmt_.buf
                        : spEdit_.active ? &spEdit_.buf
                                       : &nmEdit_.buf;
                    while (!buf->empty() && (buf->back() & 0x80) &&
                           !(buf->back() & 0x40))
                        buf->pop_back();
                    if (!buf->empty()) buf->pop_back();
                }
                break;
            }
            // グローバルショートカット
            if ((mod & KMOD_CTRL) && sym == SDLK_z) {
                if (mod & KMOD_SHIFT) redo();
                else undo();
                break;
            }
            if ((mod & KMOD_CTRL) && (sym == SDLK_y)) { redo(); break; }
            if ((mod & KMOD_CTRL) && sym == SDLK_s) {
                bool ok = saveSb3File(*rt, path.c_str());
                std::fprintf(stderr, "editor: saved %s (%s)\n", path.c_str(),
                             ok ? "ok" : "FAILED");
                break;
            }
            if (sym == SDLK_DELETE || sym == SDLK_BACKSPACE) {
                if (!selBlock.empty()) deleteBlockChain(selBlock);
                break;
            }
            if (sym == SDLK_ESCAPE) {
                if (drag.active || drag.pending) cancelDrag();
                else if (menu_.open) menu_.open = false;
                else if (ctx_.open) ctx_.open = false;
                else if (topMenu_.open) topMenu_.open = false;
                else if (stageLarge_) stageLarge_ = false;
                break;
            }
            // sensing_keypressed 等へ転送
            ren.handleEvent(ev);
            break;
        }
        case SDL_KEYUP:
            if (ev.key.keysym.sym == SDLK_SPACE) spaceDown = false;
            ren.handleEvent(ev);
            break;
        default: break;
        }
        return true;
    }

    void applySpriteEdit() {
        Target* t = selected();
        if (t && spEdit_.active) {
            char* end = nullptr;
            double dv = std::strtod(spEdit_.buf.c_str(), &end);
            bool num = end != spEdit_.buf.c_str() && *end == 0;
            switch (spEdit_.which) {
            case 0:
                if (t->sprite) t->sprite->name = utf8ToU16(spEdit_.buf);
                break;
            case 1: if (num) t->x = dv; break;
            case 2: if (num) t->y = dv; break;
            case 4: if (num) t->direction = dv; break;
            case 5: if (num) t->size = dv; break;
            }
        }
        spEdit_.active = false;
        SDL_StopTextInput();
    }
    void applyNameEdit() {
        Target* t = selected();
        if (t && t->sprite && nmEdit_.active) {
            int ci = std::clamp(t->currentCostume, 0,
                                (int)t->sprite->costumes.size() - 1);
            int si = std::clamp(soundSel_, 0,
                                (int)t->sprite->sounds.size() - 1);
            if (nmEdit_.which == 6 && !t->sprite->costumes.empty())
                t->sprite->costumes[ci].name = utf8ToU16(nmEdit_.buf);
            else if (nmEdit_.which == 7 && !t->sprite->sounds.empty())
                t->sprite->sounds[si].name = utf8ToU16(nmEdit_.buf);
        }
        nmEdit_.active = false;
        SDL_StopTextInput();
    }
    /// チェーン削除（ドラッグ中に detach 済みのチェーンを消す補助）。
    void blocks_deleteChain(Blocks* blocks, const std::string& head) {
        if (!blocks || head.empty()) return;
        blocks->deleteBlock(head);
        blocks->resetCache();
    }

    // ---- ブロックツリーダンプ -------------------------------------------------
    void dumpChain(Blocks* blocks, const std::string& start, int depth) {
        std::string id = start;
        while (!id.empty()) {
            Block* b = blocks->getBlock(id);
            if (!b) break;
            std::string ind(depth * 2, ' ');
            std::fprintf(stderr, "%s%s (%s)%s%s\n", ind.c_str(),
                         b->opcode.c_str(), id.c_str(),
                         b->comment.empty() ? "" : "  // ",
                         b->comment.c_str());
            std::string sub = blocks->getBranch(id, 0);
            if (!sub.empty()) {
                std::fprintf(stderr, "%s  [substack]\n", ind.c_str());
                dumpChain(blocks, sub, depth + 2);
            }
            std::string sub2 = blocks->getBranch(id, 2);
            if (!sub2.empty()) {
                std::fprintf(stderr, "%s  [else]\n", ind.c_str());
                dumpChain(blocks, sub2, depth + 2);
            }
            id = b->next;
        }
    }
    void dumpBlockTree() {
        if (!rt) return;
        for (size_t ti = 0; ti < rt->targets.size(); ++ti) {
            Target* t = rt->targets[ti];
            if (!t || !t->blocks) continue;
            Blocks* blocks = t->blocks;
            std::fprintf(stderr,
                         "BLOCKDUMP: [%zu] target=%s scripts=%zu blocks=%zu\n",
                         ti, utf16ToUtf8(t->getName()).c_str(),
                         blocks->getScripts().size(), blocks->size());
            for (auto& sid : blocks->getScripts()) {
                std::fprintf(stderr, "  script %s:\n", sid.c_str());
                dumpChain(blocks, sid, 2);
            }
        }
    }

    // ---- 自己テスト -----------------------------------------------------------
    int runBlockEditSelfTest() {
        int fails = 0;
        auto check = [&](bool cond, const char* what) {
            if (!cond) { ++fails; std::fprintf(stderr, "EDIT-TEST FAIL: %s\n", what); }
        };
        Target* t = selected();
        if (!t || !t->blocks) { std::fprintf(stderr, "EDIT-TEST FAIL: no target\n"); return 1; }
        Blocks* blocks = t->blocks;
        // addBlock の簡易版（selBlock の後ろへ挿入 or トップレベル）
        auto addBlock = [&](const std::string& opcode) -> std::string {
            pushUndo();
            const std::string anchor = selBlock;
            Block b;
            b.id = newBlockId();
            b.opcode = opcode;
            setupBlockDefaults(b, blocks);
            bindVariableField(b, t);
            const std::string nid = b.id;
            const edit::BlockDef* def = edit::findBlock(opcode);
            bool rep = def && (def->shape == edit::Shape::Reporter ||
                               def->shape == edit::Shape::Boolean);
            Block* ab = anchor.empty() || rep ? nullptr : blocks->getBlock(anchor);
            if (ab) {
                const std::string oldNext = ab->next;
                b.topLevel = false;
                blocks->createBlock(b);
                Block* a2 = blocks->getBlock(anchor);
                Block* nb = blocks->getBlock(nid);
                if (a2 && nb) {
                    a2->next = nid;
                    nb->parent = anchor;
                    if (!oldNext.empty()) {
                        nb->next = oldNext;
                        if (Block* on = blocks->getBlock(oldNext)) on->parent = nid;
                    }
                }
                blocks->resetCache();
            } else {
                b.topLevel = true;
                b.x = 10; b.y = 10;
                blocks->createBlock(b);
            }
            selBlock = nid;
            return nid;
        };

        selBlock.clear();
        std::string a = addBlock("event_whenflagclicked");
        std::string b = addBlock("motion_movesteps");
        std::string c = addBlock("looks_say");
        check(!a.empty() && !b.empty() && !c.empty(), "addBlock returned ids");
        check(blocks->getBlock(a)->next == b, "A.next == B");
        check(blocks->getBlock(b)->parent == a, "B.parent == A");
        {
            Block* bb = blocks->getBlock(b);
            const BlockInput* in = bb ? bb->inputs.find("STEPS") : nullptr;
            check(in && !in->block.empty(), "B has STEPS input");
            Block* sh = in ? blocks->getBlock(in->block) : nullptr;
            check(sh && sh->opcode == "math_number", "STEPS shadow is math_number");
            check(sh && bl::fieldStr(sh, "NUM") == "10", "STEPS NUM == 10");
        }
        selBlock = b;
        deleteSelectedBlock();
        check(blocks->getBlock(b) == nullptr, "B deleted");
        check(blocks->getBlock(a)->next == c, "A.next == C after delete");
        selBlock = a;
        deleteSelectedBlock();
        check(blocks->getBlock(c)->topLevel, "C promoted");
        // 入力編集
        {
            selBlock = c;
            Block* cc = blocks->getBlock(c);
            startEditingInputOf(cc);
            check(edit_.active && edit_.field == "TEXT", "editing C TEXT");
            edit_.buf = "Edited!";
            applyEdit();
            const BlockInput* in = cc ? cc->inputs.find("MESSAGE") : nullptr;
            Block* sh = in && !in->block.empty() ? blocks->getBlock(in->block) : nullptr;
            check(sh && bl::fieldStr(sh, "TEXT") == "Edited!", "MESSAGE updated");
        }
        // ドラッグ: before/after/attach
        {
            selBlock.clear();
            std::string d1 = addBlock("event_whenflagclicked");
            std::string d2 = addBlock("motion_movesteps");
            std::string d3 = addBlock("looks_say");
            drag.headId = d3; drag.shape = blockShape(d3);
            detachDrag();
            bl::insertBefore(blocks, d3, drag.tailId, d2);
            blocks->resetCache();
            check(blocks->getBlock(d1)->next == d3 &&
                  blocks->getBlock(d3)->next == d2, "drag before");
            drag.headId = d2; drag.shape = blockShape(d2);
            detachDrag();
            bl::insertAfter(blocks, d2, drag.tailId, d1);
            blocks->resetCache();
            check(blocks->getBlock(d1)->next == d2 &&
                  blocks->getBlock(d2)->next == d3, "drag after");
            std::string rep = addBlock("operator_add");
            drag.headId = rep; drag.shape = blockShape(rep);
            detachDrag();
            bl::attachToValue(blocks, rep, d2, "STEPS");
            blocks->resetCache();
            {
                Block* d2b = blocks->getBlock(d2);
                const BlockInput* in = d2b ? d2b->inputs.find("STEPS") : nullptr;
                check(in && in->block == rep, "attach: STEPS.block == rep");
            }
            drag.headId = rep; drag.shape = blockShape(rep);
            detachDrag();
            cancelDrag();
            blocks->resetCache();
            {
                Block* d2c = blocks->getBlock(d2);
                const BlockInput* in = d2c ? d2c->inputs.find("STEPS") : nullptr;
                check(in && in->block == rep, "cancel: STEPS restored");
            }
            blocks->deleteBlock(d1);
            check(blocks->getBlock(d1) == nullptr, "drag cleanup");
        }
        // substack
        {
            selBlock.clear();
            std::string ifb = addBlock("control_if");
            std::string body = addBlock("motion_movesteps");
            drag.headId = body; drag.shape = blockShape(body);
            detachDrag();
            check(drag.kind == DragKind::Chain, "substack src Chain");
            bl::attachToSubstack(blocks, body, drag.tailId, ifb, 0);
            {
                Block* ifc = blocks->getBlock(ifb);
                const BlockInput* in = ifc ? ifc->inputs.find("SUBSTACK") : nullptr;
                check(in && in->block == body, "SUBSTACK.block == body");
            }
            drag.headId = body; drag.shape = blockShape(body);
            detachDrag();
            check(drag.kind == DragKind::Substack, "substack detach kind");
            cancelDrag();
            {
                Block* ifc = blocks->getBlock(ifb);
                const BlockInput* in = ifc ? ifc->inputs.find("SUBSTACK") : nullptr;
                check(in && in->block == body, "substack restored");
            }
            blocks->deleteBlock(ifb);
        }
        // dropdown サイクル
        {
            selBlock.clear();
            std::string dd = addBlock("sensing_setdragmode");
            Block* bb = blocks->getBlock(dd);
            check(bb && bl::fieldStr(bb, "DRAG_MODE") == "draggable",
                  "dropdown default");
            if (bb) {
                // options から next へ
                const edit::BlockDef* d = edit::findBlock("sensing_setdragmode");
                int arg = -1;
                if (d) for (int i = 0; i < (int)d->args.size(); ++i)
                    if (d->args[i].kind == edit::ArgKind::Dropdown) { arg = i; break; }
                if (arg >= 0) {
                    // advanceField 相当（Dropdown は openDropdownMenu 経由だが
                    // selftest では直接次候補をセット）
                    const auto& a = d->args[arg];
                    std::vector<std::string> vals;
                    for (size_t i = 1; i < a.options.size(); i += 2)
                        vals.push_back(a.options[i]);
                    bb->fields[a.name] = BlockField{
                        Value::string(SStr::fromUtf8("not draggable"))};
                    check(bl::fieldStr(bb, "DRAG_MODE") == "not draggable",
                          "dropdown changed");
                }
            }
            blocks->deleteBlock(dd);
        }
        // undo/redo
        {
            clearHistory();
            selBlock.clear();
            size_t before = blocks->size();
            std::string u1 = addBlock("event_whenflagclicked");
            std::string u2 = addBlock("motion_movesteps");
            size_t after = blocks->size();
            check(after > before, "undo: blocks added");
            undo();
            check(blocks->getBlock(u2) == nullptr, "undo: u2 removed");
            undo();
            check(blocks->getBlock(u1) == nullptr, "undo: u1 removed");
            redo(); redo();
            check(blocks->getBlock(u2) != nullptr, "redo: u2 restored");
            clearHistory();
            selBlock = u1; deleteSelectedBlock();
            selBlock = u2; deleteSelectedBlock();
            clearHistory();
        }
        // snap
        {
            clearHistory();
            selBlock.clear();
            std::string mv = addBlock("motion_movesteps");
            std::string rep = addBlock("operator_add");
            wsDirty_ = true;
            rebuildWs();
            std::set<std::string> ex;
            std::vector<Conn> cands;
            collectConnections(blocks, ex, cands);
            float sockX = -1, sockY = -1, nextX = -1, nextY = -1;
            for (auto& cn : cands) {
                if (cn.blockId == mv && cn.kind == 2 && cn.inputName == "STEPS") {
                    sockX = cn.x; sockY = cn.y;
                }
                if (cn.blockId == mv && cn.kind == 0) { nextX = cn.x; nextY = cn.y; }
            }
            check(sockX >= 0 && nextX >= 0, "conn points found");
            drag.active = true; drag.fromFlyout = false;
            drag.headId = rep; drag.shape = edit::Shape::Reporter;
            drag.mx = sockX; drag.my = sockY - 16;
            updateDropTarget();
            check(drag.valueTarget && drag.valueInput == "STEPS",
                  "snap: reporter -> STEPS");
            doDrop();
            {
                Block* mvb = blocks->getBlock(mv);
                const BlockInput* in = mvb ? mvb->inputs.find("STEPS") : nullptr;
                check(in && in->block == rep, "snap attached");
            }
            // statement snap
            std::string sy = addBlock("looks_say");
            drag.active = true; drag.fromFlyout = false;
            drag.headId = sy; drag.shape = edit::Shape::Stack;
            detachDrag();
            drag.mx = nextX - 32; drag.my = nextY;
            updateDropTarget();
            check(drag.targetId == mv && !drag.before,
                  "snap: stack -> after mv");
            doDrop();
            if (blocks->getBlock(mv)) check(blocks->getBlock(mv)->next == sy,
                                          "mv.next == sy");
            clearHistory();
            if (blocks->getBlock(mv)) blocks->deleteBlock(mv);
            if (blocks->getBlock(sy)) blocks->deleteBlock(sy);
            if (blocks->getBlock(rep)) blocks->deleteBlock(rep);
        }
        // duplicate
        {
            clearHistory();
            selBlock.clear();
            std::string mv = addBlock("motion_movesteps");
            size_t before = blocks->size();
            duplicateBlock(mv);
            check(blocks->size() == before + 2, "dup: +2 blocks");
            std::string d2 = selBlock;
            check(!d2.empty() && d2 != mv, "dup: new sel");
            blocks->deleteBlock(d2);
            blocks->deleteBlock(mv);
            clearHistory();
        }
        // comment
        {
            clearHistory();
            selBlock.clear();
            std::string cb = addBlock("looks_say");
            Block* bb = blocks->getBlock(cb);
            startEditingComment(cb);
            cmt_.buf = "hello-comment-xyz";
            applyCommentEdit();
            bb = blocks->getBlock(cb);
            check(bb && bb->comment == "hello-comment-xyz", "comment stored");
            Sb3SaveResult sr = saveSb3(*rt);
            // 保存は deflate 圧縮されるので、zip バイト列を直接検索せず
            // project.json を展開して確認する。
            std::string blob;
            {
                ZipReader zr;
                std::vector<uint8_t> pj;
                if (sr.ok && ZipReader::open(sr.data.data(), sr.data.size(),
                                             zr)) {
                    if (const ZipEntry* e = zr.find("project.json"))
                        zr.extract(*e, pj);
                }
                blob.assign(pj.begin(), pj.end());
            }
            check(sr.ok && blob.find("hello-comment-xyz") != std::string::npos,
                  "comment serialized");
            blocks->deleteBlock(cb);
            clearHistory();
        }
        // backpack
        {
            clearHistory();
            bool ro0 = backpackReadOnly;
            backpackReadOnly = true;
            auto saved = backpack;
            backpack.clear();
            selBlock.clear();
            std::string mv = addBlock("motion_movesteps");
            size_t before = blocks->size();
            backpackAdd(mv);
            check(backpack.size() == 1, "bp: added");
            backpackPaste(0);
            check(blocks->size() == before + 2, "bp: pasted +2");
            std::string p1 = selBlock;
            check(!p1.empty() && p1 != mv, "bp: new id");
            blocks->deleteBlock(p1);
            blocks->deleteBlock(mv);
            backpack = saved;
            backpackReadOnly = ro0;
            clearHistory();
        }
#include "editor_drag_tests.inc"
        if (fails == 0) std::fprintf(stderr, "EDIT-TEST: all checks passed\n");
        else std::fprintf(stderr, "EDIT-TEST: %d failure(s)\n", fails);
        return fails;
    }
}; // struct Editor

// ---------------------------------------------------------------------------
// entry
// ---------------------------------------------------------------------------
namespace xyo {
static std::vector<uint8_t> loadFile(const char* path, bool& ok) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { ok = false; return {}; }
    ok = true;
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()};
}

int editorMain(const char* path, bool danger) {
    bool ok;
    std::vector<uint8_t> data = loadFile(path, ok);
    if (!ok) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }
    std::unique_ptr<Runtime> rtOwn(new Runtime);
    Sb3LoadResult res = loadSb3File(*rtOwn, data.data(), data.size());
    if (!res.ok) { std::fprintf(stderr, "load failed: %s\n", res.error.c_str()); return 1; }
    if (danger) rtOwn->dangerMode = true;

    Editor ed;
    ed.rt = rtOwn.get();
    ed.path = path;
    ed.buildCategories();
    if (const char* sh = std::getenv("XYO_EDIT_SHOT")) ed.shotPath = sh;
    // scratch-gui は最初のスプライト（ステージでない最初のターゲット）を選択
    {
        for (size_t i = 0; i < ed.rt->targets.size(); ++i)
            if (!ed.rt->targets[i]->isStage) { ed.selTarget = (int)i; break; }
    }
    if (const char* tg = std::getenv("XYO_EDIT_TARGET")) ed.selTarget = std::atoi(tg);
    if (const char* cc = std::getenv("XYO_EDIT_CAT")) {
        if (*cc >= '0' && *cc <= '9') {
            ed.catSel = std::atoi(cc);
        } else {
            for (int i = 0; i < ed.catCount(); ++i)
                if (ed.catAt(i).id == cc) { ed.catSel = i; break; }
        }
    }

    // SDL_main を使わないので明示的に main ready を立てる（Windows で必須）
    SDL_SetMainReady();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0 && !SDL_WasInit(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "editor: SDL video init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    ed.win = SDL_CreateWindow(("xyo — " + std::string(path)).c_str(),
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              1280, 800,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
                                  SDL_WINDOW_ALLOW_HIGHDPI);
    if (!ed.win) { std::fprintf(stderr, "editor: window failed\n"); return 1; }
    ed.r = SDL_CreateRenderer(ed.win, -1, SDL_RENDERER_ACCELERATED |
                                              SDL_RENDERER_PRESENTVSYNC);
    if (!ed.r) ed.r = SDL_CreateRenderer(ed.win, -1, SDL_RENDERER_SOFTWARE);
    if (!ed.r) { std::fprintf(stderr, "editor: renderer failed\n"); return 1; }

    // Dedicated UI renderer: SDL2 + the already-vendored stb_truetype only.
    ui::DrawList canvas;
    ui::Font regular, bold;
#ifdef XYO_HAVE_EMBEDDED_FONT
    if (!regular.init(ed.r, xyoFontRegular, (int)xyoFontRegularSize) ||
        !bold.init(ed.r, xyoFontBold, (int)xyoFontBoldSize)) {
        std::fprintf(stderr, "editor: bundled font init failed\n");
        return 1;
    }
#else
    std::fprintf(stderr, "editor: bundled fonts are required\n");
    return 1;
#endif
    ed.fontReg_ = &regular;
    ed.fontBold_ = &bold;
    SDL_StartTextInput();
    SDL_RendererInfo rendererInfo{};
    SDL_GetRendererInfo(ed.r, &rendererInfo);
    const bool gpu = (rendererInfo.flags & SDL_RENDERER_ACCELERATED) != 0;
    ed.gpuUi_ = gpu;
    if (SDL_GL_GetCurrentContext()) {
        auto getString = reinterpret_cast<decltype(&glGetString)>(
            SDL_GL_GetProcAddress("glGetString"));
        if (getString) {
            const auto* device = getString(GL_RENDERER);
            if (device) std::fprintf(stderr, "editor: GL device: %s\n", device);
        }
    }
    std::fprintf(stderr, "editor: native batched UI / %s / %s\n",
                 rendererInfo.name ? rendererInfo.name : "unknown",
                 gpu ? "GPU" : "software fallback");

    // SdlRenderer を共有レンダラへ埋め込む（ステージはオフスクリーンへ描く）
    if (!ed.ren.initEmbedded(*rtOwn, ed.r)) {
        std::fprintf(stderr, "editor: embedded renderer init failed\n");
        return 1;
    }
    ed.backpackLoad();
    ed.layoutUi();
    ed.rebuildFlyout();
    ed.rebuildWs();

    // 自己テスト（ヘッドレス）
    if (std::getenv("XYO_EDIT_TEST")) {
        int fails = ed.runBlockEditSelfTest();
        return fails == 0 ? 0 : 1;
    }
    if (const char* bpa = std::getenv("XYO_EDIT_BPADD")) {
        int want = std::atoi(bpa);
        Target* t = ed.selected();
        if (t && t->blocks) {
            int added = 0;
            for (const std::string& sid : t->blocks->getScripts()) {
                if (want > 0 && added >= want) break;
                ed.backpackAdd(sid);
                ++added;
            }
        }
        std::fprintf(stderr, "editor: backpack add -> %d item(s)\n",
                     (int)ed.backpack.size());
    }
    if (std::getenv("XYO_EDIT_BPPASTE")) {
        int cnt = (int)ed.backpack.size();
        for (int i = 0; i < cnt; ++i) ed.backpackPaste(i);
        std::fprintf(stderr, "editor: backpack paste -> %d item(s)\n", cnt);
    }
    if (std::getenv("XYO_BLOCKDUMP")) {
        ed.dumpBlockTree();
        return 0;
    }
    // XYO_EDIT_SAVE 単独時はループ前保存して終了するが、INJECT/EXIT_FRAMES
    // 併用時はループ実行後に保存する（SHOT と同じ扱い）。
    const char* savePath = std::getenv("XYO_EDIT_SAVE");
    bool saveOnly = savePath && !std::getenv("XYO_INJECT") &&
                    !std::getenv("XYO_EXIT_FRAMES");
    if (saveOnly) {
        bool ok = saveSb3File(*rtOwn, savePath);
        std::fprintf(stderr, "editor: save %s -> %s\n", path, ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }

    using clock = std::chrono::steady_clock;
    const auto kFrame = std::chrono::microseconds(16667); // 60 Hz UI
    const auto vmFrame = std::chrono::microseconds(33333); // Scratch 30 Hz
    auto nextVmStep = clock::now();
    int exitFrames = 0;
    if (const char* ef = std::getenv("XYO_EXIT_FRAMES")) exitFrames = std::atoi(ef);
    // XYO_INJECT="N:op:args|...": フレーム N の poll 前にイベントを注入する
    //   op: md:x,y / mu:x,y / mm:x,y（ウィンドウ px）、txt:<utf8>、key:<name>
    struct InjEv { long frame; char op[4]; std::string arg; };
    std::vector<InjEv> inject;
    if (const char* spec = std::getenv("XYO_INJECT")) {
        std::string s = spec;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t e2 = s.find('|', pos);
            std::string tok =
                s.substr(pos, e2 == std::string::npos ? std::string::npos
                                                      : e2 - pos);
            pos = e2 == std::string::npos ? s.size() : e2 + 1;
            size_t c1 = tok.find(':');
            size_t c2 = c1 == std::string::npos ? std::string::npos
                                                : tok.find(':', c1 + 1);
            if (c2 == std::string::npos) continue;
            InjEv ie;
            ie.frame = std::atol(tok.c_str());
            std::string op = tok.substr(c1 + 1, c2 - c1 - 1);
            std::snprintf(ie.op, sizeof(ie.op), "%s", op.c_str());
            ie.arg = tok.substr(c2 + 1);
            inject.push_back(ie);
        }
        std::sort(inject.begin(), inject.end(),
                  [](const InjEv& a, const InjEv& b) { return a.frame < b.frame; });
    }
    uint32_t injButtons = 0;
    auto pushInject = [&](const InjEv& ie) -> SDL_Event {
        SDL_Event ev{};
        if (ie.op[0] == 'm' || ie.op[0] == 'r') {
            // "md|mu|mm"=左、"rmd|rmu|rmm"=右（'r' 接頭辞後の第2文字が
            // 操作: d=down, u=up, m=motion）。
            bool right = ie.op[0] == 'r';
            char sub = right ? ie.op[2] : ie.op[1];
            uint32_t mask = right ? SDL_BUTTON_RMASK : SDL_BUTTON_LMASK;
            int x = 0, y = 0;
            std::sscanf(ie.arg.c_str(), "%d,%d", &x, &y);
            if (sub == 'm' || sub == 0) {
                ev.type = SDL_MOUSEMOTION;
                ev.motion.x = x; ev.motion.y = y;
                ev.motion.state = injButtons;
            } else {
                if (sub == 'd') injButtons |= mask;
                else injButtons &= ~mask;
                ev.type = sub == 'd' ? SDL_MOUSEBUTTONDOWN
                                     : SDL_MOUSEBUTTONUP;
                ev.button.button =
                    right ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
                ev.button.x = x; ev.button.y = y;
            }
        } else if (ie.op[0] == 't' && ie.op[1] == 'x') {
            ev.type = SDL_TEXTINPUT;
            std::strncpy(ev.text.text, ie.arg.c_str(),
                         sizeof(ev.text.text) - 1);
        } else if (ie.op[0] == 'k') {
            ev.type = SDL_KEYDOWN;
            const char* n = ie.arg.c_str();
            SDL_Keycode sym = SDLK_UNKNOWN;
            if (!std::strcmp(n, "enter") || !std::strcmp(n, "return"))
                sym = SDLK_RETURN;
            else if (!std::strcmp(n, "backspace")) sym = SDLK_BACKSPACE;
            else if (!std::strcmp(n, "escape")) sym = SDLK_ESCAPE;
            else if (!std::strcmp(n, "space")) sym = SDLK_SPACE;
            else if (!std::strcmp(n, "delete")) sym = SDLK_DELETE;
            else if (std::strlen(n) == 1) sym = (SDL_Keycode)n[0];
            ev.key.keysym.sym = sym;
        }
        // SDL_PushEvent は使わない — SDL2-compat/SDL3 層が TEXTINPUT の
        // 配列バッファをポインタ扱いして SIGSEGV するため。エディタの
        // ディスパッチャへ直接渡す（キューの先頭扱い）。
        return ev;
    };
    size_t injIdx = 0;
    int frameCount = 0;
    while (!ed.quit) {
        if (exitFrames > 0 && frameCount >= exitFrames) break;
        ++frameCount;
        auto t0 = clock::now();
        while (injIdx < inject.size() && inject[injIdx].frame <= frameCount) {
            SDL_Event iev = pushInject(inject[injIdx++]);
            if (iev.type != 0) {
                if (!ed.handleEvent(iev)) { ed.quit = true; break; }
            }
        }
        if (ed.quit) break;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            // TEXTINPUT は注入イベントでも来るが、エディタは自前で消費する。
            if (!ed.handleEvent(ev)) { ed.quit = true; break; }
        }
        if (ed.quit) break;
        if (ed.lastSel != ed.selTarget) {
            ed.lastSel = ed.selTarget;
            ed.clearHistory();
        }
        // File → New（scratch-gui の default-project と同一の最小プロジェクト）
        if (ed.newRequested_) {
            ed.newRequested_ = false;
#ifdef XYO_HAVE_DEFAULT_PROJECT
            std::unique_ptr<Runtime> fresh(new Runtime);
            if (loadSb3File(*fresh, xyoDefaultProject, xyoDefaultProjectSize)
                    .ok) {
                if (danger) fresh->dangerMode = true;
                ed.rt = nullptr;
                ed.ren.~SdlRenderer();
                rtOwn = std::move(fresh);
                new (&ed.ren) SdlRenderer();
                if (ed.ren.initEmbedded(*rtOwn, ed.r)) {
                    ed.rt = rtOwn.get();
                    ed.selTarget = 0;
                    ed.lastSel = 0;
                    ed.clearHistory();
                    ed.buildCategories();
                    ed.drag = Editor::Drag{};
                    ed.wsDirty_ = true;
                    ed.flyDirty_ = true;
                    for (size_t i = 0; i < ed.rt->targets.size(); ++i)
                        if (!ed.rt->targets[i]->isStage) {
                            ed.selTarget = (int)i;
                            break;
                        }
                } else {
                    ed.rt = rtOwn.get();
                }
            }
#else
            std::fprintf(stderr, "editor: default project not embedded\n");
#endif
        }
        // File → Load from your computer（同じ sb3 を読み直して未保存変更を捨てる）
        if (ed.reloadRequested_) {
            ed.reloadRequested_ = false;
            std::unique_ptr<Runtime> fresh(new Runtime);
            if (loadSb3File(*fresh, data.data(), data.size()).ok) {
                if (danger) fresh->dangerMode = true;
                ed.rt = nullptr;
                ed.ren.~SdlRenderer();
                rtOwn = std::move(fresh);
                new (&ed.ren) SdlRenderer();
                if (ed.ren.initEmbedded(*rtOwn, ed.r)) {
                    ed.rt = rtOwn.get();
                    ed.selTarget = 0;
                    ed.lastSel = 0;
                    ed.clearHistory();
                    ed.buildCategories();
                    ed.drag = Editor::Drag{};
                    ed.wsDirty_ = true;
                    ed.flyDirty_ = true;
                    for (size_t i = 0; i < ed.rt->targets.size(); ++i)
                        if (!ed.rt->targets[i]->isStage) {
                            ed.selTarget = (int)i;
                            break;
                        }
                } else {
                    ed.rt = rtOwn.get();
                }
            } else {
                std::fprintf(stderr, "editor: reload failed\n");
            }
        }
        // Keep Scratch simulation at 30 Hz independently of the responsive UI.
        // Frame-limited automation keeps the historical one-step-per-frame rule.
        auto vmNow = clock::now();
        if (exitFrames > 0 || vmNow >= nextVmStep) {
            ed.rt->step();
            nextVmStep = vmNow + vmFrame;
        }
        // Reuse vertex/index storage; commands merge by texture and clip.
        int logicalW = 0, logicalH = 0, pixelW = 0, pixelH = 0;
        SDL_GetWindowSize(ed.win, &logicalW, &logicalH);
        SDL_GetRendererOutputSize(ed.r, &pixelW, &pixelH);
        SDL_RenderSetScale(ed.r,
            logicalW > 0 ? float(pixelW) / logicalW : 1.f,
            logicalH > 0 ? float(pixelH) / logicalH : 1.f);
        canvas.begin(float(logicalW), float(logicalH));
        bl::setDrawList(&canvas);
        // ed.draw() の内部で SDL 直接描画（全画面ステージの ren.draw()）が
        // あるため、クリアは記録前に済ませる（後だと直接描画が消える）。
        // .stage-wrapper.full-screen の背景は $ui-white。
        Color bgc = ed.stageLarge_ ? Color{255, 255, 255, 255} : kBody;
        SDL_SetRenderDrawColor(ed.r, bgc.r, bgc.g, bgc.b, 255);
        SDL_RenderClear(ed.r);
        ed.draw();
        if (!canvas.render(ed.r) || regular.HasError() || bold.HasError()) {
            std::fprintf(stderr, "editor: UI render failed: %s\n", SDL_GetError());
            ed.quit = true;
        }
        if (std::getenv("XYO_EDIT_STATS") && frameCount == 1) {
            const auto st = canvas.stats();
            std::fprintf(stderr, "editor: vertices=%zu indices=%zu batches=%zu\n",
                         size_t(st.vertices), size_t(st.indices), size_t(st.batches));
        }
        // スクリーンショット — 最終フレーム（XYO_EXIT_FRAMES 指定時）に取る
        bool shotNow = exitFrames > 0 ? frameCount >= exitFrames
                                      : frameCount >= 1;
        if (!ed.shotPath.empty() && !ed.shotDone && shotNow) {
            int w = 0, h = 0;
            SDL_GetWindowSize(ed.win, &w, &h);
            if (SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(
                    0, w, h, 24, SDL_PIXELFORMAT_RGB24)) {
                if (SDL_RenderReadPixels(ed.r, nullptr,
                                         SDL_PIXELFORMAT_RGB24,
                                         surf->pixels, surf->pitch) == 0) {
                    FILE* f = std::fopen(ed.shotPath.c_str(), "wb");
                    if (f) {
                        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
                        std::fwrite(surf->pixels, 1, surf->h * surf->pitch, f);
                        std::fclose(f);
                        ed.shotDone = true;
                    }
                }
                SDL_FreeSurface(surf);
            }
        }
        SDL_RenderPresent(ed.r);
        auto dt = clock::now() - t0;
        if (dt < kFrame) std::this_thread::sleep_for(kFrame - dt);
    }
    if (savePath && !saveOnly) {
        bool ok = saveSb3File(*rtOwn, savePath);
        std::fprintf(stderr, "editor: save %s -> %s\n", path, ok ? "ok" : "FAILED");
    }
    if (ed.stageTex_) SDL_DestroyTexture(ed.stageTex_);
    bl::setDrawList(nullptr);
    SDL_StopTextInput();
    bl::releaseIcons();
    if (ed.panCursor) { SDL_FreeCursor(ed.panCursor); ed.panCursor = nullptr; }
    regular.reset();
    bold.reset();
    // Release embedded renderer textures while their SDL renderer is alive.
    ed.ren.~SdlRenderer();
    new (&ed.ren) SdlRenderer();
    SDL_DestroyRenderer(ed.r);
    SDL_DestroyWindow(ed.win);
    return 0;
}
} // namespace xyo

#else
namespace xyo {
int editorMain(const char* path, bool) {
    std::fprintf(stderr, "editor: %s (this build has no SDL2)\n", path);
    return 1;
}
} // namespace xyo
#endif
