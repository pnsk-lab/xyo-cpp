// xyo-cpp: project.json → Runtime → interpreter の E2E テスト。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>

#include "core/cast.hpp"
#include "core/gc.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"
#if defined(XYO_JIT)
#include "jit/jit.hpp"

extern "C" void xyo_proc_push(xyo::JitCtx* c, const char* blockId,
                              const xyo::BlockCached* op);
extern "C" void xyo_proc_pop(xyo::JitCtx* c);
#endif

using namespace xyo;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; \
    if (!((a) == (b))) { ++g_fail; \
    std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

static Target* spriteOf(Runtime& rt, const char16_t* name) {
    for (Target* t : rt.targets)
        if (t->sprite && t->sprite->name == name) return t;
    return nullptr;
}

// 変数代入スクリプト: flag → set var = add(1, 2) * join ではなく素直に演算のみ
static const char* kProject1 = R"JSON({
  "targets": [
    {
      "isStage": true, "name": "Stage",
      "variables": {}, "lists": {}, "broadcasts": {},
      "blocks": {}, "comments": {}, "costumes": [], "sounds": [],
      "currentCostume": 0, "volume": 100, "layerOrder": 0
    },
    {
      "isStage": false, "name": "Sprite1",
      "variables": {"v1": ["score", 0], "v2": ["done", 0]},
      "lists": {"l1": ["mylist", ["a", "b"]]},
      "broadcasts": {"b1": "go"},
      "blocks": {
        "hat": {"opcode": "event_whenflagclicked", "next": "addvar",
                "parent": null, "inputs": {}, "fields": {},
                "topLevel": true, "shadow": false, "x": 0, "y": 0},
        "addvar": {"opcode": "data_changevariableby", "next": "setdone",
                "parent": "hat",
                "inputs": {"VALUE": [1, "mul"]},
                "fields": {"VARIABLE": ["score", "v1"]},
                "topLevel": false, "shadow": false},
        "mul": {"opcode": "operator_multiply", "next": null, "parent": "addvar",
                "inputs": {"NUM1": [1, [4, "6"]], "NUM2": [1, [4, "7"]]},
                "fields": {}, "topLevel": false, "shadow": false},
        "setdone": {"opcode": "data_setvariableto", "next": "ifcheck",
                "parent": "addvar",
                "inputs": {"VALUE": [1, [10, "yes"]]},
                "fields": {"VARIABLE": ["done", "v2"]},
                "topLevel": false, "shadow": false},
        "ifcheck": {"opcode": "control_if", "next": null, "parent": "setdone",
                "inputs": {"CONDITION": [1, "gt"], "SUBSTACK": [2, "pushitem"]},
                "fields": {}, "topLevel": false, "shadow": false},
        "gt": {"opcode": "operator_gt", "next": null, "parent": "ifcheck",
                "inputs": {"OPERAND1": [1, "var"], "OPERAND2": [1, [4, "40"]]},
                "fields": {}, "topLevel": false, "shadow": false},
        "var": {"opcode": "data_variable", "next": null, "parent": "gt",
                "inputs": {}, "fields": {"VARIABLE": ["score", "v1"]},
                "topLevel": false, "shadow": false},
        "pushitem": {"opcode": "data_addtolist", "next": null,
                "parent": "ifcheck",
                "inputs": {"ITEM": [1, "len"]},
                "fields": {"LIST": ["mylist", "l1"]},
                "topLevel": false, "shadow": false},
        "len": {"opcode": "operator_length", "next": null, "parent": "pushitem",
                "inputs": {"STRING": [1, [10, "abc"]]},
                "fields": {}, "topLevel": false, "shadow": false}
      },
      "costumes": [], "sounds": [],
      "currentCostume": 0, "x": 10, "y": -5, "size": 100,
      "direction": 90, "draggable": false,
      "rotationStyle": "all around", "visible": true,
      "layerOrder": 1, "volume": 100
    }
  ],
  "monitors": [], "extensions": [],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";

// repeat + broadcast テスト
static const char* kProject2 = R"JSON({
  "targets": [
    {
      "isStage": true, "name": "Stage",
      "variables": {"counter": ["counter", 0]},
      "lists": {},
      "broadcasts": {"b1": "go"},
      "blocks": {
        "recv": {"opcode": "event_whenbroadcastreceived", "next": "inc",
                "parent": null, "inputs": {},
                "fields": {"BROADCAST_OPTION": ["go", "b1"]},
                "topLevel": true, "shadow": false, "x": 0, "y": 0},
        "inc": {"opcode": "data_changevariableby", "next": null,
                "parent": "recv",
                "inputs": {"VALUE": [1, [4, "5"]]},
                "fields": {"VARIABLE": ["counter", "counter"]},
                "topLevel": false, "shadow": false}
      },
      "comments": {}, "costumes": [], "sounds": [],
      "currentCostume": 0, "volume": 100, "layerOrder": 0
    },
    {
      "isStage": false, "name": "Sprite1",
      "variables": {"i": ["i", 0]},
      "lists": {},
      "broadcasts": {},
      "blocks": {
        "hat": {"opcode": "event_whenflagclicked", "next": "rep",
                "parent": null, "inputs": {}, "fields": {},
                "topLevel": true, "shadow": false, "x": 0, "y": 0},
        "rep": {"opcode": "control_repeat", "next": "bc",
                "parent": "hat",
                "inputs": {"TIMES": [1, [4, "3"]], "SUBSTACK": [2, "inci"]},
                "fields": {}, "topLevel": false, "shadow": false},
        "inci": {"opcode": "data_changevariableby", "next": null,
                "parent": "rep",
                "inputs": {"VALUE": [1, [4, "1"]]},
                "fields": {"VARIABLE": ["i", "i"]},
                "topLevel": false, "shadow": false},
        "bc": {"opcode": "event_broadcast", "next": null, "parent": "rep",
                "inputs": {"BROADCAST_INPUT": [1, "bcmenu"]},
                "fields": {}, "topLevel": false, "shadow": false},
        "bcmenu": {"opcode": "event_broadcast_menu", "next": null,
                "parent": "bc", "inputs": {},
                "fields": {"BROADCAST_OPTION": ["go", "b1"]},
                "topLevel": false, "shadow": true}
      },
      "costumes": [], "sounds": [],
      "currentCostume": 0, "x": 0, "y": 0, "size": 100,
      "direction": 90, "draggable": false,
      "rotationStyle": "all around", "visible": true,
      "layerOrder": 1, "volume": 100
    }
  ],
  "monitors": [], "extensions": [],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";

static void test_load_and_run() {
    Runtime rt;
    Sb3LoadResult r = loadSb3(rt, kProject1, nullptr);
    CHECK(r.ok);
    if (!r.ok) { std::printf("load error: %s\n", r.error.c_str()); return; }
    CHECK_EQ(rt.targets.size(), 2u);
    Target* sp = spriteOf(rt, u"Sprite1");
    CHECK(sp != nullptr);
    if (!sp) return;
    CHECK_EQ(sp->x, 10.0);
    CHECK_EQ(sp->y, -5.0);
    Variable* score = sp->lookupVariableById("v1");
    CHECK(score != nullptr);
    CHECK_EQ(cast::toNumber(score->value), 0.0);

    rt.greenFlag();
    bool ran = rt.runUntilDone(100000);
    CHECK(ran);
    CHECK_EQ(cast::toNumber(score->value), 42.0);
    Variable* done = sp->lookupVariableById("v2");
    CHECK(done != nullptr);
    SStr* doneStr = cast::toString(done->value);
    CHECK_EQ(std::u16string(doneStr->view()), u"yes");

    // if (score > 40) → add len("abc")=3 to list ["a","b"] → ["a","b",3]
    Variable* list = sp->lookupVariableById("l1");
    CHECK(list != nullptr);
    if (list && list->list) {
        CHECK_EQ(list->list->len, 3u);
        if (list->list->len == 3)
            CHECK_EQ(cast::toNumber(list->list->items[2]), 3.0);
    }
}

static void test_repeat_broadcast() {
    Runtime rt;
    Sb3LoadResult r = loadSb3(rt, kProject2, nullptr);
    CHECK(r.ok);
    if (!r.ok) { std::printf("load error: %s\n", r.error.c_str()); return; }
    Target* stage = rt.getTargetForStage();
    Target* sp = spriteOf(rt, u"Sprite1");
    CHECK(stage != nullptr && sp != nullptr);
    if (!stage || !sp) return;

    rt.greenFlag();
    bool ran = rt.runUntilDone(100000);
    CHECK(ran);
    // repeat 3 × +1 → i = 3
    Variable* i = sp->lookupVariableById("i");
    CHECK(i != nullptr);
    CHECK_EQ(cast::toNumber(i->value), 3.0);
    // broadcast "go" → stage hat → counter += 5
    Variable* counter = stage->lookupVariableById("counter");
    CHECK(counter != nullptr);
    CHECK_EQ(cast::toNumber(counter->value), 5.0);
}

// 手続き: flag → call "double %1"(21) → i += arg
static const char* kProject3 = R"JSON({
  "targets": [
    {
      "isStage": true, "name": "Stage",
      "variables": {}, "lists": {}, "broadcasts": {},
      "blocks": {}, "comments": {}, "costumes": [], "sounds": [],
      "currentCostume": 0, "volume": 100, "layerOrder": 0
    },
    {
      "isStage": false, "name": "Sprite1",
      "variables": {"i": ["i", 0]},
      "lists": {}, "broadcasts": {},
      "blocks": {
        "hat": {"opcode": "event_whenflagclicked", "next": "call",
                "parent": null, "inputs": {}, "fields": {},
                "topLevel": true, "shadow": false, "x": 0, "y": 0},
        "call": {"opcode": "procedures_call", "next": null, "parent": "hat",
                "inputs": {"arg0": [1, [4, "21"]]},
                "fields": {}, "topLevel": false, "shadow": false,
                "mutation": {"tagName": "mutation", "children": [],
                  "proccode": "double %1",
                  "argumentids": "[\"arg0\"]",
                  "argumentnames": "[\"x\"]",
                  "argumentdefaults": "[\"\"]",
                  "warp": "false"}},
        "def": {"opcode": "procedures_definition", "next": "body",
                "parent": null,
                "inputs": {"custom_block": [1, "proto"]},
                "fields": {}, "topLevel": true, "shadow": false,
                "x": 100, "y": 100},
        "proto": {"opcode": "procedures_prototype", "next": null,
                "parent": "def",
                "inputs": {"arg0": [1, "argname"]},
                "fields": {}, "topLevel": false, "shadow": true,
                "mutation": {"tagName": "mutation", "children": [],
                  "proccode": "double %1",
                  "argumentids": "[\"arg0\"]",
                  "argumentnames": "[\"x\"]",
                  "argumentdefaults": "[\"\"]",
                  "warp": "false"}},
        "argname": {"opcode": "argument_reporter_string_number",
                "next": null, "parent": "proto", "inputs": {},
                "fields": {"VALUE": ["x", null]},
                "topLevel": false, "shadow": true},
        "body": {"opcode": "data_changevariableby", "next": null,
                "parent": "def",
                "inputs": {"VALUE": [1, "argrep"]},
                "fields": {"VARIABLE": ["i", "i"]},
                "topLevel": false, "shadow": false},
        "argrep": {"opcode": "argument_reporter_string_number",
                "next": null, "parent": "body", "inputs": {},
                "fields": {"VALUE": ["x", null]},
                "topLevel": false, "shadow": false}
      },
      "costumes": [], "sounds": [],
      "currentCostume": 0, "x": 0, "y": 0, "size": 100,
      "direction": 90, "draggable": false,
      "rotationStyle": "all around", "visible": true,
      "layerOrder": 1, "volume": 100
    }
  ],
  "monitors": [], "extensions": [],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";

static void test_procedure() {
    Runtime rt;
    Sb3LoadResult r = loadSb3(rt, kProject3, nullptr);
    CHECK(r.ok);
    if (!r.ok) { std::printf("load error: %s\n", r.error.c_str()); return; }
    Target* sp = spriteOf(rt, u"Sprite1");
    CHECK(sp != nullptr);
    if (!sp) return;
    rt.greenFlag();
    bool ran = rt.runUntilDone(100000);
    CHECK(ran);
    Variable* i = sp->lookupVariableById("i");
    CHECK(i != nullptr);
    CHECK_EQ(cast::toNumber(i->value), 21.0);
}

// GC 回帰: ロード中に collect が走っても、まだ runtime.targets 未登録の
// Target 内にある SStr/SList が解放されないこと（3D2.sb3 クラッシュの
// 最小再現）。threshold=1 で全割当が collect を誘発する。
static const char* kProjectGc = R"JSON({
  "targets": [
    {
      "isStage": true, "name": "Stage",
      "variables": {}, "lists": {}, "broadcasts": {},
      "blocks": {}, "comments": {}, "costumes": [], "sounds": [],
      "currentCostume": 0, "volume": 100, "layerOrder": 0
    },
    {
      "isStage": false, "name": "Sprite1",
      "variables": {"sv": ["svar", "hello"], "sv2": ["svar2", "world"]},
      "lists": {"aa_l": ["alist", ["a", "b", "c"]],
                "zz_l": ["zlist", ["x", "y", "z"]]},
      "broadcasts": {},
      "blocks": {
        "proto": {"opcode": "procedures_prototype", "next": null,
                "parent": null, "inputs": {}, "fields": {},
                "topLevel": false, "shadow": true,
                "mutation": {"tagName": "mutation", "children": [],
                  "proccode": "double %1",
                  "argumentids": "[\"arg0\"]",
                  "argumentnames": "[\"x\"]",
                  "argumentdefaults": "[\"\"]",
                  "warp": "false"}}
      },
      "costumes": [], "sounds": [],
      "currentCostume": 0, "x": 0, "y": 0, "size": 100,
      "direction": 90, "draggable": false,
      "rotationStyle": "all around", "visible": true,
      "layerOrder": 1, "volume": 100
    }
  ],
  "monitors": [], "extensions": [],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";

static void test_load_gc_pressure() {
    Runtime rt;
    Gc& gc = Gc::get();
    const size_t n0 = gc.collectCount();
    gc.setThreshold(1);   // 全割当で collect 誘発（ロード中も）
    Sb3LoadResult r = loadSb3(rt, kProjectGc, nullptr);
    // DeferCollect の契約: ロード区間では一度も collect しない。
    // 無 defer では未登録 Target 内の SStr/SList が途中で解放される。
    CHECK_EQ(gc.collectCount(), n0);
    // 登録済みブロックの markBlocks → Mutation::parseArgs → SStr::create
    // の再入割当経路（旧実装では allocMtx_ でデッドロック）もここで通す。
    gc.collect();
    gc.setThreshold(2 << 20);
    CHECK(r.ok);
    if (!r.ok) { std::printf("load error: %s\n", r.error.c_str()); return; }
    Target* sp = spriteOf(rt, u"Sprite1");
    CHECK(sp != nullptr);
    if (!sp) return;
    Variable* sv = sp->lookupVariableById("sv");
    CHECK(sv != nullptr);
    if (sv) {
        SStr* s = cast::toString(sv->value);
        CHECK_EQ(std::u16string(s->view()), u"hello");
    }
    Variable* sv2 = sp->lookupVariableById("sv2");
    CHECK(sv2 != nullptr);
    if (sv2) {
        SStr* s = cast::toString(sv2->value);
        CHECK_EQ(std::u16string(s->view()), u"world");
    }
    // aa_l は zz_l のロードでローカル Variable のスタックスロットが
    // 上書きされた後に collect が走る — DeferCollect 無しでは解放される。
    Variable* sl = sp->lookupVariableById("aa_l");
    CHECK(sl != nullptr && sl->list != nullptr);
    if (sl && sl->list) {
        CHECK_EQ(sl->list->len, 3u);
        if (sl->list->len == 3) {
            SStr* s2 = cast::toString(sl->list->items[2]);
            CHECK_EQ(std::u16string(s2->view()), u"c");
        }
    }
    Variable* sz = sp->lookupVariableById("zz_l");
    CHECK(sz != nullptr && sz->list != nullptr && sz->list->len == 3);
    // ロード後の collect でも壊れないこと（生存確認）
    gc.collect();
    Variable* sl2 = sp->lookupVariableById("aa_l");
    CHECK(sl2 != nullptr && sl2->list != nullptr && sl2->list->len == 3);
}

// DeferCollect の機構自体の検証: defer 中は閾値超過でも collect せず、
// 解除後に回収が動くこと。
[[gnu::noinline]] static void clobberStack() {
    volatile char buf[4096];
    for (int i = 0; i < 4096; ++i) buf[i] = char(i);
}
static void test_defer_collect() {
    Gc& g = Gc::get();
    g.collect();
    const size_t n0 = g.collectCount();
    const size_t base = g.objectCount();
    {
        Gc::DeferCollect d;
        g.setThreshold(1);
        for (int i = 0; i < 64; ++i)
            SStr::create(std::u16string(u"defer_tmp"));  // 参照を残さず生成
        g.setThreshold(2 << 20);
        CHECK_EQ(g.collectCount(), n0);
        CHECK(g.objectCount() >= base + 64);
    }
    // 保守スキャンが拾うスタック残滓を掃除してから collect
    clobberStack();
    g.collect();
    CHECK_EQ(g.collectCount(), n0 + 1);
    CHECK(g.objectCount() < base + 64);
}

// tests/fixtures/basic.sb3 — zip ロード経路の検証
static void test_sb3_file() {
    // __FILE__ = .../tests/unit/test_e2e.cpp → ../../fixtures/basic.sb3
    std::string path = __FILE__;
    auto pos = path.find_last_of("/\\");
    path = path.substr(0, pos) + "/../fixtures/basic.sb3";
    std::ifstream f(path, std::ios::binary);
    CHECK(f.good());
    if (!f.good()) return;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    Runtime rt;
    Sb3LoadResult r = loadSb3File(rt, data.data(), data.size());
    CHECK(r.ok);
    if (!r.ok) { std::printf("sb3 err: %s\n", r.error.c_str()); return; }
    Target* sp = spriteOf(rt, u"Sprite1");
    CHECK(sp != nullptr);
    if (!sp) return;
    // 変数参照は stage の変数テーブルにフォールスルーする
    Variable* hv = rt.getTargetForStage()->lookupVariableById("hv");
    CHECK(hv != nullptr);

    rt.greenFlag();
    bool ran = rt.runUntilDone(100000);
    CHECK(ran);
    // looks_say → join(hello, " world") → bubble "none world"
    CHECK_EQ(std::u16string(sp->bubbleText), u"none world");
    // move 10 steps dir 90 → x=10
    CHECK_EQ(sp->x, 10.0);
}

static const char* kProjectNaN = R"JSON({
  "targets": [
    {"isStage": true, "name": "Stage",
     "variables": {}, "lists": {}, "broadcasts": {}, "blocks": {},
     "comments": {}, "costumes": [], "sounds": [], "currentCostume": 0,
     "volume": 100, "layerOrder": 0},
    {"isStage": false, "name": "Sprite1",
     "variables": {"nanv": ["nanv", 0], "t": ["t", 0], "sv": ["sv", 0],
       "r1": ["r1", 0], "r2": ["r2", 0], "r3": ["r3", 0], "r4": ["r4", 0],
       "r5": ["r5", 0], "r6": ["r6", 0], "r7": ["r7", 0], "r8": ["r8", 0],
       "r8n": ["r8n", 0], "r9": ["r9", 0], "r10": ["r10", 0],
       "r11": ["r11", 0], "r12": ["r12", 0], "r13": ["r13", 0]},
     "lists": {}, "broadcasts": {},
     "blocks": {
       "hat": {"opcode": "event_whenflagclicked", "next": "s_nan",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 0},
       "s_nan": {"opcode": "data_setvariableto", "next": "s_r1",
                 "parent": "hat", "inputs": {"VALUE": [1, "d00"]},
                 "fields": {"VARIABLE": ["nanv", "nanv"]},
                 "topLevel": false, "shadow": false},
       "d00": {"opcode": "operator_divide", "next": null, "parent": "s_nan",
               "inputs": {"NUM1": [1, [4, "0"]], "NUM2": [1, [4, "0"]]},
               "fields": {}, "topLevel": false, "shadow": false},
       "s_r1": {"opcode": "data_setvariableto", "next": "s_r2",
                "parent": "s_nan", "inputs": {"VALUE": [1, "a1"]},
                "fields": {"VARIABLE": ["r1", "r1"]},
                "topLevel": false, "shadow": false},
       "a1": {"opcode": "operator_add", "next": null, "parent": "s_r1",
              "inputs": {"NUM1": [1, "d01"], "NUM2": [1, [4, "1"]]},
              "fields": {}, "topLevel": false, "shadow": false},
       "d01": {"opcode": "operator_divide", "next": null, "parent": "a1",
               "inputs": {"NUM1": [1, [4, "0"]], "NUM2": [1, [4, "0"]]},
               "fields": {}, "topLevel": false, "shadow": false},
       "s_r2": {"opcode": "data_setvariableto", "next": "s_r3",
                "parent": "s_r1", "inputs": {"VALUE": [1, "m2"]},
                "fields": {"VARIABLE": ["r2", "r2"]},
                "topLevel": false, "shadow": false},
       "m2": {"opcode": "operator_multiply", "next": null, "parent": "s_r2",
              "inputs": {"NUM1": [1, "d02"], "NUM2": [1, [4, "240"]]},
              "fields": {}, "topLevel": false, "shadow": false},
       "d02": {"opcode": "operator_divide", "next": null, "parent": "m2",
               "inputs": {"NUM1": [1, [4, "0"]], "NUM2": [1, [4, "0"]]},
               "fields": {}, "topLevel": false, "shadow": false},
       "s_r3": {"opcode": "data_setvariableto", "next": "s_r4",
                "parent": "s_r2", "inputs": {"VALUE": [1, "a3"]},
                "fields": {"VARIABLE": ["r3", "r3"]},
                "topLevel": false, "shadow": false},
       "a3": {"opcode": "operator_add", "next": null, "parent": "s_r3",
              "inputs": {"NUM1": [1, "v_n"], "NUM2": [1, [4, "1"]]},
              "fields": {}, "topLevel": false, "shadow": false},
       "v_n": {"opcode": "data_variable", "next": null, "parent": "a3",
               "inputs": {}, "fields": {"VARIABLE": ["nanv", "nanv"]},
               "topLevel": false, "shadow": false},
       "s_r4": {"opcode": "data_setvariableto", "next": "s_cx",
                "parent": "s_r3", "inputs": {"VALUE": [1, "m4"]},
                "fields": {"VARIABLE": ["r4", "r4"]},
                "topLevel": false, "shadow": false},
       "m4": {"opcode": "operator_mathop", "next": null, "parent": "s_r4",
              "inputs": {"NUM": [1, "v_n2"]},
              "fields": {"OPERATOR": ["abs", null]},
              "topLevel": false, "shadow": false},
       "v_n2": {"opcode": "data_variable", "next": null, "parent": "m4",
                "inputs": {}, "fields": {"VARIABLE": ["nanv", "nanv"]},
                "topLevel": false, "shadow": false},
       "s_cx": {"opcode": "motion_changexby", "next": "s_r5",
                "parent": "s_r4", "inputs": {"DX": [1, "v_n3"]},
                "fields": {}, "topLevel": false, "shadow": false},
       "v_n3": {"opcode": "data_variable", "next": null, "parent": "s_cx",
                "inputs": {}, "fields": {"VARIABLE": ["nanv", "nanv"]},
                "topLevel": false, "shadow": false},
       "s_r5": {"opcode": "data_setvariableto", "next": "s_r6",
                "parent": "s_cx", "inputs": {"VALUE": [1, "a5"]},
                "fields": {"VARIABLE": ["r5", "r5"]},
                "topLevel": false, "shadow": false},
       "a5": {"opcode": "operator_add", "next": null, "parent": "s_r5",
              "inputs": {"NUM1": [1, "sq"], "NUM2": [1, [4, "1"]]},
              "fields": {}, "topLevel": false, "shadow": false},
       "sq": {"opcode": "operator_mathop", "next": null, "parent": "a5",
              "inputs": {"NUM": [1, [4, "-1"]]},
              "fields": {"OPERATOR": ["sqrt", null]},
              "topLevel": false, "shadow": false},
       "s_r6": {"opcode": "data_setvariableto", "next": "s_r7",
                "parent": "s_r5", "inputs": {"VALUE": [1, "a6"]},
                "fields": {"VARIABLE": ["r6", "r6"]},
                "topLevel": false, "shadow": false},
       "a6": {"opcode": "operator_add", "next": null, "parent": "s_r6",
              "inputs": {"NUM1": [1, "m6"], "NUM2": [1, [4, "1"]]},
              "fields": {}, "topLevel": false, "shadow": false},
       "m6": {"opcode": "operator_multiply", "next": null, "parent": "a6",
              "inputs": {"NUM1": [1, "d10"], "NUM2": [1, [4, "0"]]},
              "fields": {}, "topLevel": false, "shadow": false},
       "d10": {"opcode": "operator_divide", "next": null, "parent": "m6",
               "inputs": {"NUM1": [1, [4, "1"]], "NUM2": [1, [4, "0"]]},
               "fields": {}, "topLevel": false, "shadow": false},
       "s_r7": {"opcode": "data_setvariableto", "next": "s_r8",
                "parent": "s_r6", "inputs": {"VALUE": [1, "m7"]},
                "fields": {"VARIABLE": ["r7", "r7"]},
                "topLevel": false, "shadow": false},
       "m7": {"opcode": "operator_multiply", "next": null, "parent": "s_r7",
              "inputs": {"NUM1": [1, "d11"], "NUM2": [1, [4, "0"]]},
              "fields": {}, "topLevel": false, "shadow": false},
       "d11": {"opcode": "operator_divide", "next": null, "parent": "m7",
               "inputs": {"NUM1": [1, [4, "1"]], "NUM2": [1, [4, "0"]]},
               "fields": {}, "topLevel": false, "shadow": false},
       "s_r8": {"opcode": "data_setvariableto", "next": "s_r8n",
                "parent": "s_r7", "inputs": {"VALUE": [1, "a8"]},
                "fields": {"VARIABLE": ["r8", "r8"]},
                "topLevel": false, "shadow": false},
       "a8": {"opcode": "operator_add", "next": null, "parent": "s_r8",
              "inputs": {"NUM1": [1, "d12"], "NUM2": [1, [4, "1"]]},
              "fields": {}, "topLevel": false, "shadow": false},
       "d12": {"opcode": "operator_divide", "next": null, "parent": "a8",
               "inputs": {"NUM1": [1, [4, "1"]], "NUM2": [1, [4, "0"]]},
               "fields": {}, "topLevel": false, "shadow": false},
       "s_r8n": {"opcode": "data_setvariableto", "next": "s_r9",
                 "parent": "s_r8", "inputs": {"VALUE": [1, "m8n"]},
                 "fields": {"VARIABLE": ["r8n", "r8n"]},
                 "topLevel": false, "shadow": false},
       "m8n": {"opcode": "operator_multiply", "next": null,
               "parent": "s_r8n",
               "inputs": {"NUM1": [1, "d13"], "NUM2": [1, [4, "1"]]},
               "fields": {}, "topLevel": false, "shadow": false},
       "d13": {"opcode": "operator_divide", "next": null, "parent": "m8n",
               "inputs": {"NUM1": [1, [4, "-1"]], "NUM2": [1, [4, "0"]]},
               "fields": {}, "topLevel": false, "shadow": false},
       "s_r9": {"opcode": "data_setvariableto", "next": "s_r10",
                "parent": "s_r8n", "inputs": {"VALUE": [1, "d14"]},
                "fields": {"VARIABLE": ["r9", "r9"]},
                "topLevel": false, "shadow": false},
       "d14": {"opcode": "operator_divide", "next": null, "parent": "s_r9",
               "inputs": {"NUM1": [1, [4, "1"]], "NUM2": [1, "m9"]},
               "fields": {}, "topLevel": false, "shadow": false},
       "m9": {"opcode": "operator_multiply", "next": null, "parent": "d14",
              "inputs": {"NUM1": [1, [4, "0"]], "NUM2": [1, [4, "-1"]]},
              "fields": {}, "topLevel": false, "shadow": false},
       "s_r10": {"opcode": "data_setvariableto", "next": "s_sv",
                 "parent": "s_r9", "inputs": {"VALUE": [1, "cm"]},
                 "fields": {"VARIABLE": ["r10", "r10"]},
                 "topLevel": false, "shadow": false},
       "cm": {"opcode": "operator_multiply", "next": null, "parent": "s_r10",
              "inputs": {"NUM1": [1, "cd"], "NUM2": [1, "cs"]},
              "fields": {}, "topLevel": false, "shadow": false},
       "cd": {"opcode": "operator_divide", "next": null, "parent": "cm",
              "inputs": {"NUM1": [1, "vt1"], "NUM2": [1, "ca1"]},
              "fields": {}, "topLevel": false, "shadow": false},
       "vt1": {"opcode": "data_variable", "next": null, "parent": "cd",
               "inputs": {}, "fields": {"VARIABLE": ["t", "t"]},
               "topLevel": false, "shadow": false},
       "ca1": {"opcode": "operator_mathop", "next": null, "parent": "cd",
               "inputs": {"NUM": [1, "vt2"]},
               "fields": {"OPERATOR": ["abs", null]},
               "topLevel": false, "shadow": false},
       "vt2": {"opcode": "data_variable", "next": null, "parent": "ca1",
               "inputs": {}, "fields": {"VARIABLE": ["t", "t"]},
               "topLevel": false, "shadow": false},
       "cs": {"opcode": "operator_subtract", "next": null, "parent": "cm",
              "inputs": {"NUM1": [1, [4, "240"]], "NUM2": [1, "cm2"]},
              "fields": {}, "topLevel": false, "shadow": false},
       "cm2": {"opcode": "operator_multiply", "next": null, "parent": "cs",
               "inputs": {"NUM1": [1, "cs2"], "NUM2": [1, "cg"]},
               "fields": {}, "topLevel": false, "shadow": false},
       "cs2": {"opcode": "operator_subtract", "next": null, "parent": "cm2",
               "inputs": {"NUM1": [1, [4, "240"]], "NUM2": [1, "ca2"]},
               "fields": {}, "topLevel": false, "shadow": false},
       "ca2": {"opcode": "operator_mathop", "next": null, "parent": "cs2",
               "inputs": {"NUM": [1, "vt3"]},
               "fields": {"OPERATOR": ["abs", null]},
               "topLevel": false, "shadow": false},
       "vt3": {"opcode": "data_variable", "next": null, "parent": "ca2",
               "inputs": {}, "fields": {"VARIABLE": ["t", "t"]},
               "topLevel": false, "shadow": false},
       "cg": {"opcode": "operator_gt", "next": null, "parent": "cm2",
              "inputs": {"OPERAND1": [1, [4, "240"]], "OPERAND2": [1, "ca3"]},
              "fields": {}, "topLevel": false, "shadow": false},
       "ca3": {"opcode": "operator_mathop", "next": null, "parent": "cg",
               "inputs": {"NUM": [1, "vt4"]},
               "fields": {"OPERATOR": ["abs", null]},
               "topLevel": false, "shadow": false},
       "vt4": {"opcode": "data_variable", "next": null, "parent": "ca3",
               "inputs": {}, "fields": {"VARIABLE": ["t", "t"]},
               "topLevel": false, "shadow": false},
       "s_sv": {"opcode": "data_setvariableto", "next": "s_r13",
                "parent": "s_r10", "inputs": {"VALUE": [1, [10, "12"]]},
                "fields": {"VARIABLE": ["sv", "sv"]},
                "topLevel": false, "shadow": false},
       "s_r13": {"opcode": "data_setvariableto", "next": "s_r11",
                 "parent": "s_sv", "inputs": {"VALUE": [1, "a13"]},
                 "fields": {"VARIABLE": ["r13", "r13"]},
                 "topLevel": false, "shadow": false},
       "a13": {"opcode": "operator_add", "next": null, "parent": "s_r13",
               "inputs": {"NUM1": [1, "v_s1"], "NUM2": [1, "v_s2"]},
               "fields": {}, "topLevel": false, "shadow": false},
       "v_s1": {"opcode": "data_variable", "next": null, "parent": "a13",
                "inputs": {}, "fields": {"VARIABLE": ["sv", "sv"]},
                "topLevel": false, "shadow": false},
       "v_s2": {"opcode": "data_variable", "next": null, "parent": "a13",
                "inputs": {}, "fields": {"VARIABLE": ["sv", "sv"]},
                "topLevel": false, "shadow": false},
       "s_r11": {"opcode": "data_setvariableto", "next": "s_r12",
                 "parent": "s_r13", "inputs": {"VALUE": [1, "a11"]},
                 "fields": {"VARIABLE": ["r11", "r11"]},
                 "topLevel": false, "shadow": false},
       "a11": {"opcode": "operator_add", "next": null, "parent": "s_r11",
               "inputs": {"NUM1": [1, [10, "12"]], "NUM2": [1, "g1"]},
               "fields": {}, "topLevel": false, "shadow": false},
       "g1": {"opcode": "operator_gt", "next": null, "parent": "a11",
              "inputs": {"OPERAND1": [1, [4, "1"]], "OPERAND2": [1, [4, "0"]]},
              "fields": {}, "topLevel": false, "shadow": false},
       "s_r12": {"opcode": "data_setvariableto", "next": null,
                 "parent": "s_r11", "inputs": {"VALUE": [1, "a12"]},
                 "fields": {"VARIABLE": ["r12", "r12"]},
                 "topLevel": false, "shadow": false},
       "a12": {"opcode": "operator_add", "next": null, "parent": "s_r12",
               "inputs": {"NUM1": [1, [10, "3.5"]], "NUM2": [1, [10, "0.5"]]},
               "fields": {}, "topLevel": false, "shadow": false}
     },
     "costumes": [], "sounds": [],
     "currentCostume": 0, "x": 10, "y": 0, "size": 100,
     "direction": 90, "draggable": false,
     "rotationStyle": "all around", "visible": true,
     "layerOrder": 1, "volume": 100}
  ],
  "monitors": [], "extensions": [],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";

static void test_nan_operand_coercion() {
    Runtime rt;
    Sb3LoadResult r = loadSb3(rt, kProjectNaN, nullptr);
    CHECK(r.ok);
    if (!r.ok) { std::printf("load error: %s\n", r.error.c_str()); return; }
    Target* sp = spriteOf(rt, u"Sprite1");
    CHECK(sp != nullptr);
    if (!sp) return;
    auto num = [&](const char* id) {
        Variable* v = sp->lookupVariableById(id);
        return v ? v->value.asNumberUnchecked() : -999.0;
    };

    rt.greenFlag();
    const bool jitExpected =
        rt.jit != nullptr && std::getenv("XYO_JIT_DISABLE") == nullptr;
    CHECK_EQ(rt.threads.size(), 1u);
    if (jitExpected)
        for (Thread* t : rt.threads) CHECK(t->isCompiled);
    bool ran = rt.runUntilDone(100000);
    CHECK(ran);

    Variable* nanv = sp->lookupVariableById("nanv");
    CHECK(nanv != nullptr && nanv->value.isNumber() &&
          std::isnan(nanv->value.asNumberUnchecked()));
    if (jitExpected && nanv) CHECK(nanv->value.isNaN());
    CHECK_EQ(sp->x, 10.0);
    CHECK_EQ(num("r1"), 1.0);
    CHECK_EQ(num("r2"), 0.0);
    CHECK_EQ(num("r3"), 1.0);
    CHECK_EQ(num("r4"), 0.0);
    CHECK_EQ(num("r5"), 1.0);
    CHECK_EQ(num("r6"), 1.0);
    Variable* r7 = sp->lookupVariableById("r7");
    CHECK(r7 != nullptr && r7->value.isNumber() &&
          std::isnan(r7->value.asNumberUnchecked()));
    if (jitExpected && r7) CHECK(r7->value.isNaN());
    CHECK(std::isinf(num("r8")) && !std::signbit(num("r8")));
    CHECK(std::isinf(num("r8n")) && std::signbit(num("r8n")));
    CHECK(std::isinf(num("r9")) && std::signbit(num("r9")));
    CHECK_EQ(num("r10"), 0.0);
    CHECK_EQ(num("r11"), 13.0);
    CHECK_EQ(num("r12"), 4.0);
    CHECK_EQ(num("r13"), 24.0);
}

static const char* kProjectAllocaWarp = R"JSON({
  "targets": [
    {"isStage": true, "name": "Stage",
     "variables": {}, "lists": {}, "broadcasts": {}, "blocks": {},
     "comments": {}, "costumes": [], "sounds": [], "currentCostume": 0,
     "volume": 100, "layerOrder": 0},
    {"isStage": false, "name": "Sprite1",
     "variables": {"sum": ["sum", 0], "sum2": ["sum2", 0],
                   "i": ["i", 0]},
     "lists": {}, "broadcasts": {},
     "blocks": {
       "hat": {"opcode": "event_whenflagclicked", "next": "callo",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 0},
       "callo": {"opcode": "procedures_call", "next": "nrep",
                 "parent": "hat", "inputs": {}, "fields": {},
                 "topLevel": false, "shadow": false,
                 "mutation": {"tagName": "mutation", "children": [],
                     "proccode": "outer", "argumentids": "[]",
                     "warp": "true"}},
       "odef": {"opcode": "procedures_definition", "next": "orep",
                "parent": null, "inputs": {"custom_block": [1, "oproto"]},
                "fields": {}, "topLevel": true, "shadow": false,
                "x": 0, "y": 0},
       "oproto": {"opcode": "procedures_prototype", "next": null,
                  "parent": "odef", "inputs": {}, "fields": {},
                  "topLevel": false, "shadow": false,
                  "mutation": {"tagName": "mutation", "children": [],
                      "proccode": "outer", "argumentids": "[]",
                      "argumentnames": "[]", "argumentdefaults": "[]",
                      "warp": "true"}},
       "orep": {"opcode": "control_repeat", "next": null,
                "parent": "odef",
                "inputs": {"TIMES": [1, [4, "100000"]],
                           "SUBSTACK": [2, "calli"]},
                "fields": {}, "topLevel": false, "shadow": false},
       "calli": {"opcode": "procedures_call", "next": null,
                 "parent": "orep",
                 "inputs": {"a1": [1, [4, "1"]], "a2": [1, [4, "2"]],
                            "a3": [1, [4, "3"]]},
                 "fields": {}, "topLevel": false, "shadow": false,
                 "mutation": {"tagName": "mutation", "children": [],
                     "proccode": "sum3 %n %n %n",
                     "argumentids": "[\"a1\",\"a2\",\"a3\"]",
                     "warp": "true"}},
       "idef": {"opcode": "procedures_definition", "next": "ichg",
                "parent": null, "inputs": {"custom_block": [1, "iproto"]},
                "fields": {}, "topLevel": true, "shadow": false,
                "x": 0, "y": 0},
       "iproto": {"opcode": "procedures_prototype", "next": null,
                  "parent": "idef", "inputs": {}, "fields": {},
                  "topLevel": false, "shadow": false,
                  "mutation": {"tagName": "mutation", "children": [],
                      "proccode": "sum3 %n %n %n",
                      "argumentids": "[\"a1\",\"a2\",\"a3\"]",
                      "argumentnames": "[\"x\",\"y\",\"z\"]",
                      "argumentdefaults": "[\"0\",\"0\",\"0\"]",
                      "warp": "true"}},
       "ichg": {"opcode": "data_changevariableby", "next": null,
                "parent": "idef", "inputs": {"VALUE": [1, "iadd"]},
                "fields": {"VARIABLE": ["sum", "sum"]},
                "topLevel": false, "shadow": false},
       "iadd": {"opcode": "operator_add", "next": null, "parent": "ichg",
                "inputs": {"NUM1": [1, "iadd2"], "NUM2": [1, "rep3"]},
                "fields": {}, "topLevel": false, "shadow": false},
       "iadd2": {"opcode": "operator_add", "next": null, "parent": "iadd",
                 "inputs": {"NUM1": [1, "rep1"], "NUM2": [1, "rep2"]},
                 "fields": {}, "topLevel": false, "shadow": false},
       "rep1": {"opcode": "argument_reporter_string_number", "next": null,
                "parent": "iadd2", "inputs": {},
                "fields": {"VALUE": ["x", null]},
                "topLevel": false, "shadow": false},
       "rep2": {"opcode": "argument_reporter_string_number", "next": null,
                "parent": "iadd2", "inputs": {},
                "fields": {"VALUE": ["y", null]},
                "topLevel": false, "shadow": false},
       "rep3": {"opcode": "argument_reporter_string_number", "next": null,
                "parent": "iadd", "inputs": {},
                "fields": {"VALUE": ["z", null]},
                "topLevel": false, "shadow": false},
       "nrep": {"opcode": "control_repeat", "next": null,
                "parent": "callo",
                "inputs": {"TIMES": [1, [4, "50000"]],
                           "SUBSTACK": [2, "nfe"]},
                "fields": {}, "topLevel": false, "shadow": false},
       "nfe": {"opcode": "control_for_each", "next": null,
               "parent": "nrep",
               "inputs": {"VALUE": [1, [4, "2"]],
                          "SUBSTACK": [2, "nchg"]},
               "fields": {"VARIABLE": ["i", "i"]},
               "topLevel": false, "shadow": false},
       "nchg": {"opcode": "data_changevariableby", "next": null,
                "parent": "nfe", "inputs": {"VALUE": [1, [4, "1"]]},
                "fields": {"VARIABLE": ["sum2", "sum2"]},
                "topLevel": false, "shadow": false}
     },
     "costumes": [], "sounds": [],
     "currentCostume": 0, "x": 0, "y": 0, "size": 100,
     "direction": 90, "draggable": false,
     "rotationStyle": "all around", "visible": true,
     "layerOrder": 1, "volume": 100}
  ],
  "monitors": [], "extensions": [],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";

static const char* kProjectAllocaNonwarp = R"JSON({
  "targets": [
    {"isStage": true, "name": "Stage",
     "variables": {}, "lists": {}, "broadcasts": {}, "blocks": {},
     "comments": {}, "costumes": [], "sounds": [], "currentCostume": 0,
     "volume": 100, "layerOrder": 0},
    {"isStage": false, "name": "Sprite1",
     "variables": {"sum": ["sum", 0]},
     "lists": {}, "broadcasts": {},
     "blocks": {
       "hat": {"opcode": "event_whenflagclicked", "next": "crep",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 0},
       "crep": {"opcode": "control_repeat", "next": null,
                "parent": "hat",
                "inputs": {"TIMES": [1, [4, "50000"]],
                           "SUBSTACK": [2, "calle"]},
                "fields": {}, "topLevel": false, "shadow": false},
       "calle": {"opcode": "procedures_call", "next": null,
                 "parent": "crep",
                 "inputs": {"a1": [1, [4, "1"]], "a2": [1, [4, "2"]],
                            "a3": [1, [4, "3"]]},
                 "fields": {}, "topLevel": false, "shadow": false,
                 "mutation": {"tagName": "mutation", "children": [],
                     "proccode": "sum3n %n %n %n",
                     "argumentids": "[\"a1\",\"a2\",\"a3\"]",
                     "warp": "false"}},
       "cdef": {"opcode": "procedures_definition", "next": "cwait",
                "parent": null, "inputs": {"custom_block": [1, "cproto"]},
                "fields": {}, "topLevel": true, "shadow": false,
                "x": 0, "y": 0},
       "cwait": {"opcode": "control_wait", "next": "cchg",
                 "parent": "cdef",
                 "inputs": {"DURATION": [1, [4, "0"]]},
                 "fields": {}, "topLevel": false, "shadow": false},
       "cproto": {"opcode": "procedures_prototype", "next": null,
                  "parent": "cdef", "inputs": {}, "fields": {},
                  "topLevel": false, "shadow": false,
                  "mutation": {"tagName": "mutation", "children": [],
                      "proccode": "sum3n %n %n %n",
                      "argumentids": "[\"a1\",\"a2\",\"a3\"]",
                      "argumentnames": "[\"x\",\"y\",\"z\"]",
                      "argumentdefaults": "[\"0\",\"0\",\"0\"]",
                      "warp": "false"}},
       "cchg": {"opcode": "data_changevariableby", "next": null,
                "parent": "cwait", "inputs": {"VALUE": [1, "cadd"]},
                "fields": {"VARIABLE": ["sum", "sum"]},
                "topLevel": false, "shadow": false},
       "cadd": {"opcode": "operator_add", "next": null, "parent": "cchg",
                "inputs": {"NUM1": [1, "cadd2"], "NUM2": [1, "crep3"]},
                "fields": {}, "topLevel": false, "shadow": false},
       "cadd2": {"opcode": "operator_add", "next": null, "parent": "cadd",
                 "inputs": {"NUM1": [1, "crep1"], "NUM2": [1, "crep2"]},
                 "fields": {}, "topLevel": false, "shadow": false},
       "crep1": {"opcode": "argument_reporter_string_number", "next": null,
                 "parent": "cadd2", "inputs": {},
                 "fields": {"VALUE": ["x", null]},
                 "topLevel": false, "shadow": false},
       "crep2": {"opcode": "argument_reporter_string_number", "next": null,
                 "parent": "cadd2", "inputs": {},
                 "fields": {"VALUE": ["y", null]},
                 "topLevel": false, "shadow": false},
       "crep3": {"opcode": "argument_reporter_string_number", "next": null,
                 "parent": "cadd", "inputs": {},
                 "fields": {"VALUE": ["z", null]},
                 "topLevel": false, "shadow": false}
     },
     "costumes": [], "sounds": [],
     "currentCostume": 0, "x": 0, "y": 0, "size": 100,
     "direction": 90, "draggable": false,
     "rotationStyle": "all around", "visible": true,
     "layerOrder": 1, "volume": 100}
  ],
  "monitors": [], "extensions": [],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";

static void test_jit_alloca_loops() {
    const bool jitExpected =
        std::getenv("XYO_JIT_DISABLE") == nullptr;
    {
        Runtime rt;
        Sb3LoadResult r = loadSb3(rt, kProjectAllocaWarp, nullptr);
        CHECK(r.ok);
        if (!r.ok) return;
        Target* sp = spriteOf(rt, u"Sprite1");
        CHECK(sp != nullptr);
        if (!sp) return;
        rt.greenFlag();
        if (jitExpected && rt.jit)
            for (Thread* t : rt.threads) CHECK(t->isCompiled);
        bool ran = rt.runUntilDone(100000);
        CHECK(ran);
        CHECK(rt.threads.empty());
        size_t trash = 0;
        for (const auto& ot : rt.ownedThreads)
            trash += ot->trashFrames.size();
        if (jitExpected && rt.jit) CHECK_EQ(trash, 0u);
        auto num = [&](const char* id) {
            Variable* v = sp->lookupVariableById(id);
            return v ? v->value.asNumberUnchecked() : -999.0;
        };
        CHECK_EQ(num("sum"), 600000.0);
        CHECK_EQ(num("sum2"), 100000.0);
    }
    {
        Runtime rt;
        Sb3LoadResult r = loadSb3(rt, kProjectAllocaNonwarp, nullptr);
        CHECK(r.ok);
        if (!r.ok) return;
        Target* sp = spriteOf(rt, u"Sprite1");
        CHECK(sp != nullptr);
        if (!sp) return;
        rt.greenFlag();
        if (jitExpected && rt.jit)
            for (Thread* t : rt.threads) CHECK(t->isCompiled);
        int steps = 0;
        while (!rt.threads.empty() && steps < 400000) {
            rt.step();
            ++steps;
        }
        CHECK(rt.threads.empty());
        CHECK(steps > 1);
        size_t trash = 0;
        for (const auto& ot : rt.ownedThreads)
            trash += ot->trashFrames.size();
        if (jitExpected && rt.jit) CHECK_EQ(trash, 0u);
        Variable* v = sp->lookupVariableById("sum");
        CHECK(v != nullptr && v->value.asNumberUnchecked() == 300000.0);
    }
}

// 未定義手続き呼び出しは no-op（TW irgen が StackOpcode.NOP を返すのと
// 同じ。interp の procCall も prototype/def 無しで早期 return）。
// linux.sb3 の '​​log​​ %s'（ゼロ幅スペース入り proccode）呼び出しで
// JIT がスクリプト全体を bail → interp フォールバックしていた回帰。
// 孤立 prototype（def 無し）への呼び出しも同じく no-op。
static const char* kProjectUndefProc = R"JSON({
  "targets": [
    {"isStage": true, "name": "Stage",
     "variables": {}, "lists": {}, "broadcasts": {}, "blocks": {},
     "comments": {}, "costumes": [], "sounds": [], "currentCostume": 0,
     "volume": 100, "layerOrder": 0},
    {"isStage": false, "name": "Sprite1",
     "variables": {"i": ["i", 0]},
     "lists": {}, "broadcasts": {},
     "blocks": {
       "hat": {"opcode": "event_whenflagclicked", "next": "callu",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 0},
       "callu": {"opcode": "procedures_call", "next": "callo",
                 "parent": "hat",
                 "inputs": {"arg0": [1, [4, "1"]]},
                 "fields": {}, "topLevel": false, "shadow": false,
                 "mutation": {"tagName": "mutation", "children": [],
                     "proccode": "gone %s",
                     "argumentids": "[\"arg0\"]",
                     "argumentnames": "[\"p\"]",
                     "warp": "false"}},
       "callo": {"opcode": "procedures_call", "next": "seti",
                 "parent": "callu",
                 "inputs": {"a0": [1, [4, "9"]]},
                 "fields": {}, "topLevel": false, "shadow": false,
                 "mutation": {"tagName": "mutation", "children": [],
                     "proccode": "orphan %n",
                     "argumentids": "[\"a0\"]",
                     "argumentnames": "[\"p\"]",
                     "warp": "false"}},
       "oproto": {"opcode": "procedures_prototype", "next": null,
                  "parent": null, "inputs": {}, "fields": {},
                  "topLevel": true, "shadow": false,
                  "x": 0, "y": 200,
                  "mutation": {"tagName": "mutation", "children": [],
                      "proccode": "orphan %n",
                      "argumentids": "[\"a0\"]",
                      "argumentnames": "[\"p\"]",
                      "argumentdefaults": "[\"0\"]",
                      "warp": "false"}},
       "seti": {"opcode": "data_setvariableto", "next": "callr",
                "parent": "callo", "inputs": {"VALUE": [1, [4, "42"]]},
                "fields": {"VARIABLE": ["i", "i"]},
                "topLevel": false, "shadow": false},
       "callr": {"opcode": "procedures_call", "next": null,
                 "parent": "seti",
                 "inputs": {"arg0": [1, [4, "21"]]},
                 "fields": {}, "topLevel": false, "shadow": false,
                 "mutation": {"tagName": "mutation", "children": [],
                     "proccode": "double %n",
                     "argumentids": "[\"arg0\"]",
                     "argumentnames": "[\"x\"]",
                     "warp": "false"}},
       "def": {"opcode": "procedures_definition", "next": "body",
               "parent": null,
               "inputs": {"custom_block": [1, "proto"]},
               "fields": {}, "topLevel": true, "shadow": false,
               "x": 100, "y": 100},
       "proto": {"opcode": "procedures_prototype", "next": null,
                 "parent": "def",
                 "inputs": {"arg0": [1, "argname"]},
                 "fields": {}, "topLevel": false, "shadow": true,
                 "mutation": {"tagName": "mutation", "children": [],
                     "proccode": "double %n",
                     "argumentids": "[\"arg0\"]",
                     "argumentnames": "[\"x\"]",
                     "argumentdefaults": "[\"\"]",
                     "warp": "false"}},
       "argname": {"opcode": "argument_reporter_string_number",
                   "next": null, "parent": "proto", "inputs": {},
                   "fields": {"VALUE": ["x", null]},
                   "topLevel": false, "shadow": true},
       "body": {"opcode": "data_changevariableby", "next": null,
                "parent": "def",
                "inputs": {"VALUE": [1, "argrep"]},
                "fields": {"VARIABLE": ["i", "i"]},
                "topLevel": false, "shadow": false},
       "argrep": {"opcode": "argument_reporter_string_number",
                  "next": null, "parent": "body", "inputs": {},
                  "fields": {"VALUE": ["x", null]},
                  "topLevel": false, "shadow": false}
     },
     "costumes": [], "sounds": [],
     "currentCostume": 0, "x": 0, "y": 0, "size": 100,
     "direction": 90, "draggable": false,
     "rotationStyle": "all around", "visible": true,
     "layerOrder": 1, "volume": 100}
  ],
  "monitors": [], "extensions": [],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";

static void test_undefined_procedure_call() {
    Runtime rt;
    Sb3LoadResult r = loadSb3(rt, kProjectUndefProc, nullptr);
    CHECK(r.ok);
    if (!r.ok) { std::printf("load error: %s\n", r.error.c_str()); return; }
    Target* sp = spriteOf(rt, u"Sprite1");
    CHECK(sp != nullptr);
    if (!sp) return;
    rt.greenFlag();
    const bool jitExpected =
        rt.jit != nullptr && std::getenv("XYO_JIT_DISABLE") == nullptr;
    CHECK_EQ(rt.threads.size(), 1u);
    // JIT 有効時は未定義 call を含むスクリプトもコンパイルされる
    // （以前は scanChain が bail → interp フォールバック）
    if (jitExpected)
        for (Thread* t : rt.threads) CHECK(t->isCompiled);
    bool ran = rt.runUntilDone(100000);
    CHECK(ran);
    // 42 (set) + 21 (定義済み proc 呼出し) = 63 — 未定義呼出しは
    // 前後のブロック実行を妨げない
    Variable* i = sp->lookupVariableById("i");
    CHECK(i != nullptr);
    CHECK_EQ(cast::toNumber(i->value), 63.0);
}

#if defined(XYO_JIT)
static void test_jit_proc_frame_trash() {
    Thread t("outer");
    t.pushStack("outer");
    const size_t stack0 = t.stack.size();
    const size_t frames0 = t.stackFrames.size();
    auto sentinel = std::make_unique<StackFrame>();
    sentinel->isLoop = true;
    StackFrame* sp = sentinel.get();
    t.trashFrames.push_back(std::move(sentinel));

    JitCtx ctx{};
    ctx.thread = &t;
    for (int i = 0; i < 10000; ++i) {
        xyo_proc_push(&ctx, "child", nullptr);
        xyo_proc_pop(&ctx);
    }
    CHECK_EQ(t.stack.size(), stack0);
    CHECK_EQ(t.stackFrames.size(), frames0);
    CHECK_EQ(t.trashFrames.size(), 1u);
    CHECK_EQ(t.trashFrames.front().get(), sp);
    CHECK(t.trashFrames.front()->isLoop);
}
#endif

int test_e2e_main() {
    test_load_and_run();
    test_repeat_broadcast();
    test_procedure();
    test_load_gc_pressure();
    test_defer_collect();
    test_sb3_file();
    test_nan_operand_coercion();
    test_undefined_procedure_call();
    test_jit_alloca_loops();
#if defined(XYO_JIT)
    test_jit_proc_frame_trash();
#endif
    std::printf("e2e: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}
