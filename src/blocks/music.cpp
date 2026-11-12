// Scratch 3 music extension: timing and MIDI-backed sound via AudioAPI.
// Audio rendering is optional; all block semantics work in headless mode.
#include "core.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "../core/cast.hpp"
#include "../engine/block_utility.hpp"
#include "../engine/runtime.hpp"
#include "../model/target.hpp"

namespace xyo {
namespace {
constexpr std::array<int, 21> kPrograms = {
    0, 4, 19, 24, 26, 32, 45, 48, 56, 71, 65,
    73, 74, 68, 52, 11, 9, 13, 47, 80, 88
};

constexpr std::array<int, 128> kMidiInstrumentToScratch = {
    1, 1, 1, 1, 2, 2, 4, 4, 17, 17, 17, 16, 19, 16, 17, 17,
    3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 5, 5, 5, 5, 5, 5,
    6, 6, 6, 6, 6, 6, 6, 6, 8, 8, 8, 8, 8, 7, 8, 19,
    8, 8, 8, 8, 15, 15, 15, 19, 9, 9, 9, 9, 9, 9, 9, 9,
    11, 11, 11, 11, 14, 14, 14, 10, 12, 12, 13, 13, 13, 13, 12, 12,
    20, 20, 20, 20, 20, 20, 20, 20, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 21, 21, 21, 21, 4, 4, 4, 4, 17, 14, 8, 10,
    17, 17, 18, 19, 1, 1, 1, 1, 21, 21, 21, 21, 21, 21, 21, 21
};
constexpr std::array<int, 47> kMidiDrumToScratch = {
    1, 1, 2, 0, 7, 0, 1, 5, 1, 5, 1, 4, 1, 1, 3, 1,
    4, 3, 4, 6, 3, 10, 3, 16, 4, 12, 12, 13, 13, 13, 12, 12,
    10, 10, 14, 14, 17, 17, 15, 15, 8, 9, 9, 17, 17, 11, 11
};

// Standard GM percussion key corresponding to Scratch's 18-item menu.
// Some original Scratch drum samples are pitched/processed; these are
// timbral approximations, not sample-identical reproductions.
constexpr std::array<int, 18> kDrums = {
    38, 36, 42, 46, 49, 47, 51, 39, 75,
    60, 54, 56, 76, 77, 81, 80, 37, 35
};

double numeric(const ArgValues& args, const char* name) {
    const double value = cast::toNumber(args.get(name));
    return std::isfinite(value) ? value : 0.0;
}
int wrapIndex(double number, int count) {
    const int index = static_cast<int>(std::round(
        std::clamp(number, -1000000.0, 1000000.0))) - 1;
    return (index % count + count) % count;
}
int selectedInstrument(const Target& t) {
    return std::clamp(t.musicInstrument, 0, int(kPrograms.size()) - 1);
}
double beatSeconds(const ArgValues& args, Runtime& rt) {
    const Target* stage = rt.getTargetForStage();
    const double bpm = stage ? std::clamp(stage->tempo, 20.0, 500.0) : 60.0;
    return 60.0 / bpm * std::clamp(numeric(args, "BEATS"), 0.0, 100.0);
}
void waitSeconds(BlockUtility& util, double seconds) {
    // Reuse the same one-shot asynchronous wait queue as say-for-seconds.
    // No note is replayed on a scheduler resume, unlike an eager yield.
    Runtime* rt = util.runtime();
    Runtime::WaitEntry wait;
    wait.thread = util.thread;
    wait.deadlineMs = rt->currentMSecs + seconds * 1000.0;
    rt->enqueueWait(wait);
    util.runtime()->transitionThread(*util.thread, STATUS_PROMISE_WAIT);
}
Value playNote(const ArgValues& args, BlockUtility& util) {
    const double seconds = beatSeconds(args, *util.runtime());
    if (seconds <= 0) return Value::undefined();
    const int note = int(std::round(std::clamp(numeric(args, "NOTE"), 0.0, 130.0)));
    if (auto* audio = util.runtime()->audioEngine)
        audio->playMusicNote(kPrograms[selectedInstrument(*util.target())],
                             std::clamp(note, 0, 127), seconds,
                             std::clamp(util.target()->volume, 0.0, 100.0) / 100.0,
                             false);
    waitSeconds(util, seconds);
    return Value::undefined();
}
Value playDrum(const ArgValues& args, BlockUtility& util, bool midi) {
    const double seconds = beatSeconds(args, *util.runtime());
    const int drumIndex = midi
        ? kMidiDrumToScratch[std::clamp(
            int(std::round(numeric(args, "DRUM"))), 35, 81) - 35]
        : wrapIndex(numeric(args, "DRUM"), int(kDrums.size()));
    const int key = kDrums[drumIndex];
    if (auto* audio = util.runtime()->audioEngine)
        audio->playMusicNote(0, key, std::max(seconds, 0.02),
                             std::clamp(util.target()->volume, 0.0, 100.0) / 100.0,
                             true);
    waitSeconds(util, seconds);
    return Value::undefined();
}
Value drum(const ArgValues& a, BlockUtility& u) { return playDrum(a,u,false); }
Value midiDrum(const ArgValues& a, BlockUtility& u) { return playDrum(a,u,true); }
Value rest(const ArgValues& a, BlockUtility& u) {
    waitSeconds(u, beatSeconds(a, *u.runtime()));
    return Value::undefined();
}
Value instrument(const ArgValues& a, BlockUtility& u) {
    u.target()->musicInstrument = wrapIndex(numeric(a, "INSTRUMENT"),
                                            int(kPrograms.size()));
    return Value::undefined();
}
Value midiInstrument(const ArgValues& a, BlockUtility& u) {
    const int gm = std::clamp(int(std::round(numeric(a,"INSTRUMENT"))),
                              1, 128) - 1;
    u.target()->musicInstrument = kMidiInstrumentToScratch[gm] - 1;
    return Value::undefined();
}
Value setTempo(const ArgValues& a, BlockUtility& u) {
    if (Target* stage = u.runtime()->getTargetForStage())
        stage->tempo = std::clamp(numeric(a,"TEMPO"), 20.0, 500.0);
    return Value::undefined();
}
Value changeTempo(const ArgValues& a, BlockUtility& u) {
    if (Target* stage = u.runtime()->getTargetForStage())
        stage->tempo = std::clamp(stage->tempo + numeric(a,"TEMPO"), 20.0, 500.0);
    return Value::undefined();
}
Value getTempo(const ArgValues&, BlockUtility& u) {
    const Target* stage = u.runtime()->getTargetForStage();
    return Value::num(stage ? stage->tempo : 60.0);
}
Value menu(const ArgValues& a, BlockUtility&) {
    return a.vals.empty() ? Value::undefined() : a.vals.begin()->second;
}
} // namespace

void registerMusicBlocks(Runtime& rt) {
    rt.registerPrimitive("music_playNoteForBeats", playNote);
    rt.registerPrimitive("music_playDrumForBeats", drum);
    rt.registerPrimitive("music_midiPlayDrumForBeats", midiDrum);
    rt.registerPrimitive("music_restForBeats", rest);
    rt.registerPrimitive("music_setInstrument", instrument);
    rt.registerPrimitive("music_midiSetInstrument", midiInstrument);
    rt.registerPrimitive("music_setTempo", setTempo);
    rt.registerPrimitive("music_changeTempo", changeTempo);
    rt.registerPrimitive("music_getTempo", getTempo);
    rt.registerPrimitive("music_menu_DRUM", menu);
    rt.registerPrimitive("music_menu_INSTRUMENT", menu);
}
} // namespace xyo
