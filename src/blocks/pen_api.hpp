// xyo-cpp: pen プリミティブの共有コア API。
// blocks/pen.cpp の実装を JIT ヘルパ（rt_blocks.cpp）からも
// ArgValues を介さず直接呼べるようにする薄いファサード。
#pragma once

#include <string>

#include "../core/value.hpp"

namespace xyo {

struct Target;
struct Runtime;
struct PenState;

namespace penrt {
void up(Target* t, Runtime* rt);
void down(Target* t, Runtime* rt);
void clear(Runtime* rt);
void stamp(Target* t, Runtime* rt);
void setColorToColor(Target* t, Value color);
void colorParam(Target* t, std::u16string_view param, Value v, bool change);
void size(Target* t, Runtime* rt, Value v, bool change);

// --- JIT inline 用 -------------------------------------------------------
/// penDownCore が Target::onTargetMoved に設定するフックのアドレス。
/// 生成コードがフックポインタを直接 store するために使う。
using MovedHook = void (*)(Target*, double, double, bool);
MovedHook movedHook();
/// updatePenColor 相当（PenState は必ず存在する前提 — 生成コードが
/// penState != null を確認した経路からのみ呼ぶ）。
void recolor(Target* t);
/// kind(0=color,1=saturation,2=brightness,3=transparency) 指定の
/// setOrChangeColorParam + updatePenColor。penState 遅延生成込み。
void colorParamNum(Target* t, int kind, double v, bool change);
/// penSetColorToColorCore の純粋計算部 — PenState を直接書き換える
/// （JIT が定数 COLOR をコンパイル時に畳み込む用途）。
void setColorToColorState(PenState& p, Value color);
} // namespace penrt

} // namespace xyo
