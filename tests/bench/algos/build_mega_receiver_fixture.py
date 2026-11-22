#!/usr/bin/env python3
"""A single sender and three same-message Scratch receiver hats.

The sender performs repeat(N) { broadcast PING and wait }. Each started
recipient has a real Thread identity and yields in its own repeat.
"""
import argparse
import json
from pathlib import Path
import zipfile
from build_mega_broadcast_fixture import block


def project(rounds: int, counts: list[int]) -> dict:
    assert rounds >= 0 and len(counts) == 3 and all(n >= 0 for n in counts)
    blocks = {
        "sender": block("event_whenflagclicked", "sendLoop", hat=True),
        "sendLoop": block("control_repeat", "senderDone", parent="sender",
                          inputs={"TIMES": [1, [4, str(rounds)]],
                                  "SUBSTACK": [2, "send"]}),
        "send": block("event_broadcastandwait", parent="sendLoop",
                      inputs={"BROADCAST_INPUT": [1, [11, "PING", "ping"]]}),
        "senderDone": block("data_changevariableby", parent="sendLoop",
                            inputs={"VALUE": [1, [4, "1"]]},
                            fields={"VARIABLE": ["done", "done"]})
    }
    for i, count in enumerate(counts):
        suffix = str(i)
        blocks["recv" + suffix] = block(
            "event_whenbroadcastreceived", "repeat" + suffix,
            fields={"BROADCAST_OPTION": ["PING", "ping"]}, hat=True)
        blocks["repeat" + suffix] = block(
            "control_repeat", parent="recv" + suffix,
            inputs={"TIMES": [1, [4, str(count)]],
                    "SUBSTACK": [2, "inc" + suffix]})
        blocks["inc" + suffix] = block(
            "data_changevariableby", parent="repeat" + suffix,
            inputs={"VALUE": [1, [4, str(i + 1)]]},
            fields={"VARIABLE": ["rx" + suffix, "rx" + suffix]})
    variables = {"done": ["done", 0]}
    variables.update({f"rx{i}": [f"rx{i}", 0] for i in range(3)})
    return {"targets": [{
        "isStage": True, "name": "Stage", "variables": variables,
        "broadcasts": {"ping": "PING"}, "lists": {},
        "blocks": blocks, "costumes": [], "sounds": [],
        "currentCostume": 0, "layerOrder": 0, "volume": 100
    }], "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--rounds", type=int, default=250)
    p.add_argument("--counts", default="2,3,4")
    a = p.parse_args()
    counts = [int(x) for x in a.counts.split(",")]
    if len(counts) != 3 or any(x < 0 for x in counts) or a.rounds < 0:
        p.error("--counts needs three nonnegative counts and --rounds >= 0")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(project(a.rounds, counts)))
    print(json.dumps({"done": 1, **{
        f"rx{i}": a.rounds * n * (i+1) for i, n in enumerate(counts)
    }}))


if __name__ == "__main__":
    main()
