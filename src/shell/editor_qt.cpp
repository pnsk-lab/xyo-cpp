// xyo-cpp: ビルトインエディタ（Qt Quick 版）。
//
// scratch-gui 準拠の単一ウィンドウ構成を Qt Quick で実装する。
//  - Editor.qml: メニューバー / タブ / ツールボックス / 右列の chrome
//  - BlockCanvas (QQuickPaintedItem): ワークスペース + フライアウトの
//    ブロック描画（blockly::Canvas を QPainter にマップする QpCanvas 経由）
//  - StageItem (QQuickPaintedItem): SdlRenderer を headless SDL_Renderer で
//    走らせ SDL_RenderReadPixels → QImage で表示する
//  - EditorController: Runtime/Blocks 操作・ドラッグ・undo・保存を保持する
//    コンテキストオブジェクト（QML から直接触る）
//
// ブロック形状・レイアウト・ドロップ判定は SDL 版と同じ src/blockly の
// コードを共有する（TextRenderer 関数ポインタに QFontMetrics/QPainter を束縛）。
#ifdef XYO_HAVE_QT

#include <QGuiApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QMap>
#include <QPainter>
#include <QPointer>
#include <QPolygonF>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSettings>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickView>
#include <QTimer>
#include <QVariant>
#include <QtQml/qqml.h>

// Qt は `slots` を Q_SLOTS にするマクロを定義する。xyo ヘッダに入る前に外す。
#undef slots

#include <SDL.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "blockly/catalog.hpp"
#include "blockly/dynamic_catalog.hpp"
#include "blockly/geom.hpp"
#include "blockly/label.hpp"
#include "blockly/layout.hpp"
#include "blockly/ops.hpp"
#include "blockly/shape.hpp"
#include "core/cast.hpp"
#include "core/fileio.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "extensions/manifest.hpp"
#include "load/sb3.hpp"
#include "load/sb3_save.hpp"
#include "model/blocks.hpp"
#include "model/target.hpp"
#include "render/sdl_renderer.hpp"
#include "shell/editor.hpp"
#include "shell/theme.hpp"
#include "ui/draw.hpp"

#ifdef XYO_HAVE_EMBEDDED_FONT
#include "xyo_fonts.inc"
#endif

using namespace xyo;
using xyo::blockly::Color;
using xyo::blockly::Path;
using namespace xyo::theme;
namespace bl = xyo::blockly;

namespace {

static std::string utf16ToUtf8(const std::u16string& s) {
    return SStr::toUtf8(s);
}
static QString qstr(const std::string& s) {
    return QString::fromUtf8(s.data(), (qsizetype)s.size());
}
static bool hit(float x, float y, float w, float h, float px, float py) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

// ---------------------------------------------------------------------------
// QpCanvas — blockly::Canvas を QPainter へマップする描画先
// ---------------------------------------------------------------------------
struct QtTextCtx {
    QPainter* p = nullptr;
    // フォント: 埋め込み Liberation Sans（無ければシステム sans）
    QString family = "Liberation Sans";
    mutable std::map<std::pair<int, bool>, QFont> fonts;
    mutable std::map<std::pair<int, bool>, QFontMetricsF> mets;

    const QFont& font(int px, bool bold) const {
        auto key = std::make_pair(px, bold);
        auto it = fonts.find(key);
        if (it == fonts.end()) {
            QFont f(family);
            f.setPixelSize(px);
            f.setBold(bold);
            it = fonts.emplace(key, f).first;
            mets.emplace(key, QFontMetricsF(f));
        }
        return it->second;
    }
    const QFontMetricsF& met(int px, bool bold) const {
        font(px, bold);
        return mets.at({px, bold});
    }
    QColor qcol(Color c) const {
        return QColor(c.r, c.g, c.b, c.a);
    }
    int measure(const std::string& s, int px, bool bold) const {
        return (int)std::lround(
            met(px, bold).horizontalAdvance(qstr(s)));
    }
    /// y はテキスト上端（ui::Font の AddText と同じ座標系）。
    void draw(float x, float y, const std::string& s, int px, bool bold,
              Color c) {
        if (!p || s.empty()) return;
        const QFont& f = font(px, bold);
        const QFontMetricsF& m = met(px, bold);
        p->setFont(f);
        p->setPen(qcol(c));
        p->drawText(QPointF(x, y + m.ascent()), qstr(s));
    }
    /// 中線基準（field.js dominant-baseline=middle。Liberation Sans の
    /// xHeight ≈ 0.52em。yMid はスケール済み座標）。
    void drawMid(float x, float yMid, const std::string& s, int px,
                 bool bold, Color c) {
        const QFontMetricsF& m = met(px, bold);
        float xh = m.xHeight();
        float y0 = yMid + xh * 0.5f - m.ascent();
        draw(x, y0, s, px, bold, c);
    }
};

class QpCanvas final : public bl::Canvas {
public:
    QpCanvas(QPainter* p, QtTextCtx* tc, SDL_Renderer* iconR)
        : p_(p), tc_(tc), iconR_(iconR) {}

    void fillPoly(const float* xy, int n, Color c) override {
        QPolygonF poly;
        poly.reserve(n);
        for (int i = 0; i < n; ++i) poly << QPointF(xy[i * 2], xy[i * 2 + 1]);
        p_->setPen(Qt::NoPen);
        p_->setBrush(qcol(c));
        p_->drawPolygon(poly);
    }
    void strokePoly(const float* xy, int n, Color c, float t,
                    bool close) override {
        QPolygonF poly;
        poly.reserve(n + 1);
        for (int i = 0; i < n; ++i) poly << QPointF(xy[i * 2], xy[i * 2 + 1]);
        if (close && n) poly << poly.first();
        p_->setBrush(Qt::NoBrush);
        p_->setPen(QPen(qcol(c), t, Qt::SolidLine, Qt::FlatCap,
                        Qt::MiterJoin));
        p_->drawPolyline(poly);
    }
    void fillRect(float x, float y, float w, float h, Color c) override {
        p_->fillRect(QRectF(x, y, w, h), qcol(c));
    }
    void strokeRect(float x, float y, float w, float h, Color c,
                    float t) override {
        p_->setBrush(Qt::NoBrush);
        p_->setPen(QPen(qcol(c), t));
        p_->drawRect(QRectF(x, y, w, h));
    }
    void fillRoundRect(float x, float y, float w, float h, float rad,
                       Color c) override {
        p_->setPen(Qt::NoPen);
        p_->setBrush(qcol(c));
        p_->drawRoundedRect(QRectF(x, y, w, h), rad, rad);
    }
    void strokeRoundRect(float x, float y, float w, float h, float rad,
                         Color c, float t) override {
        p_->setBrush(Qt::NoBrush);
        p_->setPen(QPen(qcol(c), t));
        p_->drawRoundedRect(QRectF(x, y, w, h), rad, rad);
    }
    void line(float x0, float y0, float x1, float y1, Color c,
              float t) override {
        p_->setPen(QPen(qcol(c), t));
        p_->drawLine(QPointF(x0, y0), QPointF(x1, y1));
    }
    void fillCircle(float cx, float cy, float rad, Color c) override {
        p_->setPen(Qt::NoPen);
        p_->setBrush(qcol(c));
        p_->drawEllipse(QPointF(cx, cy), rad, rad);
    }
    void strokeCircle(float cx, float cy, float rad, float t,
                      Color c) override {
        p_->setBrush(Qt::NoBrush);
        p_->setPen(QPen(qcol(c), t));
        p_->drawEllipse(QPointF(cx, cy), rad, rad);
    }
    void icon(const bl::IconPx* ipx, SDL_Renderer* r, float x, float y,
              float w, float h, float alpha) override {
        if (!ipx) return;
        QImage& img = icons_[ipx];
        if (img.isNull()) {
            // icons_rgba.inc は RGBA8（premultiply して描画）。
            img = QImage(ipx->w, ipx->h, QImage::Format_RGBA8888);
            std::memcpy(img.bits(), ipx->px, size_t(ipx->w) * ipx->h * 4);
            img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
        const qreal old = p_->opacity();
        p_->setOpacity(old * alpha);
        p_->drawImage(QRectF(x, y, w, h), img);
        p_->setOpacity(old);
    }
    void pushClip(float x0, float y0, float x1, float y1,
                  bool intersect) override {
        saves_.push_back(0);
        p_->save();
        p_->setClipRect(QRectF(x0, y0, x1 - x0, y1 - y0),
                        intersect ? Qt::IntersectClip : Qt::ReplaceClip);
    }
    void popClip() override {
        if (!saves_.empty()) saves_.pop_back();
        p_->restore();
    }
    void clearIcons() override { icons_.clear(); }

private:
    QColor qcol(Color c) const { return QColor(c.r, c.g, c.b, c.a); }
    QPainter* p_;
    QtTextCtx* tc_;
    SDL_Renderer* iconR_;
    std::map<const bl::IconPx*, QImage> icons_;
    std::vector<int> saves_;
};

// ---------------------------------------------------------------------------
// EdCore — Qt 非依存のエディタ状態（editor.cpp の Editor から移植した核）
// ---------------------------------------------------------------------------
struct EdCore {
    Runtime* rt = nullptr;
    SDL_Renderer* r = nullptr;   // アイコン描画用（SDL パス互換。Qt では未使用）
    QtTextCtx* tc = nullptr;     // テキスト計測・描画
    std::string path;

    // ---- 選択 / カテゴリ ----
    int tab = 0;
    int selTarget = 0;
    int catSel = 0;
    float flyScroll = 0;
    std::vector<edit::CategoryDef> cats_;

    Target* selected() {
        if (!rt) return nullptr;
        selTarget = std::clamp(selTarget, 0, (int)rt->targets.size() - 1);
        return rt->targets[selTarget];
    }
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
        cats_.push_back(edit::CategoryDef{"more", "My Blocks", 0xFF6680, {}});
        for (const auto& c : edit::kExtensionCategories)
            if (uses(c.id + "_")) cats_.push_back(c);
    }
    int catCount() const { return (int)cats_.size(); }
    const edit::CategoryDef& catAt(int i) const { return cats_[i]; }

    // ---- ワークスペース ----
    float panX = 60, panY = 40;
    float zoom = 1.0f;
    float wsScale() const { return bl::kWsZoom * zoom; }
    float wsW = 800, wsH = 600;   // ビューのサイズ（QML 側から設定）

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
        wsDirty_ = true;
    }
    void redo() {
        Target* t = selected();
        if (!t || !t->blocks || redoStack.empty()) return;
        undoStack.push_back(takeSnapshot());
        Snapshot s = std::move(redoStack.back());
        redoStack.pop_back();
        t->blocks->restoreState(std::move(s.blocks), std::move(s.scripts));
        selBlock.clear();
        wsDirty_ = true;
    }
    void clearHistory() { undoStack.clear(); redoStack.clear(); }

    // ---- TextRenderer ----
    float trScale_ = 1.0f;
    static int measureThunk(void* ctx, const std::string& s, int px,
                            bool bold) {
        return static_cast<EdCore*>(ctx)->tc->measure(s, px, bold);
    }
    static void drawThunk(void* ctx, float x, float y, const std::string& s,
                          int px, bool bold, Color c) {
        EdCore* e = static_cast<EdCore*>(ctx);
        int npx = std::max(4, (int)std::lround(px * e->trScale_));
        e->tc->draw(x, y, s, npx, bold, c);
    }
    static void drawMidThunk(void* ctx, float x, float yMid,
                             const std::string& s, int px, bool bold,
                             Color c) {
        EdCore* e = static_cast<EdCore*>(ctx);
        int npx = std::max(4, (int)std::lround(px * e->trScale_));
        e->tc->drawMid(x, yMid, s, npx, bold, c);
    }
    bl::TextRenderer tr(float s = -1.f) {
        bl::TextRenderer t;
        t.ctx = this;
        t.measure = &EdCore::measureThunk;
        t.draw = &EdCore::drawThunk;
        t.drawMid = &EdCore::drawMidThunk;
        t.scale = s > 0 ? s : wsScale();
        trScale_ = t.scale;
        return t;
    }

