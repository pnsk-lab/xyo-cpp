// xyo-cpp: complexity.cpp — Scratch 向けの軽量な静的計算量見積り。
#include "complexity.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/sstr.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "project_analysis.hpp"

namespace xyo::analysis {

namespace {

bool isLoopOp(const std::string& op) {
    return op == "control_forever" || op == "control_repeat" ||
           op == "control_repeat_until" || op == "control_while" ||
           op == "control_for_each";
}

bool isConstantBlock(const Block& b) {
    return b.shadow && b.opcode != "data_variable";
}

// O(|L|) の線形走査・シフト系（index 参照・length は O(1) なので含めない）。
bool isLinearScanOp(const std::string& op) {
    return op == "data_itemnumoflist" || op == "data_listcontainsitem" ||
           op == "data_deleteoflist" || op == "data_insertatlist";
}

std::string u8(std::u16string_view sv) { return SStr::toUtf8(sv); }

std::string fieldName(const Block& b, const char* field) {
    auto it = b.fields.find(field);
    if (it == b.fields.end()) return {};
    const BlockField& f = it->second;
    if (f.value.isString()) {
        if (const SStr* s = f.value.asStrPtr()) return u8(s->view());
        return {};
    }
    if (f.value.isNumber()) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", f.value.asNumberUnchecked());
        return buf;
    }
    return {};
}

std::string truncate(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t cut = n;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "…";
}

// --- ループ 1 段の局所 bound ----------------------------------------------

struct LoopBound {
    enum class Kind : uint8_t { Const, Param, Unknown, Infinite };
    Kind kind = Kind::Unknown;
    double trips = 0;        // Const のときのみ
    std::string expr;        // Param/Unknown の表示（"|L|" / "N" / "条件"）
    std::string note;        // 根拠（"repeat 10" 等）
};

// Scratch の repeat 回数意味論の近似：jsRound して負・NaN は 0 回。
// （core.cpp の ctlRepeat と対応。+Inf は終わらないので Infinite 扱い。）
LoopBound constTrips(double d, const std::string& note) {
    LoopBound b;
    if (std::isnan(d)) {
        b.kind = LoopBound::Kind::Const;
        b.trips = 0;
        b.note = note + " (NaN→0回)";
        return b;
    }
    if (std::isinf(d)) {
        if (d > 0) {
            b.kind = LoopBound::Kind::Infinite;
            b.note = note + " (∞)";
            return b;
        }
        b.kind = LoopBound::Kind::Const;
        b.trips = 0;
        b.note = note + " (-∞→0回)";
        return b;
    }
    const double r = std::floor(d + 0.5);
    b.kind = LoopBound::Kind::Const;
    b.trips = r < 0 ? 0 : r;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.0f", b.trips);
    b.note = note + buf;
    return b;
}

double numFieldOf(const Block& b, const char* field, bool& ok) {
    auto it = b.fields.find(field);
    if (it == b.fields.end()) {
        ok = false;
        return 0;
    }
    const Value& v = it->second.value;
    if (v.isNumber()) {
        ok = true;
        return v.asNumberUnchecked();
    }
    if (v.isString()) {
        if (const SStr* s = v.asStrPtr()) {
            const std::string t = u8(s->view());
            // 例外を使わずに数値化（"10abc" 等は Scratch では NaN → 0 回）
            char* end = nullptr;
            const double d = std::strtod(t.c_str(), &end);
            if (end == nullptr || *end != '\0') {
                ok = true;
                return std::numeric_limits<double>::quiet_NaN();
            }
            ok = true;
            return d;
        }
        ok = true;
        return 0;  // 空文字列 → 0
    }
    ok = true;
    return std::numeric_limits<double>::quiet_NaN();
}

// レポータ式の記号化（成功すれば式文字列、できなければ nullopt）。
// 数値を保つ演算（四則・mod・round）だけを展開し、深さ 3 で打ち切る。
// join・random・sensing 等は非数値/非決定的なので UNKNOWN 側へ落とす。
std::optional<std::string> symbolicExpr(Blocks& bs, const Block* v, int depth) {
    if (!v || depth > 3) return std::nullopt;
    const std::string& op = v->opcode;
    if (op == "math_number" || op == "math_positive_number" ||
        op == "math_whole_number" || op == "math_integer" ||
        op == "math_angle") {
        bool ok = false;
        const double d = numFieldOf(*v, "NUM", ok);
        if (!ok || std::isnan(d) || std::isinf(d)) return std::nullopt;
        const double r = std::floor(d + 0.5);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f", r);
        return std::string(buf);
    }
    if (op == "text") {
        bool ok = false;
        const double d = numFieldOf(*v, "TEXT", ok);
        if (!ok || std::isnan(d) || std::isinf(d)) return std::nullopt;
        const double r = std::floor(d + 0.5);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f", r);
        return std::string(buf);
    }
    if (op == "data_variable") {
        std::string name = fieldName(*v, "VARIABLE");
        if (name.empty()) name = "N";
        return name;
    }
    if (op == "data_lengthoflist" || op == "data_listcontents") {
        std::string name = fieldName(*v, "LIST");
        if (name.empty()) {
            if (const BlockInput* li = v->inputs.find("LIST")) {
                const std::string lid =
                    !li->block.empty() ? li->block : li->shadow;
                if (const Block* lb = lid.empty() ? nullptr : bs.getBlock(lid))
                    name = fieldName(*lb, "LIST");
            }
        }
        if (name.empty()) name = "L";
        return "|" + name + "|";
    }
    if (op == "argument_reporter_string_number" ||
        op == "argument_reporter_boolean") {
        std::string name = fieldName(*v, "VALUE");
        if (name.empty()) name = "引数";
        return name;
    }
    auto operand = [&](const char* n) -> std::optional<std::string> {
        const BlockInput* in = v->inputs.find(n);
        if (!in) return std::nullopt;
        const std::string id = !in->block.empty() ? in->block : in->shadow;
        const Block* c = id.empty() ? nullptr : bs.getBlock(id);
        return symbolicExpr(bs, c, depth + 1);
    };
    if (op == "operator_add" || op == "operator_subtract" ||
        op == "operator_multiply" || op == "operator_divide" ||
        op == "operator_mod") {
        auto a = operand("NUM1"), b2 = operand("NUM2");
        if (!a || !b2) return std::nullopt;
        const char sym = op == "operator_add"        ? '+'
                         : op == "operator_subtract" ? '-'
                         : op == "operator_multiply" ? '*'
                         : op == "operator_divide"   ? '/'
                                                     : '%';
        return "(" + *a + sym + *b2 + ")";
    }
    if (op == "operator_round") {
        auto a = operand("NUM");
        if (!a) return std::nullopt;
        return "round(" + *a + ")";
    }
    return std::nullopt;
}

// TIMES / VALUE 入力の式を局所 bound へ（ランキング関数の自明版：
// 定数ならそのまま、変数・リスト長・引数・数値演算なら記号パラメータ、
// それ以外は UNKNOWN）。
LoopBound inputBound(Blocks& bs, const Block* loop, const char* inputName,
                     const std::string& what) {
    LoopBound b;
    const BlockInput* in = loop->inputs.find(inputName);
    if (!in) {
        b.kind = LoopBound::Kind::Unknown;
        b.expr = "条件";
        b.note = what + "入力なし（条件依存）";
        return b;
    }
    const std::string id = !in->block.empty() ? in->block : in->shadow;
    const Block* v = id.empty() ? nullptr : bs.getBlock(id);
    if (!v) {
        b.kind = LoopBound::Kind::Unknown;
        b.expr = "条件";
        b.note = what + "解決不能（条件依存）";
        return b;
    }
    // 純粋な定数は jsRound して exact 回数に畳む（ctlRepeat と対応）
    if (v->opcode == "math_number" || v->opcode == "math_positive_number" ||
        v->opcode == "math_whole_number" || v->opcode == "math_integer" ||
        v->opcode == "math_angle") {
        bool ok = false;
        const double d = numFieldOf(*v, "NUM", ok);
        if (ok) return constTrips(d, what + " ");
    }
    if (v->opcode == "text") {
        bool ok = false;
        const double d = numFieldOf(*v, "TEXT", ok);
        if (ok) return constTrips(d, what + " ");
    }
    // 変数・リスト長・引数・数値演算は記号パラメータへ
    if (auto e = symbolicExpr(bs, v, 0)) {
        b.kind = LoopBound::Kind::Param;
        b.expr = *e;
        b.note = what + *e;
        return b;
    }
    const std::string& op = v->opcode;
    if (op == "sensing_timer" || op == "sensing_dayssince2000" ||
        op == "sensing_current") {
        b.kind = LoopBound::Kind::Unknown;
        b.expr = "時刻";
        b.note = what + op + "（時刻依存）";
        return b;
    }
    // operator_*・sensing_*・その他レポータ：記号化せず UNKNOWN（過小評価しない）
    b.kind = LoopBound::Kind::Unknown;
    b.expr = "式";
    b.note = what + op + "（式依存）";
    return b;
}

// repeat_until / while のカウンタ比較パターン（1変数の線形ランキング）。
//   repeat until <V > C> + 本体で change V by +d（d>0 定数）→ (C-V)/d
// 成功すれば Param、失敗すれば nullopt（呼び出し側で UNKNOWN へ）。
std::optional<LoopBound> rankBound(Blocks& bs, const Block* loop) {
    const std::string& op = loop->opcode;
    const bool until = (op == "control_repeat_until");
    if (!until && op != "control_while") return std::nullopt;

    const BlockInput* ci = loop->inputs.find("CONDITION");
    if (!ci) return std::nullopt;
    const std::string cid = !ci->block.empty() ? ci->block : ci->shadow;
    const Block* cond = cid.empty() ? nullptr : bs.getBlock(cid);
    if (!cond || (cond->opcode != "operator_gt" &&
                  cond->opcode != "operator_lt"))
        return std::nullopt;
    const bool gt = (cond->opcode == "operator_gt");

    auto side = [&](const char* n) -> const Block* {
        const BlockInput* in = cond->inputs.find(n);
        if (!in) return nullptr;
        const std::string id = !in->block.empty() ? in->block : in->shadow;
        return id.empty() ? nullptr : bs.getBlock(id);
    };
    const Block* a = side("OPERAND1");
    const Block* b2 = side("OPERAND2");
    if (!a || !b2) return std::nullopt;

    // 片側が変数 V、もう片側が定数 C（または変数 W）
    auto asVar = [&](const Block* x, std::string& id,
                     std::string& name) -> bool {
        if (!x || x->opcode != "data_variable") return false;
        auto it = x->fields.find("VARIABLE");
        if (it == x->fields.end() || it->second.id.empty()) return false;
        id = it->second.id;
        name = fieldName(*x, "VARIABLE");
        if (name.empty()) name = "V";
        return true;
    };
    std::string varId, varName, tgtExpr;
    bool varFirst = false;
    if (std::string id, nm; asVar(a, id, nm)) {
        varId = id;
        varName = nm;
        varFirst = true;
    } else if (std::string id, nm; asVar(b2, id, nm)) {
        varId = id;
        varName = nm;
        varFirst = false;
    } else {
        return std::nullopt;
    }
    const Block* tgt = varFirst ? b2 : a;
    {
        auto e = symbolicExpr(bs, tgt, 0);
        if (!e) return std::nullopt;
        tgtExpr = *e;
    }

    // 本体配下（ネスト含む）で V への change を集める。set V や
    // 非定数の増分があるとランキング不成立。
    std::vector<double> deltas;
    {
        std::unordered_set<std::string> seen;
        std::vector<std::string> stack;
        for (const auto& [name, in] : loop->inputs.items) {
            if (name.rfind("SUBSTACK", 0) == 0 && !in.block.empty())
                stack.push_back(in.block);
        }
        size_t guard = 0;
        while (!stack.empty() && guard++ < 2048) {
            std::string id = std::move(stack.back());
            stack.pop_back();
            if (!seen.insert(id).second) continue;
            const Block* x = bs.getBlock(id);
            if (!x) continue;
            if (x->opcode == "data_changevariableby") {
                auto it = x->fields.find("VARIABLE");
                if (it != x->fields.end() && it->second.id == varId) {
                    const BlockInput* vi = x->inputs.find("VALUE");
                    const std::string vid =
                        vi ? (!vi->block.empty() ? vi->block : vi->shadow)
                           : std::string{};
                    const Block* vb =
                        vid.empty() ? nullptr : bs.getBlock(vid);
                    bool ok = false;
                    double d = 0;
                    if (vb &&
                        (vb->opcode == "math_number" ||
                         vb->opcode == "math_positive_number" ||
                         vb->opcode == "math_whole_number" ||
                         vb->opcode == "math_integer")) {
                        d = numFieldOf(*vb, "NUM", ok);
                    }
                    if (!ok || std::isnan(d)) return std::nullopt;
                    deltas.push_back(d);
                }
            } else if (x->opcode == "data_setvariableto") {
                auto it = x->fields.find("VARIABLE");
                if (it != x->fields.end() && it->second.id == varId)
                    return std::nullopt;  // リセットされると単調性が崩れる
            }
            if (!x->next.empty()) stack.push_back(x->next);
            for (const auto& [n, in] : x->inputs.items) {
                if (!in.block.empty()) stack.push_back(in.block);
                if (!in.shadow.empty()) stack.push_back(in.shadow);
            }
        }
    }
    if (deltas.empty()) return std::nullopt;
    const double d0 = deltas[0];
    for (double d : deltas)
        if (d != d0) return std::nullopt;  // 増分が一定でない
    if (d0 == 0) return std::nullopt;

    // 向きの検査：脱出条件へ単調に近づく場合だけ bound 化する。
    //   until は条件真で脱出 → (V>C かつ d>0) / (V<C かつ d<0)
    //   while は条件真で継続 → (V>C かつ d<0) / (V<C かつ d>0)
    // それ以外（0 回か無限かの二択）は UNKNOWN のまま。
    const bool isGt = (gt == varFirst);  // 正規化: V>C か
    bool okDir;
    if (until)
        okDir = (isGt && d0 > 0) || (!isGt && d0 < 0);
    else
        okDir = (isGt && d0 < 0) || (!isGt && d0 > 0);
    if (!okDir) return std::nullopt;
    const bool up = d0 > 0;

    auto fmtN = [](double d) {
        char buf[32];
        if (d == std::floor(d) && std::fabs(d) < 1e15)
            std::snprintf(buf, sizeof buf, "%.0f", d);
        else
            std::snprintf(buf, sizeof buf, "%.6g", d);
        return std::string(buf);
    };
    const std::string num =
        up ? "(" + tgtExpr + "-" + varName + ")" : "(" + varName + "-" + tgtExpr + ")";
    const double ad = std::fabs(d0);
    LoopBound lb;
    lb.kind = LoopBound::Kind::Param;
    lb.expr = (ad == 1) ? num : num + "/" + fmtN(ad);
    lb.note = op + " " + varName + (isGt ? ">" : "<") + tgtExpr +
              " Δ" + (d0 > 0 ? "+" : "") + fmtN(d0);
    return lb;
}

LoopBound loopBound(Blocks& bs, const Block* b) {
    const std::string& op = b->opcode;
    if (op == "control_forever") {
        LoopBound lb;
        lb.kind = LoopBound::Kind::Infinite;
        lb.expr = "∞";
        lb.note = "forever (∞)";
        return lb;
    }
    if (op == "control_repeat") return inputBound(bs, b, "TIMES", "repeat");
    if (op == "control_for_each") return inputBound(bs, b, "VALUE", "for_each");
    if (op == "control_repeat_until" || op == "control_while") {
        if (auto rb = rankBound(bs, b)) return *rb;
    }
    LoopBound lb;
    lb.kind = LoopBound::Kind::Unknown;
    lb.expr = "条件";
    lb.note = op + " (ガード依存)";
    return lb;
}

std::string scanListOf(Blocks& bs, const Block* b) {
    auto it = b->fields.find("LIST");
    if (it != b->fields.end()) {
        std::string name = fieldName(*b, "LIST");
        if (!name.empty()) return name;
    }
    if (const BlockInput* li = b->inputs.find("LIST")) {
        const std::string lid = !li->block.empty() ? li->block : li->shadow;
        if (const Block* lb = lid.empty() ? nullptr : bs.getBlock(lid)) {
            std::string name = fieldName(*lb, "LIST");
            if (!name.empty()) return name;
        }
    }
    return "L";
}

/// ループスタックの積式（ホット項・spawn 倍率の表示用）。
std::string stackExpr(const std::vector<LoopBound>& stack,
                      const std::string& scan = "") {
    std::string e;
    for (const auto& lb : stack) {
        std::string seg;
        if (lb.kind == LoopBound::Kind::Const) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.0f", lb.trips);
            seg = buf;
        } else if (lb.kind == LoopBound::Kind::Param) {
            seg = lb.expr;
        } else if (lb.kind == LoopBound::Kind::Infinite) {
            seg = "∞";
        } else {
            seg = "U";
        }
        if (!e.empty()) e += " * ";
        e += seg;
    }
    if (!scan.empty()) {
        if (!e.empty()) e += " * ";
        e += scan;
    }
    return e;
}

