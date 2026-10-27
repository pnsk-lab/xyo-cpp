// xyo-cpp: ネイティブ GPU コンパイラ/プレイヤ/レンダラ相互運用テスト。
// 既定は CPU/構造ケースのみ。XYO_TEST_GPU=1 で実ドライバ経由の
// ライブ dispatch ケースを追加実行する。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/slist.hpp"
#include "core/value.hpp"
#include "engine/runtime.hpp"
#include "gpu/gpu_native.hpp"
#include "gpu/gpu_realtime.hpp"
#include "load/sb3.hpp"
#include "model/blocks.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"
#include "render/sdl_renderer.hpp"

using namespace xyo;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; \
    if (!((a) == (b))) { ++g_fail; \
    std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

// ---------------------------------------------------------------
// 正準 r.render / pt.pass 構造を持つフィクスチャ。
// bvh ターゲット: @res/x/y/r.c/r.l/pt.pn/pt.pi + pt:r/g/b リスト。
// 手続き本体は最小限（trace/shade/sample は空定義）。
// ---------------------------------------------------------------
static const char* kProject = R"JSON({
  "targets": [
    {
      "isStage": true, "name": "Stage",
      "variables": {}, "lists": {}, "broadcasts": {},
      "blocks": {}, "comments": {}, "costumes": [], "sounds": [],
      "currentCostume": 0, "volume": 100, "layerOrder": 0
    },
    {
      "isStage": false, "name": "bvh",
      "variables": {
        "vres": ["@res", 1],
        "vx": ["x", 0], "vy": ["y", 0],
        "vtc": ["r.c", 0], "vlc": ["r.l", 0],
        "vpn": ["pt.pn", 0], "vpi": ["pt.pi", 0]
      },
      "lists": {
        "lar": ["pt:r", [0, 0, 0, 0]],
        "lag": ["pt:g", [0, 0, 0, 0]],
        "lab": ["pt:b", [0, 0, 0, 0]]
      },
      "broadcasts": {},
      "blocks": {
        "pd_render": {"opcode": "procedures_definition", "topLevel": true,
            "next": "b_pn", "parent": null,
            "inputs": {"custom_block": [1, "pp_render"]}, "fields": {},
            "shadow": false, "x": 0, "y": 0},
        "pp_render": {"opcode": "procedures_prototype", "topLevel": false,
            "shadow": true, "parent": "pd_render", "inputs": {}, "fields": {},
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "r.render", "argumentids": [],
                "argumentnames": [], "argumentdefaults": [], "warp": true}},
        "pd_pass": {"opcode": "procedures_definition", "topLevel": true,
            "next": "p_pn", "parent": null,
            "inputs": {"custom_block": [1, "pp_pass"]}, "fields": {},
            "shadow": false, "x": 0, "y": 10},
        "pp_pass": {"opcode": "procedures_prototype", "topLevel": false,
            "shadow": true, "parent": "pd_pass", "inputs": {}, "fields": {},
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "pt.pass", "argumentids": [],
                "argumentnames": [], "argumentdefaults": [], "warp": true}},
        "pd_trace": {"opcode": "procedures_definition", "topLevel": true,
            "next": null, "parent": null,
            "inputs": {"custom_block": [1, "pp_trace"]}, "fields": {},
            "shadow": false, "x": 0, "y": 20},
        "pp_trace": {"opcode": "procedures_prototype", "topLevel": false,
            "shadow": true, "parent": "pd_trace", "inputs": {}, "fields": {},
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "r.trace", "argumentids": [],
                "argumentnames": [], "argumentdefaults": [], "warp": true}},
        "pd_shade": {"opcode": "procedures_definition", "topLevel": true,
            "next": null, "parent": null,
            "inputs": {"custom_block": [1, "pp_shade"]}, "fields": {},
            "shadow": false, "x": 0, "y": 30},
        "pp_shade": {"opcode": "procedures_prototype", "topLevel": false,
            "shadow": true, "parent": "pd_shade", "inputs": {}, "fields": {},
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "r.shade", "argumentids": [],
                "argumentnames": [], "argumentdefaults": [], "warp": true}},
        "pd_reflect": {"opcode": "procedures_definition", "topLevel": true,
            "next": null, "parent": null,
            "inputs": {"custom_block": [1, "pp_reflect"]}, "fields": {},
            "shadow": false, "x": 0, "y": 40},
        "pp_reflect": {"opcode": "procedures_prototype", "topLevel": false,
            "shadow": true, "parent": "pd_reflect", "inputs": {}, "fields": {},
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "r.reflect", "argumentids": [],
                "argumentnames": [], "argumentdefaults": [], "warp": true}},
        "pd_sample": {"opcode": "procedures_definition", "topLevel": true,
            "next": null, "parent": null,
            "inputs": {"custom_block": [1, "pp_sample"]}, "fields": {},
            "shadow": false, "x": 0, "y": 50},
        "pp_sample": {"opcode": "procedures_prototype", "topLevel": false,
            "shadow": true, "parent": "pd_sample", "inputs": {}, "fields": {},
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "pt.sample", "argumentids": [],
                "argumentnames": [], "argumentdefaults": [], "warp": true}},
        "pd_tex": {"opcode": "procedures_definition", "topLevel": true,
            "next": null, "parent": null,
            "inputs": {"custom_block": [1, "pp_tex"]}, "fields": {},
            "shadow": false, "x": 0, "y": 60},
        "pp_tex": {"opcode": "procedures_prototype", "topLevel": false,
            "shadow": true, "parent": "pd_tex", "inputs": {}, "fields": {},
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "tex setup", "argumentids": [],
                "argumentnames": [], "argumentdefaults": [], "warp": true}},
        "pd_reset": {"opcode": "procedures_definition", "topLevel": true,
            "next": null, "parent": null,
            "inputs": {"custom_block": [1, "pp_reset"]}, "fields": {},
            "shadow": false, "x": 0, "y": 70},
        "pp_reset": {"opcode": "procedures_prototype", "topLevel": false,
            "shadow": true, "parent": "pd_reset", "inputs": {}, "fields": {},
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "pt.reset", "argumentids": [],
                "argumentnames": [], "argumentdefaults": [], "warp": true}},
        "pd_camera": {"opcode": "procedures_definition", "topLevel": true,
            "next": null, "parent": null,
            "inputs": {"custom_block": [1, "pp_camera"]}, "fields": {},
            "shadow": false, "x": 0, "y": 80},
        "pp_camera": {"opcode": "procedures_prototype", "topLevel": false,
            "shadow": true, "parent": "pd_camera", "inputs": {}, "fields": {},
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "camera", "argumentids": [],
                "argumentnames": [], "argumentdefaults": [], "warp": true}},
        "pd_math": {"opcode": "procedures_definition", "topLevel": true,
            "next": null, "parent": null,
            "inputs": {"custom_block": [1, "pp_math"]}, "fields": {},
            "shadow": false, "x": 0, "y": 90},
        "pp_math": {"opcode": "procedures_prototype", "topLevel": false,
            "shadow": true, "parent": "pd_math", "inputs": {}, "fields": {},
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "math", "argumentids": [],
                "argumentnames": [], "argumentdefaults": [], "warp": true}},
        "pd_draw": {"opcode": "procedures_definition", "topLevel": true,
            "next": null, "parent": null,
            "inputs": {"custom_block": [1, "pp_draw"]}, "fields": {},
            "shadow": false, "x": 0, "y": 100},
        "pp_draw": {"opcode": "procedures_prototype", "topLevel": false,
            "shadow": true, "parent": "pd_draw", "inputs": {}, "fields": {},
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "draw", "argumentids": [],
                "argumentnames": [], "argumentdefaults": [], "warp": true}},

        "b_pn": {"opcode": "data_changevariableby", "next": "b_sety",
            "parent": "pd_render", "inputs": {"VALUE": [1, [4, "1"]]},
            "fields": {"VARIABLE": ["pt.pn", "vpn"]},
            "topLevel": false, "shadow": false},
        "b_sety": {"opcode": "data_setvariableto", "next": "b_outer",
            "parent": "pd_render", "inputs": {"VALUE": [1, [4, "-179.5"]]},
            "fields": {"VARIABLE": ["y", "vy"]},
            "topLevel": false, "shadow": false},
        "b_outer": {"opcode": "control_repeat", "next": null,
            "parent": "pd_render",
            "inputs": {"TIMES": [1, "b_div360"], "SUBSTACK": [2, "b_setx"]},
            "fields": {}, "topLevel": false, "shadow": false},
        "b_div360": {"opcode": "operator_divide", "next": null,
            "parent": "b_outer",
            "inputs": {"NUM1": [1, [4, "360"]],
                       "NUM2": [1, [12, "@res", "vres"]]},
            "fields": {}, "topLevel": false, "shadow": false},
        "b_setx": {"opcode": "data_setvariableto", "next": "b_inner",
            "parent": "b_outer", "inputs": {"VALUE": [1, [4, "-239.5"]]},
            "fields": {"VARIABLE": ["x", "vx"]},
            "topLevel": false, "shadow": false},
        "b_inner": {"opcode": "control_repeat", "next": "b_chy",
            "parent": "b_outer",
            "inputs": {"TIMES": [1, "b_div480"], "SUBSTACK": [2, "i_trace"]},
            "fields": {}, "topLevel": false, "shadow": false},
        "b_div480": {"opcode": "operator_divide", "next": null,
            "parent": "b_inner",
            "inputs": {"NUM1": [1, [4, "480"]],
                       "NUM2": [1, [12, "@res", "vres"]]},
            "fields": {}, "topLevel": false, "shadow": false},
        "i_trace": {"opcode": "procedures_call", "next": "i_shade",
            "parent": "b_inner", "inputs": {}, "fields": {},
            "topLevel": false, "shadow": false,
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "r.trace", "argumentids": [],
                "argumentnames": [], "argumentdefaults": []}},
        "i_shade": {"opcode": "procedures_call", "next": "i_reflect",
            "parent": "b_inner", "inputs": {}, "fields": {},
            "topLevel": false, "shadow": false,
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "r.shade", "argumentids": [],
                "argumentnames": [], "argumentdefaults": []}},
        "i_reflect": {"opcode": "procedures_call", "next": "i_setc",
            "parent": "b_inner", "inputs": {}, "fields": {},
            "topLevel": false, "shadow": false,
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "r.reflect", "argumentids": [],
                "argumentnames": [], "argumentdefaults": []}},
        "i_setc": {"opcode": "data_setvariableto", "next": "i_if",
            "parent": "b_inner", "inputs": {"VALUE": [1, [4, "16711680"]]},
            "fields": {"VARIABLE": ["r.c", "vtc"]},
            "topLevel": false, "shadow": false},
        "i_if": {"opcode": "control_if", "next": "i_chx",
            "parent": "b_inner",
            "inputs": {"CONDITION": [2, "c_not"], "SUBSTACK": [2, "i_setl"]},
            "fields": {}, "topLevel": false, "shadow": false},
        "c_not": {"opcode": "operator_not", "next": null, "parent": "i_if",
            "inputs": {"OPERAND": [1, "c_eq"]},
            "fields": {}, "topLevel": false, "shadow": false},
        "c_eq": {"opcode": "operator_equals", "next": null, "parent": "c_not",
            "inputs": {"OPERAND1": [2, "c_vt"], "OPERAND2": [2, "c_vl"]},
            "fields": {}, "topLevel": false, "shadow": false},
        "c_vt": {"opcode": "data_variable", "next": null, "parent": "c_eq",
            "inputs": {}, "fields": {"VARIABLE": ["r.c", "vtc"]},
            "topLevel": false, "shadow": false},
        "c_vl": {"opcode": "data_variable", "next": null, "parent": "c_eq",
            "inputs": {}, "fields": {"VARIABLE": ["r.l", "vlc"]},
            "topLevel": false, "shadow": false},
        "i_setl": {"opcode": "data_setvariableto", "next": null,
            "parent": "i_if", "inputs": {"VALUE": [2, "c_vt2"]},
            "fields": {"VARIABLE": ["r.l", "vlc"]},
            "topLevel": false, "shadow": false},
        "c_vt2": {"opcode": "data_variable", "next": null, "parent": "i_setl",
            "inputs": {}, "fields": {"VARIABLE": ["r.c", "vtc"]},
            "topLevel": false, "shadow": false},
        "i_chx": {"opcode": "data_changevariableby", "next": null,
            "parent": "b_inner",
            "inputs": {"VALUE": [1, [12, "@res", "vres"]]},
            "fields": {"VARIABLE": ["x", "vx"]},
            "topLevel": false, "shadow": false},
        "b_chy": {"opcode": "data_changevariableby", "next": null,
            "parent": "b_outer",
            "inputs": {"VALUE": [1, [12, "@res", "vres"]]},
            "fields": {"VARIABLE": ["y", "vy"]},
            "topLevel": false, "shadow": false},

        "p_pn": {"opcode": "data_changevariableby", "next": "p_sety",
            "parent": "pd_pass", "inputs": {"VALUE": [1, [4, "1"]]},
            "fields": {"VARIABLE": ["pt.pn", "vpn"]},
            "topLevel": false, "shadow": false},
        "p_sety": {"opcode": "data_setvariableto", "next": "p_outer",
            "parent": "pd_pass", "inputs": {"VALUE": [1, [4, "-179.5"]]},
            "fields": {"VARIABLE": ["y", "vy"]},
            "topLevel": false, "shadow": false},
        "p_outer": {"opcode": "control_repeat", "next": null,
            "parent": "pd_pass",
            "inputs": {"TIMES": [1, "p_div360"], "SUBSTACK": [2, "p_setx"]},
            "fields": {}, "topLevel": false, "shadow": false},
        "p_div360": {"opcode": "operator_divide", "next": null,
            "parent": "p_outer",
            "inputs": {"NUM1": [1, [4, "360"]],
                       "NUM2": [1, [12, "@res", "vres"]]},
            "fields": {}, "topLevel": false, "shadow": false},
        "p_setx": {"opcode": "data_setvariableto", "next": "p_inner",
            "parent": "p_outer", "inputs": {"VALUE": [1, [4, "-239.5"]]},
            "fields": {"VARIABLE": ["x", "vx"]},
            "topLevel": false, "shadow": false},
        "p_inner": {"opcode": "control_repeat", "next": "p_chy",
            "parent": "p_outer",
            "inputs": {"TIMES": [1, "p_div480"], "SUBSTACK": [2, "q_pinc"]},
            "fields": {}, "topLevel": false, "shadow": false},
        "p_div480": {"opcode": "operator_divide", "next": null,
            "parent": "p_inner",
            "inputs": {"NUM1": [1, [4, "480"]],
                       "NUM2": [1, [12, "@res", "vres"]]},
            "fields": {}, "topLevel": false, "shadow": false},
        "q_pinc": {"opcode": "data_changevariableby", "next": "q_trace",
            "parent": "p_inner", "inputs": {"VALUE": [1, [4, "1"]]},
            "fields": {"VARIABLE": ["pt.pi", "vpi"]},
            "topLevel": false, "shadow": false},
        "q_trace": {"opcode": "procedures_call", "next": "q_sample",
            "parent": "p_inner", "inputs": {}, "fields": {},
            "topLevel": false, "shadow": false,
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "r.trace", "argumentids": [],
                "argumentnames": [], "argumentdefaults": []}},
        "q_sample": {"opcode": "procedures_call", "next": "q_r",
            "parent": "p_inner", "inputs": {}, "fields": {},
            "topLevel": false, "shadow": false,
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "pt.sample", "argumentids": [],
                "argumentnames": [], "argumentdefaults": []}},
        "q_r": {"opcode": "data_replaceitemoflist", "next": "q_g",
            "parent": "p_inner",
            "inputs": {"INDEX": [2, "q_pi1"], "ITEM": [1, [4, "1"]]},
            "fields": {"LIST": ["pt:r", "lar"]},
            "topLevel": false, "shadow": false},
        "q_pi1": {"opcode": "data_variable", "next": null, "parent": "q_r",
            "inputs": {}, "fields": {"VARIABLE": ["pt.pi", "vpi"]},
            "topLevel": false, "shadow": false},
        "q_g": {"opcode": "data_replaceitemoflist", "next": "q_b",
            "parent": "p_inner",
            "inputs": {"INDEX": [2, "q_pi2"], "ITEM": [1, [4, "1"]]},
            "fields": {"LIST": ["pt:g", "lag"]},
            "topLevel": false, "shadow": false},
        "q_pi2": {"opcode": "data_variable", "next": null, "parent": "q_g",
            "inputs": {}, "fields": {"VARIABLE": ["pt.pi", "vpi"]},
            "topLevel": false, "shadow": false},
        "q_b": {"opcode": "data_replaceitemoflist", "next": "q_chx",
            "parent": "p_inner",
            "inputs": {"INDEX": [2, "q_pi3"], "ITEM": [1, [4, "1"]]},
            "fields": {"LIST": ["pt:b", "lab"]},
            "topLevel": false, "shadow": false},
        "q_pi3": {"opcode": "data_variable", "next": null, "parent": "q_b",
            "inputs": {}, "fields": {"VARIABLE": ["pt.pi", "vpi"]},
            "topLevel": false, "shadow": false},
        "q_chx": {"opcode": "data_changevariableby", "next": null,
            "parent": "p_inner",
            "inputs": {"VALUE": [1, [12, "@res", "vres"]]},
            "fields": {"VARIABLE": ["x", "vx"]},
            "topLevel": false, "shadow": false},
        "p_chy": {"opcode": "data_changevariableby", "next": null,
            "parent": "p_outer",
            "inputs": {"VALUE": [1, [12, "@res", "vres"]]},
            "fields": {"VARIABLE": ["y", "vy"]},
            "topLevel": false, "shadow": false},

        "h_r": {"opcode": "event_whenkeypressed", "topLevel": true,
            "next": "h_tex", "parent": null, "inputs": {},
            "fields": {"KEY_OPTION": ["r", null]},
            "shadow": false, "x": 0, "y": 200},
        "h_tex": {"opcode": "procedures_call", "next": "h_rr",
            "parent": "h_r", "inputs": {}, "fields": {},
            "topLevel": false, "shadow": false,
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "tex setup", "argumentids": [],
                "argumentnames": [], "argumentdefaults": []}},
        "h_rr": {"opcode": "procedures_call", "next": null,
            "parent": "h_r", "inputs": {}, "fields": {},
            "topLevel": false, "shadow": false,
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "r.render", "argumentids": [],
                "argumentnames": [], "argumentdefaults": []}},
        "h_p": {"opcode": "event_whenkeypressed", "topLevel": true,
            "next": "h_rst", "parent": null, "inputs": {},
            "fields": {"KEY_OPTION": ["p", null]},
            "shadow": false, "x": 0, "y": 210},
        "h_rst": {"opcode": "procedures_call", "next": "h_pf",
            "parent": "h_p", "inputs": {}, "fields": {},
            "topLevel": false, "shadow": false,
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "pt.reset", "argumentids": [],
                "argumentnames": [], "argumentdefaults": []}},
        "h_pf": {"opcode": "control_forever", "next": null,
            "parent": "h_p",
            "inputs": {"SUBSTACK": [2, "h_pp"]},
            "fields": {}, "topLevel": false, "shadow": false},
        "h_pp": {"opcode": "procedures_call", "next": null,
            "parent": "h_pf", "inputs": {}, "fields": {},
            "topLevel": false, "shadow": false,
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "pt.pass", "argumentids": [],
                "argumentnames": [], "argumentdefaults": []}},

        "h_gf": {"opcode": "event_whenflagclicked", "topLevel": true,
            "next": "d_for", "parent": null, "inputs": {}, "fields": {},
            "shadow": false, "x": 0, "y": 300},
        "d_for": {"opcode": "control_forever", "next": null,
            "parent": "h_gf",
            "inputs": {"SUBSTACK": [2, "d_cam"]},
            "fields": {}, "topLevel": false, "shadow": false},
        "d_cam": {"opcode": "procedures_call", "next": "d_math",
            "parent": "d_for", "inputs": {}, "fields": {},
            "topLevel": false, "shadow": false,
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "camera", "argumentids": [],
                "argumentnames": [], "argumentdefaults": []}},
        "d_math": {"opcode": "procedures_call", "next": "d_draw",
            "parent": "d_for", "inputs": {}, "fields": {},
            "topLevel": false, "shadow": false,
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "math", "argumentids": [],
                "argumentnames": [], "argumentdefaults": []}},
        "d_draw": {"opcode": "procedures_call", "next": "d_if",
            "parent": "d_for", "inputs": {}, "fields": {},
            "topLevel": false, "shadow": false,
            "mutation": {"tagName": "mutation", "children": [],
                "proccode": "draw", "argumentids": [],
                "argumentnames": [], "argumentdefaults": []}},
        "d_if": {"opcode": "control_if", "next": null,
            "parent": "d_for",
            "inputs": {"CONDITION": [2, "d_key"], "SUBSTACK": [2, "d_stop"]},
            "fields": {}, "topLevel": false, "shadow": false},
        "d_key": {"opcode": "sensing_keypressed", "next": null,
            "parent": "d_if", "inputs": {"KEY_OPTION": [1, "d_ko"]},
            "fields": {}, "topLevel": false, "shadow": false},
        "d_ko": {"opcode": "sensing_keyoptions", "next": null,
            "parent": "d_key", "inputs": {},
            "fields": {"KEY_OPTION": ["p", null]},
            "topLevel": false, "shadow": true},
        "d_stop": {"opcode": "control_stop", "next": null,
            "parent": "d_if", "inputs": {},
            "fields": {"STOP_OPTION": ["all", null]},
            "topLevel": false, "shadow": false}
      },
      "comments": {}, "costumes": [], "sounds": [],
      "currentCostume": 0, "x": 0, "y": 0, "size": 100,
      "direction": 90, "draggable": false,
      "rotationStyle": "all around", "visible": true,
      "layerOrder": 1, "volume": 100
    }
  ],
  "monitors": [], "extensions": ["pen"],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";

