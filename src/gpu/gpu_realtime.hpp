// xyo-cpp: GPU real-time ray/path プレイヤ。
// --gpu-realtime 有効時に r.render / pt.pass の描画ループを
// ネイティブ compute dispatch に置き換えるアダプタ。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "gpu_native.hpp"

namespace xyo {
class Runtime;
}

namespace xyo::gpu {

class RealtimePlayer {
public:
    RealtimePlayer();
    ~RealtimePlayer();

    RealtimePlayer(const RealtimePlayer&) = delete;
    RealtimePlayer& operator=(const RealtimePlayer&) = delete;

    /// rt にパッチを適用して real-time モードを開始する。
    /// 非対応プロジェクトでは false を返し、Runtime/Blocks は無変更。
    bool init(Runtime& rt, RealtimeMode mode, std::string* why = nullptr);

    uint64_t completedFrames() const;
    uint64_t samples() const;
    bool usingGpu() const;
    bool active() const;

    /// パッチ解除（全スレッド停止後に呼ぶ — デストラクタでも実行）。
    void uninstall();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
