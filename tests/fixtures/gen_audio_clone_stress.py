#!/usr/bin/env python3
"""Generate an SB3 with rapidly disposed sound-playing clones.

Before the fix the SDL audio callback read Target::soundEffects from freed
clone memory and could SIGSEGV in std::map::find.
"""
import json
from pathlib import Path
import sys
import tempfile
import zipfile

from gen_audio_async import make, block


def generate(output: Path):
    with tempfile.TemporaryDirectory(prefix="xyo-clone-sound-") as temp:
        base = Path(temp) / "base.sb3"
        make(base, False)
        with zipfile.ZipFile(base) as archive:
            project = json.loads(archive.read("project.json"))
            wav = archive.read("tone.wav")
    stage, sprite = project["targets"]
    b = {
        "hat": block("event_whenflagclicked", next_id="repeat"),
        "repeat": block("control_repeat", parent="hat",
            inputs={"TIMES": [1, [6, "120"]], "SUBSTACK": [2, "create"]}),
        "create": block("control_create_clone_of", parent="repeat",
            inputs={"CLONE_OPTION": [1, "cloneMenu"]}),
        "cloneMenu": block("control_create_clone_of_menu", parent="create",
            fields={"CLONE_OPTION": ["_myself_", None]}),
        "cloneHat": block("control_start_as_clone", next_id="started"),
        "started": block("data_addtolist", parent="cloneHat", next_id="play",
            fields={"LIST": ["events", "events-id"]},
            inputs={"ITEM": [1, [10, "started"]]}),
        "play": block("sound_play", parent="started", next_id="pitch",
            inputs={"SOUND_MENU": [1, "soundMenu"]}),
        "soundMenu": block("sound_sounds_menu", parent="play",
            fields={"SOUND_MENU": ["tone", None]}),
        # Mutate the std::map used by the old SDL callback.
        "pitch": block("sound_seteffectto", parent="play", next_id="wait",
            fields={"EFFECT": ["PITCH", None]},
            inputs={"VALUE": [1, [4, "20"]]}),
        "wait": block("control_wait", parent="pitch", next_id="done",
            inputs={"DURATION": [1, [5, "0.03"]]}),
        "done": block("data_addtolist", parent="wait", next_id="delete",
            fields={"LIST": ["events", "events-id"]},
            inputs={"ITEM": [1, [10, "disposed"]]}),
        "delete": block("control_delete_this_clone", parent="done"),
    }
    sprite["blocks"] = b
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("project.json", json.dumps(project))
        archive.writestr("tone.wav", wav)


if __name__ == "__main__":
    generate(Path(sys.argv[1]))
