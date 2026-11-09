// Backend-neutral Scratch execution/effect graph.
//
// Captures ALL source nodes (including unknown extensions and procedures).
// Semantics unsupported by a backend must remain opaque: no silent deletion
// and no assumption that a reported value is a pure expression.
// Edges refer to stable indices in the immutable ScratchScript snapshot.
// This view neither owns GC roots nor encodes an executable SSA value graph.
#pragma once

#include "scratch_script.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xyo::ir {

enum class ScratchNodeKind : uint8_t {
    Hat, Constant, ReadVariable, WriteVariable, ChangeVariable,
    ReadList, WriteList, Arithmetic, Compare, Boolean, Control,
    Procedure, Yield, Effect, Opaque
};

enum class ScratchEffect : uint16_t {
    None = 0,
    ReadState = 1 << 0,
    WriteState = 1 << 1,
    MayAllocate = 1 << 2,
    MayYield = 1 << 3,
    MayCall = 1 << 4,
    Unknown = 1 << 5,
    // These are barriers, even when a Scratch statement never yields.
    // A speculative replay must never duplicate externally visible work.
    Observable = 1 << 6,
    MaySpawn = 1 << 7,
};
constexpr ScratchEffect operator|(ScratchEffect lhs, ScratchEffect rhs) {
    return static_cast<ScratchEffect>(static_cast<uint16_t>(lhs) |
                                      static_cast<uint16_t>(rhs));
}
constexpr bool hasEffect(ScratchEffect actual, ScratchEffect flag) {
    return (static_cast<uint16_t>(actual) &
            static_cast<uint16_t>(flag)) != 0;
}

// Opcode classification is conservative: this function does not claim
// that any extension can be compiled by either backend. It merely identifies
// effects that CANNOT be re-executed after an optimistic bailout.
inline ScratchEffect externalEffectFlags(std::string_view opcode) {
    const auto startsWith = [opcode](std::string_view prefix) {
        return opcode.substr(0, prefix.size()) == prefix;
    };
    if (opcode == "event_broadcast" ||
        opcode == "event_broadcastandwait" ||
        opcode == "control_create_clone_of" ||
        opcode == "control_delete_this_clone")
        return ScratchEffect::Observable | ScratchEffect::MaySpawn |
               ScratchEffect::WriteState | ScratchEffect::MayYield;
    if (startsWith("motion_") || startsWith("looks_") ||
        startsWith("sound_") || startsWith("pen_") ||
        startsWith("music_") || startsWith("videoSensing_") ||
        startsWith("text2speech_") || startsWith("translate_") ||
        startsWith("sensing_") || opcode == "operator_random" ||
        opcode == "data_showvariable" || opcode == "data_hidevariable" ||
        opcode == "data_showlist" || opcode == "data_hidelist")
        return ScratchEffect::Observable;
    return ScratchEffect::None;
}

// Only verified, fully staged effects are eligible for speculative replay.
// External writes, threads, clock-dependent reads, yields, calls and unknown
// extensions remain interpreter-owned until a shared continuation/effect
// commit protocol is implemented.
constexpr bool mayReplaySpeculatively(ScratchEffect effects) {
    return !hasEffect(effects, ScratchEffect::Unknown) &&
           !hasEffect(effects, ScratchEffect::MayYield) &&
           !hasEffect(effects, ScratchEffect::MayCall) &&
           !hasEffect(effects, ScratchEffect::Observable) &&
           !hasEffect(effects, ScratchEffect::MaySpawn);
}

struct ScratchOperand {
    std::string name;
    std::string activeId;
    std::string shadowId;
    // The active input is preferred over its shadow in Scratch.
    std::string selectedId() const {
        return activeId.empty() ? shadowId : activeId;
    }
};

struct ScratchNode {
    std::string id;
    std::string opcode;
    ScratchNodeKind kind = ScratchNodeKind::Opaque;
    ScratchEffect effects = ScratchEffect::Unknown;
    std::vector<ScratchOperand> operands; // always in source evaluation order
    std::string nextId;
    std::string parentId;
    std::string variableId;
    std::string listId;
    bool isReporter = false;
    bool isProcedure = false;
    bool opaque = true;
};

