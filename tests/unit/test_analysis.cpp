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

    // 解析は Runtime を書き換えない（続けて greenFlag で実行できる）
    rt.greenFlag();
    rt.runUntilDone(10000);
    Target* stage = rt.getTargetForStage();
    CHECK(stage != nullptr);

    std::printf("analysis: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}
