// xyo-cpp: Mega scheduler preset application points.
//
// The preset is applied from main() (before the CLI creates a Runtime) and
// from Sequencer::Sequencer() (for tests and embedders that build a Runtime
// directly). It is deliberately NOT applied from a static initializer:
// `--no-mega` must be able to cancel an inherited XYO_JIT_MEGA=1 before any
// feature flag is written into the environment.
#include "engine/mega_preset.hpp"