struct ScratchEffects {
    std::string topId;
    std::vector<ScratchNode> nodes;
    std::unordered_map<std::string, size_t> byId;
    bool valid = false;

    const ScratchNode* getBlock(const std::string& id) const {
        const auto it = byId.find(id);
        return it == byId.end() ? nullptr : &nodes[it->second];
    }
    const ScratchOperand* inputOf(const std::string& id,
                                  std::string_view input) const {
        const auto* n = getBlock(id);
        if (!n) return nullptr;
        for (const auto& edge : n->operands)
            if (edge.name == input) return &edge;
        return nullptr;
    }

    // Union the effects of a reporter or stack subtree. A plain local
    // opcode classification is never sufficient for hoisting: a Boolean
    // operator can contain a variable read or a procedure call.
    // Include next-chain edges only for a whole sequential script. For
    // reporter purity checks, next=false respects the expression boundary.
    ScratchEffect subtreeEffects(const std::string& id,
                                 bool includeNext = false) const {
        if (!valid || !getBlock(id)) return ScratchEffect::Unknown;
        ScratchEffect aggregate = ScratchEffect::None;
        std::vector<std::string> pending{id};
        std::unordered_set<std::string> visited;
        while (!pending.empty()) {
            std::string current = std::move(pending.back());
            pending.pop_back();
            if (!visited.insert(current).second) continue;
            const auto* node = getBlock(current);
            if (!node) return ScratchEffect::Unknown;
            aggregate = aggregate | node->effects;
            for (const auto& operand : node->operands) {
                const std::string selected = operand.selectedId();
                if (!selected.empty()) pending.push_back(selected);
            }
            if (includeNext && !node->nextId.empty())
                pending.push_back(node->nextId);
        }
        return aggregate;
    }

