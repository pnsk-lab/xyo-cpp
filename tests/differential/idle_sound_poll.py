#!/usr/bin/env python3
"""Check conservative sound-poll scheduling preserves event response."""
import csv
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(binary, project, profile, optimized, jit):
    env = os.environ.copy()
    env.update(
        SDL_VIDEODRIVER="offscreen",
        SDL_RENDER_DRIVER="opengl",
        SDL_AUDIODRIVER="dummy",
        XYO_EXIT_FRAMES="23",
        XYO_AUDIOPROF="1",
        XYO_IDLE_SOUND_POLL="1" if optimized else "0",
        XYO_PLAY_PROFILE=str(profile),
    )
    proc = subprocess.run(
        [str(binary), "play", str(project), f"--jit={jit}",
         "--fps", "30", "--print-list", "events"],
        env=env, capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 0, proc.stderr
    events = [line for line in proc.stdout.splitlines() if line.strip()]
    assert events == ["flagged", "cleared"], events
    assert "[audioprof] decode tone" in proc.stderr, proc.stderr
    with profile.open() as f:
        frames = list(csv.DictReader(f))
    assert len(frames) == 23
    return events, frames


def test(binary):
    generator = Path(__file__).resolve().parents[1] / "fixtures/gen_idle_sound_poll.py"
    with tempfile.TemporaryDirectory(prefix="xyo-poll-") as td:
        directory = Path(td)
        project = directory / "poll.sb3"
        subprocess.run([sys.executable, str(generator), str(project)], check=True)
        for jit in ("off", "hotspot"):
            ref_events, ref_frames = run(
                binary, project, directory / f"{jit}-off.csv", False, jit
            )
            opt_events, opt_frames = run(
                binary, project, directory / f"{jit}-on.csv", True, jit
            )
            assert ref_events == opt_events
            before = sum(float(f["vm_ms"]) for f in ref_frames[2:5]) / 3
            after = sum(float(f["vm_ms"]) for f in opt_frames[2:5]) / 3
            assert after < before * 0.5, (before, after)
            print(
                f"PASS: {jit} sound poll responds after a delayed flag; "
                f"idle VM time {before:.3f}ms -> {after:.3f}ms"
            )


if __name__ == "__main__":
    test(Path(sys.argv[1]).resolve())