// r.render 定義を持たない最小プロジェクト
static const char* kProjectEmpty = R"JSON({
  "targets": [
    {"isStage": true, "name": "Stage", "variables": {}, "lists": {},
     "broadcasts": {}, "blocks": {}, "comments": {}, "costumes": [],
     "sounds": [], "currentCostume": 0, "volume": 100, "layerOrder": 0},
    {"isStage": false, "name": "bvh", "variables": {}, "lists": {},
     "broadcasts": {}, "blocks": {}, "comments": {}, "costumes": [],
     "sounds": [], "currentCostume": 0, "x": 0, "y": 0, "size": 100,
     "direction": 90, "draggable": false, "rotationStyle": "all around",
     "visible": true, "layerOrder": 1, "volume": 100}
  ],
  "monitors": [], "extensions": [],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";

static bool loadFixture(Runtime& rt, const char* json = kProject) {
    Sb3LoadResult r = loadSb3(rt, json, nullptr);
    if (!r.ok) std::printf("load error: %s\n", r.error.c_str());
    return r.ok;
}

static Target* bvhTarget(Runtime& rt) {
    for (Target* t : rt.targets)
        if (!t->isStage) return t;
    return nullptr;
}

static bool contains(const std::string& s, const char* sub) {
    return s.find(sub) != std::string::npos;
}

