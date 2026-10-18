// xyo-cpp: deps.hpp 実装 — opcode ルール表 + 再帰ウォーカ + グラフ構築。
#include "deps.hpp"

#include <cstdio>
#include <unordered_map>

#include "../core/sstr.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"
#include "runtime.hpp"

namespace xyo {

namespace {

constexpr uint16_t fb(TF f) { return uint16_t(1) << uint8_t(f); }
constexpr uint16_t gb(GK g) { return uint16_t(1) << uint8_t(g); }

/// opcode → アクセスルール。var/list はフィールド名から解決する種別。
struct OpRule {
    uint16_t fr = 0, fw = 0;     // Field 読み/書きマスク
    uint16_t gr = 0, gw = 0;     // Global 読み/書きマスク
    enum class Var : uint8_t { None, Read, Write };
    Var var = Var::None;         // VARIABLE フィールド
    Var list = Var::None;        // LIST フィールド
    bool spawn = false;
    bool unknown = false;
    /// 入力メニュー経由で他ターゲットの X,Y を読む op
    /// (motion_goto / pointtowards / glideto / sensing_distanceto)
    const char* menuInput = nullptr;
};

OpRule ruleVar(OpRule::Var v) { OpRule r; r.var = v; return r; }
OpRule ruleList(OpRule::Var v) { OpRule r; r.list = v; return r; }
OpRule ruleF(uint16_t r, uint16_t w) { OpRule o; o.fr = r; o.fw = w; return o; }
OpRule ruleG(uint16_t r, uint16_t w) { OpRule o; o.gr = r; o.gw = w; return o; }

const std::unordered_map<std::string, OpRule>& opRules() {
    static const std::unordered_map<std::string, OpRule> m = [] {
        std::unordered_map<std::string, OpRule> t;
        auto add = [&](const char* op, OpRule r) { t.emplace(op, r); };
        const uint16_t XY = fb(TF::X) | fb(TF::Y);

        // --- data ---
        add("data_variable", ruleVar(OpRule::Var::Read));
        add("data_setvariableto", ruleVar(OpRule::Var::Write));
        add("data_changevariableby", ruleVar(OpRule::Var::Write));
        // monitor 表示切替は monitorState(OrderedMap) を同期変更 → spawn
        { OpRule r = ruleG(0, gb(GK::Monitor)); r.spawn = true;
          add("data_showvariable", r); add("data_hidevariable", r);
          add("data_showlist", r); add("data_hidelist", r); }
        add("data_addtolist", ruleList(OpRule::Var::Write));
        add("data_deleteoflist", ruleList(OpRule::Var::Write));
        add("data_deletealloflist", ruleList(OpRule::Var::Write));
        add("data_insertatlist", ruleList(OpRule::Var::Write));
        add("data_replaceitemoflist", ruleList(OpRule::Var::Write));
        add("data_itemoflist", ruleList(OpRule::Var::Read));
        add("data_itemnumoflist", ruleList(OpRule::Var::Read));
        add("data_lengthoflist", ruleList(OpRule::Var::Read));
        add("data_listcontainsitem", ruleList(OpRule::Var::Read));
        add("data_listcontents", ruleList(OpRule::Var::Read));

        // --- motion ---
        add("motion_movesteps", ruleF(fb(TF::Dir), XY));
        add("motion_turnright", ruleF(0, fb(TF::Dir)));
        add("motion_turnleft", ruleF(0, fb(TF::Dir)));
        add("motion_pointindirection", ruleF(0, fb(TF::Dir)));
        add("motion_changexby", ruleF(0, fb(TF::X)));
        add("motion_setx", ruleF(0, fb(TF::X)));
        add("motion_changeyby", ruleF(0, fb(TF::Y)));
        add("motion_sety", ruleF(0, fb(TF::Y)));
        add("motion_gotoxy", ruleF(0, XY));
        { OpRule r = ruleF(0, XY); r.menuInput = "TO"; add("motion_goto", r); }
        { OpRule r = ruleF(0, XY); r.menuInput = "TO"; add("motion_glideto", r); }
        add("motion_glidesecstoxy", ruleF(0, XY));
        // pointtowards は自位置も読む（dx = tx - t->x）
        { OpRule r = ruleF(XY, fb(TF::Dir)); r.menuInput = "TOWARDS";
          add("motion_pointtowards", r); }
        add("motion_ifonedgebounce",
            ruleF(fb(TF::RotStyle), XY | fb(TF::Dir)));
        add("motion_setrotationstyle", ruleF(0, fb(TF::RotStyle)));
        add("motion_xposition", ruleF(fb(TF::X), 0));
        add("motion_yposition", ruleF(fb(TF::Y), 0));
        add("motion_direction", ruleF(fb(TF::Dir), 0));
        add("motion_xscroll", OpRule{});
        add("motion_yscroll", OpRule{});
        add("motion_scroll_right", OpRule{});
        add("motion_scroll_up", OpRule{});
        add("motion_align_scene", OpRule{});

        // --- looks ---
        // say/think は updateBubble → onSay を同期呼出し — ランタイム
        // コールバックは観測可能順序を持つ共有状態のため逐次
        { OpRule r = ruleF(0, fb(TF::Bubble)); r.spawn = true;
          add("looks_say", r); add("looks_think", r); }
        // sayforsecs/thinkforsecs は waitQueue push — スケジューラ共有
        // 構造への書き込みのため逐次
        { OpRule r = ruleF(0, fb(TF::Bubble)); r.spawn = true;
          add("looks_sayforsecs", r);
          add("looks_thinkforsecs", r); }
        add("looks_show", ruleF(0, fb(TF::Visible)));
        add("looks_hide", ruleF(0, fb(TF::Visible)));
        add("looks_switchcostumeto", ruleF(0, fb(TF::Costume)));
        add("looks_nextcostume", ruleF(0, fb(TF::Costume)));
        add("looks_costumenumbername", ruleF(fb(TF::Costume), 0));
        { OpRule r; r.spawn = true; add("looks_switchbackdropto", r); }
        { OpRule r; r.spawn = true; add("looks_nextbackdrop", r); }
        { OpRule r; r.spawn = true; add("looks_switchbackdroptoandwait", r); }
        add("looks_backdropnumbername", ruleG(gb(GK::Backdrop), 0));
        add("looks_changeeffectby", ruleF(fb(TF::Effect), fb(TF::Effect)));
        add("looks_seteffectto", ruleF(0, fb(TF::Effect)));
        add("looks_cleargraphiceffects", ruleF(0, fb(TF::Effect)));
        add("looks_changesizeby", ruleF(0, fb(TF::Size)));
        add("looks_setsizeto", ruleF(0, fb(TF::Size)));
        add("looks_size", ruleF(fb(TF::Size), 0));
        // レイヤ操作は executableTargets 共有配列を変える → 逐次
        { OpRule r; r.unknown = true; add("looks_gotofrontback", r); }
        { OpRule r; r.unknown = true; add("looks_goforwardbackwardlayers", r); }
        add("looks_hideallsprites", OpRule{});
        add("looks_changestretchby", OpRule{});
        add("looks_setstretchto", OpRule{});

        // --- sensing ---
        add("sensing_timer", ruleG(gb(GK::Timer), 0));
        add("sensing_resettimer", ruleG(0, gb(GK::Timer)));
        add("sensing_mousex", ruleG(gb(GK::Io), 0));
        add("sensing_mousey", ruleG(gb(GK::Io), 0));
        add("sensing_mousedown", ruleG(gb(GK::Io), 0));
        add("sensing_keypressed", ruleG(gb(GK::Io), 0));
        add("sensing_loudness", ruleG(gb(GK::Io), 0));
        add("sensing_loud", ruleG(gb(GK::Io), 0));
        add("sensing_answer", ruleG(gb(GK::Io), 0));
        add("sensing_username", ruleG(gb(GK::Io), 0));
        add("sensing_online", ruleG(gb(GK::Io), 0));
        add("sensing_userid", OpRule{});
        add("sensing_current", ruleG(gb(GK::Io), 0));
        add("sensing_dayssince2000", ruleG(gb(GK::Io), 0));
        add("sensing_setdragmode", ruleF(0, fb(TF::Drag)));
        // 動的ターゲット/プロパティ解決 — 任意 var/field/renderer 状態を読む
        { OpRule r; r.unknown = true; add("sensing_of", r); }
        { OpRule r; r.unknown = true; add("sensing_touchingobject", r); }
        { OpRule r; r.unknown = true; add("sensing_touchingcolor", r); }
        { OpRule r; r.unknown = true;
          add("sensing_coloristouchingcolor", r); }
        { OpRule r = ruleF(fb(TF::X) | fb(TF::Y), 0);
          r.menuInput = "DISTANCETOMENU";
          add("sensing_distanceto", r); }
        { OpRule r; r.unknown = true; add("sensing_askandwait", r); }

        // --- control ---
        add("control_get_counter", ruleG(gb(GK::Counter), 0));
        add("control_incr_counter", ruleG(0, gb(GK::Counter)));
        add("control_clear_counter", ruleG(0, gb(GK::Counter)));
        { OpRule r; r.spawn = true; add("control_stop", r); }
        { OpRule r; r.spawn = true; add("control_create_clone_of", r); }
        { OpRule r; r.spawn = true; add("control_delete_this_clone", r); }
        // wait/wait_until — rt_wait が waitQueue/redrawRequested に触れる
        { OpRule r; r.spawn = true; add("control_wait", r); }
        { OpRule r; r.spawn = true; add("control_wait_until", r); }

        // --- event ---
        { OpRule r; r.spawn = true; add("event_broadcast", r); }
        { OpRule r; r.spawn = true; add("event_broadcastandwait", r); }

        // --- sound ---
        // soundBank の playSound/stopAllSounds は共有オーディオエンジン
        // 状態を同期変更するため逐次
        { OpRule r = ruleG(0, gb(GK::Audio)); r.spawn = true;
          add("sound_play", r); add("sound_playuntildone", r);
          add("sound_stopallsounds", r); }
        // effect/volume 変更は miscLimits 時 promiseNextTick →
        // waitQueue push になるため逐次扱い
        { OpRule r = ruleF(0, fb(TF::Sfx)); r.gw = gb(GK::Audio);
          r.spawn = true;
          add("sound_seteffectto", r); add("sound_changeeffectby", r); }
        add("sound_cleareffects", ruleF(0, fb(TF::Sfx)));
        { OpRule r = ruleF(0, fb(TF::Volume)); r.gw = gb(GK::Audio);
          r.spawn = true;
          add("sound_setvolumeto", r); add("sound_changevolumeby", r); }
        add("sound_volume", ruleF(fb(TF::Volume), 0));

        // --- procedures ---
        add("procedures_return", OpRule{});
        return t;
    }();
    return m;
}

/// 制御構造 op — SUBSTACK/SUBSTACK2 を持ち、固有アクセスは無い
/// （wait/wait_until はルール表で spawn 扱い — waitQueue を共有するため）
bool isStructOp(const std::string& op) {
    return op == "control_if" || op == "control_if_else" ||
           op == "control_repeat" || op == "control_forever" ||
           op == "control_while" || op == "control_repeat_until" ||
           op == "control_for_each" || op == "control_all_at_once" ||
           op == "control_start_as_clone";
}

/// 純粋演算/引数 op — 入力走査のみ
bool isPureOp(const std::string& op) {
    if (op.rfind("operator_", 0) == 0) return true;
    if (op.rfind("argument_", 0) == 0) return true;
    if (op == "procedures_definition" || op == "procedures_prototype" ||
        op == "procedures_declaration") return true;
    return false;
}

/// フィールドの (id, name) から変数を**非破壊**解決。
/// 解決不能（= 実行時に変数生成）なら nullptr。
Variable* resolveVarNoCreate(Target& t, const BlockField* f,
                             const char* type) {
    if (!f) return nullptr;
    if (Variable* v = t.lookupVariableById(f->id)) return v;
    std::u16string name;
    if (f->value.isString() && f->value.asStrPtr())
        name = std::u16string(f->value.asStrPtr()->view());
    return t.lookupVariableByNameAndType(name, type);
}

/// メニュー入力の定数文字列を取得（shadow メニューブロック or リテラル）。
/// 動的入力なら false。
bool menuConstant(Blocks& blocks, const BlockInput* in,
                  std::u16string& out) {
    if (!in) return false;
    std::string id = !in->block.empty() ? in->block : in->shadow;
    if (id.empty()) return false;
    const Block* m = blocks.getBlock(id);
    if (!m) return false;
    if (!m->shadow) return false;   // 動的入力（変数等）
    if (m->opcode == "data_variable") return false;
    // shadow ブロック — フィールド先頭の値を取る（*_menu は単一フィールド）
    if (m->fields.empty()) return false;
    const BlockField& f = m->fields.begin()->second;
    if (!f.value.isString() || !f.value.asStrPtr()) return false;
    out = std::u16string(f.value.asStrPtr()->view());
    return true;
}

struct Walker {
    Runtime& rt;
    Target& target;
    Blocks& blocks;
    AccessSet& out;
    std::unordered_set<const Block*> activeProcs;

