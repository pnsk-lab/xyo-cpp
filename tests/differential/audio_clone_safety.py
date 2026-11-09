#!/usr/bin/env python3
"""Regression: sound-playing Scratch clones can be deleted during SDL mixing.

Both WAV-decoding modes exercise SdlAudioBank::targetDisposed() while the
audio device callback is active. The crash this guards against occurred in
std::map::find from SdlAudioBank::mix on SDLAudioP19.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run(binary: Path, project: Path, jit: str, async_all: bool) -> None:
    env = {
        **os.environ,
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_RENDER_DRIVER": "opengl",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_EXIT_FRAMES": "160",
        "XYO_AUDIOPROF": "1",
        
    }
    if async_all:
        env["XYO_AUDIO_ASYNC_ALL"] = "1"
    else:
        env.pop("XYO_AUDIO_ASYNC_ALL", None)
    result = subprocess.run(
        [str(binary), "play", str(project), f"--jit={jit}", "--fps", "30",
         "--print-list", "events"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=60,
    )
    if result.returncode:
        raise AssertionError((result.returncode, result.stderr[-1600:]))
    lines = result.stdout.splitlines()
    started = lines.count("started")
    disposed = lines.count("disposed")
    assert (started, disposed) == (120, 120), (jit, async_all, started, disposed)
    expected = "async decode tone" if async_all else "decode tone"
    assert expected in result.stderr, result.stderr[-900:]
    print(f"PASS: {jit}, async_all={int(async_all)}, "
          f"{started} sound-playing clones disposed without SIGSEGV",
          flush=True)


def main(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="xyo-audio-clone-") as directory:
        project = Path(directory) / "audio-clone.sb3"
        generator = ROOT / "tests/fixtures/gen_audio_clone_stress.py"
        subprocess.run([sys.executable, str(generator), str(project)], check=True)
        run(binary, project, "off", False)
        run(binary, project, "hotspot", True)


if __name__ == "__main__":
    main((Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/xyo").resolve())
