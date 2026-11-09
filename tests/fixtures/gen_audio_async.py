#!/usr/bin/env python3
"""Generate Scratch 3 audio wait + cancellation regression fixtures."""
import argparse
import io
import json
import math
import wave
import zipfile


def block(op, next_id=None, parent=None, inputs=None, fields=None, **extras):
    return dict(
        opcode=op, next=next_id, parent=parent,
        inputs=inputs or {}, fields=fields or {}, shadow=False,
        topLevel=parent is None, **extras,
    )


def make(output, cancel):
    pcm = bytearray()
    for i in range(48000 // 5):
        value = round(math.sin(2 * math.pi * i * 440 / 48000) * 12000)
        pcm.extend(int(value).to_bytes(2, "little", signed=True))
    wav = io.BytesIO()
    with wave.open(wav, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(48000)
        f.writeframes(pcm)
    sounds = [{
        "assetId": "tone",
        "name": "tone",
        "dataFormat": "wav",
        "md5ext": "tone.wav",
        "rate": 48000,
        "sampleCount": 9600,
        "format": "",
    }]
    blocks = {
        "hat": block("event_whenflagclicked", next_id="start"),
        "start": block("data_addtolist", next_id="sound", parent="hat",
            fields={"LIST": ["events", "events-id"]},
            inputs={"ITEM": [1, [10, "started"]]}),
        "sound": block("sound_playuntildone", next_id="end", parent="start",
            inputs={"SOUND_MENU": [1, "menu"]}),
        "menu": block("sound_sounds_menu", parent="sound",
            fields={"SOUND_MENU": ["tone", None]}),
        "end": block("data_addtolist", parent="sound",
            fields={"LIST": ["events", "events-id"]},
            inputs={"ITEM": [1, [10, "completed"]]}),
    }
    if cancel:
        blocks["cancelhat"] = block("event_whenflagclicked", next_id="delay")
        blocks["delay"] = block("control_wait", parent="cancelhat",
            next_id="cancel", inputs={"DURATION": [1, [5, "0.06"]]})
        blocks["cancel"] = block("sound_stopallsounds", parent="delay")
    stage = {
        "isStage": True, "name": "Stage", "variables": {},
        "lists": {"events-id": ["events", []]}, "broadcasts": {},
        "blocks": {}, "costumes": [], "sounds": [], "currentCostume": 0,
        "layerOrder": 0,
    }
    sprite = {
        "isStage": False, "name": "AudioWait", "variables": {}, "lists": {},
        "broadcasts": {}, "blocks": blocks, "costumes": [], "sounds": sounds,
        "currentCostume": 0, "layerOrder": 1,
        "x": 0, "y": 0, "size": 100, "direction": 90,
        "rotationStyle": "all around", "visible": True, "volume": 100,
    }
    project = {
        "targets": [stage, sprite], "monitors": [],
        "extensions": [], "meta": {"semver": "3.0.0"},
    }
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(project))
        z.writestr("tone.wav", wav.getvalue())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("output")
    parser.add_argument("--cancel", action="store_true")
    a = parser.parse_args()
    make(a.output, a.cancel)