static void dumpSrc(const gpu::NativeProgram& p, const char* name) {
    const char* dir = std::getenv("XYO_GPU_DUMP");
    if (!dir) return;
    std::string path = std::string(dir) + "/" + name + ".comp";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(p.source.data(), 1, p.source.size(), f);
    std::fclose(f);
}

// --------------------------------------------------------------- compile --

static void test_compile_ray() {
    Runtime rt;
    CHECK(loadFixture(rt));
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) return;
    std::string why;
    auto p = gpu::compileRayKernel(bvh->blocks, false, &why);
    if (!p) {
        ++g_checks; ++g_fail;
        std::printf("FAIL ray compile: %s\n", why.c_str());
        return;
    }
    CHECK(!p->path);
    CHECK(!p->source.empty());
    const std::string& s = p->source;
    CHECK(contains(s, "#version 430"));
    CHECK(contains(s, "float xnum_(float v)"));
    CHECK(contains(s, "#if XYO_NATIVE_CAPTURE"));
    CHECK(!contains(s, "#define XYO_NATIVE_CAPTURE"));
    CHECK(contains(s, "uBasePixel"));
    CHECK(contains(s, "uDispatchPixels"));
    CHECK(contains(s, "gl_GlobalInvocationID"));
    CHECK(contains(s, "ob[kPenOff + 8u]"));
    CHECK(contains(s, "floor(a + 0.5)"));
    CHECK(!contains(s, "log10"));
    CHECK(contains(s, "floatBitsToUint(v"));
    dumpSrc(*p, "fixture_ray");
    CHECK(p->slotRes >= 0);
    CHECK(p->slotColVar >= 0);
    CHECK(p->slotRowVar >= 0);
    CHECK(p->slotThisColor >= 0);
    CHECK(p->slotLastColor >= 0);
    CHECK(!p->writtenVars.empty());
}