    static ScratchEffects lower(const ScratchScript& source) {
        ScratchEffects graph;
        if (!source.valid() || !source.getBlock(source.topId)) return graph;
        graph.topId = source.topId;
        graph.nodes.reserve(source.nodes.size());
        graph.byId.reserve(source.nodes.size());
        for (const auto& block : source.nodes) {
            ScratchNode node;
            node.id = block.id;
            node.opcode = block.opcode;
            node.nextId = block.next;
            node.parentId = block.parent;
            node.isProcedure = block.opcode.rfind("procedures_", 0) == 0 ||
                               block.opcode.rfind("argument_", 0) == 0;
            const auto& op = block.opcode;
            const auto set = [&](ScratchNodeKind k, ScratchEffect effects) {
                node.kind = k;
                node.effects = effects;
                node.opaque = false;
            };
            if (op.rfind("event_when", 0) == 0 ||
                op == "control_start_as_clone" ||
                op == "procedures_definition")
                set(ScratchNodeKind::Hat, ScratchEffect::ReadState);
            else if (op == "math_number" || op == "math_integer" ||
                     op == "math_whole_number" ||
                     op == "math_positive_number" || op == "math_angle" ||
                     op == "text" || op == "colour_picker")
                set(ScratchNodeKind::Constant, ScratchEffect::None);
            else if (op == "data_variable") {
                set(ScratchNodeKind::ReadVariable, ScratchEffect::ReadState);
                node.isReporter = true;
            } else if (op == "data_setvariableto" ||
                       op == "data_changevariableby") {
                set(op == "data_setvariableto" ?
                    ScratchNodeKind::WriteVariable :
                    ScratchNodeKind::ChangeVariable,
                    ScratchEffect::ReadState | ScratchEffect::WriteState |
                    ScratchEffect::MayAllocate);
            } else if (op == "data_itemoflist" ||
                       op == "data_lengthoflist" ||
                       op == "data_listcontainsitem" ||
                       op == "data_itemnumoflist") {
                set(ScratchNodeKind::ReadList, ScratchEffect::ReadState);
                node.isReporter = true;
            } else if (op == "data_addtolist" ||
                       op == "data_deleteoflist" ||
                       op == "data_deletealloflist" ||
                       op == "data_insertatlist" ||
                       op == "data_replaceitemoflist") {
                set(ScratchNodeKind::WriteList,
                    ScratchEffect::ReadState | ScratchEffect::WriteState |
                    ScratchEffect::MayAllocate);
            } else if (op == "operator_add" ||
                       op == "operator_subtract" ||
                       op == "operator_multiply" ||
                       op == "operator_divide" ||
                       op == "operator_mod" ||
                       op == "operator_round" ||
                       op == "operator_mathop") {
                // Scratch conversions may allocate or parse strings,
                // even when arithmetic operators yield numeric results.
                set(ScratchNodeKind::Arithmetic,
                    ScratchEffect::MayAllocate);
                node.isReporter = true;
            } else if (op == "operator_lt" || op == "operator_gt" ||
                       op == "operator_equals") {
                set(ScratchNodeKind::Compare,
                    ScratchEffect::MayAllocate);
                node.isReporter = true;
            } else if (op == "operator_and" || op == "operator_or" ||
                       op == "operator_not") {
                set(ScratchNodeKind::Boolean, ScratchEffect::None);
                node.isReporter = true;
            } else if (op == "control_if" ||
                       op == "control_if_else") {
                set(ScratchNodeKind::Control, ScratchEffect::None);
            } else if (op == "control_repeat" ||
                       op == "control_repeat_until" ||
                       op == "control_forever" ||
                       op == "control_while" ||
                       op == "control_for_each") {
                set(ScratchNodeKind::Control, ScratchEffect::MayYield);
            } else if (op == "control_wait" ||
                       op == "control_wait_until" ||
                       op == "control_stop" ||
                       op == "control_all_at_once") {
                set(ScratchNodeKind::Yield, ScratchEffect::MayYield |
                    ScratchEffect::WriteState);
            } else if (node.isProcedure) {
                set(ScratchNodeKind::Procedure,
                    ScratchEffect::MayCall | ScratchEffect::MayYield |
                    ScratchEffect::ReadState | ScratchEffect::WriteState |
                    ScratchEffect::MayAllocate);
            } else {
                // Unknown is maximally conservative. This includes extension
                // reporters, sensing, looks, sound, clone, etc., until their
                // exact effects are proven and implemented.
                node.effects = ScratchEffect::Unknown |
                               ScratchEffect::ReadState |
                               ScratchEffect::WriteState |
                               ScratchEffect::MayAllocate |
                               ScratchEffect::MayYield |
                               ScratchEffect::MayCall |
                               externalEffectFlags(op);
            }
            if (const auto it = block.fields.find("VARIABLE");
                it != block.fields.end())
                node.variableId = it->second.id;
            if (const auto it = block.fields.find("LIST");
                it != block.fields.end())
                node.listId = it->second.id;
            node.operands.reserve(block.inputs.items.size());
            for (const auto& [name, input] : block.inputs.items)
                node.operands.push_back({name, input.block, input.shadow});
            graph.byId.emplace(node.id, graph.nodes.size());
            graph.nodes.push_back(std::move(node));
        }
        graph.valid = graph.validate(source);
        return graph;
    }

    bool validate(const ScratchScript& source) const {
        if (!source.valid() || !getBlock(topId) ||
            nodes.size() != source.nodes.size() ||
            byId.size() != nodes.size()) return false;
        for (const auto& node : nodes) {
            if (!source.getBlock(node.id)) return false;
            if (!node.nextId.empty() && !getBlock(node.nextId)) return false;
            for (const auto& input : node.operands) {
                if ((!input.activeId.empty() && !getBlock(input.activeId)) ||
                    (!input.shadowId.empty() && !getBlock(input.shadowId)))
                    return false;
            }
        }
        return true;
    }
};

} // namespace xyo::ir
