// xyo-cpp: pen 拡張の E2E テスト（FakeRenderer で描画呼び出しを記録）。
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "blocks/pen_api.hpp"
#include "core/cast.hpp"
#include "core/color.hpp"
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

// setPenColorToColor の往復キャッシュ（RGB→HSV→RGBA）が非キャッシュ
// 計算と一致すること。同じ色を繰り返してヒット経路も通す。
static void test_color_cache() {
    const uint64_t colors[] = {
        0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF,
        0x3360030, 0x7F3F1F, 0x123456, 0xABCDEF, 0x010203,
        0x80FF0000,   // alpha=128
        0x4000FF00,   // alpha=64
    };
    PenState ps;
    for (int round = 0; round < 2; ++round) {
        for (uint64_t c : colors) {
            const Value v = Value::num(double(c));
            penrt::setColorToColorState(ps, v);
            const Rgba rgb = cast::toRgbColorObject(v);
            const Hsv hsv = color::rgbToHsv({rgb.r, rgb.g, rgb.b});
            const double color = (hsv.h / 360.0) * 100.0;
            const double sat = hsv.s * 100.0;
            const double bright = hsv.v * 100.0;
            const double transp =
                rgb.a < 255 ? 100.0 * (1.0 - rgb.a / 255.0) : 0.0;
            const Rgb out = color::hsvToRgb(
                {color * 360.0 / 100.0, sat / 100.0, bright / 100.0});
            CHECK_EQ(ps.color, color);
            CHECK_EQ(ps.saturation, sat);
            CHECK_EQ(ps.brightness, bright);
            CHECK_EQ(ps.transparency, transp);
            CHECK_EQ(ps.shade, bright / 2.0);
            CHECK_EQ(ps.rgba[0], out.r / 255.0f);
            CHECK_EQ(ps.rgba[1], out.g / 255.0f);
            CHECK_EQ(ps.rgba[2], out.b / 255.0f);
            CHECK_EQ(ps.rgba[3], 1.0f - float(transp / 100.0));
            CHECK(!ps.colorDirty);
            CHECK_EQ(ps.lastSetColor, v.bits);
        }
    }
}

// Compare the public, unconditional recolor facade against the original formula.
// Do not use CHECK here: worker threads must not race on the test counters.
static bool recolor_matches_formula(Target& target) {
    PenState& p = *target.penState;
    const Rgb rgb = color::hsvToRgb(
        {p.color * 360.0 / 100.0, p.saturation / 100.0,
         p.brightness / 100.0});
    const float expected[] = {
        rgb.r / 255.0f, rgb.g / 255.0f, rgb.b / 255.0f,
        1.0f - static_cast<float>(p.transparency / 100.0)};
    // A clean state and poisoned output ensure recolor cannot silently rely on
    // colorDirty or on a previous target's rgba (including its alpha).
    p.colorDirty = false;
    for (float& channel : p.rgba) channel = -123.0f;
    penrt::recolor(&target);
    for (unsigned channel = 0; channel < 4; ++channel) {
        if (std::bit_cast<uint32_t>(p.rgba[channel]) !=
            std::bit_cast<uint32_t>(expected[channel])) return false;
    }
    return true;
}

static void test_recolor_parity() {
    Sprite sprite(nullptr);
    Target target(nullptr, &sprite);
    target.penState = std::make_unique<PenState>();
    PenState& p = *target.penState;
    p.color = 37.125;
    p.saturation = 83.75;
    p.brightness = 64.5;
    // Same triple, changing only alpha: repeated recolors must refresh alpha.
    for (int round = 0; round < 3; ++round) {
        for (double transparency : {0.0, 100.0, 12.3456789, -25.0, 125.0}) {
            p.transparency = transparency;
            CHECK(recolor_matches_formula(target));
        }
    }
    Target fresh(nullptr, &sprite);
    fresh.penState = std::make_unique<PenState>(p);
    fresh.penState->transparency = 43.0;
    CHECK(recolor_matches_formula(fresh));

    // Exercise a broad finite palette, then replay it in both directions to
    // verify that results do not depend on previous recolor calls.
    constexpr unsigned keys = 4097;
    auto churn = [&](unsigned key) {
        p.color = (key % 401) * 0.25 - 0.125;
        p.saturation = (key % 97) + 0.125;
        p.brightness = 0.25 + key * (99.0 / keys);
        p.transparency = key % 101;
        CHECK(recolor_matches_formula(target));
    };
    for (unsigned key = 0; key < keys; ++key) churn(key);
    for (unsigned key = keys; key-- > 0;) churn(key);
    for (unsigned key = 0; key < keys; ++key) churn(key);

    // Exercise every combination of signed zero in the HSV inputs.
    for (unsigned mask = 0; mask < 8; ++mask) {
        p.color = (mask & 1) ? -0.0 : 0.0;
        p.saturation = (mask & 2) ? -0.0 : 0.0;
        p.brightness = (mask & 4) ? -0.0 : 0.0;
        CHECK(recolor_matches_formula(target));
        CHECK(recolor_matches_formula(target));
    }

    // Preserve double precision around every channel quantization boundary;
    // narrowing brightness to float can change the final channel bits.
    p.color = 0.0;
    p.saturation = 100.0;
    for (unsigned channel = 0; channel <= 255; ++channel) {
        const double boundary = 100.0 * channel / 255.0;
        const double neighbors[] = {
            std::nextafter(boundary, -std::numeric_limits<double>::infinity()),
            boundary,
            std::nextafter(boundary, std::numeric_limits<double>::infinity())};
        for (int round = 0; round < 2; ++round) {
            for (double brightness : neighbors) {
                p.brightness = brightness;
                CHECK(recolor_matches_formula(target));
            }
        }
    }

    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // Keep hue finite, including after scaling: nonfinite hue would enter the
    // reference conversion's undefined floating-to-integer cast path.
    for (double hue : {-1000000.25, -100.0, 0.0, 100.0, 1000000.25}) {
        p.color = hue;
        for (double saturation : {-1000.0, 0.0, 100.0, 1000.0, -inf, inf, nan}) {
            p.saturation = saturation;
            for (double brightness : {-1000.0, 0.0, 100.0, 1000.0, -inf, inf, nan}) {
                p.brightness = brightness;
                CHECK(recolor_matches_formula(target));
                CHECK(recolor_matches_formula(target));
            }
        }
    }
}

