// xyo-cpp: project_analysis.cpp — 実行しない構造解析。
#include "project_analysis.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/sstr.hpp"
#include "../engine/runtime.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"

namespace xyo::analysis {

namespace {

// --- opcode 分類 ---------------------------------------------------------

bool isLoopOp(const std::string& op) {
    return op == "control_forever" || op == "control_repeat" ||
           op == "control_repeat_until" || op == "control_while" ||
           op == "control_for_each";
}

/// warp 文脈に入る C ブロック（"all at once" は内側のループが yield しない）
bool isWarpWrap(const std::string& op) { return op == "control_all_at_once"; }

/// yield し得る op。JIT の helperMayYield と interp の step 境界に合わせる。
bool isYieldOp(const std::string& op) {
    static const std::unordered_set<std::string_view> k = {
        "control_wait", "control_wait_until",
        "event_broadcastandwait",
        "looks_sayforsecs", "looks_thinkforsecs",
        "looks_switchbackdroptoandwait",
        "sound_playuntildone",
        "sound_seteffectto", "sound_changeeffectby",
        "sound_setvolumeto", "sound_changevolumeby",
        "sensing_askandwait",
        "motion_glideto", "motion_glidesecstoxy",
        "control_deleteclone",
    };
    return k.count(op) != 0;
}

/// 定数 shadow（math_number / text / colour / menu）はブロック数に数えない。
/// data_variable は shadow 位置でも live 評価なので「コード」として数える。
bool isConstantBlock(const Block& b) {
    return b.shadow && b.opcode != "data_variable";
}

/// procedures_definition の custom_block（prototype）を返す。
const Block* customBlockProto(Blocks& bs, const Block* def) {
    if (!def) return nullptr;
    const BlockInput* in = def->inputs.find("custom_block");
    if (!in) return nullptr;
    std::string id = !in->block.empty() ? in->block : in->shadow;
    return id.empty() ? nullptr : bs.getBlock(id);
}

std::string proccodeOf(Blocks& bs, const Block* def) {
    const Block* proto = customBlockProto(bs, def);
    return proto && proto->mutation ? proto->mutation->attr("proccode")
                                    : std::string{};
}

bool warpOf(Blocks& bs, const Block* def) {
    const Block* proto = customBlockProto(bs, def);
    return proto && proto->mutation && proto->mutation->warp();
}

// --- スクリプト内ウォーカ -------------------------------------------------

struct StatWalker {
    Blocks& bs;
    ScriptInfo& si;
    std::unordered_set<const Block*> seenProcs;
    int loopDepth = 0;
    int warpDepth = 0;
    int depthGuard = 0;
    bool inProc = false;   // 手続き本体の中（selfBlocks に数えない）

    void chain(const std::string& firstId, size_t depth) {
        if (depthGuard++ > 8192) return;
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
        if (!b) return;
        if (isConstantBlock(*b)) return;
        visit(b, depth);
    }

    void visit(const Block* b, size_t depth) {
        const std::string& op = b->opcode;
        ++si.blocks;
        if (!inProc) ++si.selfBlocks;
        if (depth > si.maxDepth) si.maxDepth = depth;

        const bool loop = isLoopOp(op);
        const bool warpwrap = isWarpWrap(op);
        if (loop) {
            ++si.loops;
            // 非 warp のループはループ境界で yield する（OSR 無し・warp 無し）
            if (warpDepth == 0 && !si.warp) si.yields = true;
        }
        if (isYieldOp(op)) {
            si.yields = true;
            si.inherentYield = true;
            if (loopDepth > 0) ++si.patterns.waitInLoop;
        }
        if (op == "event_broadcast" || op == "event_broadcastandwait") {
            ++si.broadcasts;
            if (loopDepth > 0) ++si.patterns.broadcastInLoop;
        }
        if (op == "control_create_clone_of" && loopDepth > 0)
            ++si.patterns.cloneInLoop;
        if ((op == "data_itemoflist" || op == "data_itemnumoflist") &&
            loopDepth > 0)
            ++si.patterns.itemOfInLoop;
        if ((op == "data_listcontainsitem" || op == "data_lengthoflist") &&
            loopDepth > 0)
            ++si.patterns.listScanInLoop;

        // 入力（評価順）— SUBSTACK は文脈付きで辿る
        for (const auto& [name, in] : b->inputs.items) {
            if (name.rfind("SUBSTACK", 0) == 0) continue;
            reporter(in, depth + 1);
        }

        if (op == "procedures_call") {
            ++si.procCalls;
            callProc(b, depth);
            return;
        }

        for (const auto& [name, in] : b->inputs.items) {
            if (name.rfind("SUBSTACK", 0) != 0 || in.block.empty()) continue;
            if (loop) ++loopDepth;
            if (warpwrap) ++warpDepth;
            chain(in.block, depth + 1);
            if (warpwrap) --warpDepth;
            if (loop) --loopDepth;
        }
    }

