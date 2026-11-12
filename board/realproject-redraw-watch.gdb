set pagination off
set confirm off
set environment SDL_VIDEODRIVER offscreen
set environment SDL_AUDIODRIVER dummy
set environment XYO_EXIT_FRAMES 70
set environment XYO_JIT_MODE off
break src/engine/runtime.cpp:897 if stepSerial == 45
run play scratch-bench/.cache/build/projects/1274137785.sb3 --print-list output
watch this->redrawRequested
continue
bt 14
