#!/usr/bin/env python3
"""File-board smoke test (isolated sandbox; never mutates live tasks)."""
from __future__ import annotations

import os
import re
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SANDBOX = ROOT / "board/.test-sandbox"


def command(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bun", "tools/board.ts", *args],
        cwd=ROOT,
        env=dict(os.environ, XYO_BOARD_ROOT=str(SANDBOX)),
        capture_output=True, text=True, timeout=25,
    )


def success(*args: str) -> str:
    result = command(*args)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result.stdout


def main() -> None:
    if SANDBOX.exists():
        shutil.rmtree(SANDBOX)
    (SANDBOX / "tasks").mkdir(parents=True)
    try:
        # Copy a real task's structure, not its live ownership. The shared
        # board can have IR-001 claimed by a different agent at any time.
        fixture = (ROOT / "board/tasks/IR-001.md").read_text()
        fixture = re.sub(r"(?m)^status: .*", "status: open", fixture, count=1)
        fixture = re.sub(r"(?m)^owner: .*", "owner: unassigned", fixture, count=1)
        (SANDBOX / "tasks/IR-001.md").write_text(fixture)
        assert "IR-001" in success("list")
        assert "unassigned" in success("show", "IR-001")

        # Both agents attempt to claim the same task concurrently.
        env = dict(os.environ, XYO_BOARD_ROOT=str(SANDBOX))
        commands = [
            ["bun", "tools/board.ts", "claim", "IR-001", "alpha"],
            ["bun", "tools/board.ts", "claim", "IR-001", "beta"],
        ]
        processes = [
            subprocess.Popen(
                cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True,
            ) for cmd in commands
        ]
        results = [p.communicate(timeout=25) for p in processes]
        codes = [p.returncode for p in processes]
        assert sorted(codes) == [0, 1], (codes, results)
        owner = "alpha" if codes[0] == 0 else "beta"
        other = "beta" if owner == "alpha" else "alpha"
        assert owner in success("show", "IR-001")
        assert command("move", "IR-001", "done", other).returncode != 0
        success("post", "IR-001", owner, "task-specific test post")
        success("post", "general", other, "general test post")
        assert "task-specific test post" in success("read", "IR-001")
        assert "general test post" in success("read", "general")
        assert "task-specific test post" not in success("read", "general")

        assert "review" in success("move", "IR-001", "review", owner)
        assert command("claim", "IR-001", other).returncode != 0
        success("release", "IR-001", owner)
        assert "unassigned" in success("show", "IR-001")
        success("claim", "IR-001", other)
        success("move", "IR-001", "done", other)
        assert command("release", "IR-001", other).returncode != 0

        success("new", "IR-009", "New test task", "P1")
        assert "New test task" in success("list")
        assert command("new", "IR-009", "Duplicate").returncode != 0
        assert command("post", "../private", owner, "bad").returncode != 0
        print("PASS: exclusive claim, owner rights, per-task messages, "
              "review/release/done, new task, path validation")
    finally:
        shutil.rmtree(SANDBOX, ignore_errors=True)


if __name__ == "__main__":
    main()