    /// 手続き本体は 1 回だけ辿る（相互再帰でも有限）。ループ文脈は
    /// 呼出し元から切り離す（本体側のループで数える）。
    void callProc(const Block* call, size_t depth) {
        if (!call->mutation) return;
        const std::string pc = call->mutation->attr("proccode");
        const std::string defId = bs.getProcedureDefinition(pc);
        const Block* def = defId.empty() ? nullptr : bs.getBlock(defId);
        if (!def) return;  // 未定義呼出しは no-op
        if (!seenProcs.insert(def).second) return;
        const int savedLoop = loopDepth, savedWarp = warpDepth;
        const bool savedInProc = inProc;
        loopDepth = 0;
        warpDepth = 0;
        inProc = true;
        if (!def->next.empty()) chain(def->next, depth + 1);
        inProc = savedInProc;
        loopDepth = savedLoop;
        warpDepth = savedWarp;
    }
};

/// トップレベル（topLevel フラグ）から到達できるブロックの内訳。
struct BlockStats {
    size_t code = 0;          // コードブロック総数
    size_t shadow = 0;        // 定数 shadow 総数
    size_t reachCode = 0;     // 到達できるコードブロック
    size_t reachShadow = 0;   // 到達できる定数 shadow
};

BlockStats blockStats(Blocks& bs) {
    BlockStats st;
    for (auto& [id, b] : bs.allBlocks()) {
        (void)id;
        if (isConstantBlock(b))
            ++st.shadow;
        else
            ++st.code;
    }
    std::unordered_set<std::string> seen;
    std::vector<std::string> stack;
    for (auto& [id, b] : bs.allBlocks())
        if (b.topLevel) stack.push_back(id);
    while (!stack.empty()) {
        std::string id = std::move(stack.back());
        stack.pop_back();
        if (id.empty() || !seen.insert(id).second) continue;
        const Block* b = bs.getBlock(id);
        if (!b) continue;
        if (isConstantBlock(*b)) {
            ++st.reachShadow;
            continue;   // 定数は子を持たない
        }
        ++st.reachCode;
        if (!b->next.empty()) stack.push_back(b->next);
        for (const auto& [n, in] : b->inputs.items) {
            (void)n;
            if (!in.block.empty()) stack.push_back(in.block);
            if (!in.shadow.empty()) stack.push_back(in.shadow);
        }
    }
    return st;
}

// --- 表示ヘルパ -----------------------------------------------------------

std::string u8(const std::u16string& s) { return SStr::toUtf8(s); }

std::string flagsOf(const AccessSet& a) {
    std::string s;
    if (a.unknown) s += "unknown,";
    if (a.spawn) s += "spawn,";
    if (a.mainThread) s += "main,";
    if (s.empty()) return "worker";
    s.pop_back();
    return s;
}

std::string truncate(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    // UTF-8 の途中で切らない
    size_t cut = n;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "…";
}

const char* kindName(ScriptKind k) {
    switch (k) {
        case ScriptKind::Hat: return "hat";
        case ScriptKind::Definition: return "def";
        default: return "top";
    }
}

// --- ビルダ ---------------------------------------------------------------

struct Builder {
    Runtime& rt;
    Report rep;
    /// Variable* → "name@owner"（レポートのラベル用）
    std::unordered_map<const Variable*, std::string> varLabel;

    explicit Builder(Runtime& r) : rt(r) {}

