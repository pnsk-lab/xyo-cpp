// End-to-end: FluidSynth SoundFont -> the existing SDL callback mixer.
#include "../../src/audio/sdl_audio.hpp"
#include "../../src/engine/runtime.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

int main() {
    std::fputs("[music-smoke] before Runtime\n", stderr);
    xyo::Runtime runtime;
    std::fputs("[music-smoke] after Runtime\n", stderr);
    xyo::SdlAudioBank audio;
    std::fputs("[music-smoke] before audio init\n", stderr);
    if (!audio.init(runtime) || !audio.ok()) {
        std::fputs("failed to open dummy SDL audio device\n", stderr);
        return 1;
    }
    std::fputs("[music-smoke] after audio init\n", stderr);
    audio.playMusicNote(0, 60, 0.35, 1.0, false);
    std::fputs("[music-smoke] after note on\n", stderr);
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    const float peak = audio.outputPeak();
    audio.stopAll();
    audio.shutdown();
    std::fprintf(stderr, "music synth peak=%.7f\n", peak);
    return std::isfinite(peak) && peak > 0.001f ? 0 : 1;
}
