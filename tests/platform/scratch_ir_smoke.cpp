// Whole-script structural IR + WASM adapter. All Scratch opcodes are
// preserved, including unsupported effects and detached procedures.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/scratch_script.hpp"
#include "../../src/ir/scratch_effect.hpp"
#include "../../src/ir/scratch_pipeline.hpp"
#include "../../src/ir/scratch_numeric_kernel.hpp"
#include "../../src/ir/scratch_f64_kernel.hpp"
#include "../../src/wasm/wasm_compile.hpp"
#include "../../src/load/sb3.hpp"
#include "../../src/engine/runtime.hpp"
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace xyo;

static void make(Blocks& blocks, const char* id, const char* opcode,
                 const char* next = "") {
    Block b;
    b.id = id;
    b.opcode = opcode;
    b.next = next;
    blocks.createBlock(std::move(b));
}

int main(int argc, char** argv) {
    if (argc == 2) {
        std::ifstream file(argv[1], std::ios::binary);
        assert(file);
        std::vector<uint8_t> bytes(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        assert(!bytes.empty());
        Runtime runtime;
        const auto loaded = loadSb3File(runtime, bytes.data(), bytes.size());
        assert(loaded.ok);
        size_t scripts = 0, blocksTotal = 0;
        for (Target* target : loaded.targets) {
            assert(target && target->blocks);
            auto& source = *target->blocks;
            const auto project = ir::ScratchProject::capture(source);
            assert(project.error == ir::ScratchIrError::None);
            assert(project.nodes.size() == source.size());
            blocksTotal += project.nodes.size();
            for (const auto& root : project.scriptIds) {
                const auto snapshot = ir::ScratchScript::capture(
                    source, source.getBlock(root));
                assert(snapshot.valid());
                assert(snapshot.getBlock(root));
                const auto effect = ir::ScratchEffects::lower(snapshot);
                assert(effect.valid);
                assert(effect.nodes.size() == snapshot.nodes.size());
                // The strict frontend must safely reject unsupported actual
                // SB3 scripts; a successful lowering must always verify.
                if (auto numeric = ir::lowerNumericF64Script(snapshot))
                    assert(ir::verifyExecutable(numeric->executable));
                ++scripts;
            }
        }
        assert(blocksTotal > 0 && scripts > 0);
        return 0;
    }
    assert(argc == 1);
    Blocks blocks(nullptr);
    make(blocks, "hat", "event_whenflagclicked", "set");
    make(blocks, "set", "data_setvariableto", "opaque");
    make(blocks, "opaque", "looks_say");
    make(blocks, "reporter", "operator_add");
    make(blocks, "left", "math_number");
    make(blocks, "right", "math_number");
    make(blocks, "fallback", "math_number");
    make(blocks, "proc", "procedures_definition");
    make(blocks, "prototype", "procedures_prototype");
    make(blocks, "unknown", "external_extension_unrecognized");
    blocks.getBlock("hat")->topLevel = true;
    blocks.addScript("hat");
    blocks.getBlock("set")->fields["VARIABLE"].id = "variable-id";
    blocks.getBlock("set")->inputs.set("VALUE", {"reporter", "fallback"});
    blocks.getBlock("reporter")->inputs.set("NUM1", {"left", ""});
    blocks.getBlock("reporter")->inputs.set("NUM2", {"right", ""});
    blocks.getBlock("left")->fields["NUM"].value = Value::num(10);
    blocks.getBlock("right")->fields["NUM"].value = Value::num(32);
    blocks.getBlock("fallback")->fields["NUM"].value = Value::num(41);
    blocks.getBlock("proc")->inputs.set("custom_block", {"prototype", ""});
    blocks.getBlock("prototype")->mutation.emplace();
    blocks.getBlock("prototype")->mutation->attrs["proccode"] = "f %s";
    blocks.getBlock("unknown")->fields["K"].id = "opaque-id";

    auto captured = ir::ScratchScript::capture(blocks, blocks.getBlock("hat"));
    assert(captured.valid());
    assert(captured.nodes.size() == 7);
    assert(captured.getBlock("fallback"));
    assert(captured.getBlock("opaque")->opcode == "looks_say");
    assert(captured.getBlock("reporter")->inputs.items[0].first == "NUM1");
    assert(captured.getBlock("reporter")->inputs.items[1].first == "NUM2");

    auto project = ir::ScratchProject::capture(blocks);
    assert(project.error == ir::ScratchIrError::None);
    assert(project.nodes.size() == 10);
    assert(project.getBlock("prototype")->mutation->attr("proccode") == "f %s");
    assert(project.getBlock("unknown")->opcode ==
           "external_extension_unrecognized");

    // After modifying the live editor graph, captured IR is still stable.
    blocks.getBlock("reporter")->opcode = "operator_subtract";
    blocks.getBlock("left")->fields["NUM"].value = Value::num(500);
    blocks.getBlock("set")->next.clear();
    assert(captured.getBlock("reporter")->opcode == "operator_add");
    assert(captured.getBlock("left")->fields.at("NUM").value.asNumberUnchecked()
           == 10);
    assert(captured.getBlock("set")->next == "opaque");

    const auto effects = ir::ScratchEffects::lower(captured);
    assert(effects.valid && effects.nodes.size() == captured.nodes.size());
    const auto* setEffect = effects.getBlock("set");
    assert(setEffect && setEffect->kind ==
           ir::ScratchNodeKind::WriteVariable);
    assert(setEffect->variableId == "variable-id");
    assert(ir::hasEffect(setEffect->effects,
                         ir::ScratchEffect::WriteState));
    assert(ir::hasEffect(setEffect->effects,
                         ir::ScratchEffect::MayAllocate));
    const auto* input = effects.inputOf("set", "VALUE");
    assert(input && input->activeId == "reporter" &&
           input->shadowId == "fallback");
    assert(input->selectedId() == "reporter");
    assert(effects.getBlock("reporter")->kind ==
           ir::ScratchNodeKind::Arithmetic);
    assert(effects.getBlock("opaque")->opaque);
    assert(ir::hasEffect(effects.getBlock("opaque")->effects,
                         ir::ScratchEffect::MayYield));
    assert(ir::hasEffect(effects.getBlock("opaque")->effects,
                         ir::ScratchEffect::Unknown));
    assert(effects.getBlock("hat")->kind == ir::ScratchNodeKind::Hat);
    assert(!ir::hasEffect(effects.subtreeEffects("reporter"),
                          ir::ScratchEffect::Unknown));
    assert(ir::hasEffect(effects.subtreeEffects("set"),
                         ir::ScratchEffect::WriteState));
    assert(!ir::hasEffect(effects.subtreeEffects("set"),
                          ir::ScratchEffect::Unknown));
    assert(ir::hasEffect(effects.subtreeEffects("hat", true),
                         ir::ScratchEffect::Unknown));
    assert(ir::hasEffect(effects.subtreeEffects("missing"),
                         ir::ScratchEffect::Unknown));

    assert(!ir::lowerNumericF64Script(captured));
    wasm::Program rejected;
    assert(!wasm::lowerCapturedScript(captured, rejected));
    assert(rejected.exprs.empty() && rejected.main.empty());

    // All of these unknown/externally observable opcodes must have BOTH
    // transfer sites and must fail-closed in the speculative WASM frontend.
    // Test the frontend API directly, not just output parity from a
    // project whose unsupported opcode happened to be a no-op.
    for (const char* opcode : {
            "extension_private_unknown_side_effect",
            "extension_private_unknown_reporter",
            "looks_say", "looks_think", "looks_sayforsecs",
            "pen_stamp", "event_broadcast",
            "event_broadcastandwait", "sound_playuntildone",
            "control_create_clone_of"}) {
        Blocks controls(nullptr);
        make(controls, "opaque-hat", "event_whenflagclicked", "opaque-node");
        make(controls, "opaque-node", opcode, "after-node");
        make(controls, "after-node", "data_changevariableby");
        auto isolated = ir::ScratchCompilationUnit::capture(
            controls, controls.getBlock("opaque-hat"));
        assert(isolated.valid());
        const auto* node = isolated.effects.getBlock("opaque-node");
        assert(node && node->opaque);
        assert(ir::hasEffect(node->effects, ir::ScratchEffect::Unknown));
        assert(ir::hasEffect(node->effects, ir::ScratchEffect::MayYield));
        assert(!ir::mayReplaySpeculatively(node->effects));
        int effectSites = 0, yieldSites = 0;
        for (const auto& site : isolated.transfers.sites) {
            if (site.sourceBlockId != "opaque-node") continue;
            if (site.kind == ir::ScratchTransferKind::Effect) ++effectSites;
            if (site.kind == ir::ScratchTransferKind::Yield) ++yieldSites;
        }
        assert(effectSites == 1 && yieldSites == 1);
        wasm::Program untouched;
        assert(!wasm::lowerCapturedScript(isolated.script, untouched));
        assert(untouched.main.empty() && untouched.exprs.empty());
        assert(!wasm::lowerCapturedStatement(
            isolated.script, isolated.effects, "opaque-node", untouched));
        assert(untouched.main.empty() && untouched.exprs.empty());
        assert(!wasm::lowerCapturedRange(
            isolated.script, isolated.effects,
            "opaque-node", "after-node", untouched));
        assert(untouched.main.empty() && untouched.exprs.empty());
    }

    // A second snapshot without the unsupported effect can be compiled
    // even after the original editor graph is changed/deleted.
    auto supported = ir::ScratchScript::capture(blocks, blocks.getBlock("hat"));
    assert(supported.valid());
    wasm::Program output;
    assert(wasm::lowerCapturedScript(supported, output));
    assert(output.slotIds.size() == 1 &&
           output.slotIds[0] == "variable-id");
    assert(output.main.size() == 1);
    assert(!wasm::compileProgram(output).empty());
    // Also run the exact-integer executable frontend on a captured, genuine
    // Blocks-based Scratch source graph, independently of WASM's Program.
    const auto integerKernel = ir::lowerExactIntegerScript(supported);
    assert(integerKernel);
    assert(integerKernel->resultVariable == "variable-id");
    assert(integerKernel->slotIds.size() == 1);
    assert(ir::verifyExecutable(integerKernel->executable));
    const auto decimalKernel = ir::lowerNumericF64Script(supported);
    assert(decimalKernel);
    assert(decimalKernel->executable.externalValueSlots);
    assert(decimalKernel->resultVariable == "variable-id");
    assert(ir::verifyExecutable(decimalKernel->executable));
    const auto supportedEffects = ir::ScratchEffects::lower(supported);
    assert(supportedEffects.valid);
    assert(supportedEffects.inputOf("set", "VALUE")->selectedId() ==
           "reporter");

    // Both native and WASM frontends consume the same lossless source unit.
    const auto sharedUnit = ir::ScratchCompilationUnit::capture(
        blocks, blocks.getBlock("hat"));
    assert(sharedUnit.valid());
    assert(sharedUnit.script.nodes.size() == supported.nodes.size());
    assert(sharedUnit.effects.nodes.size() == sharedUnit.script.nodes.size());
    assert(sharedUnit.effects.inputOf("set", "VALUE")->selectedId()
           == "reporter");
    wasm::Program viaSharedFrontend;
    assert(wasm::lowerCapturedScript(sharedUnit.script, viaSharedFrontend));

    wasm::Program viaPublicFrontend;
    assert(wasm::lowerScript(blocks, blocks.getBlock("hat"),
                             viaPublicFrontend));
    assert(viaSharedFrontend.slotIds == output.slotIds);
    assert(wasm::compileProgram(viaSharedFrontend) == wasm::compileProgram(output));
    assert(viaPublicFrontend.slotIds == output.slotIds);
    assert(viaPublicFrontend.main.size() == output.main.size());
    assert(wasm::compileProgram(viaPublicFrontend) ==
           wasm::compileProgram(output));

    // A single source graph must contain every call, return point, yield
    // and effect, with the SAME stable IDs in LLVM and Binaryen. In
    // particular the return continuation comes from the original call's
    // next edge, not a mutable runtime stack or a backend-specific guess.
    {
        Blocks controls(nullptr);
        make(controls, "entry", "event_whenflagclicked", "invoke");
        make(controls, "invoke", "procedures_call", "delay");
        make(controls, "delay", "control_wait", "effect");
        make(controls, "effect", "looks_say");
        make(controls, "definition", "procedures_definition", "body");
        make(controls, "prototype", "procedures_prototype");
        make(controls, "body", "data_changevariableby");
        controls.getBlock("invoke")->mutation.emplace();
        controls.getBlock("invoke")->mutation->attrs["proccode"] = "sample";
        controls.getBlock("definition")->inputs.set(
            "custom_block", {"prototype", ""});
        controls.getBlock("prototype")->mutation.emplace();
        controls.getBlock("prototype")->mutation->attrs["proccode"] =
            "sample";
        auto unit = ir::ScratchCompilationUnit::capture(
            controls, controls.getBlock("entry"));
        assert(unit.valid());
        assert(unit.procedureDefinitions.at("sample") == "definition");
        assert(unit.script.getBlock("definition"));
        assert(unit.script.getBlock("body"));
        int calls = 0, entries = 0, returns = 0, yields = 0, effects = 0;
        for (const auto& site : unit.transfers.sites) {
            assert(unit.script.getBlock(site.sourceBlockId));
            switch (site.kind) {
            case ir::ScratchTransferKind::Call:
                ++calls;
                assert(site.sourceBlockId == "invoke");
                assert(site.continuationBlockId == "delay");
                assert(site.procedureDefinitionId == "definition");
                break;
            case ir::ScratchTransferKind::ProcedureEntry:
                ++entries;
                assert(site.procedureDefinitionId == "definition");
                break;
            case ir::ScratchTransferKind::Return:
                ++returns;
                assert(site.procedureDefinitionId == "definition");
                break;
            case ir::ScratchTransferKind::Yield:
                ++yields;
                // A known scheduler wait plus a conservative unknown
                // looks-effect that might yield. Each keeps its own
                // source ID and both have a corresponding resume PC.
                assert(site.sourceBlockId == "delay" ||
                       site.sourceBlockId == "effect");
                assert(ir::hasEffect(site.effects, ir::ScratchEffect::MayYield));
                break;
            case ir::ScratchTransferKind::Effect:
                ++effects;
                assert(site.sourceBlockId == "effect");
                assert(ir::hasEffect(site.effects, ir::ScratchEffect::Unknown));
                break;
            }
        }
        assert(calls == 1 && entries == 1 && returns == 1 &&
               yields == 2 && effects == 1);
        // The opaque looks node carries *both* an externally visible
        // effect barrier and a may-yield boundary. Dropping either while
        // lowering into a new backend is a correctness bug.
        bool effectHasYield = false, effectHasObservable = false;
        for (const auto& site : unit.transfers.sites) {
            if (site.sourceBlockId != "effect") continue;
            effectHasYield |= site.kind == ir::ScratchTransferKind::Yield;
            effectHasObservable |= site.kind == ir::ScratchTransferKind::Effect;
        }
        assert(effectHasYield && effectHasObservable);
        assert(unit.transfers.validate(unit.script));
        assert(unit.transfers.requiresRuntime());

        // Captured source remains valid after live blocks change; a broken
        // call or a made-up return PC must not accidentally be compiled.
        controls.getBlock("invoke")->next.clear();
        controls.getBlock("prototype")->mutation->attrs["proccode"] =
            "another";
        assert(unit.transfers.validate(unit.script));
        auto corrupted = unit.transfers;
        for (auto& site : corrupted.sites)
            if (site.kind == ir::ScratchTransferKind::Call)
                site.procedureDefinitionId = "effect";
        assert(!corrupted.validate(unit.script));
        controls.getBlock("invoke")->mutation->attrs["proccode"] =
            "missing";
        const auto unresolved = ir::ScratchCompilationUnit::capture(
            controls, controls.getBlock("entry"));
        assert(unresolved.valid());
        bool foundUnresolved = false;
        for (const auto& site : unresolved.transfers.sites)
            if (site.kind == ir::ScratchTransferKind::Call &&
                site.sourceBlockId == "invoke" &&
                site.procedureDefinitionId.empty())
                foundUnresolved = true;
        assert(foundUnresolved);
        assert(!ir::lowerNumericF64Script(unresolved));
    }

    // A long script is captured with an iterative walk rather than
    // compiler recursion (and no 512-depth WASM restriction is applied).
    Blocks longScript(nullptr);
    constexpr int kLongLength = 10000;
    for (int i = 0; i < kLongLength; ++i) {
        const std::string id = "node-" + std::to_string(i);
        const std::string next = i + 1 == kLongLength
            ? "" : "node-" + std::to_string(i + 1);
        make(longScript, id.c_str(), "extension_opaque", next.c_str());
    }
    const auto longCaptured = ir::ScratchScript::capture(
        longScript, longScript.getBlock("node-0"));
    assert(longCaptured.valid() && longCaptured.nodes.size() == kLongLength);
    assert(longCaptured.getBlock("node-9999"));

    // Detect actual cycles, not repeated references to shared reporters.
    blocks.getBlock("left")->inputs.set("LOOP", {"reporter", ""});
    const auto cyclic = ir::ScratchScript::capture(blocks, blocks.getBlock("hat"));
    assert(cyclic.error == ir::ScratchIrError::CyclicGraph);

    // Broken references fail safely before the backend sees an invalid IR.
    blocks.getBlock("left")->inputs.erase("LOOP");
    blocks.getBlock("set")->next = "missing";
    const auto missing = ir::ScratchScript::capture(blocks, blocks.getBlock("hat"));
    assert(missing.error == ir::ScratchIrError::MissingBlock);

    return 0;
}
