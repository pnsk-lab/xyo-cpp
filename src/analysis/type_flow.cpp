#include "type_flow.hpp"
#include "type_sites.hpp"

#include <algorithm>
#include <deque>
#include <string_view>
#include <unordered_set>
#include "../model/blocks.hpp"
#include "../model/variable.hpp"
#include "../model/target.hpp"

namespace xyo::analysis {
namespace {

constexpr uint32_t kAny =
    Variable::kObservedNumber | Variable::kObservedString |
    Variable::kObservedBoolean | Variable::kObservedColor |
    Variable::kObservedOther;

// A Scratch field can reference a variable by ID or, in malformed/legacy
// projects, resolve through its name. Canonicalize both forms to the same
// Variable::id before recording any type fact. With no Target (unit tests),
// the caller must provide already canonical IDs.
std::string variableKey(Target* target, const BlockField& field) {
    if (!target) return field.id;
    // ID lookup takes precedence over a name fallback. In particular, if
    // an ID resolves to a list or broadcast, never replace it with an
    // unrelated scalar of the same name.
    if (Variable* variable = target->lookupVariableById(field.id)) {
        return variable->type == Variable::kScalarType
            ? variable->id : std::string();
    }
    if (!field.value.isString() || field.value.isEmptyString())
        return {};
    const SStr* name = field.value.asStrPtr();
    if (!name) return {};
    Variable* variable = target->lookupVariableByNameAndType(
        std::u16string(name->view()), Variable::kScalarType);
    return variable ? variable->id : std::string();
}

uint32_t get(const TypeState& state, const std::string& id) {
    const auto it = state.find(id);
    return it == state.end() ? kAny : it->second;
}

void put(TypeState& state, const std::string& id, uint32_t kinds) {
    if (id.empty()) {
        state.clear();
    } else if (kinds == kAny || kinds == 0) {
        state.erase(id);
    } else {
        state[id] = kinds;
    }
}

const Block* inputBlock(const Blocks& blocks, const BlockInput* input) {
    if (!input) return nullptr;
    const std::string& id = input->block.empty()
        ? input->shadow : input->block;
    return blocks.getBlock(id);
}

bool isPureReporter(const std::string& op) {
    // Never trust an arbitrary opcode merely because its name starts with
    // "operator_" or "math_": extension reporters may have side effects.
    static const std::unordered_set<std::string_view> pure = {
        "math_number", "math_integer", "math_whole_number",
        "math_positive_number", "math_angle", "math_boolean",
        "operator_add", "operator_subtract", "operator_multiply",
        "operator_divide", "operator_mod", "operator_round",
        "operator_mathop", "operator_random", "operator_lt",
        "operator_gt", "operator_equals", "operator_and",
        "operator_or", "operator_not", "operator_join",
        "operator_letter_of", "operator_substring",
        "operator_length", "operator_contains", "operator_count",
        "text", "colour_picker", "data_variable",
        "data_itemoflist", "data_lengthoflist", "data_itemnumoflist",
        "data_listcontainsitem", "argument_reporter_string_number",
        "argument_reporter_boolean", "argument_reporter",
        "motion_xposition", "motion_yposition", "motion_direction",
        "looks_size", "looks_costumenumbername", "looks_backdropnumbername",
        "sensing_answer", "sensing_timer", "sensing_dayssince2000",
        "sensing_loudness", "sensing_mousex", "sensing_mousey",
        "sensing_mousedown", "sensing_keypressed", "sensing_touchingobject",
        "sensing_touchingcolor", "sensing_coloristouchingcolor",
        "sensing_distanceto", "sensing_of", "sensing_current",
        "sensing_username", "sensing_userid", "sensing_isloggedin",
        "control_get_counter", "sound_volume",
    };
    return pure.count(op) != 0;
}

bool pureInput(const Blocks& blocks, const BlockInput* input,
               unsigned depth = 0) {
    if (depth > 64) return false;
    const Block* block = inputBlock(blocks, input);
    if (!block || !isPureReporter(block->opcode)) return false;
    for (const auto& [name, child] : block->inputs.items) {
        (void)name;
        if (!pureInput(blocks, &child, depth + 1)) return false;
    }
    return true;
}

uint32_t inputKinds(const Blocks& blocks, const BlockInput* input,
                    const TypeState& state, Target* target) {
    const Block* block = inputBlock(blocks, input);
    if (!block) return kAny;
    if (block->opcode == "data_variable") {
        const auto field = block->fields.find("VARIABLE");
        return field == block->fields.end() ? kAny
            : get(state, variableKey(target, field->second));
    }
    return inferInputStorageKinds(blocks, input);
}

bool pureStackInputs(const Blocks& blocks, const Block& block) {
    for (const auto& [name, input] : block.inputs.items) {
        if (name.rfind("SUBSTACK", 0) == 0) continue;
        if (!pureInput(blocks, &input)) return false;
    }
    return true;
}

TypeState transfer(const Blocks& blocks, const FlowNode& node,
                   TypeState state, Target* target) {
    if (node.kind == FlowNodeKind::LoopLatch ||
        node.kind == FlowNodeKind::Barrier) {
        // A Scratch yield is a global external mutation barrier. No loop
        // invariant may be inferred from values before the handoff.
        state.clear();
        return state;
    }
    if (node.kind != FlowNodeKind::Statement) return state;

    const Block* block = blocks.getBlock(node.blockId);
    if (!block) {
        state.clear();
        return state;
    }
    const std::string& op = block->opcode;
    if (op == "data_setvariableto" || op == "data_changevariableby") {
        auto it = block->fields.find("VARIABLE");
        const std::string id = it == block->fields.end()
            ? std::string() : variableKey(target, it->second);
        const BlockInput* input = block->inputs.find("VALUE");
        const bool pure = pureInput(blocks, input);
        const uint32_t kinds = op == "data_changevariableby"
            ? Variable::kObservedNumber
            : inputKinds(blocks, input, state, target);
        if (!pure) state.clear();
        put(state, id, kinds);
        return state;
    }

    if (op == "control_if" || op == "control_if_else" ||
        op == "control_repeat" || op == "control_repeat_until" ||
        op == "control_while" || op == "control_forever" ||
        op == "control_for_each") {
        // Reporter inputs are evaluated before branching; unknown extensions
        // may have observable effects including changes to variables.
        if (!pureStackInputs(blocks, *block)) state.clear();
        if (op == "control_for_each") {
            // Scratch's for-each changes VARIABLE implicitly at iteration
            // entry. It can also execute zero iterations, so neither an
            // unconditionally numeric post-state nor the incoming type is
            // valid on *both* outgoing CFG edges. Kill the fact conservatively.
            auto field = block->fields.find("VARIABLE");
            const std::string id = field == block->fields.end()
                ? std::string() : variableKey(target, field->second);
            if (id.empty()) state.clear();
            else state.erase(id);
        }
        return state;
    }

    // A non-assignment stack op is assumed pure only if it is a known
    // reporter. Procedures, broadcasts, waits, arbitrary extension blocks,
    // host changes and other opaque calls all invalidate the entire state.
    if (!isPureReporter(op) || !pureStackInputs(blocks, *block))
        state.clear();
    return state;
}

// Intersection of known keys, union of possible kinds. A missing key is ANY.
bool joinInto(TypeState& dest, const TypeState& incoming) {
    bool changed = false;
    for (auto it = dest.begin(); it != dest.end();) {
        const uint32_t combined = get(incoming, it->first) | it->second;
        if (combined == kAny) {
            it = dest.erase(it);
            changed = true;
        } else {
            if (combined != it->second) {
                it->second = combined;
                changed = true;
            }
            ++it;
        }
    }
    return changed;
}

} // namespace

uint32_t TypeFlowResult::kindBefore(
    size_t node, const std::string& variableId) const {
    return node < before.size() && reachable[node]
        ? get(before[node], variableId) : kAny;
}

uint32_t TypeFlowResult::kindAfter(
    size_t node, const std::string& variableId) const {
    return node < after.size() && reachable[node]
        ? get(after[node], variableId) : kAny;
}

TypeFlowResult analyzeTypeFlow(const Blocks& blocks,
                               const std::string& firstBlock,
                               size_t maxNodes, Target* target) {
    TypeFlowResult result;
    result.cfg = buildFlowGraph(blocks, firstBlock, maxNodes);
    const size_t n = result.cfg.nodes.size();
    result.before.resize(n);
    result.after.resize(n);
    result.reachable.resize(n, 0);

    std::deque<size_t> queue{result.cfg.entry};
    std::vector<uint8_t> queued(n, 0);
    queued[result.cfg.entry] = 1;
    result.reachable[result.cfg.entry] = 1;

    while (!queue.empty()) {
        const size_t node = queue.front();
        queue.pop_front();
        queued[node] = 0;

        result.after[node] =
            transfer(blocks, result.cfg.nodes[node], result.before[node],
                     target);
        for (size_t next : result.cfg.nodes[node].successors) {
            bool changed = false;
            if (!result.reachable[next]) {
                result.before[next] = result.after[node];
                result.reachable[next] = 1;
                changed = true;
            } else {
                changed = joinInto(result.before[next], result.after[node]);
            }
            if (changed && !queued[next]) {
                queued[next] = 1;
                queue.push_back(next);
            }
        }
    }
    return result;
}

} // namespace xyo::analysis