    /// チェーン (next リンク) を走査
    void chain(const std::string& firstId, int depth = 0) {
        if (depth > 4096) { out.unknown = true; return; }
        std::string id = firstId;
        while (!id.empty() && !out.unknown) {
            const Block* b = blocks.getBlock(id);
            if (!b) return;
            block(b, depth);
            id = b->next;
        }
    }

    /// スタック位置のブロック: 自身のルール + 入力/サブスタック走査
    void block(const Block* blk, int depth) {
        const std::string& op = blk->opcode;

        // 非 SUBSTACK 入力を順序通り走査（reporter 子ブロック）
        for (const auto& [name, in] : blk->inputs.items) {
            if (name.rfind("SUBSTACK", 0) == 0) continue;
            reporter(in, depth);
        }

        if (op == "procedures_call") { callProc(blk, depth); return; }

        if (isStructOp(op) || isPureOp(op)) {
            // 構造 op — サブスタックを走査
            for (const auto& [name, in] : blk->inputs.items) {
                if (name.rfind("SUBSTACK", 0) == 0 && !in.block.empty())
                    chain(in.block, depth + 1);
            }
            if (op == "control_for_each") {
                // ループ変数への書き込み
                const BlockField* f = field(blk, "VARIABLE");
                writeVar(f, Variable::kScalarType);
            }
            return;
        }

        auto it = opRules().find(op);
        if (it == opRules().end()) {
            // hat ブロック自体はトップにしか現れないが、未登録 op は
            // compat/未実装 — 保守的に unknown
            if (!rt.getIsHat(op)) out.unknown = true;
            return;
        }
        const OpRule& r = it->second;
        applyRule(blk, r);
    }