static void test_compile_path() {
    Runtime rt;
    CHECK(loadFixture(rt));
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) return;
    std::string why;
    auto p = gpu::compileRayKernel(bvh->blocks, true, &why);
    if (!p) {
        ++g_checks; ++g_fail;
        std::printf("FAIL path compile: %s\n", why.c_str());
        return;
    }
    CHECK(p->path);
    CHECK(!p->source.empty());
    CHECK(!p->accumVars[0].id.empty());
    CHECK(!p->accumVars[1].id.empty());
    CHECK(!p->accumVars[2].id.empty());
    CHECK(p->accumVars[3].id.empty());
    CHECK(p->slotPI >= 0);
    CHECK(p->slotPN >= 0);
    CHECK(contains(p->source, "acout[pixel_]"));
    CHECK(contains(p->source, "acin[pixel_]"));
    dumpSrc(*p, "fixture_path");
}

static void test_reject_no_proc() {
    Runtime rt;
    CHECK(loadFixture(rt, kProjectEmpty));
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) return;
    std::string why;
    auto p = gpu::compileRayKernel(bvh->blocks, false, &why);
    CHECK(!p.has_value());
    CHECK(!why.empty());
}

static void test_reject_dims() {
    Runtime rt;
    CHECK(loadFixture(rt));
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) return;
    Block* div = bvh->blocks->getBlock("b_div480");
    CHECK(div != nullptr);
    if (!div) return;
    const BlockInput* n1 = div->inputs.find("NUM1");
    CHECK(n1 != nullptr);
    if (!n1) return;
    Block* lit = bvh->blocks->getBlock(
        !n1->block.empty() ? n1->block : n1->shadow);
    CHECK(lit != nullptr);
    if (!lit) return;
    lit->fields["NUM"].value = Value::num(320);
    std::string why;
    auto p = gpu::compileRayKernel(bvh->blocks, false, &why);
    CHECK(!p.has_value());
}

static void test_reject_stacklist() {
    Runtime rt;
    CHECK(loadFixture(rt));
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) return;
    Variable other = Variable::create("lother", u"other",
                                      Variable::kListType, false);
    bvh->variables["lother"] = other;
    Block add;
    add.id = "zz_add";
    add.opcode = "data_addtolist";
    add.fields["LIST"] = {Value::string(SStr::create(u"other")), "lother"};
    add.next = "i_chx";
    bvh->blocks->createBlock(add);
    Block* setl = bvh->blocks->getBlock("i_setl");
    CHECK(setl != nullptr);
    if (!setl) return;
    setl->next = "zz_add";
    std::string why;
    auto p = gpu::compileRayKernel(bvh->blocks, false, &why);
    CHECK(!p.has_value());
}

