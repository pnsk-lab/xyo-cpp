// Live headless Vulkan regression tests; no renderer / OpenGL involved.
// Run in separate processes with XYO_TEST_VULKAN=1, then additionally
// XYO_GPU_VK_RECORDS=1 for the legacy allocation/pipeline comparison.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include "core/rng.hpp"
#include "engine/runtime.hpp"
#include "gpu/gpu_isa.hpp"
#include "gpu/gpu_vulkan.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"
#if defined(XYO_JIT)
#include "jit/jit.hpp"
extern "C" int32_t xyo_gpu_repeat(xyo::JitCtx*, const void*, double);
#endif

namespace {
using namespace xyo;
int checks = 0, failures = 0;
#define VCHECK(c) do { ++checks; if (!(c)) { ++failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
uint32_t bits(float f) {
    uint32_t u; std::memcpy(&u, &f, sizeof(u)); return u;
}
void op(std::vector<uint32_t>& c, uint32_t opcode, uint32_t arg = 0) {
    c.push_back(opcode); c.push_back(arg);
}
void push(std::vector<uint32_t>& c, float f) { op(c, gpu::OP_PUSH, bits(f)); }

// Explicit independent oracle: each lane writes its index and red pen
// (width 3, transparency 25), then a point and two lines. A move after
// penUp changes position but must NOT increment the stroke count.
std::vector<uint32_t> fixture() {
    std::vector<uint32_t> c;
    op(c, gpu::OP_PENUP);
    op(c, gpu::OP_ITER); op(c, gpu::OP_STOREVM, 0);
    push(c, 3); op(c, gpu::OP_PENSIZE);
    push(c, 16711680); op(c, gpu::OP_PENCOLN);
    push(c, 25); op(c, gpu::OP_PENCOLP, 3);
    op(c, gpu::OP_ITER); push(c, 7); op(c, gpu::OP_GOTOXY);
    op(c, gpu::OP_PENDOWN);
    op(c, gpu::OP_ITER); push(c, 8); op(c, gpu::OP_GOTOXY);
    push(c, 1); op(c, gpu::OP_CHANGEY);
    op(c, gpu::OP_PENUP);
    op(c, gpu::OP_ITER); push(c, 2); op(c, gpu::OP_ADD);
    op(c, gpu::OP_SETX);
    op(c, gpu::OP_HALT);
    return c;
}
gpu::ExecRequest request(const std::vector<uint32_t>& code, uint32_t lanes,
                         uint32_t maxPerLane = 32) {
    static const uint32_t wbSlot = 0;
    gpu::ExecRequest r;
    r.code = code.data(); r.codeLen = code.size();
    r.lanes = lanes; r.maxPerLane = maxPerLane;
    r.nSlots = 1; r.nWb = 1; r.wbSlots = &wbSlot;
    r.fenceL = r.fenceB = -1e30f;
    r.fenceR = r.fenceT = 1e30f;
    return r;
}
void checkLanes(const gpu::ExecRequest& r) {
    VCHECK(r.executed); VCHECK(!r.overflow);
    VCHECK(r.nRecords == r.lanes * 3u);
    VCHECK(r.laneOut != nullptr);
    if (!r.laneOut) return;
    const uint32_t stride = gpu::laneOutStride(1);
    const uint32_t flags = 1u | 2u | 4u | 16u |
        gpu::kLaneFlagX | gpu::kLaneFlagY | gpu::kLaneFlagSize |
        gpu::kLaneFlagHue | gpu::kLaneFlagSat |
        gpu::kLaneFlagBri | gpu::kLaneFlagTransp;
    bool allMatch = true;
    for (uint32_t lane = 0; lane < r.lanes; ++lane) {
        const float* p = r.laneOut + size_t(lane) * stride;
        if (bits(p[0]) != flags || p[1] != float(lane + 2) || p[2] != 9 ||
            p[3] != 3 || p[4] != 0 || p[5] != 100 || p[6] != 100 ||
            p[7] != 25 || bits(p[8]) != 1u || p[9] != float(lane) ||
            p[10] != 50) {
            std::printf("Vulkan lane oracle mismatch at lane %u\n", lane);
            allMatch = false; break;
        }
    }
    VCHECK(allMatch);
}
// Verify all seven carried-initialization parameters independently of the
// body. A move produces no stroke in lane0 (up), one in every other lane
// (penDownAll), and does not modify any of the carried pen attributes.
void carriedInitializationCase() {
    std::vector<uint32_t> code;
    push(code, 4); push(code, 5); op(code, gpu::OP_GOTOXY);
    op(code, gpu::OP_HALT);
    auto r = request(code, 65);
    r.startX = 1; r.startY = 2;
    r.penDown = false; r.penDownAll = true;
    r.penSize = 3;
    r.penHSV[0] = 10; r.penHSV[1] = 20;
    r.penHSV[2] = 30; r.penHSV[3] = 40;
    r.endAttrMask = 31;
    r.endAttr[0] = 6; r.endAttr[1] = 50; r.endAttr[2] = 60;
    r.endAttr[3] = 70; r.endAttr[4] = 80;
    const bool ok = gpu::vulkanExecParallel(r);
    VCHECK(ok);
    if (!ok) return;
    VCHECK(r.executed); VCHECK(!r.overflow);
    VCHECK(r.nRecords == 64);
    VCHECK(r.laneOut != nullptr);
    if (!r.laneOut) return;
    bool allMatch = true;
    for (uint32_t lane = 0; lane < r.lanes; ++lane) {
        const float* p = r.laneOut + size_t(lane) * gpu::laneOutStride(1);
        const uint32_t flags = 1u | gpu::kLaneFlagX | gpu::kLaneFlagY |
                               (lane ? 8u : 0u);
        if (bits(p[0]) != flags || p[1] != 4 || p[2] != 5 ||
            p[3] != (lane ? 6 : 3) || p[4] != (lane ? 50 : 10) ||
            p[5] != (lane ? 60 : 20) || p[6] != (lane ? 70 : 30) ||
            p[7] != (lane ? 80 : 40) || bits(p[8]) != 0 || p[9] != 0) {
            std::printf("Vulkan carried initialization mismatch at lane %u\n", lane);
            allMatch = false; break;
        }
    }
    VCHECK(allMatch);
}
void rawCases(bool records) {
    carriedInitializationCase();
    const auto code = fixture();
    auto small = request(code, 65); // partially filled final workgroup
    const bool smallOk = gpu::vulkanExecParallel(small);
    VCHECK(smallOk);
    if (smallOk) checkLanes(small);
    auto overflow = request(code, 16, 2); // three strokes exceed capacity
    VCHECK(!gpu::vulkanExecParallel(overflow));
    VCHECK(!overflow.executed); VCHECK(overflow.laneOut == nullptr);
    VCHECK(overflow.overflow);
    // Nonterminating bytecode: instruction guard must remain active even
    // when record writes have been compiled away.
    const std::vector<uint32_t> loop{gpu::OP_JMP, 0};
    auto guarded = request(loop, 16);
    VCHECK(!gpu::vulkanExecParallel(guarded));
    VCHECK(!guarded.executed); VCHECK(guarded.laneOut == nullptr);
    VCHECK(guarded.overflow);
    auto large = request(code, 1000000);
    // 1M * 32 * 48 = 1.536 GB theoretical records (>512 MiB).
    // Default headless must not allocate this; legacy must reject it.
    const bool largeOk = gpu::vulkanExecParallel(large);
    VCHECK(largeOk == !records);
    if (largeOk) checkLanes(large);
    else { VCHECK(!large.executed); VCHECK(large.laneOut == nullptr); }
}
#if defined(XYO_JIT)
void runtimeCases() {
    Runtime rt;
    static const char* project = R"JSON({"targets":[
      {"isStage":true,"name":"Stage","variables":{},"lists":{},
       "broadcasts":{},"blocks":{},"comments":{},"costumes":[],"sounds":[],
       "currentCostume":0,"volume":100,"layerOrder":0},
      {"isStage":false,"name":"S","variables":{"v":["v",123]},"lists":{},
       "broadcasts":{},"blocks":{},"comments":{},"costumes":[],"sounds":[],
       "currentCostume":0,"x":11,"y":12,"size":100,"direction":90,
       "rotationStyle":"all around","visible":true,"layerOrder":1}],
      "monitors":[],"extensions":["pen"],
      "meta":{"semver":"3.0.0","vm":"0.0.0","agent":""}})JSON";
    const auto loaded = loadSb3(rt, project, nullptr);
    VCHECK(loaded.ok);
    if (!loaded.ok) return;
    Target* sp = nullptr;
    for (Target* t : rt.targets) if (!t->isStage) { sp = t; break; }
    VCHECK(sp != nullptr);
    if (!sp) return;
    Variable* v = sp->lookupVariableById("v");
    VCHECK(v != nullptr);
    if (!v) return;
    sp->penState = std::make_unique<PenState>();
    sp->penState->penSize = 9; sp->penState->color = 55;
    sp->penState->shade = 33; sp->penState->penDown = true;
    const PenState before = *sp->penState;
    JitCtx ctx{};
    ctx.runtime = &rt; ctx.target = sp; ctx.isWarp = 1;
    gpu::Program p;
    p.code = fixture(); p.nSlots = 1; p.hasPenOps = true;
    p.wbVars.push_back({"v", u"v", 0, 0}); p.maxPerLane = 2;
    auto unchanged = [&] {
        VCHECK(sp->x == 11 && sp->y == 12);
        VCHECK(v->value.asNumberUnchecked() == 123);
        VCHECK(sp->penState->penSize == before.penSize);
        VCHECK(sp->penState->color == before.color);
        VCHECK(sp->penState->saturation == before.saturation);
        VCHECK(sp->penState->brightness == before.brightness);
        VCHECK(sp->penState->transparency == before.transparency);
        VCHECK(sp->penState->colorDirty == before.colorDirty);
        VCHECK(sp->penState->lastSetColor == before.lastSetColor);
        VCHECK(std::memcmp(sp->penState->rgba, before.rgba,
                           sizeof(before.rgba)) == 0);
        VCHECK(sp->penState->shade == before.shade);
        VCHECK(sp->penState->penDown == before.penDown);
        VCHECK(sp->onTargetMoved == nullptr);
    };
    Rng saved = Rng::global();
    const uint32_t next = saved.next();
    VCHECK(xyo_gpu_repeat(&ctx, &p, 16) == 0); unchanged();
    Rng after = Rng::global(); VCHECK(after.next() == next);
    p.code = {gpu::OP_JMP, 0}; p.maxPerLane = 32;
    VCHECK(xyo_gpu_repeat(&ctx, &p, 16) == 0); unchanged();
    after = Rng::global(); VCHECK(after.next() == next);
    p.code = fixture();
    VCHECK(xyo_gpu_repeat(&ctx, &p, 65) == 1);
    VCHECK(sp->x == 66 && sp->y == 9);
    VCHECK(v->value.asNumberUnchecked() == 64);
    VCHECK(sp->penState->penSize == 3);
    VCHECK(sp->penState->color == 0);
    VCHECK(sp->penState->saturation == 100);
    VCHECK(sp->penState->brightness == 100);
    VCHECK(sp->penState->transparency == 25);
    VCHECK(sp->penState->shade == 50);
    VCHECK(!sp->penState->penDown);
}
#endif
} // namespace
int test_vulkan_headless() {
    const char* live = std::getenv("XYO_TEST_VULKAN");
    if (!live || std::strcmp(live, "1") != 0) {
        std::printf("SKIP headless Vulkan (set XYO_TEST_VULKAN=1)\n"); return 0;
    }
    checks = failures = 0;
    // Explicit live opt-in must fail, not silently skip, if unsupported.
    const bool supported = xyo::gpu::vulkanExecSupported();
    VCHECK(supported);
    if (supported) {
        const char* mode = std::getenv("XYO_GPU_VK_RECORDS");
        rawCases(mode != nullptr); // match the executor's presence-based switch
#if defined(XYO_JIT)
        runtimeCases();
#else
        std::printf("SKIP Vulkan JIT-helper cases (XYO_JIT disabled)\n");
#endif
    }
    std::printf("headless Vulkan: %d checks, %d failures\n", checks, failures);
    return failures;
}
