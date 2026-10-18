// xyo-cpp: pen 拡張の E2E テスト（FakeRenderer で描画呼び出しを記録）。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#if defined(XYO_JIT)
#include "jit/jit.hpp"
#endif

using namespace xyo;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; \
    if (!((a) == (b))) { ++g_fail; \
    std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

namespace {

struct FakeRenderer : public RendererAPI {
    struct Line {
        float rgba[4];
        double dia, x0, y0, x1, y1;
    };
    int skin = -1;
    int points = 0, clears = 0, stamps = 0;
    std::vector<Line> lines;
    int createPenSkin() override { return (skin = 7); }
    void penLine(int s, const float rgba[4], double dia, double x0,
                 double y0, double x1, double y1) override {
        CHECK_EQ(s, skin);
        Line l{{rgba[0], rgba[1], rgba[2], rgba[3]}, dia, x0, y0, x1, y1};
        lines.push_back(l);
    }
    void penPoint(int s, const float[4], double, double,
                  double) override {
        CHECK_EQ(s, skin);
        ++points;
    }
    void penClear(int s) override {
        CHECK_EQ(s, skin);
        ++clears;
    }
    void penStamp(int s, int) override {
        CHECK_EQ(s, skin);
        ++stamps;
    }
};

// flag → pen down → move 50 → pen up → move 50 → set size 20 → pen down →
// move 50 → pen up → stamp → clear
const char* kProject = R"JSON({
  "targets": [
    {"isStage": true, "name": "Stage",
     "variables": {}, "lists": {}, "broadcasts": {}, "blocks": {},
     "costumes": [], "sounds": [], "currentCostume": 0, "layerOrder": 0},
    {"isStage": false, "name": "Sprite1",
     "variables": {}, "lists": {}, "broadcasts": {},
     "blocks": {
       "hat": {"opcode": "event_whenflagclicked", "next": "down1",
               "parent": null, "inputs": {}, "fields": {}, "topLevel": true},
       "down1": {"opcode": "pen_penDown", "next": "move1", "parent": "hat",
               "inputs": {}, "fields": {}},
       "move1": {"opcode": "motion_movesteps", "next": "up1", "parent": "down1",
               "inputs": {"STEPS": [1, [4, "50"]]}, "fields": {}},
       "up1": {"opcode": "pen_penUp", "next": "move2", "parent": "move1",
               "inputs": {}, "fields": {}},
       "move2": {"opcode": "motion_movesteps", "next": "sz", "parent": "up1",
               "inputs": {"STEPS": [1, [4, "50"]]}, "fields": {}},
       "sz": {"opcode": "pen_setPenSizeTo", "next": "col", "parent": "move2",
               "inputs": {"SIZE": [1, [4, "20"]]}, "fields": {}},
       "col": {"opcode": "pen_setPenColorToColor", "next": "cpm",
               "parent": "sz",
               "inputs": {"COLOR": [1, [9, "#ff0000"]]}, "fields": {}},
       "cpm": {"opcode": "pen_setPenColorParamTo", "next": "down2",
               "parent": "col",
               "inputs": {"COLOR_PARAM": [1, "menu"],
                          "VALUE": [1, [4, "50"]]}, "fields": {}},
       "menu": {"opcode": "pen_menu_colorParam", "next": null, "parent": "cpm",
               "inputs": {},
               "fields": {"colorParam": ["transparency", null]},
               "shadow": true},
       "down2": {"opcode": "pen_penDown", "next": "move3", "parent": "cpm",
               "inputs": {}, "fields": {}},
       "move3": {"opcode": "motion_movesteps", "next": "up2", "parent": "down2",
               "inputs": {"STEPS": [1, [4, "50"]]}, "fields": {}},
       "up2": {"opcode": "pen_penUp", "next": "st", "parent": "move3",
               "inputs": {}, "fields": {}},
       "st": {"opcode": "pen_stamp", "next": "cl", "parent": "up2",
               "inputs": {}, "fields": {}},
       "cl": {"opcode": "pen_clear", "next": null, "parent": "st",
               "inputs": {}, "fields": {}}
     },
     "costumes": [], "sounds": [], "currentCostume": 0,
     "x": 0, "y": 0, "size": 100, "direction": 90,
     "rotationStyle": "all around", "visible": true, "layerOrder": 1}
  ],
  "monitors": [], "extensions": ["pen"],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";

Target* spriteOf(Runtime& rt) {
    for (Target* t : rt.targets) if (t->sprite && !t->isStage) return t;
    return nullptr;
}

} // namespace

// JIT / インタプリタの双方で同一のペン状態に収束するか検証する。
// pen_menu_colorParam shadow のフィールド名は実 SB3 では
// colorParam（親の入力名 COLOR_PARAM ではない）— かつて interp は
// 大文字名を読んで transparency を無効化していた。
static void runPenScenario(bool useJit) {
    Runtime rt;
    FakeRenderer ren;
    rt.renderer = &ren;
#if defined(XYO_JIT)
    if (!useJit) rt.jit.reset();
#else
    (void)useJit;
#endif
    Sb3LoadResult r = loadSb3(rt, kProject, nullptr);
    CHECK(r.ok);
    if (!r.ok) { std::printf("load: %s\n", r.error.c_str()); return; }
    Target* sp = spriteOf(rt);
    CHECK(sp != nullptr);
    if (!sp) return;

    rt.greenFlag();
    rt.runUntilDone(100000);

    // penDown 2 回 → penPoint 2 回（遅延 skin 生成は 1 度だけ）
    CHECK_EQ(ren.skin, 7);
    CHECK_EQ(ren.points, 2);
    // pen down 中の移動のみ線になる → move1 (0,0)→(50,0) と
    // move3 (100,0)→(150,0) の 2 本。pen up 中の move2 は線にならない。
    CHECK_EQ(ren.lines.size(), 2u);
    if (ren.lines.size() >= 2) {
        CHECK_EQ(ren.lines[0].x0, 0.0);
        CHECK_EQ(ren.lines[0].x1, 50.0);
        CHECK_EQ(ren.lines[0].dia, 1.0);
        // 2 本目は size=20 + #ff0000
        CHECK_EQ(ren.lines[1].x0, 100.0);
        CHECK_EQ(ren.lines[1].x1, 150.0);
        CHECK_EQ(ren.lines[1].dia, 20.0);
        CHECK(ren.lines[1].rgba[0] > 0.99f);
        CHECK(ren.lines[1].rgba[1] < 0.01f);
        // transparency=50（メニュー経由）→ alpha≈0.5
        CHECK(ren.lines[1].rgba[3] > 0.45f && ren.lines[1].rgba[3] < 0.55f);
    }
    CHECK_EQ(ren.stamps, 1);
    CHECK_EQ(ren.clears, 1);
    // pen up 後はフックが外れている
    CHECK(sp->onTargetMoved == nullptr);
    CHECK(sp->penState && !sp->penState->penDown);
    CHECK_EQ(sp->penState->penSize, 20.0);

    // --- clone は penState を複製（penDown 中ならフックも）---
    Target* c = sp->makeClone();
    CHECK(c != nullptr);
    if (c) {
        CHECK(c->penState != nullptr);
        CHECK_EQ(c->penState->penSize, 20.0);
        CHECK_EQ(c->onTargetMoved, nullptr);   // penUp 済み
        sp->penState->penDown = true;
        sp->onTargetMoved = [](Target*, double, double, bool) {};
        Target* c2 = sp->makeClone();
        CHECK(c2->onTargetMoved != nullptr);
        CHECK(c2->penState->penDown);
        c->dispose();
        c2->dispose();
    }
}

int test_pen() {
    runPenScenario(true);    // JIT（rt.jit があればコンパイル経路）
    runPenScenario(false);   // インタプリタ
    std::printf("pen: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}
