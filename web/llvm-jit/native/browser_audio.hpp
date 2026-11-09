#pragma once
// Browser sound command queue. No JS reentry; audio is decoded by the main
// browser thread and completion is acknowledged on the runtime Worker.
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "model/target.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace xyo::browser {
class AudioBank final : public SoundBankAPI {
public:
    struct Event {
        int kind = 0;  // 1 = start, 2 = stop-all
        uint32_t id = 0;
        std::string format;
        std::vector<uint8_t> bytes;
        double volume = 100, pitch = 0, pan = 0;
    };

    explicit AudioBank(Runtime& runtime) : rt_(runtime) {
        for (auto& sprite : rt_.ownedSprites) sprite->soundBank = this;
        rt_.onProjectStopped = [this]() { stopAll(); };
        rt_.onTargetRemoved = [this](Target* t) { forget(t); };
    }
    ~AudioBank() override {
        rt_.onProjectStopped = nullptr;
        rt_.onTargetRemoved = nullptr;
        for (auto& sprite : rt_.ownedSprites) if (sprite->soundBank == this) sprite->soundBank = nullptr;
    }

    void playSound(Target* target, int id) override { start(target, id, nullptr); }
    void playSoundAndWait(Target* target, int id, Thread* thread) override {
        start(target, id, thread);
    }
    void stopAllSounds(Target*) override { stopAll(); }
    void stop(Target*, int) override { stopAll(); }
    void setEffects(Target*) override { /* Applied to subsequent voices. */ }

    size_t size() const { return events_.size(); }
    const Event* event(size_t index) const {
        return index < events_.size() ? &events_[index] : nullptr;
    }
    void consume() { events_.clear(); }

    void acknowledge(uint32_t id) {
        auto it = voices_.find(id);
        if (it == voices_.end()) return;
        Thread* thread = it->second.second;
        voices_.erase(it);
        // Verify the address is a live thread before dereferencing: finished
        // threads may have been reclaimed by the current xyo scheduler.
        if (!thread || std::find(rt_.threads.begin(), rt_.threads.end(), thread) == rt_.threads.end()) return;
        if (!thread->isKilled && thread->status == STATUS_PROMISE_WAIT)
            resolveWait(*rt_.sequencer, *thread, Value::undefined());
    }
private:
    void start(Target* target, int soundId, Thread* thread) {
        if (!target || !target->sprite || soundId < 0 ||
            static_cast<size_t>(soundId) >= target->sprite->sounds.size()) return;
        if (events_.size() >= 256 || voices_.size() >= 256 || nextId_ == 0) return;
        const Sound& sound = target->sprite->sounds[soundId];
        if (sound.data.size() > 8 * 1024 * 1024) return;
        Event e;
        e.kind = 1;
        e.id = nextId_++;
        e.format = sound.dataFormat;
        e.bytes = sound.data;
        e.volume = target->volume;
        auto pitch = target->soundEffects.find("pitch");
        auto pan = target->soundEffects.find("pan");
        if (pitch != target->soundEffects.end()) e.pitch = pitch->second;
        if (pan != target->soundEffects.end()) e.pan = pan->second;
        voices_.emplace(e.id, std::make_pair(target, thread));
        events_.push_back(std::move(e));
    }
    void stopAll() {
        if (events_.size() < 256) {
            Event e;
            e.kind = 2;
            events_.push_back(std::move(e));
        }
        voices_.clear();
    }
    void forget(Target* target) {
        for (auto it = voices_.begin(); it != voices_.end();) {
            if (it->second.first == target) it = voices_.erase(it);
            else ++it;
        }
    }
    Runtime& rt_;
    uint32_t nextId_ = 1;
    std::map<uint32_t, std::pair<Target*, Thread*>> voices_;
    std::vector<Event> events_;
};
} // namespace xyo::browser
