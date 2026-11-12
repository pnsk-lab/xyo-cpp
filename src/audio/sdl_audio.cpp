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
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <array>
#include <string>
#define TSF_IMPLEMENTATION
#include "../../third_party/tsf.h"
#if defined(XYO_HAVE_FLUIDSYNTH)
#include <fluidsynth.h>
#endif

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
    const bool musicTrace = std::getenv("XYO_MUSIC_TRACE") != nullptr;
    if (musicTrace) std::fprintf(stderr, "[music] SDL audio init, requested=%s\n",
        std::getenv("SDL_AUDIODRIVER") ? std::getenv("SDL_AUDIODRIVER") : "(unset)");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return false;
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_F32;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &SdlAudioBank::audioCb;
    want.userdata = this;
    if (musicTrace) std::fprintf(stderr, "[music] SDL audio initialized\n");
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (musicTrace) std::fprintf(stderr, "[music] audio device=%u\n", dev_);
    if (dev_) {
        outRate_ = have.freq;
        const char* overrideFont = std::getenv("XYO_MUSIC_SOUNDFONT");
        const char* backend = std::getenv("XYO_MUSIC_BACKEND");
        const bool useFluid = backend && std::strcmp(backend, "fluidsynth") == 0;
        if (!useFluid) {
            // SF2 synthesizer is MIT-licensed, entirely independent of ALSA,
            // JACK, PulseAudio and system FluidSynth. A small original CC0
            // SF2 file is the last-resort fallback.
            const char* base = SDL_GetBasePath();
            const std::string exeDir = base ? base : "";
            if (base) SDL_free(const_cast<char*>(base));
            const std::string localFont =
                exeDir + "assets/soundfonts/xyo-basic.sf2";
            const std::string parentFont =
                exeDir + "../assets/soundfonts/xyo-basic.sf2";
            const std::string nestedFont =
                exeDir + "../../../assets/soundfonts/xyo-basic.sf2";
            const std::string deepFont =
                exeDir + "../../../../assets/soundfonts/xyo-basic.sf2";
            const std::array<const char*, 9> fonts = {
                overrideFont, "assets/soundfonts/xyo-basic.sf2",
                localFont.c_str(), parentFont.c_str(), nestedFont.c_str(),
                deepFont.c_str(),
                "/usr/share/sounds/sf2/FluidR3_GM.sf2",
                "/usr/share/soundfonts/FluidR3_GM.sf2",
                "/usr/share/soundfonts/default.sf2"
            };
            for (const char* path : fonts) {
                if (!path || !*path) continue;
                const size_t length = std::strlen(path);
                if (length < 4 || std::strcmp(path + length - 4, ".sf2") != 0)
                    continue;
                if (musicTrace) std::fprintf(stderr, "[music] loading SF2: %s\n", path);
                auto* synth = tsf_load_filename(path);
                if (!synth) continue;
                tsf_set_output(synth, TSF_STEREO_INTERLEAVED, outRate_, -3.0f);
                tsf_set_max_voices(synth, 128);
                // Allocate all required channel slots on the main thread.
                for (int ch = 0; ch < 16; ++ch)
                    tsf_channel_set_presetnumber(synth, ch, 0, ch == 9);
                musicTsf_ = synth;
                musicScratch_.resize(2048);
                if (musicTrace) std::fprintf(stderr, "[music] loaded SF2\n");
                break;
            }
        }
#if defined(XYO_HAVE_FLUIDSYNTH)
        if (useFluid) {
            // SF3 support is explicitly opt-in; this API may enumerate host
            // audio drivers during settings creation on some installations.
            const std::array<const char*, 3> fonts = {
                overrideFont, "/usr/share/mscore-4.7/sound/MS Basic.sf3",
                "/usr/share/sounds/sf3/MS Basic.sf3"
            };
            for (const char* path : fonts) {
                if (!path || !*path) continue;
                auto* settings = new_fluid_settings();
                if (!settings) break;
                fluid_settings_setnum(settings, "synth.sample-rate", outRate_);
                fluid_settings_setnum(settings, "synth.gain", 0.35);
                fluid_settings_setint(settings, "synth.reverb.active", 0);
                fluid_settings_setint(settings, "synth.chorus.active", 0);
                auto* synth = new_fluid_synth(settings);
                if (synth && fluid_synth_sfload(synth, path, 1) >= 0) {
                    musicSettings_ = settings;
                    musicSynth_ = synth;
                    musicScratch_.resize(2048);
                    if (musicTrace) std::fprintf(stderr, "[music] loaded SF3: %s\n", path);
                    break;
                }
                if (synth) delete_fluid_synth(synth);
                delete_fluid_settings(settings);
            }
        }
#endif
        SDL_PauseAudioDevice(dev_, 0);
    }
    // 全 sprite に soundBank を設定（clone は Sprite* を共有）
    for (Target* t : rt_->targets)
        if (t->sprite) t->sprite->soundBank = this;
    rt_->audioEngine = this;
    rt_->onProjectStopped = [this] { stopAll(); };
    // Never decode a multi-minute MP3 on the render thread. The audio
    // callback only reads immutable Samples published by update().
    // Explicit opt-out preserves the synchronous path for comparisons.
    const char* sync = std::getenv("XYO_AUDIO_SYNC");
    asyncDecode_ = dev_ && !(sync && sync[0] == '1');
    if (asyncDecode_) decoder_ = std::thread(&SdlAudioBank::decoderLoop, this);
    return true;
}

