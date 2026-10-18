// xyo-cpp: scratch3_pen 拡張（refs/scratch-vm/src/extensions/scratch3_pen/index.js）。
// ペン状態は Target::penState に保持し、移動は Target::onTargetMoved フックで追跡する。
// renderer 非接続（ヘッドレス）では全て no-op。

#include "pen_api.hpp"

#include <cmath>
#include <memory>
#include <string>

#include "../core/cast.hpp"
#include "../core/color.hpp"
#include "../engine/block_utility.hpp"
#include "../engine/runtime.hpp"
#include "../model/target.hpp"

namespace xyo {
namespace {

PenState& penStateOf(Target* t) {
    if (!t->penState) t->penState = std::make_unique<PenState>();
    return *t->penState;
}

// JS: _updatePenColor — HSV+transparency → color4f
void updatePenColor(PenState& p) {
    const Rgb rgb =
        color::hsvToRgb({p.color * 360.0 / 100.0, p.saturation / 100.0,
                         p.brightness / 100.0});
    p.rgba[0] = rgb.r / 255.0f;
    p.rgba[1] = rgb.g / 255.0f;
    p.rgba[2] = rgb.b / 255.0f;
    p.rgba[3] = 1.0f - static_cast<float>(p.transparency / 100.0);
}

/// rgba の遅延同期 — 描画読み取り前に必ず呼ぶ。
/// 状態変更は colorDirty 立てのみ（hsvToRgb をスキップ）。
void syncPenColor(PenState& p) {
    if (p.colorDirty) {
        updatePenColor(p);
        p.colorDirty = false;
    }
}

// JS: MathUtil.wrapClamp(value, 0, 100)
double wrapColor(double v) {
    v = std::fmod(v, 100.0);
    if (v < 0) v += 100.0;
    return v;
}
double clampColorParam(double v) { return std::clamp(v, 0.0, 100.0); }

// JS: _getPenLayerID — レンダラのペン skin を遅延生成
int penSkin(Runtime* rt) {
    if (!rt->renderer) return -1;
    if (rt->penSkinId < 0) rt->penSkinId = rt->renderer->createPenSkin();
    return rt->penSkinId;
}

// JS: _clampPenSize — useHighQualityRender 相当は未実装のため miscLimits のみ
double clampPenSize(Runtime* rt, double size) {
    if (!rt->miscLimits) return std::max(0.0, size);
    return std::clamp(size, 1.0, 1200.0);
}

// JS: _onTargetMoved — ペン下げ中の移動で線を引く（force=ドラッグは無視）
void penOnTargetMoved(Target* t, double oldX, double oldY, bool force) {
    if (force || !t->penState || !t->penState->penDown) return;
    Runtime* rt = t->runtime;
    int skin = penSkin(rt);
    if (skin >= 0) {
        PenState& p = *t->penState;
        syncPenColor(p);
        rt->renderer->penLine(skin, p.rgba, p.penSize, oldX, oldY, t->x, t->y);
        rt->redrawRequested = true;
    }
}

// JS: _setOrChangeColorParam
void setOrChangeColorParam(std::u16string_view param, double value,
                           PenState& p, bool change) {
    if (param == u"color") {
        p.color = wrapColor(value + (change ? p.color : 0));
    } else if (param == u"saturation") {
        p.saturation = clampColorParam(value + (change ? p.saturation : 0));
    } else if (param == u"brightness") {
        p.brightness = clampColorParam(value + (change ? p.brightness : 0));
    } else if (param == u"transparency") {
        p.transparency = clampColorParam(value + (change ? p.transparency : 0));
    }
    p.colorDirty = true;
    p.lastSetColor = ~0ULL;
}

// JS: _legacyUpdatePenColor — scratch2 の shade モデルで RGB 計算し HSV に戻す
void legacyUpdatePenColor(PenState& p) {
    RgbF rgb = [&] {
        Rgb base =
            color::hsvToRgb({p.color * 360.0 / 100.0, 1.0, 1.0});
        const double shade = p.shade > 100 ? 200.0 - p.shade : p.shade;
        if (shade < 50)
            return color::mixRgb({0, 0, 0}, base, (10.0 + shade) / 60.0);
        return color::mixRgb(base, {255, 255, 255}, (shade - 50.0) / 60.0);
    }();
    // rgbToHsv は整数入力 — JS と同じ値に近づけるため丸めて変換
    Hsv hsv = color::rgbToHsv({static_cast<int>(std::lround(rgb.r)),
                                      static_cast<int>(std::lround(rgb.g)),
                                      static_cast<int>(std::lround(rgb.b))});
    p.color = 100.0 * hsv.h / 360.0;
    p.saturation = 100.0 * hsv.s;
    p.brightness = 100.0 * hsv.v;
    p.colorDirty = true;
    p.lastSetColor = ~0ULL;
}

// JS: _setPenShadeToNumber — shade を %200 で正規化
void setPenShade(Target* t, double shade) {
    PenState& p = penStateOf(t);
    shade = std::fmod(shade, 200.0);
    if (shade < 0) shade += 200.0;
    p.shade = shade;
    legacyUpdatePenColor(p);
}

Value argV(const ArgValues& a, const char* n) { return a.get(n); }

// --- 共有コア（プリミティブ・JIT ヘルパ双方が呼ぶ。pen_api 経由）---

void penDownCore(Target* t, Runtime* rt) {
    PenState& p = penStateOf(t);
    if (!p.penDown) {
        p.penDown = true;
        t->onTargetMoved = penOnTargetMoved;
    }
    int skin = penSkin(rt);
    if (skin >= 0) {
        syncPenColor(p);
        rt->renderer->penPoint(skin, p.rgba, p.penSize, t->x, t->y);
        rt->redrawRequested = true;
    }
}
void penUpCore(Target* t) {
    PenState& p = penStateOf(t);
    p.penDown = false;
    t->onTargetMoved = nullptr;
}
void penClearCore(Runtime* rt) {
    int skin = penSkin(rt);
    if (skin >= 0) {
        rt->renderer->penClear(skin);
        rt->redrawRequested = true;
    }
}
void penStampCore(Target* t, Runtime* rt) {
    int skin = penSkin(rt);
    if (skin >= 0) {
        rt->renderer->penStamp(skin, t->drawableID);
        rt->redrawRequested = true;
    }
}
void penSetColorToColorCore(Target* t, Value color) {
    PenState& p = penStateOf(t);
    const Rgba rgb = cast::toRgbColorObject(color);
    const Hsv hsv = color::rgbToHsv({rgb.r, rgb.g, rgb.b});
    p.color = (hsv.h / 360.0) * 100.0;
    p.saturation = hsv.s * 100.0;
    p.brightness = hsv.v * 100.0;
    // JS は hasOwnProperty('a') で判定するが、decimalToRgb は無ければ 255 を返す
    p.transparency = rgb.a < 255 ? 100.0 * (1.0 - rgb.a / 255.0) : 0.0;
    p.shade = p.brightness / 2.0;   // scratch2 と同じ更新
    p.colorDirty = true;
    p.lastSetColor = color.bits;
}
void penColorParamCore(Target* t, std::u16string_view param, Value v,
                       bool change) {
    PenState& p = penStateOf(t);
    setOrChangeColorParam(param, cast::toNumber(v), p, change);
}
void penSizeCore(Target* t, Runtime* rt, Value v, bool change) {
    PenState& p = penStateOf(t);
    const double d = cast::toNumber(v);
    p.penSize = clampPenSize(rt, change ? p.penSize + d : d);
}

Value penClear(const ArgValues&, BlockUtility& u) {
    penClearCore(u.runtime());
    return Value::undefined();
}

Value penStamp(const ArgValues&, BlockUtility& u) {
    penStampCore(u.target(), u.runtime());
    return Value::undefined();
}

Value penDown(const ArgValues&, BlockUtility& u) {
    penDownCore(u.target(), u.runtime());
    return Value::undefined();
}

Value penUp(const ArgValues&, BlockUtility& u) {
    penUpCore(u.target());
    return Value::undefined();
}

Value penSetColorToColor(const ArgValues& a, BlockUtility& u) {
    penSetColorToColorCore(u.target(), argV(a, "COLOR"));
    return Value::undefined();
}

Value penChangeColorParamBy(const ArgValues& a, BlockUtility& u) {
    SStr* s = cast::toString(argV(a, "COLOR_PARAM"));
    penColorParamCore(u.target(),
                      s ? s->view() : std::u16string_view{},
                      argV(a, "VALUE"), true);
    return Value::undefined();
}
Value penSetColorParamTo(const ArgValues& a, BlockUtility& u) {
    SStr* s = cast::toString(argV(a, "COLOR_PARAM"));
    penColorParamCore(u.target(),
                      s ? s->view() : std::u16string_view{},
                      argV(a, "VALUE"), false);
    return Value::undefined();
}

Value penChangeSizeBy(const ArgValues& a, BlockUtility& u) {
    penSizeCore(u.target(), u.runtime(), argV(a, "SIZE"), true);
    return Value::undefined();
}
Value penSetSizeTo(const ArgValues& a, BlockUtility& u) {
    penSizeCore(u.target(), u.runtime(), argV(a, "SIZE"), false);
    return Value::undefined();
}

// --- レガシー opcodes（scratch2 互換）---
Value penChangeHueBy(const ArgValues& a, BlockUtility& u) {
    PenState& p = penStateOf(u.target());
    setOrChangeColorParam(u"color", cast::toNumber(argV(a, "HUE")) / 2.0,
                          p, true);
    legacyUpdatePenColor(p);
    return Value::undefined();
}
Value penSetHueToNumber(const ArgValues& a, BlockUtility& u) {
    PenState& p = penStateOf(u.target());
    setOrChangeColorParam(u"color", cast::toNumber(argV(a, "HUE")) / 2.0,
                          p, false);
    setOrChangeColorParam(u"transparency", 0.0, p, false);
    legacyUpdatePenColor(p);
    return Value::undefined();
}
Value penChangeShadeBy(const ArgValues& a, BlockUtility& u) {
    PenState& p = penStateOf(u.target());
    setPenShade(u.target(), p.shade + cast::toNumber(argV(a, "SHADE")));
    return Value::undefined();
}
Value penSetShadeToNumber(const ArgValues& a, BlockUtility& u) {
    setPenShade(u.target(), cast::toNumber(argV(a, "SHADE")));
    return Value::undefined();
}

// メニュー shadow — 自身のフィールド値をそのまま返す。
// 実 SB3 のフィールド名は colorParam（小文字。親の入力名
// COLOR_PARAM ではない）。definedBlockFunction があるため
// isShadowBlock にならず独立 op として走り、戻り値が親の
// argValues の COLOR_PARAM へ書き戻される。JIT の penParamKind が
// fields.begin() を読むのと同じく名前非依存にするため、
// 唯一のフィールド値へフォールバックする。
Value penMenuColorParam(const ArgValues& a, BlockUtility&) {
    if (Value v = argV(a, "colorParam"); !v.isUndefined()) return v;
    return a.vals.empty() ? Value::undefined() : a.vals.begin()->second;
}

} // namespace

void registerPenBlocks(Runtime& rt) {
    auto prim = [&](const char* op, PrimFn fn) { rt.registerPrimitive(op, fn); };
    prim("pen_clear", penClear);
    prim("pen_stamp", penStamp);
    prim("pen_penDown", penDown);
    prim("pen_penUp", penUp);
    prim("pen_setPenColorToColor", penSetColorToColor);
    prim("pen_changePenColorParamBy", penChangeColorParamBy);
    prim("pen_setPenColorParamTo", penSetColorParamTo);
    prim("pen_changePenSizeBy", penChangeSizeBy);
    prim("pen_setPenSizeTo", penSetSizeTo);
    prim("pen_changePenHueBy", penChangeHueBy);
    prim("pen_setPenHueToNumber", penSetHueToNumber);
    prim("pen_changePenShadeBy", penChangeShadeBy);
    prim("pen_setPenShadeToNumber", penSetShadeToNumber);
    prim("pen_menu_colorParam", penMenuColorParam);
}

// --- JIT ヘルパ用ファサード（pen_api.hpp）---
namespace penrt {
void up(Target* t, Runtime*) { penUpCore(t); }
void down(Target* t, Runtime* rt) { penDownCore(t, rt); }
void clear(Runtime* rt) { penClearCore(rt); }
void stamp(Target* t, Runtime* rt) { penStampCore(t, rt); }
void setColorToColor(Target* t, Value color) {
    penSetColorToColorCore(t, color);
}
void setColorToColorState(PenState& p, Value color) {
    const Rgba rgb = cast::toRgbColorObject(color);
    const Hsv hsv = color::rgbToHsv({rgb.r, rgb.g, rgb.b});
    p.color = (hsv.h / 360.0) * 100.0;
    p.saturation = hsv.s * 100.0;
    p.brightness = hsv.v * 100.0;
    p.transparency = rgb.a < 255 ? 100.0 * (1.0 - rgb.a / 255.0) : 0.0;
    p.shade = p.brightness / 2.0;
    p.colorDirty = true;
    p.lastSetColor = color.bits;
}
void colorParam(Target* t, std::u16string_view param, Value v, bool change) {
    penColorParamCore(t, param, v, change);
}
void size(Target* t, Runtime* rt, Value v, bool change) {
    penSizeCore(t, rt, v, change);
}

MovedHook movedHook() { return &penOnTargetMoved; }
void recolor(Target* t) { updatePenColor(*t->penState); }
void colorParamNum(Target* t, int kind, double v, bool change) {
    PenState& p = penStateOf(t);
    switch (kind) {
    case 0: p.color = wrapColor(v + (change ? p.color : 0)); break;
    case 1: p.saturation =
              clampColorParam(v + (change ? p.saturation : 0)); break;
    case 2: p.brightness =
              clampColorParam(v + (change ? p.brightness : 0)); break;
    case 3: p.transparency =
              clampColorParam(v + (change ? p.transparency : 0)); break;
    }
    p.colorDirty = true;
    p.lastSetColor = ~0ULL;
}
} // namespace penrt

} // namespace xyo
