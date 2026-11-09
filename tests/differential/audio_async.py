#!/usr/bin/env python3
"""Differential test for non-blocking SDL audio playback and cancellation."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(binary, project, sync, force_async=False):
    env = os.environ.copy()
    env.update(
        SDL_VIDEODRIVER="offscreen",
        SDL_RENDER_DRIVER="opengl",
        SDL_AUDIODRIVER="dummy",
        XYO_EXIT_FRAMES="25",
        XYO_AUDIOPROF="1",
    )
    if sync:
        env["XYO_AUDIO_SYNC"] = "1"
    else:
        env.pop("XYO_AUDIO_SYNC", None)
    if force_async:
        env["XYO_AUDIO_ASYNC_ALL"] = "1"
    else:
        env.pop("XYO_AUDIO_ASYNC_ALL", None)
    result = subprocess.run(
        [str(binary), "play", str(project), "--jit=off",
         "--fps", "30", "--print-list", "events"],
        env=env, text=True, capture_output=True, timeout=45,
    )
    assert result.returncode == 0, result.stderr
    events = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    assert events == ["started", "completed"], events
    keyword = (
        "[audioprof] async decode tone" if force_async and not sync
        else "[audioprof] decode tone"
    )
    assert keyword in result.stderr, result.stderr
    return events


def test(binary):
    generator = Path(__file__).resolve().parents[1] / "fixtures/gen_audio_async.py"
    with tempfile.TemporaryDirectory(prefix="xyo-async-audio-") as tmp:
        for cancel in (False, True):
            project = Path(tmp) / ("cancel.sb3" if cancel else "normal.sb3")
            args = [sys.executable, str(generator), str(project)]
            if cancel:
                args.append("--cancel")
            subprocess.run(args, check=True)
            for sync, force_async in ((True, False),
                                      (False, False),
                                      (False, True)):
                run(binary, project, sync, force_async)
    print("PASS: sound_playuntildone completes in sync, hybrid, and "
          "forced-async modes, including sound_stopallsounds")


if __name__ == "__main__":
    test(Path(sys.argv[1]).resolve())
