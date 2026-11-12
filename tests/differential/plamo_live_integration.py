#!/usr/bin/env python3
"""Optional real PLaMo server test. Does not download model weights."""
import json
import os
import subprocess
import sys
from pathlib import Path
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from translate_plamo_local import create_fixture


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: python3 tests/differential/plamo_live_integration.py PATH_TO_XYO")
    binary = Path(sys.argv[1]).resolve()
    port = int(os.environ.get("XYO_PLAMO_PORT", "8000"))
    assert 1 <= port <= 65535
    with urlopen(f"http://127.0.0.1:{port}/v1/models", timeout=4) as response:
        models = json.load(response)
    assert any(m.get("id") == "plamo-3-translate"
               for m in models.get("data", [])), models
    project = ROOT / "board/.translate-implementation/test/plamo-live.sb3"
    create_fixture(project)
    for enabled in (False, True):
        for mode in ("off", "eager", "hotspot"):
            env = os.environ.copy()
            env.update(XYO_DUMPVARS="json", XYO_GPU="0",
                       XYO_PAR="0", XYO_PLAMO_PORT=str(port))
            if enabled:
                env["XYO_PLAMO_TRANSLATE"] = "1"
            else:
                env.pop("XYO_PLAMO_TRANSLATE", None)
            proc = subprocess.run(
                [str(binary), "run", str(project), "--jit", mode,
                 "--frames", "30"],
                env=env, cwd=ROOT, text=True, capture_output=True, timeout=25)
            assert proc.returncode == 0, (enabled, mode, proc.stderr[-500:])
            result = json.loads(proc.stdout[proc.stdout.index("{"):
                                            proc.stdout.rindex("}")+1])["Bench"]
            expected = "スクラッチからこんにちは" if enabled else "Hello from Scratch"
            assert result["translation"] == expected, (enabled, mode, result)
            assert result["second"] == expected, (enabled, mode, result)
            assert result["digits"] == "12345", result
            assert result["done"] == "1", result
            print(f"PASS real PLaMo mode={mode} enabled={enabled} result={expected}")


if __name__ == "__main__":
    main()