// Exercise the RGB round-trip setter independently of unconditional recolor.
// Cached output is inspected directly; the disabled-cache path is legitimately
// lazy, so only that path needs recolor before comparing the final rgba.
static bool set_color_matches_formula(Target& target, uint32_t packed) {
    const Value value = Value::num(static_cast<double>(packed));
    const Rgba rgb = cast::toRgbColorObject(value);
    const Hsv hsv = color::rgbToHsv({rgb.r, rgb.g, rgb.b});
    const double expectedColor = (hsv.h / 360.0) * 100.0;
    const double expectedSat = hsv.s * 100.0;
    const double expectedBright = hsv.v * 100.0;
    const double expectedTransparency =
        rgb.a < 255 ? 100.0 * (1.0 - rgb.a / 255.0) : 0.0;
    const Rgb out = color::hsvToRgb(
        {expectedColor * 360.0 / 100.0, expectedSat / 100.0,
         expectedBright / 100.0});
    const float expectedRgba[] = {
        out.r / 255.0f, out.g / 255.0f, out.b / 255.0f,
        1.0f - static_cast<float>(expectedTransparency / 100.0)};
    PenState& p = *target.penState;
    for (float& channel : p.rgba) channel = -123.0f;
    penrt::setColorToColorState(p, value);
    if (p.colorDirty) penrt::recolor(&target);
    const auto sameDouble = [](double actual, double expected) {
        return std::bit_cast<uint64_t>(actual) == std::bit_cast<uint64_t>(expected);
    };
    if (!sameDouble(p.color, expectedColor) ||
        !sameDouble(p.saturation, expectedSat) ||
        !sameDouble(p.brightness, expectedBright) ||
        !sameDouble(p.shade, expectedBright / 2.0) ||
        !sameDouble(p.transparency, expectedTransparency) ||
        p.lastSetColor != value.bits) return false;
    for (unsigned channel = 0; channel < 4; ++channel) {
        if (std::bit_cast<uint32_t>(p.rgba[channel]) !=
            std::bit_cast<uint32_t>(expectedRgba[channel])) return false;
    }
    return true;
}

