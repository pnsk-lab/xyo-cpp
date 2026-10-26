// xyo-cpp: ループ不変 pen setter のホイスト（warp 手続き内）のテスト。
//   - 本体先頭の `pen set size to <定数>` がループ外へ 1 回だけ出る
//   - ループが 0 回なら setter は 1 回も実行されない（実行ガード）
//   - 本体に手続き呼出しがある / 引数がループ内で書き換わる場合はホイストしない
#include <cstdio>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "engine/runtime.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "load/zip.hpp"
#include "model/target.hpp"

using namespace xyo;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; \
    if (!((a) == (b))) { ++g_fail; \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, \
                #a, #b, (long long)(a), (long long)(b)); } } while (0)

extern "C" int64_t xyo_pen_size_calls;

namespace {

/// `set pen size to 100` → `repeat N { pen set size to 3 }` を warp 手続きで実行。
/// callBody=true ならループ本体に手続き呼出しを挟む（ホイスト禁止の確認）。
std::string projectJson(int n, bool callBody) {
    char buf[8192];
    const char* bodyTail =
        callBody
            ? "\"chg\": {\"opcode\": \"procedures_call\", \"next\": \"set3\","
              " \"parent\": \"loop\", \"inputs\": {}, \"fields\": {},"
              " \"topLevel\": false, \"shadow\": false,"
              " \"mutation\": {\"tagName\": \"mutation\", \"children\": [],"
              " \"proccode\": \"noop\", \"argumentids\": \"[]\","
              " \"argumentnames\": \"[]\", \"argumentdefaults\": \"[]\","
              " \"warp\": \"true\"}},"
            : "";
    std::snprintf(
        buf, sizeof buf,
        R"JSON({
  "targets": [
    {"isStage": true, "name": "Stage", "variables": {}, "lists": {},
     "broadcasts": {}, "blocks": {}, "comments": {}, "costumes": [],
     "sounds": [], "currentCostume": 0, "volume": 100, "layerOrder": 0},
    {"isStage": false, "name": "S", "layerOrder": 1,
     "variables": {}, "lists": {}, "broadcasts": {},
     "blocks": {
       "hat": {"opcode": "event_whenflagclicked", "next": "call",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 0},
       "call": {"opcode": "procedures_call", "next": null, "parent": "hat",
                "inputs": {"argN": [1, "argn"]}, "fields": {},
                "topLevel": false, "shadow": false,
                "mutation": {"tagName": "mutation", "children": [],
                             "proccode": "run %%s", "argumentids": "[\"argN\"]",
                             "argumentnames": "[\"n\"]",
                             "argumentdefaults": "[\"0\"]", "warp": "true"}},
       "argn": {"opcode": "math_number", "next": null, "parent": "call",
                "inputs": {}, "fields": {"NUM": [%d, null]},
                "topLevel": false, "shadow": true},
       "def": {"opcode": "procedures_definition", "next": "sz100",
               "parent": null, "inputs": {"custom_block": [1, "proto"]},
               "fields": {}, "topLevel": true, "shadow": false,
               "x": 0, "y": 400},
       "proto": {"opcode": "procedures_prototype", "next": null,
                 "parent": "def", "inputs": {"argN": [1, "argn2"]},
                 "fields": {}, "topLevel": false, "shadow": true,
                 "mutation": {"tagName": "mutation", "children": [],
                              "proccode": "run %%s",
                              "argumentids": "[\"argN\"]",
                              "argumentnames": "[\"n\"]",
                              "argumentdefaults": "[\"0\"]",
                              "warp": "true"}},
       "argn2": {"opcode": "argument_reporter_string_number", "next": null,
                 "parent": "proto", "inputs": {},
                 "fields": {"VALUE": ["n", null]},
                 "topLevel": false, "shadow": true},
       "sz100": {"opcode": "pen_setPenSizeTo", "next": "loop",
                 "parent": "def", "inputs": {"SIZE": [1, [4, "100"]]},
                 "fields": {}, "topLevel": false, "shadow": false},
       "loop": {"opcode": "control_repeat", "next": null, "parent": "sz100",
                "inputs": {"TIMES": [3, "argr", [6, "0"]],
                           "SUBSTACK": [2, "%s"]},
                "fields": {}, "topLevel": false, "shadow": false},
       "argr": {"opcode": "argument_reporter_string_number", "next": null,
                "parent": "loop", "inputs": {}, "fields": {"VALUE": ["n", null]},
                "topLevel": false, "shadow": false},
       %s
       "set3": {"opcode": "pen_setPenSizeTo", "next": null, "parent": "loop",
                "inputs": {"SIZE": [1, [4, "3"]]},
                "fields": {}, "topLevel": false, "shadow": false},
       "noopdef": {"opcode": "procedures_definition", "next": null,
                   "parent": null, "inputs": {"custom_block": [1, "np"]},
                   "fields": {}, "topLevel": true, "shadow": false,
                   "x": 0, "y": 800},
       "np": {"opcode": "procedures_prototype", "next": null,
              "parent": "noopdef", "inputs": {}, "fields": {},
              "topLevel": false, "shadow": true,
              "mutation": {"tagName": "mutation", "children": [],
                           "proccode": "noop", "argumentids": "[]",
                           "argumentnames": "[]", "argumentdefaults": "[]",
                           "warp": "true"}}
     },
     "comments": {}, "costumes": [], "sounds": [], "currentCostume": 0,
     "volume": 100, "visible": true, "x": 0, "y": 0, "size": 100,
     "direction": 90, "draggable": false, "rotationStyle": "all around"}
  ],
  "monitors": [], "extensions": ["pen"], "meta": {"semver": "3.0.0"}
})JSON",
        n, callBody ? "chg" : "set3", bodyTail);
    return buf;
}

