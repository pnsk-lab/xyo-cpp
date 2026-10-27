// xyo-cpp: io/* 移植実装。
#include "io_devices.hpp"

#include <algorithm>
#include <cmath>

#include "../core/cast.hpp"
#include "../model/target.hpp"
#include "runtime.hpp"

namespace xyo {

// ------------------------------------------------------------------ Clock --

Clock::Clock(Runtime* rt) : runtime(rt) {
    timer_ = Timer([rt] { return rt->currentMSecs; });
    timer_.start();
}

double Clock::projectTimer() const {
    if (paused_) return pausedTime_ / 1000.0;
    return timer_.timeElapsed() / 1000.0;
}

void Clock::resetProjectTimer() { timer_.start(); }

void Clock::pause() {
    paused_ = true;
    pausedTime_ = timer_.timeElapsed();
}

void Clock::resume() {
    paused_ = false;
    const double dt = timer_.timeElapsed() - pausedTime_;
    timer_.setStartTime(timer_.startTime() + dt);
}

// --------------------------------------------------------------- Keyboard --

std::u16string Keyboard::keyStringToScratchKey(std::u16string key) {
    static const std::unordered_map<std::u16string, std::u16string> kMap = {
        {u" ", u"space"},
        {u"ArrowLeft", u"left arrow"}, {u"Left", u"left arrow"},
        {u"ArrowUp", u"up arrow"}, {u"Up", u"up arrow"},
        {u"ArrowRight", u"right arrow"}, {u"Right", u"right arrow"},
        {u"Down", u"down arrow"}, {u"ArrowDown", u"down arrow"},
        {u"Enter", u"enter"},
        {u"Backspace", u"backspace"}, {u"Delete", u"delete"},
        {u"Shift", u"shift"}, {u"CapsLock", u"caps lock"},
        {u"ScrollLock", u"scroll lock"}, {u"Control", u"control"},
        {u"Escape", u"escape"}, {u"Insert", u"insert"},
        {u"Home", u"home"}, {u"End", u"end"},
        {u"PageUp", u"page up"}, {u"PageDown", u"page down"},
    };
    auto it = kMap.find(key);
    if (it != kMap.end()) return it->second;
    if (key.size() > 1) return u"";   // 修飾キー等を無視
    return key;
}

std::u16string Keyboard::keyArgToScratchKey(Value keyArg) {
    static const std::set<std::u16string> kKeyNames = {
        u"space", u"left arrow", u"up arrow", u"right arrow", u"down arrow",
        u"enter", u"backspace", u"delete", u"shift", u"caps lock",
        u"scroll lock", u"control", u"escape", u"insert", u"home", u"end",
        u"page up", u"page down"
    };
    if (keyArg.isNumber()) {
        double d = keyArg.asNumberUnchecked();
        if (d >= 48 && d <= 90) {
            return std::u16string(1, char16_t(static_cast<int>(d)));
        }
        switch (static_cast<int>(d)) {
        case 32: return u"space";
        case 37: return u"left arrow";
        case 38: return u"up arrow";
        case 39: return u"right arrow";
        case 40: return u"down arrow";
        }
    }
    std::u16string s(cast::toString(keyArg)->view());
    if (s.size() > 1 && kKeyNames.count(s)) return s;
    if (s.size() > 1) s = s.substr(0, 1);
    if (s == u" ") return u"space";
    if (s == u"\r") return u"enter";
    if (s == std::u16string(1, char16_t(0x1B))) return u"escape";
    return u16ToUpper(s);
}

void Keyboard::postData(std::u16string key, bool isDown, int keyCode) {
    std::u16string cased = keyStringToScratchKey(std::move(key));
    if (cased.empty()) return;
    std::u16string scratchKey = cased.size() == 1 ? u16ToUpper(cased) : cased;
    auto it = std::find(keysPressed_.begin(), keysPressed_.end(), scratchKey);
    if (isDown) {
        lastKeyPressed = cased;
        runtime->emitKeyPressed(scratchKey);
        if (it == keysPressed_.end()) keysPressed_.push_back(scratchKey);
    } else if (it != keysPressed_.end()) {
        keysPressed_.erase(it);
    }
    if (keyCode >= 0) {
        auto kit = keyCodeToKey_.find(keyCode);
        if (kit != keyCodeToKey_.end() && kit->second != scratchKey) {
            auto lit = std::find(keysPressed_.begin(), keysPressed_.end(), kit->second);
            if (lit != keysPressed_.end()) keysPressed_.erase(lit);
        }
        keyCodeToKey_[keyCode] = scratchKey;
    }
}

bool Keyboard::getKeyIsDown(Value keyArg) {
    if (keyArg.isString()) {
        SStr* s = keyArg.asStrPtr();
        if (s && s->view() == u"any") return !keysPressed_.empty();
    }
    std::u16string scratchKey = keyArgToScratchKey(keyArg);
    usedKeys_.insert(scratchKey);
    return std::find(keysPressed_.begin(), keysPressed_.end(), scratchKey)
        != keysPressed_.end();
}

// ------------------------------------------------------------------ Mouse --

static double roundToThree(double n) { return jsnum::jsRound(n * 1000.0) / 1000.0; }

void Mouse::postData(const PostData& data) {
    if (data.hasXY) {
        clientX_ = data.x;
        scratchX_ = std::clamp(
            runtime->stageWidth * ((data.x / data.canvasWidth) - 0.5),
            -runtime->stageWidth / 2.0, runtime->stageWidth / 2.0);
        clientY_ = data.y;
        scratchY_ = std::clamp(
            -runtime->stageHeight * ((data.y / data.canvasHeight) - 0.5),
            -runtime->stageHeight / 2.0, runtime->stageHeight / 2.0);
    }
    if (data.hasIsDown) {
        int button = data.button;
        if (data.isDown) buttons_.insert(button);
        else buttons_.erase(button);

        bool previousDown = isDown_;
        isDown_ = data.isDown;
        if (previousDown == isDown_) return;
        if (data.wasDragged) return;
        if (!(data.x > 0 && data.x < data.canvasWidth &&
              data.y > 0 && data.y < data.canvasHeight)) return;
        Target* target = pickTarget(data.x, data.y);
        if (target) {
            bool isNewDown = !previousDown && isDown_;
            bool isNewUp = previousDown && !isDown_;
            if (target->draggable && isNewUp) activateClickHats(target);
            else if (!target->draggable && isNewDown) activateClickHats(target);
        }
    }
}

void Mouse::activateClickHats(Target* target) {
    runtime->startHats("event_whenthisspriteclicked", nullptr, target);
    runtime->startHats("event_whenstageclicked", nullptr, target);
}

Target* Mouse::pickTarget(double x, double y) {
    if (runtime->renderer) {
        int drawable = runtime->renderer->pick(x, y);
        for (Target* t : runtime->targets)
            if (t->drawableID == drawable) return t;
    }
    return runtime->getTargetForStage();
}

double Mouse::getScratchX() const {
    if (runtime->miscLimits) return std::round(scratchX_);
    return roundToThree(scratchX_);
}
double Mouse::getScratchY() const {
    if (runtime->miscLimits) return std::round(scratchY_);
    return roundToThree(scratchY_);
}

bool Mouse::getButtonIsDown(int button) {
    if (button == 2) usesRightClickDown = true;
    return buttons_.count(button) != 0;
}

} // namespace xyo