    /// reporter 位置の子ブロック（emitInputBlock の走査相当）
    void reporter(const BlockInput& in, int depth) {
        if (depth > 4096) { out.unknown = true; return; }
        std::string id = !in.block.empty() ? in.block : in.shadow;
        if (id.empty()) return;
        const Block* blk = blocks.getBlock(id);
        if (!blk) return;
        // data_variable は shadow 位置でも live 評価（codegen と同一規則）
        if (blk->opcode == "data_variable") {
            const BlockField* f = field(blk, "VARIABLE");
            readVar(f, Variable::kScalarType);
            return;
        }
        if (blk->shadow) return;   // 定数 shadow — アクセス無し
        block(blk, depth + 1);     // 非 shadow → opcode ルール + 入力行き
    }

    const BlockField* field(const Block* blk, const std::string& n) const {
        auto it = blk->fields.find(n);
        return it == blk->fields.end() ? nullptr : &it->second;
    }

    void readVar(const BlockField* f, const char* type) {
        if (Variable* v = resolveVarNoCreate(target, f, type))
            out.reads.insert(MemKey::var(v));
        else
            out.unknown = true;   // 未解決 → 実行時変数生成
    }
    void writeVar(const BlockField* f, const char* type) {
        if (!f) return;           // フィールド欠落 → codegen も no-op
        if (Variable* v = resolveVarNoCreate(target, f, type))
            out.writes.insert(MemKey::var(v));
        else
            out.unknown = true;
    }

