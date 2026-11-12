// xyo-cpp: コアブロックパッケージ登録。
#pragma once

#include <string>
#include <vector>

#include "../core/value.hpp"

namespace xyo {
struct Runtime;
struct Target;
struct Thread;
struct Value;

/// Runtime 構築時に全 scratch3_* プリミティブとハットを登録する。
void registerCoreBlocks(Runtime& rt);

/// scratch3_pen 拡張（src/blocks/pen.cpp）。registerCoreBlocks から呼ばれる。
void registerPenBlocks(Runtime& rt);
void registerMusicBlocks(Runtime& rt);
void registerTranslateBlocks(Runtime& rt);

// --- JIT ランタイムヘルパからも再利用するブロック意味論 -------------------

/// looks_say/think のバブル更新（formatBubbleText + onSay 通知込み）。
void updateBubble(Target* t, const char* type, Value message);

/// _setCostume — Scratch 2.0 互換の型分岐（optZeroIndex は sb2 用）。
void setCostumeArg(Target* target, Value requestedCostume, bool optZeroIndex);

/// _setBackdrop — stage 対象。開始された hat スレッドを返す。
std::vector<Thread*> setBackdropArg(Runtime& rt, Target* stage,
                                  Value requestedBackdrop, bool optZeroIndex);

/// motion_movesteps の座標計算 + setXY。
void moveSteps(double steps, Target* t);

/// motion_ifonedgebounce の共通実装（interpreter / JIT 共用）。
/// bounds取得、反射方向、ステージ内への押し戻しを一箇所で管理する。
void bounceOffEdge(Target* target, Runtime* runtime);

/// '_mouse_'/'_random_'/スプライト名 → [x,y]。見つからなければ false。
bool getTargetXY(Value targetName, Runtime& rt, double& tx, double& ty);

/// サウンド名/番号 → sounds index（見つからなければ -1）。
int getSoundIndex(Value soundName, Target* t);

// Exactly one host-side sound playback start. The optional waiting Thread*
// retains the historical promise/wait-queue contract; the host journal
// commits only the start, not completion or any future audio callback.
void startSoundPlayback(Target* target, int soundId, Thread* waitingThread);

// Synchronous create-clone host boundary; cloning itself may start clone
// hats, but no generated code may replay it after a speculative rollback.
void createCloneForTarget(Target* source, Runtime* runtime);

/// looks effect の範囲クランプ。
double clampEffect(const std::u16string& effect, double value);

} // namespace xyo
