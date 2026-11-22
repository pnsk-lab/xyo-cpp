// xyo-cpp: 計算量見積り（xyo analyze --complexity）のテスト。
//   - 定数ネストは exact ステップ数 + O(1)
//   - 変数回数は記号パラメータ N
//   - until/forever は U/∞
//   - リスト走査は |L| 因子
//   - 再帰は打切りフラグ
#include <cstdio>
#include <string>
#include <vector>

#include "analysis/complexity.hpp"
#include "analysis/project_analysis.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "load/zip.hpp"

using namespace xyo;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// repeat 10 { repeat 5 { set X } } ほか、bound 種別ごとのスクリプト集。
const char* kProject = R"JSON({
  "targets": [
    {
      "isStage": true, "name": "Stage",
      "variables": {}, "lists": {}, "broadcasts": {},
      "blocks": {}, "comments": {},
      "costumes": [], "sounds": [],
      "currentCostume": 0, "volume": 100, "layerOrder": 0
    },
    {
      "isStage": false, "name": "Sprite1",
      "variables": {"n1": ["N", 0], "x1": ["X", 0], "i1": ["I", 0]},
      "lists": {"L": ["items", [1, 2, 3]]},
      "broadcasts": {},
      "blocks": {
        "h_const": {"opcode": "event_whenflagclicked", "next": "rc1",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 0},
        "rc1": {"opcode": "control_repeat", "next": null, "parent": "h_const",
               "inputs": {"TIMES": [1, [6, "10"]], "SUBSTACK": [2, "rc2"]},
               "fields": {}, "topLevel": false, "shadow": false},
        "rc2": {"opcode": "control_repeat", "next": null, "parent": "rc1",
               "inputs": {"TIMES": [1, [5, "5"]], "SUBSTACK": [2, "sc1"]},
               "fields": {}, "topLevel": false, "shadow": false},
        "sc1": {"opcode": "data_setvariableto", "next": null, "parent": "rc2",
               "inputs": {}, "fields": {"VARIABLE": ["X", "x1"]},
               "topLevel": false, "shadow": false},

        "h_var": {"opcode": "event_whenkeypressed", "next": "rv1",
               "parent": null,
               "inputs": {}, "fields": {"KEY_OPTION": ["space", null]},
               "topLevel": true, "shadow": false, "x": 0, "y": 200},
        "rv1": {"opcode": "control_repeat", "next": null, "parent": "h_var",
               "inputs": {"TIMES": [2, "vv1"], "SUBSTACK": [2, "sv1"]},
               "fields": {}, "topLevel": false, "shadow": false},
        "vv1": {"opcode": "data_variable", "next": null, "parent": "rv1",
               "inputs": {}, "fields": {"VARIABLE": ["N", "n1"]},
               "topLevel": false, "shadow": false},
        "sv1": {"opcode": "data_setvariableto", "next": null, "parent": "rv1",
               "inputs": {}, "fields": {"VARIABLE": ["X", "x1"]},
               "topLevel": false, "shadow": false},

        "h_until": {"opcode": "event_whenbroadcastreceived", "next": "ru1",
               "parent": null,
               "inputs": {}, "fields": {"BROADCAST_OPTION": ["go", "b1"]},
               "topLevel": true, "shadow": false, "x": 0, "y": 400},
        "ru1": {"opcode": "control_repeat_until", "next": null, "parent": "h_until",
               "inputs": {"SUBSTACK": [2, "su1"]},
               "fields": {}, "topLevel": false, "shadow": false},
        "su1": {"opcode": "data_setvariableto", "next": null, "parent": "ru1",
               "inputs": {}, "fields": {"VARIABLE": ["X", "x1"]},
               "topLevel": false, "shadow": false},

        "h_forev": {"opcode": "control_start_as_clone", "next": "rf1",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 600},
        "rf1": {"opcode": "control_forever", "next": null, "parent": "h_forev",
               "inputs": {"SUBSTACK": [2, "sf1"]},
               "fields": {}, "topLevel": false, "shadow": false},
        "sf1": {"opcode": "data_setvariableto", "next": null, "parent": "rf1",
               "inputs": {}, "fields": {"VARIABLE": ["X", "x1"]},
               "topLevel": false, "shadow": false},

        "h_scan": {"opcode": "event_whenthisspriteclicked", "next": "rs1",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 800},
        "rs1": {"opcode": "control_repeat", "next": null, "parent": "h_scan",
               "inputs": {"TIMES": [1, [6, "10"]], "SUBSTACK": [2, "q1"]},
               "fields": {}, "topLevel": false, "shadow": false},
        "q1": {"opcode": "data_itemnumoflist", "next": "sq1", "parent": "rs1",
              "inputs": {}, "fields": {"LIST": ["items", "L"]},
              "topLevel": false, "shadow": false},
        "sq1": {"opcode": "data_setvariableto", "next": null, "parent": "q1",
               "inputs": {}, "fields": {"VARIABLE": ["X", "x1"]},
               "topLevel": false, "shadow": false},

        "h_rec": {"opcode": "event_whenflagclicked", "next": "call_top",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 400, "y": 0},
        "call_top": {"opcode": "procedures_call", "next": null, "parent": "h_rec",
               "inputs": {}, "fields": {},
               "topLevel": false, "shadow": false,
               "mutation": {"tagName": "mutation", "children": [],
                            "proccode": "rec", "argumentids": "[]",
                            "argumentnames": "[]", "argumentdefaults": "[]",
                            "warp": "false"}},
        "d_rec": {"opcode": "procedures_definition", "next": "call_body",
               "parent": null, "inputs": {"custom_block": [1, "p_rec"]},
               "fields": {}, "topLevel": true, "shadow": false,
               "x": 400, "y": 200},
        "p_rec": {"opcode": "procedures_prototype", "next": null,
               "parent": "d_rec", "inputs": {}, "fields": {},
               "topLevel": false, "shadow": true,
               "mutation": {"tagName": "mutation", "children": [],
                            "proccode": "rec", "argumentids": "[]",
                            "argumentnames": "[]", "argumentdefaults": "[]",
                            "warp": "false"}},
        "call_body": {"opcode": "procedures_call", "next": null, "parent": "d_rec",
               "inputs": {}, "fields": {},
               "topLevel": false, "shadow": false,
               "mutation": {"tagName": "mutation", "children": [],
                            "proccode": "rec", "argumentids": "[]",
                            "argumentnames": "[]", "argumentdefaults": "[]",
                            "warp": "false"}},

        "h_expr": {"opcode": "event_whenflagclicked", "next": "re1",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 1000},
        "re1": {"opcode": "control_repeat", "next": null, "parent": "h_expr",
               "inputs": {"TIMES": [2, "add1"], "SUBSTACK": [2, "se1"]},
               "fields": {}, "topLevel": false, "shadow": false},
        "add1": {"opcode": "operator_add", "next": null, "parent": "re1",
               "inputs": {"NUM1": [2, "vv_expr"], "NUM2": [1, [6, "1"]]},
               "fields": {}, "topLevel": false, "shadow": false},
        "vv_expr": {"opcode": "data_variable", "next": null, "parent": "add1",
               "inputs": {}, "fields": {"VARIABLE": ["N", "n1"]},
               "topLevel": false, "shadow": false},
        "se1": {"opcode": "data_setvariableto", "next": null, "parent": "re1",
               "inputs": {}, "fields": {"VARIABLE": ["X", "x1"]},
               "topLevel": false, "shadow": false},

        "h_rank": {"opcode": "event_whenflagclicked", "next": "ru_rank",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 1200},
        "ru_rank": {"opcode": "control_repeat_until", "next": null, "parent": "h_rank",
               "inputs": {"CONDITION": [2, "gt1"], "SUBSTACK": [2, "ch1"]},
               "fields": {}, "topLevel": false, "shadow": false},
        "gt1": {"opcode": "operator_gt", "next": null, "parent": "ru_rank",
               "inputs": {"OPERAND1": [2, "vi1"], "OPERAND2": [1, [6, "10"]]},
               "fields": {}, "topLevel": false, "shadow": false},
        "vi1": {"opcode": "data_variable", "next": null, "parent": "gt1",
               "inputs": {}, "fields": {"VARIABLE": ["I", "i1"]},
               "topLevel": false, "shadow": false},
        "ch1": {"opcode": "data_changevariableby", "next": null, "parent": "ru_rank",
               "inputs": {"VALUE": [1, [6, "1"]]},
               "fields": {"VARIABLE": ["I", "i1"]},
               "topLevel": false, "shadow": false},

        "h_bcast": {"opcode": "event_whenflagclicked", "next": "rb1",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 1400},
        "rb1": {"opcode": "control_repeat", "next": null, "parent": "h_bcast",
               "inputs": {"TIMES": [1, [6, "3"]], "SUBSTACK": [2, "bc1"]},
               "fields": {}, "topLevel": false, "shadow": false},
        "bc1": {"opcode": "event_broadcast", "next": null, "parent": "rb1",
               "inputs": {"BROADCAST_INPUT": [1, [11, "go", "b1"]]},
               "fields": {}, "topLevel": false, "shadow": false},

        "h_clone": {"opcode": "event_whenflagclicked", "next": "cc1",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 1600},
        "cc1": {"opcode": "control_create_clone_of", "next": null, "parent": "h_clone",
               "inputs": {"CLONE_OPTION": [1, "cmenu"]},
               "fields": {}, "topLevel": false, "shadow": false},
        "cmenu": {"opcode": "control_create_clone_of_menu", "next": null, "parent": "cc1",
               "inputs": {}, "fields": {"CLONE_OPTION": ["myself", "_myself_"]},
               "topLevel": false, "shadow": true}
      },
      "comments": {},
      "costumes": [], "sounds": [],
      "currentCostume": 0, "volume": 100, "layerOrder": 1,
      "visible": true, "x": 0, "y": 0, "size": 100, "direction": 90,
      "draggable": false, "rotationStyle": "all around"
    }
  ],
  "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}
})JSON";