/// メニュー入力（BROADCAST_INPUT / CLONE_OPTION）の選択名。
std::string menuFieldOf(Blocks& bs, const Block* b, const char* input,
                        const char* field) {
    const BlockInput* in = b->inputs.find(input);
    if (!in) {
        auto it = b->fields.find(field);
        if (it != b->fields.end()) {
            std::string name = fieldName(*b, field);
            if (!name.empty()) return name;
        }
        return "?";
    }
    const std::string id = !in->block.empty() ? in->block : in->shadow;
    const Block* m = id.empty() ? nullptr : bs.getBlock(id);
    if (!m) return "?";
    std::string name = fieldName(*m, field);
    return name.empty() ? "?" : name;
}

// --- 本体ウォーカ ------------------------------------------------------------
// 呼び出し元のループスタックを引き継いで手続き本体を展開する
// （global composition の簡易版：呼出し回数 × 本体 bound）。
// 相互再帰は展開せず recursive フラグに畳む。

struct Walker {
    Blocks& bs;
    ComplexityInfo& out;
    std::vector<LoopBound> stack;          // 外側→内側のループ bound
    std::vector<std::string> procStack;   // 展開中の proccode（再帰検出）
    std::unordered_set<std::string> seenNote;
    int guard = 0;
    double exactAcc = 0;    // 全定数のときの総ステップ
    bool exactOk = true;
    // ホット項（支配項）の追跡：記号因子数 → 定数積 の順で最大を取る
    size_t hotSyms = 0;
    double hotConst = 1;
    std::vector<std::string> hotFactors;
    std::string hotExpr;

