// xyo-cpp: dp_transform.cpp — 純粋な自己再帰 reporter 手続きのメモ化。
#include "dp_transform.hpp"

#include <cstdlib>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/sstr.hpp"
#include "../core/value.hpp"
#include "../engine/runtime.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"

namespace xyo::dp {
namespace {

constexpr int64_t kMaxEntries = 8192;   // メモ化リストの上限（暴走防止）

bool enabled() {
    const char* e = std::getenv("XYO_DP");
    return !e || e[0] != '0';
}

/// しきい値未満の純粋 opcode だけを許可する。文字列を返す op（join 等）は
/// 除外する: `item # of` は数値等価でキーを合わせるため、文字列の違いを
/// 無視してしまう（"01" と 1 が同じキーになる）。
bool pureOpcode(const std::string& op) {
    static const std::unordered_set<std::string> kPure = {
        "operator_add", "operator_subtract", "operator_multiply",
        "operator_divide", "operator_mod", "operator_round",
        "operator_mathop", "operator_lt", "operator_gt",
        "operator_equals", "operator_and", "operator_or", "operator_not",
        "control_if", "control_if_else", "control_repeat",
        "control_repeat_until", "control_while",
        "procedures_call", "procedures_return",
        "argument_reporter_string_number", "argument_reporter_boolean",
        "math_number", "math_integer", "math_whole_number",
        "math_positive_number", "math_angle",
    };
    return kPure.count(op) != 0;
}

struct Rewriter {
    Blocks& blocks;
    Target& target;
    int64_t uidCounter = 0;

    std::string freshId() {
        for (;;) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "xdp_%08llx",
                          (unsigned long long)(++uidCounter));
            if (!blocks.getBlock(buf)) return buf;
        }
    }

    std::string freshVarId() {
        for (;;) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "xdv_%08llx",
                          (unsigned long long)(++uidCounter));
            if (!target.lookupVariableById(buf)) return buf;
        }
    }

    Block* mk(const char* opcode) {
        const std::string id = freshId();
        Block b;
        b.id = id;
        b.opcode = opcode;
        blocks.createBlock(std::move(b));
        return blocks.getBlock(id);
    }

    /// 変数/リストフィールドを設定する。
    void setRef(const std::string& bid, const char* field,
                const std::u16string& name, const std::string& id) {
        Block* b = blocks.getBlock(bid);
        if (!b) return;
        b->fields[field] =
            BlockField{Value::string(SStr::create(name)), id};
    }

    std::string mkNum(double v) {
        Block* b = mk("math_number");
        b->fields["NUM"] = BlockField{Value::num(v), ""};
        return b->id;
    }

    std::string mkVarRef(const std::u16string& name, const std::string& id) {
        Block* b = mk("data_variable");
        b->fields["VARIABLE"] =
            BlockField{Value::string(SStr::create(name)), id};
        return b->id;
    }

    std::string mkArgRef(const std::u16string& name) {
        Block* b = mk("argument_reporter_string_number");
        b->fields["VALUE"] = BlockField{Value::string(SStr::create(name)), ""};
        return b->id;
    }

    std::string mkBinop(const char* op, const std::string& a,
                        const std::string& b) {
        // 比較は sb3 の正準名 OPERAND1/OPERAND2 を使う（NUM1/NUM2 は
        // sb2 互換名で、ローダの正規化を通らない生成ブロックでは
        // 別入力として扱われてしまう）。
        Block* p = mk(op);
        p->inputs.set("OPERAND1", BlockInput{a, ""});
        p->inputs.set("OPERAND2", BlockInput{b, ""});
        return p->id;
    }
};