SdlAudioBank::~SdlAudioBank() { shutdown(); }

void SdlAudioBank::shutdown() {
    if (decoder_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(decoderMutex_);
            decoderStop_ = true;
            decodeQueue_.clear();
        }
        decoderCv_.notify_one();
        decoder_.join(); // Sound data remains valid until the worker exits.
    }
    pendingVoices_.clear();
    decoding_.clear();
    decodeReady_.clear();
    if (dev_) {
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
#if defined(XYO_HAVE_FLUIDSYNTH)
    if (musicSynth_) delete_fluid_synth(static_cast<fluid_synth_t*>(musicSynth_));
    if (musicSettings_) delete_fluid_settings(static_cast<fluid_settings_t*>(musicSettings_));
#endif
    musicSynth_ = musicSettings_ = nullptr;
    if (musicTsf_) tsf_close(static_cast<tsf*>(musicTsf_));
    musicTsf_ = nullptr;
    musicVoices_.clear();
    musicScratch_.clear();
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

const Sound* SdlAudioBank::soundFor(Target* t, int soundId) const {
    if (!t || !t->sprite || soundId < 0 ||
        soundId >= static_cast<int>(t->sprite->sounds.size()))
        return nullptr;
    return &t->sprite->sounds[soundId];
}

const SdlAudioBank::Sample* SdlAudioBank::sampleFor(Target* t,
                                                  int soundId) {
    const Sound* s = soundFor(t, soundId);
    if (!s) return nullptr;
    auto it = decoded_.find(s);
    if (it != decoded_.end()) return &it->second;
    if (decodeFailed_.count(s)) return nullptr;
    Sample smp;
    const bool timing = std::getenv("XYO_AUDIOPROF") != nullptr;
    const auto t0 = timing ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
    const bool ok = decodeSound(*s, smp);
    if (timing) {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[audioprof] decode %s bytes=%zu pcm=%zu %.3fms %s\n",
                     SStr::toUtf8(s->name).c_str(), s->data.size(),
                     smp.pcm.size() * sizeof(float), ms, ok ? "ok" : "failed");
    }
    if (!ok) {
        decodeFailed_[s] = true;
        return nullptr;
    }
    return &decoded_.emplace(s, std::move(smp)).first->second;
}

// ------------------------------------------------------------- 再生 ----

namespace {
bool audioWaiterIsLive(Runtime* rt, Thread* wait) {
    // A stopped script may have been recycled while the worker was
    // decoding. Check pointer identity *before* inspecting Thread fields.
    if (!rt || !wait ||
        std::find(rt->threads.begin(), rt->threads.end(), wait) ==
            rt->threads.end())
        return false;
    return wait->status == STATUS_PROMISE_WAIT ||
           wait->status == STATUS_YIELD_TICK;
}
} // namespace

void SdlAudioBank::decoderLoop() {
    for (;;) {
        const Sound* source = nullptr;
        {
            std::unique_lock<std::mutex> lock(decoderMutex_);
            decoderCv_.wait(lock, [this] {
                return decoderStop_ || !decodeQueue_.empty();
            });
            if (decoderStop_) return;
            source = decodeQueue_.front();
            decodeQueue_.pop_front();
        }

        DecodeResult result;
        result.sound = source;
        const bool timing = std::getenv("XYO_AUDIOPROF") != nullptr;
        const auto start = std::chrono::steady_clock::now();
        result.ok = decodeSound(*source, result.sample);
        if (timing) {
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            std::fprintf(stderr,
                "[audioprof] async decode %s bytes=%zu pcm=%zu %.3fms %s\n",
                SStr::toUtf8(source->name).c_str(), source->data.size(),
                result.sample.pcm.size() * sizeof(float), ms,
                result.ok ? "ok" : "failed");
        }
        {
            std::lock_guard<std::mutex> lock(decoderMutex_);
            if (decoderStop_) return;
            decodeReady_.push_back(std::move(result));
        }
    }
}

