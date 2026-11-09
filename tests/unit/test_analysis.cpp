// xyo-cpp: 構造解析（xyo analyze）のテスト。
//   - ハット/定義/トップレベル reporter の分類
//   - self / total ブロック数（手続き呼出しの先を含むか）
//   - 衝突グラフ（共有変数で直列化、別変数なら別成分）
//   - warp 化候補（ループあり・warp 無し・消えない yield が無い）
//   - ループ内 item # of の検出
//   - 到達不能ブロック / 孤立 shadow の計数
//   - 逐次実行の理由（pen_clear → mainThread）
#include <cstdio>
#include <string>
#include <vector>

#include "analysis/project_analysis.hpp"
#include "analysis/type_sites.hpp"
#include "analysis/type_flow.hpp"
#include "analysis/procedure_effects.hpp"
#include "engine/deps.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "load/zip.hpp"
#include "model/target.hpp"

using namespace xyo;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; \
    if (!((a) == (b))) { ++g_fail; \
    std::printf("FAIL %s:%d: %s == %s (%ld vs %ld)\n", __FILE__, __LINE__, \
                #a, #b, (long)(a), (long)(b)); } } while (0)

namespace {

const char* kProject = R"JSON({
  "targets": [
    {
      "isStage": true, "name": "Stage",
      "variables": {"a": ["shared", 0], "b": ["other", 0]},
      "lists": {"L": ["items", [1, 2, 3]]},
      "broadcasts": {}, "blocks": {}, "comments": {},
      "costumes": [], "sounds": [],
      "currentCostume": 0, "volume": 100, "layerOrder": 0
    },
    {
      "isStage": false, "name": "Sprite1",
      "variables": {}, "lists": {}, "broadcasts": {},
      "blocks": {
        "h1": {"opcode": "event_whenflagclicked", "next": "s1",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 0},
        "s1": {"opcode": "data_setvariableto", "next": null, "parent": "h1",
               "inputs": {}, "fields": {"VARIABLE": ["shared", "a"]},
               "topLevel": false, "shadow": false},
        "h2": {"opcode": "event_whenflagclicked", "next": "s2",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 300},
        "s2": {"opcode": "data_setvariableto", "next": null, "parent": "h2",
               "inputs": {}, "fields": {"VARIABLE": ["shared", "a"]},
               "topLevel": false, "shadow": false},
        "h3": {"opcode": "event_whenflagclicked", "next": "s3",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 600},
        "s3": {"opcode": "data_setvariableto", "next": null, "parent": "h3",
               "inputs": {}, "fields": {"VARIABLE": ["other", "b"]},
               "topLevel": false, "shadow": false},
        "h4": {"opcode": "event_whenflagclicked", "next": "penc",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 900},
        "penc": {"opcode": "pen_clear", "next": null, "parent": "h4",
                 "inputs": {}, "fields": {},
                 "topLevel": false, "shadow": false},

        "d1": {"opcode": "procedures_definition", "next": "r1",
               "parent": null, "inputs": {"custom_block": [1, "p1"]},
               "fields": {}, "topLevel": true, "shadow": false,
               "x": 400, "y": 0},
        "p1": {"opcode": "procedures_prototype", "next": null,
               "parent": "d1", "inputs": {}, "fields": {},
               "topLevel": false, "shadow": true,
               "mutation": {"tagName": "mutation", "children": [],
                            "proccode": "gen", "argumentids": "[]",
                            "argumentnames": "[]", "argumentdefaults": "[]",
                            "warp": "false"}},
        "r1": {"opcode": "control_repeat", "next": null, "parent": "d1",
               "inputs": {"TIMES": [1, [6, "10"]],
                          "SUBSTACK": [2, "loop1"]},
               "fields": {}, "topLevel": false, "shadow": false},
        "loop1": {"opcode": "data_itemoflist", "next": "loop2",
                  "parent": "r1", "inputs": {},
                  "fields": {"LIST": ["items", "L"]},
                  "topLevel": false, "shadow": false},
        "loop2": {"opcode": "data_addtolist", "next": null, "parent": "loop1",
                  "inputs": {}, "fields": {"LIST": ["items", "L"]},
                  "topLevel": false, "shadow": false},

        "dead": {"opcode": "motion_movesteps", "next": null, "parent": null,
                 "inputs": {}, "fields": {},
                 "topLevel": false, "shadow": false},
        "deadshadow": {"opcode": "math_number", "next": null, "parent": null,
                       "inputs": {}, "fields": {"NUM": [4, "7"]},
                       "topLevel": false, "shadow": true}
      },
      "comments": {},
      "costumes": [], "sounds": [],
      "currentCostume": 0, "volume": 100, "layerOrder": 1,
      "visible": true, "x": 0, "y": 0, "size": 100, "direction": 90,
      "draggable": false, "rotationStyle": "all around"
    }
  ],
  "monitors": [], "extensions": ["pen"], "meta": {"semver": "3.0.0"}
})JSON";

std::vector<uint8_t> zipWithProject(const char* json) {
    ZipWriter zw;
    const std::string s = json;
    zw.addFile("project.json", std::vector<uint8_t>(s.begin(), s.end()));
    return zw.finish();
}

const analysis::ScriptInfo* findScript(const analysis::Report& r,
                                       const std::string& label) {
    for (const auto& s : r.scripts)
        if (s.label == label) return &s;
    return nullptr;
}

const analysis::ProcedureInfo* findProc(const analysis::Report& r,
                                        const std::string& proccode) {
    for (const auto& p : r.procedures)
        if (p.proccode == proccode) return &p;
    return nullptr;
}

void testProject(const analysis::Report& r) {
    // --- 分類 ---
    CHECK_EQ(r.executableScripts, 4u);   // h1..h4
    CHECK_EQ(r.definitions, 1u);         // d1
    CHECK_EQ(r.reporterTops, 0u);
    CHECK_EQ(r.scripts.size(), 5u);

    const analysis::ScriptInfo* h1 = findScript(r, "Sprite1:event_whenflagclicked");
    CHECK(h1 != nullptr);
    if (h1) {
        CHECK(h1->kind == analysis::ScriptKind::Hat);
        CHECK_EQ(h1->blocks, 2u);       // hat + set
        CHECK_EQ(h1->selfBlocks, 2u);
        CHECK(h1->access.workerSafe());
        CHECK(!h1->yields);
    }

    const auto* d1 = findScript(r, "Sprite1/gen");
    CHECK(d1 != nullptr);
    if (d1) {
        CHECK(d1->kind == analysis::ScriptKind::Definition);
        CHECK(!d1->warp);
        // hat 自身 + repeat + itemoflist + addtolist = 4（手続き本体は含まない）
        CHECK_EQ(d1->selfBlocks, 4u);
        CHECK_EQ(d1->loops, 1u);
        CHECK_EQ(d1->patterns.itemOfInLoop, 1u);   // repeat の中の item # of
    }

    // --- 到達不能 / 孤立 shadow ---
    CHECK_EQ(r.unreachableBlocks, 1u);   // dead
    CHECK_EQ(r.orphanedShadows, 1u);     // deadshadow

    // --- 並列性: a を共有する h1/h2 が 1 成分、b の h3 が別成分 ---
    CHECK_EQ(r.workerSafe, 3u);          // h4 は mainThread
    CHECK_EQ(r.mainThreadScripts, 1u);
    CHECK_EQ(r.componentSizes.size(), 2u);
    CHECK_EQ(r.maxComponent, 2u);
    if (r.componentSizes.size() == 2) CHECK_EQ(r.componentSizes[1], 1u);

    // --- 衝突キーは共有変数 a ---
    CHECK(!r.conflicts.empty());
    if (!r.conflicts.empty()) {
        CHECK(r.conflicts[0].key.find("shared") != std::string::npos);
        CHECK_EQ(r.conflicts[0].writers, 2u);
        CHECK_EQ(r.conflicts[0].scripts.size(), 2u);
    }

    // --- 手続き: ループあり・warp 無し・消えない yield 無し → warp 化候補 ---
    const auto* gen = findProc(r, "gen");
    CHECK(gen != nullptr);
    if (gen) {
        CHECK(!gen->warp);
        CHECK(gen->hasLoop);
        CHECK(!gen->yields);
    }
    const auto hints = analysis::suggestTransforms(r);
    bool warpHint = false;
    for (const auto& h : hints)
        if (h.kind == analysis::TransformHint::Kind::WarpProcedure) warpHint = true;
    CHECK(warpHint);

    // --- 逐次実行の理由: pen_clear は mainThread ---
    bool penNote = false;
    for (const auto& s : r.scripts)
        for (const auto& n : s.notes)
            if (n.opcode == "pen_clear" &&
                n.kind == AccessNote::Kind::MainThread)
                penNote = true;
    CHECK(penNote);

    // --- テキスト/DOT ---
    const std::string text = analysis::formatText(r);
    CHECK(text.find("warp 化候補") != std::string::npos);
    CHECK(text.find("item # of in loop") != std::string::npos);
    CHECK(text.find("直列化ホットスポット") != std::string::npos);
    const std::string dot = analysis::formatDot(r);
    CHECK(dot.find("digraph deps") != std::string::npos);
    CHECK(dot.find("Sprite1:event_whenflagclicked") != std::string::npos);
}

void testProcedureEffects(Runtime& rt) {
    Blocks blocks(&rt);
    Target* stage = nullptr;
    for (Target* t : rt.targets)
        if (t->isStage) stage = t;
    CHECK(stage != nullptr);
    if (!stage) return;

    auto add = [&](std::string id, std::string opcode,
                   std::string next = {}, bool top = false) -> Block& {
        Block b;
        b.id = std::move(id);
        b.opcode = std::move(opcode);
        b.next = std::move(next);
        b.topLevel = top;
        const std::string key = b.id;
        blocks.createBlock(std::move(b));
        return *blocks.getBlock(key);
    };
    auto define = [&](const std::string& code, const std::string& body) {
        Block& def = add("definition_" + code, "procedures_definition",
                         body, true);
        const std::string prototype = "prototype_" + code;
        def.inputs.set("custom_block", BlockInput{prototype, {}});
        Block& proto = add(prototype, "procedures_prototype");
        proto.mutation.emplace();
        proto.mutation->attrs["proccode"] = code;
    };
    auto call = [&](const std::string& id, const std::string& code,
                    const std::string& next = {}) {
        Block& b = add(id, "procedures_call", next);
        b.mutation.emplace();
        b.mutation->attrs["proccode"] = code;
    };

    define("simple", "simple_set");
    Block& set = add("simple_set", "data_setvariableto");
    set.fields["VARIABLE"].id = "a";
    set.inputs.set("VALUE", BlockInput{"read_b", {}});
    Block& reporter = add("read_b", "data_variable");
    reporter.fields["VARIABLE"].id = "b";

    const auto simple = analysis::summarizeProcedureEffects(
        blocks, stage, "simple");
    CHECK(simple.writtenScalarIds.count("a") == 1);
    CHECK(simple.readScalarIds.count("b") == 1);
    CHECK(!simple.unknownScalarWrites);
    CHECK(!simple.bodyMayYield);
    CHECK(simple.callMayYield); // potential warp-deadline handoff

    define("nested", "nested_call");
    call("nested_call", "simple", "nested_list");
    add("nested_list", "data_addtolist");
    const auto nested = analysis::summarizeProcedureEffects(
        blocks, stage, "nested");
    CHECK(nested.writtenScalarIds.count("a") == 1);
    CHECK(nested.mayMutateLists);
    CHECK(nested.bodyMayYield);
    CHECK(!nested.unknownScalarWrites);

    define("recursive", "recursive_call");
    call("recursive_call", "recursive");
    const auto recursive = analysis::summarizeProcedureEffects(
        blocks, stage, "recursive");
    CHECK(recursive.recursive);
    CHECK(recursive.unknownScalarWrites);
    CHECK(recursive.bodyMayYield);

    define("spawner", "spawn_clone");
    add("spawn_clone", "control_create_clone_of");
    const auto spawner = analysis::summarizeProcedureEffects(
        blocks, stage, "spawner");
    CHECK(spawner.mayLaunchScripts);
    CHECK(spawner.unknownScalarWrites);
    CHECK(spawner.bodyMayYield);

    define("opaque", "opaque_reporter");
    add("opaque_reporter", "operator_extension_mutates");
    const auto opaque = analysis::summarizeProcedureEffects(
        blocks, stage, "opaque");
    CHECK(opaque.unknownScalarWrites);
    CHECK(opaque.bodyMayYield);

    const auto truncated = analysis::summarizeProcedureEffects(
        blocks, stage, "nested", 1);
    CHECK(truncated.truncated);
    CHECK(truncated.unknownScalarWrites);
}

void testControlFlowAndTypeFixedPoint(Runtime& rt) {
    Blocks bs(&rt);
    auto put = [&](std::string id, std::string opcode,
                   std::string next = {}) -> Block& {
        Block block;
        block.id = id;
        block.opcode = opcode;
        block.next = std::move(next);
        bs.createBlock(std::move(block));
        return *bs.getBlock(id);
    };
    auto assign = [&](const std::string& id,
                      const std::string& next,
                      const std::string& variable,
                      const std::string& reporter) {
        Block& b = put(id, "data_setvariableto", next);
        b.fields["VARIABLE"].id = variable;
        b.inputs.set("VALUE", BlockInput{reporter, {}});
    };
    Block& number = put("num", "math_number");
    number.shadow = true;
    number.fields["NUM"].value = Value::num(1);
    Block& string = put("str", "text");
    string.shadow = true;
    string.fields["TEXT"].value = Value::string(SStr::create(u"x"));
    Block& variableReporter = put("get_v", "data_variable");
    variableReporter.fields["VARIABLE"].id = "v";

    assign("set_v", "branch", "v", "num");
    Block& branch = put("branch", "control_if_else", "copy_v");
    branch.inputs.set("SUBSTACK", BlockInput{"then_v", {}});
    branch.inputs.set("SUBSTACK2", BlockInput{"else_v", {}});
    assign("then_v", {}, "v", "str");
    assign("else_v", {}, "v", "num");
    assign("copy_v", "repeat", "w", "get_v");
    Block& repeat = put("repeat", "control_repeat", "after_loop");
    repeat.inputs.set("SUBSTACK", BlockInput{"loop_set", {}});
    repeat.inputs.set("TIMES", BlockInput{"num", {}});
    assign("loop_set", {}, "v", "num");
    assign("after_loop", "opaque", "w", "get_v");
    put("opaque", "procedures_call");

    const auto result = analysis::analyzeTypeFlow(bs, "set_v");
    const auto node = [&](const char* id) {
        const auto& matches = result.cfg.blockNodes.at(id);
        CHECK_EQ(matches.size(), 1u);
        return matches.front();
    };
    const size_t first = node("set_v");
    const size_t condition = node("branch");
    const size_t thenNode = node("then_v");
    const size_t elseNode = node("else_v");
    const size_t copy = node("copy_v");
    const size_t loop = node("repeat");
    const size_t afterLoop = node("after_loop");
    const size_t opaque = node("opaque");
    constexpr uint32_t N = Variable::kObservedNumber;
    constexpr uint32_t S = Variable::kObservedString;
    constexpr uint32_t Any = N | S | Variable::kObservedBoolean |
        Variable::kObservedColor | Variable::kObservedOther;

    CHECK(!result.cfg.truncated);
    CHECK_EQ(result.kindAfter(first, "v"), N);
    CHECK_EQ(result.kindBefore(condition, "v"), N);
    CHECK_EQ(result.kindAfter(thenNode, "v"), S);
    CHECK_EQ(result.kindAfter(elseNode, "v"), N);
    CHECK_EQ(result.kindBefore(copy, "v"), N | S);
    CHECK_EQ(result.kindAfter(copy, "w"), N | S);
    // A loop backedge can run a different Scratch thread and change v.
    CHECK_EQ(result.kindBefore(loop, "v"), Any);
    CHECK_EQ(result.kindBefore(afterLoop, "v"), Any);
    CHECK_EQ(result.kindAfter(opaque, "w"), Any);

    CHECK(result.cfg.dominates(first, copy));
    CHECK(result.cfg.dominates(condition, thenNode));
    CHECK(!result.cfg.dominates(thenNode, copy));
    CHECK(!result.cfg.dominates(elseNode, copy));
    CHECK(result.cfg.dominates(result.cfg.entry, result.cfg.exit));

    // For-each implicitly assigns its counter at iteration entry. Keeping
    // the previous String type would be unsound inside the loop body.
    assign("foreach_preset", "foreach", "v", "str");
    Block& foreach = put("foreach", "control_for_each", "foreach_end");
    foreach.fields["VARIABLE"].id = "v";
    foreach.inputs.set("VALUE", BlockInput{"num", {}});
    foreach.inputs.set("SUBSTACK", BlockInput{"foreach_body", {}});
    assign("foreach_body", {}, "w", "get_v");
    assign("foreach_end", {}, "w", "get_v");
    const auto foreachFlow = analysis::analyzeTypeFlow(bs, "foreach_preset");
    const size_t foreachHeader =
        foreachFlow.cfg.blockNodes.at("foreach").front();
    const size_t foreachBody =
        foreachFlow.cfg.blockNodes.at("foreach_body").front();
    const size_t foreachEnd =
        foreachFlow.cfg.blockNodes.at("foreach_end").front();
    CHECK_EQ(foreachFlow.kindAfter(
                 foreachFlow.cfg.blockNodes.at("foreach_preset").front(),
                 "v"), S);
    // The loop header includes an unknown backedge after a scheduler handoff.
    CHECK_EQ(foreachFlow.kindBefore(foreachHeader, "v"), Any);
    CHECK_EQ(foreachFlow.kindBefore(foreachBody, "v"), Any);
    CHECK_EQ(foreachFlow.kindBefore(foreachEnd, "v"), Any);

    // Unknown/malformed input must invalidate rather than invent a type.
    auto truncated = analysis::analyzeTypeFlow(bs, "set_v", 3);
    CHECK(truncated.cfg.truncated);
    CHECK_EQ(truncated.kindBefore(truncated.cfg.exit, "v"), Any);

    // Opcode appearance does not determine storage type. Scratch's JSON
    // numeric primitive can carry a string ("7"), a NaN Value, etc.
    auto literalKind = [&](const std::string& id, const std::string& op,
                           Value value) {
        Block& block = put(id, op);
        block.shadow = true;
        block.fields["LITERAL"].value = value;
        BlockInput input{id, {}};
        return analysis::inferInputStorageKinds(bs, &input);
    };
    CHECK_EQ(literalKind("numeric_str", "math_number",
                         Value::string(SStr::create(u"7"))), S);
    CHECK_EQ(literalKind("literal_nan", "math_number", Value::nan()), N);
    CHECK_EQ(literalKind("literal_bool", "math_boolean",
                         Value::boolean(true)), Variable::kObservedBoolean);
    CHECK_EQ(literalKind("literal_color", "colour_picker",
                         Value::color(1, 2, 3)), Variable::kObservedColor);
    CHECK_EQ(literalKind("literal_undefined", "text",
                         Value::undefined()), Variable::kObservedOther);

    // A legacy Scratch block may have a missing ID but a matching variable
    // name. Without canonicalization, the analysis can incorrectly keep the
    // older type of the same variable under its canonical ID.
    Target* stage = nullptr;
    for (Target* t : rt.targets)
        if (t->isStage) stage = t;
    CHECK(stage != nullptr);
    if (stage) {
        Block& aliasReporter = put("alias_get", "data_variable");
        aliasReporter.fields["VARIABLE"].id = "a";
        assign("alias_set_first", "alias_set_name", "a", "num");
        Block& aliasWrite = put("alias_set_name", "data_setvariableto",
                                "alias_copy");
        aliasWrite.fields["VARIABLE"].id = "stale_id";
        aliasWrite.fields["VARIABLE"].value =
            Value::string(SStr::create(u"shared"));
        aliasWrite.inputs.set("VALUE", BlockInput{"str", {}});
        assign("alias_copy", "alias_collision", "b", "alias_get");
        Block& collision = put("alias_collision", "data_setvariableto",
                               "alias_bad");
        collision.fields["VARIABLE"].id = "L";  // list ID, not scalar
        collision.fields["VARIABLE"].value =
            Value::string(SStr::create(u"shared"));  // scalar name alias
        collision.inputs.set("VALUE", BlockInput{"str", {}});
        Block& invalid = put("alias_bad", "data_setvariableto",
                             "alias_after");
        invalid.fields["VARIABLE"].id = "unresolved";
        invalid.fields["VARIABLE"].value =
            Value::string(SStr::create(u"nonexistent"));
        invalid.inputs.set("VALUE", BlockInput{"num", {}});
        assign("alias_after", {}, "b", "alias_get");

        const auto aliased = analysis::analyzeTypeFlow(
            bs, "alias_set_first", 16384, stage);
        const auto a = [&](const char* id) {
            return aliased.cfg.blockNodes.at(id).front();
        };
        CHECK_EQ(aliased.kindAfter(a("alias_set_first"), "a"), N);
        CHECK_EQ(aliased.kindAfter(a("alias_set_name"), "a"), S);
        CHECK_EQ(aliased.kindAfter(a("alias_copy"), "b"), S);
        // The stage list "L" must not resolve to scalar "shared", even
        // though the field's name would otherwise match.
        CHECK_EQ(aliased.kindBefore(a("alias_collision"), "a"), S);
        CHECK_EQ(aliased.kindAfter(a("alias_collision"), "a"), Any);
        // Unresolvable/unknown writes must invalidate all known kinds.
        CHECK_EQ(aliased.kindAfter(a("alias_bad"), "a"), Any);
        CHECK_EQ(aliased.kindAfter(a("alias_after"), "b"), Any);

        // An extension can register an opcode with an operator_ prefix;
        // that prefix alone must never grant "no side effects" status.
        put("unknown_operator", "operator_custom_mutates");
        assign("effect_numeric", "effect_unknown", "a", "num");
        assign("effect_unknown", {}, "b", "unknown_operator");
        const auto effects = analysis::analyzeTypeFlow(
            bs, "effect_numeric", 16384, stage);
        const auto root = effects.cfg.blockNodes.at("effect_numeric").front();
        const auto unknown = effects.cfg.blockNodes.at("effect_unknown").front();
        CHECK_EQ(effects.kindAfter(root, "a"), N);
        CHECK_EQ(effects.kindAfter(unknown, "a"), Any);
    }
}

}  // namespace

