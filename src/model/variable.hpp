// xyo-cpp: engine/variable.js 相当 — Scratch 変数・コスチューム・サウンド。
// Scratch 向け文字列（変数名等）は UTF-16 (std::u16string)、
// 内部 id / 型名は ASCII std::string。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "../core/slist.hpp"
#include "../core/sstr.hpp"
#include "../core/value.hpp"

namespace xyo {

struct Variable {
    static constexpr const char* kScalarType = "";         // Variable.SCALAR_TYPE
    static constexpr const char* kListType = "list";       // Variable.LIST_TYPE
    static constexpr const char* kBroadcastType = "broadcast_msg";

    std::string id;
    std::u16string name;
    std::string type = kScalarType;
    bool isCloud = false;
    /// SCALAR_TYPE / BROADCAST_MESSAGE_TYPE の値（broadcast は name と同一）。
    /// LIST_TYPE のときは list を使う。
    Value value = Value::num(0);
    SList* list = nullptr;

    // Monotonic type feedback for HotSpot compilation. The profile is only
    // a hint: another script, clone or host may change the value at any time.
    // atomic_ref preserves Variable's copyability (required for clones).
    enum ObservedKind : uint32_t {
        kObservedNumber = 1u,
        kObservedString = 2u,
        kObservedBoolean = 4u,
        kObservedColor = 8u,
        kObservedOther = 16u,
    };
    mutable uint32_t observedKinds = 0;

    static uint32_t observedKind(Value v) {
        if (v.isNumber()) return kObservedNumber;
        if (v.isString()) return kObservedString;
        if (v.isBool()) return kObservedBoolean;
        if (v.isColor()) return kObservedColor;
        return kObservedOther;
    }

    void observeValueType(Value v) const {
        auto kinds = std::atomic_ref<uint32_t>(observedKinds);
        const uint32_t kind = observedKind(v);
        if (!(kinds.load(std::memory_order_relaxed) & kind))
            kinds.fetch_or(kind, std::memory_order_relaxed);
    }

    uint32_t typeProfile() const {
        return std::atomic_ref<uint32_t>(observedKinds)
            .load(std::memory_order_relaxed);
    }

    bool observedOnlyNumber() const {
        return typeProfile() == kObservedNumber;
    }   // LIST_TYPE の実体（GC管理）

    static Variable create(std::string id, std::u16string name,
                           std::string type, bool isCloud) {
        Variable v;
        v.id = std::move(id);
        v.name = std::move(name);
        v.type = std::move(type);
        v.isCloud = isCloud;
        if (v.type == kListType) {
            v.list = SList::create();
            v.value = Value::num(0);
        } else if (v.type == kBroadcastType) {
            v.value = Value::string(SStr::create(v.name));
        } else {
            v.value = Value::num(0);
        }
        return v;
    }
};

struct Costume {
    std::string assetId;
    std::u16string name;
    std::string dataFormat;   // "png"/"svg"
    std::string md5;          // md5ext
    double bitmapResolution = 1;
    double rotationCenterX = 0;
    double rotationCenterY = 0;
    int skinId = -1;
    std::vector<uint8_t> data;   // 生アセット（必要なら）
};

struct Sound {
    std::string assetId;
    std::u16string name;
    std::string dataFormat;
    std::string md5;
    std::string format;
    int soundId = -1;            // soundBank 内の再生 id
    double rate = 0;
    double sampleCount = 0;
    std::vector<uint8_t> data;
};

struct Target;
struct Thread;

/// Sprite::soundBank — AudioBank 抽象（ヘッドレスでは null）。
/// playSoundAndWait は再生完了時に resolveWait を呼ぶ責務を持つ。
struct SoundBankAPI {
    virtual ~SoundBankAPI() = default;
    virtual void playSound(Target*, int soundId) { (void)soundId; }
    virtual void playSoundAndWait(Target*, int soundId, Thread*) {}
    virtual void stopAllSounds(Target*) {}
    virtual void stop(Target*, int soundId) { (void)soundId; }
    virtual void setEffects(Target*) {}
};

} // namespace xyo