// Main-thread only: take a snapshot of volatile Scratch target state.
// The SDL callback must never dereference Target or its soundEffects map.
void SdlAudioBank::snapshotVoice(Voice& v) const {
    if (!v.t || !v.s) return;
    const Target& t = *v.t;
    const double volume = std::clamp(t.volume, 0.0, 100.0) / 100.0;
    double pitch = 0.0, pan = 0.0;
    if (const auto it = t.soundEffects.find("pitch");
        it != t.soundEffects.end()) pitch = it->second;
    if (const auto it = t.soundEffects.find("pan");
        it != t.soundEffects.end()) pan = it->second;

    // Scratch's PitchEffect clamps to +/-360 (three octaves).
    pitch = std::clamp(pitch, -360.0, 360.0);
    v.step = (v.s->rate / double(outRate_)) * std::pow(2.0, pitch / 120.0);
    const double p = (std::clamp(pan, -100.0, 100.0) + 100.0) / 200.0;
    v.gainL = std::cos(p * M_PI / 2) * volume;
    v.gainR = std::sin(p * M_PI / 2) * volume;
}

void SdlAudioBank::requestSound(Target* t, int soundId, Thread* wait) {
    if (!dev_) {
        if (audioWaiterIsLive(rt_, wait))
            resolveWait(*rt_->sequencer, *wait, Value::undefined());
        return;
    }
    const Sound* source = soundFor(t, soundId);
    if (!source) {
        if (audioWaiterIsLive(rt_, wait))
            resolveWait(*rt_->sequencer, *wait, Value::undefined());
        return;
    }
    // A Scratch sound source is a singleton per Sprite, shared by clones.
    // Restarting it cancels the old voice and releases play-until-done
    // waiters. Do this before enqueueing the new request, including for
    // sounds that are currently decoding asynchronously.
    stop(t, soundId);
    auto existing = decoded_.find(source);
    if (existing != decoded_.end()) {
        Voice voice{&existing->second, 0, t, wait, source};
        snapshotVoice(voice);
        SDL_LockAudioDevice(dev_);
        voices_.push_back(voice);
        SDL_UnlockAudioDevice(dev_);
        return;
    }
    if (decodeFailed_.count(source)) {
        if (audioWaiterIsLive(rt_, wait))
            resolveWait(*rt_->sequencer, *wait, Value::undefined());
        return;
    }
    // Small sound effects should start immediately. A worker-only path
    // would add up to one full frame of latency even for a 0.05 ms WAV.
    // Long WAVs and music/MP3s are decoded off the render thread.
    const bool smallSound = source->dataFormat == "wav"
        ? source->data.size() <= 1024 * 1024
        : source->data.size() <= 64 * 1024;
    const bool forceAsync = std::getenv("XYO_AUDIO_ASYNC_ALL") != nullptr;
    if (!asyncDecode_ || (smallSound && !forceAsync)) {
        const Sample* s = sampleFor(t, soundId);
        if (s) {
            Voice voice{s, 0, t, wait, source};
            snapshotVoice(voice);
            SDL_LockAudioDevice(dev_);
            voices_.push_back(voice);
            SDL_UnlockAudioDevice(dev_);
        } else if (audioWaiterIsLive(rt_, wait)) {
            resolveWait(*rt_->sequencer, *wait, Value::undefined());
        }
        return;
    }
    // Queued voices retain their original request order. The decoder
    // never touches live Target/Thread pointers or the SDL audio callback.
    pendingVoices_.push_back({source, t, wait});
    if (decoding_.insert(source).second) {
        {
            std::lock_guard<std::mutex> lock(decoderMutex_);
            decodeQueue_.push_back(source);
        }
        decoderCv_.notify_one();
    }
}

void SdlAudioBank::playSound(Target* t, int soundId) {
    requestSound(t, soundId, nullptr);
}

void SdlAudioBank::playSoundAndWait(Target* t, int soundId, Thread* th) {
    requestSound(t, soundId, th);
}