    // ---- ブロック操作（editor.cpp から移植）----
    std::string selBlock;
    int soundSel_ = -1;          // Sounds タブの選択行
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
                    b.fields[a.name] =
                        BlockField{Value::string(SStr::fromUtf8(a.dflt))};
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
            const char* wantType = "";
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
        Blocks::StructuralEdit sourceEdit(*blocks);
        blocks->createBlock(b);
        if (argTexts) {
            for (auto& [inm, txt] : *argTexts) {
                if (txt.empty()) continue;
                std::string sid = makeShadowBlock(blocks, "text", "TEXT", txt, id);
                if (Block* nb = sourceEdit.getBlock(id)) {
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
        Blocks::StructuralEdit edit(*blocks);
        std::string prev = bl::prevOf(blocks, id);
        std::string parent;
        if (Block* b = edit.getBlock(id)) {
            parent = b->parent;
            if (!prev.empty()) {
                if (Block* p = edit.getBlock(prev)) p->next.clear();
            } else if (!parent.empty()) {
                if (Block* p = edit.getBlock(parent)) {
                    for (auto& [nm, in] : p->inputs.items)
                        if (in.block == id) in.block.clear();
                }
            }
            if (b->topLevel) { blocks->deleteScript(id); }
        }
        blocks->deleteBlock(id);
        if (selBlock == id) selBlock.clear();
        wsDirty_ = true;
    }
    void deleteSelectedBlock() {
        if (selBlock.empty()) return;
        Target* t = selected();
        if (!t || !t->blocks) return;
        pushUndo();
        Blocks* blocks = t->blocks;
        const std::string delId = selBlock;
        const Block* source = blocks->getBlock(delId);
        if (source) {
            Blocks::StructuralEdit edit(*blocks);
            Block* sb = edit.getBlock(delId);
            const std::string nxt = sb->next;
            const bool wasTop = sb->topLevel;
            std::string parentId = bl::prevOf(blocks, delId);
            sb->next.clear();
            if (!parentId.empty()) {
                if (Block* pb = edit.getBlock(parentId)) pb->next = nxt;
            }
            if (!nxt.empty()) {
                if (Block* nb = edit.getBlock(nxt)) {
                    nb->parent = parentId;
                    if (wasTop) { nb->topLevel = true; blocks->addScript(nxt); }
                }
            }
            blocks->deleteBlock(delId);
        }
        selBlock.clear();
        wsDirty_ = true;
    }
    std::string dupRec(Blocks* blocks, const std::string& srcId,
                       std::unordered_map<std::string, std::string>& map,
                       bool head) {
        auto it = map.find(srcId);
        if (it != map.end()) return it->second;
        const Block* sp = blocks->getBlock(srcId);
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
        Blocks::StructuralEdit edit(*blocks);
        std::string nid = dupRec(blocks, id, map, true);
        if (!nid.empty()) selBlock = nid;
        wsDirty_ = true;
    }
    void deleteAllBlocks() {
        Target* t = selected();
        Blocks* blocks = t ? t->blocks : nullptr;
        if (!blocks) return;
        pushUndo();
        Blocks::StructuralEdit sourceEdit(*blocks);
        // deleteBlock() mutates getScripts(); never iterate that vector
        // while deleting its entries (invalidated references/iterators).
        const auto scripts = blocks->getScripts();
        for (const auto& sid : scripts) blocks->deleteBlock(sid);
        selBlock.clear();
        wsDirty_ = true;
    }
    /// チェーン内の非 shadow ブロック数（ctx の "Delete N Blocks" 用）
    int chainCount(const std::string& id) const {
        Target* t = const_cast<EdCore*>(this)->selected();
        Blocks* blocks = t ? t->blocks : nullptr;
        if (!blocks || id.empty()) return 0;
        int n = 0;
        for (Block* cur = blocks->getBlock(id); cur;
             cur = cur->next.empty() ? nullptr : blocks->getBlock(cur->next))
            if (!cur->shadow) ++n;
        return n;
    }
    int allBlockCount() const {
        Target* t = const_cast<EdCore*>(this)->selected();
        Blocks* blocks = t ? t->blocks : nullptr;
        if (!blocks) return 0;
        int n = 0;
        for (auto& [id, b] : blocks->allBlocks())
            if (!b.shadow) ++n;
        return n;
    }

    // ---- コメント編集 ----
    struct CmtEdit {
        bool active = false;
        std::string blockId;
    } cmt_;
    void startEditingComment(const std::string& id) {
        Target* t = selected();
        if (!t || !t->blocks || id.empty()) return;
        if (!t->blocks->getBlock(id)) return;
        cmt_.active = true;
        cmt_.blockId = id;
    }
    QString commentText() const {
        Target* t = const_cast<EdCore*>(this)->selected();
        if (!t || !t->blocks) return QString();
        const Block* b = t->blocks->getBlock(cmt_.blockId);
        return b ? QString::fromStdString(b->comment) : QString();
    }
    void commitComment(const QString& text) {
        Target* t = selected();
        if (t && t->blocks) {
            if (Block* b = t->blocks->getBlock(cmt_.blockId)) {
                pushUndo();
                b->comment = text.toStdString();
                t->blocks->resetCache();
                wsDirty_ = true;
            }
        }
        cmt_.active = false;
        cmt_.blockId.clear();
    }
    void removeComment(const std::string& id) {
        Target* t = selected();
        if (!t || !t->blocks) return;
        if (Block* b = t->blocks->getBlock(id)) {
            pushUndo();
            b->comment.clear();
            t->blocks->resetCache();
            wsDirty_ = true;
        }
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
    /// バックパック項目を ws 中央へ貼り付ける
    void backpackPaste(int i, float viewW, float viewH) {
        Target* t = selected();
        if (!t || !t->blocks) return;
        if (i < 0 || i >= (int)backpack.size()) return;
        JsonValue frag = jsonParse(backpack[i].json);
        if (!frag.isObject()) return;
        pushUndo();
        Blocks::StructuralEdit sourceEdit(*t->blocks);
        std::string head;
        loadBlocksFragment(*t->blocks, frag, &head);
        if (head.empty()) return;
        if (Block* h = sourceEdit.getBlock(head)) {
            h->topLevel = true;
            h->parent.clear();
            h->x = (viewW / 2 - panX) / wsScale();
            h->y = (viewH / 2 - panY) / wsScale();
            t->blocks->addScript(head);
        }
        selBlock = head;
        wsDirty_ = true;
    }

    // ---- 右クリックコンテキストメニュー ----
    struct CtxItem { QString label; QString act; bool enabled = true; };
    std::vector<CtxItem> ctx_;
    std::string ctxBlockId_;
    /// ws 上の右クリック（x,y は ws ビュー座標）。メニュー内容を構築する。
    /// 戻り値: メニューを開くなら true。
    bool buildWsContext(float x, float y) {
        ctx_.clear();
        ctxBlockId_.clear();
        float s = wsScale();
        bl::LBlock* lb = wsBlockAtLogical((x - panX) / s, (y - panY) / s);
        if (lb) {
            // scratch-blocks BlockSvg.showContextMenu_ 準拠:
            // Duplicate / Add|Remove Comment / Delete N Blocks / Copy to Backpack
            ctxBlockId_ = lb->id;
            selBlock = lb->id;
            bool hasCmt = lb->block && !lb->block->comment.empty();
            int n = chainCount(lb->id);
            ctx_.push_back({"Duplicate", "dup"});
            ctx_.push_back({hasCmt ? "Remove Comment" : "Add Comment",
                            hasCmt ? "rmcomment" : "addcomment"});
            ctx_.push_back({n <= 1 ? "Delete Block"
                                   : QString("Delete %1 Blocks").arg(n),
                            "del"});
            ctx_.push_back({"Copy to Backpack", "backpack"});
        } else {
            // WorkspaceSvg.showContextMenu_ 準拠:
            // Undo / Redo / Clean up Blocks / Paste / Delete N Blocks
            Target* t = selected();
            Blocks* blocks = t ? t->blocks : nullptr;
            ctx_.push_back({"Undo", "undo", !undoStack.empty()});
            ctx_.push_back({"Redo", "redo", !redoStack.empty()});
            ctx_.push_back({"Clean up Blocks", "cleanup",
                            blocks && !blocks->getScripts().empty()});
            if (!backpack.empty())
                ctx_.push_back({"Paste", "paste"});
            int total = allBlockCount();
            ctx_.push_back({total <= 1 ? "Delete Block"
                                       : QString("Delete %1 Blocks").arg(total),
                            "delall", total > 0});
        }
        return true;
    }
    QVariantList ctxItems() const {
        QVariantList l;
        for (auto& c : ctx_) {
            QVariantMap m;
            m["label"] = c.label;
            m["enabled"] = c.enabled;
            l.push_back(m);
        }
        return l;
    }

    // ---- Make a Variable / List ----
    void addVariable(bool list) {
        Target* t = selected();
        if (!t) return;
        pushUndo();
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

    // ---- モニタ表示切替（変数行チェックボックス）----
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
        if (rt->monitorBlocks) {
            Blocks::StructuralEdit sourceEdit(*rt->monitorBlocks);
            if (Block* b = sourceEdit.getBlock(varId))
                b->isMonitored = rec.visible;
        }
    }

    // ---- Make a Block ダイアログ（簡易版: QML 側で parts 編集）----
    struct MakeBlockDlg {
        enum PartKind { Text = 0, StrInput = 1, BoolInput = 2 };
        struct Part { PartKind kind; std::string text; };
        bool open = false;
        std::vector<Part> parts;
        bool warp = false;
        void reset() {
            open = true; warp = false;
            parts.clear();
            parts.push_back({Text, "block name"});
        }
        std::string proccode() const {
            std::string pc;
            for (const Part& p : parts) {
                if (p.kind == Text) {
                    if (!pc.empty()) pc += " ";
                    pc += p.text;
                } else {
                    pc += (p.kind == BoolInput ? " %b" : " %s");
                }
            }
            return pc;
        }
    };
    MakeBlockDlg mb_;

    /// Make a Block — proccode/mutation 付きで定義（prototype + hat）を生成。
    void makeProcedure() {
        Target* t = selected();
        if (!t || !t->blocks) return;
        Blocks* blocks = t->blocks;
        pushUndo();
        Blocks::StructuralEdit sourceEdit(*blocks);
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
        // ワークスペース表示中央上あたり（論理座標）
        def.x = (-panX) / wsScale() + 40;
        def.y = (-panY) / wsScale() + 40;
        BlockInput in;
        in.block = protoId;
        in.shadow = protoId;
        def.inputs.set("custom_block", in);
        proto.parent = def.id;
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
        if (Block* dp = sourceEdit.getBlock(def.id)) {
            dp->topLevel = true;
            blocks->addScript(def.id);
        }
        wsDirty_ = true;
        flyDirty_ = true;
    }

    // ---- フライアウト ----
    struct FlyItem {
        enum Kind { BlockT, Header, Button, VarRow, ListRow, Divider } kind = BlockT;
        std::string opcode;
        std::string text;
        std::string varId;
        bool list = false;
        std::map<std::string, BlockField> initFields;
        std::optional<Mutation> mutation;
        std::map<std::string, std::string> argTexts;
        bool makeBlock = false;
        bl::LBlock lb;
        bl::BlockStyle st;
        float y = 0, h = 0;
    };
    std::vector<FlyItem> flyItems_;
    std::string flyCat_;
    int flyTarget_ = -1;
    bool flyDirty_ = true;

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

    void rebuildFlyout() {
        flyItems_.clear();
        if (catSel < 0 || catSel >= catCount()) return;
        const auto& cat = catAt(catSel);
        Blocks tmp(rt);
        float y = 12;
        auto& items = flyItems_;
        Target* t = selected();
        if (cat.id == "data") {
            FlyItem mkv;
            mkv.kind = FlyItem::Button;
            mkv.text = "Make a Variable";
            mkv.list = false;
            mkv.y = y; mkv.h = 40;
            items.push_back(mkv);
            y += 52;
            if (t) {
                for (auto& [vid, v] : t->variables) {
                    if (v.type == Variable::kListType) continue;
                    FlyItem fi;
                    // ドラッグ用テンプレ（data_variable に VAR を束縛）
                    std::map<std::string, BlockField> init;
                    BlockField f{Value::string(
                        SStr::fromUtf8(utf16ToUtf8(v.name)))};
                    f.id = vid;
                    init["VAR"] = f;
                    makeFlyItem(&tmp, "data_variable", init, fi);
                    fi.kind = FlyItem::VarRow;
                    fi.list = false;
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
                    bool isListRep = op == "data_listcontents";
                    bool hasVar = false;
                    if (t) for (auto& [vid, v] : t->variables)
                        if ((v.type == Variable::kListType) == isListRep)
                            hasVar = true;
                    if (!hasVar) continue;
                    continue;
                }
                FlyItem fi;
                makeFlyItem(&tmp, op, {}, fi);
                fi.y = y;
                items.push_back(fi);
                y += fi.h + 12;
            }
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
                    std::map<std::string, BlockField> init;
                    BlockField f{Value::string(
                        SStr::fromUtf8(utf16ToUtf8(v.name)))};
                    f.id = vid;
                    init["LIST"] = f;
                    makeFlyItem(&tmp, "data_listcontents", init, fi);
                    fi.kind = FlyItem::ListRow;
                    fi.list = true;
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
            FlyItem mkb;
            mkb.kind = FlyItem::Button;
            mkb.text = "Make a Block";
            mkb.makeBlock = true;
            mkb.y = y; mkb.h = 40;
            items.push_back(mkb);
            y += 52;
            if (t && t->blocks) {
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
                    fi.mutation->parseArgs();
                    int i = 0;
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
            y += fi.h + 12;
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
    /// flyout ビュー座標 (mx,my) → FlyItem（ビュー幅 w は QML 側から渡す）。
    FlyItem* flyItemAt(float mx, float my, float viewW) {
        for (auto& f : flyItems_) {
            float sy = f.y * bl::kWsZoom - flyScroll;
            if (my < sy || my >= sy + f.h * bl::kWsZoom) continue;
            if (f.kind == FlyItem::BlockT) {
                float bx = 10;
                if (mx >= bx && mx < bx + f.lb.w * bl::kWsZoom) return &f;
            } else {
                if (mx >= 0 && mx < viewW) return &f;
            }
        }
        return nullptr;
    }

    // ---- ワークスペースレイアウト ----
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
    /// ws 論理座標 (lx,ly) のブロックをヒットテスト。
    bl::LBlock* wsBlockAtLogical(float lx, float ly) {
        bl::LBlock* best = nullptr;
        for (auto& lb : wsLayout_.blocks) {
            if (lb.shadow) continue;
            const float rx = lx - lb.x;
            const float ry = ly - lb.y;
            if (rx < 0 || rx >= lb.w) continue;
            bool inside = ry >= 0 && ry < lb.fullH;
            if (!inside && lb.hat && ry < 0 && ry >= -48) {
                const Path path = bl::blockOutline(lb);
                if (path.pts.size() >= 3) {
                    for (size_t i = 0, j = path.pts.size() - 1;
                         i < path.pts.size(); j = i++) {
                        const auto& a = path.pts[i];
                        const auto& b = path.pts[j];
                        if ((a.y > ry) != (b.y > ry) &&
                            rx < (b.x - a.x) * (ry - a.y) / (b.y - a.y) + a.x)
                            inside = !inside;
                    }
                }
            }
            if (inside && (!best || lb.depth > best->depth)) best = &lb;
        }
        return best;
    }
    bl::LBlock* wsBlockAtScreen(float sx, float sy) {
        float s = wsScale();
        return wsBlockAtLogical((sx - panX) / s, (sy - panY) / s);
    }

    // ---- ドラッグ（editor.cpp から移植）----
    enum class DragKind { None, Chain, ValueInput, Substack, TopLevel };
    struct Drag {
        bool active = false;
        bool pending = false;
        bool fromFlyout = false;
        FlyItem* flyItem = nullptr;
        std::string opcode;
        std::map<std::string, BlockField> initFields;
        std::optional<Mutation> mutation;
        std::map<std::string, std::string> argTexts;
        std::string headId, tailId;
        DragKind kind = DragKind::None;
        std::string prevId, tailNext, prevParent, prevInput;
        bool wasTop = false;
        edit::Shape shape = edit::Shape::Stack;
        float mx = 0, my = 0;
        float offX = 0, offY = 0;
        std::string targetId;
        bool before = false, valueTarget = false, substackTarget = false;
        int subBranch = 0;
        std::string valueInput;
        bool hasSnap = false;
        float snapX = 0, snapY = 0;
        float snapHeadX = 0, snapHeadY = 0;
        float chainH = 0, headH = 0;
        std::vector<bl::LBlock> previewLayout;
        bool connectionsReady = false, hasPrevious = true, hasNext = true;
        bool snapshotTaken = false;
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
        const Block* found = blocks->getBlock(drag.headId);
        if (!found) return;
        Blocks::StructuralEdit edit(*blocks);
        Block* b = edit.getBlock(drag.headId);
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
        Block* tl = edit.getBlock(drag.tailId);
        drag.tailNext = tl ? tl->next : "";

        if (isReporter) {
            if (!b->parent.empty()) {
                drag.kind = DragKind::ValueInput;
                drag.prevParent = b->parent;
                if (Block* p = edit.getBlock(drag.prevParent)) {
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
                if (Block* px = edit.getBlock(drag.prevId)) px->next = drag.tailNext;
            } else if (!b->parent.empty() &&
                       substackBranch(edit.getBlock(b->parent)) >= 0) {
                drag.kind = DragKind::Substack;
                drag.prevParent = b->parent;
                drag.prevInput = substackBranch(edit.getBlock(b->parent)) == 0
                                     ? "SUBSTACK" : "SUBSTACK2";
                if (Block* p = edit.getBlock(b->parent)) {
                    BlockInput* in = p->inputs.find(drag.prevInput);
                    if (in) in->block = drag.tailNext;
                }
            } else {
                drag.kind = DragKind::TopLevel;
                if (drag.wasTop) { blocks->deleteScript(drag.headId); b->topLevel = false; }
            }
            if (tl) tl->next.clear();
            if (!drag.tailNext.empty())
                if (Block* g = edit.getBlock(drag.tailNext))
                    g->parent = drag.prevId.empty() ? b->parent : drag.prevId;
            b->parent.clear();
        }
        wsDirty_ = true;
    }

    struct Conn {
        int kind = 0;
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
                if (g.input.empty() || g.kind == bl::SegKind::Text) continue;
                if (!b->inputs.has(g.input)) continue;
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
        if (drag.shape == edit::Shape::Hat) return;
        bl::LBlock* lb = wsBlockAtLogical(drag.mx + 32, drag.my);
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
        if (std::getenv("XYO_EDIT_DEBUG"))
            std::fprintf(stderr,
                         "[edqt] doDrop fly=%d op=%s t=%p blocks=%p mx=%.0f my=%.0f\n",
                         (int)drag.fromFlyout, drag.opcode.c_str(),
                         (void*)t, t ? (void*)t->blocks : nullptr,
                         drag.mx, drag.my);
        if (!t || !t->blocks) { drag.active = false; return; }
        Blocks* blocks = t->blocks;
        Blocks::StructuralEdit edit(*blocks);
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
        bool isRep = drag.shape == edit::Shape::Reporter ||
                     drag.shape == edit::Shape::Boolean;
        const std::string tail = bl::chainTail(blocks, id);
        if (Block* head = edit.getBlock(id)) {
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
                bl::insertBefore(blocks, id, tail, drag.targetId);
            else
                bl::insertAfter(blocks, id, tail, drag.targetId);
        } else {
            if (Block* b = edit.getBlock(id)) { b->x = drag.mx; b->y = drag.my; }
            bl::makeTopLevel(blocks, id);
        }
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
            Blocks::StructuralEdit edit(*blocks);
            if (drag.kind == DragKind::ValueInput) {
                if (!drag.prevParent.empty() && !drag.prevInput.empty()) {
                    if (Block* p = edit.getBlock(drag.prevParent)) {
                        BlockInput* in = p->inputs.find(drag.prevInput);
                        if (in) in->block = drag.headId;
                    }
                    if (Block* h = edit.getBlock(drag.headId)) h->parent = drag.prevParent;
                }
            } else if (drag.kind == DragKind::Substack) {
                if (!drag.prevParent.empty() && !drag.prevInput.empty()) {
                    if (Block* p = edit.getBlock(drag.prevParent)) {
                        BlockInput* in = p->inputs.find(drag.prevInput);
                        if (in) in->block = drag.headId;
                    }
                    if (Block* h = edit.getBlock(drag.headId)) h->parent = drag.prevParent;
                }
                if (Block* tl = edit.getBlock(drag.tailId)) tl->next = drag.tailNext;
                if (!drag.tailNext.empty())
                    if (Block* g = edit.getBlock(drag.tailNext)) g->parent = drag.tailId;
            } else if (drag.kind == DragKind::Chain) {
                if (!drag.prevId.empty()) {
                    if (Block* px = edit.getBlock(drag.prevId)) px->next = drag.headId;
                    if (Block* h = edit.getBlock(drag.headId)) h->parent = drag.prevId;
                } else if (drag.wasTop) {
                    if (Block* h = edit.getBlock(drag.headId)) {
                        h->topLevel = true;
                        blocks->addScript(drag.headId);
                    }
                }
                if (Block* tl = edit.getBlock(drag.tailId)) tl->next = drag.tailNext;
                if (!drag.tailNext.empty())
                    if (Block* g = edit.getBlock(drag.tailNext)) g->parent = drag.tailId;
            } else if (drag.kind == DragKind::TopLevel) {
                if (drag.wasTop) bl::makeTopLevel(blocks, drag.headId);
            }
        }
        drag.active = false;
        drag.pending = false;
        wsDirty_ = true;
    }

    // ---- 実行中ハットのグロウ ----
    std::set<std::string> runningHeads() {
        std::set<std::string> out;
        Target* t = selected();
        if (!rt || !t) return out;
        for (const Thread* th : rt->threads)
            if (th && th->target == t && !th->topBlock.empty())
                out.insert(th->topBlock);
        return out;
    }

    // ---- スプライト ----
    void selectTarget(int i) {
        if (!rt || i < 0 || i >= (int)rt->targets.size()) return;
        if (selTarget != i) {
            selTarget = i;
            clearHistory();
            selBlock.clear();
            wsDirty_ = true;
            flyDirty_ = true;
        }
    }
    void deleteSprite(int ti) {
        if (!rt || ti < 0 || ti >= (int)rt->targets.size()) return;
        Target* t = rt->targets[ti];
        if (!t || t->isStage) return;
        Sprite* spr = t->sprite;
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
            for (int i = 0; i < (int)rt->targets.size(); ++i)
                if (!rt->targets[i]->isStage) { selTarget = i; break; }
        wsDirty_ = true; flyDirty_ = true;
    }

    std::string savePath;   // XYO_EDIT_SAVE（空なら path へ上書き）
    bool save() {
        if (!rt) return false;
        const std::string& dst = savePath.empty() ? path : savePath;
        if (dst.empty()) return false;
        return saveSb3File(*rt, dst.c_str());
    }

    // ---- インラインフィールド編集（editor.cpp:657-835 移植、SDL → QML） ----
    struct FRect { float x, y, w, h; };
    struct FieldEdit {
        bool active = false;
        std::string blockId;
        std::string field;
        bool numeric = false;
        std::string buf;
        FRect rect{};    // 論理 rect（ws 上のフィールド箱）
        bool hasRect = false;
    } fedit_;
    struct DropMenu {
        bool open = false;
        std::vector<std::string> items;
        std::vector<std::string> ids;
        std::string blockId;
        std::string fieldName;
        bool variable = false;
        int checked = -1;
        bl::Color bg{255, 255, 255, 255}, border{128, 128, 128, 255};
        float x = 0, y = 0;  // ws ビュー座標
    } menu_;
    /// クリックした seg が編集対象なら編集/メニューを開き、true を返す。
    /// sx/sy は ws ビュー px（メニュー位置）。
    bool clickSeg(bl::LBlock& lb, const bl::LSeg& g, float sx, float sy) {
        switch (g.kind) {
        case bl::SegKind::Dropdown:
        case bl::SegKind::Variable:
            openDropdownMenu(lb.id, g, sx, sy + 10);
            return true;
        case bl::SegKind::NumField:
        case bl::SegKind::StrField:
            startEditingField(lb.block, g, lb);
            return true;
        default:
            break;
        }
        return false;
    }
    void openDropdownMenu(const std::string& blockId, const bl::LSeg& g,
                          float sx, float sy) {
        Target* t = selected();
        if (!t || !t->blocks) return;
        Block* b = t->blocks->getBlock(blockId);
        if (!b) return;
        menu_.items.clear();
        menu_.ids.clear();
        menu_.fieldName = g.field;
        menu_.variable = false;
        if (g.kind == bl::SegKind::Variable) {
            menu_.blockId = blockId;
            menu_.variable = true;
            for (auto& [vid, v] : t->variables) {
                bool isList = v.type == Variable::kListType;
                if (isList != g.list) continue;
                menu_.items.push_back(utf16ToUtf8(v.name));
                menu_.ids.push_back(vid);
            }
        } else {
            // ソケット内のメニュー shadow（motion_goto_menu 等）は g.input
            // 経由で shadow ブロックへ解決してから def を引く。親の
            // Value arg（options 空）へ当たると空メニューになる。
            Block* mb = b;
            if (!g.input.empty()) {
                const BlockInput* in = b->inputs.find(g.input);
                if (in && !in->block.empty()) {
                    if (Block* sh = t->blocks->getBlock(in->block))
                        mb = sh;
                }
            }
            const edit::BlockDef* d = edit::findBlock(mb->opcode);
            if (!d) {
                if (std::getenv("XYO_EDIT_DEBUG"))
                    std::fprintf(stderr, "[edqt] menu: no def for %s\n",
                                 mb->opcode.c_str());
                return;
            }
            // フィールド名が分かっていればそれで、なければ最初の Dropdown arg
            const edit::ArgDef* a = nullptr;
            if (!menu_.fieldName.empty())
                for (const auto& arg : d->args)
                    if (arg.name == menu_.fieldName) { a = &arg; break; }
            if (!a)
                for (const auto& arg : d->args)
                    if (arg.kind == edit::ArgKind::Dropdown) { a = &arg; break; }
            if (!a || a->options.empty()) return;
            menu_.blockId = mb->id;
            menu_.fieldName = a->name;
            for (size_t i = 0; i < a->options.size(); i += 2) {
                menu_.items.push_back(a->options[i]);
                menu_.ids.push_back(a->options.size() > i + 1 ? a->options[i + 1]
                                                            : a->options[i]);
            }
            b = mb;
        }
        menu_.open = true;
        menu_.x = sx;
        menu_.y = sy;
        bl::BlockStyle st = bl::styleOf(t->blocks, b);
        menu_.bg = st.primary;
        menu_.border = st.tertiary;
        menu_.checked = -1;
        std::string cur = bl::fieldStr(b, g.field);
        std::string curId =
            b->fields.count(g.field) ? b->fields.at(g.field).id : "";
        for (size_t i = 0; i < menu_.ids.size(); ++i)
            if (menu_.variable ? menu_.ids[i] == curId
                               : menu_.ids[i] == cur) {
                menu_.checked = (int)i;
                break;
            }
    }
    void menuPick(int i) {
        if (std::getenv("XYO_EDIT_DEBUG"))
            std::fprintf(stderr,
                         "[edqt] menuPick open=%d i=%d n=%zu blk=%s fld=%s\n",
                         (int)menu_.open, i, menu_.items.size(),
                         menu_.blockId.c_str(), menu_.fieldName.c_str());
        if (!menu_.open || i < 0 || i >= (int)menu_.items.size()) return;
        Target* t = selected();
        if (!t || !t->blocks) { menu_.open = false; return; }
        Block* b = t->blocks->getBlock(menu_.blockId);
        if (std::getenv("XYO_EDIT_DEBUG"))
            std::fprintf(stderr, "[edqt] menuPick b=%p\n", (void*)b);
        if (b) {
            pushUndo();
            if (menu_.variable) {
                std::string nm;
                for (auto& [vid, v] : t->variables)
                    if (vid == menu_.ids[i]) { nm = utf16ToUtf8(v.name); break; }
                BlockField f{Value::string(SStr::fromUtf8(nm))};
                f.id = menu_.ids[i];
                t->blocks->setBlockField(menu_.blockId, menu_.fieldName,
                                         std::move(f));
            } else {
                t->blocks->setBlockField(
                    menu_.blockId, menu_.fieldName,
                    BlockField{Value::string(SStr::fromUtf8(menu_.ids[i]))});
            }
            wsDirty_ = true;
        }
        menu_.open = false;
    }
    void startEditingField(Block* b, const bl::LSeg& g, const bl::LBlock& lb) {
        if (!b) return;
        Target* t = selected();
        if (!t || !t->blocks) return;
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
        fedit_.active = true;
        fedit_.blockId = target->id;
        fedit_.field = fn;
        fedit_.numeric = isNum;
        fedit_.buf = bl::fieldStr(target, fn);
        fedit_.rect = FRect{lb.x + g.x, lb.y + g.y, g.w, g.h};
        fedit_.hasRect = true;
    }
    void applyEdit() {
        Target* t = selected();
        if (t && t->blocks && fedit_.active) {
            if (Block* b = t->blocks->getBlock(fedit_.blockId)) {
                pushUndo();
                BlockField value;
                if (fedit_.numeric) {
                    char* end = nullptr;
                    double dv = std::strtod(fedit_.buf.c_str(), &end);
                    if (end != fedit_.buf.c_str() && *end == 0)
                        value.value = Value::num(dv);
                    else
                        value.value = Value::string(SStr::fromUtf8(fedit_.buf));
                } else {
                    value.value = Value::string(SStr::fromUtf8(fedit_.buf));
                }
                t->blocks->setBlockField(fedit_.blockId, fedit_.field,
                                         std::move(value));
                wsDirty_ = true;
            }
        }
        cancelEdit();
    }
    void cancelEdit() {
        fedit_.active = false;
        fedit_.buf.clear();
        fedit_.hasRect = false;
    }

    // ---- スプライト並び替え・複製・書き出し ----
    /// 非 stage スプライトの visual index ↔ target index
    int spriteVisIndex(int ti) {
        if (!rt) return -1;
        int v = 0;
        for (size_t i = 0; i < rt->targets.size(); ++i) {
            if (rt->targets[i]->isStage) continue;
            if ((int)i == ti) return v;
            ++v;
        }
        return -1;
    }
    int spriteTiFromVis(int visIdx) {
        if (!rt) return -1;
        int v = 0;
        for (size_t i = 0; i < rt->targets.size(); ++i) {
            if (rt->targets[i]->isStage) continue;
            if (v == visIdx) return (int)i;
            ++v;
        }
        return -1;
    }
    /// targets / executableTargets を reorder（orderAt.js 相当:
    /// stage は先頭固定、残りをスプライトタイル順に揃える）。
    void spriteReorder(int ti, int ins) {
        if (!rt) return;
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
        std::vector<Target*> nt, ne;
        for (Target* t : rt->targets) if (t->isStage) nt.push_back(t);
        for (Target* t : spr) nt.push_back(t);
        for (Target* t : nt)
            if (std::find(rt->executableTargets.begin(),
                          rt->executableTargets.end(), t) !=
                rt->executableTargets.end())
                ne.push_back(t);
        rt->targets = std::move(nt);
        rt->executableTargets = std::move(ne);
        selTarget = spriteTiFromVis(
            std::min(ins, (int)spr.size() - 1));
        clearHistory();
        wsDirty_ = true;
        flyDirty_ = true;
    }
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
    /// Add Extension — カテゴリをツールボックスへ恒常追加。
    /// 既に有効な拡張 id の一覧（QML メニュー制御用）。
    std::vector<std::string> enabledExtensions() const {
        std::vector<std::string> l;
        for (const auto& c : cats_)
            if (c.id == "pen" || c.id == "music") l.push_back(c.id);
        return l;
    }
    void addExtension(const std::string& id) {
        for (const auto& c : cats_)
            if (c.id == id) return;            // 追加済み
        for (const auto& c : edit::kExtensionCategories)
            if (c.id == id) cats_.push_back(c);
        flyDirty_ = true;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// EditorController — QML との橋渡し
// ---------------------------------------------------------------------------
class EditorController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString projectName READ projectName CONSTANT)
    Q_PROPERTY(int tab READ tab WRITE setTab NOTIFY uiChanged)
    Q_PROPERTY(int selTarget READ selTarget WRITE setSelTarget NOTIFY uiChanged)
    Q_PROPERTY(bool running READ running NOTIFY uiChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY uiChanged)
    Q_PROPERTY(int layoutRev READ layoutRev NOTIFY layoutChanged)
    Q_PROPERTY(QStringList categoryNames READ categoryNames NOTIFY uiChanged)
    Q_PROPERTY(QVariantList categoryColors READ categoryColors NOTIFY uiChanged)
    Q_PROPERTY(int categorySel READ categorySel WRITE setCategorySel NOTIFY uiChanged)
    Q_PROPERTY(QVariantList spriteNames READ spriteNames NOTIFY uiChanged)
    Q_PROPERTY(QVariantList spriteInfo READ spriteInfo NOTIFY uiChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY uiChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY uiChanged)
    Q_PROPERTY(bool fieldEditActive READ fieldEditActive NOTIFY layoutChanged)
    Q_PROPERTY(QRectF fieldEditRect READ fieldEditRect NOTIFY layoutChanged)
    Q_PROPERTY(QString fieldEditText READ fieldEditText WRITE setFieldEditText NOTIFY layoutChanged)
    Q_PROPERTY(bool menuOpen READ menuOpen NOTIFY layoutChanged)
    Q_PROPERTY(QPointF menuPos READ menuPos NOTIFY layoutChanged)
    Q_PROPERTY(QStringList menuItems READ menuItems NOTIFY layoutChanged)
    Q_PROPERTY(int menuChecked READ menuChecked NOTIFY layoutChanged)
    Q_PROPERTY(QColor menuBg READ menuBg NOTIFY layoutChanged)
    Q_PROPERTY(QColor menuBorder READ menuBorder NOTIFY layoutChanged)

public:
    Runtime* rt = nullptr;
    EdCore ed;
    SdlRenderer ren;
    SDL_Window* sdlWin = nullptr;
    SDL_Renderer* sdlR = nullptr;
    SDL_Texture* stageTex = nullptr;
    int stageTexW = 0, stageTexH = 0;
    QImage stageImg;
    bool stageImgValid = false;
    int layoutRev_ = 0;
    // ステージドラッグ（QML からの座標をテクスチャ座標へ変換）
    float stageDispX = 0, stageDispY = 0, stageDispW = 480, stageDispH = 360;
    // キー状態 → ランタイム（ren.handleEvent へ合成 SDL_Event）
    bool mouseDown_ = false;
    // 設定永続化（stageMode/zoom/catSel）
    QSettings settings_{"xyo", "editor"};

    EditorController() { ed.tc = &tc_; }
    QtTextCtx tc_;

    QString projectName() const {
        std::string p = ed.path;
        size_t k = p.find_last_of("/\\");
        return qstr(k == std::string::npos ? p : p.substr(k + 1));
    }
    int tab() const { return ed.tab; }
    void setTab(int v) { ed.tab = v; Q_EMIT uiChanged(); }
    int selTarget() const { return ed.selTarget; }
    void setSelTarget(int v) {
        ed.selectTarget(v);
        bumpLayout();
    }
    bool running() const { return rt && !rt->threads.empty(); }
    bool dirty() const { return true; }
    int layoutRev() const { return layoutRev_; }
    QStringList categoryNames() const {
        QStringList l;
        for (auto& c : ed.cats_) l << qstr(c.name);
        return l;
    }
    QVariantList categoryColors() const {
        QVariantList l;
        for (auto& c : ed.cats_) {
            QColor q((c.primary >> 16) & 0xFF, (c.primary >> 8) & 0xFF,
                     c.primary & 0xFF);
            l << q.name();
        }
        return l;
    }
    int categorySel() const { return ed.catSel; }
    void setCategorySel(int v) {
        if (ed.catSel == v) return;
        ed.catSel = v;
        ed.flyDirty_ = true;
        persistCategory();
        bumpLayout();
    }
    QVariantList spriteNames() const {
        QVariantList l;
        if (!rt) return l;
        for (Target* t : rt->targets)
            l << (t->isStage ? QStringLiteral("Stage")
                             : qstr(utf16ToUtf8(t->getName())));
        return l;
    }
    QVariantList spriteInfo() const {
        QVariantList l;
        Target* t = const_cast<EdCore&>(ed).selected();
        if (!t) return l;
        l << qstr(utf16ToUtf8(t->getName()));
        l << t->x << t->y << t->size << t->direction << t->visible;
        return l;
    }
    // ---- スプライト情報パネルの編集 ----
    Q_INVOKABLE void setSpriteName(const QString& name) {
        Target* t = ed.selected();
        if (!t || !t->sprite || t->isStage || name.isEmpty()) return;
        t->sprite->name = utf8ToU16(name.toStdString());
        bumpLayout();
    }
    Q_INVOKABLE void setSpriteX(double x) {
        Target* t = ed.selected();
        if (!t || t->isStage) return;
        t->setXY(x, t->y);
        bumpLayout();
    }
    Q_INVOKABLE void setSpriteY(double y) {
        Target* t = ed.selected();
        if (!t || t->isStage) return;
        t->setXY(t->x, y);
        bumpLayout();
    }
    Q_INVOKABLE void setSpriteSize(double s) {
        Target* t = ed.selected();
        if (!t || t->isStage) return;
        t->size = std::clamp(s, 1.0, 1000.0);
        bumpLayout();
    }
    Q_INVOKABLE void setSpriteDirection(double d) {
        Target* t = ed.selected();
        if (!t || t->isStage) return;
        t->direction = d;
        bumpLayout();
    }
    Q_INVOKABLE void setSpriteVisible(bool v) {
        Target* t = ed.selected();
        if (!t || t->isStage) return;
        t->visible = v;
        bumpLayout();
    }
    Q_INVOKABLE QString spriteRotStyle() const {
        Target* t = const_cast<EdCore&>(ed).selected();
        if (!t) return {};
        return QString::fromStdString(t->rotationStyle);
    }
    Q_INVOKABLE void setSpriteRotStyle(const QString& s) {
        Target* t = ed.selected();
        if (!t || t->isStage) return;
        t->rotationStyle = s.toStdString();
        bumpLayout();
    }
    /// 新規スプライト（生成コスチューム付き、editor.cpp::addSprite 準拠）
    Q_INVOKABLE void addSprite() {
        if (!ed.rt) return;
        auto spr = std::make_unique<Sprite>(ed.rt);
        Sprite* sp = spr.get();
        ed.rt->ownedSprites.push_back(std::move(spr));
        int n = 1;
        for (Target* t : ed.rt->targets)
            if (!t->isStage) n++;
        sp->name = utf8ToU16("Sprite" + std::to_string(n));
        // 生成コスチューム（塗りつぶし円 + 目）
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
            c.assetId = "gen" + std::to_string(SDL_GetTicks());
            c.bitmapResolution = 1;
            c.rotationCenterX = 40;
            c.rotationCenterY = 30;
            c.skinId = 0;
            sp->costumes.push_back(c);
        }
        Target* nt = sp->createClone();
        nt->visible = true;
        nt->size = 100;
        nt->direction = 90;
        nt->draggable = true;
        nt->rotationStyle = "all around";
        nt->volume = 100;
        ed.rt->addTarget(nt);
        ed.selTarget = (int)ed.rt->targets.size() - 1;
        ed.clearHistory();
        ed.wsDirty_ = true;
        ed.flyDirty_ = true;
        bumpLayout();
        refreshCostumeThumbs();
    }
    bool canUndo() const { return !ed.undoStack.empty(); }
    bool canRedo() const { return !ed.redoStack.empty(); }
    // ---- フィールド編集 / ドロップダウンメニュー（QML オーバーレイ） ----
    bool fieldEditActive() const { return ed.fedit_.active; }
    QRectF fieldEditRect() const {
        if (!ed.fedit_.hasRect) return {};
        float s = ed.wsScale();
        return QRectF(ed.fedit_.rect.x * s + ed.panX - 2,
                      ed.fedit_.rect.y * s + ed.panY - 2,
                      ed.fedit_.rect.w * s + 6,
                      ed.fedit_.rect.h * s + 4);
    }
    QString fieldEditText() const { return qstr(ed.fedit_.buf); }
    void setFieldEditText(const QString& v) {
        ed.fedit_.buf = v.toStdString();
    }
    bool menuOpen() const { return ed.menu_.open; }
    QPointF menuPos() const { return QPointF(ed.menu_.x, ed.menu_.y); }
    QStringList menuItems() const {
        QStringList l;
        for (auto& s : ed.menu_.items) l << qstr(s);
        return l;
    }
    int menuChecked() const { return ed.menu_.checked; }
    QColor menuBg() const {
        return QColor(ed.menu_.bg.r, ed.menu_.bg.g, ed.menu_.bg.b,
                      ed.menu_.bg.a);
    }
    QColor menuBorder() const {
        return QColor(ed.menu_.border.r, ed.menu_.border.g,
                      ed.menu_.border.b, ed.menu_.border.a);
    }
    Q_INVOKABLE void fieldEditCommit(const QString& text) {
        if (!ed.fedit_.active) return;
        ed.fedit_.buf = text.toStdString();
        ed.applyEdit();
        bumpLayout();
    }
    Q_INVOKABLE void fieldEditCancel() {
        if (!ed.fedit_.active) return;
        ed.cancelEdit();
        bumpLayout();
    }
    Q_INVOKABLE void menuPick(int i) {
        if (!ed.menu_.open) return;
        ed.menuPick(i);
        bumpLayout();
    }
    Q_INVOKABLE void menuDismiss() {
        if (!ed.menu_.open) return;
        ed.menu_.open = false;
        bumpLayout();
    }

    void bumpLayout() {
        ++layoutRev_;
        Q_EMIT layoutChanged();
        Q_EMIT uiChanged();
    }

    // ---- QML → 操作 ----
    Q_INVOKABLE void save() {
        const std::string& dst =
            ed.savePath.empty() ? ed.path : ed.savePath;
        if (ed.save()) {
            std::fprintf(stderr, "editor: saved %s\n", dst.c_str());
            Q_EMIT saved();
        }
    }
    Q_INVOKABLE void undo() { ed.undo(); bumpLayout(); }
    Q_INVOKABLE void redo() { ed.redo(); bumpLayout(); }
    Q_INVOKABLE void deleteSelected() { ed.deleteSelectedBlock(); bumpLayout(); }
    Q_INVOKABLE void duplicateSelected() {
        if (!ed.selBlock.empty()) { ed.duplicateBlock(ed.selBlock); bumpLayout(); }
    }
    // ---- Costumes / Sounds タブ ----
    Q_INVOKABLE QVariantList costumeItems() const {
        QVariantList out;
        const Target* t = ed.rt && ed.selTarget >= 0 &&
                                  ed.selTarget < (int)ed.rt->targets.size()
                              ? ed.rt->targets[ed.selTarget]
                              : nullptr;
        if (!t || !t->sprite) return out;
        for (size_t i = 0; i < t->sprite->costumes.size(); ++i) {
            const Costume& c = t->sprite->costumes[i];
            QVariantMap m;
            m["name"] = QString::fromStdString(utf16ToUtf8(c.name));
            m["index"] = (int)i;
            m["current"] = (int)i == t->currentCostume;
            out.push_back(m);
        }
        return out;
    }
    Q_INVOKABLE QVariantList soundItems() const {
        QVariantList out;
        const Target* t = ed.rt && ed.selTarget >= 0 &&
                                  ed.selTarget < (int)ed.rt->targets.size()
                              ? ed.rt->targets[ed.selTarget]
                              : nullptr;
        if (!t || !t->sprite) return out;
        for (size_t i = 0; i < t->sprite->sounds.size(); ++i) {
            const Sound& s = t->sprite->sounds[i];
            QVariantMap m;
            m["name"] = QString::fromStdString(utf16ToUtf8(s.name));
            m["index"] = (int)i;
            m["secs"] = s.rate > 0 ? s.sampleCount / s.rate : 0.0;
            out.push_back(m);
        }
        return out;
    }
    // 波形包絡（scratch-gui wavEnvelope 準拠）: WAV を float に
    // デコードして ~120 バケットのピークを返す（QVariantList<double>）
    Q_INVOKABLE QVariantList soundEnvelope(int idx) const {
        QVariantList out;
        const Target* t = ed.rt && ed.selTarget >= 0 &&
                                  ed.selTarget < (int)ed.rt->targets.size()
                              ? ed.rt->targets[ed.selTarget]
                              : nullptr;
        if (!t || !t->sprite || idx < 0 ||
            idx >= (int)t->sprite->sounds.size())
            return out;
        const Sound& s = t->sprite->sounds[size_t(idx)];
        // WAV だけ簡易デコード（mp3 は sdl_audio の経路に任せる）
        if (s.data.size() < 44 ||
            !(s.data[0] == 'R' && s.data[1] == 'I' && s.data[2] == 'F' &&
              s.data[3] == 'F' && s.data[8] == 'W' && s.data[9] == 'A' &&
              s.data[10] == 'V' && s.data[11] == 'E'))
            return out;
        SDL_RWops* rw =
            SDL_RWFromConstMem(s.data.data(), (int)s.data.size());
        if (!rw) return out;
        SDL_AudioSpec spec;
        Uint8* buf = nullptr;
        Uint32 len = 0;
        if (!SDL_LoadWAV_RW(rw, 1, &spec, &buf, &len) || !buf) return out;
        int ch = spec.channels > 0 ? spec.channels : 1;
        size_t frames = 0;
        std::vector<float> pcm;
        if (spec.format == AUDIO_S16SYS) {
            frames = len / 2 / ch;
            pcm.resize(frames);
            const int16_t* p = (const int16_t*)buf;
            for (size_t i = 0; i < frames; ++i) {
                float acc = 0;
                for (int c = 0; c < ch; ++c)
                    acc = std::max(acc, std::fabs(float(p[i * ch + c])
                                                   / 32768.f));
                pcm[i] = acc;
            }
        } else if (spec.format == AUDIO_U8) {
            frames = len / ch;
            pcm.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                float acc = 0;
                for (int c = 0; c < ch; ++c)
                    acc = std::max(acc,
                                   std::fabs(float(buf[i * ch + c] - 128)
                                             / 128.f));
                pcm[i] = acc;
            }
        } else if (spec.format == AUDIO_F32LSB ||
                   spec.format == AUDIO_F32SYS) {
            frames = len / 4 / ch;
            pcm.resize(frames);
            const float* p = (const float*)buf;
            for (size_t i = 0; i < frames; ++i) {
                float acc = 0;
                for (int c = 0; c < ch; ++c)
                    acc = std::max(acc, std::fabs(p[i * ch + c]));
                pcm[i] = acc;
            }
        }
        SDL_FreeWAV(buf);
        if (frames == 0) return out;
        const int kBuckets = 120;
        for (int b = 0; b < kBuckets; ++b) {
            size_t i0 = size_t(b) * frames / kBuckets;
            size_t i1 = size_t(b + 1) * frames / kBuckets;
            if (i1 <= i0) i1 = i0 + 1;
            float pk = 0;
            for (size_t i = i0; i < i1 && i < frames; ++i)
                pk = std::max(pk, pcm[i]);
            out.push_back(double(pk));
        }
        return out;
    }
    Q_INVOKABLE void selectCostume(int i) {
        Target* t = ed.selected();
        if (!t || !t->sprite) return;
        t->setCostume(double(std::clamp(i, 0,
            (int)t->sprite->costumes.size() - 1)));
        refreshCostumeThumbs();
        Q_EMIT uiChanged();
    }
    Q_INVOKABLE void addCostume() {
        Target* t = ed.selected();
        if (!t || !t->sprite) return;
        ed.pushUndo();
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
        refreshCostumeThumbs();
        bumpLayout();
    }
    Q_INVOKABLE void duplicateCostume(int i) {
        Target* t = ed.selected();
        if (!t || !t->sprite ||
            i < 0 || i >= (int)t->sprite->costumes.size()) return;
        ed.pushUndo();
        Costume c = t->sprite->costumes[i];
        std::string bn = utf16ToUtf8(c.name);
        while (!bn.empty() && std::isdigit((unsigned char)bn.back()))
            bn.pop_back();
        auto exists = [&](const std::string& nm) {
            for (auto& cc : t->sprite->costumes)
                if (utf16ToUtf8(cc.name) == nm) return true;
            return false;
        };
        int n = 2;
        while (exists(bn + std::to_string(n))) ++n;
        c.name = utf8ToU16(bn + std::to_string(n));
        c.assetId.clear();  // 保存時に再採番
        t->sprite->costumes.push_back(c);
        refreshCostumeThumbs();
        bumpLayout();
    }
    Q_INVOKABLE void deleteCostume(int i) {
        Target* t = ed.selected();
        if (!t || !t->sprite || t->sprite->costumes.size() <= 1) return;
        if (i < 0 || i >= (int)t->sprite->costumes.size()) return;
        ed.pushUndo();
        t->sprite->costumes.erase(t->sprite->costumes.begin() + i);
        if (t->currentCostume >= (int)t->sprite->costumes.size())
            t->setCostume((double)t->sprite->costumes.size() - 1);
        refreshCostumeThumbs();
        bumpLayout();
    }
    Q_INVOKABLE void renameCostume(int i, const QString& nm) {
        Target* t = ed.selected();
        if (!t || !t->sprite ||
            i < 0 || i >= (int)t->sprite->costumes.size()) return;
        ed.pushUndo();
        t->sprite->costumes[i].name = utf8ToU16(nm.toStdString());
        bumpLayout();
    }
    Q_INVOKABLE void selectSound(int i) { ed.soundSel_ = i; Q_EMIT uiChanged(); }
    Q_INVOKABLE void playSound(int i) {
        Target* t = ed.selected();
        if (!t || !t->sprite || !t->sprite->soundBank) return;
        if (i < 0 || i >= (int)t->sprite->sounds.size()) return;
        t->sprite->soundBank->playSound(t, t->sprite->sounds[i].soundId);
    }
    Q_INVOKABLE void addSound() {
        Target* t = ed.selected();
        if (!t || !t->sprite) return;
        ed.pushUndo();
        int n = (int)t->sprite->sounds.size() + 1;
        Sound s2;
        s2.name = utf8ToU16("pop" + std::to_string(n));
        s2.dataFormat = "wav";
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
        s2.soundId = (int)t->sprite->sounds.size();
        t->sprite->sounds.push_back(std::move(s2));
        ed.soundSel_ = (int)t->sprite->sounds.size() - 1;
        bumpLayout();
    }
    Q_INVOKABLE void duplicateSound(int i) {
        Target* t = ed.selected();
        if (!t || !t->sprite ||
            i < 0 || i >= (int)t->sprite->sounds.size()) return;
        ed.pushUndo();
        Sound s2 = t->sprite->sounds[i];
        std::string bn = utf16ToUtf8(s2.name);
        while (!bn.empty() && std::isdigit((unsigned char)bn.back()))
            bn.pop_back();
        auto exists = [&](const std::string& nm) {
            for (auto& ss : t->sprite->sounds)
                if (utf16ToUtf8(ss.name) == nm) return true;
            return false;
        };
        int n = 2;
        while (exists(bn + std::to_string(n))) ++n;
        s2.name = utf8ToU16(bn + std::to_string(n));
        s2.assetId.clear();
        t->sprite->sounds.push_back(s2);
        ed.soundSel_ = (int)t->sprite->sounds.size() - 1;
        bumpLayout();
    }
    Q_INVOKABLE void deleteSound(int i) {
        Target* t = ed.selected();
        if (!t || !t->sprite ||
            i < 0 || i >= (int)t->sprite->sounds.size()) return;
        ed.pushUndo();
        t->sprite->sounds.erase(t->sprite->sounds.begin() + i);
        if (ed.soundSel_ >= (int)t->sprite->sounds.size())
            ed.soundSel_ = (int)t->sprite->sounds.size() - 1;
        bumpLayout();
    }
    Q_INVOKABLE void renameSound(int i, const QString& nm) {
        Target* t = ed.selected();
        if (!t || !t->sprite ||
            i < 0 || i >= (int)t->sprite->sounds.size()) return;
        ed.pushUndo();
        t->sprite->sounds[i].name = utf8ToU16(nm.toStdString());
        bumpLayout();
    }
    // ---- サウンドエフェクト（sound-editor.jsx の 9 ボタン相当）----
    // faster/slower/louder/softer/mute/fadein/fadeout/reverse/robot
    static bool decodePcmMono(const std::vector<uint8_t>& data,
                              std::vector<float>& out, int& rate) {
        if (data.size() < 44 ||
            !(data[0] == 'R' && data[1] == 'I' && data[2] == 'F' &&
              data[3] == 'F' && data[8] == 'W' && data[9] == 'A' &&
              data[10] == 'V' && data[11] == 'E'))
            return false;
        SDL_RWops* rw = SDL_RWFromConstMem(data.data(), (int)data.size());
        if (!rw) return false;
        SDL_AudioSpec spec;
        Uint8* buf = nullptr;
        Uint32 len = 0;
        if (!SDL_LoadWAV_RW(rw, 1, &spec, &buf, &len) || !buf) return false;
        int ch = spec.channels > 0 ? spec.channels : 1;
        if ((spec.format & 0xFF) == 0x10) {       // 16-bit signed
            size_t frames = len / 2 / ch;
            out.resize(frames);
            const int16_t* p = (const int16_t*)buf;
            for (size_t i = 0; i < frames; ++i) {
                float acc = 0;
                for (int c = 0; c < ch; ++c)
                    acc += float(p[i * ch + c]) / ch;
                out[i] = acc / 32768.f;
            }
        } else if (spec.format == AUDIO_U8) {
            size_t frames = len / ch;
            out.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                float acc = 0;
                for (int c = 0; c < ch; ++c)
                    acc += float(buf[i * ch + c] - 128) / ch;
                out[i] = acc / 128.f;
            }
        } else if ((spec.format & 0xFF) == 0x20) { // 32-bit float
            size_t frames = len / 4 / ch;
            out.resize(frames);
            const float* p = (const float*)buf;
            for (size_t i = 0; i < frames; ++i) {
                float acc = 0;
                for (int c = 0; c < ch; ++c) acc += p[i * ch + c];
                out[i] = acc / ch;
            }
        }
        SDL_FreeWAV(buf);
        rate = spec.freq;
        return !out.empty();
    }
    static std::vector<uint8_t> encodeWav16(const std::vector<float>& pcm,
                                            int rate) {
        const size_t N = pcm.size();
        std::vector<uint8_t> wav(44 + N * 2);
        auto w32 = [&](size_t o, uint32_t v) { std::memcpy(&wav[o], &v, 4); };
        auto w16 = [&](size_t o, uint16_t v) { std::memcpy(&wav[o], &v, 2); };
        std::memcpy(&wav[0], "RIFF", 4);
        w32(4, 36 + (uint32_t)N * 2);
        std::memcpy(&wav[8], "WAVEfmt ", 8);
        w32(16, 16);
        w16(20, 1); w16(22, 1); w32(24, (uint32_t)rate);
        w32(28, (uint32_t)rate * 2); w16(32, 2); w16(34, 16);
        std::memcpy(&wav[36], "data", 4);
        w32(40, (uint32_t)N * 2);
        for (size_t i = 0; i < N; ++i) {
            float v = std::clamp(pcm[i], -1.f, 1.f);
            int16_t s = (int16_t)(v * 32767.f);
            std::memcpy(&wav[44 + i * 2], &s, 2);
        }
        return wav;
    }
    Q_INVOKABLE void soundEffect(int idx, const QString& fx) {
        Target* t = ed.selected();
        if (!t || !t->sprite ||
            idx < 0 || idx >= (int)t->sprite->sounds.size()) return;
        Sound& s = t->sprite->sounds[size_t(idx)];
        std::vector<float> pcm;
        int rate = 0;
        if (!decodePcmMono(s.data, pcm, rate)) return;
        ed.pushUndo();
        const size_t n = pcm.size();
        const std::string f = fx.toStdString();
        if (f == "faster" || f == "slower") {
            // 1.5x / 0.67x — scratch-paint の speed up/down 相当
            double g = f == "faster" ? 1.5 : 0.67;
            size_t nn = size_t(n / g);
            std::vector<float> o(nn);
            for (size_t i = 0; i < nn; ++i) {
                double pos = i * g;
                size_t j = (size_t)pos;
                double fr = pos - j;
                o[i] = j + 1 < n
                           ? pcm[j] * float(1 - fr) + pcm[j + 1] * float(fr)
                           : pcm[std::min(j, n - 1)];
            }
            pcm = std::move(o);
        } else if (f == "louder") {
            for (auto& v : pcm) v = std::clamp(v * 1.25f, -1.f, 1.f);
        } else if (f == "softer") {
            for (auto& v : pcm) v *= 0.8f;
        } else if (f == "mute") {
            std::fill(pcm.begin(), pcm.end(), 0.f);
        } else if (f == "fadein") {
            for (size_t i = 0; i < n; ++i)
                pcm[i] *= std::min(1.0f, float(i) * 2 / n);
        } else if (f == "fadeout") {
            for (size_t i = 0; i < n; ++i)
                pcm[i] *= std::min(1.0f, float(n - i) * 2 / n);
        } else if (f == "reverse") {
            std::reverse(pcm.begin(), pcm.end());
        } else if (f == "robot") {
            // 50 Hz 矩形波 AM（scratch-paint の robot effect 相当）
            for (size_t i = 0; i < n; ++i) {
                double ph = std::fmod(i * 50.0 / rate, 1.0);
                pcm[i] *= (ph < 0.5) ? 1.f : 0.15f;
            }
        }
        s.data = encodeWav16(pcm, rate);
        s.dataFormat = "wav";
        s.sampleCount = (double)pcm.size();
        s.rate = rate;
        s.md5.clear();   // 保存時に assetId 由来の新名を使う
        s.assetId = "fx" + std::to_string(SDL_GetTicks());
        bumpLayout();
    }
    // ---- Convert to Bitmap（paint-editor のボタン相当）----
    // costumePixels の RGBA8 を zlib で PNG 化して data へ差し替える。
    static bool encodePng(const uint8_t* rgba, int w, int h,
                          std::vector<uint8_t>& out) {
        if (w <= 0 || h <= 0) return false;
        std::vector<uint8_t> raw(size_t(h) * (size_t(w) * 4 + 1));
        for (int y = 0; y < h; ++y) {
            raw[y * (w * 4 + 1)] = 0;  // filter: none
            std::memcpy(raw.data() + y * (w * 4 + 1) + 1,
                        rgba + size_t(y) * w * 4, size_t(w) * 4);
        }
        uLongf clen = compressBound((uLong)raw.size());
        std::vector<uint8_t> cbuf(clen);
        if (compress2(cbuf.data(), &clen, raw.data(),
                      (uLong)raw.size(), 6) != Z_OK)
            return false;
        cbuf.resize(clen);
        auto u32 = [&](uint32_t v) {
            out.push_back(uint8_t(v >> 24)); out.push_back(uint8_t(v >> 16));
            out.push_back(uint8_t(v >> 8));  out.push_back(uint8_t(v));
        };
        auto chunk = [&](const char* tag, const uint8_t* d, size_t n) {
            u32((uint32_t)n);
            size_t at = out.size();
            out.push_back((uint8_t)tag[0]); out.push_back((uint8_t)tag[1]);
            out.push_back((uint8_t)tag[2]); out.push_back((uint8_t)tag[3]);
            out.insert(out.end(), d, d + n);
            uint32_t c = crc32(0, out.data() + at, (uInt)(4 + n));
            u32(c);
        };
        static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
        out.insert(out.end(), sig, sig + 8);
        uint8_t ihdr[13];
        ihdr[0] = uint8_t(w >> 24); ihdr[1] = uint8_t(w >> 16);
        ihdr[2] = uint8_t(w >> 8);  ihdr[3] = uint8_t(w);
        ihdr[4] = uint8_t(h >> 24); ihdr[5] = uint8_t(h >> 16);
        ihdr[6] = uint8_t(h >> 8);  ihdr[7] = uint8_t(h);
        ihdr[8] = 8; ihdr[9] = 6;  // 8bit RGBA
        ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
        chunk("IHDR", ihdr, 13);
        chunk("IDAT", cbuf.data(), cbuf.size());
        chunk("IEND", nullptr, 0);
        return true;
    }
    Q_INVOKABLE bool convertCostumeToBitmap(int idx) {
        Target* t = ed.selected();
        if (!t || !t->sprite ||
            idx < 0 || idx >= (int)t->sprite->costumes.size())
            return false;
        Costume& c = t->sprite->costumes[size_t(idx)];
        if (c.dataFormat != "svg") return false;   // 既にビットマップ
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!ren.costumePixels(&c, px, &w, &h)) return false;
        std::vector<uint8_t> png;
        if (!encodePng(px.data(), w, h, png)) return false;
        ed.pushUndo();
        c.data = std::move(png);
        c.dataFormat = "png";
        c.bitmapResolution = 2;
        c.md5.clear();
        c.assetId = "png" + std::to_string(SDL_GetTicks());
        refreshCostumeThumbs();
        bumpLayout();
        return true;
    }
    Q_INVOKABLE int costumeSel() const {
        const Target* t = ed.rt && ed.selTarget >= 0 &&
                                  ed.selTarget < (int)ed.rt->targets.size()
                              ? ed.rt->targets[ed.selTarget]
                              : nullptr;
        return t ? t->currentCostume : 0;
    }
    Q_INVOKABLE int soundSel() const { return ed.soundSel_; }
    /// コスチュームタブ用サムネイルを作り直す（target 単位）。
    void refreshCostumeThumbs();  // provider へのアクセスのため main 側で実装

    // ---- Make a Block ダイアログ ----
    Q_INVOKABLE bool mbOpen() const { return ed.mb_.open; }
    Q_INVOKABLE QVariantList mbParts() const {
        QVariantList out;
        for (const auto& p : ed.mb_.parts) {
            QVariantMap m;
            m["kind"] = (int)p.kind;      // 0=text 1=str 2=bool
            m["text"] = QString::fromStdString(p.text);
            out.push_back(m);
        }
        return out;
    }
    Q_INVOKABLE bool mbWarp() const { return ed.mb_.warp; }
    Q_INVOKABLE void mbSetWarp(bool v) { ed.mb_.warp = v; bumpLayout(); }
    Q_INVOKABLE void mbAddPart(int kind) {
        if (!ed.mb_.open || kind < 0 || kind > 2) return;
        QString dflt = kind == 0 ? "label text"
                       : kind == 1 ? "string or number" : "boolean";
        ed.mb_.parts.push_back(
            {(EdCore::MakeBlockDlg::PartKind)kind,
             dflt.toStdString()});
        bumpLayout();
    }
    Q_INVOKABLE void mbRemovePart(int i) {
        if (!ed.mb_.open || i < 0 || i >= (int)ed.mb_.parts.size()) return;
        ed.mb_.parts.erase(ed.mb_.parts.begin() + i);
        bumpLayout();
    }
    Q_INVOKABLE void mbSetPartText(int i, const QString& tx) {
        if (!ed.mb_.open || i < 0 || i >= (int)ed.mb_.parts.size()) return;
        ed.mb_.parts[i].text = tx.toStdString();
    }
    Q_INVOKABLE void mbOk() {
        ed.mb_.open = false;
        ed.makeProcedure();
        bumpLayout();
    }
    Q_INVOKABLE void mbCancel() { ed.mb_.open = false; Q_EMIT uiChanged(); }

    Q_INVOKABLE void cleanupBlocks() {
        Target* t = ed.selected();
        if (!t || !t->blocks) return;
        ed.pushUndo();
        ed.rebuildWs();
        std::unordered_map<std::string, const bl::LBlock*> topH;
        for (auto& lb : ed.wsLayout_.blocks)
            if (lb.depth == 0 && !lb.shadow) topH[lb.id] = &lb;
        float y = 10;
        {
            Blocks::StructuralEdit edit(*t->blocks);
            for (const std::string& id : t->blocks->getScripts()) {
                if (Block* b = t->blocks->getBlock(id)) {
                    b->x = 10;
                    b->y = y;
                    auto it = topH.find(id);
                    y += (it != topH.end() ? it->second->fullH : 80.f) + 40;
                }
            }
        }
        ed.wsDirty_ = true;
        bumpLayout();
    }
    Q_INVOKABLE void deleteSprite(int i) { ed.deleteSprite(i); bumpLayout(); }
    Q_INVOKABLE void duplicateSprite(int i) {
        ed.duplicateSprite(i);
        bumpLayout();
        refreshCostumeThumbs();
    }
    Q_INVOKABLE bool exportSprite(int i) { return ed.exportSprite(i); }
    /// スプライトタイルのドラッグ並び替え。
    /// ti = target index、ins = 非 stage タイル上の挿入位置（0..n）。
    Q_INVOKABLE void spriteReorder(int ti, int ins) {
        ed.spriteReorder(ti, ins);
        bumpLayout();
        refreshCostumeThumbs();
    }
    // ---- Add Extension ----
    Q_INVOKABLE QStringList enabledExtensions() const {
        QStringList l;
        for (auto& s : const_cast<EdCore&>(ed).enabledExtensions())
            l << QString::fromStdString(s);
        return l;
    }
    Q_INVOKABLE void addExtension(const QString& id) {
        ed.addExtension(id.toStdString());
        bumpLayout();
    }
    // ---- 設定永続化（scratch-gui の localStorage 相当:
    // stageMode / zoom / catSel を QSettings へ）----
    Q_INVOKABLE int loadStageMode() const {
        return settings_.value("stageMode", 0).toInt();
    }
    Q_INVOKABLE void persistStageMode(int m) {
        settings_.setValue("stageMode", m);
        settings_.sync();
    }
    Q_INVOKABLE double loadZoom() const {
        return settings_.value("zoom", 1.0).toDouble();
    }
    Q_INVOKABLE void persistZoom() {
        settings_.setValue("zoom", (double)ed.zoom);
        settings_.sync();
    }
    Q_INVOKABLE int loadCategory() const {
        return settings_.value("catSel", 0).toInt();
    }
    Q_INVOKABLE void persistCategory() {
        settings_.setValue("catSel", ed.catSel);
        settings_.sync();
    }

    // ---- 右クリックコンテキストメニュー ----
    /// ws 上で右クリック: メニュー項目を構築（QML が ctxItems() で表示）。
    Q_INVOKABLE void openWsContextMenu(double x, double y) {
        ed.buildWsContext(float(x), float(y));
        bumpLayout();
    }
    /// メニュー項目 {label, enabled} 一覧
    Q_INVOKABLE QVariantList ctxItems() const { return ed.ctxItems(); }
    /// メニュー項目実行（index は ctxItems のもの）。
    /// px/py は paste 位置計算用の ws ビューサイズ。
    Q_INVOKABLE void runCtxItem(int i, double viewW, double viewH) {
        if (i < 0 || i >= (int)ed.ctx_.size()) return;
        const EdCore::CtxItem& it = ed.ctx_[i];
        if (!it.enabled) return;
        const QString& a = it.act;
        const std::string bid = ed.ctxBlockId_;
        if (a == "dup") ed.duplicateBlock(bid);
        else if (a == "del") ed.deleteBlockChain(bid);
        else if (a == "delall") ed.deleteAllBlocks();
        else if (a == "cleanup") cleanupBlocks();
        else if (a == "undo") undo();
        else if (a == "redo") redo();
        else if (a == "backpack") { ed.backpackAdd(bid); ed.bpOpen = true; }
        else if (a == "paste")
            ed.backpackPaste(0, float(viewW), float(viewH));
        else if (a == "addcomment") { ed.startEditingComment(bid); }
        else if (a == "rmcomment") { ed.removeComment(bid); }
        bumpLayout();
    }

    // ---- コメント編集 ----
    Q_INVOKABLE bool commentActive() const { return ed.cmt_.active; }
    Q_INVOKABLE QString commentText() const { return ed.commentText(); }
    Q_INVOKABLE void commentCommit(const QString& text) {
        ed.commitComment(text);
        bumpLayout();
    }
    Q_INVOKABLE void commentCancel() {
        ed.cmt_.active = false;
        ed.cmt_.blockId.clear();
        bumpLayout();
    }

    // ---- バックパック ----
    Q_INVOKABLE QVariantList backpackItems() const {
        QVariantList l;
        for (auto& it : ed.backpack) {
            QVariantMap m;
            m["label"] = QString::fromStdString(it.label);
            l.push_back(m);
        }
        return l;
    }
    Q_INVOKABLE bool backpackOpen() const { return ed.bpOpen; }
    Q_INVOKABLE void toggleBackpack() { ed.bpOpen = !ed.bpOpen; bumpLayout(); }
    Q_INVOKABLE void backpackPasteAt(int i, double viewW, double viewH) {
        ed.backpackPaste(i, float(viewW), float(viewH));
        bumpLayout();
    }
    Q_INVOKABLE int backpackCount() const { return (int)ed.backpack.size(); }

    Q_INVOKABLE void greenFlag() {
        if (rt) rt->greenFlag();
    }
    Q_INVOKABLE void stopAll() {
        if (rt) rt->stopAll();
    }
    // ---- ワークスペース入力（ビュー座標 px）----
    Q_INVOKABLE void wsMousePress(double x, double y) {
        wsPress_ = {x, y};
        wsPanning_ = false;
        float s = ed.wsScale();
        float lx = (x - ed.panX) / s, ly = (y - ed.panY) / s;
        bl::LBlock* lb = ed.wsBlockAtLogical(lx, ly);
        if (lb) {
            ed.selBlock = lb->id;
            ed.drag.pending = true;
            ed.drag.fromFlyout = false;
            ed.drag.headId = lb->id;
            ed.drag.offX = lx - lb->x;
            ed.drag.offY = ly - lb->y;
            ed.drag.mx = lx; ed.drag.my = ly;
        } else {
            ed.selBlock.clear();
            wsPanning_ = true;
            panStart_ = {x, y};
            panOrig_ = {ed.panX, ed.panY};
        }
        bumpLayout();
    }
    Q_INVOKABLE void wsMouseMove(double x, double y) {
        float s = ed.wsScale();
        float lx = (x - ed.panX) / s, ly = (y - ed.panY) / s;
        if (ed.drag.pending && !ed.drag.fromFlyout && !ed.drag.active) {
            float dx = float(x) - float(wsPress_.x());
            float dy = float(y) - float(wsPress_.y());
            if (dx * dx + dy * dy > 16) {
                ed.pushUndo();
                ed.drag.snapshotTaken = true;
                ed.drag.active = true;
                ed.detachDrag();
            }
        }
        if (ed.drag.active) {
            dragVx_ = float(x); dragVy_ = float(y);
            if (ed.drag.fromFlyout) {
                // QML が ws ローカル座標で呼ぶ（ゴースト位置は mx/my）
                ed.drag.mx = lx;
                ed.drag.my = ly;
            } else {
                ed.drag.mx = lx - ed.drag.offX;
                ed.drag.my = ly - ed.drag.offY;
            }
            ed.updateDropTarget();
            bumpLayout();
            return;
        }
        if (wsPanning_) {
            ed.panX = float(panOrig_.x()) + (float(x) - float(panStart_.x()));
            ed.panY = float(panOrig_.y()) + (float(y) - float(panStart_.y()));
            bumpLayout();
        }
    }
    /// ws ビュー外（ツールボックス/フライアウト）で解放 = 削除ドロップ。
    Q_INVOKABLE void wsDropDelete() {
        dragVx_ = dragVy_ = -1;
        if (ed.drag.active) {
            if (ed.drag.fromFlyout) {
                if (ed.drag.snapshotTaken) { ed.popUndo(); ed.drag.snapshotTaken = false; }
                ed.drag.active = false;
                ed.drag.pending = false;
            } else {
                ed.deleteBlockChain(ed.drag.headId);
                ed.drag.active = false;
                ed.drag.pending = false;
                ed.drag.snapshotTaken = false;
                ed.drag.connectionsReady = false;
            }
            bumpLayout();
        }
    }
    /// 削除ゾーン矩形（ws ビュー座標）。
    Q_INVOKABLE QVariantList trashRect() {
        if (!(ed.drag.active && !ed.drag.fromFlyout)) return {};
        return {ed.wsW - 160, ed.wsH - 56, 56.0, 44.0};
    }
    Q_INVOKABLE void wsMouseRelease(double x, double y) {
        dragVx_ = dragVy_ = -1;
        if (ed.drag.active) {
            // 削除ゾーン上へのドロップ = 削除（ws ドラッグのみ）
            QVariantList tr = trashRect();
            bool inTrash = tr.size() == 4 &&
                hit(tr[0].toDouble(), tr[1].toDouble(),
                    tr[2].toDouble(), tr[3].toDouble(), x, y);
            if (inTrash) {
                ed.deleteBlockChain(ed.drag.headId);
                ed.drag.active = false;
                ed.drag.pending = false;
                ed.drag.snapshotTaken = false;
                ed.drag.connectionsReady = false;
            } else {
                ed.doDrop();
            }
            bumpLayout();
            return;
        }
        if (ed.drag.pending) {
            ed.drag.pending = false;
            if (ed.drag.fromFlyout) {
                // フライアウトから ws 内で移動せず解放 = その位置に配置
                ed.drag.active = true;
                ed.drag.snapshotTaken = true;
                ed.pushUndo();
                float s = ed.wsScale();
                ed.drag.mx = float((x - ed.panX) / s);
                ed.drag.my = float((y - ed.panY) / s);
                ed.updateDropTarget();
                ed.doDrop();
            } else {
                // クリック: フィールド箱なら編集/ドロップダウン、
                // そうでなければトップブロック実行
                float s = ed.wsScale();
                float lx = float((x - ed.panX) / s);
                float ly = float((y - ed.panY) / s);
                ed.rebuildWs();
                bl::LBlock* lb = ed.wsBlockAtLogical(lx, ly);
                bool consumed = false;
                if (lb) {
                    if (const bl::LSeg* g =
                            lb->segAt(lx - lb->x, ly - lb->y)) {
                        if (std::getenv("XYO_EDIT_DEBUG"))
                            std::fprintf(stderr,
                                         "[edqt] seg hit kind=%d "
                                         "g=%.0f,%.0f %.0fx%.0f\n",
                                         (int)g->kind, g->x, g->y,
                                         g->w, g->h);
                        consumed = ed.clickSeg(*lb, *g, float(x), float(y));
                    }
                }
                if (!consumed) runTopAt(lx, ly);
            }
            bumpLayout();
            return;
        }
        wsPanning_ = false;
    }
    /// Esc: ドラッグを始める前の状態へ巻き戻す。ws ドラッグは開始時に
    /// pushUndo 済みなので undo() で復元し redo スタックを捨てる。
    /// フライアウト発はまだ ws に挿入していないので捨てるだけ。
    Q_INVOKABLE void cancelDrag() {
        if (ed.drag.active) {
            if (ed.drag.snapshotTaken) {
                if (ed.drag.fromFlyout) ed.popUndo();
                else { ed.undo(); ed.redoStack.clear(); }
                ed.drag.snapshotTaken = false;
            }
            ed.drag.active = false;
            ed.drag.pending = false;
            ed.drag.connectionsReady = false;
            dragVx_ = dragVy_ = -1;
            bumpLayout();
        } else if (ed.drag.pending) {
            ed.drag.pending = false;
            bumpLayout();
        }
    }
    /// ホバーカーソル（Qt::CursorShape。ブロック上=手、フィールド上=I-beam
    /// 等）。x,y は ws ビュー座標。
    Q_INVOKABLE int wsCursor(double x, double y) {
        if (ed.drag.active) return Qt::ClosedHandCursor;
        float s = ed.wsScale();
        float lx = (float(x) - ed.panX) / s, ly = (float(y) - ed.panY) / s;
        if (ed.wsDirty_) ed.rebuildWs();
        bl::LBlock* lb = ed.wsBlockAtLogical(lx, ly);
        if (!lb) return Qt::ArrowCursor;
        if (const bl::LSeg* g = lb->segAt(lx - lb->x, ly - lb->y)) {
            switch (g->kind) {
            case bl::SegKind::NumField:
            case bl::SegKind::StrField:
                return Qt::IBeamCursor;
            case bl::SegKind::Dropdown:
            case bl::SegKind::Variable:
            case bl::SegKind::Swatch:
                return Qt::PointingHandCursor;
            default: break;
            }
        }
        return Qt::OpenHandCursor;
    }
    /// フライアウト上のホバーカーソル。
    Q_INVOKABLE int flyCursor(double x, double y, double viewW) {
        if (ed.drag.active) return Qt::ClosedHandCursor;
        if (EdCore::FlyItem* fi =
                ed.flyItemAt(float(x), float(y), float(viewW))) {
            if (fi->kind == EdCore::FlyItem::Button) return Qt::PointingHandCursor;
            return Qt::OpenHandCursor;
        }
        return Qt::ArrowCursor;
    }
    /// 現在のワークスペースズーム（% 表示用）。
    Q_INVOKABLE double zoomLevel() const { return ed.zoom; }
    Q_INVOKABLE void wsWheel(double x, double y, double dy) {
        float old = ed.wsScale();
        ed.zoom = std::clamp(ed.zoom * (dy > 0 ? 1.1f : 1 / 1.1f),
                             0.3f, 3.0f);
        float s = ed.wsScale();
        ed.panX = float(x) - (float(x) - ed.panX) * s / old;
        ed.panY = float(y) - (float(y) - ed.panY) * s / old;
        persistZoom();
        bumpLayout();
    }
    Q_INVOKABLE void zoomBy(double f) {
        ed.zoom = std::clamp(ed.zoom * float(f), 0.3f, 3.0f);
        persistZoom();
        bumpLayout();
    }
    Q_INVOKABLE void zoomReset() { ed.zoom = 1.0f; bumpLayout(); }

    // ---- フライアウト入力 ----
    Q_INVOKABLE void flyMousePress(double x, double y, double viewW) {
        if (EdCore::FlyItem* fi = ed.flyItemAt(float(x), float(y), float(viewW))) {
            if (fi->kind == EdCore::FlyItem::Button) {
                if (fi->makeBlock) {
                    ed.mb_.reset();
                    bumpLayout();
                } else {
                    ed.addVariable(fi->list);
                    bumpLayout();
                }
                return;
            }
            if (fi->kind == EdCore::FlyItem::VarRow ||
                fi->kind == EdCore::FlyItem::ListRow) {
                // チェックボックス部（左 14px、行頭）ならモニタ切替
                float sy = fi->y * bl::kWsZoom - ed.flyScroll;
                if (x >= 14 && x < 28 && y >= sy + 10 && y < sy + 24) {
                    ed.toggleMonitor(fi->varId, fi->text);
                    bumpLayout();
                    return;
                }
                // 変数記者としてドラッグ（lb は rebuildFlyout で計測済み）
                ed.drag = EdCore::Drag{};
                ed.drag.pending = true;
                ed.drag.fromFlyout = true;
                ed.drag.flyItem = fi;
                ed.drag.opcode = fi->opcode;
                ed.drag.initFields = fi->initFields;
                ed.drag.shape = edit::Shape::Reporter;
                ed.drag.tmpl = fi->lb;
                ed.drag.tmplSt = fi->st;
                return;
            }
            if (fi->kind == EdCore::FlyItem::BlockT) {
                ed.drag.pending = true;
                ed.drag.fromFlyout = true;
                ed.drag.flyItem = fi;
                ed.drag.opcode = fi->opcode;
                ed.drag.initFields = fi->initFields;
                ed.drag.mutation = fi->mutation;
                ed.drag.argTexts = fi->argTexts;
                ed.drag.tmpl = fi->lb;
                ed.drag.tmplSt = fi->st;
                ed.drag.shape = fi->st.shape;
            }
        }
    }
    /// フライアウト発ドラッグ中にポインタが ws ビュー上にある間の位置更新
    /// （QML が ws ローカル座標に変換して渡す。x,y は ws ビュー座標）。
    Q_INVOKABLE void wsDragMove(double x, double y) {
        if (!ed.drag.fromFlyout) return;
        if (ed.drag.pending && !ed.drag.active) {
            ed.drag.active = true;
            ed.drag.snapshotTaken = true;
            ed.pushUndo();
        }
        if (ed.drag.active) {
            dragVx_ = float(x); dragVy_ = float(y);
            float s = ed.wsScale();
            ed.drag.mx = float((x - ed.panX) / s);
            ed.drag.my = float((y - ed.panY) / s);
            ed.updateDropTarget();
            bumpLayout();
        }
    }
    /// フライアウト上で解放（=キャンセル。ws 内は wsMouseRelease が拾う）。
    Q_INVOKABLE void flyMouseRelease() {
        dragVx_ = dragVy_ = -1;
        if (ed.drag.active && ed.drag.fromFlyout) {
            if (ed.drag.snapshotTaken) { ed.popUndo(); ed.drag.snapshotTaken = false; }
            ed.drag.active = false;
            ed.drag.pending = false;
            bumpLayout();
        } else if (ed.drag.pending) {
            ed.drag.pending = false;
            ed.drag.active = false;
        }
    }
    Q_INVOKABLE void flyWheel(double dy, double viewH) {
        float ch = ed.flyContentH() * bl::kWsZoom;
        float mx = std::max(0.f, ch - float(viewH));
        ed.flyScroll = std::clamp(ed.flyScroll - float(dy), 0.f, mx);
        bumpLayout();
    }

    // ---- ステージ ----
    Q_INVOKABLE void setStageDisplay(double w, double h) {
        stageDispW = std::max(1.f, float(w));
        stageDispH = std::max(1.f, float(h));
    }
    // ---- ステージ入力（QML 座標 → テクスチャ座標 → SDL_Event）----
    Q_INVOKABLE void stageMouseDown(double x, double y) {
        SDL_Event ev{};
        ev.type = SDL_MOUSEBUTTONDOWN;
        ev.button.button = SDL_BUTTON_LEFT;
        ev.button.x = int(x * stageTexW / stageDispW);
        ev.button.y = int(y * stageTexH / stageDispH);
        mouseDown_ = true;
        ren.handleEvent(ev);
    }
    Q_INVOKABLE void stageMouseMove(double x, double y) {
        SDL_Event ev{};
        ev.type = SDL_MOUSEMOTION;
        ev.motion.x = int(x * stageTexW / stageDispW);
        ev.motion.y = int(y * stageTexH / stageDispH);
        ev.motion.state = mouseDown_ ? SDL_BUTTON_LMASK : 0;
        ren.handleEvent(ev);
    }
    Q_INVOKABLE void stageMouseUp(double x, double y) {
        SDL_Event ev{};
        ev.type = SDL_MOUSEBUTTONUP;
        ev.button.button = SDL_BUTTON_LEFT;
        ev.button.x = int(x * stageTexW / stageDispW);
        ev.button.y = int(y * stageTexH / stageDispH);
        mouseDown_ = false;
        ren.handleEvent(ev);
    }
    Q_INVOKABLE void stageKey(int sdlKey, bool down) {
        SDL_Event ev{};
        ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        ev.key.keysym.sym = (SDL_Keycode)sdlKey;
        ren.handleEvent(ev);
    }
    /// Qt のキーを SDL_Keycode へ変換して stageKey へ回す（QML 用）。
    Q_INVOKABLE void keyEvent(int qtKey, bool down) {
        int sdl = 0;
        if (qtKey >= 0x41 && qtKey <= 0x5a)            // Key_A..Key_Z
            sdl = 'a' + (qtKey - 0x41);
        else if (qtKey >= 0x30 && qtKey <= 0x39)       // Key_0..9
            sdl = qtKey;
        else switch (qtKey) {
            case 0x20: sdl = SDLK_SPACE; break;          // Key_Space
            case 0x01000013: sdl = SDLK_UP; break;       // Key_Up
            case 0x01000015: sdl = SDLK_DOWN; break;     // Key_Down
            case 0x01000012: sdl = SDLK_LEFT; break;     // Key_Left
            case 0x01000014: sdl = SDLK_RIGHT; break;    // Key_Right
            case 0x01000004: sdl = SDLK_RETURN; break;   // Key_Return
            case 0x01000005: sdl = SDLK_RETURN; break;   // Key_Enter
            default: return;
        }
        stageKey(sdl, down);
    }

    // ---- 毎フレーム ----
    Q_INVOKABLE void tick() {
        if (rt) {
            rt->step();
            if (!rt->threads.empty()) bumpLayout();
        }
        dragEdgeScroll();
        renderStage();
    }
    /// ブロックドラッグ中にポインタが ws 端に近づくと自動でパンする
    /// （scratch-blocks の auto-scroll 相当）。ポインタを論理座標で不動に
    /// 保つため mx/my へ逆分を足す。ws 外（ツールボックス方向）では動かない。
    void dragEdgeScroll() {
        if (!ed.drag.active) return;
        if (dragVx_ < 0 || dragVy_ < 0) return;
        if (dragVx_ >= ed.wsW || dragVy_ >= ed.wsH) return;
        const float kEdge = 32.f, kStep = 12.f;
        float dx = 0, dy = 0;
        if (dragVx_ < kEdge) dx = -kStep;
        else if (dragVx_ > ed.wsW - kEdge) dx = kStep;
        if (dragVy_ < kEdge) dy = -kStep;
        else if (dragVy_ > ed.wsH - kEdge) dy = kStep;
        if (!dx && !dy) return;
        ed.panX += dx;
        ed.panY += dy;
        float s = ed.wsScale();
        ed.drag.mx -= dx / s;
        ed.drag.my -= dy / s;
        ed.updateDropTarget();
        bumpLayout();
    }
    void renderStage() {
        if (!sdlR || !rt) return;
        int wantW = std::max(1, (int)(stageDispW * 2));
        int wantH = std::max(1, (int)(stageDispH * 2));
        if (!stageTex || stageTexW != wantW || stageTexH != wantH) {
            if (stageTex) SDL_DestroyTexture(stageTex);
            stageTex = SDL_CreateTexture(sdlR, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_TARGET,
                                         wantW, wantH);
            stageTexW = wantW; stageTexH = wantH;
        }
        if (!stageTex) return;
        SDL_SetRenderTarget(sdlR, stageTex);
        SDL_SetRenderDrawColor(sdlR, 255, 255, 255, 255);
        SDL_RenderClear(sdlR);
        ren.setStageRect(IRect{0, 0, stageTexW, stageTexH});
        ren.draw();
        // RenderReadPixels は「現在のレンダーターゲット」を読む — ターゲットを
        // 解除する前に読まないとウィンドウのバックバッファを拾う。
        QImage img(stageTexW, stageTexH, QImage::Format_ARGB32);
        bool ok = SDL_RenderReadPixels(sdlR, nullptr,
                                       SDL_PIXELFORMAT_ARGB8888,
                                       img.bits(), stageTexW * 4) == 0;
        SDL_SetRenderTarget(sdlR, nullptr);
        if (ok) {
            stageImg =
                img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            stageImgValid = true;
        } else {
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr, "editor-qt: ReadPixels: %s\n",
                             SDL_GetError());
            }
        }
    }

