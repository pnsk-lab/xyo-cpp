// xyo-cpp: GPU 並列ループの JIT ランタイムヘルパ。
// 生成コードは `if (xyo_gpu_repeat(ctx, prog, limit)) skip; else CPU loop`
// を emit する。実行可否の判定・dispatch 時スナップショット・
// 終了状態の書き戻し（直列意味論と一致するよう「最後に書いたレーン」
// の値を成分別に採用）を行う。
#include "../jit/jit.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../blocks/pen_api.hpp"
#include "../core/cast.hpp"
#include "../core/rng.hpp"
#include "../engine/runtime.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"
#include "gpu_isa.hpp"
#include "gpu_vulkan.hpp"

namespace xyo {

namespace {

// laneOut flags（gpu_isa.hpp のレイアウトと一致）
constexpr uint32_t kFPosDirty = 1, kFPenAttr = 2, kFPdDirty = 4,
                   kFPenDown = 8, kFColN = 16;
constexpr uint32_t kWbMaskOff = 8;   // wbMask 開始オフセット

constexpr double kExactF32Int = 16777216.0;   // 2^24 — f32 整数厳密範囲

uint32_t flagAt(const float* lo, size_t stride, uint32_t lane) {
    uint32_t u;
    float f = lo[size_t(lane) * stride];
    std::memcpy(&u, &f, 4);
    return u;
}
uint32_t maskAt(const float* lo, size_t stride, uint32_t lane,
                uint32_t wbIdx) {
    uint32_t u;
    float f = lo[size_t(lane) * stride + kWbMaskOff + wbIdx / 32];
    std::memcpy(&u, &f, 4);
    return u;
}

// 変数の非破壊的解決（生成しない）。未解決は nullptr。
Variable* resolveVar(Target* t, const std::string& id,
                     const std::u16string& name) {
    if (Variable* v = t->lookupVariableById(id)) return v;
    return t->lookupVariableByNameAndType(name, Variable::kScalarType);
}

// f32 厳密整数か
bool exactInt(double d) {
    return std::isfinite(d) && d == std::floor(d) &&
           d >= -kExactF32Int && d <= kExactF32Int;
}

} // namespace

extern "C" int32_t xyo_gpu_repeat(JitCtx* c, const void* progPtr,
                                  double limit) {
    static const bool disabled = std::getenv("XYO_GPU") &&
                                 std::getenv("XYO_GPU")[0] == '0';
    static const bool dbg = std::getenv("XYO_GPU_DEBUG") != nullptr;
    if (disabled || !progPtr || !c || !c->runtime || !c->target)
        return 0;
    const gpu::Program& p = *static_cast<const gpu::Program*>(progPtr);
    Runtime* rt = c->runtime;
    Target* t = c->target;

    // 非 warp では直列は反復毎に yield する — GPU 一括実行は
    // 観測可能なタイミングを変えるため CPU ループを維持する。
    if (!c->isWarp) return 0;
    // stage は setXY no-op、dragging 中は移動拒否 — 意味論が違うため
    if (t->isStage || t->dragging) return 0;
    if (!rt->renderer && !gpu::vulkanExecSupported()) return 0;

    // ---- 変数解決（失敗しても状態を変更しない — 生成もしない）----
    const uint32_t nG = uint32_t(p.globals.size());
    const uint32_t nWb = uint32_t(p.wbVars.size());
    std::vector<Variable*> gvars(nG), wbvars(nWb);
    std::unordered_map<Variable*, std::string> seenIds;
    std::unordered_set<std::string> writtenIds;
    for (auto& w : p.wbVars) writtenIds.insert(w.id);
    auto noteVar = [&](Variable* v, const std::string& id) -> bool {
        auto [it, ins] = seenIds.emplace(v, id);
        if (!ins && it->second != id) {
            // 別 id が同一 Variable* へ解決（エイリアス）— 書き込み側が
            // 絡む場合 id ベースの証明が成り立たないため拒否。
            if (writtenIds.count(id) || writtenIds.count(it->second))
                return false;
        }
        return true;
    };
    for (uint32_t i = 0; i < nG; ++i) {
        Variable* v = resolveVar(t, p.globals[i].id, p.globals[i].name);
        if (!v || v->type != Variable::kScalarType) return 0;
        // スナップショット/bound ガードは有限の数値値を要求 —
        // 任意の文字列比較を数値と見做さない。
        if (!v->value.isNumber()) return 0;
        double d = v->value.asNumberUnchecked();
        if (!std::isfinite(d) ||
            std::fabs(d) > double(std::numeric_limits<float>::max()))
            return 0;
        if (!noteVar(v, p.globals[i].id)) return 0;
        gvars[i] = v;
    }
    for (uint32_t i = 0; i < nWb; ++i) {
        Variable* v = resolveVar(t, p.wbVars[i].id, p.wbVars[i].name);
        if (!v || v->type != Variable::kScalarType) return 0;
        if (!noteVar(v, p.wbVars[i].id)) return 0;
        wbvars[i] = v;
    }

    // ---- lanes の導出 ------------------------------------------------
    double lanesD = 0;
    if (p.mode == gpu::LoopMode::While) {
        // counted while: 初期カウンタ・ステップ・境界から回数を導出。
        // 反転方向・ゼロステップ・非整数開始・非数値境界・範囲外は CPU。
        if (p.whileInd < 0 || size_t(p.whileInd) >= p.induction.size())
            return 0;
        const gpu::InductionVar& iv = p.induction[size_t(p.whileInd)];
        const gpu::GlobalVar& cg = p.globals[iv.globalIdx];
        Variable* cv = gvars[iv.globalIdx];
        (void)cg;
        double i0 = cv->value.asNumberUnchecked();
        double step = iv.step;
        double bound = limit;
        if (!exactInt(i0) || !exactInt(step) || step == 0.0 ||
            !std::isfinite(bound))
            return 0;
        double dist;   // 境界方向への距離（正=進む余地あり）
        bool inclusive;
        switch (p.whileCmp) {
        case 1:   // counter < bound (step>0)
            if (step <= 0) return 0;
            dist = bound - i0; inclusive = false; break;
        case 2:   // counter <= bound
            if (step <= 0) return 0;
            dist = bound - i0; inclusive = true; break;
        case 3:   // counter > bound (step<0)
            if (step >= 0) return 0;
            dist = i0 - bound; inclusive = false; break;
        case 4:   // counter >= bound
            if (step >= 0) return 0;
            dist = i0 - bound; inclusive = true; break;
        default: return 0;
        }
        double as = std::fabs(step);
        double count;
        if (inclusive)
            count = dist >= 0.0 ? std::floor(dist / as) + 1.0 : 0.0;
        else
            count = dist > 0.0 ? std::ceil(dist / as) : 0.0;
        if (!std::isfinite(count) || count < 0.0 ||
            count > double(UINT32_MAX))
            return 0;
        // 導出回数の検証: 最終レーンが条件を満たし次が満たさないこと。
        auto holds = [&](double v) {
            switch (p.whileCmp) {
            case 1: return v < bound;
            case 2: return v <= bound;
            case 3: return v > bound;
            case 4: return v >= bound;
            default: return false;
            }
        };
        if (count == 0.0) {
            if (holds(i0)) return 0;   // 0 なのに条件成立は矛盾
        } else {
            double last = i0 + (count - 1.0) * step;
            double next = i0 + count * step;
            if (!holds(last) || holds(next)) return 0;
        }
        lanesD = count;
    } else {
        lanesD = p.mode == gpu::LoopMode::ForEach
                     ? std::floor(limit)
                     : std::floor(limit + 0.5);
    }
    if (!(lanesD > 0.0) || !std::isfinite(lanesD)) return 0;
    static const uint32_t kMinLanes = [] {
        const char* e = std::getenv("XYO_GPU_MIN");
        return e ? std::max(1, std::atoi(e)) : 16;
    }();
    if (lanesD < kMinLanes) return 0;
    if (lanesD > double(UINT32_MAX)) return 0;
    const uint32_t lanes = uint32_t(lanesD);
    if (dbg)
        std::fprintf(stderr, "xyo-gpu: dispatch lanes=%u slots=%u wb=%u "
                             "code=%zu penOps=%d mode=%u ind=%zu\n",
                     lanes, p.nSlots, nWb, p.code.size() / 2,
                     int(p.hasPenOps), uint32_t(p.mode),
                     p.induction.size());
    if (std::getenv("XYO_GPU_DUMP")) {
        static bool dumped = false;
        if (!dumped) {
            dumped = true;
            std::fprintf(stderr, "=== GPU program (%zu instrs, %u slots,"
                                 " %u wb, %u globals) ===\n",
                         p.code.size() / 2, p.nSlots,
                         uint32_t(p.wbVars.size()),
                         uint32_t(p.globals.size()));
            for (size_t i = 0; i < p.code.size(); i += 2) {
                uint32_t op = p.code[i], ar = p.code[i + 1];
                float f;
                std::memcpy(&f, &ar, 4);
                std::fprintf(stderr, "  %4zu: %2u  %10u (%.6g)\n",
                             i / 2, op, ar, double(f));
            }
        }
    }

    // ---- induction exactness ガード ----------------------------------
    // 初期値・ステップ・lane 積・中間/終端値が全て f32 厳密整数範囲。
    // 近似はせず範囲外は CPU へ。
    for (const gpu::InductionVar& iv : p.induction) {
        Variable* v = gvars[iv.globalIdx];
        double i0 = v->value.asNumberUnchecked();
        double step = iv.step;
        if (!exactInt(i0) || !exactInt(step)) return 0;
        double lastK = double(lanes - 1) * step;
        if (!exactInt(lastK)) return 0;
        for (double base : {i0, i0 + lastK})
            for (double off : {double(iv.minOff), double(iv.maxOff)})
                if (!exactInt(base + off)) return 0;
        if (!exactInt(i0 + lastK)) return 0;
    }

    gpu::ExecRequest req;
    req.code = p.code.data();
    req.codeLen = p.code.size();
    req.nGlobals = nG;
    req.nSlots = p.nSlots;
    req.lanes = lanes;
    req.maxPerLane = p.maxPerLane;
    req.nWb = nWb;
    std::vector<uint32_t> wbs(nWb);
    for (uint32_t i = 0; i < nWb; ++i) wbs[i] = p.wbVars[i].slot;
    req.wbSlots = wbs.data();

    // 読み専用変数のスナップショット（上で解決済み・有限数値を確認済み）
    std::vector<float> gvals(nG);
    for (uint32_t i = 0; i < nG; ++i)
        gvals[i] = float(gvars[i]->value.asNumberUnchecked());
    req.globals = gvals.data();

    // フェンス矩形（getFencedPosition と同じ計算を矩形化したもの）。
    // renderer 無しの headless では setXY が clamp をスキップするため
    // ±inf（= clamp 無し）に合わせる。
    if (rt->renderer) {
        double fr[4];
        rt->renderer->getFenceRect(t->drawableID, fr);
        req.fenceL = float(fr[0]); req.fenceR = float(fr[1]);
        req.fenceB = float(fr[2]); req.fenceT = float(fr[3]);
    }
    req.startX = float(t->x);
    req.startY = float(t->y);
    if (!std::isfinite(t->x) || !std::isfinite(t->y)) return 0;
    if (rt->fencing) {
        // 初期位置がフェンス外なら SETX 等で「書かれない成分」が
        // 暗黙の初期クランプで変わり得る — CPU フォールバック。
        if (t->x < req.fenceL || t->x > req.fenceR ||
            t->y < req.fenceB || t->y > req.fenceT)
            return 0;
    }
    if (t->penState) {
        PenState& ps = *t->penState;
        // HSV/size は真値なので recolor（rgba 再計算）不要 — 状態を
        // 変更せずにスナップショットする。
        req.penSize = float(ps.penSize);
        req.penHSV[0] = float(ps.color);
        req.penHSV[1] = float(ps.saturation);
        req.penHSV[2] = float(ps.brightness);
        req.penHSV[3] = float(ps.transparency);
        req.penDown = ps.penDown;
    }
    // 本体に penUp/penDown が無いならペン状態は不変 — dispatch 時に
    // penDown だと全ストロークの起点が前反復終端に依存（逐次）なので
    // この場合のみ dispatch 状態で拒否する。
    if (req.penDown && !p.hasPenOps) return 0;
    // 全終了経路 down 確定 → 逐列では lanes>0 が前レーン終端の
    // down を継承するので VM の lanes>0 も down 開始にする。
    // 終端一様属性も lanes>0 の初期値にする（carried 観測一致）。
    req.penDownAll = p.endMustDown;
    req.endAttrMask = p.endAttrMask;
    std::memcpy(req.endAttr, p.endAttr, sizeof(req.endAttr));
    req.miscLimits = rt->miscLimits;
    // 乱数系列は dispatch 毎に進める — 固定だと全 dispatch が同一の
    // 乱数値を生成して描画が静止する。シードの一致は不要（分布のみ）。
    // 失敗パスで実 RNG を進めないようコピーから採り、成功時に確定する。
    Rng rngCopy = Rng::global();
    req.baseSeed = rngCopy.next();
    if (!rt->fencing || !rt->renderer) {
        req.fenceL = req.fenceB = -1e30f;
        req.fenceR = req.fenceT = 1e30f;
    }

    // renderer 接続中は GL 経路（ストロークをペン FBO へ直接ラスタ化）。
    // headless/Windows では Vulkan compute executor（ストロークは
    // 描画しない — headless の CPU フォールバックと同じ観測結果）。
    bool execOk = rt->renderer
                      ? rt->renderer->gpuExecParallel(req)
                      : gpu::vulkanExecParallel(req);
    if (!execOk || !req.executed || !req.laneOut) {
        if (dbg)
            std::fprintf(stderr, "xyo-gpu: exec failed (overflow/guard/"
                                 "unavailable) -> CPU fallback\n");
        return 0;
    }
    if (dbg)
        std::fprintf(stderr, "xyo-gpu: executed %u lanes\n", lanes);
    // ここまで副作用なし — dispatch 成功を確定してから実 RNG を進める。
    Rng::global() = rngCopy;
    if (req.nRecords > 0) rt->redrawRequested = true;

    // ---- 終了状態の書き戻し（直列意味論: 最後に変更したレーンが勝つ）----
    const size_t stride = gpu::laneOutStride(req.nWb);
    const float* lo = req.laneOut;

    // 位置は成分別の最終ライタ（集約 dirty では他成分の古い値が
    // 復活し得るため）。
    int lastX = -1, lastY = -1;
    for (uint32_t l = lanes; l-- > 0 && (lastX < 0 || lastY < 0);) {
        uint32_t fl = flagAt(lo, stride, l);
        if (lastX < 0 && (fl & gpu::kLaneFlagX)) lastX = int(l);
        if (lastY < 0 && (fl & gpu::kLaneFlagY)) lastY = int(l);
    }
    if (lastX >= 0 || lastY >= 0) {
        if (lastX >= 0) t->x = lo[size_t(lastX) * stride + 1];
        if (lastY >= 0) t->y = lo[size_t(lastY) * stride + 2];
        if (rt->renderer && t->visible) {
            t->emitVisualChange();
            rt->redrawRequested = true;
        }
        rt->requestTargetsUpdate(t);
    }

    // ペン属性 / penDown: それぞれ成分別の最終ライタ
    {
        int lastPd = -1, lastColN = -1;
        int lastAttr[5] = {-1, -1, -1, -1, -1};   // size,hue,sat,bri,transp
        static const uint32_t kBit[5] = {
            gpu::kLaneFlagSize, gpu::kLaneFlagHue, gpu::kLaneFlagSat,
            gpu::kLaneFlagBri, gpu::kLaneFlagTransp};
        bool any = false;
        for (uint32_t l = lanes; l-- > 0;) {
            uint32_t fl = flagAt(lo, stride, l);
            if (lastPd < 0 && (fl & kFPdDirty)) lastPd = int(l);
            if (lastColN < 0 && (fl & kFColN)) lastColN = int(l);
            for (int k = 0; k < 5; ++k)
                if (lastAttr[k] < 0 && (fl & kBit[k])) lastAttr[k] = int(l);
            if (lastPd >= 0 && lastColN >= 0) {
                bool done = true;
                for (int k = 0; k < 5; ++k)
                    if (lastAttr[k] < 0) { done = false; break; }
                if (done) break;
            }
        }
        for (int k = 0; k < 5; ++k) if (lastAttr[k] >= 0) any = true;
        if (any || lastPd >= 0 || lastColN >= 0) {
            if (!t->penState)
                t->penState = std::make_unique<PenState>();
            PenState& ps = *t->penState;
            bool attrWrote = false;
            if (lastAttr[0] >= 0) {
                ps.penSize = lo[size_t(lastAttr[0]) * stride + 3];
                attrWrote = true;
            }
            const int fieldOf[4] = {4, 5, 6, 7};   // hue,sat,bri,transp
            for (int k = 1; k < 5; ++k) {
                if (lastAttr[k] >= 0) {
                    double v = lo[size_t(lastAttr[k]) * stride +
                                   fieldOf[k - 1]];
                    if (k == 1) ps.color = v;
                    else if (k == 2) ps.saturation = v;
                    else if (k == 3) ps.brightness = v;
                    else ps.transparency = v;
                    attrWrote = true;
                }
            }
            // shade は setPenColorToColor (PENCOLN) 経由でのみ
            // 「その時点の brightness/2」に更新される（pen.cpp と
            // 同一規則）— 後の brightness 変更では更新しない。
            if (lastColN >= 0)
                ps.shade = lo[size_t(lastColN) * stride +
                              gpu::laneOutShadeOffset(req.nWb)];
            if (attrWrote) {
                ps.colorDirty = true;
                ps.lastSetColor = ~0ULL;
            }
            if (lastPd >= 0) {
                bool down =
                    (flagAt(lo, stride, uint32_t(lastPd)) & kFPenDown) != 0;
                ps.penDown = down;
                t->onTargetMoved = down ? penrt::movedHook() : nullptr;
            }
        }
    }

    // 変数: 各 wbVar について最後に書いたレーンの値
    {
        const uint32_t mw = (req.nWb + 31) / 32;
        const size_t wbOff = kWbMaskOff + mw;
        for (uint32_t i = 0; i < req.nWb; ++i) {
            for (uint32_t l = lanes; l-- > 0;) {
                if (maskAt(lo, stride, l, i) & (1u << (i % 32))) {
                    float v = lo[size_t(l) * stride + wbOff + i];
                    wbvars[i]->value = Value::num(v);
                    break;
                }
            }
        }
    }
    return 1;
}

} // namespace xyo