std::vector<SoundPlaybackState> SdlAudioBank::captureActiveSounds() {
    std::vector<SoundPlaybackState> state;
    if (dev_) SDL_LockAudioDevice(dev_);
    for (const Voice& voice : voices_) {
        if (!voice.t || !voice.t->sprite || !voice.sound) continue;
        const auto& sounds = voice.t->sprite->sounds;
        for (size_t i = 0; i < sounds.size(); ++i)
            if (&sounds[i] == voice.sound) {
                state.push_back({voice.t, voice.wait, int(i), voice.pos, false});
                break;
            }
    }
    if (dev_) SDL_UnlockAudioDevice(dev_);
    for (const PendingVoice& voice : pendingVoices_) {
        if (!voice.target || !voice.target->sprite || !voice.sound) continue;
        const auto& sounds = voice.target->sprite->sounds;
        for (size_t i = 0; i < sounds.size(); ++i)
            if (&sounds[i] == voice.sound) {
                state.push_back({voice.target, voice.wait,
                                 int(i), voice.position, true});
                break;
            }
    }
    return state;
}

bool SdlAudioBank::restoreActiveSounds(
    const std::vector<SoundPlaybackState>& state) {
    if (!dev_) return state.empty();
    if (!voices_.empty() || !pendingVoices_.empty()) return false;
    for (const SoundPlaybackState& entry : state) {
        const Sound* source = soundFor(entry.target, entry.soundId);
        if (!source || !std::isfinite(entry.position) ||
            entry.position < 0.0 || entry.position > double(1ull << 40))
            return false;
        const auto found = decoded_.find(source);
        if (found != decoded_.end()) {
            Voice voice{&found->second, entry.position,
                        entry.target, entry.wait, source};
            snapshotVoice(voice);
            SDL_LockAudioDevice(dev_);
            voices_.push_back(voice);
            SDL_UnlockAudioDevice(dev_);
            continue;
        }
        if (decodeFailed_.contains(source)) return false;
        if (!asyncDecode_) {
            // XYO_AUDIO_SYNC=1 deliberately has no decoder worker. Resume
            // synchronously instead of leaving the audio wait stranded.
            const Sample* sample = sampleFor(entry.target, entry.soundId);
            if (!sample) return false;
            Voice voice{sample, entry.position, entry.target,
                        entry.wait, source};
            snapshotVoice(voice);
            SDL_LockAudioDevice(dev_);
            voices_.push_back(voice);
            SDL_UnlockAudioDevice(dev_);
            continue;
        }
        // Reconstruct the PCM off-thread and begin at the saved offset.
        // No expensive multi-minute MP3 decoding on the render thread.
        pendingVoices_.push_back({
            source, entry.target, entry.wait, entry.position
        });
        if (decoding_.insert(source).second) {
            {
                std::lock_guard<std::mutex> lock(decoderMutex_);
                decodeQueue_.push_back(source);
            }
            decoderCv_.notify_one();
        }
    }
    return true;
}

void SdlAudioBank::setEffects(Target* t) {
    if (!dev_ || !t) return;
    // Called on the main thread. The lock excludes the callback while
    // changing scalars used by the real-time mixer.
    SDL_LockAudioDevice(dev_);
    for (Voice& voice : voices_)
        if (voice.t == t) snapshotVoice(voice);
    SDL_UnlockAudioDevice(dev_);
}

void SdlAudioBank::targetDisposed(Target* t) {
    if (!t) return;
    if (dev_) SDL_LockAudioDevice(dev_);
    for (const Voice& voice : voices_)
        if (voice.t == t && voice.wait)
            done_.push_back(voice);
    std::erase_if(voices_, [t](const Voice& v) { return v.t == t; });
    for (const PendingVoice& voice : pendingVoices_)
        if (voice.target == t && voice.wait)
            done_.push_back({nullptr, 0, t, voice.wait});
    std::erase_if(pendingVoices_,
                  [t](const PendingVoice& v) { return v.target == t; });
    if (dev_) SDL_UnlockAudioDevice(dev_);
}

void SdlAudioBank::stopAllSounds(Target*) {
    // TW: stopAllSounds は全ボイスを止める
    stopAll();
}

