// xyo-cpp: io/*.js 相当 — ランタイム I/O デバイス（ヘッドレス実装）。
#pragma once

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/value.hpp"
#include "timer.hpp"

namespace xyo {

struct Runtime;
struct Target;
struct Variable;

/// io/clock.js — プロジェクトタイマ（秒）。
class Clock {
public:
    explicit Clock(Runtime* rt);
    double projectTimer() const;
    void resetProjectTimer();
    void pause();
    void resume();
private:
    Runtime* runtime;
    Timer timer_;
    bool paused_ = false;
    double pausedTime_ = 0;
};

/// io/keyboard.js — Scratch キー状態。
class Keyboard {
public:
    explicit Keyboard(Runtime* rt) : runtime(rt) {}
    Runtime* runtime;

    /// キー名正規化（DOM key → scratch key）
    static std::u16string keyStringToScratchKey(std::u16string key);
    static std::u16string keyArgToScratchKey(Value keyArg);

    /// postData: DOMイベント相当の入力
    void postData(std::u16string key, bool isDown, int keyCode = -1);
    bool getKeyIsDown(Value keyArg);
    const std::u16string& getLastKeyPressed() const { return lastKeyPressed; }
    bool hasUsedKey(const std::u16string& scratchKey) const {
        return usedKeys_.count(scratchKey) != 0;
    }

private:
    std::vector<std::u16string> keysPressed_;
    std::u16string lastKeyPressed;
    std::unordered_map<int, std::u16string> keyCodeToKey_;
    std::set<std::u16string> usedKeys_;
};

/// io/mouse.js — マウス状態 + クリックハット起動。
class Mouse {
public:
    explicit Mouse(Runtime* rt) : runtime(rt) {}
    Runtime* runtime;

    struct PostData {
        double x = 0, y = 0;              // client 座標
        double canvasWidth = 0, canvasHeight = 0;
        bool hasXY = false;
        bool isDown = false;
        bool hasIsDown = false;
        int button = 0;
        bool wasDragged = false;
    };

    void postData(const PostData& data);
    /// _activateClickHats（テスト・埋め込みから直接起動も可能）
    void activateClickHats(Target* target);
    Target* pickTarget(double x, double y);

    double getClientX() const { return clientX_; }
    double getClientY() const { return clientY_; }
    double getScratchX() const;
    double getScratchY() const;
    bool getIsDown() const { return isDown_; }
    /// JIT 直接フィールドアクセス用（レイアウト計算）
    const bool* isDownPtr() const { return &isDown_; }
    const double* scratchXPtr() const { return &scratchX_; }
    const double* scratchYPtr() const { return &scratchY_; }
    bool getButtonIsDown(int button);

    bool usesRightClickDown = false;

private:
    double clientX_ = 0, clientY_ = 0;
    double scratchX_ = 0, scratchY_ = 0;
    std::set<int> buttons_;
    bool isDown_ = false;
};

/// io/mouseWheel.js — スクロール量。
class MouseWheel {
public:
    explicit MouseWheel(Runtime* rt) : runtime(rt) {}
    Runtime* runtime;
    void postData(double deltaX, double deltaY) {
        scrollX_ += -deltaX;
        scrollY_ += -deltaY;
    }
    double getScrollX() const { return scrollX_; }
    double getScrollY() const { return scrollY_; }
private:
    double scrollX_ = 0, scrollY_ = 0;
};

/// io/userData.js — ユーザー名。
class UserData {
public:
    void postData(std::u16string username) { username_ = std::move(username); }
    const std::u16string& getUsername() const { return username_; }
private:
    std::u16string username_;
};

/// io/cloud.js — クラウド変数プロバイダ（ヘッドレスは no-op）。
class Cloud {
public:
    explicit Cloud(Runtime* rt) : runtime(rt) {}
    Runtime* runtime;
    void requestCreateVariable(Variable*) {}
    void requestUpdateVariable(const std::u16string&, const std::u16string&) {}
    void requestRenameVariable(const std::u16string&, const std::u16string&) {}
    void requestDeleteVariable(const std::u16string&) {}
    void clear() {}
};

/// io/video.js — ビデオ（no provider → ready=false）。
class Video {
public:
    explicit Video(Runtime* rt) : runtime(rt) {}
    Runtime* runtime;
    bool videoReady = false;
};

struct IoDevices {
    Clock clock;
    Keyboard keyboard;
    Mouse mouse;
    MouseWheel mouseWheel;
    UserData userData;
    Cloud cloud;
    Video video;
    explicit IoDevices(Runtime* rt)
        : clock(rt), keyboard(rt), mouse(rt), mouseWheel(rt),
          cloud(rt), video(rt) {}
};

} // namespace xyo
