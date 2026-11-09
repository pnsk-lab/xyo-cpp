#include "procedure_effects.hpp"

#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"

namespace xyo::analysis {
namespace {

bool knownReporter(std::string_view op) {
    static const std::unordered_set<std::string_view> kReporters = {
        "math_number", "math_integer", "math_whole_number",
        "math_positive_number", "math_angle", "math_boolean",
        "text", "colour_picker", "data_variable", "data_itemoflist",
        "data_lengthoflist", "data_itemnumoflist",
        "data_listcontainsitem", "data_listcontents",
        "argument_reporter_string_number", "argument_reporter_boolean",
        "argument_reporter", "operator_add", "operator_subtract",
        "operator_multiply", "operator_divide", "operator_mod",
        "operator_round", "operator_mathop", "operator_random",
        "operator_lt", "operator_gt", "operator_equals", "operator_and",
        "operator_or", "operator_not", "operator_join",
        "operator_letter_of", "operator_substring", "operator_length",
        "operator_contains", "operator_count", "sensing_timer",
        "sensing_dayssince2000", "sensing_answer", "sensing_loudness",
        "sensing_mousex", "sensing_mousey", "sensing_mousedown",
        "sensing_keypressed", "sensing_touchingobject",
        "sensing_touchingcolor", "sensing_coloristouchingcolor",
        "sensing_distanceto", "sensing_of", "sensing_current",
        "sensing_username", "sensing_userid", "sensing_isloggedin",
        "motion_xposition", "motion_yposition", "motion_direction",
        "looks_size", "looks_costumenumbername",
        "looks_backdropnumbername", "sound_volume", "control_get_counter",
    };
    return kReporters.count(op) != 0;
}

bool controlBranch(std::string_view op) {
    return op == "control_if" || op == "control_if_else" ||
           op == "control_repeat" || op == "control_repeat_until" ||
           op == "control_forever" || op == "control_while" ||
           op == "control_for_each";
}

bool yieldControl(std::string_view op) {
    return op == "control_wait" || op == "control_wait_until" ||
           op == "control_all_at_once" || op == "control_stop" ||
           op == "control_create_clone_of" ||
           op == "control_delete_this_clone";
}

// Returns a canonical scalar identity, or an empty identity if the field
// cannot be verified. Duplicate names must never be invented as an alias.
std::string scalarKey(Target* target, const BlockField& field) {
    if (!target) return field.id;
    if (Variable* v = target->lookupVariableById(field.id)) {
        return v->type == Variable::kScalarType
            ? v->id : std::string();
    }
    if (!field.value.isString() || field.value.isEmptyString())
        return {};
    SStr* name = field.value.asStrPtr();
    if (!name) return {};
    Variable* v = target->lookupVariableByNameAndType(
        std::u16string(name->view()), Variable::kScalarType);
    return v ? v->id : std::string();
}

struct Analyzer {
    Blocks& blocks;
    Target* target;
    ProcedureEffects result;
    size_t maxBlocks;
    std::unordered_set<std::string> activeProcedures;
    std::unordered_set<std::string> visitedBlocks;

    void unknown() {
        result.unknownScalarWrites = true;
        result.unknownScalarReads = true;
        result.bodyMayYield = true;
    }

    void variable(const Block& b, bool write) {
        auto it = b.fields.find("VARIABLE");
        if (it == b.fields.end()) {
            if (write) result.unknownScalarWrites = true;
            else result.unknownScalarReads = true;
            return;
        }
        std::string id = scalarKey(target, it->second);
        if (id.empty()) {
            if (write) result.unknownScalarWrites = true;
            else result.unknownScalarReads = true;
        } else if (write) {
            result.writtenScalarIds.insert(std::move(id));
        } else {
            result.readScalarIds.insert(std::move(id));
        }
    }

