// xyo-cpp: SDL2 オーディオバンク実装。
// 参照: refs/scratch-vm/node_modules/scratch-audio/src/
//   PitchEffect: ratio = 2^(value/120)（10=1半音, 120=1オクターブ）
//   PanEffect:   p=(v+100)/200; L=cos(p*π/2), R=sin(p*π/2)（等ラウドネス）
//   音量: target->volume/100（クランプ済み想定）

#include "sdl_audio.hpp"

#if defined(XYO_HAVE_SDL2)

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// M_PI は ISO C++ 標準ではない（mingw の <cmath> では未定義）
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../../third_party/dr_mp3.h"

namespace xyo {

bool SdlAudioBank::init(Runtime& rt) {
    rt_ = &rt;
    // SDL_main を使わないので明示的に main ready を立てる（Windows で必須）
    SDL_SetMainReady();
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return false;
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_F32;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &SdlAudioBank::audioCb;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev_) {
        outRate_ = have.freq;
        SDL_PauseAudioDevice(dev_, 0);
    }
    // 全 sprite に soundBank を設定（clone は Sprite* を共有）
    for (Target* t : rt_->targets)
        if (t->sprite) t->sprite->soundBank = this;
    rt_->audioEngine = this;
    rt_->onProjectStopped = [this] { stopAll(); };
    return true;
}

SdlAudioBank::~SdlAudioBank() { shutdown(); }