    void runTopAt(float lx, float ly) {
        Target* t = ed.selected();
        if (!t || !t->blocks || !rt) return;
        if (bl::LBlock* lb = ed.wsBlockAtLogical(lx, ly)) {
            std::string top = lb->id;
            for (int i = 0; i < 10000; ++i) {
                std::string p = bl::prevOf(t->blocks, top);
                if (!p.empty()) { top = p; continue; }
                Block* b = t->blocks->getBlock(top);
                if (b && !b->parent.empty()) { top = b->parent; continue; }
                break;
            }
            rt->toggleScript(top, t, true);
        }
    }

    QPointF wsPress_;
    bool wsPanning_ = false;
    QPointF panStart_, panOrig_;
    /// ドラッグ中ポインタの ws ビュー座標（エッジスクロール用。未使用は -1）
    float dragVx_ = -1, dragVy_ = -1;

Q_SIGNALS:
    void uiChanged();
    void layoutChanged();
    void saved();   // 保存完了（QML の toast 用）
};

// ---------------------------------------------------------------------------
// BlockCanvas — ワークスペースまたはフライアウトを描く QQuickPaintedItem
// ---------------------------------------------------------------------------
class BlockCanvas : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(EditorController* ctrl READ ctrl WRITE setCtrl)
    Q_PROPERTY(bool flyout READ flyout WRITE setFlyout)
