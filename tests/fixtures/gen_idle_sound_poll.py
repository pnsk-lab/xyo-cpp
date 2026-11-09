#!/usr/bin/env python3
"""Scratch fixture: a pure music-polled forever reacts to a flag after wait."""
import json
import sys
import tempfile
import zipfile
from pathlib import Path
from gen_audio_async import make, block


def generate(output):
    with tempfile.TemporaryDirectory(prefix="audio-poller-") as td:
        base = Path(td) / "base.sb3"
        make(str(base), False)
        with zipfile.ZipFile(base) as z:
            project = json.loads(z.read("project.json"))
            tone = z.read("tone.wav")
    stage, sprite = project["targets"]
    stage["variables"] = {"flag-id": ["flag", 0]}
    blocks = {
        "pollhat": block("event_whenflagclicked", next_id="forever"),
        "forever": block(
            "control_forever", parent="pollhat",
            inputs={"SUBSTACK": [2, "if"]},
        ),
        "if": block(
            "control_if", parent="forever",
            inputs={"CONDITION": [2, "eq"], "SUBSTACK": [2, "play"]},
        ),
        "eq": block(
            "operator_equals", parent="if",
            inputs={
                "OPERAND1": [3, [12, "flag", "flag-id"], [10, ""]],
                "OPERAND2": [1, [10, "1"]],
            },
        ),
        "play": block(
            "sound_playuntildone", parent="if",
            inputs={"SOUND_MENU": [1, "menu"]},
        ),
        "menu": block(
            "sound_sounds_menu", parent="play",
            fields={"SOUND_MENU": ["tone", None]},
        ),
        "timer": block("event_whenflagclicked", next_id="wait"),
        "wait": block(
            "control_wait", parent="timer", next_id="set",
            inputs={"DURATION": [1, [5, "0.16"]]},
        ),
        "set": block(
            "data_setvariableto", parent="wait", next_id="marked",
            fields={"VARIABLE": ["flag", "flag-id"]},
            inputs={"VALUE": [1, [10, "1"]]},
        ),
        "marked": block(
            "data_addtolist", parent="set", next_id="wait2",
            fields={"LIST": ["events", "events-id"]},
            inputs={"ITEM": [1, [10, "flagged"]]},
        ),
        "wait2": block(
            "control_wait", parent="marked", next_id="clear",
            inputs={"DURATION": [1, [5, "0.07"]]},
        ),
        "clear": block(
            "data_setvariableto", parent="wait2", next_id="cleared",
            fields={"VARIABLE": ["flag", "flag-id"]},
            inputs={"VALUE": [1, [10, "0"]]},
        ),
        "cleared": block(
            "data_addtolist", parent="clear",
            fields={"LIST": ["events", "events-id"]},
            inputs={"ITEM": [1, [10, "cleared"]]},
        ),
    }
    sprite["blocks"] = blocks
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(project, ensure_ascii=False))
        z.writestr("tone.wav", tone)


if __name__ == "__main__":
    generate(sys.argv[1])