static void test_reject_recursion() {
    Runtime rt;
    CHECK(loadFixture(rt));
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) return;
    Block self;
    self.id = "zz_self";
    self.opcode = "procedures_call";
    Mutation m;
    m.tagName = "mutation";
    m.attrs["proccode"] = "r.trace";
    m.attrs["argumentids"] = "[]";
    m.attrs["argumentnames"] = "[]";
    m.attrs["argumentdefaults"] = "[]";
    self.mutation = m;
    self.parent = "pd_trace";
    bvh->blocks->createBlock(self);
    Block* def = bvh->blocks->getBlock("pd_trace");
    CHECK(def != nullptr);
    if (!def) return;
    def->next = "zz_self";
    std::string why;
    auto p = gpu::compileRayKernel(bvh->blocks, false, &why);
    CHECK(!p.has_value());
}

// kProject に r.stack リストと push/read/pop を追加（trace が x,y を push、
// i_setc が index1 を r.c へ、reflect が pop×2）
static Target* loadStackFixture(Runtime& rt) {
    if (!loadFixture(rt)) return nullptr;
    Target* bvh = bvhTarget(rt);
    if (!bvh) return nullptr;
    bvh->variables["lsk"] = Variable::create("lsk", u"r.stack",
                                             Variable::kListType, false);
    Blocks* bl = bvh->blocks;
    auto mk = [&](const char* id, const char* op) {
        Block b;
        b.id = id;
        b.opcode = op;
        bl->createBlock(b);
        return bl->getBlock(id);
    };
    auto listRef = [](const char* name, const char* id) {
        BlockField f;
        f.value = Value::string(SStr::create(utf8ToU16(name)));
        f.id = id;
        return f;
    };
    auto varRef = [](const char* name, const char* id) {
        BlockField f;
        f.value = Value::string(SStr::create(utf8ToU16(name)));
        f.id = id;
        return f;
    };
    Block* vx = mk("s_vx", "data_variable");
    vx->fields["VARIABLE"] = varRef("x", "vx");
    Block* vy = mk("s_vy", "data_variable");
    vy->fields["VARIABLE"] = varRef("y", "vy");
    Block* pu1 = mk("s_push1", "data_addtolist");
    pu1->fields["LIST"] = listRef("r.stack", "lsk");
    pu1->inputs.set("ITEM", {"s_vx", ""});
    pu1->next = "s_push2";
    pu1->parent = "pd_trace";
    Block* pu2 = mk("s_push2", "data_addtolist");
    pu2->fields["LIST"] = listRef("r.stack", "lsk");
    pu2->inputs.set("ITEM", {"s_vy", ""});
    pu2->parent = "pd_trace";
    Block* pdTrace = bl->getBlock("pd_trace");
    if (pdTrace) pdTrace->next = "s_push1";
    Block* n1 = mk("s_n1", "math_number");
    n1->shadow = true;
    n1->fields["NUM"].value = Value::num(1);
    Block* get = mk("s_get", "data_itemoflist");
    get->fields["LIST"] = listRef("r.stack", "lsk");
    get->inputs.set("INDEX", {"", "s_n1"});
    get->parent = "b_inner";
    Block* setc = bl->getBlock("i_setc");
    if (setc) setc->inputs.set("VALUE", {"s_get", ""});
    Block* dn2 = mk("s_dn2", "math_number");
    dn2->shadow = true;
    dn2->fields["NUM"].value = Value::num(2);
    Block* dn1 = mk("s_dn1", "math_number");
    dn1->shadow = true;
    dn1->fields["NUM"].value = Value::num(1);
    Block* del2 = mk("s_del2", "data_deleteoflist");
    del2->fields["LIST"] = listRef("r.stack", "lsk");
    del2->inputs.set("INDEX", {"", "s_dn2"});
    del2->next = "s_del1";
    del2->parent = "pd_reflect";
    Block* del1 = mk("s_del1", "data_deleteoflist");
    del1->fields["LIST"] = listRef("r.stack", "lsk");
    del1->inputs.set("INDEX", {"", "s_dn1"});
    del1->parent = "pd_reflect";
    Block* pdReflect = bl->getBlock("pd_reflect");
    if (pdReflect) pdReflect->next = "s_del2";
    return bvh;
}

// kProject に r.stack + 引数2個の r.helper を追加（trace 内で add(x,y) と
// mul(y,3) を渡して呼び、helper が両方 push、i_setc が index1 を r.c へ、
// reflect が pop×2）
static Target* loadArgFixture(Runtime& rt) {
    if (!loadFixture(rt)) return nullptr;
    Target* bvh = bvhTarget(rt);
    if (!bvh) return nullptr;
    bvh->variables["lsk"] = Variable::create("lsk", u"r.stack",
                                             Variable::kListType, false);
    Blocks* bl = bvh->blocks;
    auto mk = [&](const char* id, const char* op) {
        Block b;
        b.id = id;
        b.opcode = op;
        bl->createBlock(b);
        return bl->getBlock(id);
    };
    auto fld = [](const char* name, const char* id) {
        BlockField f;
        f.value = Value::string(SStr::create(utf8ToU16(name)));
        f.id = id;
        return f;
    };
    auto numSh = [&](const char* id, double v) {
        Block* b = mk(id, "math_number");
        b->shadow = true;
        b->fields["NUM"].value = Value::num(v);
        return b;
    };
    Mutation hm;
    hm.tagName = "mutation";
    hm.attrs["proccode"] = "r.helper %s %s";
    hm.attrs["argumentids"] = "[\"a1\",\"a2\"]";
    hm.attrs["argumentnames"] = "[\"a1\",\"a2\"]";
    hm.attrs["argumentdefaults"] = "[\"0\",\"0\"]";
    hm.attrs["warp"] = "true";
    Block* pp = mk("s_pph", "procedures_prototype");
    pp->shadow = true;
    pp->parent = "s_pdh";
    pp->mutation = hm;
    Block* pd = mk("s_pdh", "procedures_definition");
    pd->topLevel = true;
    pd->inputs.set("custom_block", {"s_pph", "s_pph"});
    pd->next = "s_pusha";
    Block* ra = mk("sa_arga", "argument_reporter_string_number");
    ra->fields["VALUE"].value = Value::string(SStr::create(u"a1"));
    ra->parent = "s_pusha";
    Block* rb = mk("sa_argb", "argument_reporter_string_number");
    rb->fields["VALUE"].value = Value::string(SStr::create(u"a2"));
    rb->parent = "s_pushb";
    Block* pa = mk("s_pusha", "data_addtolist");
    pa->fields["LIST"] = fld("r.stack", "lsk");
    pa->inputs.set("ITEM", {"sa_arga", ""});
    pa->next = "s_pushb";
    pa->parent = "s_pdh";
    Block* pb = mk("s_pushb", "data_addtolist");
    pb->fields["LIST"] = fld("r.stack", "lsk");
    pb->inputs.set("ITEM", {"sa_argb", ""});
    pb->parent = "s_pdh";
    Block* vx = mk("ia_vx", "data_variable");
    vx->fields["VARIABLE"] = fld("x", "vx");
    Block* vy = mk("ia_vy", "data_variable");
    vy->fields["VARIABLE"] = fld("y", "vy");
    Block* vy2 = mk("ia_vy2", "data_variable");
    vy2->fields["VARIABLE"] = fld("y", "vy");
    numSh("ia_n3", 3);
    Block* add = mk("ia_add", "operator_add");
    add->inputs.set("NUM1", {"ia_vx", ""});
    add->inputs.set("NUM2", {"ia_vy", ""});
    add->parent = "s_callh";
    Block* mul = mk("ia_mul", "operator_multiply");
    mul->inputs.set("NUM1", {"ia_vy2", ""});
    mul->inputs.set("NUM2", {"", "ia_n3"});
    mul->parent = "s_callh";
    Block* call = mk("s_callh", "procedures_call");
    call->mutation = hm;
    call->inputs.set("a1", {"ia_add", ""});
    call->inputs.set("a2", {"ia_mul", ""});
    call->parent = "pd_trace";
    Block* pdTrace = bl->getBlock("pd_trace");
    if (pdTrace) pdTrace->next = "s_callh";
    numSh("sa_n1", 1);
    Block* get = mk("sa_get", "data_itemoflist");
    get->fields["LIST"] = fld("r.stack", "lsk");
    get->inputs.set("INDEX", {"", "sa_n1"});
    get->parent = "b_inner";
    Block* setc = bl->getBlock("i_setc");
    if (setc) setc->inputs.set("VALUE", {"sa_get", ""});
    numSh("sa_dn2", 2);
    numSh("sa_dn1", 1);
    Block* del2 = mk("sa_del2", "data_deleteoflist");
    del2->fields["LIST"] = fld("r.stack", "lsk");
    del2->inputs.set("INDEX", {"", "sa_dn2"});
    del2->next = "sa_del1";
    del2->parent = "pd_reflect";
    Block* del1 = mk("sa_del1", "data_deleteoflist");
    del1->fields["LIST"] = fld("r.stack", "lsk");
    del1->inputs.set("INDEX", {"", "sa_dn1"});
    del1->parent = "pd_reflect";
    Block* pdReflect = bl->getBlock("pd_reflect");
    if (pdReflect) pdReflect->next = "sa_del2";
    return bvh;
}