public:
    BlockCanvas(QQuickItem* parent = nullptr) : QQuickPaintedItem(parent) {
        setAntialiasing(true);
        setOpaquePainting(true);
        setFillColor(QColor(0xF9, 0xF9, 0xF9));
    }
    EditorController* ctrl() const { return ctrl_; }
    void setCtrl(EditorController* c) {
        ctrl_ = c;
        if (c) connect(c, &EditorController::layoutChanged,
                       this, [this]() { update(); });
    }
    bool flyout() const { return flyout_; }
    void setFlyout(bool v) { flyout_ = v; }

    void paint(QPainter* p) override {
        if (!ctrl_) return;
        EdCore& ed = ctrl_->ed;
        ed.wsW = width(); ed.wsH = height();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setRenderHint(QPainter::SmoothPixmapTransform, true);
        QpCanvas cv(p, &ctrl_->tc_, nullptr);
        ctrl_->tc_.p = p;   // テキスト描画はこのペインタへ
        bl::setCanvas(&cv);
        if (flyout_) drawFlyout(p, cv);
        else drawWs(p, cv);
        bl::setCanvas(nullptr);
        ctrl_->tc_.p = nullptr;
    }

    void drawFlyout(QPainter* p, QpCanvas& cv) {
        EdCore& ed = ctrl_->ed;
        p->fillRect(0, 0, width(), height(), QColor(0xF9, 0xF9, 0xF9));
        p->setPen(QColor(0, 0, 0, 38));
        p->drawLine(QPointF(width() - 0.5, 0), QPointF(width() - 0.5, height()));
        if (ed.flyDirty_ || ed.flyCat_ != ed.catAt(ed.catSel).id ||
            ed.flyTarget_ != ed.selTarget) {
            ed.flyDirty_ = true;
            ed.rebuildFlyout();
        }
        cv.pushClip(0, 0, width(), height(), false);
        for (auto& f : ed.flyItems_) {
            float sy = f.y * bl::kWsZoom - ed.flyScroll;
            if (sy > height() || sy + f.h * bl::kWsZoom < 0) continue;
            switch (f.kind) {
            case EdCore::FlyItem::BlockT: {
                float bx = 10;
                bl::LBlock lb = f.lb;
                lb.x = 0; lb.y = 0;
                bl::drawLBlock(nullptr, lb, nullptr, ed.tr(bl::kWsZoom), f.st,
                               bx / bl::kWsZoom, sy / bl::kWsZoom,
                               bl::kWsZoom, false, false, 1.f);
                break;
            }
            case EdCore::FlyItem::Header:
                ctrl_->tc_.draw(12, sy + 2, f.text, 19, true, kFg);
                break;

            case EdCore::FlyItem::Button: {
                float bw = ctrl_->tc_.measure(f.text, 13, true) + 80;
                if (bw > width() - 24) bw = width() - 24;
                float bx = (width() - bw) / 2;
                cv.strokeRoundRect(bx, sy, bw, 40, 4, rgb(0xC6C6C6), 1);
                float tw = ctrl_->tc_.measure(f.text, 13, true);
                ctrl_->tc_.draw(bx + (bw - tw) / 2, sy + 13, f.text, 13, true,
                                kFg);
                break;
            }
            case EdCore::FlyItem::VarRow:
            case EdCore::FlyItem::ListRow: {
                bool mon = false;
                auto it = ed.rt ? ed.rt->monitorState.records.find(f.varId)
                                : ed.rt->monitorState.records.end();
                if (it != ed.rt->monitorState.records.end())
                    mon = it->second.visible;
                Color col = f.list ? rgb(0xFF661A) : rgb(0xFF8C1A);
                float cx = 14;
                cv.strokeRoundRect(cx, sy + 10, 14, 14, 3, kDim, 1.5f);
                if (mon) {
                    cv.fillRect(cx + 3, sy + 13, 8, 8, col);
                    ctrl_->tc_.draw(cx + 3.5f, sy + 11, "✓", 9, false,
                                    Color{255, 255, 255, 255});
                }
                float px = cx + 22;
                float pw = ctrl_->tc_.measure(f.text, 11, true) + 20;
                cv.fillRoundRect(px, sy + 6, pw, 22, 11, col);
                ctrl_->tc_.draw(px + 10, sy + 11, f.text, 11, true,
                                Color{255, 255, 255, 255});
                break;
            }
            case EdCore::FlyItem::Divider:
                cv.line(8, sy + 4, width() - 8, sy + 4, kBorder, 1);
                break;
            }
        }
        float ch = ed.flyContentH() * bl::kWsZoom;
        if (ch > height()) {
            float barH = height() * height() / ch;
            float barY = ed.flyScroll * (height() - barH) / (ch - height());
            cv.fillRoundRect(width() - 8.5f, barY, 6, barH, 3,
                             Color{0xBB, 0xBB, 0xBB, 255});
        }
        cv.popClip();
    }

    void drawWs(QPainter* p, QpCanvas& cv) {
        EdCore& ed = ctrl_->ed;
        // 背景 + ドットグリッド
        p->fillRect(0, 0, width(), height(), QColor(0xF9, 0xF9, 0xF9));
        float s = ed.wsScale();
        float step = 32 * s;
        if (step > 4) {
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0xDD, 0xDD, 0xDD));
            float x0 = std::fmod(ed.panX, step);
            float y0 = std::fmod(ed.panY, step);
            for (float sy = y0; sy < height(); sy += step)
                for (float sx = x0; sx < width(); sx += step)
                    p->drawEllipse(QPointF(sx, sy), 1.2, 1.2);
        }
        if (ed.wsDirty_ || ed.wsTarget_ != ed.selTarget) {
            ed.wsDirty_ = true;
            ed.rebuildWs();
        }
        Target* t = ed.selected();
        Blocks* blocks = t ? t->blocks : nullptr;
        auto heads = ed.runningHeads();
        cv.pushClip(0, 0, width(), height(), false);
        std::set<std::string> dragged;
        if (ed.drag.active && !ed.drag.fromFlyout)
            bl::collectSubtree(blocks, ed.drag.headId, dragged);
        for (auto& lb : ed.wsLayout_.blocks) {
            if (dragged.count(lb.id)) continue;
            const float bx = ed.panX + lb.x * s, by = ed.panY + lb.y * s;
            if ((!lb.block || lb.block->comment.empty()) &&
                (bx + lb.w * s < -12 || by + lb.fullH * s < -12 ||
                 bx > width() + 12 ||
                 by - (lb.hat ? (lb.defineHat ? 40.f : 17.f) * s : 0.f) >
                     height() + 12)) continue;
            bl::BlockStyle st = bl::styleOf(blocks, lb.block);
            bool glow = heads.count(lb.id) > 0 ||
                        (lb.block && !lb.block->topLevel &&
                         heads.count(lb.block->id));
            bool isHead = lb.block && lb.block->topLevel;
            bl::drawLBlock(nullptr, lb, blocks, ed.tr(s), st,
                           ed.panX / s, ed.panY / s, s,
                           lb.id == ed.selBlock,
                           isHead && heads.count(lb.id), 1.f);
            if (lb.block && !lb.block->comment.empty())
                bl::drawComment(nullptr, lb.block->comment,
                                ed.panX + lb.x * s, ed.panY + lb.y * s, s,
                                ed.tr(s));
        }
        if (ed.drag.active && ed.drag.hasSnap) {
            cv.fillCircle(ed.panX + ed.drag.snapX * s,
                          ed.panY + ed.drag.snapY * s, 5 * s, kGlow);
            cv.strokeCircle(ed.panX + ed.drag.snapX * s,
                            ed.panY + ed.drag.snapY * s, 7 * s, 1.5f * s,
                            kGlow);
        }
        if (ed.drag.active && ed.drag.fromFlyout) {
            // フライアウト由来: テンプレをゴースト表示
            bl::LBlock g = ed.drag.tmpl;
            g.x = ed.drag.mx;
            g.y = ed.drag.my;
            bl::drawLBlock(nullptr, g, nullptr, ed.tr(s), ed.drag.tmplSt,
                           ed.panX / s, ed.panY / s, s, false, false, 0.85f);
        } else if (ed.drag.active && blocks) {
            for (const auto& lb : ed.drag.previewLayout) {
                const float bx = ed.panX + (ed.drag.mx + lb.x) * s;
                const float by = ed.panY + (ed.drag.my + lb.y) * s;
                if (bx + lb.w * s < -12 || by + lb.fullH * s < -12 ||
                    bx > width() + 12 ||
                    by - (lb.hat ? (lb.defineHat ? 40.f : 17.f) * s : 0.f) >
                        height() + 12) continue;
                bl::BlockStyle st = bl::styleOf(blocks, lb.block);
                bl::drawLBlock(nullptr, lb, blocks, ed.tr(s), st,
                               ed.panX / s + ed.drag.mx,
                               ed.panY / s + ed.drag.my, s, false, false,
                               0.85f);
            }
        }
        cv.popClip();
        // ドラッグ中の削除ゾーン（Scratch 3 にはゴミ箱が無いので右下の赤枠）
        if (ed.drag.active && !ed.drag.fromFlyout) {
            float tx = width() - 160, ty = height() - 56;
            cv.fillRoundRect(tx, ty, 56, 44, 8, Color{0xE6, 0x4C, 0x4C, 0x88});
            cv.strokeRoundRect(tx, ty, 56, 44, 8, kBorder, 1);
            float cx = tx + 28, cy = ty + 10;
            cv.fillRect(cx - 10, cy, 20, 3, Color{255, 255, 255, 255});
            cv.fillRect(cx - 4, cy - 3, 8, 3, Color{255, 255, 255, 255});
            Path bucket;
            bucket.moveTo(cx - 8, cy + 3);
            bucket.lineTo(cx + 8, cy + 3);
            bucket.lineTo(cx + 5, cy + 24);
            bucket.lineTo(cx - 5, cy + 24);
            bl::fillPath(nullptr, bucket, 1.f, Color{255, 255, 255, 255});
        }
    }