struct RunResult {
    double penSize = -1;
    int64_t sizeCalls = 0;
};

RunResult run(int n, bool callBody) {
    RunResult r;
    const std::string json = projectJson(n, callBody);
    ZipWriter zw;
    zw.addFile("project.json", std::vector<uint8_t>(json.begin(), json.end()));
    const std::vector<uint8_t> sb3 = zw.finish();
    Runtime rt;
    Sb3LoadResult lr = loadSb3File(rt, sb3.data(), sb3.size());
    if (!lr.ok) {
        std::printf("  load error: %s\n", lr.error.c_str());
        return r;
    }
    const int64_t before = xyo_pen_size_calls;
    rt.greenFlag();
    rt.runUntilDone(10000);
    r.sizeCalls = xyo_pen_size_calls - before;
    if (Target* sp = rt.getSpriteTargetByName(u"S"))
        if (sp->penState) r.penSize = sp->penState->penSize;
    return r;
}

}  // namespace

int test_pen_hoist_main() {
#ifndef XYO_JIT_DISABLED
    if (jitEnabled()) {
        // 5 回のループ → setter はホイストされて 1 回だけ。最終サイズは 3。
        RunResult a = run(5, false);
        CHECK_EQ(a.penSize, 3.0);
        CHECK_EQ(a.sizeCalls, 2);   // sz100 + ホイストした 1 回
        // 0 回のループ → 本体は実行されないので setter も走らない（サイズ 100 のまま）
        RunResult b = run(0, false);
        CHECK_EQ(b.penSize, 100.0);
        CHECK_EQ(b.sizeCalls, 1);   // sz100 のみ（ループ本体は 0 回）
        // 本体に手続き呼出しがある → 不変性を保証しないのでホイストしない
        RunResult c = run(5, true);
        CHECK_EQ(c.penSize, 3.0);
        CHECK_EQ(c.sizeCalls, 6);   // sz100 + 本体 5 回（ホイストしない）
    }
#endif
    std::printf("pen hoist: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}