    void note(const std::string& s) {
        if (out.notes.size() >= 8) return;
        if (!seenNote.insert(s).second) return;
        out.notes.push_back(s);
    }

    double constProd() const {
        double p = 1;
        for (const auto& lb : stack)
            if (lb.kind == LoopBound::Kind::Const) {
                p *= lb.trips;
                if (p > 1e15) return 1e15;
            }
        return p;
    }

    // 現在の囲みに対する 1 ブロック分の項を記録する
    void term(const Block* b) {
        if (isConstantBlock(*b)) return;
        std::vector<std::string> syms;
        double cp = 1;
        bool unk = false, inf = false;
        for (const auto& lb : stack) {
            if (lb.kind == LoopBound::Kind::Const) {
                cp *= lb.trips;
                if (cp > 1e15) cp = 1e15;
            } else if (lb.kind == LoopBound::Kind::Param) {
                syms.push_back(lb.expr);
            } else if (lb.kind == LoopBound::Kind::Infinite) {
                inf = true;
            } else {
                unk = true;
            }
        }
        std::string scan;
        if (isLinearScanOp(b->opcode)) {
            scan = "|" + scanListOf(bs, b) + "|";
            syms.push_back(scan);
        }
        if (inf) out.infinite = true;
        if (unk) out.unknown = true;
        for (const auto& s : syms) out.factors.push_back(s);
        out.maxDepth = std::max(out.maxDepth, stack.size());
        if (stack.size() > 0 && syms.empty() && !unk && !inf) {
            // 定数ネスト内のブロック：exact 加算の対象（下で処理）
        }
        if (!unk && !inf && syms.empty()) {
            exactAcc += cp;
            if (exactAcc > 1e15) {
                exactAcc = 1e15;
                exactOk = false;
            }
        } else {
            // 記号・未知・走査が絡む項は exact にできない
            exactOk = false;
        }
        // ホット項の更新
        const size_t ns = syms.size();
        if (ns > hotSyms || (ns == hotSyms && cp > hotConst)) {
            hotSyms = ns;
            hotConst = cp;
            hotFactors = syms;
            hotExpr = stackExpr(stack, scan);
        }
    }