private:
    EditorController* ctrl_ = nullptr;
    bool flyout_ = false;
};

// ---------------------------------------------------------------------------
// StageItem — SdlRenderer の出力を表示
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// CostumeImageProvider — image://xyocostume/<target>/<rev> でスプライトタイルへ
// ---------------------------------------------------------------------------
class CostumeImageProvider : public QQuickImageProvider {
public:
    /// GUI スレッドで事前生成したサムネイル。
    /// キー: "ti" = スプライトタイル用の現行コスチューム、
    ///       "ti_ci" = コスチュームタブ用の個別コスチューム。
    /// requestImage は別スレッドから呼ばれ得るので decode はしない。
    std::map<std::string, QImage> thumbs;
    CostumeImageProvider()
        : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString& id, QSize*, const QSize&) override {
        auto it = thumbs.find(id.section('/', 0, 0).toStdString());
        return it == thumbs.end() ? QImage() : it->second;
    }
};

/// 選択ターゲットの全コスチューム + タイル用現行コスチュームを再生成
/// するコールバック（editorMainQt で provider へ束縛）。
static std::function<void()> g_thumbRefresh;
void EditorController::refreshCostumeThumbs() {
    if (g_thumbRefresh) g_thumbRefresh();
}

class StageItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(EditorController* ctrl READ ctrl WRITE setCtrl)
public:
    StageItem(QQuickItem* parent = nullptr) : QQuickPaintedItem(parent) {
        setAntialiasing(true);
        setOpaquePainting(true);
    }
    EditorController* ctrl() const { return ctrl_; }
    void setCtrl(EditorController* c) {
        ctrl_ = c;
        if (c) connect(c, &EditorController::layoutChanged,
                       this, [this]() { update(); });
    }
    void paint(QPainter* p) override {
        p->fillRect(0, 0, width(), height(), QColor(0xFF, 0xFF, 0xFF));
        if (!ctrl_ || !ctrl_->stageImgValid) return;
        p->setRenderHint(QPainter::SmoothPixmapTransform, true);
        p->drawImage(QRectF(0, 0, width(), height()), ctrl_->stageImg);
    }