static void test_recolor_parity_threads() {
    // std::thread is already used by the parallel-string unit tests. Construct
    // targets on the caller thread so unrelated target ID allocation is serial.
    constexpr unsigned workers = 4;
    // Distinct RGBs with varied HSV payloads, all hashing to bucket 1998.
    constexpr std::array<uint32_t, workers> collidingRgb = {
        0x123456u, 0x4567ECu, 0x89ADFDu, 0xCDF40Eu};
    for (uint32_t rgb : collidingRgb)
        CHECK_EQ((rgb * 2654435761u) >> 21, 1998u);
    Sprite sprite(nullptr);
    std::array<std::unique_ptr<Target>, workers> targets;
    for (auto& target : targets) {
        target = std::make_unique<Target>(nullptr, &sprite);
        target->penState = std::make_unique<PenState>();
    }
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    std::array<unsigned, workers> failures{};
    std::vector<std::thread> threads;
    for (unsigned worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            Target& target = *targets[worker];
            PenState& p = *target.penState;
            // Each thread repeatedly overwrites the same hash bucket with
            // different RGB payloads. Two consecutive calls use the same RGB
            // but distinct alpha, covering miss and hit without alpha caching.
            for (unsigned round = 0; round < 12000; ++round) {
                const uint32_t rgb = collidingRgb[(round + worker) % workers];
                const uint32_t alpha = 1 + (round + worker * 53) % 255;
                if (!set_color_matches_formula(target, (alpha << 24) | rgb))
                    ++failures[worker];
                const uint32_t otherAlpha = 1 + (alpha + 126) % 255;
                if (!set_color_matches_formula(target, (otherAlpha << 24) | rgb))
                    ++failures[worker];
                if ((round & 127) == 0) std::this_thread::yield();
            }
            for (unsigned round = 0; round < 4; ++round) {
                for (unsigned key = 0; key < 2049; ++key) {
                    // Different traversal, HSV inputs, and alpha per worker
                    // verify that independent recolors cannot affect each other.
                    const unsigned index = (key + worker * 509) % 2049;
                    p.color = (index % 101) + worker * 0.125;
                    p.saturation = (index % 89) + worker * 0.25;
                    p.brightness = (index % 97) + worker * 0.5;
                    p.transparency = (round * 17 + worker * 23) % 101;
                    if (!recolor_matches_formula(target)) ++failures[worker];
                    p.transparency = 100.0 - p.transparency;
                    if (!recolor_matches_formula(target)) ++failures[worker];
                }
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != workers)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    for (unsigned failuresInWorker : failures) CHECK_EQ(failuresInWorker, 0u);
}

// Adjacent down/up must still emit every point and expose the intermediate
// down state to renderer callbacks, even when the pen was already down.
static void test_pen_pair(bool useJit, bool initialDown) {
    struct PairRenderer : RendererAPI {
        Target* target = nullptr;
        int points = 0, skins = 0;
        int createPenSkin() override {
            ++skins;
            CHECK(target->penState && target->penState->penDown);
            return 17;
        }
        void penPoint(int skin, const float rgba[4], double, double, double) override {
            CHECK_EQ(skin, 17);
            CHECK(target->penState && target->penState->penDown);
            CHECK(target->onTargetMoved == penrt::movedHook());
            CHECK_EQ(rgba[3], 1.0f);
            ++points;
            // Up must act on the current state/hook, not a pointer saved before
            // this callback. These changes must be cleared by the following up.
            if (points % 3 == 0) {
                auto replacement = std::make_unique<PenState>(*target->penState);
                replacement->penDown = true;
                target->penState = std::move(replacement);
                target->onTargetMoved = [](Target*, double, double, bool) {};
            }
        }
    } ren;
    Runtime rt;
    rt.renderer = &ren;
#if defined(XYO_JIT)
    if (!useJit) rt.jit.reset();
#else
    (void)useJit;
#endif
    std::string project = R"JSON({"targets":[
      {"isStage":true,"name":"Stage","variables":{},"lists":{},"broadcasts":{},"blocks":{},"costumes":[],"sounds":[]},
      {"isStage":false,"name":"Pair","variables":{},"lists":{},"broadcasts":{},"costumes":[],"sounds":[],"blocks":{
        "hat":{"opcode":"event_whenflagclicked","next":"first","topLevel":true,"inputs":{},"fields":{}},
        "first":{"opcode":"pen_penDown","next":"loop","parent":"hat","inputs":{},"fields":{}},
        "loop":{"opcode":"control_repeat","next":"lastDown","parent":"first","inputs":{"TIMES":[1,[4,"16"]],"SUBSTACK":[2,"down"]},"fields":{}},
        "down":{"opcode":"pen_penDown","next":"up","parent":"loop","inputs":{},"fields":{}},
        "up":{"opcode":"pen_penUp","next":null,"parent":"down","inputs":{},"fields":{}},
        "lastDown":{"opcode":"pen_penDown","next":"lastUp","parent":"loop","inputs":{},"fields":{}},
        "lastUp":{"opcode":"pen_penUp","next":null,"parent":"lastDown","inputs":{},"fields":{}}
      }}],"monitors":[],"extensions":["pen"],"meta":{"semver":"3.0.0"}})JSON";
    if (!initialDown) {
        const std::string old = "\"next\":\"first\"";
        project.replace(project.find(old), old.size(), "\"next\":\"loop\"");
    }
    auto loaded = loadSb3(rt, project.c_str(), nullptr);
    CHECK(loaded.ok);
    if (!loaded.ok) return;
    ren.target = spriteOf(rt);
    CHECK(ren.target != nullptr);
    if (!ren.target) return;
    rt.greenFlag();
    rt.runUntilDone(100000);
    CHECK_EQ(ren.points, initialDown ? 18 : 17);
    CHECK_EQ(ren.skins, 1);
    CHECK(ren.target->penState && !ren.target->penState->penDown);
    CHECK(ren.target->onTargetMoved == nullptr);
    // Renderer is declared before Runtime so teardown cannot call a dead backend.
}

int test_pen() {
    for (bool jit : {false, true})
        for (bool initialDown : {false, true}) test_pen_pair(jit, initialDown);
    test_color_cache();
    test_recolor_parity();
    test_recolor_parity_threads();
    runPenScenario(true);    // JIT（rt.jit があればコンパイル経路）
    runPenScenario(false);   // インタプリタ
    std::printf("pen: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}
