#include "type_sites.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>
#include "../core/sstr.hpp"
#include "../engine/runtime.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"

namespace xyo::analysis {
namespace {
using K = Variable;
constexpr uint32_t N = K::kObservedNumber, S = K::kObservedString,
                   B = K::kObservedBoolean, C = K::kObservedColor,
                   U = K::kObservedOther;
constexpr uint32_t Any = N | S | B | C | U;


bool isLoop(const std::string& op) {
    return op == "control_repeat" || op == "control_forever" ||
           op == "control_repeat_until" || op == "control_while" ||
           op == "control_for_each";
}
uint32_t expressionKind(const Blocks& bs, const BlockInput* in, unsigned depth = 0) {
    if (!in || depth > 64) return Any;
    const auto& id = in->block.empty() ? in->shadow : in->block;
    const Block* b = bs.getBlock(id);
    if (!b) return Any;
    const auto& op = b->opcode;
    // Scratch's primitive shadow blocks return the *stored* field value,
    // not an opcode-derived C++ type. In particular, serialized [4,"123"]
    // is a string Value even when used in a numeric input position. Some
    // malformed/custom projects may also carry Boolean or Color values.
    if (op == "math_number" || op == "math_integer" ||
        op == "math_whole_number" || op == "math_positive_number" ||
        op == "math_angle" || op == "math_boolean" ||
        op == "text" || op == "colour_picker") {
        return b->fields.size() == 1
            ? K::observedKind(b->fields.begin()->second.value) : Any;
    }
    if (op == "operator_join" || op == "operator_letter_of" ||
        op == "operator_substring") return S;
    if (op == "operator_equals" || op == "operator_gt" ||
        op == "operator_lt" || op == "operator_and" ||
        op == "operator_or" || op == "operator_not" ||
        op == "operator_contains") return B;
    if (op == "operator_add" || op == "operator_subtract" ||
        op == "operator_multiply" || op == "operator_divide" ||
        op == "operator_mod" || op == "operator_round" ||
        op == "operator_mathop" || op == "operator_random" ||
        op == "operator_length" || op == "operator_count" ||
        op == "data_lengthoflist" || op == "data_itemnumoflist") return N;
    if (op == "data_variable" || op == "data_itemoflist" ||
        op == "argument_reporter_string_number" ||
        op == "procedures_call") return Any;
    if (op == "argument_reporter_boolean") return B;
    return Any; // Unknown extensions and custom reporters are not specialized.
}
Variable* resolve(Target& t, const Block& b) {
    auto it = b.fields.find("VARIABLE");
    if (it == b.fields.end()) return nullptr;
    const BlockField& f = it->second;
    if (auto* v = t.lookupVariableById(f.id)) return v;
    std::u16string name;
    if (f.value.isString() && f.value.asStrPtr())
        name = std::u16string(f.value.asStrPtr()->view());
    return t.lookupVariableByNameAndType(name, Variable::kScalarType);
}
struct Walker {
    Target& t;
    Blocks& bs;
    TypeSiteReport& report;
    std::string target, script;
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> procedures;
    void walk(const std::string& first, bool loop, unsigned depth = 0) {
        if (depth > 128) return;
        std::string id = first;
        std::unordered_set<std::string> chainSeen;
        while (!id.empty() && chainSeen.insert(id).second) {
            const Block* b = bs.getBlock(id);
            if (!b) break;
            // A procedure can be visited through distinct callsites and contexts.
            // Preserve one report per block and whether it occurs in a loop.
            auto visitKey = id + (loop ? ":loop" : ":plain");
            if (!visited.insert(visitKey).second) { id = b->next; continue; }
            if (b->opcode == "data_setvariableto" ||
                b->opcode == "data_changevariableby") {
                TypeWriteSite site;
                site.target = target; site.script = script;
                site.blockId = id; site.opcode = b->opcode;
                site.inLoop = loop;
                Variable* v = resolve(t, *b);
                site.unresolved = !v;
                if (v) site.variable = SStr::toUtf8(v->name);
                else site.variable = "<unresolved>";
                site.kinds = b->opcode == "data_changevariableby"
                    ? N : expressionKind(bs, b->inputs.find("VALUE"));
                report.sites.push_back(std::move(site));
            }
            if (b->opcode == "procedures_call" && b->mutation) {
                const std::string& pc = b->mutation->attr("proccode");
                std::string guard = pc + (loop ? ":loop" : ":plain");
                if (procedures.insert(guard).second) {
                    std::string defId = bs.getProcedureDefinition(pc);
                    if (const Block* def = bs.getBlock(defId))
                        walk(def->next, loop, depth + 1);
                    procedures.erase(guard);
                }
            }
            for (auto& [name, in] : b->inputs.items) {
                if (name.rfind("SUBSTACK", 0) == 0 && !in.block.empty())
                    walk(in.block, loop || isLoop(b->opcode), depth + 1);
            }
            id = b->next;
        }
    }
};
std::string kindsString(uint32_t mask) {
    if (mask == Any) return "unknown";
    std::string s;
    for (auto [bit, name] : {std::pair{N, "number"}, {S, "string"},
                             {B, "boolean"}, {C, "color"}, {U, "other"}}) {
        if (mask & bit) { if (!s.empty()) s += "|"; s += name; }
    }
    return s.empty() ? "unknown" : s;
}
}
uint32_t inferInputStorageKinds(const Blocks& blocks, const BlockInput* input) {
    return expressionKind(blocks, input);
}
TypeSiteReport analyzeTypeSites(Runtime& rt) {
    TypeSiteReport result;
    for (Target* t : rt.targets) {
        if (!t || !t->blocks) continue;
        Blocks& bs = *t->blocks;
        std::string tn = SStr::toUtf8(t->getName());
        for (const auto& topId : bs.getScripts()) {
            const Block* top = bs.getBlock(topId);
            if (!top || top->opcode == "procedures_definition") continue;
            Walker w{*t, bs, result, tn, topId};
            w.walk(top->next, false);
        }
    }
    std::map<std::pair<std::string, std::string>, uint32_t> masks;
    for (auto& site : result.sites) {
        if (site.unresolved) ++result.unknownSites;
        auto& mask = masks[{site.target, site.variable}];
        mask |= site.kinds;
    }
    for (auto& [key, mask] : masks) {
        if ((mask & (mask - 1)) != 0 || mask == Any)
            ++result.polymorphicVariables;
    }
    std::sort(result.sites.begin(), result.sites.end(),
        [](const auto& a, const auto& b) {
            return std::tie(a.target, a.variable, a.script, a.blockId, a.inLoop)
                 < std::tie(b.target, b.variable, b.script, b.blockId, b.inLoop);
        });
    return result;
}
std::string formatTypeSites(const TypeSiteReport& r) {
    std::ostringstream out;
    out << "\n== 変数の格納型遷移候補（静的・保守側） ==\n";
    out << "write sites " << r.sites.size()
        << " / mixed-or-unknown variables " << r.polymorphicVariables
        << " / unresolved writes " << r.unknownSites << "\n";
    for (auto& s : r.sites)
        out << "  " << s.target << "/" << s.variable << " [" << s.script
            << "] block=" << s.blockId << " " << s.opcode << " -> "
            << kindsString(s.kinds) << (s.inLoop ? " (loop)" : "")
            << (s.unresolved ? " (unresolved)" : "") << "\n";
    out << "注: 静的な書き込み候補であり、実行時の型遷移発生を保証しない。"
           " 初期値・他スクリプト・クローン・外部変更は別途考慮する。\n";
    return out.str();
}
} // namespace xyo::analysis