private:
    EditorController* ctrl_ = nullptr;
};

// ---------------------------------------------------------------------------
// entry
// ---------------------------------------------------------------------------
int xyo::editorMainQt(const char* path, bool danger) {
    // `xyo edit` 直起動では QGuiApplication がまだ無い（ランチャー経由では
    // guiMain が既に作っている）。無ければここで生成し、終了時に破棄する。
    std::unique_ptr<QGuiApplication> ownApp;
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char arg0[] = "xyo";
        static char* argv[] = {arg0, nullptr};
        ownApp = std::make_unique<QGuiApplication>(argc, argv);
        QGuiApplication::setApplicationName("xyo-editor");
    }
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }
    std::vector<uint8_t> data{std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>()};
    std::unique_ptr<Runtime> rtOwn(new Runtime);
    ExtensionRegistry extensions;
    const ExtensionLoadReport extReport =
        bootstrapExtensionsFromEnvironment(*rtOwn, extensions);
    for (const ExtensionDef& def : extReport.loaded)
        edit::registerExtensionDef(def);
    for (const std::string& error : extReport.errors)
        std::fprintf(stderr, "extension: %s\n", error.c_str());
    Sb3LoadResult res = loadSb3File(*rtOwn, data.data(), data.size());
    if (!res.ok) {
        std::fprintf(stderr, "load failed: %s\n", res.error.c_str());
        return 1;
    }
    if (danger) rtOwn->dangerMode = true;

    EditorController ctrl;
    ctrl.rt = rtOwn.get();
    ctrl.ed.rt = rtOwn.get();
    ctrl.ed.path = path;
    ctrl.ed.buildCategories();
    ctrl.ed.backpackLoad();
    for (size_t i = 0; i < rtOwn->targets.size(); ++i)
        if (!rtOwn->targets[i]->isStage) { ctrl.ed.selTarget = (int)i; break; }
    if (const char* tg = std::getenv("XYO_EDIT_TARGET"))
        ctrl.ed.selTarget = std::atoi(tg);
    if (const char* cc = std::getenv("XYO_EDIT_CAT")) {
        if (*cc >= '0' && *cc <= '9') {
            ctrl.ed.catSel = std::atoi(cc);
        } else {
            for (int i = 0; i < ctrl.ed.catCount(); ++i)
                if (ctrl.ed.catAt(i).id == cc) { ctrl.ed.catSel = i; break; }
        }
    }

    // フォント: 埋め込み Liberation Sans（QML/Qt 両方で使う）