static void test_stack_source_modes() {
    const char* prev = std::getenv("XYO_GPU_SHARED_STACK");
    std::string saved = prev ? prev : "";
    unsetenv("XYO_GPU_SHARED_STACK");
    Runtime rt;
    CHECK(loadFixture(rt));
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) { if (!saved.empty()) setenv("XYO_GPU_SHARED_STACK", saved.c_str(), 1); return; }
    std::string why;
    auto pp = gpu::compileRayKernel(bvh->blocks, false, &why);
    CHECK(pp.has_value());
    if (pp) {
        CHECK(!pp->sharedStack);
        CHECK(contains(pp->source, "float pstk_[128];"));
        CHECK(!contains(pp->source, "pstkShared_"));
        CHECK(!contains(pp->source, "localLane_"));
    }
    setenv("XYO_GPU_SHARED_STACK", "1", 1);
    Runtime rt2;
    CHECK(loadFixture(rt2));
    Target* bvh2 = bvhTarget(rt2);
    CHECK(bvh2 != nullptr);
    if (bvh2) {
        auto ps = gpu::compileRayKernel(bvh2->blocks, false, &why);
        CHECK(ps.has_value());
        if (ps) {
            CHECK(ps->sharedStack);
            CHECK(contains(ps->source,
                           "shared float pstkShared_[128 * 64];"));
            CHECK(contains(ps->source,
                           "localLane_ = gl_LocalInvocationID.x;"));
            CHECK(contains(ps->source, "64u + localLane_"));
            CHECK(!contains(ps->source, "float pstk_[128];"));
            CHECK(!contains(ps->source, "pstk_[pstkLen_"));
        }
    }
    if (saved.empty()) unsetenv("XYO_GPU_SHARED_STACK");
    else setenv("XYO_GPU_SHARED_STACK", saved.c_str(), 1);
}

static bool hasProcCallSite(const std::string& s) {
    for (size_t i = 0; i + 2 < s.size(); ++i)
        if (s[i] == 'p' && std::isdigit((unsigned char)s[i + 1])) {
            size_t j = i + 1;
            while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
            if (j < s.size() && s[j] == '(') return true;
        }
    return false;
}

static void test_inline_source() {
    const char* prev = std::getenv("XYO_GPU_INLINE_LOCALS");
    std::string saved = prev ? prev : "";
    unsetenv("XYO_GPU_INLINE_LOCALS");
    Runtime rt;
    CHECK(loadFixture(rt));
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) {
        if (!saved.empty())
            setenv("XYO_GPU_INLINE_LOCALS", saved.c_str(), 1);
        return;
    }
    std::string why;
    auto pf = gpu::compileRayKernel(bvh->blocks, false, &why);
    CHECK(pf.has_value());
    if (pf) {
        CHECK(!pf->inlineLocals);
        CHECK(contains(pf->source, "float v0;"));
        CHECK(contains(pf->source, "void p"));
        CHECK(hasProcCallSite(pf->source));
    }
    setenv("XYO_GPU_INLINE_LOCALS", "1", 1);
    auto pi = gpu::compileRayKernel(bvh->blocks, false, &why);
    CHECK(pi.has_value());
    if (pi) {
        CHECK(pi->inlineLocals);
        CHECK(!contains(pi->source, "float v0;"));
        CHECK(contains(pi->source, "float v0 = pr["));
        CHECK(contains(pi->source, "void kernelBody() {\nfloat v0 = pr["));
        CHECK(!contains(pi->source, "\nv0 = pr["));
        CHECK(!contains(pi->source, "void p"));
        CHECK(!hasProcCallSite(pi->source));
        CHECK(contains(pi->source, "seed_ ^= seed_ << 13u"));
        CHECK(contains(pi->source, "uDispatchPixels"));
        CHECK(contains(pi->source, "if (err_ != 0u) return;"));
        CHECK(contains(pi->source, "atomicOr(ob[0], err_)"));
        CHECK(contains(pi->source, "cnts[pixel_]"));
        CHECK(contains(pi->source, "ob[kColOff + pixel_]"));
        CHECK(contains(pi->source, "#if XYO_NATIVE_CAPTURE"));
        dumpSrc(*pi, "fixture_inline");
    }
    setenv("XYO_GPU_INLINE_LOCALS", "yes", 1);
    auto pn = gpu::compileRayKernel(bvh->blocks, false, &why);
    CHECK(pn.has_value());
    if (pn) CHECK(!pn->inlineLocals);
    if (saved.empty()) unsetenv("XYO_GPU_INLINE_LOCALS");
    else setenv("XYO_GPU_INLINE_LOCALS", saved.c_str(), 1);
}

static void test_reject_chain_cycle() {
    Runtime rt;
    CHECK(loadFixture(rt));
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) return;
    Block* chx = bvh->blocks->getBlock("i_chx");
    CHECK(chx != nullptr);
    if (!chx) return;
    chx->next = "i_trace";
    std::string why;
    auto p = gpu::compileRayKernel(bvh->blocks, false, &why);
    CHECK(!p.has_value());
}

// ---------------------------------------------------------------- player --

