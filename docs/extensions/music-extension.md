# Scratch music extension

The runtime now recognizes music_playNoteForBeats, music_playDrumForBeats, music_midiPlayDrumForBeats, music_restForBeats, music_setInstrument, music_midiSetInstrument, music_setTempo, music_changeTempo, music_getTempo, and menu reporters.

Tempo is stored on Stage (default 60 BPM, range 20–500). Each sprite has its own instrument from Scratch's 21 instrument categories, copied to clones. Beats are clamped to 0–100; duration is 60 * beats / tempo. Runtime wait-queue operation prevents re-triggering notes across scheduler yields. The same block semantics work in headless, Interpreter, eager JIT and HotSpot JIT.

## Sound synthesis and licensing

TinySoundFont is vendored in third_party/tsf.h under the MIT License; see third_party/tsf.LICENSE. Upstream: https://github.com/schellingb/TinySoundFont

assets/soundfonts/xyo-basic.sf2 is our original compact fallback with 21 simple melodic timbres and 18 percussion voices. Its PCM is generated only from mathematical signals; no outside samples are copied. The asset and tools/gen_basic_soundfont.py are offered under CC0 1.0, https://creativecommons.org/publicdomain/zero/1.0/. No attribution, royalties or copyleft obligations apply. It is **not** a timbrally accurate replacement for Scratch's original recordings.

To use a licensed high-quality GM SF2, set XYO_MUSIC_SOUNDFONT=/absolute/path/to/font.sf2. Default SF2 selection uses the working directory plus several common Linux paths. Running from another directory may require the explicit path.

System FluidSynth is disabled by default (opt in with -DXYO_MUSIC_FLUIDSYNTH=ON). It is dynamically linked under its LGPL terms and not bundled. SF3 loading is only attempted when XYO_MUSIC_BACKEND=fluidsynth; it has not been tested successfully in the local restricted sandbox due to audio-backend enumeration. Default TinySoundFont never starts a system audio server.

## Verification

Build xyo-cli and xyo-music-synth-smoke then run CTest with expression ^xyo-music-. The extension differential test uses a generated SB3 with Interpreter/eager/hotspot; the synth smoke verifies nonzero real SDL float32 PCM using the dummy driver. The expected generated piano peak in the current smoke is about 0.18 (may vary slightly).

Limitations: the 21 synthesized instruments are approximations mapped to GM programs. Drums, pitch envelopes, and legacy mappings are not sample-exact with Scratch. Polyphony is limited by MIDI channels (15 melodic + percussion) and a total 30-note ceiling. Target volume is sampled at note start.