    void procedure(const std::string& code, unsigned depth) {
        if (code.empty() || depth > 128) {
            result.truncated = true;
            unknown();
            return;
        }
        if (!activeProcedures.insert(code).second) {
            result.recursive = true;
            unknown();
            return;
        }
        const std::string definitionId = blocks.getProcedureDefinition(code);
        const Block* definition = blocks.getBlock(definitionId);
        if (!definition || definition->opcode != "procedures_definition") {
            // Missing definitions normally become no-ops, but the effect
            // analyzer cannot trust malformed/relinked procedures.
            unknown();
        } else {
            chain(definition->next, depth + 1);
        }
        activeProcedures.erase(code);
    }

    void chain(const std::string& first, unsigned depth) {
        if (depth > 128) {
            result.truncated = true;
            unknown();
            return;
        }
        std::vector<std::string> todo;
        if (!first.empty()) todo.push_back(first);
        while (!todo.empty()) {
            std::string id = std::move(todo.back());
            todo.pop_back();
            if (!visitedBlocks.insert(id).second) continue;
            if (result.visitedBlocks >= maxBlocks) {
                result.truncated = true;
                unknown();
                return;
            }
            const Block* b = blocks.getBlock(id);
            if (!b) {
                unknown();
                continue;
            }
            ++result.visitedBlocks;
            const std::string& op = b->opcode;
            if (!b->next.empty()) todo.push_back(b->next);

            if (op == "data_variable") {
                variable(*b, false);
            } else if (op == "data_setvariableto") {
                variable(*b, true);
            } else if (op == "data_changevariableby") {
                variable(*b, true);
                variable(*b, false);
            } else if (op == "control_for_each") {
                variable(*b, true);
                result.bodyMayYield = true;
            } else if (op == "procedures_call") {
                result.bodyMayYield = true;
                if (!b->mutation) {
                    unknown();
                } else {
                    procedure(b->mutation->attr("proccode"), depth + 1);
                }
            } else if (op == "event_broadcast" ||
                       op == "event_broadcastandwait") {
                result.mayLaunchScripts = true;
                result.bodyMayYield = true;
                result.unknownScalarWrites = true;
            } else if (op == "data_addtolist" ||
                       op == "data_deleteoflist" ||
                       op == "data_insertatlist" ||
                       op == "data_replaceitemoflist" ||
                       op == "data_deletealloflist") {
                result.mayMutateLists = true;
            } else if (op == "data_itemoflist" ||
                       op == "data_listcontainsitem" ||
                       op == "data_lengthoflist" ||
                       op == "data_itemnumoflist" ||
                       op == "data_listcontents") {
                // No scalar mutation; list reads remain visible separately.
            } else if (controlBranch(op)) {
                // Non-warp loops can yield after each completed iteration;
                // even warp loops can yield on their scheduler deadline.
                if (op != "control_if" && op != "control_if_else")
                    result.bodyMayYield = true;
            } else if (yieldControl(op)) {
                result.bodyMayYield = true;
                if (op == "control_create_clone_of") {
                    result.mayLaunchScripts = true;
                    result.unknownScalarWrites = true;
                } else if (op == "control_delete_this_clone") {
                    result.unknownScalarWrites = true;
                }
            } else if (op == "procedures_definition" ||
                       op == "procedures_prototype") {
                // Container/prototype nodes are not executable statements.
            } else if (!knownReporter(op)) {
                // Unknown extension, a host operation or an unsupported
                // mutation is a conservative barrier.
                unknown();
            }

            for (const auto& [name, in] : b->inputs.items) {
                (void)name;
                if (!in.block.empty()) todo.push_back(in.block);
                else if (!in.shadow.empty()) todo.push_back(in.shadow);
            }
        }
    }
};

} // namespace

ProcedureEffects summarizeProcedureEffects(
    Blocks& blocks, Target* target, const std::string& proccode,
    size_t maxBlocks) {
    Analyzer analyzer{blocks, target, {}, maxBlocks};
    analyzer.procedure(proccode, 0);
    // Any call can yield when the warp budget expires before entering the
    // callee. The summary may be used for diagnostics and risk selection,
    // but never for removing guards across a call boundary.
    analyzer.result.callMayYield = true;
    return std::move(analyzer.result);
}

} // namespace xyo::analysis