void SdlAudioBank::stop(Target* t, int soundId) {
    const Sound* sound = soundFor(t, soundId);
    if (!sound) return;
    if (dev_) SDL_LockAudioDevice(dev_);
    for (auto& v : voices_)
        if (v.sound == sound)
            done_.push_back(v);
    voices_.erase(
        std::remove_if(voices_.begin(), voices_.end(),
                       [&](const Voice& v) { return v.sound == sound; }),
        voices_.end());
    for (const auto& v : pendingVoices_) {
        if (v.sound == sound && v.wait)
            done_.push_back({nullptr, 0, v.target, v.wait});
    }
    pendingVoices_.erase(
        std::remove_if(pendingVoices_.begin(), pendingVoices_.end(),
                       [&](const PendingVoice& v) {
                           return v.sound == sound;
                       }),
        pendingVoices_.end());
    if (dev_) SDL_UnlockAudioDevice(dev_);
}

void SdlAudioBank::stopAll() {
    if (dev_) SDL_LockAudioDevice(dev_);
    if (musicTsf_) tsf_reset(static_cast<tsf*>(musicTsf_));
#if defined(XYO_HAVE_FLUIDSYNTH)
    if (musicSynth_) for (int ch = 0; ch < 16; ++ch)
        fluid_synth_all_sounds_off(static_cast<fluid_synth_t*>(musicSynth_), ch);
#endif
    musicVoices_.clear();
    finishAllVoices();
    if (dev_) SDL_UnlockAudioDevice(dev_);
}

void SdlAudioBank::finishAllVoices() {
    for (auto& v : voices_) done_.push_back(v);
    voices_.clear();
    // A stopped play-until-done must resolve even if decoding has not
    // finished. Keep the decode job for future playback/cache reuse.
    for (const auto& v : pendingVoices_)
        if (v.wait) done_.push_back({nullptr, 0, v.target, v.wait});
    pendingVoices_.clear();
}

// ------------------------------------------------------------ ミックス --

void SdlAudioBank::audioCb(void* ud, unsigned char* buf, int len) {
    auto* self = static_cast<SdlAudioBank*>(ud);
    self->mix(reinterpret_cast<float*>(buf),
              len / static_cast<int>(sizeof(float)) / 2);
}