    void applyRule(const Block* blk, const OpRule& r) {
        if (r.unknown) { out.unknown = true; return; }
        if (r.spawn) out.spawn = true;
        for (int i = 0; i < int(TF::_Count); ++i) {
            TF s = TF(i);
            if (r.fr & fb(s)) out.reads.insert(MemKey::field(&target, s));
            if (r.fw & fb(s)) out.writes.insert(MemKey::field(&target, s));
        }
        for (int i = 0; i < int(GK::_Count); ++i) {
            GK s = GK(i);
            if (r.gr & gb(s)) out.reads.insert(MemKey::global(s));
            if (r.gw & gb(s)) out.writes.insert(MemKey::global(s));
        }
        if (r.var == OpRule::Var::Read)
            readVar(field(blk, "VARIABLE"), Variable::kScalarType);
        else if (r.var == OpRule::Var::Write)
            writeVar(field(blk, "VARIABLE"), Variable::kScalarType);
        if (r.list == OpRule::Var::Read)
            readVar(field(blk, "LIST"), Variable::kListType);
        else if (r.list == OpRule::Var::Write)
            writeVar(field(blk, "LIST"), Variable::kListType);

        // メニュー経由の他ターゲット読み取り
        if (r.menuInput) {
            std::u16string name;
            const BlockInput* in = blk->inputs.find(r.menuInput);
            if (menuConstant(blocks, in, name)) {
                if (name == u"_mouse_") {
                    out.reads.insert(MemKey::global(GK::Io));
                } else if (name == u"_edge_") {
                    // 境界読み — ターゲット自身の位置のみ
                } else if (name != u"_random_") {
                    if (Target* t = rt.getSpriteTargetByName(name)) {
                        out.reads.insert(MemKey::field(t, TF::X));
                        out.reads.insert(MemKey::field(t, TF::Y));
                    } else {
                        out.unknown = true;   // 名前未解決 — 動的かも
                    }
                }
                // _random_ は rng が thread_local のためキー不要
            } else {
                out.unknown = true;   // 動的メニュー
            }
        }
    }