int test_analysis_main() {
    const std::vector<uint8_t> sb3 = zipWithProject(kProject);
    Runtime rt;
    Sb3LoadResult lr = loadSb3File(rt, sb3.data(), sb3.size());
    CHECK(lr.ok);
    if (!lr.ok) {
        std::printf("  load error: %s\n", lr.error.c_str());
        std::printf("analysis: %d checks, %d failures\n", g_checks, g_fail);
        return g_fail;
    }
    const analysis::Report r = analysis::analyzeProject(rt, "test.sb3");
    testProject(r);
    testControlFlowAndTypeFixedPoint(rt);
    testProcedureEffects(rt);
    // The shared static kind analysis supplies the loop-promotion gate.
    const analysis::TypeSiteReport sites = analysis::analyzeTypeSites(rt);
    CHECK_EQ(sites.sites.size(), 3u);
    CHECK_EQ(sites.unknownSites, 0u);
    CHECK(sites.sites[0].kinds != 0u);
    const std::string typeText = analysis::formatTypeSites(sites);
    CHECK(typeText.find("write sites 3") != std::string::npos);

    // 解析は Runtime を書き換えない（続けて greenFlag で実行できる）
    rt.greenFlag();
    rt.runUntilDone(10000);
    Target* stage = rt.getTargetForStage();
    CHECK(stage != nullptr);

    std::printf("analysis: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}
