// xyo-cpp: Blockly 準拠のブロックレイアウトの実装。
#include "layout.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "label.hpp"
#include "core/sstr.hpp"
#include "model/blocks.hpp"

namespace xyo {
namespace blockly {

// ---------------------------------------------------------------- helpers --

std::string chainTail(Blocks* blocks, const std::string& id) {
    std::string t = id;
    for (int i = 0; i < 100000; i++) {
        Block* b = blocks->getBlock(t);
        if (!b || b->next.empty()) break;
        t = b->next;
    }
    return t;
}

std::string prevOf(Blocks* blocks, const std::string& id) {
    for (auto& [k, b] : blocks->allBlocks())
        if (b.next == id) return k;
    return "";
}

edit::Shape blockShapeOf(Blocks* blocks, const std::string& id) {
    Block* b = blocks ? blocks->getBlock(id) : nullptr;
    if (!b) return edit::Shape::Stack;
    const edit::BlockDef* d = edit::findBlock(b->opcode);
    return d ? d->shape : edit::Shape::Stack;
}

void collectSubtree(Blocks* blocks, const std::string& id,
                    std::set<std::string>& out) {
    if (id.empty() || out.count(id)) return;
    Block* b = blocks->getBlock(id);
    if (!b) return;
    out.insert(id);
    for (auto& [name, in] : b->inputs.items) {
        if (!in.block.empty()) collectSubtree(blocks, in.block, out);
        if (!in.shadow.empty()) collectSubtree(blocks, in.shadow, out);
    }
    if (!b->next.empty()) collectSubtree(blocks, b->next, out);
}

static std::string prettyOpcode(const std::string& op);

BlockStyle styleOf(Blocks* blocks, const Block* b) {
    BlockStyle s;
    if (!b) return s;
    const edit::BlockDef* d = edit::findBlock(b->opcode);
    if (d) {
        s.known = true;
        s.shape = d->shape;
        s.cblock = d->cblock;
        s.label = d->label;
        s.sub2Label = d->sub2Label;
        s.primary = rgb(d->primary);
        s.secondary = rgb(d->secondary);
        s.tertiary = rgb(d->tertiary);
        s.ext = b->opcode.rfind("pen_", 0) == 0 ||
                b->opcode.rfind("music_", 0) == 0;
        // procedures_call/prototype は mutation.proccode がラベル（%s/%b 付き）
        const std::string& op = b->opcode;
        if (op == "procedures_call") {
            s.shape = (b->mutation && b->mutation->isReporterCall())
                          ? edit::Shape::Reporter
                          : edit::Shape::Stack;
            s.label = (b->mutation && b->mutation->has("proccode"))
                          ? b->mutation->attr("proccode")
                          : "custom block";
        } else if (op == "procedures_prototype") {
            s.label = (b->mutation && b->mutation->has("proccode"))
                          ? b->mutation->attr("proccode")
                          : "";
        }
        return s;
    }
    const std::string& op = b->opcode;
    s.ext = op.rfind("pen_", 0) == 0 || op.rfind("music_", 0) == 0;
    if (op.rfind("procedures_", 0) == 0 || op.rfind("argument_", 0) == 0) {
        s.primary = rgb(0xFF6680);
        s.secondary = rgb(0xFF4D6A);
        s.tertiary = rgb(0xFF3355);
        if (op == "procedures_definition") {
            s.shape = edit::Shape::Hat;
            s.label = "define";
        } else if (op == "argument_reporter_string_number") {
            s.shape = edit::Shape::Reporter;
            s.label = "%1";
        } else if (op == "argument_reporter_boolean") {
            s.shape = edit::Shape::Boolean;
            s.label = "%1";
        } else if (op == "procedures_call") {
            s.shape = (b->mutation && b->mutation->isReporterCall())
                          ? edit::Shape::Reporter
                          : edit::Shape::Stack;
            s.label = (b->mutation && b->mutation->has("proccode"))
                          ? b->mutation->attr("proccode")
                          : "custom block";
        } else {
            s.shape = edit::Shape::Stack;
            s.label = (b->mutation && b->mutation->has("proccode"))
                          ? b->mutation->attr("proccode")
                          : op;
        }
        return s;
    }
    // 拡張（pen/music 等、カタログ外）: カテゴリ色 + camelCase を空白区切りにしたラベル
    if (op.rfind("pen_", 0) == 0) {
        s.primary = rgb(0x0FBD8C);
        s.secondary = rgb(0x0DA57A);
        s.tertiary = rgb(0x0B8E69);
        s.shape = edit::Shape::Stack;
        s.label = prettyOpcode(op);
        return s;
    }
    if (op.rfind("music_", 0) == 0) {
        s.primary = rgb(0xCF63CF);
        s.secondary = rgb(0xC94FC9);
        s.tertiary = rgb(0xBD42BD);
        s.shape = edit::Shape::Stack;
        s.label = prettyOpcode(op);
        return s;
    }
    // menu/未知: フィールド値か opcode を見せる小さなピル
    s.shape = edit::Shape::Reporter;
    s.label = !b->fields.empty() ? "%1" : op;
    return s;
}

/// "pen_setPenSizeTo" → "set pen size to"（カタログ外 opcode の表示名）
static std::string prettyOpcode(const std::string& op) {
    std::string s = op;
    size_t p = s.find('_');
    if (p != std::string::npos) s = s.substr(p + 1);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (i > 0 && std::isupper((unsigned char)c)) out += ' ';
        out += (char)std::tolower((unsigned char)c);
    }
    return out;
}

// ---- ラベル分解 ------------------------------------------------------------

struct Tok {
    int arg = -1;
    std::string text;
    bool procStr = false;   // %s（proccode の文字列/数値引数）
    bool procBool = false;  // %b（同真偽値引数）
};

static std::vector<Tok> tokenize(const std::string& label) {
    std::vector<Tok> toks;
    std::string lit;
    auto flush = [&] {
        if (!lit.empty()) { toks.push_back(Tok{-1, lit}); lit.clear(); }
    };
    for (size_t i = 0; i < label.size();) {
        if (label[i] == '%' && i + 1 < label.size() && label[i + 1] >= '1' &&
            label[i + 1] <= '9') {
            flush();
            toks.push_back(Tok{label[i + 1] - '0' - 1, ""});
            i += 2;
        } else if (label[i] == '%' && i + 1 < label.size() &&
                   (label[i + 1] == 's' || label[i + 1] == 'b')) {
            flush();
            Tok tk;
            if (label[i + 1] == 'b') tk.procBool = true;
            else tk.procStr = true;
            toks.push_back(tk);
            i += 2;
        } else {
            lit += label[i];
            i++;
        }
    }
    flush();
    return toks;
}

static bool isBooleanInput(const std::string& name) {
    return name == "CONDITION" || name == "OPERAND" || name == "OPERAND1" ||
           name == "OPERAND2" || name == "BOOL" ||
           name.rfind("CONDITION", 0) == 0;
}

static Block* inputChild(Blocks* blocks, const Block* b,
                         const std::string& name) {
    if (!b) return nullptr;
    const BlockInput* in = b->inputs.find(name);
    if (!in || in->block.empty()) return nullptr;
    return blocks->getBlock(in->block);
}

// ---- 測量（再帰）-----------------------------------------------------------

struct Ctx {
    Blocks* blocks;
    const TextRenderer* tr;
    std::map<std::string, BlockSize> memo;
};

static BlockSize measureBlockImpl(Ctx& cx, const Block* b);

/// shadow ブロック 1 個の計測（白いフィールド or 小さいピル）。
static BlockSize measureShadow(Ctx& cx, const Block* b) {
    const TextRenderer& tr = *cx.tr;
    BlockSize sz;
    std::string fn;
    bool isNum = false;
    float rowH = kShadowH;
    float x = 0;
    if (b->opcode == "procedures_prototype" ||
        b->opcode == "procedures_call" ||
        b->opcode.rfind("argument_", 0) == 0) {
        // prototype/call は shadow フラグ付きでも実ブロック形状
        return measureBlockImpl(cx, b);
    }
    if (b->opcode == "colour_picker") {
        // 色スウォッチ（白枠 + 色）
        LSeg g;
        g.kind = SegKind::Swatch;
        g.field = b->fields.empty() ? "" : b->fields.begin()->first;
        g.text = g.field.empty() ? "" : fieldStr(const_cast<Block*>(b), g.field);
        g.x = 0;
        g.y = 0;
        g.h = rowH;
        g.w = rowH;
        sz.segs.push_back(g);
        x += g.w;
    } else if (shadowIsField(const_cast<Block*>(b), &fn, &isNum)) {
        LSeg g;
        g.kind = isNum ? SegKind::NumField : SegKind::StrField;
        g.input = g.field = fn;
        g.text = fieldStr(const_cast<Block*>(b), fn);
        g.x = 0;
        g.y = 0;
        g.h = rowH;
        g.w = std::max<float>(kFieldMinW,
                              tr.width(g.text, kFieldTextPx) + kFieldPadX);
        sz.segs.push_back(g);
        x += g.w;
    } else {
        // menu / その他 shadow: ドロップダウン表示
        LSeg g;
        g.kind = SegKind::Dropdown;
        g.text = shadowValue(cx.blocks, const_cast<Block*>(b));
        if (!b->fields.empty()) g.field = b->fields.begin()->first;
        g.x = 0;
        g.y = 0;
        g.h = rowH;
        g.w = tr.width(g.text, kFieldTextPx) + kFieldPadX +
              (kArrowW + kArrowPad) + kBoxPadX * 2;
        sz.segs.push_back(g);
        x += g.w;
    }
    sz.w = std::max<float>(kMinShadowW, x);
    sz.h = rowH;
    return sz;
}

static BlockSize measureBlockImpl(Ctx& cx, const Block* b) {
    auto it = cx.memo.find(b->id);
    if (it != cx.memo.end()) return it->second;
    const TextRenderer& tr = *cx.tr;
    BlockStyle st = styleOf(cx.blocks, b);
    BlockSize sz;
    if (b->shadow && b->opcode != "procedures_prototype" &&
        b->opcode != "procedures_call" &&
        b->opcode.rfind("argument_", 0) != 0)
        return cx.memo[b->id] = measureShadow(cx, b);

    bool rep = st.shape == edit::Shape::Reporter ||
               st.shape == edit::Shape::Boolean;
    float rowH = rep ? kRepH : kStackH;
    // 拡張ブロックは 2 グリッド単位（8px）高い（scratch の isScratchExtension）
    if (st.ext) rowH += 8;
    float segY = (rowH - kFieldH) / 2.0f;   // フィールド行の y
    float textY = segY;
    float x = kLabelPadX;
    int order = 0;

    // 拡張ブロックは行頭に 24px アイコンを持つ（field_image、SEP_SPACE_X 区切り）
    if (st.ext) {
        LSeg g;
        g.segOrder = order++;
        g.kind = SegKind::Icon;
        g.text = b->opcode;
        g.x = x;
        g.y = segY + (kFieldH - 24) / 2.0f;
        g.h = 24;
        g.w = kIconW;
        sz.segs.push_back(g);
        x += g.w + kSepX;
    }

    const edit::BlockDef* def = st.known ? edit::findBlock(b->opcode) : nullptr;

    auto textSeg = [&](const std::string& s) {
        if (s.empty()) return;
        LSeg g;
        g.kind = SegKind::Text;
        g.text = s;
        g.segOrder = order++;
        g.x = x;
        g.y = textY;
        g.h = kFieldH;
        // blocklyText は font-weight:500 → 実効 400（regular）で計測
        g.w = (float)tr.width(s, kTextPx, false);
        sz.segs.push_back(g);
        x += g.w + 2;
    };

    // 上に previous 接続を持つブロックでは、値入力の先頭を
    // INPUT_AND_FIELD_MIN_X(48) 以降にしてノッチと重ならないようにする
    bool hasPrev = st.shape == edit::Shape::Stack ||
                   st.shape == edit::Shape::End || st.cblock;
    auto valueSeg = [&](const std::string& name, int arg) {
        if (hasPrev) x = std::max(x, (float)kInputMinX);
        LSeg g;
        g.segOrder = order++;
        g.arg = arg;
        g.input = name;
        g.y = segY;
        g.h = kFieldH;
        Block* child = inputChild(cx.blocks, b, name);
        if (!child) {
            // INPUT_SAME_BLOCK_SHADOW（custom_block の prototype 等）は
            // in.block ではなく in.shadow へ id が入る
            const BlockInput* in = b->inputs.find(name);
            if (in && !in->shadow.empty())
                child = cx.blocks->getBlock(in->shadow);
        }
        bool hex = isBooleanInput(name);
        if (child) {
            if (child->shadow) {
                BlockSize cs = measureShadow(cx, child);
                if (cs.segs.size() > 1) {
                    // prototype 等の複数 seg shadow — そのまま全部並べる
                    float cx0 = x - cs.segs[0].x;
                    for (const LSeg& in : cs.segs) {
                        LSeg sg = in;
                        sg.segOrder = order++;
                        sg.input = name;
                        sg.sh = true;
                        sg.x = in.x + cx0;
                        sg.y += segY;
                        sz.segs.push_back(sg);
                    }
                    x += cs.w + kSepX;
                    return;
                }
                const LSeg& in = cs.segs[0];
                g.kind = in.kind;
                g.text = in.text;
                g.hex = hex;
                g.sh = true;
                g.w = cs.w;
            } else {
                BlockSize cs = measureBlockImpl(cx, child);
                g.kind = hex ? SegKind::BoolInput : SegKind::Input;
                g.hex = hex;
                g.w = cs.w;
                g.h = cs.h;
                g.y = segY - (cs.h - kFieldH) / 2.0f;
                // INLINE_PADDING_Y=4 が上下に付く（child.h + 2*4）
                float need = cs.h + 2 * 4;
                if (need > rowH) rowH = need;
            }
        } else {
            g.kind = SegKind::Socket;
            g.hex = hex;
            // scratch-blocks getInputShapeInfo_: text shadow 入力は
            // OUTPUT_SHAPE_SQUARE（角丸四角）、数値系は ROUND（ピル）。
            bool square = false;
            if (!hex && def && arg >= 0 && arg < (int)def->args.size() &&
                def->args[arg].kind == edit::ArgKind::Value &&
                def->args[arg].shadow.rfind("text", 0) == 0)
                square = true;
            g.square = square;
            g.w = hex ? (float)kBoolSocketW
                      : square ? 40.f : (float)kBoolSocketW;
        }
        g.x = x;
        sz.segs.push_back(g);
        x += g.w + kSepX;
    };

    // フィールドの EDITABLE に対応（field.js: default true、
    // false は Label/Image/VariableGetter/VerticalSeparator のみ）。
    auto editable = [](SegKind k) {
        return k == SegKind::NumField || k == SegKind::StrField ||
               k == SegKind::Dropdown || k == SegKind::Variable ||
               k == SegKind::Swatch;
    };
    // 行頭のノッチ重なり回避（renderFields_: Label と Image 以外は
    // previous 接続持ちで INPUT_AND_FIELD_MIN_X=48 へ寄せる）。
    auto bumpable = [](SegKind k) {
        return k != SegKind::Text && k != SegKind::Icon;
    };

    int procArgIdx = 0;
    // procedures_call の %s/%b: mutation.argumentIds の順で対応入力を置く
    auto procArgSeg = [&](bool isBool) {
        std::string iname;
        if (b->mutation) {
            b->mutation->parseArgs();
            if (procArgIdx < (int)b->mutation->argumentIds.size())
                iname = b->mutation->argumentIds[procArgIdx];
        }
        if (iname.empty()) iname = "arg" + std::to_string(procArgIdx);
        ++procArgIdx;
        if (isBool && !b->inputs.has(iname)) {
            LSeg g;
            g.segOrder = order++;
            g.arg = -1;
            g.input = iname;
            g.kind = SegKind::Socket;
            g.hex = true;
            g.w = (float)kBoolSocketW;
            g.x = x;
            g.y = segY;
            g.h = kFieldH;
            sz.segs.push_back(g);
            x += g.w + kSepX;
            return;
        }
        valueSeg(iname, -1);
    };

    bool isProto = b->opcode == "procedures_prototype";

    if (def) {
        std::vector<bool> argUsed(def->args.size(), false);
        for (auto& t : tokenize(st.label)) {
            if (t.procStr || t.procBool) {
                if (isProto) {
                    // プロトタイプ: 引数名ピル（%b は六角）
                    std::string nm;
                    if (b->mutation) {
                        b->mutation->parseArgs();
                        if (procArgIdx <
                            (int)b->mutation->argumentNames.size())
                            nm = SStr::toUtf8(
                                b->mutation->argumentNames[procArgIdx]);
                    }
                    ++procArgIdx;
                    if (nm.empty()) nm = t.procBool ? "?" : "name";
                    LSeg g;
                    g.segOrder = order++;
                    g.kind = SegKind::Variable;
                    g.text = nm;
                    g.x = x;
                    g.y = segY;
                    g.h = kFieldH;
                    g.w = tr.width(nm, kFieldTextPx) + kBoxPadX * 2 +
                          (t.procBool ? 8 : 0);
                    sz.segs.push_back(g);
                    x += g.w + kSepX;
                } else {
                    procArgSeg(t.procBool);
                }
                continue;
            }
            if (t.arg < 0) { textSeg(t.text); continue; }
            if (t.arg >= (int)def->args.size()) continue;
            argUsed[t.arg] = true;
            const edit::ArgDef& a = def->args[t.arg];
            LSeg g;
            g.segOrder = order++;
            g.arg = t.arg;
            g.input = a.name;
            g.field = a.name;
            g.x = x;
            g.y = segY;
            g.h = kFieldH;
            switch (a.kind) {
            case edit::ArgKind::Value:
                valueSeg(a.name, t.arg);
                continue;
            case edit::ArgKind::Number:
            case edit::ArgKind::Angle: {
                g.kind = SegKind::NumField;
                g.text = fieldStr(const_cast<Block*>(b), a.name);
                if (g.text.empty()) g.text = a.dflt;
                g.w = std::max<float>(kFieldMinW,
                                      tr.width(g.text, kFieldTextPx) +
                                          kFieldPadX);
                break;
            }
            case edit::ArgKind::Dropdown: {
                g.kind = SegKind::Dropdown;
                std::string v = fieldStr(const_cast<Block*>(b), a.name);
                g.text = v;
                for (size_t i = 0; i + 1 < a.options.size(); i += 2)
                    if (a.options[i + 1] == v) g.text = a.options[i];
                if (g.text.empty() && !a.options.empty()) g.text = a.options[0];
                // text + EDITABLE(6) + arrow(12+8) + box(2*8)
                g.w = tr.width(g.text, kFieldTextPx) + kFieldPadX +
                      (kArrowW + kArrowPad) + kBoxPadX * 2;
                break;
            }
            case edit::ArgKind::Variable: {
                g.kind = SegKind::Variable;
                g.list = (a.name == "LIST");
                g.text = fieldStr(const_cast<Block*>(b), a.name);
                if (g.text.empty())
                    g.text = g.list ? "my list" : "my variable";
                g.w = tr.width(g.text, kFieldTextPx) + kFieldPadX +
                      (kArrowW + kArrowPad) + kBoxPadX * 2;
                break;
            }
            case edit::ArgKind::Image: {
                g.kind = SegKind::Icon;
                g.text = a.dflt.empty() ? a.name : a.dflt;
                // 画像名が取れない（生成時未保存）ときは opcode で判定
                if (g.text.empty()) g.text = b->opcode;
                g.y = segY + (kFieldH - 24) / 2.0f;
                g.h = 24;
                g.w = kIconW;
                break;
            }
            default: continue;
            }
            sz.segs.push_back(g);
            x += g.w + kSepX;
        }
        // ラベルの %N に現れない Image 引数は行末へ追加
        // （scratch の field_image: repeat/forever/while の矢印など）
        for (size_t i = 0; i < def->args.size(); i++) {
            if (argUsed[i] || def->args[i].kind != edit::ArgKind::Image)
                continue;
            LSeg g;
            g.segOrder = order++;
            g.kind = SegKind::Icon;
            g.input = def->args[i].name;
            g.text = b->opcode;
            g.x = x;
            g.y = segY + (kFieldH - 24) / 2.0f;
            g.h = 24;
            g.w = kIconW;
            sz.segs.push_back(g);
            x += g.w + kSepX;
        }
    } else {
        // カタログ外（menu/拡張その他）: inputs と fields を順に並べる。
        // %s/%b があれば argumentIds 順で差し込み、無ければ inputs 全展開。
        bool handledProcTok = false;
        for (auto& t : tokenize(st.label)) {
            if (t.procStr || t.procBool) {
                handledProcTok = true;
                procArgSeg(t.procBool);
            } else if (t.arg < 0) {
                textSeg(t.text);
            }
        }
        if (!handledProcTok) {
            bool first = true;
            for (auto& [name, in] : b->inputs.items) {
                if (name.rfind("SUBSTACK", 0) == 0) continue;
                if (!first) { /* 区切り */ }
                valueSeg(name, -1);
                first = false;
            }
        }
        for (auto& [name, f] : b->fields) {
            LSeg g;
            g.segOrder = order++;
            g.field = name;
            g.text = fieldStr(const_cast<Block*>(b), name);
            g.x = x;
            g.y = segY;
            g.h = kFieldH;
            // #rrggbb の値はカラースウォッチ（pen の色箱）
            if (g.text.size() == 7 && g.text[0] == '#') {
                g.kind = SegKind::Swatch;
                g.w = kFieldH;
            } else {
                g.kind = SegKind::Variable;
                g.w = tr.width(g.text, kFieldTextPx) + kBoxPadX + kArrowW +
                      kArrowPad;
            }
            sz.segs.push_back(g);
            x += g.w + kSepX;
        }
    }

    // 区間の x/y を確定する（renderFields_/renderDrawRight_ 準拠）:
    // ・隣接区間の間は SEP_SPACE_X(8)。直前と直後が共に EDITABLE なら +8
    // ・previous 接続持ちでは Label/Image 以外を INPUT_AND_FIELD_MIN_X(48) へ
    // ・全区間を確定行高 rowH の中央に置く（yOffset=-h/2 相当。
    //   行高が入れ子で伸びても入れ子ブロックがずれない）
    // （ループ中の x/g.x/g.y は暫定値で、ここで全て上書きする）
    float contentW = x;
    {
        float px = kLabelPadX;
        for (size_t i = 0; i < sz.segs.size(); i++) {
            LSeg& sg = sz.segs[i];
            if (hasPrev && bumpable(sg.kind))
                px = std::max(px, (float)kInputMinX);
            sg.x = px;
            if (sg.kind == SegKind::Input || sg.kind == SegKind::BoolInput) {
                sg.y = (rowH - sg.h) / 2.0f;
            } else if (sg.kind == SegKind::Icon) {
                sg.y = (rowH - sg.h) / 2.0f;
                // 拡張先頭アイコンは previous 接続持ちで 1GU 下げる
                if (st.ext && i == 0 &&
                    (st.shape == edit::Shape::Stack ||
                     st.shape == edit::Shape::End || st.cblock))
                    sg.y += 4;
            } else {
                sg.y = (rowH - kFieldH) / 2.0f;
            }
            // +8 ボーナスは同一 fieldRow 内のフィールド間のみ。
            // shadow 展開・入力同士は SEP のみ（renderDrawRight_）。
            const LSeg& nx =
                i + 1 < sz.segs.size() ? sz.segs[i + 1] : sg;
            bool ee = editable(sg.kind) && editable(nx.kind) &&
                      !sg.sh && !nx.sh && i + 1 < sz.segs.size();
            px += sg.w + kSepX + (ee ? kSepX : 0);
        }
        contentW = px;  // 末尾 SEP を含む（右パディング）
        x = px;
    }
    sz.headerH = rowH;
    if (rep) {
        sz.w = std::max<float>(kMinRepW, contentW);
        sz.h = rowH;
        return cx.memo[b->id] = sz;
    }
    // ハットのドームはブロック上辺より上に張り出すだけで、
    // 行のレイアウト高さ（48）は通常ブロックと同じ（START_HAT_PATH が
    // 負の y を使う）。高さや区間には加えない。
    sz.w = std::max<float>(kMinW, contentW);
    sz.h = sz.headerH;
    if (st.cblock >= 1) {
        auto innerH = [&](int br) {
            std::string sub = cx.blocks->getBranch(b->id, br == 0 ? 0 : 2);
            if (sub.empty()) return (float)kMouthHMin;
            float h = 0;
            std::string id = sub;
            while (!id.empty()) {
                Block* c = cx.blocks->getBlock(id);
                if (!c) break;
                if (!cx.memo.count(id))
                    cx.memo[id] = measureBlockImpl(cx, c);
                h += cx.memo[id].h;
                id = c->next;
            }
            return std::max<float>(kMouthHMin, h);
        };
        // scratch-blocks: this.width = max(width,
        //   STATEMENT_INPUT_EDGE_WIDTH + innerStack.width)
        // 内側チェーンの最小幅を測って親ブロックを腕+内側幅に広げる
        auto innerW = [&](int br) {
            std::string sub = cx.blocks->getBranch(b->id, br == 0 ? 0 : 2);
            float w = 0;
            std::string id = sub;
            while (!id.empty()) {
                Block* c = cx.blocks->getBlock(id);
                if (!c) break;
                if (!cx.memo.count(id))
                    cx.memo[id] = measureBlockImpl(cx, c);
                w = std::max(w, cx.memo[id].w);
                id = c->next;
            }
            return w;
        };
        float in1 = innerH(0);
        sz.h += in1;
        if (st.cblock >= 2) {
            sz.h += kStackH;          // else バー
            sz.h += innerH(1);
        }
        sz.h += kCapH;
        sz.w = std::max<float>(sz.w, 40 * 4);  // MIN_BLOCK_X_WITH_STATEMENT
        sz.w = std::max<float>(sz.w,
                               kArmW + std::max(innerW(0), innerW(1)));
    }
    return cx.memo[b->id] = sz;
}

BlockSize measureBlock(Blocks* blocks, const Block* b,
                       const TextRenderer& tr) {
    Ctx cx{blocks, &tr, {}};
    return measureBlockImpl(cx, b);
}

// ---- 配置 ------------------------------------------------------------------

static float layoutChainImpl(Ctx& cx, const std::string& head, float x,
                             float y, std::vector<LBlock>& out, int depth) {
    float cy = y;
    std::string id = head;
    while (!id.empty()) {
        Block* b = cx.blocks->getBlock(id);
        if (!b) break;
        if (!cx.memo.count(id)) cx.memo[id] = measureBlockImpl(cx, b);
        const BlockSize& sz = cx.memo[id];
        BlockStyle st = styleOf(cx.blocks, b);

        LBlock lb;
        lb.id = id;
        lb.block = b;
        lb.x = x;
        lb.y = cy;
        lb.w = sz.w;
        lb.h = sz.h;
        lb.shape = st.shape;
        lb.cblock = st.cblock;
        lb.shadow = b->shadow;
        lb.hat = st.shape == edit::Shape::Hat;
        lb.defineHat = b->opcode == "procedures_definition";
        lb.depth = depth;
        lb.sub2Label = st.sub2Label;
        lb.segs = sz.segs;
        lb.headerH = sz.headerH;
        lb.noBump = st.shape == edit::Shape::End ||
                    b->opcode == "control_forever";
        lb.capRound = b->opcode == "control_forever";
        lb.fullH = lb.h + (lb.noBump ? 0 : kNotchH);

        if (st.cblock >= 1) {
            float headerH = sz.headerH;
            LMouth m;
            m.branch = 0;
            m.x = x + kArmW;
            m.y = cy + headerH;
            m.w = lb.w - kArmW;
            std::string sub = cx.blocks->getBranch(id, 0);
            float used = kMouthHMin;
            if (!sub.empty()) {
                m.headX = m.x;
                m.headY = m.y;
                float end = layoutChainImpl(cx, sub, m.x, m.y, out,
                                            depth + 1);
                used = std::max<float>(kMouthHMin, end - m.y);
            }
            m.innerH = used;
            lb.mouths.push_back(m);
            cy = m.y + used;
            if (st.cblock >= 2) {
                lb.sub2LabelY = cy;
                cy += kStackH;
                LMouth m2;
                m2.branch = 1;
                m2.x = x + kArmW;
                m2.y = cy;
                m2.w = lb.w - kArmW;
                std::string sub2 = cx.blocks->getBranch(id, 2);
                float used2 = kMouthHMin;
                if (!sub2.empty()) {
                    m2.headX = m2.x;
                    m2.headY = m2.y;
                    float end = layoutChainImpl(cx, sub2, m2.x, m2.y, out,
                                                depth + 1);
                    used2 = std::max<float>(kMouthHMin, end - m2.y);
                }
                m2.innerH = used2;
                lb.mouths.push_back(m2);
                cy = m2.y + used2;
            }
            cy += kCapH;
        } else {
            cy += lb.h;
        }
        out.push_back(lb);

        // 入れ子の value input（socket 内の実ブロック）を配置
        for (auto& g : lb.segs) {
            if (g.kind != SegKind::Input && g.kind != SegKind::BoolInput)
                continue;
            Block* child = inputChild(cx.blocks, b, g.input);
            if (child && !child->shadow)
                layoutChainImpl(cx, child->id, lb.x + g.x, lb.y + g.y, out,
                                depth + 1);
        }
        id = b->next;
    }
    return cy;
}

float layoutChain(Blocks* blocks, const std::string& head, float x, float y,
                  const TextRenderer& tr, std::vector<LBlock>& out,
                  int depth) {
    Ctx cx{blocks, &tr, {}};
    return layoutChainImpl(cx, head, x, y, out, depth);
}

float chainHeight(Blocks* blocks, const std::string& head,
                  const TextRenderer& tr) {
    Ctx cx{blocks, &tr, {}};
    float h = 0;
    std::string id = head;
    while (!id.empty()) {
        Block* b = cx.blocks->getBlock(id);
        if (!b) break;
        if (!cx.memo.count(id)) cx.memo[id] = measureBlockImpl(cx, b);
        h += cx.memo[id].h;
        id = b->next;
    }
    return h + kNotchH;
}

// ---- ワークスペース配置 -----------------------------------------------------

ScriptLayout layoutWorkspace(Blocks* blocks, const TextRenderer& tr) {
    ScriptLayout sl;
    if (!blocks) return sl;
    Ctx cx{blocks, &tr, {}};

    // x/y を持つものはその位置へ。座標が他と衝突するものは自動整列。
    std::set<std::pair<long long, long long>> used;
    std::vector<std::string> auto_;
    std::vector<std::pair<std::string, std::pair<double, double>>> placed;
    for (const auto& sid : blocks->getScripts()) {
        Block* b = blocks->getBlock(sid);
        if (!b) continue;
        long long ix = std::llround(b->x), iy = std::llround(b->y);
        if (used.insert({ix, iy}).second)
            placed.push_back({sid, {b->x, b->y}});
        else
            auto_.push_back(sid);
    }
    float autoY = 48;
    for (auto& [sid, p] : placed) {
        Block* b = blocks->getBlock(sid);
        if (!cx.memo.count(sid)) cx.memo[sid] = measureBlockImpl(cx, b);
        autoY = std::max(autoY,
                         (float)(p.second + chainHeight(blocks, sid, tr) + 48));
    }
    for (const auto& sid : auto_) {
        placed.push_back({sid, {48.0, autoY}});
        autoY += chainHeight(blocks, sid, tr) + 48;
    }
    for (auto& [sid, p] : placed) {
        sl.headAt[sid] = (int)sl.blocks.size();
        layoutChainImpl(cx, sid, (float)p.first, (float)p.second, sl.blocks,
                        0);
    }
    return sl;
}

} // namespace blockly
} // namespace xyo