void SdlAudioBank::shutdown() {
    if (dev_) {
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
    if (rt_) {
        if (rt_->audioEngine == this) rt_->audioEngine = nullptr;
        rt_->onProjectStopped = nullptr;
        for (Target* t : rt_->targets)
            if (t->sprite && t->sprite->soundBank == this)
                t->sprite->soundBank = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

// ------------------------------------------------------------ デコード --

bool SdlAudioBank::decodeSound(const Sound& s, Sample& out) {
    if (s.data.size() < 12) return false;
    const uint8_t* d = s.data.data();
    const bool isWav = d[0] == 'R' && d[1] == 'I' && d[2] == 'F' &&
                       d[3] == 'F' && d[8] == 'W' && d[9] == 'A' &&
                       d[10] == 'V' && d[11] == 'E';
    const bool isMp3 = s.dataFormat == "mp3" ||
                       (d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) ||
                       (d[0] == 'I' && d[1] == 'D' && d[2] == '3');
    if (isWav) {
        SDL_RWops* rw =
            SDL_RWFromConstMem(s.data.data(),
                               static_cast<int>(s.data.size()));
        if (!rw) return false;
        SDL_AudioSpec spec;
        Uint8* buf = nullptr;
        Uint32 len = 0;
        if (!SDL_LoadWAV_RW(rw, 1, &spec, &buf, &len)) return false;
        // float32 に変換（ch/rate はソース維持 — ミキサでリサンプル）
        SDL_AudioCVT cvt;
        if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq,
                              AUDIO_F32, spec.channels, spec.freq) < 0) {
            SDL_FreeWAV(buf);
            return false;
        }
        cvt.len = static_cast<int>(len);
        cvt.buf = static_cast<Uint8*>(
            SDL_malloc(static_cast<size_t>(cvt.len * cvt.len_mult)));
        if (!cvt.buf) { SDL_FreeWAV(buf); return false; }
        std::memcpy(cvt.buf, buf, len);
        SDL_FreeWAV(buf);
        if (SDL_ConvertAudio(&cvt) != 0) {
            SDL_free(cvt.buf);
            return false;
        }
        out.pcm.assign(reinterpret_cast<float*>(cvt.buf),
                       reinterpret_cast<float*>(cvt.buf) +
                           static_cast<size_t>(cvt.len_cvt) / 4);
        SDL_free(cvt.buf);
        out.ch = spec.channels;
        out.rate = spec.freq;
        out.frames = static_cast<int>(out.pcm.size() / out.ch);
        return out.frames > 0;
    }
    if (isMp3) {
        drmp3 mp3;
        if (!drmp3_init_memory(&mp3, d, s.data.size(), nullptr))
            return false;
        drmp3_uint64 total = drmp3_get_pcm_frame_count(&mp3);
        if (total == 0) { drmp3_uninit(&mp3); return false; }
        out.pcm.resize(static_cast<size_t>(total) * mp3.channels);
        drmp3_uint64 got = drmp3_read_pcm_frames_f32(
            &mp3, total, out.pcm.data());
        unsigned ch = mp3.channels;
        unsigned rate = mp3.sampleRate;
        drmp3_uninit(&mp3);
        if (got == 0) return false;
        out.pcm.resize(static_cast<size_t>(got) * ch);
        out.ch = static_cast<int>(ch);
        out.rate = static_cast<int>(rate);
        out.frames = static_cast<int>(got);
        return true;
    }
    return false;
}

const SdlAudioBank::Sample* SdlAudioBank::sampleFor(Target* t,
                                                  int soundId) {
    if (!t || !t->sprite || soundId < 0 ||
        soundId >= static_cast<int>(t->sprite->sounds.size()))
        return nullptr;
    const Sound* s = &t->sprite->sounds[soundId];
    auto it = decoded_.find(s);
    if (it != decoded_.end()) return &it->second;
    if (decodeFailed_.count(s)) return nullptr;
    Sample smp;
    if (!decodeSound(*s, smp)) {
        decodeFailed_[s] = true;
        return nullptr;
    }
    return &decoded_.emplace(s, std::move(smp)).first->second;
}

// ------------------------------------------------------------- 再生 ----

void SdlAudioBank::playSound(Target* t, int soundId) {
    if (!dev_) return;   // デバイス無しではボイスが完了しない
    const Sample* s = sampleFor(t, soundId);
    if (std::getenv("XYO_DEBUG"))
        std::fprintf(stderr, "[audio] play id=%d %s\n", soundId,
                     s ? "ok" : "decode-fail");
    if (!s) return;
    SDL_LockAudioDevice(dev_);
    voices_.push_back({s, 0, t, nullptr});
    SDL_UnlockAudioDevice(dev_);
}

void SdlAudioBank::playSoundAndWait(Target* t, int soundId, Thread* th) {
    const Sample* s = dev_ ? sampleFor(t, soundId) : nullptr;
    if (!s) {   // 再生不可なら即座に解決して待ち続けさせない
        if (rt_ && rt_->isWaitingThread(th))
            resolveWait(*rt_->sequencer, *th, Value::undefined());
        return;
    }
    if (dev_) SDL_LockAudioDevice(dev_);
    voices_.push_back({s, 0, t, th});
    if (dev_) SDL_UnlockAudioDevice(dev_);
}

void SdlAudioBank::stopAllSounds(Target*) {
    // TW: stopAllSounds は全ボイスを止める
    stopAll();
}

void SdlAudioBank::stop(Target* t, int soundId) {
    const Sample* s = sampleFor(t, soundId);
    if (dev_) SDL_LockAudioDevice(dev_);
    for (auto& v : voices_)
        if (v.t == t && v.s == s)
            done_.push_back(v);
    voices_.erase(
        std::remove_if(voices_.begin(), voices_.end(),
                       [&](const Voice& v) { return v.t == t && v.s == s; }),
        voices_.end());
    if (dev_) SDL_UnlockAudioDevice(dev_);
}

void SdlAudioBank::stopAll() {
    if (dev_) SDL_LockAudioDevice(dev_);
    finishAllVoices();
    if (dev_) SDL_UnlockAudioDevice(dev_);
}

void SdlAudioBank::finishAllVoices() {
    for (auto& v : voices_) done_.push_back(v);
    voices_.clear();
}

// ------------------------------------------------------------ ミックス --

void SdlAudioBank::audioCb(void* ud, unsigned char* buf, int len) {
    auto* self = static_cast<SdlAudioBank*>(ud);
    self->mix(reinterpret_cast<float*>(buf),
              len / static_cast<int>(sizeof(float)) / 2);
}

void SdlAudioBank::mix(float* out, int frames) {
    std::memset(out, 0, static_cast<size_t>(frames) * 2 * sizeof(float));
    for (auto& v : voices_) {
        Target* t = v.t;
        double vol = t ? std::clamp(t->volume, 0.0, 100.0) / 100.0 : 1.0;
        double pitch = 0, pan = 0;
        if (t) {
            if (auto it = t->soundEffects.find("pitch");
                it != t->soundEffects.end()) pitch = it->second;
            if (auto it = t->soundEffects.find("pan");
                it != t->soundEffects.end()) pan = it->second;
        }
        double ratio = std::pow(2.0, pitch / 120.0);   // PitchEffect.getRatio
        double step = (v.s->rate / double(outRate_)) * ratio;
        double p = (std::clamp(pan, -100.0, 100.0) + 100.0) / 200.0;
        double gl = std::cos(p * M_PI / 2) * vol;      // 等ラウドネス
        double gr = std::sin(p * M_PI / 2) * vol;
        const float* pcm = v.s->pcm.data();
        int sframes = v.s->frames;
        int ch = v.s->ch;
        bool alive = true;
        for (int i = 0; i < frames && alive; ++i) {
            int idx = static_cast<int>(v.pos);
            if (idx >= sframes - 1) {
                alive = false;
                break;
            }
            double frac = v.pos - idx;
            int nidx = idx + 1 < sframes ? idx + 1 : idx;
            float l0, r0, l1, r1;
            if (ch == 1) {
                l0 = r0 = pcm[idx];
                l1 = r1 = pcm[nidx];
            } else {
                l0 = pcm[idx * 2]; r0 = pcm[idx * 2 + 1];
                l1 = pcm[nidx * 2]; r1 = pcm[nidx * 2 + 1];
            }
            out[i * 2]     += static_cast<float>(
                (l0 + (l1 - l0) * frac) * gl);
            out[i * 2 + 1] += static_cast<float>(
                (r0 + (r1 - r0) * frac) * gr);
            v.pos += step;
        }
        if (!alive) {
            v.pos = sframes;   // 完了印（後段の除去で検出）
            done_.push_back(v);
        }
    }
    // 完了ボイスを除去
    voices_.erase(
        std::remove_if(voices_.begin(), voices_.end(),
                       [](const Voice& v) {
                           return v.pos >= v.s->frames;
                       }),
        voices_.end());
    // ピーク（loudness 近似 — マイク非対応のため出力レベルを報告）
    for (int i = 0; i < frames * 2; ++i) {
        float a = std::abs(out[i]);
        if (a > peak_) peak_ = a;
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
    }
}

// -------------------------------------------------------------- update --

void SdlAudioBank::update() {
    if (dev_) SDL_LockAudioDevice(dev_);
    std::deque<Voice> fin;
    fin.swap(done_);
    if (dev_) SDL_UnlockAudioDevice(dev_);
    for (auto& v : fin) {
        if (std::getenv("XYO_DEBUG") && v.wait)
            std::fprintf(stderr, "[audio] voice done → resolveWait\n");
        if (v.wait && rt_ && rt_->isWaitingThread(v.wait))
            resolveWait(*rt_->sequencer, *v.wait, Value::undefined());
    }
    peak_ *= 0.9f;
}

double SdlAudioBank::getLoudness() const {
    return std::min(100.0, static_cast<double>(peak_) * 100.0);
}

} // namespace xyo

#endif // XYO_HAVE_SDL2