    /// procedures_call — 定義本体を再帰解析（循環 → unknown）
    void callProc(const Block* call, int depth) {
        if (!call->mutation) { out.unknown = true; return; }
        const std::string pc = call->mutation->attr("proccode");
        const std::string defId = blocks.getProcedureDefinition(pc);
        const Block* def = defId.empty() ? nullptr : blocks.getBlock(defId);
        // 未定義手続き呼び出しは no-op（JIT/TW とも NOP）— 依存なし
        if (!def) return;
        if (!activeProcs.insert(def).second) { out.unknown = true; return; }
        chain(def->next, depth + 1);
        activeProcs.erase(def);
    }
};

} // namespace

std::string describeKey(const MemKey& k) {
    char buf[96];
    if (k.kind == MemKey::Var) {
        const Variable* v = static_cast<const Variable*>(k.a);
        std::string nm = SStr::toUtf8(std::u16string_view(v->name));
        std::snprintf(buf, sizeof buf, "var:%s", nm.c_str());
    } else if (k.kind == MemKey::Field) {
        const Target* t = static_cast<const Target*>(k.a);
        static const char* names[] = {"x","y","dir","size","vis","rot",
                                      "costume","effect","layer","bubble",
                                      "drag","vol","sfx"};
        std::string tn = SStr::toUtf8(t->getName());
        std::snprintf(buf, sizeof buf, "f:%s@%s", names[k.sub], tn.c_str());
    } else {
        static const char* names[] = {"io","timer","monitor","say",
                                      "audio","counter","backdrop"};
        std::snprintf(buf, sizeof buf, "g:%s", names[k.sub]);
    }
    return buf;
}

AccessSet analyzeScript(Runtime& rt, Target& target, Blocks& blocks,
                        const Block* top) {
    AccessSet as;
    if (!top) { as.unknown = true; return as; }
    Walker w{rt, target, blocks, as, {}};
    if (!top->next.empty()) w.chain(top->next);
    return as;
}

DepGraph buildDepGraph(const std::vector<const AccessSet*>& sets) {
    DepGraph g;
    g.n = sets.size();
    g.adj.resize(g.n);
    for (size_t i = 0; i < g.n; ++i) {
        for (size_t j = i + 1; j < g.n; ++j) {
            if (!sets[i]->conflicts(*sets[j])) continue;
            g.adj[i].push_back(j);
            g.adj[j].push_back(i);
            g.edges.emplace_back(i, j);
            // 衝突キーの説明（先頭 3 件）
            std::string lbl;
            int shown = 0;
            for (const MemKey& w : sets[i]->writes) {
                if (sets[j]->writes.count(w) || sets[j]->reads.count(w)) {
                    if (shown++) lbl += ",";
                    lbl += describeKey(w);
                    if (shown >= 3) break;
                }
            }
            if (!shown) {
                for (const MemKey& w : sets[j]->writes) {
                    if (sets[i]->reads.count(w)) {
                        if (shown++) lbl += ",";
                        lbl += describeKey(w);
                        if (shown >= 3) break;
                    }
                }
            }
            g.edgeLabels.push_back(std::move(lbl));
        }
    }
    return g;
}

std::vector<std::vector<size_t>> DepGraph::groups() const {
    // union-find
    std::vector<size_t> par(n);
    for (size_t i = 0; i < n; ++i) par[i] = i;
    auto find = [&](size_t x) {
        while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; }
        return x;
    };
    for (size_t i = 0; i < n; ++i)
        for (size_t j : adj[i]) {
            size_t a = find(i), b = find(j);
            if (a != b) par[a] = b;
        }
    // バッチ順を保って成分にまとめる
    std::unordered_map<size_t, size_t> comp;
    std::vector<std::vector<size_t>> out;
    for (size_t i = 0; i < n; ++i) {
        size_t r = find(i);
        auto it = comp.find(r);
        if (it == comp.end()) {
            comp[r] = out.size();
            out.push_back({i});
        } else {
            out[it->second].push_back(i);
        }
    }
    return out;
}

std::string depGraphToDot(const DepGraph& g,
                          const std::vector<std::string>& names) {
    std::string s = "digraph deps {\n  rankdir=LR;\n";
    for (size_t i = 0; i < g.n; ++i) {
        s += "  n" + std::to_string(i) + " [label=\"";
        s += std::to_string(i) + ": ";
        for (char c : names[i]) s += (c == '"' || c == '\\') ? ' ' : c;
        s += "\"];\n";
    }
    for (size_t e = 0; e < g.edges.size(); ++e) {
        auto [a, b] = g.edges[e];
        s += "  n" + std::to_string(a) + " -> n" + std::to_string(b) +
             " [dir=none,label=\"" + g.edgeLabels[e] + "\"];\n";
    }
    s += "}\n";
    return s;
}

} // namespace xyo
