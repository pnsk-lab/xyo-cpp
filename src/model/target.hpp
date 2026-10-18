// xyo-cpp: engine/target.js + sprites/rendered-target.js + sprite.js 相当。
// Target と RenderedTarget は単一クラスに統合（ヘッドレス実行では
// 描画依存部分は renderer 非所有でも意味を持つ状態として保持する）。
#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/json.hpp"
#include "blocks.hpp"
#include "variable.hpp"

namespace xyo {

struct Runtime;
struct Target;

/// Sprite — クローン間で共有されるブロック/コスチューム/サウンド群。
struct Sprite {
    explicit Sprite(Runtime* rt);

    Runtime* runtime;
    std::unique_ptr<Blocks> blocks;
    std::u16string name;
    std::vector<Costume> costumes;
    std::vector<Sound> sounds;
    SoundBankAPI* soundBank = nullptr;   // 非所有
    std::vector<Target*> clones;

    Target* createClone(const std::string& layerGroup = "");
    void removeClone(Target* clone);
};

/// Scratch.pen 拡張の per-target 状態（JS: customState["Scratch.pen"]）。
/// ペン拡張の初回使用時に遅延生成される。penAttributes は rgba+penSize。
struct PenState {
    bool penDown = false;
    double color = 66.66;
    double saturation = 100;
    double brightness = 100;
    double transparency = 0;
    double shade = 50;               // レガシー "shade"（JS: _shade）
    double penSize = 1;              // penAttributes.diameter
    float rgba[4] = {0, 0, 1, 1};    // penAttributes.color4f
    /// color/sat/bright/transp が rgba に未反映 —
    /// updatePenColor (hsvToRgb) を描画時まで遅延する。
    /// 消費側（penLine/penPoint）は syncPenColor で同期。
    bool colorDirty = false;
    /// setPenColorToColor のメモ — 最後に適用した COLOR arg の
    /// Value bits。色フィールドを変更する他 op は ~0 を書いて
    /// 無効化する（~0 は有効な Value bits になり得ないタグ）。
    uint64_t lastSetColor = ~0ULL;
};

/// Target — 実行対象（Sprite 共有 + クローン個別状態）。
class Target {
public:
    explicit Target(Runtime* rt, Sprite* sprite = nullptr);

    Runtime* runtime;
    std::string id;
    Sprite* sprite;            // nullptr の場合は独立 target（monitor 等）
    Blocks* blocks;            // sprite->blocks.get() への非所有参照

    bool isStage = false;
    bool isOriginal = true;
    int layerOrder = 0;        // sprite pane 順（実行順は別）
    int drawableID = -1;       // renderer.drawableID（renderer 接続時のみ）
    int drawableLayerOrder = 0;

    // --- 変数（id → Variable）---
    std::unordered_map<std::string, Variable> variables;
    /// 実行中スクリプトコメント（未使用だが保持）
    std::map<std::string, JsonValue> comments;
    /// 拡張ごとの保持データ
    std::map<std::string, JsonValue> extensionStorage;

    // --- RenderedTarget 状態 ---
    double x = 0, y = 0;
    double direction = 90;
    bool draggable = false;
    bool dragging = false;              // isTouchingSprite フィルタ用
    bool visible = true;
    double size = 100;
    int currentCostume = 0;
    std::string rotationStyle = "all around"; // ROTATION_STYLE_ALL_AROUND
    double volume = 100;
    double tempo = 60;                    // stage
    double videoTransparency = 50;        // stage
    std::string videoState = "on";        // stage: on/off/on-flipped
    std::string textToSpeechLanguage;     // stage
    std::map<std::string, double> effects;
    std::map<std::string, double> soundEffects; // Scratch.sound state.effects
    /// ペン拡張状態（pen.cpp が遅延生成。clone 時に複製）
    std::unique_ptr<PenState> penState;
    /// ペン用移動フック（penDown 中のみセット。JS: target.onTargetMoved）
    void (*onTargetMoved)(Target*, double oldX, double oldY, bool force) = nullptr;
    /// Scratch.looks バブル状態（formatter 済みテキスト・型・usageId）
    std::u16string bubbleText;
    std::string bubbleType = "say";
    int bubbleUsageId = 0;

    /// エッジ起動ハットの直近値
    std::unordered_map<std::string, Value> edgeActivatedHatValues;

    /// clone 用カウンタ（Clone counter）
    int cloneCount = 0;

    // ---------------------------------------------------------------
    std::u16string getName() const { return sprite ? sprite->name : utf8ToU16(id); }
    bool isRenderable() const { return sprite != nullptr; }
    const std::vector<Costume>* getCostumes() const {
        return sprite ? &sprite->costumes : nullptr;
    }

    // --- 変数ルックアップ（target.js 移植）---
    Variable* lookupVariableById(const std::string& id);
    Variable* lookupVariableByNameAndType(const std::u16string& name,
                                          const char* type,
                                          bool skipStage = false);
    Variable* lookupOrCreateVariable(const std::string& id, const std::u16string& name);
    Variable* lookupOrCreateList(const std::string& id, const std::u16string& name);
    Variable* lookupBroadcastMsg(const std::string& id, const std::u16string& name);
    Variable* lookupBroadcastByInputValue(const std::u16string& name);
    Variable* createVariable(const std::string& id, const std::u16string& name,
                             const char* type, bool isCloud = false);
    void renameVariable(const std::string& id, const std::u16string& newName);
    void deleteVariable(const std::string& id);

    /// say/think バブル状態の更新 + onSay 通知（looks + ask 共通経路）
    void setSay(const char* type, const std::u16string& text);

    // --- RenderedTarget メソッド（renderer 不在時は状態のみ更新）---
    void setXY(double nx, double ny, bool force = false);
    void setDirection(double dir);
    void setDraggable(bool d);
    void setRotationStyle(const std::string& style);
    void setVisible(bool v);
    void setSize(double s);
    /// index は JS Math.round 済み前提の double（Infinity/0 → 0）
    void setCostume(double index);
    int getCostumeIndexByName(const std::u16string& name) const;
    const Costume* getCurrentCostume() const;
    void setEffect(const std::string& name, double value);
    void clearEffects();
    void updateAllDrawableProperties();
    void emitVisualChange();
    void postSpriteInfo();
    void goToFront();
    void goToBack();
    void goForwardLayers(double n);
    void goBackwardLayers(double n);
    void goBehindOther(Target* other);

    struct Bounds { double left, right, top, bottom; };
    /// renderer.getBounds。renderer 不在なら false
    bool getBounds(Bounds& out) const;
    bool isTouchingObject(const std::u16string& obj);
    bool isTouchingPoint(double px, double py) const;
    bool isTouchingEdge() const;
    bool isTouchingSprite(const std::u16string& spriteName);
    bool isTouchingColor(uint32_t rgb) const;
    bool colorIsTouchingColor(uint32_t targetRgb, uint32_t maskRgb) const;
    /// keepInFence — bounds が得られなければ false（JS: undefined 返し）
    bool keepInFence(double nx, double ny, double& fx, double& fy) const;

    // --- クローン ---
    Target* makeClone();
    void dispose();
    std::unordered_map<std::string, Variable> duplicateVariables();

    // --- エッジ起動ハット ---
    Value updateEdgeActivatedValue(const std::string& blockId, Value newValue);
    bool hasEdgeActivatedValue(const std::string& blockId) const;
    void clearEdgeActivatedValues() { edgeActivatedHatValues.clear(); }

    // --- イベント ---
    void onGreenFlag();
    void onStopAll();
};

} // namespace xyo