static void test_player_init_patches() {
    Runtime rt;
    CHECK(loadFixture(rt));
    gpu::RealtimePlayer player;
    std::string why;
    CHECK(player.init(rt, gpu::RealtimeMode::Ray, &why));
    if (!why.empty()) std::printf("init why: %s\n", why.c_str());
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) return;
    Block* rr = bvh->blocks->getBlock("h_rr");
    Block* pf = bvh->blocks->getBlock("h_pf");
    Block* dd = bvh->blocks->getBlock("d_draw");
    Block* di = bvh->blocks->getBlock("d_if");
    CHECK(rr && rr->opcode == "xyo_internal_gpu_mode_ray");
    CHECK(pf && pf->opcode == "xyo_internal_gpu_mode_path");
    CHECK(dd && dd->opcode == "xyo_internal_gpu_frame");
    CHECK(di != nullptr);
    if (di) {
        const BlockInput* sub = di->inputs.find("SUBSTACK");
        CHECK(sub && sub->block.empty());
    }
    CHECK(rt.getOpcodeFunction("xyo_internal_gpu_frame") != nullptr);
    player.uninstall();
    CHECK(rr->opcode == "procedures_call");
    CHECK(pf->opcode == "control_forever");
    CHECK(dd->opcode == "procedures_call");
    if (di) {
        const BlockInput* sub = di->inputs.find("SUBSTACK");
        CHECK(sub && sub->block == "d_stop");
    }
    CHECK(rt.getOpcodeFunction("xyo_internal_gpu_frame") == nullptr);
    CHECK(!rt.onGpuFrame && !rt.onGpuMode && !rt.onGreenFlag);
}

static void test_player_init_reject_threads() {
    Runtime rt;
    CHECK(loadFixture(rt));
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) return;
    Thread* t = rt.pushThread("h_gf", bvh);
    CHECK(t != nullptr);
    gpu::RealtimePlayer player;
    std::string why;
    CHECK(!player.init(rt, gpu::RealtimeMode::Ray, &why));
    Block* dd = bvh->blocks->getBlock("d_draw");
    CHECK(dd && dd->opcode == "procedures_call");
    CHECK(rt.getOpcodeFunction("xyo_internal_gpu_frame") == nullptr);
    CHECK(!rt.onGpuFrame);
    rt.stopAll();
}

static void test_player_callback_chain() {
    Runtime rt;
    CHECK(loadFixture(rt));
    int flagCalls = 0;
    int modeCalls = 0;
    rt.onGreenFlag = [&]() { ++flagCalls; };
    rt.onGpuMode = [&](bool) { ++modeCalls; };
    {
        gpu::RealtimePlayer player;
        std::string why;
        CHECK(player.init(rt, gpu::RealtimeMode::Ray, &why));
        CHECK(rt.onGreenFlag != nullptr);
        rt.onGreenFlag();
        CHECK_EQ(flagCalls, 1);
        rt.onGpuMode(true);
        CHECK_EQ(modeCalls, 1);
    }
    CHECK_EQ(flagCalls, 1);
    rt.onGreenFlag();
    CHECK_EQ(flagCalls, 2);
    rt.onGpuMode(false);
    CHECK_EQ(modeCalls, 2);
}

static void test_player_cpu_fallback() {
    Runtime rt;
    CHECK(loadFixture(rt));
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) return;
    Variable* res = bvh->lookupVariableById("vres");
    CHECK(res != nullptr);
    if (!res) return;
    res->value = Value::num(120);   // 4x3 dispatch → fallback is cheap
    gpu::RealtimePlayer player;
    std::string why;
    CHECK(player.init(rt, gpu::RealtimeMode::Ray, &why));
    rt.greenFlag();
    int steps = rt.runUntilDone(20000);
    CHECK(player.completedFrames() >= 1);
    CHECK(!player.usingGpu());
    (void)steps;
}

// ------------------------------------------------------------------ live --

static bool liveGpu() { return std::getenv("XYO_TEST_GPU") != nullptr; }

static void test_live_ray() {
    if (!liveGpu()) { std::printf("SKIP live ray (XYO_TEST_GPU unset)\n"); return; }
    Runtime rt;
    CHECK(loadFixture(rt));
    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;
    gpu::RealtimePlayer player;
    std::string why;
    CHECK(player.init(rt, gpu::RealtimeMode::Ray, &why));
    if (!why.empty()) std::printf("init why: %s\n", why.c_str());
    rt.greenFlag();
    int captures = 0;
    bool saved = false;
    ren.setFrameCapture([&]() {
        ++captures;
        if (!saved && player.completedFrames() >= 2) {
            saved = true;
            ren.savePPM("/tmp/xyo_gpu_test_ray.ppm");
        }
    });
    for (int i = 0; i < 2400 && player.completedFrames() < 2; ++i) {
        rt.step();
        ren.draw();
    }
    CHECK(player.completedFrames() >= 2);
    CHECK(player.usingGpu());
    CHECK(captures > 0);
    CHECK(saved);
}

static void test_live_path() {
    if (!liveGpu()) { std::printf("SKIP live path (XYO_TEST_GPU unset)\n"); return; }
    Runtime rt;
    CHECK(loadFixture(rt));
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) return;
    const uint32_t total = 480 * 360;
    for (const char* id : {"lar", "lag", "lab"}) {
        Variable* v = bvh->lookupVariableById(id);
        CHECK(v != nullptr && v->list != nullptr);
        if (!v || !v->list) continue;
        v->list->ensure(total);
        while (v->list->len < total)
            v->list->items[v->list->len++] = Value::num(0);
    }
    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;
    gpu::RealtimePlayer player;
    std::string why;
    CHECK(player.init(rt, gpu::RealtimeMode::Path, &why));
    if (!why.empty()) std::printf("init why: %s\n", why.c_str());
    rt.greenFlag();
    int captures = 0;
    bool saved = false;
    ren.setFrameCapture([&]() {
        ++captures;
        if (!saved && player.samples() >= 2) {
            saved = true;
            ren.savePPM("/tmp/xyo_gpu_test_path.ppm");
        }
    });
    for (int i = 0; i < 2400 && player.samples() < 2; ++i) {
        rt.step();
        ren.draw();
    }
    CHECK(player.samples() >= 2);
    CHECK(player.usingGpu());
    Variable* lr = bvh->lookupVariableById("lar");
    CHECK(lr && lr->list && lr->list->len >= total);
    if (lr && lr->list && lr->list->len >= total)
        CHECK(cast::toNumber(lr->list->items[0]) >= 1.0);
}

static std::string readFileBytes(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return out;
}

