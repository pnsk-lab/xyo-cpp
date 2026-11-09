// Real looks bubble + real pen renderer through the opt-in host journal.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "../../src/blocks/core.hpp"
#include "../../src/blocks/pen_api.hpp"
#include "../../src/core/gc.hpp"
#include "../../src/core/sstr.hpp"
#include "../../src/engine/runtime.hpp"
#include "../../src/ir/commit_once_effects.hpp"
#include "../../src/model/target.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace xyo;

namespace {

struct CountingSoundBank final : SoundBankAPI {
    int plays = 0;
    int waits = 0;
    std::vector<int> soundIds;
    void playSound(Target* target, int soundId) override {
        assert(target && target->sprite);
        soundIds.push_back(soundId);
        ++plays;
        Gc::get().collect();
    }
    void playSoundAndWait(Target* target, int soundId, Thread* waiter) override {
        assert(target && target->sprite && waiter);
        soundIds.push_back(soundId);
        ++waits;
        Gc::get().collect();
    }
};

struct CountingRenderer final : RendererAPI {
    int skinCreates = 0;
    int stamps = 0;
    std::vector<int> drawableIds;

    int createPenSkin() override {
        ++skinCreates;
        return 7;
    }
    void penStamp(int skin, int drawable) override {
        assert(skin == 7);
        ++stamps;
        drawableIds.push_back(drawable);
        // The journal must not retain any collected SStr* across delivery.
        Gc::get().collect();
    }
};

} // namespace

int main() {
#if defined(_WIN32)
    _putenv_s("XYO_EFFECT_JOURNAL", "1");
#else
    setenv("XYO_EFFECT_JOURNAL", "1", 1);
#endif

    CountingRenderer renderer;
    CountingSoundBank soundBank;
    Runtime runtime;
    runtime.renderer = &renderer;
    assert(runtime.observableJournal);
    auto sprite = std::make_unique<Sprite>(&runtime);
    sprite->soundBank = &soundBank;
    auto* owner = sprite.get();
    runtime.ownedSprites.push_back(std::move(sprite));
    auto* target = new Target(&runtime, owner);
    target->drawableID = 71;
    runtime.addTarget(target);
    // The original sprite belongs to the Sprite::clones collection (as
    // it does after normal SB3 target loading); new entries are nonoriginal.
    owner->clones.push_back(target);

    std::vector<std::pair<std::string, std::u16string>> bubbles;
    runtime.onSay = [&](Target* gotTarget, const char* type,
                        const std::u16string& contents) {
        assert(gotTarget == target);
        Gc::get().collect();
        bubbles.emplace_back(type, contents);
    };

    updateBubble(target, "say", Value::string(
        SStr::fromUtf8("日本語の表示 🌟")));
    assert(target->bubbleText == u"日本語の表示 🌟");
    assert(target->bubbleType == "say");
    assert(bubbles.size() == 1);
    assert(bubbles.back().second == u"日本語の表示 🌟");
    assert(runtime.observableCommittedEffects == 1);

    penrt::stamp(target, &runtime);
    penrt::stamp(target, &runtime);
    assert(renderer.skinCreates == 1);
    assert(renderer.stamps == 2);
    assert((renderer.drawableIds == std::vector<int>{71, 71}));
    assert(runtime.redrawRequested);
    assert(runtime.observableCommittedEffects == 3);

    updateBubble(target, "think", Value::num(1.234));
    assert(target->bubbleText == u"1.23");
    assert(target->bubbleType == "think");
    assert(bubbles.size() == 2);
    assert(bubbles.back().second == u"1.23");
    assert(runtime.observableCommittedEffects == 4);

    // Nonwaiting sound and async play-until-done both journal the *start*
    // before calling the real SoundBank virtual, while the existing
    // Thread/Promise wait remains the owner's responsibility.
    Thread waiting("waiting-sound");
    startSoundPlayback(target, 27, nullptr);
    startSoundPlayback(target, 28, &waiting);
    assert(soundBank.plays == 1);
    assert(soundBank.waits == 1);
    assert((soundBank.soundIds == std::vector<int>{27, 28}));
    assert(runtime.observableCommittedEffects == 6);
    assert(runtime.observableImmediateEffects == 0);
    assert(runtime.observableSliceId == 6);

    createCloneForTarget(target, &runtime);
    assert(runtime.cloneCounter == 1);
    assert(runtime.targets.size() == 2);
    assert(runtime.targets.back() != target);
    assert(runtime.targets.back()->sprite == owner);
    assert(!runtime.targets.back()->isOriginal);
    assert(owner->clones.size() == 2);
    assert(runtime.observableCommittedEffects == 7);

    // Clone-limit violations are not externally visible effects.
    runtime.maxClones = 1;
    createCloneForTarget(target, &runtime);
    assert(runtime.cloneCounter == 1);
    assert(runtime.targets.size() == 2);
    assert(runtime.observableCommittedEffects == 7);
    assert(runtime.observableImmediateEffects == 0);
    assert(runtime.observableSliceId == 7);
    assert(runtime.observableJournal->pendingCount() == 0);
    assert(runtime.observableJournal->rootedPayloads() == 0);

    std::cout << "PASS actual Runtime Observable journal: 2 bubble "
                 "updates, 2 renderer pen stamps, 2 sound playback starts, "
                 "1 clone creation, Unicode/GC, zero replay or remaining roots\n";
}
