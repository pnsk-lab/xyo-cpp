// xyo-cpp: parfor.cpp — 並列 append ループの検出器。
#include "parfor.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "../core/cast.hpp"
#include "../core/value.hpp"
#include "../model/blocks.hpp"

namespace xyo::jit {
namespace {

/// 純粋な reporter だけを許可する（副作用・yield・共有状態の読みを避ける）。
/// リスト読みは「append 先でないリスト」に限る。
bool pureReporter(const std::string& op) {
    return op.rfind("operator_", 0) == 0 || op == "data_variable" ||
           op == "math_number" || op == "math_integer" ||
           op == "math_whole_number" || op == "math_positive_number" ||
           op == "text" || op == "data_itemoflist" ||
           op == "data_lengthoflist" || op == "data_listcontainsitem" ||
           op == "data_itemnumoflist" || op == "data_listcontents" ||
           op == "motion_xposition" || op == "motion_yposition" ||
           op == "motion_direction" || op == "looks_size";
}

/// 数値の定数入力（shadow math_number 等）を取り出す。
bool constantNumber(Blocks& blocks, const Block* blk, const char* name,
                    double* out) {
    const BlockInput* in = blk->inputs.find(name);
    if (!in) return false;
    std::string id = !in->block.empty() ? in->block : in->shadow;
    if (id.empty()) return false;
    const Block* b = blocks.getBlock(id);
    if (!b) return false;
    // 数値フィールドを持つ primitive（math_number / *_number 系）
    if (b->opcode != "math_number" && b->opcode != "math_integer" &&
        b->opcode != "math_whole_number" && b->opcode != "math_positive_number")
        return false;
    auto it = b->fields.find("NUM");
    if (it == b->fields.end()) return false;
    *out = cast::toNumber(it->second.value);
    return true;
}

struct Scanner {
    Blocks& blocks;
    const std::unordered_set<std::string>* appended;  // LIST id
    const ParForPlan* plan;
    std::string why;

    bool bad(const std::string& msg) {
        if (why.empty()) why = msg;
        return false;
    }

    /// reporter 入力の木を走査（スタック位置でないブロック）
    bool reporter(const BlockInput& in) {
        std::string id = !in.block.empty() ? in.block : in.shadow;
        if (id.empty()) return true;
        const Block* b = blocks.getBlock(id);
        if (!b) return false;
        if (b->shadow) return true;   // 定数
        return block(b, false);
    }

    bool block(const Block* b, bool statement) {
        const std::string& op = b->opcode;
        if (!pureReporter(op)) {
            if (statement)
                return bad("statement op: " + op);
            return bad("impure input: " + op);
        }
        if (op == "data_itemoflist" || op == "data_lengthoflist" ||
            op == "data_listcontainsitem" || op == "data_itemnumoflist" ||
            op == "data_listcontents") {
            auto it = b->fields.find("LIST");
            if (it != b->fields.end() && appended->count(it->second.id))
                return bad("reads an appended list: " + op);
        }
        for (const auto& [name, in] : b->inputs.items) {
            if (name.rfind("SUBSTACK", 0) == 0) return bad("substack in input");
            if (!reporter(in)) return false;
        }
        return true;
    }
};

}  // namespace

std::optional<ParForPlan> analyzeParFor(Blocks& blocks, const Block* loop,
                                        std::string* why) {
    auto reject = [&](const char* msg) -> std::optional<ParForPlan> {
        if (why) *why = msg;
        return std::nullopt;
    };
    if (!loop) return reject("no loop");
    if (loop->opcode != "control_repeat") return reject("not control_repeat");
    const BlockInput* sub = loop->inputs.find("SUBSTACK");
    if (!sub || sub->block.empty()) return reject("empty body");

    ParForPlan plan;
    plan.loop = loop;
    plan.bodyHead = blocks.getBlock(sub->block);
    if (!plan.bodyHead) return reject("body head missing");

    // --- pass 1: 直鎖を走査してカウンタと append 先を集める ---
    std::unordered_map<std::string, size_t> listIndex;
    std::string counterId;
    double delta = 0;
    bool sawChange = false;
    for (const Block* b = plan.bodyHead; b;) {
        const std::string& op = b->opcode;
        if (op == "data_changevariableby") {
            auto it = b->fields.find("VARIABLE");
            if (it == b->fields.end()) return reject("change without VARIABLE");
            if (counterId.empty()) counterId = it->second.id;
            else if (counterId != it->second.id)
                return reject("two induction counters");
            double k = 0;
            if (!constantNumber(blocks, b, "VALUE", &k))
                return reject("non-constant change step");
            delta += k;
            sawChange = true;
            plan.counterField = &it->second;
        } else if (op == "data_addtolist") {
            auto it = b->fields.find("LIST");
            if (it == b->fields.end()) return reject("add without LIST");
            if (listIndex.count(it->second.id))
                return reject("same list appended twice per iteration");
            listIndex.emplace(it->second.id, plan.lists.size());
            plan.lists.push_back(&it->second);
        } else {
            return reject(("statement op: " + op).c_str());
        }
        if (!b->next.empty()) b = blocks.getBlock(b->next);
        else b = nullptr;
    }
    if (plan.lists.empty()) return reject("no append");
    if (sawChange) {
        if (!(delta >= 1.0) || delta != std::floor(delta))
            return reject("counter step is not a positive integer");
        plan.delta = delta;
    } else {
        // カウンタ無し = 本体が反復不変 → 並列化しても意味が無い
        return reject("no induction counter");
    }

    // --- pass 2: 入力の reporter 木を検査 ---
    std::unordered_set<std::string> appended;
    for (const BlockField* f : plan.lists) appended.insert(f->id);
    Scanner sc{blocks, &appended, &plan, {}};
    for (const Block* b = plan.bodyHead; b;) {
        for (const auto& [name, in] : b->inputs.items) {
            (void)name;
            if (!sc.reporter(in)) {
                if (why) *why = sc.why;
                return std::nullopt;
            }
        }
        if (!b->next.empty()) b = blocks.getBlock(b->next);
        else b = nullptr;
    }
    // カウンタ以外を書く op は pass 1 で弾いている（change/add のみ許可）。
    return plan;
}

}  // namespace xyo::jit