    void collectVariableLabels() {
        for (Target* t : rt.targets) {
            if (!t) continue;
            const std::string tn = u8(t->getName());
            for (auto& [id, v] : t->variables) {
                (void)id;
                std::string label = u8(v.name);
                if (!t->isStage) label += "@" + tn;
                if (v.type == Variable::kListType) label += " (list)";
                varLabel.emplace(&v, std::move(label));
            }
        }
    }

    std::string labelOfKey(const MemKey& k) const {
        if (k.kind != MemKey::Var) return describeKey(k);
        auto it = varLabel.find(static_cast<const Variable*>(k.a));
        return std::string("var:") + (it != varLabel.end() ? it->second
                                                           : describeKey(k));
    }

    void run(const std::string& path, const Options& opt);
    void analyzeParallel();
    void buildConflicts(size_t topConflicts);
    void buildFindings();
};

void Builder::run(const std::string& path, const Options& opt) {
    rep.path = path;
    collectVariableLabels();

    // --- ターゲットとスクリプト ---
    for (Target* t : rt.targets) {
        if (!t || !t->blocks) continue;
        TargetInfo ti;
        ti.name = u8(t->getName());
        ti.isStage = t->isStage;
        const BlockStats bs = blockStats(*t->blocks);
        ti.blocks = bs.code;
        ti.reachable = bs.reachCode;
        rep.orphanedShadows += bs.shadow - bs.reachShadow;
        ti.costumes = t->sprite ? t->sprite->costumes.size() : 0;
        ti.sounds = t->sprite ? t->sprite->sounds.size() : 0;
        for (auto& [id, v] : t->variables) {
            (void)id;
            if (v.type == Variable::kListType)
                ++ti.lists;
            else if (v.type != Variable::kBroadcastType)
                ++ti.variables;
        }
        ti.scripts = t->blocks->getScripts().size();
        rep.totalBlocks += ti.blocks;
        rep.targets.push_back(ti);

        for (const std::string& topId : t->blocks->getScripts()) {
            const Block* top = t->blocks->getBlock(topId);
            if (!top) continue;
            ScriptInfo si;
            si.target = t;
            si.targetName = ti.name;
            si.isStage = t->isStage;
            si.top = top;
            si.topId = topId;
            si.hat = top->opcode;
            si.blocks = 1;       // トップ自身（ハット / 定義）を含める
            si.selfBlocks = 1;

            const bool isDef = top->opcode == "procedures_definition";
            if (isDef) {
                si.proccode = proccodeOf(*t->blocks, top);
                si.warp = warpOf(*t->blocks, top);
                si.kind = ScriptKind::Definition;
                si.label = si.targetName + "/" + si.proccode;
            } else if (rt.getIsHat(top->opcode)) {
                si.kind = ScriptKind::Hat;
                si.label = si.targetName + ":" + si.hat;
            } else {
                si.kind = ScriptKind::ReporterTop;
                si.label = si.targetName + ":" + si.hat;
            }

            StatWalker w{*t->blocks, si, {}, 0, si.warp ? 1 : 0, 0};
            if (!top->next.empty()) w.chain(top->next, 0);

            si.access = analyzeScript(rt, *t, *t->blocks, top, &si.notes);
            for (const MemKey& k : si.access.reads)
                si.reads.push_back(labelOfKey(k));
            for (const MemKey& k : si.access.writes)
                si.writes.push_back(labelOfKey(k));
            std::sort(si.reads.begin(), si.reads.end());
            std::sort(si.writes.begin(), si.writes.end());
            rep.scripts.push_back(std::move(si));
        }
    }

    for (const ScriptInfo& s : rep.scripts) {
        switch (s.kind) {
            case ScriptKind::Hat: ++rep.executableScripts; break;
            case ScriptKind::Definition: ++rep.definitions; break;
            default: ++rep.reporterTops; break;
        }
    }

    // 同じ opcode のハットが複数あると表示が曖昧になるので 2 件目以降に #n を付ける
    {
        std::unordered_map<std::string, size_t> total, seen;
        for (const ScriptInfo& s : rep.scripts) ++total[s.label];
        for (ScriptInfo& s : rep.scripts) {
            const size_t n = ++seen[s.label];
            if (total[s.label] > 1 && n > 1)
                s.label += "#" + std::to_string(n);
        }
    }

    // --- 未到達ブロック ---
    size_t reach = 0;
    for (const TargetInfo& ti : rep.targets) reach += ti.reachable;
    rep.unreachableBlocks = rep.totalBlocks > reach ? rep.totalBlocks - reach : 0;

    // --- 手続き ---
    std::unordered_map<std::string, size_t> callCount;
    for (Target* t : rt.targets) {
        if (!t || !t->blocks) continue;
        for (auto& [id, b] : t->blocks->allBlocks()) {
            (void)id;
            if (b.opcode != "procedures_call" || !b.mutation) continue;
            ++callCount[b.mutation->attr("proccode")];
        }
    }
    for (Target* t : rt.targets) {
        if (!t || !t->blocks) continue;
        for (auto& [id, b] : t->blocks->allBlocks()) {
            (void)id;
            if (b.opcode != "procedures_definition") continue;
            ProcedureInfo pi;
            pi.targetName = u8(t->getName());
            pi.proccode = proccodeOf(*t->blocks, &b);
            pi.warp = warpOf(*t->blocks, &b);
            auto it = callCount.find(pi.proccode);
            pi.calls = it == callCount.end() ? 0 : it->second;
            // 本体（入れ子呼び出しの先は含めない）
            std::unordered_set<std::string> seen;
            std::vector<std::string> stack;
            if (!b.next.empty()) stack.push_back(b.next);
            while (!stack.empty()) {
                std::string bid = std::move(stack.back());
                stack.pop_back();
                if (bid.empty() || !seen.insert(bid).second) continue;
                const Block* bb = t->blocks->getBlock(bid);
                if (!bb || isConstantBlock(*bb)) continue;
                ++pi.blocks;
                if (isLoopOp(bb->opcode)) pi.hasLoop = true;
                if (isYieldOp(bb->opcode)) pi.yields = true;
                if (bb->opcode == "procedures_call" && bb->mutation &&
                    bb->mutation->attr("proccode") == pi.proccode)
                    pi.selfRecursive = true;
                if (!bb->next.empty()) stack.push_back(bb->next);
                for (const auto& [n, in] : bb->inputs.items) {
                    (void)n;
                    if (!in.block.empty()) stack.push_back(in.block);
                    if (!in.shadow.empty()) stack.push_back(in.shadow);
                }
            }
            rep.procedures.push_back(std::move(pi));
        }
    }
    std::sort(rep.procedures.begin(), rep.procedures.end(),
              [](const ProcedureInfo& a, const ProcedureInfo& b) {
                  if (a.targetName != b.targetName) return a.targetName < b.targetName;
                  return a.proccode < b.proccode;
              });

    analyzeParallel();

    // --- 変数使用（未使用の検出）---
    VariableUsage vu = analyzeVariableUsage(rt);
    rep.variableUsageDynamic = vu.dynamic;
    if (vu.ok) {
        for (Target* t : rt.targets) {
            if (!t) continue;
            for (auto& [id, v] : t->variables) {
                (void)id;
                if (vu.used.count(&v)) continue;
                if (v.type == Variable::kListType)
                    ++rep.unusedLists;
                else if (v.type == Variable::kScalarType)
                    ++rep.unusedVariables;
            }
        }
    }

    buildConflicts(opt.topConflicts);
    buildFindings();
}

/// ハットだけがスレッドを spawn する（定義は呼出し側に含まれ、単体
/// reporter は startHats が拾わない）。並列性はハットのみで評価する。
void Builder::analyzeParallel() {
    std::vector<size_t> safe;
    for (size_t i = 0; i < rep.scripts.size(); ++i) {
        const ScriptInfo& s = rep.scripts[i];
        if (s.kind != ScriptKind::Hat) continue;
        const AccessSet& a = s.access;
        if (a.workerSafe()) {
            ++rep.workerSafe;
            safe.push_back(i);
            continue;
        }
        if (a.unknown) ++rep.unknownScripts;
        if (a.spawn) ++rep.spawnScripts;
        if (a.mainThread) ++rep.mainThreadScripts;
    }
    if (safe.empty()) return;
    std::vector<const AccessSet*> sets;
    sets.reserve(safe.size());
    for (size_t i : safe) sets.push_back(&rep.scripts[i].access);
    DepGraph g = buildDepGraph(sets);
    for (const std::vector<size_t>& grp : g.groups()) {
        std::vector<size_t> mapped;
        mapped.reserve(grp.size());
        for (size_t k : grp) mapped.push_back(safe[k]);
        rep.componentSizes.push_back(mapped.size());
        rep.groups.push_back(std::move(mapped));
    }
    std::sort(rep.componentSizes.rbegin(), rep.componentSizes.rend());
    rep.maxComponent =
        rep.componentSizes.empty() ? 0 : rep.componentSizes.front();
}

void Builder::buildConflicts(size_t topConflicts) {
    std::unordered_map<MemKey, std::vector<size_t>, MemKeyHash> writers, readers;
    for (size_t i = 0; i < rep.scripts.size(); ++i) {
        if (rep.scripts[i].kind != ScriptKind::Hat) continue;
        if (!rep.scripts[i].access.workerSafe()) continue;
        for (const MemKey& k : rep.scripts[i].access.writes)
            writers[k].push_back(i);
        for (const MemKey& k : rep.scripts[i].access.reads)
            readers[k].push_back(i);
    }
    std::vector<ConflictInfo> out;
    for (auto& [k, ws] : writers) {
        std::vector<size_t> involved = ws;
        auto rit = readers.find(k);
        if (rit != readers.end())
            involved.insert(involved.end(), rit->second.begin(), rit->second.end());
        std::sort(involved.begin(), involved.end());
        involved.erase(std::unique(involved.begin(), involved.end()),
                       involved.end());
        if (involved.size() < 2) continue;
        ConflictInfo ci;
        ci.key = labelOfKey(k);
        ci.raw = k;
        ci.scripts = std::move(involved);
        ci.writers = ws.size();
        out.push_back(std::move(ci));
    }
    std::sort(out.begin(), out.end(), [](const ConflictInfo& a, const ConflictInfo& b) {
        if (a.scripts.size() != b.scripts.size())
            return a.scripts.size() > b.scripts.size();
        if (a.writers != b.writers) return a.writers > b.writers;
        return a.key < b.key;
    });
    if (topConflicts && out.size() > topConflicts) out.resize(topConflicts);
    rep.conflicts = std::move(out);
}

void Builder::buildFindings() {
    auto add = [&](Finding::Level lv, std::string text) {
        rep.findings.push_back(Finding{lv, std::move(text)});
    };

    if (rep.unreachableBlocks)
        add(Finding::Level::Note,
            "到達不能ブロック " + std::to_string(rep.unreachableBlocks) +
                " 個（topLevel から next/inputs を辿れない）— minify で削除可能");
    if (rep.orphanedShadows)
        add(Finding::Level::Note,
            "孤立 shadow " + std::to_string(rep.orphanedShadows) +
                " 個（未参照の引数レポータ/メニュー等の残骸）— minify で削除可能");
    if (rep.unusedVariables || rep.unusedLists)
        add(Finding::Level::Note,
            "未使用の変数 " + std::to_string(rep.unusedVariables) + " / リスト " +
                std::to_string(rep.unusedLists) + " 個 — minify --aggressive で削除可能");
    if (rep.variableUsageDynamic)
        add(Finding::Level::Note,
            "sensing_of 等の動的変数参照があるため未使用判定は保守側（削除は手動確認）");
    if (rep.spawnScripts)
        add(Finding::Level::Warn,
            "spawn 宣言のハット " + std::to_string(rep.spawnScripts) +
                " 件（broadcast/say/backdrop/stop/wait 等）— バッチ全体が逐次実行になる");
    if (rep.unknownScripts)
        add(Finding::Level::Warn,
            "静的解析不能のハット " + std::to_string(rep.unknownScripts) +
                " 件（compat op / 動的メニュー / 未解決変数）— 逐次実行");
    if (rep.mainThreadScripts)
        add(Finding::Level::Note,
            "mainThread のハット " + std::to_string(rep.mainThreadScripts) +
                " 件（ターゲット/描画の書き換え）— worker には回らない");
    if (rep.reporterTops)
        add(Finding::Level::Note,
            "トップレベル reporter " + std::to_string(rep.reporterTops) +
                " 件 — 実行されない残骸（削除候補）");

    // warp 化候補: ループを持ち、yield しない手続き
    for (const ProcedureInfo& p : rep.procedures) {
        if (p.warp || !p.hasLoop || p.yields) continue;
        add(Finding::Level::Warn,
            "warp 化候補: " + p.targetName + "/" + truncate(p.proccode, 40) +
                " (blocks " + std::to_string(p.blocks) + ", calls " +
                std::to_string(p.calls) + ", ループあり/yield なし)");
    }

    // 未呼び出し手続き（死コード）
    {
        std::string names;
        size_t n = 0;
        for (const ProcedureInfo& p : rep.procedures) {
            if (p.calls) continue;
            ++n;
            if (n <= 5) names += (names.empty() ? "" : ", ") +
                                truncate(p.targetName + "/" + p.proccode, 28);
        }
        if (n)
            add(Finding::Level::Note,
                "未呼び出し手続き " + std::to_string(n) + " 件 — " + names +
                    (n > 5 ? " …" : ""));
    }

    // ループ内アンチパターン
    struct Pat {
        const char* name;
        size_t Patterns::*field;
        const char* advice;
    };
    static const Pat pats[] = {
        {"item # of in loop", &Patterns::itemOfInLoop,
         "線形走査 O(n) — 索引リスト/変数へ置換"},
        {"list scan in loop", &Patterns::listScanInLoop,
         "contains/length の毎回評価 — ループ外へホイスト"},
        {"broadcast in loop", &Patterns::broadcastInLoop,
         "spawn コスト + 逐次化 — ループ外へ出せないか検討"},
        {"clone in loop", &Patterns::cloneInLoop,
         "clone はスケジューラ構造を変える — 可能なら事前生成"},
    };
    for (const Pat& p : pats) {
        size_t total = 0, scripts = 0;
        for (const ScriptInfo& s : rep.scripts) {
            size_t n = s.patterns.*(p.field);
            if (!n) continue;
            total += n;
            ++scripts;
        }
        if (!total) continue;
        std::string text = std::string(p.name) + ": " + std::to_string(total) +
                           " 箇所 (" + std::to_string(scripts) + " スクリプト) — " +
                           p.advice;
        add(Finding::Level::Note, std::move(text));
    }

    if (rep.maxComponent > 1)
        add(Finding::Level::Info,
            "worker 並列: 最大 " + std::to_string(rep.maxComponent) +
                " 同時（workerSafe ハット " + std::to_string(rep.workerSafe) +
                " / 成分 " + std::to_string(rep.componentSizes.size()) + "）");
    else if (rep.executableScripts > 0)
        add(Finding::Level::Info,
            "workerSafe なハットが " +
                std::to_string(rep.workerSafe) + " 件 — 並列スケジューラは寄与しない"
                "（ハットが 1 本鎖・spawn/unknown/main のいずれか）。"
                "高速化は JIT/warp/データ構造側で狙う");
}

}  // namespace

Report analyzeProject(Runtime& rt, const std::string& path, const Options& opt) {
    Builder b(rt);
    b.run(path, opt);
    return std::move(b.rep);
}

// --- テキスト整形 ---------------------------------------------------------

std::string formatText(const Report& r, const Options& opt) {
    std::string o;
    char buf[512];
    auto line = [&](const std::string& s) { o += s; o += '\n'; };
    auto addf = [&](const char* fmt, auto... args) {
        std::snprintf(buf, sizeof buf, fmt, args...);
        o += buf;
        o += '\n';
    };

    addf("%s — 構造レポート", r.path.c_str());
    addf("targets %zu / hat %zu / def %zu / top-reporter %zu / blocks %zu (未到達 %zu, 孤立shadow %zu)",
         r.targets.size(), r.executableScripts, r.definitions, r.reporterTops,
         r.totalBlocks, r.unreachableBlocks, r.orphanedShadows);
    addf("workerSafe %zu / spawn %zu / unknown %zu / main %zu（フラグは重複可）",
         r.workerSafe, r.spawnScripts, r.unknownScripts, r.mainThreadScripts);
    addf("並列上限 %zu 同時（worker 成分 %zu）", r.maxComponent,
         r.componentSizes.size());

    line("");
    line("== ターゲット ==");
    addf("%-18s %-6s %7s %7s %6s %5s %5s %6s", "target", "stage", "blocks",
         "reach", "script", "var", "list", "asset");
    for (const TargetInfo& t : r.targets) {
        char asset[40];
        std::snprintf(asset, sizeof asset, "%zu/%zu", t.costumes, t.sounds);
        addf("%-18s %-6s %7zu %7zu %6zu %5zu %5zu %6s",
             truncate(t.name, 18).c_str(), t.isStage ? "yes" : "-", t.blocks,
             t.reachable, t.scripts, t.variables, t.lists, asset);
    }

    line("");
    line("== スクリプト（hat / def）==");
    line("  self=自己ブロック数 / total=手続き呼出しの先を含む到達ブロック数");
    addf("%4s %-4s %-16s %-36s %6s %6s %5s %4s %4s %3s %3s %3s %s", "#", "kind",
         "target", "hat (def: proccode)", "self", "total", "depth", "loop",
         "proc", "bc", "R", "W", "flags");
    const size_t limit = opt.topScripts ? opt.topScripts : r.scripts.size();
    size_t shown = 0;
    for (size_t i = 0; i < r.scripts.size() && shown < limit; ++i) {
        const ScriptInfo& s = r.scripts[i];
        if (s.kind == ScriptKind::ReporterTop) continue;
        std::string hat = s.kind == ScriptKind::Definition
                              ? "def " + s.proccode
                              : s.hat;
        addf("%4zu %-4s %-16s %-36s %6zu %6zu %5zu %4zu %4zu %3zu %3zu %3zu %s%s",
             i, kindName(s.kind), truncate(s.targetName, 16).c_str(),
             truncate(hat, 36).c_str(), s.selfBlocks, s.blocks, s.maxDepth,
             s.loops, s.procCalls, s.broadcasts, s.reads.size(), s.writes.size(),
             flagsOf(s.access).c_str(), s.yields ? ",yield" : "");
        ++shown;
    }
    if (r.reporterTops)
        addf("（トップレベル reporter %zu 件は表から除外）", r.reporterTops);

    if (!r.conflicts.empty()) {
        line("");
        line("== 直列化ホットスポット（workerSafe ハット間の共有キー）==");
        for (const ConflictInfo& c : r.conflicts) {
            addf("%-44s %zu scripts (%zu writers)", truncate(c.key, 44).c_str(),
                 c.scripts.size(), c.writers);
            std::string who = "    ";
            size_t listed = 0;
            for (size_t idx : c.scripts) {
                if (++listed > 12) break;   // 長大なリストは打ち切る
                const std::string& lbl = r.scripts[idx].label;
                if (who.size() + lbl.size() > 200) { line(who); who = "    "; }
                who += lbl + " ";
            }
            if (c.scripts.size() > 12)
                who += "… +" + std::to_string(c.scripts.size() - 12);
            if (who != "    ") line(who);
        }
    }

    if (!r.procedures.empty()) {
        line("");
        line("== 手続き ==  (blocks は本体のみ・呼出し先を含まない)");
        addf("%-16s %-36s %6s %5s %5s %-4s %-9s %-4s", "target", "proccode",
             "blocks", "calls", "loop", "warp", "recursive", "yield");
        for (const ProcedureInfo& p : r.procedures) {
            addf("%-16s %-36s %6zu %5zu %5s %-4s %-9s %-4s",
                 truncate(p.targetName, 16).c_str(),
                 truncate(p.proccode, 36).c_str(), p.blocks, p.calls,
                 p.hasLoop ? "yes" : "-", p.warp ? "yes" : "-",
                 p.selfRecursive ? "yes" : "-", p.yields ? "yes" : "-");
        }
    }

    // 逐次実行の理由（ハットのみ）
    bool anyNotes = false;
    for (const ScriptInfo& s : r.scripts) {
        if (s.kind == ScriptKind::Hat && !s.notes.empty()) { anyNotes = true; break; }
    }
    if (anyNotes) {
        line("");
        line("== 逐次実行の理由 ==");
        for (const ScriptInfo& s : r.scripts) {
            if (s.kind != ScriptKind::Hat || s.notes.empty()) continue;
            std::string text = "  " + s.label + ":";
            for (const AccessNote& n : s.notes) {
                const char* k = n.kind == AccessNote::Kind::Spawn        ? "spawn"
                                : n.kind == AccessNote::Kind::MainThread ? "main"
                                : n.kind == AccessNote::Kind::VarDynamic ? "var"
                                                                         : "unknown";
                text += " ";
                text += n.opcode.empty() ? "<top>" : n.opcode;
                text += "(";
                text += k;
                if (!n.detail.empty()) { text += ":"; text += n.detail; }
                text += ")";
            }
            line(text);
        }
    }

    line("");
    line("== 指摘 ==");
    if (r.findings.empty()) {
        line("(なし)");
    } else {
        for (const Finding& f : r.findings) {
            const char* tag = f.level == Finding::Level::Warn   ? "WARN"
                              : f.level == Finding::Level::Note ? "note"
                                                                : "info";
            line(std::string("[") + tag + "] " + f.text);
        }
    }

    std::vector<TransformHint> hints = suggestTransforms(r);
    if (!hints.empty()) {
        line("");
        line("== 変換候補 ==");
        for (const TransformHint& h : hints) {
            const char* kind = h.kind == TransformHint::Kind::WarpProcedure
                                   ? "warp化"
                                   : h.kind == TransformHint::Kind::SplitVariable
                                         ? "変数分離"
                                         : "索引化";
            line(std::string("  - [") + kind + "] " + h.target + " — " + h.reason);
        }
    }
    return o;
}

std::string formatDot(const Report& r) {
    std::vector<size_t> safe;
    std::vector<const AccessSet*> sets;
    std::vector<std::string> names;
    for (size_t i = 0; i < r.scripts.size(); ++i) {
        const ScriptInfo& s = r.scripts[i];
        if (s.kind != ScriptKind::Hat || !s.access.workerSafe()) continue;
        safe.push_back(i);
        sets.push_back(&s.access);
        names.push_back(s.label);   // ラベルは #n 付きで一意
    }
    DepGraph g = buildDepGraph(sets);
    return depGraphToDot(g, names);
}

std::vector<TransformHint> suggestTransforms(const Report& r) {
    std::vector<TransformHint> out;
    for (const ProcedureInfo& p : r.procedures) {
        if (p.warp || !p.hasLoop || p.yields) continue;
        out.push_back({TransformHint::Kind::WarpProcedure,
                       p.targetName + "/" + p.proccode,
                       "ループ " + std::to_string(p.blocks) + " ブロック・yield なし・呼出し " +
                           std::to_string(p.calls) + " 回"});
    }
    for (const ConflictInfo& c : r.conflicts) {
        if (c.raw.kind != MemKey::Var || c.writers < 2) continue;
        out.push_back({TransformHint::Kind::SplitVariable, c.key,
                       std::to_string(c.writers) + " スクリプトが書き込む → スプライト別変数 + 集計"});
    }
    // ループ内 item # of は件数の多い順に上位だけ出す（ノイズ抑制）
    std::vector<const ScriptInfo*> scans;
    for (const ScriptInfo& s : r.scripts)
        if (s.patterns.itemOfInLoop) scans.push_back(&s);
    std::sort(scans.begin(), scans.end(),
              [](const ScriptInfo* a, const ScriptInfo* b) {
                  return a->patterns.itemOfInLoop > b->patterns.itemOfInLoop;
              });
    for (size_t i = 0; i < scans.size() && i < 8; ++i) {
        out.push_back({TransformHint::Kind::ListScan, scans[i]->label,
                       "ループ内 item # of × " +
                           std::to_string(scans[i]->patterns.itemOfInLoop) +
                           " — 位置索引の併用を検討"});
    }
    return out;
}

}  // namespace xyo::analysis