/// 手続き 1 個の書き換え。成功なら true。
bool rewriteProcedure(Rewriter& rw, Block* def,
                      const std::string& proccode, const Mutation* proto) {
    Blocks& blocks = rw.blocks;
    Target& target = rw.target;
    if (!proto) return false;
    if (proto->argumentNames.size() != 1) return false;
    if (!proto->warp()) return false;   // 非 warp は yield 意味論を変えない
    const std::u16string paramName = proto->argumentNames[0];

    // --- 本体を辿って純粋性と再帰/return を確認 -------------------------
    std::vector<const Block*> reached;
    std::unordered_set<std::string> seen;
    auto pushChain = [&](std::vector<const Block*>& stack,
                         const std::string& id) {
        if (!id.empty() && seen.insert(id).second) {
            if (const Block* b = blocks.getBlock(id)) stack.push_back(b);
        }
    };
    std::vector<const Block*> stack;
    pushChain(stack, def->next);
    int selfCalls = 0;
    std::vector<const Block*> returns;
    while (!stack.empty()) {
        const Block* b = stack.back();
        stack.pop_back();
        reached.push_back(b);
        const std::string& op = b->opcode;
        if (op == "procedures_call") {
            const std::string pc =
                b->mutation ? b->mutation->attr("proccode") : "";
            if (pc != proccode) return false;   // 他の手続き呼び出しは純粋性不明
            ++selfCalls;
        } else if (op == "procedures_return") {
            const BlockInput* v = b->inputs.find("VALUE");
            if (!v || (v->block.empty() && v->shadow.empty())) return false;
            returns.push_back(b);
        } else if (!pureOpcode(op)) {
            return false;
        }
        // 次ブロック + 全入力（SUBSTACK/SUBSTACK2 も inputs に含まれる）
        for (const auto& [name, in] : b->inputs.items) {
            (void)name;
            pushChain(stack, in.block);
            pushChain(stack, in.shadow);
        }
        pushChain(stack, b->next);
    }
    if (selfCalls == 0 || returns.empty()) return false;

    // --- 親（next / 入力のどちらから参照されているか）を記録 ------------
    struct ParentRef {
        std::string owner;
        std::string input;   // 空 = owner->next
    };
    std::unordered_map<std::string, ParentRef> parents;
    for (const Block* b : reached) {
        if (!b->next.empty()) parents.emplace(b->next, ParentRef{b->id, ""});
        for (const auto& [name, in] : b->inputs.items) {
            if (!in.block.empty()) parents.emplace(in.block, ParentRef{b->id, name});
            if (!in.shadow.empty())
                parents.emplace(in.shadow, ParentRef{b->id, name});
        }
    }
    parents.emplace(def->next, ParentRef{def->id, ""});

    // --- メモ化リストと作業変数を作る -----------------------------------
    const std::string keysId = rw.freshVarId();
    const std::string valsId = rw.freshVarId();
    const std::string resId = rw.freshVarId();
    const std::string idxId = rw.freshVarId();
    const std::u16string keysName = u"__xyo_dp_keys";
    const std::u16string valsName = u"__xyo_dp_vals";
    const std::u16string resName = u"__xyo_dp_r";
    const std::u16string idxName = u"__xyo_dp_i";
    target.variables.emplace(
        keysId, Variable::create(keysId, keysName, Variable::kListType, false));
    target.variables.emplace(
        valsId, Variable::create(valsId, valsName, Variable::kListType, false));
    target.variables.emplace(resId, Variable::create(
        resId, resName, Variable::kScalarType, false));
    target.variables.emplace(idxId, Variable::create(
        idxId, idxName, Variable::kScalarType, false));

    const std::string argRef = rw.mkArgRef(paramName);
    const std::string argRef2 = rw.mkArgRef(paramName);

    // --- 入口ガード: i = item # of arg in keys; if i > 0 then return vals[i]
    {
        Block* setI = rw.mk("data_setvariableto");
        rw.setRef(setI->id, "VARIABLE", idxName, idxId);
        Block* num = rw.mk("data_itemnumoflist");
        rw.setRef(num->id, "LIST", keysName, keysId);
        num->inputs.set("ITEM", BlockInput{argRef, ""});
        setI->inputs.set("VALUE", BlockInput{num->id, ""});

        Block* cond = rw.mk("operator_gt");
        cond->inputs.set("OPERAND1",
                         BlockInput{rw.mkVarRef(idxName, idxId), ""});
        cond->inputs.set("OPERAND2", BlockInput{rw.mkNum(0), ""});
        Block* guard = rw.mk("control_if");
        guard->inputs.set("CONDITION", BlockInput{cond->id, ""});

        Block* hit = rw.mk("procedures_return");
        Block* item = rw.mk("data_itemoflist");
        rw.setRef(item->id, "LIST", valsName, valsId);
        item->inputs.set("INDEX",
                         BlockInput{rw.mkVarRef(idxName, idxId), ""});
        hit->inputs.set("VALUE", BlockInput{item->id, ""});
        guard->inputs.set("SUBSTACK", BlockInput{hit->id, ""});

        const std::string oldHead = def->next;
        guard->next = oldHead;
        setI->next = guard->id;
        def->next = setI->id;
        // 元の先頭ブロックの親は def ではなく guard になった。
        if (!oldHead.empty()) parents[oldHead] = ParentRef{guard->id, ""};
    }

    // --- 各 return: r = value; if len(keys) < cap then add keys/vals; return r
    // 元の return ブロックは**そのまま再利用**する（削除すると
    // deleteBlock が入力ブロックまで再帰削除してしまう）。
    for (const Block* ret : returns) {
        Block* retB = blocks.getBlock(ret->id);
        if (!retB) continue;
        BlockInput* vin = retB->inputs.find("VALUE");
        if (!vin) continue;
        // 戻り値の式を r へ退避し、return 側は r の参照に差し替える。
        const std::string exprBlock =
            vin->block.empty() ? vin->shadow : vin->block;
        if (exprBlock.empty()) continue;

        Block* setR = rw.mk("data_setvariableto");
        rw.setRef(setR->id, "VARIABLE", resName, resId);
        setR->inputs.set("VALUE", BlockInput{exprBlock, ""});
        Block* rref = blocks.getBlock(rw.mkVarRef(resName, resId));
        vin->block = rref->id;
        vin->shadow.clear();

        Block* len = rw.mk("data_lengthoflist");
        rw.setRef(len->id, "LIST", keysName, keysId);
        Block* cond = rw.mk("operator_lt");
        cond->inputs.set("OPERAND1", BlockInput{len->id, ""});
        cond->inputs.set("OPERAND2",
                         BlockInput{rw.mkNum(double(kMaxEntries)), ""});
        Block* store = rw.mk("control_if");
        store->inputs.set("CONDITION", BlockInput{cond->id, ""});

        Block* addKey = rw.mk("data_addtolist");
        rw.setRef(addKey->id, "LIST", keysName, keysId);
        addKey->inputs.set("ITEM", BlockInput{argRef2, ""});
        Block* addVal = rw.mk("data_addtolist");
        rw.setRef(addVal->id, "LIST", valsName, valsId);
        addVal->inputs.set("ITEM",
                           BlockInput{rw.mkVarRef(resName, resId), ""});
        addKey->next = addVal->id;
        store->inputs.set("SUBSTACK", BlockInput{addKey->id, ""});

        store->next = retB->id;
        setR->next = store->id;

        // 元の return を新しい鎖へ差し替える（ブロック自体は残す）。
        auto it = parents.find(ret->id);
        if (it == parents.end()) continue;
        Block* owner = blocks.getBlock(it->second.owner);
        if (!owner) continue;
        if (it->second.input.empty()) {
            if (owner->next == ret->id) owner->next = setR->id;
        } else if (BlockInput* in = owner->inputs.find(it->second.input)) {
            if (in->block == ret->id) in->block = setR->id;
            if (in->shadow == ret->id) in->shadow = setR->id;
        }
    }
    return true;
}

}  // namespace