    void chain(const std::string& firstId, size_t depth) {
        if (guard++ > 8192) return;
        std::string id = firstId;
        while (!id.empty()) {
            const Block* b = bs.getBlock(id);
            if (!b) return;
            visit(b, depth);
            id = b->next;
        }
    }

    void reporter(const BlockInput& in, size_t depth) {
        std::string id = !in.block.empty() ? in.block : in.shadow;
        if (id.empty()) return;
        const Block* b = bs.getBlock(id);
        if (!b || isConstantBlock(*b)) return;
        visit(b, depth);
    }

    void visit(const Block* b, size_t depth) {
        const std::string& op = b->opcode;
        term(b);

        if (op == "procedures_call") {
            callProc(b, depth);
            return;
        }
        // spawn の記録：broadcast 送信・clone 生成は別スレッド/次世代を起動する。
        // 起動あたりの Big-O は変えず、倍率は spawns と「起動関係」節で示す。
        if (op == "event_broadcast" || op == "event_broadcastandwait") {
            std::string name =
                menuFieldOf(bs, b, "BROADCAST_INPUT", "BROADCAST_OPTION");
            std::string mult = stackExpr(stack);
            if (mult.empty()) mult = "1";
            out.spawns.push_back({"broadcast", name, mult});
            note("broadcast '" + truncate(name, 24) + "'" +
                 (mult != "1" ? " ×" + mult : ""));
        } else if (op == "control_create_clone_of") {
            std::string tgt =
                menuFieldOf(bs, b, "CLONE_OPTION", "CLONE_OPTION");
            std::string mult = stackExpr(stack);
            if (mult.empty()) mult = "1";
            out.spawns.push_back({"clone", tgt, mult});
            note("clone " + truncate(tgt, 24) +
                 (mult != "1" ? " ×" + mult : ""));
        }
        for (const auto& [name, in] : b->inputs.items) {
            if (name.rfind("SUBSTACK", 0) == 0) continue;
            reporter(in, depth + 1);
        }
        if (isLoopOp(op)) {
            LoopBound lb = loopBound(bs, b);
            if (lb.kind == LoopBound::Kind::Infinite) out.infinite = true;
            if (lb.kind == LoopBound::Kind::Unknown) out.unknown = true;
            if (lb.kind == LoopBound::Kind::Param) out.factors.push_back(lb.expr);
            note(lb.note);
            stack.push_back(std::move(lb));
            for (const auto& [name, in] : b->inputs.items) {
                if (name.rfind("SUBSTACK", 0) != 0 || in.block.empty()) continue;
                chain(in.block, depth + 1);
            }
            stack.pop_back();
            return;
        }
        for (const auto& [name, in] : b->inputs.items) {
            if (name.rfind("SUBSTACK", 0) != 0 || in.block.empty()) continue;
            chain(in.block, depth + 1);
        }
    }