static void test_live_stack_storage() {
    if (!liveGpu()) {
        std::printf("SKIP live stack storage (XYO_TEST_GPU unset)\n");
        return;
    }
    const char* prev = std::getenv("XYO_GPU_SHARED_STACK");
    std::string saved = prev ? prev : "";
    auto runPass = [&](bool shared, const char* ppmPath) {
        if (shared) setenv("XYO_GPU_SHARED_STACK", "1", 1);
        else unsetenv("XYO_GPU_SHARED_STACK");
        Runtime rt;
        CHECK(loadStackFixture(rt) != nullptr);
        Target* bvh = bvhTarget(rt);
        CHECK(bvh != nullptr);
        if (!bvh) return;
        std::string why;
        auto p = gpu::compileRayKernel(bvh->blocks, false, &why);
        CHECK(p.has_value());
        if (p) {
            CHECK(p->sharedStack == shared);
            CHECK(contains(p->source, "xspush_"));
            CHECK(contains(p->source, "xsget_"));
            CHECK(contains(p->source, "xsdel_"));
            dumpSrc(*p, shared ? "fixture_stack_shared"
                               : "fixture_stack_priv");
        }
        SdlRenderer ren;
        CHECK(ren.init(rt, 1));
        rt.renderer = &ren;
        gpu::RealtimePlayer player;
        CHECK(player.init(rt, gpu::RealtimeMode::Ray, &why));
        if (!why.empty()) std::printf("init why: %s\n", why.c_str());
        rt.greenFlag();
        bool savedF = false;
        ren.setFrameCapture([&]() {
            if (!savedF && player.completedFrames() >= 2) {
                savedF = true;
                ren.savePPM(ppmPath);
            }
        });
        for (int i = 0; i < 2400 && player.completedFrames() < 2; ++i) {
            rt.step();
            ren.draw();
        }
        CHECK(player.completedFrames() >= 2);
        CHECK(player.usingGpu());
        CHECK(savedF);
    };
    runPass(false, "/tmp/xyo_gpu_test_stack_priv.ppm");
    runPass(true, "/tmp/xyo_gpu_test_stack_shared.ppm");
    if (saved.empty()) unsetenv("XYO_GPU_SHARED_STACK");
    else setenv("XYO_GPU_SHARED_STACK", saved.c_str(), 1);
    std::string a = readFileBytes("/tmp/xyo_gpu_test_stack_priv.ppm");
    std::string b = readFileBytes("/tmp/xyo_gpu_test_stack_shared.ppm");
    CHECK(!a.empty());
    CHECK(!b.empty());
    CHECK_EQ(a.size(), b.size());
    CHECK(a == b);
    if (a == b) {
        size_t nonBg = 0;
        for (size_t i = 0; i + 2 < a.size(); ++i)
            if (uint8_t(a[i]) != 0 || uint8_t(a[i + 1]) != 0 ||
                uint8_t(a[i + 2]) != 0)
                ++nonBg;
        CHECK(nonBg > 0);
    }
}

static void test_live_inline_locals() {
    if (!liveGpu()) {
        std::printf("SKIP live inline locals (XYO_TEST_GPU unset)\n");
        return;
    }
    const char* prev = std::getenv("XYO_GPU_INLINE_LOCALS");
    std::string saved = prev ? prev : "";
    auto runPass = [&](bool inlineMode, const char* ppmPath) {
        if (inlineMode) setenv("XYO_GPU_INLINE_LOCALS", "1", 1);
        else unsetenv("XYO_GPU_INLINE_LOCALS");
        Runtime rt;
        CHECK(loadArgFixture(rt) != nullptr);
        Target* bvh = bvhTarget(rt);
        CHECK(bvh != nullptr);
        if (!bvh) return;
        std::string why;
        auto p = gpu::compileRayKernel(bvh->blocks, false, &why);
        CHECK(p.has_value());
        if (p) {
            CHECK(p->inlineLocals == inlineMode);
            CHECK(contains(p->source, "xspush_"));
            CHECK(contains(p->source, "xsget_"));
            CHECK(contains(p->source, "xsdel_"));
            dumpSrc(*p, inlineMode ? "fixture_inline_locals"
                                   : "fixture_fn_locals");
        }
        SdlRenderer ren;
        CHECK(ren.init(rt, 1));
        rt.renderer = &ren;
        gpu::RealtimePlayer player;
        CHECK(player.init(rt, gpu::RealtimeMode::Ray, &why));
        if (!why.empty()) std::printf("init why: %s\n", why.c_str());
        rt.greenFlag();
        bool savedF = false;
        ren.setFrameCapture([&]() {
            if (!savedF && player.completedFrames() >= 2) {
                savedF = true;
                ren.savePPM(ppmPath);
            }
        });
        for (int i = 0; i < 2400 && player.completedFrames() < 2; ++i) {
            rt.step();
            ren.draw();
        }
        CHECK(player.completedFrames() >= 2);
        CHECK(player.usingGpu());
        CHECK(savedF);
    };
    runPass(false, "/tmp/xyo_gpu_test_inline_fn.ppm");
    runPass(true, "/tmp/xyo_gpu_test_inline_in.ppm");
    if (saved.empty()) unsetenv("XYO_GPU_INLINE_LOCALS");
    else setenv("XYO_GPU_INLINE_LOCALS", saved.c_str(), 1);
    std::string a = readFileBytes("/tmp/xyo_gpu_test_inline_fn.ppm");
    std::string b = readFileBytes("/tmp/xyo_gpu_test_inline_in.ppm");
    CHECK(!a.empty());
    CHECK(!b.empty());
    CHECK_EQ(a.size(), b.size());
    CHECK(a == b);
    if (a == b) {
        size_t nonBg = 0;
        for (size_t i = 0; i + 2 < a.size(); ++i)
            if (uint8_t(a[i]) != 0 || uint8_t(a[i + 1]) != 0 ||
                uint8_t(a[i + 2]) != 0)
                ++nonBg;
        CHECK(nonBg > 0);
    }
}

static void test_live_gpu_off() {
    if (!liveGpu()) { std::printf("SKIP live XYO_GPU=0 (unset)\n"); return; }
    setenv("XYO_GPU", "0", 1);
    Runtime rt;
    CHECK(loadFixture(rt));
    Target* bvh = bvhTarget(rt);
    CHECK(bvh != nullptr);
    if (!bvh) { unsetenv("XYO_GPU"); return; }
    Variable* res = bvh->lookupVariableById("vres");
    CHECK(res != nullptr);
    if (res) res->value = Value::num(120);
    SdlRenderer ren;
    CHECK(ren.init(rt, 1));
    rt.renderer = &ren;
    gpu::RealtimePlayer player;
    std::string why;
    CHECK(player.init(rt, gpu::RealtimeMode::Ray, &why));
    rt.greenFlag();
    for (int i = 0; i < 6000 && player.completedFrames() < 1; ++i) {
        rt.step();
        ren.draw();
    }
    unsetenv("XYO_GPU");
    CHECK(player.completedFrames() >= 1);
    CHECK(!player.usingGpu());
}

// tests/gpu/test_parallel.cpp — VM バイトコード経路のテスト群
int test_parallel_gpu();
int test_vulkan_headless();

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--vulkan-headless") == 0)
        return test_vulkan_headless() ? 1 : 0;
    if (argc == 2 && std::strcmp(argv[1], "--parallel") == 0)
        return test_parallel_gpu() ? 1 : 0;
    test_compile_ray();
    test_compile_path();
    test_reject_no_proc();
    test_reject_dims();
    test_reject_stacklist();
    test_reject_recursion();
    test_reject_chain_cycle();
    test_stack_source_modes();
    test_inline_source();
    test_player_init_patches();
    test_player_init_reject_threads();
    test_player_callback_chain();
    test_player_cpu_fallback();
    test_live_ray();
    test_live_path();
    test_live_stack_storage();
    test_live_inline_locals();
    test_live_gpu_off();
    g_fail += test_parallel_gpu();
    g_fail += test_vulkan_headless();
    std::printf("gpu tests: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