const analysis::ComplexityInfo* findByTop(const analysis::Report& r,
                                          const analysis::ComplexityReport& c,
                                          const std::string& topId) {
    size_t idx = 0;
    bool found = false;
    for (size_t i = 0; i < r.scripts.size(); ++i) {
        if (r.scripts[i].topId == topId) {
            idx = i;
            found = true;
            break;
        }
    }
    if (!found) return nullptr;
    for (const auto& s : c.scripts)
        if (s.scriptIndex == idx) return &s;
    return nullptr;
}

}  // namespace

int test_complexity_main() {
    std::string s = kProject;
    ZipWriter zw;
    zw.addFile("project.json", std::vector<uint8_t>(s.begin(), s.end()));
    const std::vector<uint8_t> sb3 = zw.finish();

    Runtime rt;
    Sb3LoadResult lr = loadSb3File(rt, sb3.data(), sb3.size());
    CHECK(lr.ok);
    if (!lr.ok) {
        std::printf("  load error: %s\n", lr.error.c_str());
        std::printf("complexity: %d checks, %d failures\n", g_checks, g_fail);
        return g_fail;
    }
    const analysis::Report r = analysis::analyzeProject(rt, "test.sb3");
    const analysis::ComplexityReport c = analysis::analyzeComplexity(r);

    const auto* kConst = findByTop(r, c, "h_const");
    CHECK(kConst != nullptr);
    if (kConst) {
        CHECK(kConst->exact);
        CHECK(kConst->exactSteps == 61);
        CHECK(kConst->bigO == "O(1)");
        CHECK(!kConst->infinite && !kConst->unknown && !kConst->recursive);
    }

    const auto* kVar = findByTop(r, c, "h_var");
    CHECK(kVar != nullptr);
    if (kVar) {
        CHECK(!kVar->exact);
        CHECK(kVar->bigO == "O(N)");
        CHECK(kVar->boundExpr.find("N") != std::string::npos);
    }

    const auto* kUntil = findByTop(r, c, "h_until");
    CHECK(kUntil != nullptr);
    if (kUntil) {
        CHECK(kUntil->unknown);
        CHECK(kUntil->bigO == "O(?)");
    }

    const auto* kForev = findByTop(r, c, "h_forev");
    CHECK(kForev != nullptr);
    if (kForev) {
        CHECK(kForev->infinite);
        CHECK(kForev->bigO == "O(∞)");
    }

    const auto* kScan = findByTop(r, c, "h_scan");
    CHECK(kScan != nullptr);
    if (kScan) {
        CHECK(kScan->bigO.find("|items|") != std::string::npos);
    }

    const auto* kRec = findByTop(r, c, "h_rec");
    CHECK(kRec != nullptr);
    if (kRec) CHECK(kRec->recursive);

    // --- 式の記号化: repeat (N+1) ---
    const auto* kExpr = findByTop(r, c, "h_expr");
    CHECK(kExpr != nullptr);
    if (kExpr) {
        CHECK(!kExpr->exact);
        CHECK(!kExpr->unknown);
        CHECK(kExpr->bigO == "O((N+1))");
        CHECK(kExpr->boundExpr.find("(N+1)") != std::string::npos);
    }

    // --- 線形ランキング: repeat until [i > 10] + change i by 1 ---
    const auto* kRank = findByTop(r, c, "h_rank");
    CHECK(kRank != nullptr);
    if (kRank) {
        CHECK(!kRank->unknown);
        CHECK(kRank->boundExpr.find("(10-I)") != std::string::npos);
        CHECK(kRank->bigO == "O((10-I))");
    }

    // --- spawn: repeat 3 { broadcast go }（受信は h_until） ---
    const auto* kBcast = findByTop(r, c, "h_bcast");
    CHECK(kBcast != nullptr);
    if (kBcast) {
        CHECK(kBcast->spawns.size() == 1);
        if (!kBcast->spawns.empty()) {
            CHECK(kBcast->spawns[0].kind == "broadcast");
            CHECK(kBcast->spawns[0].target == "go");
            CHECK(kBcast->spawns[0].mult == "3");
        }
    }

    // --- spawn: clone ---
    const auto* kClone = findByTop(r, c, "h_clone");
    CHECK(kClone != nullptr);
    if (kClone) {
        CHECK(kClone->spawns.size() == 1);
        if (!kClone->spawns.empty())
            CHECK(kClone->spawns[0].kind == "clone");
    }

    const std::string text = analysis::formatComplexity(r, c);
    CHECK(text.find("計算量") != std::string::npos);
    CHECK(text.find("O(∞)") != std::string::npos);
    CHECK(text.find("起動関係") != std::string::npos);
    CHECK(text.find("broadcast 'go'") != std::string::npos);

    std::printf("complexity: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}