    void callProc(const Block* call, size_t depth) {
        if (!call->mutation) {
            out.unknown = true;
            note("手続き呼出し（解決不能）");
            return;
        }
        const std::string pc = call->mutation->attr("proccode");
        if (pc.empty()) {
            out.unknown = true;
            return;
        }
        if (std::find(procStack.begin(), procStack.end(), pc) !=
            procStack.end()) {
            out.recursive = true;
            out.unknown = true;  // 再帰の段数は数えない（上界なし）
            note("再帰 " + truncate(pc, 28));
            return;
        }
        const std::string defId = bs.getProcedureDefinition(pc);
        const Block* def = defId.empty() ? nullptr : bs.getBlock(defId);
        if (!def) {
            out.unknown = true;
            note("未定義手続き " + truncate(pc, 28));
            return;
        }
        if (procStack.size() > 64 || guard > 8192) {
            out.unknown = true;
            note("呼出し深すぎ（打切り）");
            return;
        }
        procStack.push_back(pc);
        if (!def->next.empty()) chain(def->next, depth + 1);
        procStack.pop_back();
    }
};

ComplexityInfo analyzeOne(const Report& r, size_t idx) {
    ComplexityInfo out;
    const ScriptInfo& si = r.scripts[idx];
    out.scriptIndex = idx;
    out.label = si.label;
    out.kind = si.kind == ScriptKind::Definition ? "def" : "hat";

    if (!si.target || !si.target->blocks) {
        out.unknown = true;
        out.boundExpr = "?";
        out.bigO = "O(?)";
        out.notes.push_back("ブロック解決不能");
        return out;
    }
    Blocks& bs = *si.target->blocks;
    Walker w{bs, out, {}, {}, {}, 0, 0, true, 0, 1, {}, {}};
    if (si.top && !si.top->next.empty()) w.chain(si.top->next, 0);

    // 何も実行ブロックが無い（空スクリプト）
    if (out.factors.empty() && out.notes.empty() && !out.unknown &&
        !out.infinite && !out.recursive && w.exactAcc == 0) {
        out.exact = true;
        out.exactSteps = 0;
        out.boundExpr = "0 ステップ";
        out.bigO = "O(1)";
        return out;
    }

    // Big-O 正規化：ホット項の因子多重集合 → 冪表示
    std::map<std::string, size_t> cnt;
    for (const auto& f : w.hotFactors) cnt[f] += 1;
    if (out.infinite) {
        out.bigO = "O(∞)";
    } else if (out.recursive && w.hotFactors.empty() && out.unknown) {
        // 再帰だけが未知の原因なら再帰表記、他に未知があれば O(?) を優先
        out.bigO = "O(?)再帰";
    } else if (out.unknown && w.hotFactors.empty()) {
        out.bigO = "O(?)";
    } else if (cnt.empty()) {
        out.bigO = out.unknown ? "O(?)含む定数" : "O(1)";
    } else {
        std::string o = "O(";
        bool first = true;
        // 決定的順序：辞書順
        for (const auto& [name, n] : cnt) {
            if (!first) o += "*";
            o += name;
            if (n > 1) {
                o += "^";
                o += std::to_string(n);
            }
            first = false;
        }
        o += ")";
        if (out.unknown) o += "+U";
        out.bigO = o;
    }

    if (out.infinite) {
        out.exact = false;
        out.boundExpr = w.hotExpr.empty() ? "∞" : w.hotExpr + " (∞: forever)";
    } else if (w.exactOk && cnt.empty() && !out.unknown && !out.recursive) {
        out.exact = true;
        out.exactSteps = w.exactAcc;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.0f", w.exactAcc);
        out.boundExpr = std::string("≤ ") + buf + " ステップ (exact)";
    } else {
        out.exact = false;
        std::string e = w.hotExpr.empty() ? "1" : w.hotExpr;
        if (out.unknown) {
            e += " (+条件U";
            if (out.recursive) e += "/再帰";
            e += ")";
        } else if (out.recursive) {
            e += " (+再帰)";
        }
        out.boundExpr = e;
    }
    return out;
}

}  // namespace