#ifdef XYO_HAVE_EMBEDDED_FONT
    int fid = QFontDatabase::addApplicationFontFromData(
        QByteArray((const char*)xyoFontRegular, (int)xyoFontRegularSize));
    QFontDatabase::addApplicationFontFromData(
        QByteArray((const char*)xyoFontBold, (int)xyoFontBoldSize));
    if (fid >= 0) {
        const QString fam =
            QFontDatabase::applicationFontFamilies(fid).value(0);
        if (!fam.isEmpty()) ctrl.tc_.family = fam;
    }
#endif

    // headless SDL（ステージ + アイコンテクスチャ用。ウィンドウは出さない）
    SDL_SetMainReady();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0 &&
        !SDL_WasInit(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "editor-qt: SDL video init failed: %s\n",
                     SDL_GetError());
        return 1;
    }
    ctrl.sdlWin = SDL_CreateWindow("xyo-editor-stage",
                                   SDL_WINDOWPOS_UNDEFINED,
                                   SDL_WINDOWPOS_UNDEFINED, 16, 16,
                                   SDL_WINDOW_HIDDEN);
    if (!ctrl.sdlWin) {
        std::fprintf(stderr, "editor-qt: SDL window failed\n");
        return 1;
    }
    ctrl.sdlR = SDL_CreateRenderer(ctrl.sdlWin, -1, SDL_RENDERER_SOFTWARE);
    if (!ctrl.sdlR) {
        std::fprintf(stderr, "editor-qt: SDL renderer failed\n");
        return 1;
    }
    ctrl.ed.r = ctrl.sdlR;
    if (!ctrl.ren.initEmbedded(*rtOwn, ctrl.sdlR)) {
        std::fprintf(stderr, "editor-qt: embedded renderer init failed\n");
        return 1;
    }

    // QML 型登録 + ビュー
    qmlRegisterUncreatableType<EditorController>(
        "Xyo.Editor", 1, 0, "EditorController", "context property");
    qmlRegisterType<BlockCanvas>("Xyo.Editor", 1, 0, "BlockCanvas");
    qmlRegisterType<StageItem>("Xyo.Editor", 1, 0, "StageItem");

    auto* imgProvider = new CostumeImageProvider();
    // サムネイルを GUI スレッドで事前生成
    auto rebuildThumbs = [&]() {
        for (size_t i = 0; i < rtOwn->targets.size(); ++i) {
            Target* t = rtOwn->targets[i];
            if (!t->sprite) continue;
            for (size_t ci = 0; ci < t->sprite->costumes.size(); ++ci) {
                const Costume* c = &t->sprite->costumes[ci];
                std::vector<uint8_t> px;
                int w = 0, h = 0;
                if (!ctrl.ren.costumePixels(c, px, &w, &h)) continue;
                QImage img(w, h, QImage::Format_RGBA8888);
                std::memcpy(img.bits(), px.data(), size_t(w) * h * 4);
                img = img.convertToFormat(
                    QImage::Format_ARGB32_Premultiplied);
                if ((int)ci == t->currentCostume)
                    imgProvider->thumbs[std::to_string(i)] = img;  // タイル用
                imgProvider->thumbs[std::to_string(i) + "_" +
                                    std::to_string(ci)] = img;      // タブ用
            }
        }
    };
    rebuildThumbs();
    g_thumbRefresh = rebuildThumbs;
    QQuickView view;
    view.engine()->addImageProvider(QStringLiteral("xyocostume"),
                                    imgProvider);
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.rootContext()->setContextProperty(QStringLiteral("editor"), &ctrl);
    view.setSource(QUrl(QStringLiteral("qrc:/xyo/qml/Editor.qml")));
    if (view.status() == QQuickView::Error) {
        for (const auto& e : view.errors())
            std::fprintf(stderr, "editor-qt: %s\n",
                         e.toString().toUtf8().constData());
        return 1;
    }
    view.setTitle("xyo — " + qstr(path));
    view.resize(1280, 800);
    view.show();
    // 設定復元（zoom / catSel。stageMode は QML の Component.onCompleted から）
    ctrl.ed.zoom = (float)ctrl.loadZoom();
    ctrl.setCategorySel(ctrl.loadCategory());

    // 実行/描画 tick: ランタイム step + ステージ再描画 + 再描画通知
    QTimer tickTimer;
    QObject::connect(&tickTimer, &QTimer::timeout, &ctrl, [&]() {
        ctrl.tick();
        Q_EMIT ctrl.layoutChanged();
    });
    tickTimer.start(16);

    // 検証フック（XYO_GUI_* と同様のタイマー駆動アクション）
    int frames = 0;
    int exitFrames = 0;
    if (const char* ef = std::getenv("XYO_EDIT_EXIT_FRAMES"))
        exitFrames = std::atoi(ef);
    else if (const char* ef2 = std::getenv("XYO_EXIT_FRAMES"))
        exitFrames = std::atoi(ef2);
    const char* shot = std::getenv("XYO_EDIT_SHOT");
    QString shotPath = shot ? QString::fromUtf8(shot) : QString();
    // XYO_EDIT_SAVE: 別パスへ保存（editor.cpp と同じ検証フック）
    const char* saveEnv = std::getenv("XYO_EDIT_SAVE");
    if (saveEnv && *saveEnv) ctrl.ed.savePath = saveEnv;
    QStringList actions;
    if (const char* a = std::getenv("XYO_EDIT_ACTIONS"))
        actions = QString::fromUtf8(a).split(',');
    int actIdx = 0;
    QTimer actTimer;
    QObject::connect(&actTimer, &QTimer::timeout, &view, [&]() {
        ++frames;
        if (actIdx < actions.size()) {
            const QString a = actions[actIdx++].trimmed();
            if (a == "quit") view.close();
            else if (a == "save") ctrl.save();
            else if (a == "flag") ctrl.greenFlag();
            else if (a == "stop") ctrl.stopAll();
            else if (a == "undo") ctrl.undo();
            else if (a == "redo") ctrl.redo();
            else if (a == "dup") ctrl.duplicateSelected();
            else if (a == "del") ctrl.deleteSelected();
            else if (a == "cleanup") ctrl.cleanupBlocks();
            else if (a.startsWith("tab:")) ctrl.setTab(a.mid(4).toInt());
            else if (a == "addcostume") ctrl.addCostume();
            else if (a == "addsound") ctrl.addSound();
            else if (a.startsWith("flypress:")) {
                // flypress:<index> — フライアウト項目の中心を押す
                EdCore& e = ctrl.ed;
                e.flyDirty_ = true;
                e.rebuildFlyout();
                int idx = a.mid(9).toInt();
                if (std::getenv("XYO_EDIT_DEBUG")) {
                    std::fprintf(stderr, "[edqt] flypress items=%zu cat=%s selTarget=%d\n",
                                 e.flyItems_.size(), e.flyCat_.c_str(),
                                 e.selTarget);
                    for (size_t i = 0; i < e.flyItems_.size(); ++i)
                        std::fprintf(stderr, "  [%zu] kind=%d y=%.0f h=%.0f %s\n",
                                     i, (int)e.flyItems_[i].kind,
                                     e.flyItems_[i].y, e.flyItems_[i].h,
                                     e.flyItems_[i].text.c_str());
                }
                if (idx >= 0 && idx < (int)e.flyItems_.size()) {
                    const EdCore::FlyItem& fi = e.flyItems_[idx];
                    float sy = fi.y * bl::kWsZoom - e.flyScroll + fi.h * 0.5f;
                    ctrl.flyMousePress(60, sy, 190);
                    ctrl.flyMouseRelease();
                }
            }
            else if (a.startsWith("flycheck:")) {
                // flycheck:<index> — 変数行のチェックボックス（モニタ切替）
                EdCore& e = ctrl.ed;
                e.flyDirty_ = true;
                e.rebuildFlyout();
                int idx = a.mid(9).toInt();
                if (idx >= 0 && idx < (int)e.flyItems_.size()) {
                    const EdCore::FlyItem& fi = e.flyItems_[idx];
                    float sy = fi.y * bl::kWsZoom - e.flyScroll;
                    ctrl.flyMousePress(20, sy + 17, 190);
                }
            }
            else if (a.startsWith("mbadd:"))
                ctrl.mbAddPart(a.mid(6).toInt());
            else if (a == "mbwarp") ctrl.mbSetWarp(!ctrl.mbWarp());
            else if (a == "mbok") ctrl.mbOk();
            else if (a == "mbcancel") ctrl.mbCancel();
            else if (a.startsWith("rclick:")) {
                QStringList p = a.mid(7).split(':');
                if (p.size() == 2) {
                    ctrl.openWsContextMenu(p[0].toDouble(), p[1].toDouble());
                    if (std::getenv("XYO_EDIT_DEBUG"))
                        for (const auto& m : ctrl.ed.ctx_)
                            std::fprintf(stderr, "[edqt] ctx: %s (%s)%s\n",
                                         m.label.toStdString().c_str(),
                                         m.act.toStdString().c_str(),
                                         m.enabled ? "" : " disabled");
                }
            }
            else if (a.startsWith("ctx:"))
                ctrl.runCtxItem(a.mid(4).toInt(), 700, 500);
            else if (a.startsWith("cmt:"))
                ctrl.commentCommit(QString::fromStdString(a.mid(4).toStdString()));
            else if (a == "cmtcancel") ctrl.commentCancel();
            else if (a.startsWith("bppaste:"))
                ctrl.backpackPasteAt(a.mid(8).toInt(), 700, 500);
            else if (a.startsWith("sprdup:"))
                ctrl.duplicateSprite(a.mid(7).toInt());
            else if (a.startsWith("sprexp:"))
                ctrl.exportSprite(a.mid(7).toInt());
            else if (a.startsWith("sprord:")) {
                QStringList p = a.mid(7).split(':');
                if (p.size() == 2)
                    ctrl.spriteReorder(p[0].toInt(), p[1].toInt());
            }
            else if (a == "spradd") ctrl.addSprite();
            else if (a.startsWith("sndfx:"))
                ctrl.soundEffect(std::max(0, ctrl.ed.soundSel_), a.mid(6));
            else if (a == "c2bmp") {
                int ci = ctrl.ed.rt->targets[ctrl.ed.selTarget]->currentCostume;
                ctrl.convertCostumeToBitmap(ci);
            }
            else if (a.startsWith("sprrot:"))
                ctrl.setSpriteRotStyle(a.mid(7));
            else if (a.startsWith("addext:"))
                ctrl.addExtension(a.mid(7));
            else if (a.startsWith("sprname:"))
                ctrl.setSpriteName(a.mid(8));
            else if (a.startsWith("sprx:"))
                ctrl.setSpriteX(a.mid(5).toDouble());
            else if (a.startsWith("spry:"))
                ctrl.setSpriteY(a.mid(5).toDouble());
            else if (a.startsWith("catg:"))
                ctrl.setCategorySel(a.mid(5).toInt());
            else if (a.startsWith("stage:")) {
                // stage:<0|1|2> — small/large/fullscreen
                if (QObject* ro = view.rootObject())
                    QMetaObject::invokeMethod(ro, "setStageMode",
                                              Q_ARG(QVariant,
                                                    a.mid(6).toInt()));
            }
            else if (a.startsWith("key:")) {
                // key:<letter|space|up|down|left|right|return> — down+up
                const QString k = a.mid(4);
                int sdl = 0;
                if (k.size() == 1 && k[0] >= 'a' && k[0] <= 'z')
                    sdl = k[0].unicode();
                else if (k == "space") sdl = SDLK_SPACE;
                else if (k == "up") sdl = SDLK_UP;
                else if (k == "down") sdl = SDLK_DOWN;
                else if (k == "left") sdl = SDLK_LEFT;
                else if (k == "right") sdl = SDLK_RIGHT;
                else if (k == "return") sdl = SDLK_RETURN;
                else if (k.size() == 1) sdl = k[0].unicode();
                if (sdl) {
                    ctrl.stageKey(sdl, true);
                    ctrl.stageKey(sdl, false);
                }
            }
            else if (a.startsWith("sclick:")) {
                // sclick:x:y — ステージ表示座標でクリック
                QStringList p = a.mid(7).split(':');
                if (p.size() == 2) {
                    ctrl.stageMouseDown(p[0].toDouble(), p[1].toDouble());
                    ctrl.stageMouseUp(p[0].toDouble(), p[1].toDouble());
                }
            }
            else if (a.startsWith("target:"))
                ctrl.setSelTarget(a.mid(7).toInt());
            else if (a.startsWith("cat:"))
                ctrl.setCategorySel(a.mid(4).toInt());
            else if (a.startsWith("wsdrag:")) {
                // wsdrag:x1:y1:x2:y2 — ws 内ドラッグ（ビュー座標）
                QStringList p = a.mid(7).split(':');
                if (p.size() == 4) {
                    double x1 = p[0].toDouble(), y1 = p[1].toDouble();
                    double x2 = p[2].toDouble(), y2 = p[3].toDouble();
                    ctrl.wsMousePress(x1, y1);
                    for (int i = 1; i <= 8; ++i)
                        ctrl.wsMouseMove(x1 + (x2 - x1) * i / 8,
                                         y1 + (y2 - y1) * i / 8);
                    ctrl.wsMouseRelease(x2, y2);
                }
            } else if (a.startsWith("flydrag:")) {
                // flydrag:<item index>:x2:y2 — flyout から ws へドラッグ
                // （座標は ws ビュー座標）
                QStringList p = a.mid(8).split(':');
                if (p.size() == 3) {
                    int idx = p[0].toInt();
                    double x2 = p[1].toDouble(), y2 = p[2].toDouble();
                    EdCore& e = ctrl.ed;
                    e.flyDirty_ = true;
                    e.rebuildFlyout();
                    if (std::getenv("XYO_EDIT_DEBUG"))
                        std::fprintf(stderr,
                                     "[edqt] flydrag items=%zu idx=%d\n",
                                     e.flyItems_.size(), idx);
                    auto k = e.flyItems_[idx].kind;
                    if (idx >= 0 && idx < (int)e.flyItems_.size() &&
                        (k == EdCore::FlyItem::BlockT ||
                         k == EdCore::FlyItem::VarRow ||
                         k == EdCore::FlyItem::ListRow)) {
                        EdCore::FlyItem* fi = &e.flyItems_[idx];
                        e.drag.pending = true;
                        e.drag.fromFlyout = true;
                        e.drag.flyItem = fi;
                        e.drag.opcode = fi->opcode;
                        e.drag.initFields = fi->initFields;
                        e.drag.mutation = fi->mutation;
                        e.drag.argTexts = fi->argTexts;
                        e.drag.tmpl = fi->lb;
                        e.drag.tmplSt = fi->st;
                        e.drag.shape = fi->st.shape;
                        ctrl.wsDragMove(x2 - 30, y2 - 10);
                        for (int i = 1; i <= 8; ++i)
                            ctrl.wsDragMove(x2 - 30 + 4 * i,
                                            y2 - 10 + i);
                        ctrl.wsMouseRelease(x2, y2);
                    }
                }
            } else if (a.startsWith("click:")) {
                QStringList p = a.mid(6).split(':');
                if (p.size() == 2) {
                    double x = p[0].toDouble(), y = p[1].toDouble();
                    ctrl.wsMousePress(x, y);
                    ctrl.wsMouseRelease(x, y);
                }
            } else if (a.startsWith("field:")) {
                // field:<text> — 開いているフィールド編集を確定
                ctrl.fieldEditCommit(
                    QString::fromStdString(a.mid(6).toStdString()));
            } else if (a == "fieldesc") {
                ctrl.fieldEditCancel();
            } else if (a.startsWith("menu:")) {
                ctrl.menuPick(a.mid(5).toInt());
            }
        }
        if (exitFrames && frames >= exitFrames) {
            if (!shotPath.isEmpty()) {
                QImage img = view.grabWindow();
                if (!img.isNull()) {
                    img = img.convertToFormat(QImage::Format_RGB888);
                    if (FILE* fp = std::fopen(shotPath.toUtf8().constData(),
                                              "wb")) {
                        std::fprintf(fp, "P6\n%d %d\n255\n",
                                     img.width(), img.height());
                        for (int y = 0; y < img.height(); ++y)
                            std::fwrite(img.constScanLine(y), 1,
                                        size_t(img.width()) * 3, fp);
                        std::fclose(fp);
                    }
                }
            }
            view.close();
        }
    });
    actTimer.start(30);

    // ネストイベントループ（ランチャーの app.exec() 内でも機能する）
    QEventLoop loop;
    // QQuickWindow::closing の引数 QQuickCloseEvent は Qt 6.4 の public
    // ヘッダに無い（private のみ）ため、signal/slot の文字列接続にする。
    // 型名を引かないので qmetatype の不完全型エラーを回避できる。
    // visibleChanged も拾い、ウィンドウが閉じ/隠れたら quit する。
    const QMetaObject* vmo = view.metaObject();
    const QMetaObject* lmo = loop.metaObject();
    const int quitIdx = lmo->indexOfSlot("quit()");
    if (vmo && quitIdx >= 0) {
        int sidx = vmo->indexOfSignal("closing(QQuickCloseEvent*)");
        if (sidx >= 0)
            QMetaObject::connect(&view, sidx, &loop, quitIdx);
        sidx = vmo->indexOfSignal("visibleChanged(bool)");
        if (sidx >= 0)
            QMetaObject::connect(&view, sidx, &loop, quitIdx);
    }
    if (std::getenv("XYO_EDIT_DEBUG"))
        std::fprintf(stderr, "[edqt] entering loop, vis=%d\n",
                     (int)view.isVisible());
    // ランチャー経由だと Qt が残留 Quit を処理して exec() が vis=1 の
    // まま早抜けする場合がある。ウィンドウが生きている限り再入する。
    while (view.isVisible())
        loop.exec();
    if (std::getenv("XYO_EDIT_DEBUG"))
        std::fprintf(stderr, "[edqt] loop done, vis=%d\n",
                     (int)view.isVisible());

    // 組み込みレンダラのテクスチャは保持中の SDL_Renderer が生きている間に
    // 解放する（editor.cpp と同じく placement new で空インスタンスに戻す）。
    ctrl.ren.~SdlRenderer();
    new (&ctrl.ren) SdlRenderer();
    if (ctrl.stageTex) SDL_DestroyTexture(ctrl.stageTex);
    SDL_DestroyRenderer(ctrl.sdlR);
    SDL_DestroyWindow(ctrl.sdlWin);
    return 0;
}

#include "editor_qt.moc"

#endif  // XYO_HAVE_QT
