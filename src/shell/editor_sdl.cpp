// xyo-cpp: ビルトインエディタ（SDL2）。
// scratch-gui（Scratch 3.0）の単一ウィンドウ構成を完全再現する:
//   青いメニューバー + Code/Costumes/Sounds タブ
//   左: カテゴリ列 + フライアウト（実ブロック形状のテンプレ）
//   中央: 無限ワークスペース（ドットグリッド・パン/ズーム・自由配置）
//   右: ステージ + スプライト情報 + スプライト選択
//   下端: バックパック
#include "editor.hpp"

#include <cstdio>
#include <vector>

#ifdef XYO_HAVE_SDL2

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

    // ---- テキスト描画 ----
    struct Tex { SDL_Texture* t = nullptr; int w = 0, h = 0; };
    std::unordered_map<std::string, Tex> texCache;
    SDL_Renderer* textTarget = nullptr;   // 計測用の共有レンダラ

    SDL_Texture* textTex(const std::string& utf8, int px, bool bold, Color c,
                         int* w, int* h) {
        char suf[48];
        std::snprintf(suf, sizeof(suf), "|%d|%d|%02x%02x%02x%02x", px, bold ? 1 : 0,
                      c.r, c.g, c.b, c.a);
        std::string key = utf8 + suf;
        auto it = texCache.find(key);
        if (it != texCache.end()) {
            *w = it->second.w; *h = it->second.h;
            return it->second.t;
        }
        std::u16string u = utf8ToU16(utf8);
        std::vector<uint8_t> bmp;
        int tw = 0, th = 0;
        FontRenderer* fr = FontRenderer::get();
        if (!fr || !fr->render(u, px, c.r, c.g, c.b, bmp, tw, th, bold))
            return nullptr;
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
            bmp.data(), tw, th, 32, tw * 4, SDL_PIXELFORMAT_RGBA32);
        if (!surf) return nullptr;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(textTarget, surf);
        SDL_FreeSurface(surf);
        if (!tex) return nullptr;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        texCache[key] = Tex{tex, tw, th};
        *w = tw; *h = th;
        return tex;
    }
    void drawText(float x, float y, const std::string& s, int px, Color c,
                  bool bold = false) {
        int w = 0, h = 0;
        SDL_Texture* t = textTex(s, px, bold, c, &w, &h);
        if (!t) {
            SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
            SDL_FRect rr{x, y + px / 4, (float)s.size() * px / 2,
                         (float)px / 2};
            SDL_RenderFillRectF(r, &rr);
            return;
        }
        SDL_Rect dst{(int)x, (int)y, w, h};
        SDL_RenderCopy(r, t, nullptr, &dst);
    }
    int textW(const std::string& s, int px, bool bold = false) {
        int w = 0, h = 0;
        textTex(s, px, bold, Color{0, 0, 0, 255}, &w, &h);
        return w;
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
    bl::TextRenderer tr(float s = -1.f) {
        bl::TextRenderer t;
        t.ctx = this;
        t.measure = &Editor::measureThunk;
        t.draw = &Editor::drawThunk;
        t.scale = s > 0 ? s : wsScale();
        trScale_ = t.scale;
        return t;
    }

    // ---- 選択ターゲット ----
    int selTarget = 0;
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
        // 現在値を強調
        std::string cur = bl::fieldStr(b, g.field);
        std::string curId = b->fields.count(g.field) ? b->fields.at(g.field).id : "";
        for (size_t i = 0; i < menu_.ids.size(); ++i) {
            if (menu_.variable ? menu_.ids[i] == curId
                              : menu_.ids[i] == cur) {
                menu_.hover = (int)i;
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
            float cw = std::min(160.f, textW(backpack[i].label, 11) + 20.f);
            if (mx >= x && mx < x + cw && my >= y && my < y + 26) return i;
            x += cw + 6;
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
        int itemH = 26;
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
    static std::vector<const char*> topMenuList(int which) {
        if (which == 0)
            return {"Save to your computer", "Load from your computer"};
        if (which == 2)
            return {"Pen", "Music"};
        if (which == 3)
            return {"Choose a Sprite", "Paint", "Surprise", "Upload"};
        if (which == 4)
            return {"Choose a Backdrop", "Paint", "Surprise", "Upload"};
        return {"Undo", "Redo"};
    }
    FRect addExtRect_{0, 0, 0, 0};   // ツールボックス下部の Add Extension
    FRect menuBtnFile, menuBtnEdit;   // drawMenuBar で埋める
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
            if (i == 0) {   // Save
                if (rt && !path.empty()) {
                    bool ok = saveSb3File(*rt, path.c_str());
                    std::fprintf(stderr, "editor: saved %s (%s)\n",
                                 path.c_str(), ok ? "ok" : "FAILED");
                }
            } else if (i == 1) {   // Load from your computer
                reloadRequested_ = true;
            }
        } else if (which == 1) {
            if (i == 0) undo();
            else if (i == 1) redo();
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

    struct DirectionPickerDlg {
        bool open = false;
        float x = 0, y = 0; // ポップオーバー左上
        float w = 150, h = 175;
        float dialCx = 0, dialCy = 0, dialR = 48;
        bool dragging = false;
        FRect btnAll, btnLr, btnNo;
    } dirPicker_;

    struct Ctx {
        bool open = false;
        float x = 0, y = 0;
        int hover = -1;
        std::string blockId;
        std::vector<std::string> items;
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
        float y = 16;
        auto& items = flyItems_;
        Target* t = selected();
        if (cat.id == "data") {
            // 変数: Make a Variable / チェックボックス + 変数記者
            FlyItem mkv;
            mkv.kind = FlyItem::Button;
            mkv.text = "Make a Variable";
            mkv.list = false;
            mkv.y = y; mkv.h = 30;
            items.push_back(mkv);
            y += 38;
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
                y += fi.h + 20;
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
            mkl.y = y; mkl.h = 30;
            items.push_back(mkl);
            y += 38;
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
            mkb.y = y; mkb.h = 30;
            items.push_back(mkb);
            y += 44;
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
                    y += fi.h + 14;
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
            y += fi.h + 14;
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
        for (auto& lb : wsLayout_.blocks) {
            float bx = panX + lb.x * wsScale();
            float by = panY + lb.y * wsScale();
            if (mx >= bx && mx < bx + lb.w * wsScale() &&
                my >= by && my < by + lb.h * wsScale() + 8)
                return &lb;
        }
        return nullptr;
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
            if (isRep) continue;
            if (lb.shape == edit::Shape::Stack || lb.shape == edit::Shape::Hat)
                out.push_back({0, lb.id, "", 0, lb.x + 32, lb.y + lb.h});
            if (!lb.noBump && lb.shape != edit::Shape::Hat)
                out.push_back({1, lb.id, "", 0, lb.x + 32, lb.y});
            for (auto& g : lb.segs) {
                // value input の接続点: inputs に存在する名前（shadow 付きの
                // NumField/StrField/メニュー箱もその input の受け口になる）
                if (g.input.empty() || g.kind == bl::SegKind::Text) continue;
                if (!b->inputs.has(g.input)) continue;
                out.push_back({2, lb.id, g.input, 0,
                               lb.x + g.x + 8, lb.y + g.y + g.h / 2});
            }
            for (auto& m : lb.mouths)
                out.push_back({3, lb.id, "", m.branch, m.x + 32, m.y});
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
        float srcX = isReporter ? drag.mx : drag.mx + 32;
        float srcY = isReporter ? drag.my + 16 : drag.my;
        float radius = isReporter ? 64 : 56;
        float bestD2 = radius * radius;
        int best = -1;
        for (int i = 0; i < (int)cands.size(); ++i) {
            const Conn& c = cands[i];
            bool compat = isReporter ? (c.kind == 2)
                                     : (c.kind == 0 || c.kind == 1 || c.kind == 3);
            if (!compat) continue;
            float dx = c.x - srcX, dy = c.y - srcY;
            float d2 = dx * dx + dy * dy;
            if (d2 < bestD2) { bestD2 = d2; best = i; }
        }
        if (best >= 0) {
            const Conn& c = cands[best];
            drag.targetId = c.blockId;
            drag.snapX = c.x; drag.snapY = c.y;
            drag.hasSnap = true;
            if (c.kind == 2) { drag.valueTarget = true; drag.valueInput = c.inputName; }
            else if (c.kind == 0) drag.before = false;
            else if (c.kind == 1) drag.before = true;
            else { drag.substackTarget = true; drag.subBranch = c.branch; }
            return;
        }
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
        if (drag.valueTarget && !drag.targetId.empty()) {
            bl::attachToValue(blocks, id, drag.targetId, drag.valueInput);
        } else if (drag.substackTarget && !drag.targetId.empty() && !isRep) {
            bl::attachToSubstack(blocks, id, drag.tailId.empty() ? id : drag.tailId,
                                 drag.targetId, drag.subBranch);
        } else if (!drag.targetId.empty() && !isRep) {
            if (drag.before)
                bl::insertBefore(blocks, id,
                                 drag.tailId.empty() ? id : drag.tailId,
                                 drag.targetId);
            else
                bl::insertAfter(blocks, id,
                                drag.tailId.empty() ? id : drag.tailId,
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
    float panStartX = 0, panStartY = 0, panPX = 0, panPY = 0;
    std::string shotPath;
    bool shotDone = false;
    int lastSel = -1;
    FRect flagRect{0,0,0,0}, stopRect{0,0,0,0};
    bool stageLarge_ = false;   // ステージ拡大モード（フルスクリーン相当）

    // ---- レイアウト ----------------------------------------------------------
    void layoutUi() {
        SDL_GetWindowSize(win, &W, &H);
        ui.menuBar = {0, 0, (float)W, (float)kMenuH};
        // タブはメニューバー直下の左側（gui.css: tab 列）
        ui.tabs = {0, (float)kMenuH, (float)(kToolboxW + kFlyoutW), (float)kTabH};
        float ty = ui.tabs.y + 8;
        float tx = ui.tabs.x + 8;
        const char* names[3] = {"Code", "Costumes", "Sounds"};
        for (int i = 0; i < 3; i++) {
            ui.tabX[i] = (int)tx;
            ui.tabW[i] = textW(names[i], 13, true) + 28;
            tx += ui.tabW[i] + 6;
        }
        if (stageLarge_) {
            // 拡大モード（scratch の unselectFullScreen → ステージを最大化）
            ui.stageMenu = {0, (float)kMenuH, (float)W, (float)kStageMenuH};
            ui.stage = {0, (float)(kMenuH + kStageMenuH), (float)W,
                        (float)(H - kMenuH - kStageMenuH)};
            ui.toolbox = ui.flyout = ui.ws = ui.body = ui.tabs = {};
            ui.spriteInfo = ui.spriteSel = ui.bpBar = ui.bpPanel = {};
            float fy = ui.stageMenu.y + 8;
            flagRect = {ui.stageMenu.x + 8, fy, 28, 28};
            stopRect = {ui.stageMenu.x + 40, fy, 28, 28};
            ui.trash = {};
            return;
        }
        // 右列（stage）
        float stageW = 480, stageH = 360;
        float rightX = W - kGap - stageW;
        ui.stageMenu = {rightX, (float)kMenuH, stageW, (float)kStageMenuH};
        ui.stage = {rightX, ui.stageMenu.y + ui.stageMenu.h, stageW, stageH};
        ui.spriteInfo = {rightX, ui.stage.y + ui.stage.h, stageW,
                         (float)kSpriteInfoH};
        float selH = H - (ui.spriteInfo.y + ui.spriteInfo.h) - kGap;
        float stageSelW = 72.f;
        float sprSelW = stageW - stageSelW - 6.f;
        ui.spriteSel = {rightX, ui.spriteInfo.y + ui.spriteInfo.h, sprSelW,
                        std::max(80.f, selH)};
        ui.stageSel = {rightX + sprSelW + 6.f, ui.spriteInfo.y + ui.spriteInfo.h,
                       stageSelW, std::max(80.f, selH)};
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
        // ツールボックスのカテゴリ行
        float cy = ui.toolbox.y + 12;
        for (int i = 0; i < catCount() && i < 16; ++i) {
            ui.catRow[i] = {ui.toolbox.x, cy, ui.toolbox.w, 40};
            cy += 44;
        }
        // ズーム & ゴミ箱（ゴミ箱はツールボックス下端）
        float zx = ui.ws.x + ui.ws.w - 36, zy = ui.ws.y + ui.ws.h - 120;
        ui.zoomPlus = {zx, zy, 28, 28};
        ui.zoomMinus = {zx, zy + 32, 28, 28};
        ui.zoomReset = {zx, zy + 64, 28, 28};
        // ゴミ箱はワークスペース右下（Scratch と同じ位置）
        ui.trash = {ui.ws.x + ui.ws.w - 108, ui.ws.y + ui.ws.h - 60, 60, 48};
        // ステージメニューの flag/stop
        float fy = ui.stageMenu.y + 8;
        flagRect = {ui.stageMenu.x + 8, fy, 28, 28};
        stopRect = {ui.stageMenu.x + 40, fy, 28, 28};
    }

    // ---- 描画: 各領域 --------------------------------------------------------
    void fillR(FRect rr, Color c) { fill(r, rr.x, rr.y, rr.w, rr.h, c); }
    void outlineR(FRect rr, Color c) { outline(r, rr.x, rr.y, rr.w, rr.h, c); }

    void drawMenuBar() {
        fillR(ui.menuBar, kHeader);
        // Scratch ロゴ（実 SVG、白/橙）
        bl::drawBlockIcon(r, "logo", 10, 7, 74, 28, 1.0f);
        // File / Edit（ドロップダウン）
        float x = 96;
        const char* menus[2] = {"File", "Edit"};
        for (int i = 0; i < 2; i++) {
            float mw = textW(menus[i], 12, true) + 24;
            FRect rr{x, 9, mw, 30};
            if (i == 0) menuBtnFile = rr; else menuBtnEdit = rr;
            bool open = topMenu_.open && topMenu_.which == i;
            if (open) {
                rrFill(r, rr.x, rr.y, rr.w, rr.h, 4, Color{0x29, 0x6A, 0xD8, 255});
            }
            drawText(x + 8, 16, menus[i], 12, Color{255, 255, 255, 255}, true);
            // ドロップダウン三角（Scratch の白い下向き小矢印）
            float cx = x + mw - 10, cy = 24;
            Path caret;
            caret.moveTo(cx - 3.5f, cy - 2);
            caret.lineTo(cx + 3.5f, cy - 2);
            caret.lineTo(cx, cy + 2);
            caret.close();
            fillPath(r, caret, 1.0f, Color{255, 255, 255, 255});
            x += mw + 6;
        }
        // Tutorials（Scratch と同じ項目。装飾）
        {
            float tw = textW("Tutorials", 12, true) + 20;
            FRect rr{x, 9, tw, 30};
            drawText(x + 8, 16, "Tutorials", 12, Color{255, 255, 255, 255}, true);
            x += tw + 10;
        }
        // プロジェクト名入力欄（Scratch: 点線枠の半透明ボックス、中央）
        {
            float boxW = std::max(200.f, textW(path, 12, true) + 32.f);
            float boxX = W / 2.f - boxW / 2.f;
            rrFill(r, boxX, 9, boxW, 30, 4, Color{255, 255, 255, 0x33});
            rrOutline(r, boxX, 9, boxW, 30, 4, Color{0, 0, 0, 0x33}, 1);
            drawText(boxX + 12, 16, path, 12, Color{255, 255, 255, 255}, true);
        }
        // 言語地球アイコン（右）
        bl::drawBlockIcon(r, "globe", W - 10 - 80 - 32, 13, 22, 22, 0.95f);
        // 保存ボタン（Scratch: Save Now / Save ボタン）
        float bw = 76;
        float bx = W - bw - 10;
        rrFill(r, bx, 9, bw, 30, 4, Color{255, 255, 255, 0x33});
        rrOutline(r, bx, 9, bw, 30, 4, Color{0, 0, 0, 0x22}, 1);
        drawText(bx + bw / 2 - textW("Save", 12, true) / 2, 16, "Save",
                 12, Color{255, 255, 255, 255}, true);
    }
    void drawTabs() {
        // メニューバー下のタブ列
        fill(r, ui.tabs.x, ui.tabs.y, ui.tabs.w, ui.tabs.h, kBody);
        const char* names[3] = {"Code", "Costumes", "Sounds"};
        for (int i = 0; i < 3; i++) {
            bool sel = tab == i;
            rrFill(r, (float)ui.tabX[i], ui.tabs.y + (sel ? 6 : 10),
                   (float)ui.tabW[i], ui.tabs.h - (sel ? 6 : 10), 8,
                   sel ? kTabActive : kTabBg);
            // カテゴリ色の小アイコン
            // scratch-gui のタブアイコン（白い 20px SVG）を描く
            static const char* tabIcons[3] = {"tab_code", "tab_costumes",
                                              "tab_sounds"};
            float iy = ui.tabs.y + ui.tabs.h / 2 - 10;
            bl::drawBlockIcon(r, tabIcons[i], ui.tabX[i] + 6, iy, 20, 20, 1.0f);
            drawText(ui.tabX[i] + 30, ui.tabs.y + ui.tabs.h / 2 - 6, names[i],
                     12, sel ? kFg : kDim, true);
        }
    }
    void drawToolbox() {
        fillR(ui.toolbox, kPanel);
        outlineR(ui.toolbox, kBorder);
        for (int i = 0; i < catCount() && i < 16; ++i) {
            const auto& c = catAt(i);
            bool sel = tab == 0 && catSel == i;
            if (sel) fillR(ui.catRow[i], kToolboxSel);
            // カテゴリバブル: 直径 20px（1.25rem）+ 1px secondary 境界
            // （scratchCategoryItemBubble）。拡張カテゴリはアイコン画像。
            float bx = ui.catRow[i].x + ui.toolbox.w / 2;
            bool isExt = c.name == "Pen" || c.name == "Music";
            if (isExt && bl::drawBlockIcon(
                    r, c.name == "Pen" ? "pen" : "music",
                    bx - 10, ui.catRow[i].y + 0, 20, 20)) {
                // アイコン描画済み
            } else {
                discFill(r, bx, ui.catRow[i].y + 10, 10, rgb(c.primary));
                // バブル境界はカテゴリ tertiary（= 最初のブロックの tertiary）
                uint32_t ter = c.id == "more" ? 0xFF3355u : 0;
                if (!ter && !c.items.empty()) {
                    if (const edit::BlockDef* d =
                            edit::findBlock(c.items[0]))
                        ter = d->tertiary;
                }
                if (ter)
                    ring(r, bx, ui.catRow[i].y + 10, 10, 1.f, rgb(ter));
            }
            float tw = textW(c.name, 10, sel);
            drawText(ui.catRow[i].x + (ui.toolbox.w - tw) / 2,
                     ui.catRow[i].y + 18, c.name, 10, sel ? kFg : kDim, sel);
        }
        // 区切り線
        line(r, ui.toolbox.x + ui.toolbox.w - 1, ui.toolbox.y,
             ui.toolbox.x + ui.toolbox.w - 1, ui.toolbox.y + ui.toolbox.h,
             kBorder, 1);
        // Add Extension（ツールボックス最下部 — 水色＋ボタン + 小さなラベル）
        {
            float cx = ui.toolbox.x + ui.toolbox.w / 2;
            float cy = ui.toolbox.y + ui.toolbox.h - 44;
            discFill(r, cx, cy, 16, kAccent);
            // ＋
            SDL_RenderDrawLine(r, (int)(cx - 7), (int)cy, (int)(cx + 7),
                               (int)cy);
            SDL_RenderDrawLine(r, (int)cx, (int)(cy - 7), (int)cx,
                               (int)(cy + 7));
            float tw = textW("Add Extension", 8);
            drawText(ui.toolbox.x + (ui.toolbox.w - tw) / 2, cy + 20,
                     "Add Extension", 8, kDim);
            addExtRect_ = {ui.toolbox.x + 4, cy - 20, ui.toolbox.w - 8, 64};
        }
    }
    void drawFlyout() {
        fillR(ui.flyout, kFlyoutBg);
        outlineR(ui.flyout, kBorder);
        if (flyDirty_ || flyCat_ != catAt(catSel).id ||
            flyTarget_ != selTarget) {
            flyDirty_ = true;
            rebuildFlyout();
        }
        // クリップ
        SDL_Rect clip{(int)ui.flyout.x, (int)ui.flyout.y, (int)ui.flyout.w,
                      (int)ui.flyout.h};
        SDL_RenderSetClipRect(r, &clip);
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
                drawText(ui.flyout.x + 12, sy + 6, f.text, 12, kDim, true);
                break;
            case FlyItem::Button: {
                rrFill(r, ui.flyout.x + 12, sy + 2, ui.flyout.w - 24, 26, 13,
                       kAccent);
                float tw = textW(f.text, 11, true);
                drawText(ui.flyout.x + 12 + (ui.flyout.w - 24 - tw) / 2,
                         sy + 8, f.text, 11, Color{255,255,255,255}, true);
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
        // スクロールバー
        float ch = flyContentH() * bl::kWsZoom;
        if (ch > ui.flyout.h) {
            float barH = ui.flyout.h * ui.flyout.h / ch;
            float barY = ui.flyout.y + flyScroll * (ui.flyout.h - barH) /
                                            (ch - ui.flyout.h);
            fill(r, ui.flyout.x + ui.flyout.w - 5, barY, 3, barH, kBorder);
        }
        SDL_RenderSetClipRect(r, nullptr);
    }

    void drawWsGrid() {
        fillR(ui.ws, kWsBg);
        // ドットグリッド: ws 論理 32px 毎
        float s = wsScale();
        float gx0 = ui.ws.x - std::fmod(panX - ui.ws.x, 32 * s);
        // panX は既に ws→画面オフセット。原点 (panX,panY) に 32*wsScale 間隔
        SDL_SetRenderDrawColor(r, kWsGrid.r, kWsGrid.g, kWsGrid.b, 255);
        float step = 32 * s;
        for (float y = ui.ws.y; y < ui.ws.y + ui.ws.h; y += 1) {
            // 点は間引きなし（密度が高いと重いので 2px 間隔で描く）
        }
        for (float gy = std::fmod(panY - ui.ws.y, step); gy < ui.ws.h;
             gy += step) {
            float sy = ui.ws.y + gy;
            for (float gx = std::fmod(panX - ui.ws.x, step); gx < ui.ws.w;
                 gx += step) {
                SDL_FRect d{ui.ws.x + gx - 0.5f, sy - 0.5f, 1.5f, 1.5f};
                SDL_RenderFillRectF(r, &d);
            }
        }
    }
    void drawWorkspace() {
        drawWsGrid();
        if (wsDirty_ || wsTarget_ != selTarget) { wsDirty_ = true; rebuildWs(); }
        Target* t = selected();
        Blocks* blocks = t ? t->blocks : nullptr;
        auto heads = runningHeads();
        SDL_Rect clip{(int)ui.ws.x, (int)ui.ws.y, (int)ui.ws.w,
                      (int)ui.ws.h};
        SDL_RenderSetClipRect(r, &clip);
        float s = wsScale();
        for (auto& lb : wsLayout_.blocks) {
            // ドラッグ中のブロックは飛ばす（ゴーストで描く）
            if (drag.active && !drag.fromFlyout) {
                std::set<std::string> sub;
                bl::collectSubtree(blocks, drag.headId, sub);
                if (sub.count(lb.id)) continue;
            }
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
                std::vector<bl::LBlock> chain;
                bl::layoutChain(blocks, drag.headId, drag.mx, drag.my, tr(),
                                chain, 0);
                for (auto& lb : chain) {
                    bl::BlockStyle st = bl::styleOf(blocks, lb.block);
                    bl::drawLBlock(r, lb, blocks, tr(s), st, panX / s,
                                   panY / s, s, false, false, 0.85f);
                }
            }
        }
        SDL_RenderSetClipRect(r, nullptr);
        // ズームコントロール（ワークスペース右下）
        auto zbtn = [&](FRect rr, const char* txt) {
            rrFill(r, rr.x, rr.y, rr.w, rr.h, 14, kPanel);
            rrOutline(r, rr.x, rr.y, rr.w, rr.h, 14, kBorder, 1.2f);
            float tw = textW(txt, 14, true);
            drawText(rr.x + (rr.w - tw) / 2, rr.y + 6, txt, 14, kFg, true);
        };
        zbtn(ui.zoomPlus, "+");
        zbtn(ui.zoomMinus, "−");
        zbtn(ui.zoomReset, "=");
        // ゴミ箱（Scratch はツールボックス下）
        rrFill(r, ui.trash.x, ui.trash.y, ui.trash.w, ui.trash.h, 8,
               drag.active ? Color{0xE6, 0x4C, 0x4C, 0x88}
                           : Color{0xFF, 0xFF, 0xFF, 0x88});
        rrOutline(r, ui.trash.x, ui.trash.y, ui.trash.w, ui.trash.h, 8, kBorder);
        // 簡易バケツアイコン
        float cx = ui.trash.x + ui.trash.w / 2;
        float cy = ui.trash.y + 14;
        fill(r, cx - 11, cy, 22, 3, kDim);          // 蓋
        fill(r, cx - 5, cy - 4, 10, 4, kDim);       // 持ち手
        // バケツ本体（台形）
        Path bucket;
        bucket.moveTo(cx - 9, cy + 3);
        bucket.lineTo(cx + 9, cy + 3);
        bucket.lineTo(cx + 6, cy + 26);
        bucket.lineTo(cx - 6, cy + 26);
        fillPath(r, bucket, 1.f, kDim);
    }

    void drawStageMenu() {
        // ステージヘッダー: Scratch 3.0 では背景は透明 ($ui-primary)
        // 緑旗 (icon--green-flag.svg)
        bl::drawBlockIcon(r, "gf_button", flagRect.x + 2, flagRect.y + 2, 24, 24, 1.0f);
        // ストップ (icon--stop-all.svg)
        bl::drawBlockIcon(r, "stop_button", stopRect.x + 2, stopRect.y + 2, 24, 24,
                          (rt && !rt->threads.empty()) ? 1.0f : 0.6f);
        // フルスクリーンアイコン（右端 — 実 SVG fullscreen/unfullscreen）
        {
            float fx = ui.stageMenu.x + ui.stageMenu.w - 36;
            float fy = ui.stageMenu.y + 8;
            rrFill(r, fx, fy, 28, 28, 4, Color{255, 255, 255, 255});
            rrOutline(r, fx, fy, 28, 28, 4, Color{0, 0, 0, 0x22}, 1);
            bl::drawBlockIcon(r, stageLarge_ ? "unfullscreen" : "fullscreen",
                              fx + 6, fy + 6, 16, 16, 0.8f);
            fullScrRect_ = {fx, fy, 28, 28};
        }
        // プロジェクト実行中表示（右に小テキスト）
        if (rt && !rt->threads.empty())
            drawText(ui.stageMenu.x + ui.stageMenu.w - 110,
                     ui.stageMenu.y + 14, "running", 10, kDim);
    }
    FRect fullScrRect_{0, 0, 0, 0};
    void drawStage() {
        // ステージは SdlRenderer::draw() が描く（initEmbedded + stageRect）
        SDL_Rect clip{(int)ui.stage.x, (int)ui.stage.y, (int)ui.stage.w,
                      (int)ui.stage.h};
        IRect sr{(int)ui.stage.x, (int)ui.stage.y, (int)ui.stage.w,
                 (int)ui.stage.h};
        ren.setStageRect(sr);
        SDL_RenderSetClipRect(r, &clip);
        ren.draw();
        SDL_RenderSetClipRect(r, nullptr);
        // 白枠
        rrOutline(r, ui.stage.x, ui.stage.y, ui.stage.w, ui.stage.h, 4,
                  kBorder, 1);
    }
    /// スプライト情報パネルの 6 フィールドの矩形（which: 0=name 1=x 2=y
    /// 3=Show 4=Direction 5=Size — Scratch 大ステージ配置:
    /// 行1 = name(130), x(56), y(56)
    /// 行2 = Show(72), Size(64), Direction(72)）
    FRect spFieldRect(int which) {
        float x = ui.spriteInfo.x + 64, y = ui.spriteInfo.y + 10;
        static const float kW[6] = {130.f, 56.f, 56.f, 72.f, 72.f, 64.f};
        const int rows[2][3] = {{0, 1, 2}, {3, 5, 4}};
        for (int rw = 0; rw < 2; ++rw) {
            float fx = x;
            for (int i = 0; i < 3; ++i) {
                int w2 = rows[rw][i];
                if (w2 == which)
                    return {fx, y + 16.f + (float)rw * 38.f, kW[w2], rw ? 32.f : 24.f};
                fx += kW[w2] + 10.f;
            }
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
        for (int i = 0; i < 6; ++i)
            if (hit(spFieldRect(i), mx, my)) return i;
        return -1;
    }
    void drawSpriteInfo() {
        fillR(ui.spriteInfo, kPanel);
        outlineR(ui.spriteInfo, kBorder);
        Target* t = selected();
        if (!t) return;
        // ステージ選択時はパネル全体を無効表示（scratch-gui:
        // sprites[selectedId] が無い → spriteInfoDisabled=true）
        bool disabled = t->isStage;
        float x = ui.spriteInfo.x + 10, y = ui.spriteInfo.y + 10;
        // サムネイル（左上 48x48）
        rrFill(r, x, y, 48, 48, 8, kSel);
        double lw = 0, lh = 0, rcx = 0, rcy = 0;
        if (SDL_Texture* tex = ren.spriteTexture(t, &lw, &lh, &rcx, &rcy)) {
            float sc = std::min(40.f / (float)lw, 40.f / (float)lh);
            SDL_FRect dst{x + 24 - (float)lw * sc / 2,
                          y + 24 - (float)lh * sc / 2,
                          (float)lw * sc, (float)lh * sc};
            SDL_SetTextureAlphaMod(tex, disabled ? 128 : 255);
            SDL_RenderCopyF(r, tex, nullptr, &dst);
            SDL_SetTextureAlphaMod(tex, 255);
        }
        const char* labels[6] = {"Sprite", "x", "y",
                                 "Show", "Direction", "Size"};
        std::string vals[6] = {utf16ToUtf8(t->getName()), fmtNum(t->x),
                               fmtNum(t->y), "",
                               fmtNum(t->direction), fmtNum(t->size)};
        if (disabled)
            for (auto& v : vals) v.clear();
        for (int i = 0; i < 6; ++i) {
            FRect fr = spFieldRect(i);
            drawText(fr.x, fr.y - 13, labels[i], 10, kDim);
            bool foc = spEdit_.active && spEdit_.which == i;
            std::string dispVal = vals[i];
            if (foc) {
                dispVal = spEdit_.buf;
            } else if (i == 4 && dirPicker_.open) {
                dispVal = fmtNum(t->direction);
            }
            if (i == 3) {
                // Show は 2 つのトグルボタン (Show 目 / Hide 斜線目)
                bool showSel = !disabled && t->visible;
                bool hideSel = !disabled && !t->visible;
                float bw = fr.w / 2.f;
                // 左ボタン: Show
                FRect bLeft{fr.x, fr.y, bw, fr.h};
                rrFill(r, bLeft.x, bLeft.y, bLeft.w, bLeft.h, 4,
                       showSel ? kSel : Color{255, 255, 255, 255});
                rrOutline(r, bLeft.x, bLeft.y, bLeft.w, bLeft.h, 4,
                          showSel ? kLooksSecondary : kBorder,
                          showSel ? 1.5f : 1.f);
                bl::drawBlockIcon(r, "spr_show", bLeft.x + (bw - 18) / 2.f,
                                  bLeft.y + (fr.h - 18) / 2.f, 18, 18,
                                  showSel ? 1.0f : 0.45f);

                // 右ボタン: Hide
                FRect bRight{fr.x + bw, fr.y, bw, fr.h};
                rrFill(r, bRight.x, bRight.y, bRight.w, bRight.h, 4,
                       hideSel ? kSel : Color{255, 255, 255, 255});
                rrOutline(r, bRight.x, bRight.y, bRight.w, bRight.h, 4,
                          hideSel ? kLooksSecondary : kBorder,
                          hideSel ? 1.5f : 1.f);
                bl::drawBlockIcon(r, "spr_hide", bRight.x + (bw - 18) / 2.f,
                                  bRight.y + (fr.h - 18) / 2.f, 18, 18,
                                  hideSel ? 1.0f : 0.45f);
            } else {
                rrFill(r, fr.x, fr.y, fr.w, fr.h, 4, Color{255, 255, 255, 255});
                rrOutline(r, fr.x, fr.y, fr.w, fr.h, 4, kBorder, 1);
                if (foc) rrOutline(r, fr.x, fr.y, fr.w, fr.h, 4, kAccent, 2.0f);
                float tx = fr.x + 8;
                if (i == 1 || i == 2) {
                    bl::drawBlockIcon(r, i == 1 ? "spr_x" : "spr_y",
                                      fr.x + 4, fr.y + (fr.h - 14) / 2.f, 14, 14, 0.8f);
                    tx += 18;
                }
                drawText(tx, fr.y + (fr.h - 12) / 2.f, dispVal, 11, kFg);
            }
        }
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
        // スプライトタイル（グリッド）
        float x = ui.spriteSel.x + 8, y = ui.spriteSel.y + 8;
        const float tw = 60, th = 64, gap = 8;
        int cols = std::max(1, (int)((ui.spriteSel.w - 16) / (tw + gap)));
        int i = 0;
        for (size_t ti = 0; ti < rt->targets.size(); ++ti) {
            Target* t = rt->targets[ti];
            if (t->isStage) continue; // Stage は専用カードで描画
            if (i > 0 && i % cols == 0) { x = ui.spriteSel.x + 8; y += th + gap; }
            bool sel = (int)ti == selTarget;
            rrFill(r, x, y, tw, th, 6, sel ? kPanel : Color{0xFA, 0xFC, 0xFF, 255});
            rrOutline(r, x, y, tw, th, 6, sel ? kLooksSecondary : kBorder, sel ? 2.f : 1.f);
            if (sel) {
                // Scratch の薄紫グローシャドウ効果
                rrOutline(r, x - 2, y - 2, tw + 4, th + 4, 8,
                          Color{0x85, 0x5C, 0xD6, 0x33}, 2.f);
            }
            // サムネイル
            double lw = 0, lh = 0, rcx = 0, rcy = 0;
            if (SDL_Texture* tex = ren.spriteTexture(t, &lw, &lh, &rcx, &rcy)) {
                float sc = std::min(44.f / (float)lw, 36.f / (float)lh);
                SDL_FRect dst{x + tw / 2.f - (float)lw * sc / 2.f,
                              y + 6.f + 18.f - (float)lh * sc / 2.f,
                              (float)lw * sc, (float)lh * sc};
                SDL_RenderCopyF(r, tex, nullptr, &dst);
            }
            // 下部ネームバー（選択時は紫背景 + 白文字）
            FRect nb{x, y + th - 18, tw, 18};
            if (sel) {
                rrFill(r, nb.x, nb.y, nb.w, nb.h, 4, kLooksSecondary);
            }
            std::string nm = utf16ToUtf8(t->getName());
            int nw = textW(nm, 9);
            drawText(x + (tw - nw) / 2.f, y + th - 14, nm, 9,
                     sel ? Color{255, 255, 255, 255} : kFg);
            x += tw + gap;
            i++;
        }
        // + スプライト追加ボタン（右下、実 SVG の猫アイコン）
        float bx = ui.spriteSel.x + ui.spriteSel.w - 44;
        float by = ui.spriteSel.y + ui.spriteSel.h - 44;
        discFill(r, bx + 18, by + 18, 18, kAccent);
        bl::drawBlockIcon(r, "addsprite", bx + 8, by + 8, 20, 20, 1.0f);
        addSpriteRect_ = {bx, by, 36, 36};

        // ---- 右側 Stage 選択カード (ui.stageSel) ----
        Target* stageTgt = nullptr;
        for (Target* tg : rt->targets) if (tg->isStage) { stageTgt = tg; break; }
        if (stageTgt) {
            bool stageSel = (selTarget >= 0 && (size_t)selTarget < rt->targets.size() &&
                             rt->targets[selTarget]->isStage);
            fillR(ui.stageSel, kPanel);
            outlineR(ui.stageSel, stageSel ? kLooksSecondary : kBorder);
            // ヘッダー ("Stage")
            FRect hRect{ui.stageSel.x, ui.stageSel.y, ui.stageSel.w, 24};
            rrFill(r, hRect.x, hRect.y, hRect.w, hRect.h, 4,
                   stageSel ? kLooksSecondary : Color{255, 255, 255, 255});
            float thw = textW("Stage", 10, true);
            drawText(hRect.x + (hRect.w - thw) / 2.f, hRect.y + 7, "Stage", 10,
                     stageSel ? Color{255, 255, 255, 255} : kFg, true);
            // サムネイル
            double lw = 0, lh = 0, rcx = 0, rcy = 0;
            if (SDL_Texture* tex = ren.spriteTexture(stageTgt, &lw, &lh, &rcx, &rcy)) {
                float sc = std::min(54.f / (float)lw, 42.f / (float)lh);
                SDL_FRect dst{ui.stageSel.x + (ui.stageSel.w - (float)lw * sc) / 2.f,
                              ui.stageSel.y + 30.f,
                              (float)lw * sc, (float)lh * sc};
                SDL_RenderCopyF(r, tex, nullptr, &dst);
                rrOutline(r, dst.x, dst.y, dst.w, dst.h, 2, kBorder, 1);
            }
            // "Backdrops" ラベルと枚数
            const auto* cList = stageTgt->getCostumes();
            int nBg = cList ? (int)cList->size() : 1;
            drawText(ui.stageSel.x + (ui.stageSel.w - textW("Backdrops", 9)) / 2.f,
                     ui.stageSel.y + 78, "Backdrops", 9, kDim);
            drawText(ui.stageSel.x + (ui.stageSel.w - textW(std::to_string(nBg), 10, true)) / 2.f,
                     ui.stageSel.y + 92, std::to_string(nBg), 10, kFg, true);
            // + 背景追加ボタン（下部中央）
            float bbx = ui.stageSel.x + (ui.stageSel.w - 36) / 2.f;
            float bby = ui.stageSel.y + ui.stageSel.h - 44;
            discFill(r, bbx + 18, bby + 18, 18, kLooksSecondary);
            bl::drawBlockIcon(r, "addbackdrop", bbx + 8, bby + 8, 20, 20, 1.0f);
            addBackdropRect_ = {bbx, bby, 36, 36};
        }
    }
    FRect addSpriteRect_{0, 0, 0, 0};
    FRect addBackdropRect_{0, 0, 0, 0};

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

    void drawBackpack() {
        if (bpOpen) {
            fillR(ui.bpPanel, kBody);
            outlineR(ui.bpPanel, kBorder);
            float x = ui.bpPanel.x + 8, y = ui.bpPanel.y + 8;
            for (int i = 0; i < (int)backpack.size(); ++i) {
                float cw = std::min(160.f, textW(backpack[i].label, 11) + 20.f);
                rrFill(r, x, y, cw, 26, 13, kPanel);
                rrOutline(r, x, y, cw, 26, 13, kBorder);
                drawText(x + 10, y + 7, backpack[i].label, 11, kFg);
                x += cw + 6;
            }
            if (backpack.empty())
                drawText(x, y + 6, "Backpack is empty — right-click a block → Copy to Backpack",
                         11, kDim);
        }
        // 開閉バー
        fillR(ui.bpBar, kPanel);
        outlineR(ui.bpBar, kBorder);
        // 中央のハンドル
        float cx = ui.bpBar.x + ui.bpBar.w / 2;
        rrFill(r, cx - 30, ui.bpBar.y + 6, 60, 12, 6, kTabBg);
        drawText(cx - 24, ui.bpBar.y + 7, bpOpen ? "▾ backpack" : "▴ backpack",
                 9, kDim, true);
    }
    void drawCostumes() {
        Target* t = selected();
        fillR(ui.body, kWsBg);
        if (!t) return;
        const std::vector<Costume>* cs = t->getCostumes();
        if (!cs || cs->empty()) {
            drawText(ui.body.x + 20, ui.body.y + 20, "(no costumes)", 12, kDim);
            return;
        }
        // 左: コスチュームタイル列（縦並び、scratch-gui 準拠）
        float x = ui.body.x + 16, y = ui.body.y + 16;
        float x0 = x;
        float listW = 300;   // タイル列の幅上限
        for (size_t i = 0; i < cs->size(); ++i) {
            const Costume& c = (*cs)[i];
            bool sel = (int)i == t->currentCostume;
            rrFill(r, x, y, 96, 96, 8, sel ? kSel : kPanel);
            if (sel) rrOutline(r, x, y, 96, 96, 8, kAccent, 2);
            double lw = 0, lh = 0;
            if (SDL_Texture* tex = ren.costumeTexture(&c, &lw, &lh)) {
                float sc = std::min(80.f / (float)lw, 68.f / (float)lh);
                SDL_FRect dst{x + 48 - (float)lw * sc / 2,
                              y + 8 + 34 - (float)lh * sc / 2,
                              (float)lw * sc, (float)lh * sc};
                SDL_RenderCopyF(r, tex, nullptr, &dst);
            }
            std::string nm = utf16ToUtf8(c.name);
            int nw = textW(nm, 10);
            drawText(x + (96 - nw) / 2, y + 76, nm, 10, kFg);
            x += 108;
            if (x + 96 > ui.body.x + listW) { x = x0; y += 108; }
        }
        // 右: 選択コスチュームのプレビュー + 情報（ペイントエディタ領域の代替）
        const Costume& sel = (*cs)[std::clamp(t->currentCostume, 0,
                                             (int)cs->size() - 1)];
        float px = ui.body.x + listW + 32, py = ui.body.y + 32;
        float pw = ui.body.x + ui.body.w - px - 32;
        float ph = ui.body.y + ui.body.h - py - 60;
        if (pw > 40 && ph > 40) {
            rrFill(r, px, py, pw, ph, 8, kPanel);
            rrOutline(r, px, py, pw, ph, 8, kBorder, 1);
            double lw = 0, lh = 0;
            if (SDL_Texture* tex = ren.costumeTexture(&sel, &lw, &lh)) {
                float sc = std::min((pw - 40) / (float)lw,
                                    (ph - 80) / (float)lh);
                if (sc > 4.f) sc = 4.f;
                SDL_FRect dst{px + pw / 2 - (float)lw * sc / 2,
                              py + (ph - 60) / 2 - (float)lh * sc / 2,
                              (float)lw * sc, (float)lh * sc};
                SDL_RenderCopyF(r, tex, nullptr, &dst);
            }
            // 情報行（名前・形式・寸法）
            std::string info = utf16ToUtf8(sel.name) + "  —  " +
                sel.dataFormat + "  " +
                std::to_string(sel.bitmapResolution) + "x";
            drawText(px + 16, py + ph - 32, info, 11, kDim);
        }
    }
    void drawSounds() {
        Target* t = selected();
        fillR(ui.body, kWsBg);
        if (!t || !t->sprite) return;
        float y = ui.body.y + 16;
        for (const Sound& s : t->sprite->sounds) {
            rrFill(r, ui.body.x + 16, y, ui.body.w - 32, 40, 6, kPanel);
            rrOutline(r, ui.body.x + 16, y, ui.body.w - 32, 40, 6, kBorder);
            drawText(ui.body.x + 28, y + 12, utf16ToUtf8(s.name), 12, kFg);
            drawText(ui.body.x + ui.body.w - 90, y + 14, s.dataFormat, 10, kDim);
            y += 48;
        }
        if (t->sprite->sounds.empty())
            drawText(ui.body.x + 20, ui.body.y + 20, "(no sounds)", 12, kDim);
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
        int mh = (int)menu_.items.size() * menu_.itemH + 4;
        fill(r, menu_.x, menu_.y, (float)menu_.w, (float)mh,
             Color{0xFF, 0xFF, 0xFF, 250});
        rrOutline(r, menu_.x, menu_.y, (float)menu_.w, (float)mh, 6, kBorder, 1);
        for (int i = 0; i < (int)menu_.items.size(); ++i) {
            float iy = menu_.y + 2 + i * menu_.itemH;
            if (i == menu_.hover)
                fill(r, menu_.x + 2, iy, menu_.w - 4, (float)menu_.itemH, kSel);
            drawText(menu_.x + 10, iy + 6, menu_.items[i], 11, kFg);
        }
    }
    void drawCtx() {
        if (!ctx_.open) return;
        int mh = (int)ctx_.items.size() * ctx_.itemH + 4;
        fill(r, ctx_.x, ctx_.y, (float)ctx_.w, (float)mh,
             Color{0xFF, 0xFF, 0xFF, 250});
        rrOutline(r, ctx_.x, ctx_.y, (float)ctx_.w, (float)mh, 6, kBorder, 1);
        for (int i = 0; i < (int)ctx_.items.size(); ++i) {
            float iy = ctx_.y + 2 + i * ctx_.itemH;
            if (i == ctx_.hover)
                fill(r, ctx_.x + 2, iy, ctx_.w - 4, (float)ctx_.itemH, kSel);
            drawText(ctx_.x + 10, iy + 5, ctx_.items[i], 11, kFg);
        }
    }
    void drawTopMenu() {
        if (!topMenu_.open) return;
        auto items = topMenuList(topMenu_.which);
        int mh = (int)items.size() * topMenu_.itemH + 4;
        rrFill(r, topMenu_.x, topMenu_.y, (float)topMenu_.w, (float)mh, 6,
               Color{0xFF, 0xFF, 0xFF, 252});
        rrOutline(r, topMenu_.x, topMenu_.y, (float)topMenu_.w, (float)mh, 6,
                  kBorder, 1);
        for (int i = 0; i < (int)items.size(); ++i) {
            float iy = topMenu_.y + 2 + i * topMenu_.itemH;
            if (i == topMenu_.hover)
                rrFill(r, topMenu_.x + 2, iy, topMenu_.w - 4,
                       (float)topMenu_.itemH, 4, kSel);
            drawText(topMenu_.x + 12, iy + 8, items[i], 11, kFg);
        }
    }
    int topMenuItemAt(float mx, float my) {
        if (!topMenu_.open) return -1;
        if (mx < topMenu_.x || mx >= topMenu_.x + topMenu_.w) return -1;
        int rel = (int)(my - (topMenu_.y + 2));
        if (rel < 0) return -1;
        int i = rel / topMenu_.itemH;
        return i < (int)topMenuList(topMenu_.which).size() ? i : -1;
    }
    int ctxItemAt(float mx, float my) {
        if (!ctx_.open) return -1;
        if (mx < ctx_.x || mx >= ctx_.x + ctx_.w) return -1;
        int rel = (int)(my - (ctx_.y + 2));
        if (rel < 0) return -1;
        int i = rel / ctx_.itemH;
        return i < (int)ctx_.items.size() ? i : -1;
    }
    void runCtxItem(int i) {
        if (i < 0 || i >= (int)ctx_.items.size()) return;
        std::string item = ctx_.items[i];
        std::string bid = ctx_.blockId;
        float wx = ctx_.wx, wy = ctx_.wy;
        ctx_.open = false;
        if (item == "Duplicate") duplicateBlock(bid);
        else if (item == "Delete") deleteBlockChain(bid);
        else if (item == "Copy to Backpack") backpackAdd(bid);
        else if (item == "Undo") undo();
        else if (item == "Redo") redo();
        else if (item == "Paste") {
            if (!backpack.empty()) backpackPaste(0);
        } else if (item == "Add Comment" || item == "Edit Comment") {
            startEditingComment(bid);
        } else if (item == "Delete Comment") {
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

    // ---- 全体描画 -------------------------------------------------------------
    void draw() {
        SDL_SetRenderDrawColor(r, kBody.r, kBody.g, kBody.b, 255);
        SDL_RenderClear(r);
        layoutUi();
        drawMenuBar();
        if (!stageLarge_) {
            drawTabs();
            if (tab == 0) {
                drawToolbox();
                drawFlyout();
                drawWorkspace();
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
        drawMakeBlock();
        SDL_RenderPresent(r);
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
        // 背景を暗転
        fill(r, 0, 0, (float)W, (float)H, Color{0, 0, 0, 120});
        FRect dlg{W / 2 - 240.f, H / 2 - 160.f, 480, 320};
        rrFill(r, dlg.x, dlg.y, dlg.w, dlg.h, 8,
               Color{255, 255, 255, 255});
        rrOutline(r, dlg.x, dlg.y, dlg.w, dlg.h, 8, kBorder, 1);
        // 左カラム（ピンク）: プレビュー
        FRect left{dlg.x, dlg.y, 190, dlg.h};
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
        if (bl::LBlock* lb = wsBlockAt(mx, my)) {
            ctx_.blockId = lb->id;
            selBlock = lb->id;
            bool hasCmt = lb->block && !lb->block->comment.empty();
            ctx_.items = {"Duplicate", "Delete", "Copy to Backpack",
                          hasCmt ? "Edit Comment" : "Add Comment"};
            if (hasCmt) ctx_.items.push_back("Delete Comment");
        } else {
            ctx_.blockId.clear();
            ctx_.items = {"Undo", "Redo"};
            if (!backpack.empty()) ctx_.items.push_back("Paste");
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
            break;
        case SDL_MOUSEBUTTONDOWN: {
            float mx = (float)ev.button.x, my = (float)ev.button.y;
            menu_.open = false;
            if (ev.button.button == SDL_BUTTON_RIGHT) {
                if (hit(ui.ws, mx, my)) { openContextMenu(mx, my); break; }
                break;
            }
            if (ev.button.button == SDL_BUTTON_MIDDLE) {
                panning = true;
                panStartX = mx; panStartY = my;
                panPX = panX; panPY = panY;
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
            // メニューバー
            if (topMenu_.open) {
                int i = topMenuItemAt(mx, my);
                if (i >= 0) { runTopMenuItem(i); break; }
                topMenu_.open = false;   // メニュー外クリックで閉じる（以降の処理は続行）
            }
            if (hit(ui.menuBar, mx, my)) {
                if (hit(menuBtnFile, mx, my)) {
                    openTopMenu(0, menuBtnFile.x, ui.menuBar.y + ui.menuBar.h + 2);
                } else if (hit(menuBtnEdit, mx, my)) {
                    openTopMenu(1, menuBtnEdit.x, ui.menuBar.y + ui.menuBar.h + 2);
                } else if (mx >= W - 90) {   // Save
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
            if (hit(fullScrRect_, mx, my)) { stageLarge_ = !stageLarge_; break; }
            // ステージ本体 → ren へ転送
            if (hit(ui.stage, mx, my)) { ren.handleEvent(ev); break; }
            // ステージ選択カード (右下)
            if (hit(ui.stageSel, mx, my)) {
                if (hit(addBackdropRect_, mx, my)) {
                    openTopMenu(4, addBackdropRect_.x - 150,
                                addBackdropRect_.y - 132);
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
                    openTopMenu(3, addSpriteRect_.x - 140,
                                addSpriteRect_.y - 132);
                    break;
                }
                float x = ui.spriteSel.x + 8, y = ui.spriteSel.y + 8;
                const float tw = 60, th = 64, gap = 8;
                int cols = std::max(1, (int)((ui.spriteSel.w - 16) / (tw + gap)));
                int i = 0;
                for (size_t ti = 0; ti < rt->targets.size(); ++ti) {
                    Target* t = rt->targets[ti];
                    if (t->isStage) continue; // Stage は専用カード
                    if (i > 0 && i % cols == 0) { x = ui.spriteSel.x + 8; y += th + gap; }
                    if (mx >= x && mx < x + tw && my >= y && my < y + th) {
                        selTarget = (int)ti;
                        clearHistory();
                        wsDirty_ = true;
                        flyDirty_ = true;
                        break;
                    }
                    x += tw + gap;
                    i++;
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
                        drag.mx = (mx - panX) / wsScale();
                        drag.my = (my - panY) / wsScale();
                        break;
                    }
                }
                break;
            }
            // コスチュームタブ: タイルクリックで切替
            if (tab == 1 && hit(ui.body, mx, my)) {
                Target* t = selected();
                if (t) {
                    const std::vector<Costume>* cs = t->getCostumes();
                    if (cs) {
                        float cx = ui.body.x + 16, cy = ui.body.y + 16;
                        float x0 = cx;
                        for (size_t i = 0; i < cs->size(); ++i) {
                            if (mx >= cx && mx < cx + 96 && my >= cy &&
                                my < cy + 96) {
                                t->setCostume((double)i);
                                break;
                            }
                            cx += 108;
                            if (cx + 96 > ui.body.x + ui.body.w - 8) {
                                cx = x0; cy += 108;
                            }
                        }
                    }
                }
                break;
            }
            // ワークスペース
            if (tab == 0 && hit(ui.ws, mx, my)) {
                if (bl::LBlock* lb = wsBlockAt(mx, my)) {
                    // ドラッグかクリックかは移動量で決める
                    drag.pending = true;
                    drag.fromFlyout = false;
                    drag.headId = lb->id;
                    drag.offX = (mx - panX) / wsScale() - lb->x;
                    drag.offY = (my - panY) / wsScale() - lb->y;
                    drag.mx = lb->x;
                    drag.my = lb->y;
                } else {
                    selBlock.clear();
                }
                break;
            }
            break;
        }
        case SDL_MOUSEMOTION: {
            float mx = (float)ev.motion.x, my = (float)ev.motion.y;
            if (menu_.open) {
                int rel = (int)(my - (menu_.y + 2));
                menu_.hover = (rel >= 0) ? rel / menu_.itemH : -1;
                if (menu_.hover >= (int)menu_.items.size()) menu_.hover = -1;
            }
            if (ctx_.open) ctx_.hover = ctxItemAt(mx, my);
            if (topMenu_.open) topMenu_.hover = topMenuItemAt(mx, my);
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
            if (panning) {
                panX = panPX + (mx - panStartX);
                panY = panPY + (my - panStartY);
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
            if (ev.button.button == SDL_BUTTON_MIDDLE) { panning = false; break; }
            if (dirPicker_.dragging) { dirPicker_.dragging = false; }
            float mx = (float)ev.button.x, my = (float)ev.button.y;
            if (hit(ui.stage, mx, my)) ren.handleEvent(ev);
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
            int mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            float fx = (float)mx, fy = (float)my;
            if (hit(ui.flyout, fx, fy)) {
                flyScroll = std::max(0.f, flyScroll - ev.wheel.y * 30.f);
            } else if (hit(ui.ws, fx, fy)) {
                if (ev.wheel.y != 0) {
                    // Ctrl+wheel でズーム、通常は垂直パン
                    SDL_Keymod m = SDL_GetModState();
                    if (m & KMOD_CTRL) {
                        float z0 = zoom;
                        zoom = std::clamp(zoom * (ev.wheel.y > 0 ? 1.1f : 0.9f),
                                          0.3f, 2.5f);
                        panX -= (fx - panX) * (zoom / z0 - 1);
                        panY -= (fy - panY) * (zoom / z0 - 1);
                    } else {
                        panY += ev.wheel.y * 30;
                        if (ev.wheel.x != 0) panX += ev.wheel.x * 30;
                    }
                }
            }
            break;
        }
        case SDL_TEXTINPUT: {
            if (mb_.open && mb_.editPart >= 0) mb_.buf += ev.text.text;
            else if (edit_.active) edit_.buf += ev.text.text;
            else if (cmt_.active) cmt_.buf += ev.text.text;
            else if (spEdit_.active) spEdit_.buf += ev.text.text;
            else ren.handleEvent(ev);
            break;
        }
        case SDL_KEYDOWN: {
            SDL_Keymod mod = (SDL_Keymod)ev.key.keysym.mod;
            SDL_Keycode sym = ev.key.keysym.sym;
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
            if (edit_.active || cmt_.active || spEdit_.active) {
                if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_TAB) {
                    if (edit_.active) applyEdit();
                    else if (cmt_.active) applyCommentEdit();
                    else applySpriteEdit();
                } else if (sym == SDLK_ESCAPE) {
                    edit_.active = false;
                    cmt_.active = false;
                    spEdit_.active = false;
                    SDL_StopTextInput();
                } else if (sym == SDLK_BACKSPACE) {
                    std::string* buf = edit_.active ? &edit_.buf
                                       : cmt_.active ? &cmt_.buf : &spEdit_.buf;
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
            // 保存は deflate 圧縮されるので project.json を展開して確認する。
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

int editorMainLegacy(const char* path) {
    bool ok;
    std::vector<uint8_t> data = loadFile(path, ok);
    if (!ok) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }
    std::unique_ptr<Runtime> rtOwn(new Runtime);
    Sb3LoadResult res = loadSb3File(*rtOwn, data.data(), data.size());
    if (!res.ok) { std::fprintf(stderr, "load failed: %s\n", res.error.c_str()); return 1; }

    Editor ed;
    ed.rt = rtOwn.get();
    ed.path = path;
    ed.buildCategories();
    if (const char* sh = std::getenv("XYO_EDIT_SHOT")) ed.shotPath = sh;
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
    ed.textTarget = ed.r;
    // SdlRenderer を共有レンダラへ埋め込む（ステージは stage rect へ描く）
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
    if (const char* sp = std::getenv("XYO_EDIT_SAVE")) {
        bool ok = saveSb3File(*rtOwn, sp);
        std::fprintf(stderr, "editor: save %s -> %s\n", path, ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }

    using clock = std::chrono::steady_clock;
    const auto kFrame = std::chrono::microseconds(33333);
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
    auto pushInject = [&](const InjEv& ie) {
        SDL_Event ev{};
        if (ie.op[0] == 'm' || ie.op[0] == 'r') {
            bool right = ie.op[0] == 'r';
            uint32_t mask = right ? SDL_BUTTON_RMASK : SDL_BUTTON_LMASK;
            int x = 0, y = 0;
            std::sscanf(ie.arg.c_str(), "%d,%d", &x, &y);
            if (ie.op[1] == 'm') {
                ev.type = SDL_MOUSEMOTION;
                ev.motion.x = x; ev.motion.y = y;
                ev.motion.state = injButtons;
            } else {
                if (ie.op[1] == 'd') injButtons |= mask;
                else injButtons &= ~mask;
                ev.type = ie.op[1] == 'd' ? SDL_MOUSEBUTTONDOWN
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
            if (!std::strcmp(n, "enter")) sym = SDLK_RETURN;
            else if (!std::strcmp(n, "backspace")) sym = SDLK_BACKSPACE;
            else if (!std::strcmp(n, "escape")) sym = SDLK_ESCAPE;
            else if (!std::strcmp(n, "space")) sym = SDLK_SPACE;
            else if (!std::strcmp(n, "delete")) sym = SDLK_DELETE;
            else if (std::strlen(n) == 1) sym = (SDL_Keycode)n[0];
            ev.key.keysym.sym = sym;
        }
        SDL_PushEvent(&ev);
    };
    size_t injIdx = 0;
    int frameCount = 0;
    while (!ed.quit) {
        if (exitFrames > 0 && frameCount >= exitFrames) break;
        ++frameCount;
        auto t0 = clock::now();
        while (injIdx < inject.size() && inject[injIdx].frame <= frameCount) {
            pushInject(inject[injIdx++]);
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (!ed.handleEvent(ev)) { ed.quit = true; break; }
        }
        if (ed.quit) break;
        if (ed.lastSel != ed.selTarget) {
            ed.lastSel = ed.selTarget;
            ed.clearHistory();
        }
        // File → Load from your computer（同じ sb3 を読み直して未保存変更を捨てる）
        if (ed.reloadRequested_) {
            ed.reloadRequested_ = false;
            std::unique_ptr<Runtime> fresh(new Runtime);
            if (loadSb3File(*fresh, data.data(), data.size()).ok) {
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
                } else {
                    ed.rt = rtOwn.get();
                }
            } else {
                std::fprintf(stderr, "editor: reload failed\n");
            }
        }
        ed.rt->step();
        ed.draw();
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
        auto dt = clock::now() - t0;
        if (dt < kFrame) std::this_thread::sleep_for(kFrame - dt);
    }
    for (auto& [k, t] : ed.texCache)
        if (t.t) SDL_DestroyTexture(t.t);
    SDL_DestroyRenderer(ed.r);
    SDL_DestroyWindow(ed.win);
    return 0;
}
} // namespace xyo

#else
namespace xyo {
int editorMainLegacy(const char* path) {
    std::fprintf(stderr, "editor: %s (this build has no SDL2)\n", path);
    return 1;
}
} // namespace xyo
#endif
