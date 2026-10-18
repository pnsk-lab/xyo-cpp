// xyo-cpp: SDL2 オーディオバンク（xyo play 用）。
// SoundBankAPI + AudioAPI の実装。WAV(SDL_LoadWAV)/MP3(dr_mp3) を
// float32 にデコードし、ボイスごとに音量/pitch/pan を反映してミックス。
// playSoundAndWait の完了通知は update() がメインスレッドで resolveWait。
#pragma once

#include <deque>
#include <unordered_map>
#include <vector>

#include "../engine/runtime.hpp"
#include "../model/variable.hpp"

namespace xyo {

class SdlAudioBank : public SoundBankAPI, public AudioAPI {
public:
    SdlAudioBank() = default;
    ~SdlAudioBank() override;

    SdlAudioBank(const SdlAudioBank&) = delete;
    SdlAudioBank& operator=(const SdlAudioBank&) = delete;

    /// オーディオデバイスを開き、runtime の全 sprite に soundBank を設定。
    /// デバイスが開けなくても bank 自体は有効（無音）。成功時 true。
    bool init(Runtime& rt);
    /// メインスレッドから毎フレーム呼ぶ: 完了 wait の resolveWait +
    /// loudness 減衰。
    void update();
    /// 全ボイス停止（stopAll/greenFlag 用。wait 中スレッドは解決される）
    void stopAll();
    void shutdown();
    bool ok() const { return dev_ != 0; }

    // --- SoundBankAPI ---
    void playSound(Target*, int soundId) override;
    void playSoundAndWait(Target*, int soundId, Thread*) override;
    void stopAllSounds(Target*) override;   // TW: 全ボイス停止
    void stop(Target*, int soundId) override;
    void setEffects(Target*) override {}    // ボイスが target を生参照するため no-op

    // --- AudioAPI ---
    double getLoudness() const override;

private:
    struct Sample {
        std::vector<float> pcm;   // float32 interleaved
        int ch = 2;
        int rate = 48000;
        int frames = 0;           // pcm.size()/ch
    };
    struct Voice {
        const Sample* s = nullptr;
        double pos = 0;
        Target* t = nullptr;
        Thread* wait = nullptr;
    };

    static void audioCb(void* ud, unsigned char* buf, int len);
    void mix(float* out, int frames);
    const Sample* sampleFor(Target* t, int soundId);
    static bool decodeSound(const Sound& s, Sample& out);
    void finishAllVoices();

    Runtime* rt_ = nullptr;
    unsigned int dev_ = 0;        // SDL_AudioDeviceID（型を隠すため unsigned）
    int outRate_ = 48000;
    std::unordered_map<const Sound*, Sample> decoded_;
    std::unordered_map<const Sound*, bool> decodeFailed_;
    std::vector<Voice> voices_;
    std::deque<Voice> done_;      // 完了済み（wait 解決待ち）
    float peak_ = 0.0f;
};

} // namespace xyo
