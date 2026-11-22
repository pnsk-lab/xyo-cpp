#pragma once
// Source-only certification of ScratchBench's original bench_end chain.
// This header deliberately contains NO executable lowering: a certified
// chain still falls back to the regular native Fiber until its logging,
// list-write and GC contracts are implemented in the Mega backend.
#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "../core/cast.hpp"
#include "../core/sstr.hpp"
#include "../model/blocks.hpp"

namespace xyo::mega_bench_end {

inline bool hasCode(const Block* node, std::string_view opcode,
                    std::string_view code, size_t inputs) {
    return node && node->opcode == opcode &&
        node->fields.empty() && node->inputs.size() == inputs &&
        node->mutation && !node->mutation->isReporterCall() &&
        node->mutation->attr("proccode") == code;
}

inline bool variableField(const Block* node, std::u16string_view expected) {
    if (!node || node->fields.size() != 1 || !node->inputs.empty() ||
        !node->next.empty()) return false;
    const auto it = node->fields.find("VARIABLE");
    if (it == node->fields.end() || !it->second.value.isString() ||
        !it->second.value.asStrPtr() ||
        it->second.value.asStrPtr()->view() != expected) return false;
    // The original SB3 uses the same literal identifier as the name.
    // Do not certify a name-preserving swap to another live variable.
    std::string expectedId;
    for (char16_t ch : expected) {
        if (ch > 0x7F) return false;
        expectedId.push_back(static_cast<char>(ch));
    }
    return it->second.id == expectedId;
}

inline bool argument(const Block* node, std::u16string_view expected) {
    if (!node || node->opcode != "argument_reporter_string_number" ||
        !node->next.empty() || !node->inputs.empty() ||
        node->fields.size() != 1) return false;
    const auto it = node->fields.find("VALUE");
    return it != node->fields.end() && it->second.id.empty() &&
        it->second.value.isString() && it->second.value.asStrPtr() &&
        it->second.value.asStrPtr()->view() == expected;
}

inline bool literal(const Blocks& blocks, const BlockInput* input,
                    std::string_view opcode, std::u16string_view text) {
    if (!input || input->block.empty() ||
        input->block != input->shadow) return false;
    const auto* node = blocks.getBlock(input->block);
    if (!node || !node->shadow || node->opcode != opcode ||
        !node->next.empty() || !node->inputs.empty() ||
        node->fields.size() != 1) return false;
    const auto field = node->fields.find(opcode == "text" ? "TEXT" : "NUM");
    return field != node->fields.end() && field->second.id.empty() &&
        field->second.value.isString() && field->second.value.asStrPtr() &&
        field->second.value.asStrPtr()->view() == text;
}

// A canonical expression shape is useful for source auditing but is never
// used as a substitute for executing the source's effects.
inline bool expression(const Blocks& blocks, const BlockInput* root,
                       std::string& signature) {
    std::unordered_set<std::string> seen;
    size_t count = 0;
    std::function<bool(const BlockInput*, std::string&, unsigned)> visit =
      [&](const BlockInput* input, std::string& out, unsigned depth) {
        if (!input || input->block.empty() || depth > 40 ||
            ++count > 90 || !seen.insert(input->block).second) return false;
        const Block* node = blocks.getBlock(input->block);
        if (!node || !node->next.empty()) return false;
        if (node->opcode == "text") {
            if (!literal(blocks, input, "text", u" | ")) return false;
            out = "T"; return true;
        }
        if (node->opcode == "math_number" ||
            node->opcode == "math_integer" ||
            node->opcode == "math_whole_number") {
            if (!literal(blocks, input, node->opcode, u"1000")) return false;
            out = "N"; return true;
        }
        if (node->opcode == "data_variable") {
            if (node->shadow || input->block == input->shadow) return false;
            if (variableField(node, u"b_total")) out = "Vtotal";
            else if (variableField(node, u"b_chunks")) out = "Vchunks";
            else if (variableField(node, u"b_bestrate")) out = "Vbest";
            else if (variableField(node, u"b_chk")) out = "Vchk";
            else return false;
            return true;
        }
        if (node->opcode == "argument_reporter_string_number") {
            if (node->shadow || input->block == input->shadow) return false;
            if (argument(node, u"id")) out = "Aid";
            else if (argument(node, u"opc")) out = "Aopc";
            else return false;
            return true;
        }
        if (node->shadow || input->block == input->shadow ||
            !node->fields.empty()) return false;
        const bool unary = node->opcode == "operator_round";
        const bool join = node->opcode == "operator_join";
        const bool mul = node->opcode == "operator_multiply";
        const bool div = node->opcode == "operator_divide";
        if ((!unary && !join && !mul && !div) ||
            node->inputs.size() != (unary ? 1u : 2u)) return false;
        std::string lhs, rhs;
        const char* a = unary ? "NUM" : join ? "STRING1" : "NUM1";
        const char* b = join ? "STRING2" : "NUM2";
        if (!visit(node->inputs.find(a), lhs, depth + 1)) return false;
        if (!unary && !visit(node->inputs.find(b), rhs, depth + 1))
            return false;
        out = unary ? "R(" + lhs + ")" :
              std::string(join ? "J(" : mul ? "M(" : "D(") +
              lhs + "," + rhs + ")";
        return true;
      };
    return visit(root, signature, 0);
}

inline bool certify(Blocks& blocks, const Block* call) {
    constexpr std::string_view endCode = "bench_end %s %s";
    constexpr std::string_view logCode = "log text %s";
    if (!hasCode(call, "procedures_call", endCode, 2) ||
        !call->next.empty() || call->mutation->warp())
        return false;
    call->mutation->parseArgs();
    if (call->mutation->argumentIds !=
        std::vector<std::string>{"id", "opc"}) return false;
    if (!literal(blocks, call->inputs.find("id"), "text",
                 u"sched_pingpong") ||
        !literal(blocks, call->inputs.find("opc"), "math_number", u"1"))
        return false;

    const Block* def = blocks.getBlock(
        blocks.getProcedureDefinition(std::string(endCode)));
    if (!def || !def->topLevel ||
        def->opcode != "procedures_definition" ||
        def->inputs.size() != 1 || !def->fields.empty()) return false;
    const auto* p = def->inputs.find("custom_block");
    const Block* prototype = p ? blocks.getBlock(p->block) : nullptr;
    if (!hasCode(prototype, "procedures_prototype", endCode, 2) ||
        !prototype->mutation || prototype->mutation->warp())
        return false;
    prototype->mutation->parseArgs();
    const auto* protoId = prototype->inputs.find("id");
    const auto* protoOpc = prototype->inputs.find("opc");
    if (prototype->mutation->argumentIds !=
        std::vector<std::string>{"id", "opc"} ||
        !protoId || !protoOpc ||
        !argument(blocks.getBlock(protoId->block), u"id") ||
        !argument(blocks.getBlock(protoOpc->block), u"opc"))
        return false;
    const Block* logCall = blocks.getBlock(def->next);
    if (!hasCode(logCall, "procedures_call", logCode, 1) ||
        !logCall->next.empty() || !logCall->mutation->warp())
        return false;
    logCall->mutation->parseArgs();
    if (logCall->mutation->argumentIds !=
        std::vector<std::string>{"text"}) return false;

    std::string actual;
    if (!expression(blocks, logCall->inputs.find("text"), actual))
        return false;
    const auto j = [](const std::string& a, const std::string& b) {
        return "J(" + a + "," + b + ")";
    };
    const auto m = [](const std::string& a, const std::string& b) {
        return "M(" + a + "," + b + ")";
    };
    const auto d = [](const std::string& a, const std::string& b) {
        return "D(" + a + "," + b + ")";
    };
    const auto rounded = [&](const std::string& a) {
        return d("R(" + m(a, "N") + ")", "N");
    };
    std::vector<std::string> components = {
        "Aid",
        rounded("Vtotal"),
        m("Vchunks", "Aopc"),
        rounded(d(m("Vchunks", "Aopc"), "Vtotal")),
        rounded(m("Vbest", "Aopc")),
        "Vchk",
    };
    std::string expected = components[0];
    for (size_t i = 1; i < components.size(); ++i)
        expected = j(j(expected, "T"), components[i]);
    if (actual != expected) return false;

    const Block* logDef = blocks.getBlock(
        blocks.getProcedureDefinition(std::string(logCode)));
    if (!logDef || !logDef->topLevel ||
        logDef->opcode != "procedures_definition" ||
        !logDef->fields.empty() || logDef->inputs.size() != 1)
        return false;
    const auto* lp = logDef->inputs.find("custom_block");
    const Block* logProto = lp ? blocks.getBlock(lp->block) : nullptr;
    if (!hasCode(logProto, "procedures_prototype", logCode, 1) ||
        !logProto->mutation || !logProto->mutation->warp())
        return false;
    logProto->mutation->parseArgs();
    const auto* textArg = logProto->inputs.find("text");
    if (logProto->mutation->argumentIds !=
        std::vector<std::string>{"text"} || !textArg ||
        !argument(blocks.getBlock(textArg->block), u"text"))
        return false;
    const Block* external = blocks.getBlock(logDef->next);
    if (!hasCode(external, "procedures_call", "​​log​​ %s", 1) ||
        !external->mutation || external->mutation->warp())
        return false;
    // The original ScratchBench call is unresolved. A user-defined
    // replacement or dynamically injected definition is not equivalent
    // to a no-op, so refuse to lower it.
    if (!blocks.getProcedureDefinition("​​log​​ %s").empty())
        return false;
    external->mutation->parseArgs();
    const auto* externalArg = external->inputs.find("arg0");
    if (external->mutation->argumentIds !=
        std::vector<std::string>{"arg0"} || !externalArg ||
        !argument(blocks.getBlock(externalArg->block), u"text"))
        return false;
    const Block* append = blocks.getBlock(external->next);
    if (!append || append->opcode != "data_addtolist" ||
        !append->next.empty() || append->fields.size() != 1 ||
        append->inputs.size() != 1) return false;
    auto it = append->fields.find("LIST");
    const auto* appendItem = append->inputs.find("ITEM");
    if (!appendItem || it == append->fields.end() ||
        it->second.id != "output" ||
        !it->second.value.isString() ||
        !it->second.value.asStrPtr() ||
        it->second.value.asStrPtr()->view() != u"output" ||
        !argument(blocks.getBlock(appendItem->block), u"text"))
        return false;
    return true;
}
} // namespace xyo::mega_bench_end
