#!/usr/bin/env python3
"""Portable execution hot-start differential regressions for xyo-cpp.

Checks independent-process continuation rather than only state-file parsing.
The fixture generators keep these tests self-contained.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests" / "fixtures"


def generate(name: str, output: Path) -> None:
    subprocess.run(
        [sys.executable, str(FIXTURES / name), str(output)],
        check=True, timeout=10,
    )


def run(
    binary: Path, project: Path, mode: str = "run",
    *args: str, env_extra: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env.update(
        XYO_SEED="1618", SDL_VIDEODRIVER="offscreen",
        SDL_RENDER_DRIVER="opengl", SDL_AUDIODRIVER="dummy",
    )
    if env_extra:
        env.update(env_extra)
    return subprocess.run(
        [str(binary), mode, str(project), "--jit=off", "--fps", "30", *args],
        env=env, text=True, capture_output=True, timeout=45,
    )


def assert_success(result: subprocess.CompletedProcess[str], codes=(0, 2)):
    assert result.returncode in codes, (
        f"return code={result.returncode}\n"
        f"stderr={result.stderr[-2000:]}\nstdout={result.stdout[-500:]}"
    )


def assert_continuation(binary: Path, project: Path, tmp: Path, 
                        list_name: str, full_frames: int, saved_frames: int):
    checkpoint = tmp / (project.stem + ".xyhot")
    args = ("--print-list", list_name)
    full = run(binary, project, "run", "--frames", str(full_frames), *args)
    assert_success(full)
    save = run(
        binary, project, "run", "--frames", str(full_frames), *args,
        "--hot-save", str(checkpoint), "--hot-save-frame", str(saved_frames),
    )
    assert_success(save, codes=(0,))
    resume = run(
        binary, project, "run", "--frames", str(full_frames - saved_frames),
        *args, "--hot-load", str(checkpoint),
    )
    assert_success(resume)
    assert full.stdout == resume.stdout, (
        f"{project}: full and resumed state diverged\n"
        f"full={full.stdout[-500:]}\nresumed={resume.stdout[-500:]}"
    )
    assert checkpoint.is_file() and checkpoint.stat().st_size > 1000
    print(f"PASS {project.stem}: {full_frames} frames, "
          f"checkpoint at {saved_frames}, stdout matches ({len(full.stdout)} bytes)")
    return checkpoint


def test_audio(binary: Path, tmp: Path, sync: bool, force_async: bool):
    project = tmp / "audio.sb3"
    if not project.exists():
        generate("gen_audio_async.py", project)
    checkpoint = tmp / ("audio-sync.xyhot" if sync else
                        "audio-pending.xyhot" if force_async else "audio.xyhot")
    environment = {"XYO_AUDIO_SYNC": "1" if sync else "0",
                   "XYO_AUDIO_ASYNC_ALL": "1" if force_async else "0"}
    args = ("--print-list", "events")
    full = run(binary, project, "play", *args,
               env_extra={**environment, "XYO_EXIT_FRAMES": "25"})
    assert_success(full, (0,))
    assert "started" in full.stdout and "completed" in full.stdout, full.stdout
    save = run(
        binary, project, "play", *args,
        "--hot-save", str(checkpoint), "--hot-save-frame", "1",
        env_extra=environment,
    )
    assert_success(save, (0,))
    checkpoint_data = json.loads(checkpoint.read_text())
    assert checkpoint_data["sounds"], checkpoint_data["threads"]
    assert any(s["wait"] != -1 for s in checkpoint_data["sounds"]), (
        checkpoint_data["sounds"]
    )
    resumed = run(
        binary, project, "play", *args, "--hot-load", str(checkpoint),
        env_extra={**environment, "XYO_EXIT_FRAMES": "25"},
    )
    assert_success(resumed, (0,))
    assert resumed.stdout == full.stdout, (
        f"sync={sync} force_async={force_async}\n"
        f"full={full.stdout}\nresumed={resumed.stdout}\n"
        f"stderr={resumed.stderr[-1000:]}"
    )
    print(f"PASS audio: sync={sync} force_async={force_async}, "
          f"voices={len(checkpoint_data['sounds'])}")


def test_pen(
    binary: Path, tmp: Path, mode: str = "default",
    extra: dict[str, str] | None = None,
):
    project = tmp / "pen.sb3"
    if not project.exists():
        generate("gen_hot_start_pen.py", project)
    checkpoint = tmp / f"pen-{mode}.xyhot"
    full_image = tmp / f"pen-{mode}-full.ppm"
    resumed_image = tmp / f"pen-{mode}-resumed.ppm"
    environment = extra or {}
    full = run(
        binary, project, "play",
        env_extra={**environment, "XYO_EXIT_FRAMES": "18",
                   "XYO_SHOT": str(full_image)},
    )
    assert_success(full, (0,))
    save = run(
        binary, project, "play",
        "--hot-save", str(checkpoint), "--hot-save-frame", "8",
        env_extra=environment,
    )
    assert_success(save, (0,))
    resumed = run(
        binary, project, "play", "--hot-load", str(checkpoint),
        env_extra={**environment, "XYO_EXIT_FRAMES": "10",
                   "XYO_SHOT": str(resumed_image)},
    )
    assert_success(resumed, (0,))
    assert full_image.exists() and resumed_image.exists()
    a, b = full_image.read_bytes(), resumed_image.read_bytes()
    assert a == b, (
        f"pen bitmap mismatch, mode={mode}, full={len(a)} "
        f"bytes, resumed={len(b)} bytes"
    )
    print(f"PASS pen: mode={mode}, exact PPM image match ({len(a)} bytes)")


def main(binary: Path):
    binary = binary.resolve()
    with tempfile.TemporaryDirectory(prefix="xyo-hot-start-") as work:
        tmp = Path(work)
        scalar_project = tmp / "scalar.sb3"
        generate("gen_hot_start.py", scalar_project)
        scalar_checkpoint = assert_continuation(
            binary, scalar_project, tmp, "history", 18, 8)
        clone_project = tmp / "clones.sb3"
        generate("gen_hot_start_clones.py", clone_project)
        assert_continuation(binary, clone_project, tmp, "events", 18, 3)

        incorrect_project = run(
            binary, clone_project, "run", "--frames", "1",
            "--hot-load", str(scalar_checkpoint),
        )
        assert incorrect_project.returncode != 0
        assert "SB3 contents differ" in incorrect_project.stderr
        print("PASS rejected checkpoint for different SB3")

        corrupt = json.loads(scalar_checkpoint.read_text())
        corrupt["targets"][0]["variables"][0]["value"] = [
            "b", "7ff90000deadbeef"
        ]
        corrupt_path = tmp / "corrupt.xyhot"
        corrupt_path.write_text(json.dumps(corrupt))
        rejected = run(
            binary, scalar_project, "run", "--frames", "1",
            "--hot-load", str(corrupt_path),
        )
        assert rejected.returncode != 0
        assert "invalid variable value" in rejected.stderr, rejected.stderr
        print("PASS rejected unsafe pointer tag in corrupted checkpoint")

        for sync, forced_async in ((True, False), (False, False), (False, True)):
            test_audio(binary, tmp, sync, forced_async)
        test_pen(binary, tmp)
        test_pen(binary, tmp, "cpu", {"XYO_PEN_CPU": "1"})
        test_pen(binary, tmp, "hq", {"XYO_HIGH_QUALITY_PEN": "1"})
    print("PASS: hot-start regression suite")


if __name__ == "__main__":
    main(Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build" / "xyo"))