ComplexityReport analyzeComplexity(const Report& r) {
    ComplexityReport c;
    for (size_t i = 0; i < r.scripts.size(); ++i) {
        if (r.scripts[i].kind == ScriptKind::ReporterTop) continue;
        c.scripts.push_back(analyzeOne(r, i));
    }
    // 深刻度順：∞ → 再帰/? → 記号数の多さ → exact ステップ数
    std::sort(c.scripts.begin(), c.scripts.end(),
              [](const ComplexityInfo& a, const ComplexityInfo& b) {
                  if (a.infinite != b.infinite) return a.infinite > b.infinite;
                  const bool au = a.unknown || a.recursive;
                  const bool bu = b.unknown || b.recursive;
                  if (au != bu) return au > bu;
                  if (a.factors.size() != b.factors.size())
                      return a.factors.size() > b.factors.size();
                  if (a.exact != b.exact) return !a.exact;
                  return a.exactSteps > b.exactSteps;
              });
    return c;
}

std::string formatComplexity(const Report& r, const ComplexityReport& c) {
    std::string o;
    auto line = [&](const std::string& s) { o += s; o += '\n'; };
    char buf[512];
    auto addf = [&](const char* fmt, auto... args) {
        std::snprintf(buf, sizeof buf, fmt, args...);
        o += buf;
        o += '\n';
    };
    line("");
    line("== 計算量 (--complexity: 起動あたりの静的見積り) ==");
    line("  モデル: 1ブロック実行=1ステップ。repeat定数は exact、変数=N・リスト長=|L|・");
    line("  引数・数値演算は記号化、ガード依存・式依存は U、forever は ∞。");
    line("  until/while のカウンタ比較（i>10 + change i by 1）は線形ランキングで回数化。");
    line("  手続きは呼出し側のループ倍率を引き継いで展開、再帰は打切り。");
    line("  見積りは上界の候補（sound ではない簡易版: ranking/TWN/SMT は使わない）。");
    line("  合計 = 起動あたり × 起動回数。起動回数は下の「起動関係」を参照。");
    addf("%4s %-4s %-30s %-26s %-14s %s", "#", "kind", "script",
         "bound", "Big-O", "根拠");
    for (const auto& s : c.scripts) {
        std::string notes;
        for (size_t i = 0; i < s.notes.size(); ++i) {
            if (i) notes += " / ";
            notes += s.notes[i];
        }
        addf("%4zu %-4s %-30s %-26s %-14s %s", s.scriptIndex,
             s.kind.c_str(), truncate(s.label, 30).c_str(),
             truncate(s.boundExpr, 26).c_str(), s.bigO.c_str(),
             truncate(notes, 60).c_str());
    }
    if (c.scripts.empty()) line("  （対象スクリプトなし）");

    // --- 起動関係：broadcast 送信→受信、clone 生成 ---
    // 受信ハット側の index（放送名 → Report::scripts の index）
    std::map<std::string, std::vector<size_t>> receivers;
    for (size_t i = 0; i < r.scripts.size(); ++i) {
        const ScriptInfo& s = r.scripts[i];
        if (s.kind != ScriptKind::Hat || !s.top ||
            s.top->opcode != "event_whenbroadcastreceived")
            continue;
        std::string name = fieldName(*s.top, "BROADCAST_OPTION");
        if (!name.empty()) receivers[name].push_back(i);
    }
    bool anySpawn = false;
    for (const auto& s : c.scripts)
        if (!s.spawns.empty()) {
            anySpawn = true;
            break;
        }
    if (anySpawn || !receivers.empty()) {
        line("");
        line("== 起動関係 (broadcast/clone: 合計 = 起動あたり × 起動回数) ==");
        // 放送ごとに送信→受信をまとめる
        std::map<std::string, std::vector<const ComplexityInfo*>> senders;
        std::vector<const ComplexityInfo*> cloners;
        for (const auto& s : c.scripts) {
            for (const auto& sp : s.spawns) {
                if (sp.kind == "broadcast")
                    senders[sp.target].push_back(&s);
                else
                    cloners.push_back(&s);
            }
        }
        for (const auto& [name, ss] : senders) {
            std::string text = "  broadcast '" + name + "': 送信 ";
            for (size_t i = 0; i < ss.size(); ++i) {
                if (i) text += ", ";
                text += (*ss[i]).label;
                if ((*ss[i]).spawns.empty() ||
                    (*ss[i]).spawns[0].mult != "1") {
                    // mult は spawns 側に記録済み。送信スクリプト単位で出す
                    for (const auto& sp : (*ss[i]).spawns) {
                        if (sp.kind == "broadcast" && sp.target == name &&
                            sp.mult != "1") {
                            text += " (×" + sp.mult + ")";
                            break;
                        }
                    }
                }
            }
            auto rit = receivers.find(name);
            if (rit == receivers.end() || rit->second.empty()) {
                text += " → 受信なし（何も起きない）";
            } else {
                text += " → 受信 ";
                for (size_t i = 0; i < rit->second.size(); ++i) {
                    if (i) text += ", ";
                    text += r.scripts[rit->second[i]].label;
                }
            }
            line(truncate(text, 220));
        }
        // 一度も送信されない放送の受信ハット
        for (const auto& [name, idxs] : receivers) {
            if (senders.count(name)) continue;
            std::string text = "  broadcast '" + name + "': 送信なし → 受信 ";
            for (size_t i = 0; i < idxs.size(); ++i) {
                if (i) text += ", ";
                text += r.scripts[idxs[i]].label;
            }
            line(truncate(text, 220));
        }
        for (const auto* s : cloners) {
            for (const auto& sp : s->spawns) {
                if (sp.kind != "clone") continue;
                std::string text = "  clone: " + s->label + " → " + sp.target;
                if (sp.mult != "1") text += " (×" + sp.mult + ")";
                text += "（clone 側の when-cloned が起動）";
                line(truncate(text, 220));
            }
        }
    }
    return o;
}

std::string formatComplexity(const Report& r) {
    return formatComplexity(r, analyzeComplexity(r));
}

}  // namespace xyo::analysis