Stats applyToTarget(Target& target) {
    Stats st;
    if (!target.blocks) return st;
    Blocks& blocks = *target.blocks;
    if (!enabled()) return st;

    // 定義ブロックを列挙（走査中に追加しないよう先にコピー）。
    std::vector<const Block*> defs;
    for (const auto& [id, b] : blocks.allBlocks()) {
        (void)id;
        if (b.opcode == "procedures_definition") defs.push_back(&b);
    }
    if (defs.empty()) return st;

    // dlopen 取得のプロトタイプはキャッシュされる — 書き換え前に確定させる。
    std::vector<std::pair<const Block*, std::string>> work;
    for (const Block* def : defs) {
        const BlockInput* cb = def->inputs.find("custom_block");
        const Block* protoBlk =
            cb ? blocks.getBlock(cb->block.empty() ? cb->shadow : cb->block)
               : nullptr;
        const std::string pc =
            protoBlk && protoBlk->mutation
                ? protoBlk->mutation->attr("proccode")
                : std::string();
        if (pc.empty()) continue;
        work.emplace_back(def, pc);
    }
    if (work.empty()) return st;

    Rewriter rw{blocks, target};
    for (auto& [def, pc] : work) {
        const Mutation* proto = blocks.getProcedurePrototype(pc);
        if (rewriteProcedure(rw, const_cast<Block*>(def), pc, proto)) {
            ++st.procedures;
            st.returns += 1;
        } else {
            ++st.rejected;
        }
    }
    return st;
}

Stats applyToRuntime(Runtime& runtime) {
    Stats total;
    for (Target* t : runtime.targets) {
        if (!t) continue;
        Stats s = applyToTarget(*t);
        total.procedures += s.procedures;
        total.returns += s.returns;
        total.rejected += s.rejected;
    }
    return total;
}

}  // namespace xyo::dp
