#!/usr/bin/env bash
# Finder-style launcher flags: real SDL mouse clicks, persistence, run wiring.
set -euo pipefail

if [ "$#" -gt 0 ]; then bin="$1"; else bin="build/xyo"; fi
test -x "$bin"
mkdir -p build
work="$(mktemp -d "$PWD/build/gui-flags-smoke.XXXXXX")"
trap 'rm -rf "$work"' EXIT

export XDG_STATE_HOME="$work/state"
export SDL_VIDEODRIVER=dummy
export SDL_AUDIODRIVER=dummy
export SDL_RENDER_DRIVER=software
mkdir -p "$XDG_STATE_HOME"

XYO_GUI_ACTIONS='settings,click:810:228,click:691:290,click:605:350,click:605:410,click:786:471,click:340:539,click:487:539' \
XYO_GUI_EXIT_FRAMES=22 "$bin" gui examples >"$work/gui.log" 2>&1

settings="$XDG_STATE_HOME/xyo/gui-settings.txt"
test -f "$settings"
for expected in scale=4 fps=60 jit=1 lazy=1 gpu=1 turbo=1; do
    grep -qx "$expected" "$settings"
done
! grep -q '^danger=' "$settings"

# A new GUI instance loads persisted flags, but starts with danger OFF.
XYO_GUI_DEBUG=1 XYO_EXIT_FRAMES=2 XYO_GUI_ACTIONS='gpu:off,end,enter' \
    "$bin" gui examples >"$work/play.log" 2>&1
grep -q 'scale=4 fps=60 turbo=1 jit=1 lazy=1 gpu=-1 danger=0' \
    "$work/play.log"

# Deliberately turning danger ON only affects this run.
XYO_GUI_DEBUG=1 XYO_EXIT_FRAMES=2 \
XYO_GUI_ACTIONS='danger:1,gpu:off,end,enter' \
    "$bin" gui examples >"$work/danger.log" 2>&1
grep -q 'danger=1' "$work/danger.log"
! grep -q '^danger=' "$settings"

# Explicit AOT playback wins over JIT=OFF and works when the sandbox's
# system temporary directory is not writable. Temporary objects are cleaned.
XYO_AOT_VERBOSE=1 XYO_EXIT_FRAMES=2 \
XYO_GUI_ACTIONS='jit:off,gpu:off,end,aotrun' \
    "$bin" gui examples >"$work/aot.log" 2>&1
grep -q 'aot: [0-9].*scripts precompiled' "$work/aot.log"
grep -q 'xyo-jit: AOT loaded' "$work/aot.log"
if find "$XDG_STATE_HOME" -type f -name 'xyo-gui-aot-*.xyo' | grep -q .; then
    echo 'Temporary AOT object leaked' >&2
    exit 1
fi

echo 'GUI flags smoke: OK'
