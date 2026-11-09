#!/usr/bin/env python3
"""Verify cached SVG raster RGBA is identical, including corruption fallback."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile


def make_project(path):
    art = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="960" height="540"',
        ' viewBox="0 0 960 540">',
        '<rect width="960" height="540" fill="#193052"/>',
    ]
    for i in range(400):
        x = (i * 43) % 950
        y = (i * 91) % 530
        art.append(
            f'<rect x="{x}" y="{y}" width="27" height="13" '
            f'fill="#{(i * 937) % 0xffffff:06x}" opacity=".6"/>'
        )
    art.append("</svg>")
    stage = {
        "isStage": True, "name": "Stage", "variables": {},
        "lists": {}, "broadcasts": {}, "blocks": {},
        "costumes": [], "sounds": [], "currentCostume": 0, "layerOrder": 0,
    }
    sprite = {
        "isStage": False, "name": "SVG",
        "variables": {}, "lists": {}, "broadcasts": {}, "blocks": {},
        "costumes": [{
            "assetId": "large", "name": "large-test",
            "md5ext": "large.svg", "dataFormat": "svg",
            "rotationCenterX": 480, "rotationCenterY": 270,
            "bitmapResolution": 1,
        }],
        "sounds": [], "currentCostume": 0, "layerOrder": 1,
        "x": 0, "y": 0, "size": 100, "direction": 90,
        "rotationStyle": "all around", "visible": True, "volume": 100,
    }
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps({
            "targets": [stage, sprite], "monitors": [], "extensions": [],
            "meta": {"semver": "3.0.0"},
        }))
        z.writestr("large.svg", "\n".join(art))


def ppm(path):
    with path.open("rb") as f:
        assert f.readline().strip() == b"P6"
        dims = tuple(map(int, f.readline().split()))
        assert f.readline().strip() == b"255"
        data = f.read()
        assert len(data) == dims[0] * dims[1] * 3
        return dims, data


def play(binary, project, path, cache_root):
    env = os.environ.copy()
    env.update(
        SDL_VIDEODRIVER="offscreen",
        SDL_RENDER_DRIVER="opengl",
        SDL_AUDIODRIVER="dummy",
        XYO_EXIT_FRAMES="3",
        XYO_SKIN_CACHE_DIR=str(cache_root),
        XYO_SKINPROF="1",
        XYO_SHOT=str(path),
    )
    env.pop("XYO_SKIN_CACHE", None)
    process = subprocess.run(
        [str(binary), "play", str(project), "--jit=off", "--fps", "30"],
        env=env, text=True, capture_output=True, timeout=60,
    )
    assert process.returncode == 0, process.stderr
    return process.stderr


def test(binary):
    with tempfile.TemporaryDirectory(prefix="xyo-skin-cache-") as tmp:
        root = Path(tmp)
        project = root / "svg.sb3"
        make_project(project)
        cache = root / "cache"
        first = root / "cold.ppm"
        second = root / "warm.ppm"
        third = root / "corrupt.ppm"
        log1 = play(binary, project, first, cache)
        assert "cache=miss" in log1, log1
        files = list(cache.glob("skin-*.rgba"))
        assert len(files) == 1, files
        log2 = play(binary, project, second, cache)
        assert "cache=hit" in log2, log2
        assert ppm(first) == ppm(second), "cached image differs from raster"
        with files[0].open("r+b") as file:
            file.seek(-100, 2)
            b = file.read(1)
            file.seek(-100, 2)
            file.write(bytes([b[0] ^ 0xff]))
        log3 = play(binary, project, third, cache)
        assert "cache=miss" in log3, log3
        assert ppm(first) == ppm(third), "corrupted cache not regenerated"
    print("PASS: warm SVG cache pixel parity and corruption recovery")


if __name__ == "__main__":
    test(Path(sys.argv[1]).resolve())