void SdlAudioBank::playMusicNote(int program, int note, double seconds,
                                 double volume, bool drum) {
    if (!dev_ || (!musicTsf_ && !musicSynth_) ||
        !std::isfinite(seconds) || seconds <= 0)
        return;
    note = std::clamp(note, 0, 127);
    const int duration = int(std::clamp(seconds * outRate_, 1.0,
                                        double(outRate_) * 100));
    SDL_LockAudioDevice(dev_);
    int channel = drum ? 9 : -1;
    if (!drum) for (int ch = 0; ch < 16; ++ch) {
        if (ch == 9) continue;
        const bool used = std::any_of(musicVoices_.begin(), musicVoices_.end(),
            [ch](const MusicVoice& v) { return v.channel == ch; });
        if (!used) { channel = ch; break; }
    }
    if (channel >= 0 && musicVoices_.size() < 30) {
        bool started = false;
        if (musicTsf_) {
            auto* sf = static_cast<tsf*>(musicTsf_);
            if (tsf_channel_set_presetnumber(sf, channel,
                                             std::clamp(program, 0, 127),
                                             drum ? 1 : 0)) {
                started = tsf_channel_note_on(sf, channel, note,
                           float(std::clamp(volume, 0.0, 1.0))) != 0;
            }
        }
#if defined(XYO_HAVE_FLUIDSYNTH)
        if (musicSynth_) {
            auto* synth = static_cast<fluid_synth_t*>(musicSynth_);
            if (!drum) fluid_synth_program_change(
                synth, channel, std::clamp(program, 0, 127));
            fluid_synth_cc(synth, channel, 7,
                           int(std::clamp(volume, 0.0, 1.0) * 127));
            started = fluid_synth_noteon(synth, channel, note, 110) == FLUID_OK;
        }
#endif
        if (started) musicVoices_.push_back({channel, note, duration});
    }
    SDL_UnlockAudioDevice(dev_);
}
void SdlAudioBank::mix(float* out, int frames) {
    std::memset(out, 0, static_cast<size_t>(frames) * 2 * sizeof(float));
    for (auto& v : voices_) {
        // No Target reads here: sprites/clones may be mutated or freed
        // concurrently by the main thread.
        const double step = v.step;
        const double gl = v.gainL;
        const double gr = v.gainR;
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
    if (musicTsf_ || musicSynth_) {
        int offset = 0;
        while (offset < frames) {
            int count = std::min(frames - offset, 1024);
            for (const auto& voice : musicVoices_)
                count = std::min(count, voice.framesLeft);
            if (count <= 0) break;
            if (musicTsf_) {
                tsf_render_float(static_cast<tsf*>(musicTsf_),
                                 musicScratch_.data(), count, 0);
            }
#if defined(XYO_HAVE_FLUIDSYNTH)
            else if (musicSynth_) {
                auto* synth = static_cast<fluid_synth_t*>(musicSynth_);
                fluid_synth_write_float(synth, count,
                                       musicScratch_.data(), 0, 2,
                                       musicScratch_.data(), 1, 2);
            }
#endif
            for (int i = 0; i < count * 2; ++i)
                out[offset * 2 + i] += musicScratch_[i];
            offset += count;
            for (auto& voice : musicVoices_) voice.framesLeft -= count;
            for (const auto& voice : musicVoices_) {
                if (voice.framesLeft > 0) continue;
                if (musicTsf_) tsf_channel_note_off(
                    static_cast<tsf*>(musicTsf_), voice.channel, voice.key);
#if defined(XYO_HAVE_FLUIDSYNTH)
                if (musicSynth_) fluid_synth_noteoff(
                    static_cast<fluid_synth_t*>(musicSynth_),
                    voice.channel, voice.key);
#endif
            }
            std::erase_if(musicVoices_,
                [](const MusicVoice& v) { return v.framesLeft <= 0; });
        }
    }
    // ピーク（loudness 近似 — マイク非対応のため出力レベルを報告）
    for (int i = 0; i < frames * 2; ++i) {
        float a = std::abs(out[i]);
        if (a > peak_) peak_ = a;
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
    }
}

// -------------------------------------------------------------- update --

void SdlAudioBank::update() {
    // Publish completed decodes on the main thread. Unordered-map nodes
    // keep a stable Sample address for the real-time SDL callback.
    std::deque<DecodeResult> ready;
    if (asyncDecode_) {
        std::lock_guard<std::mutex> lock(decoderMutex_);
        ready.swap(decodeReady_);
    }
    for (auto& result : ready) {
        decoding_.erase(result.sound);
        const Sample* sample = nullptr;
        if (result.ok) {
            sample = &decoded_.emplace(result.sound,
                                      std::move(result.sample)).first->second;
        } else {
            decodeFailed_[result.sound] = true;
        }
        std::vector<PendingVoice> requests;
        for (size_t i = 0; i < pendingVoices_.size();) {
            if (pendingVoices_[i].sound != result.sound) {
                ++i;
                continue;
            }
            requests.push_back(pendingVoices_[i]);
            pendingVoices_.erase(pendingVoices_.begin() + i);
        }
        for (const PendingVoice& request : requests) {
            // A clone may have been deleted since playback was queued.
            const bool targetAlive = rt_ && std::find(
                rt_->targets.begin(), rt_->targets.end(),
                request.target) != rt_->targets.end();
            if (sample && targetAlive && dev_) {
                Voice voice{sample, request.position, request.target,
                            request.wait, result.sound};
                snapshotVoice(voice);
                SDL_LockAudioDevice(dev_);
                voices_.push_back(voice);
                SDL_UnlockAudioDevice(dev_);
            } else if (audioWaiterIsLive(rt_, request.wait)) {
                resolveWait(*rt_->sequencer, *request.wait,
                            Value::undefined());
            }
        }
    }
    if (dev_) SDL_LockAudioDevice(dev_);
    // Refresh volume, pitch, pan on the main thread once per frame.
    // Live voices cannot reference clones after targetDisposed().
    for (Voice& voice : voices_)
        snapshotVoice(voice);
    peak_ *= 0.9f;
    std::deque<Voice> fin;
    fin.swap(done_);
    if (dev_) SDL_UnlockAudioDevice(dev_);
    for (auto& v : fin) {
        if (std::getenv("XYO_DEBUG") && v.wait)
            std::fprintf(stderr, "[audio] voice done → resolveWait\n");
        if (audioWaiterIsLive(rt_, v.wait))
            resolveWait(*rt_->sequencer, *v.wait, Value::undefined());
    }
}

float SdlAudioBank::outputPeak() const {
    if (dev_) SDL_LockAudioDevice(dev_);
    const float value = peak_;
    if (dev_) SDL_UnlockAudioDevice(dev_);
    return value;
}

double SdlAudioBank::getLoudness() const {
    // Scratch's loudness reporter reads microphone input, not the volume of
    // sounds played by the project. No microphone capture is installed here.
    return -1.0;
}

} // namespace xyo

#endif // XYO_HAVE_SDL2
