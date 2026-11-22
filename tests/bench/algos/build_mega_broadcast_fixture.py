#!/usr/bin/env python3
"""Source-certified mega-yield broadcast-and-wait Scratch fixture.

Two green-flag senders launch different broadcast receiver hats. Each
receiver executes a yield-producing control_repeat before the original
sender may execute its single numeric suffix.
"""
import argparse
import json
from pathlib import Path
import zipfile

def block(op, next_block=None, parent=None, inputs=None, fields=None, hat=False):
    b = dict(opcode=op, next=next_block, parent=parent, inputs=inputs or {},
             fields=fields or {}, topLevel=hat, shadow=False)
    if hat:
        b.update(x=0, y=0)
    return b

def project(receiver_counts, without_receiver=False, second_receiver=True):
    blocks = {}
    values = {f"v{i}": [f"v{i}", 0] for i in range(2)}
    values.update({f"rx{i}": [f"rx{i}", 0] for i in range(2)})
    messages = ["PING", "PONG"]
    for index, (name, count) in enumerate(zip(messages, receiver_counts)):
        suffix = str(index)
        blocks["hat" + suffix] = block(
            "event_whenflagclicked", "send" + suffix, hat=True)
        blocks["send" + suffix] = block(
            "event_broadcastandwait", "change" + suffix,
            parent="hat" + suffix,
            inputs={"BROADCAST_INPUT": [1, [11, name, name.lower()]]})
        blocks["change" + suffix] = block(
            "data_changevariableby", parent="send" + suffix,
            inputs={"VALUE": [1, [4, str(10 * (index + 1))]]},
            fields={"VARIABLE": ["v" + suffix, "v" + suffix]})
        if without_receiver and index == 0:
            continue
        if not second_receiver and index == 1:
            continue
        blocks["recv" + suffix] = block(
            "event_whenbroadcastreceived", "repeat" + suffix,
            fields={"BROADCAST_OPTION": [name, name.lower()]}, hat=True)
        blocks["repeat" + suffix] = block(
            "control_repeat", parent="recv" + suffix,
            inputs={"TIMES": [1, [4, str(count)]],
                    "SUBSTACK": [2, "inc" + suffix]})
        blocks["inc" + suffix] = block(
            "data_changevariableby", parent="repeat" + suffix,
            inputs={"VALUE": [1, [4, str(index+1)]]},
            fields={"VARIABLE": ["rx" + suffix, "rx" + suffix]})
    return {"targets": [{
        "isStage": True, "name": "Stage", "variables": values,
        "broadcasts": {msg.lower(): msg for msg in messages},
        "lists": {}, "blocks": blocks, "costumes": [], "sounds": [],
        "currentCostume": 0, "layerOrder": 0, "volume": 100
    }], "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--counts", default="3,2")
    p.add_argument("--without-receiver", action="store_true")
    p.add_argument("--without-second-receiver", action="store_true")
    args = p.parse_args()
    counts = [int(x) for x in args.counts.split(",")]
    if len(counts) != 2 or any(n < 0 for n in counts):
        p.error("--counts requires two nonnegative integers")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    data = project(counts, args.without_receiver,
                   not args.without_second_receiver)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("project.json", json.dumps(data))
    print("expected", {
        "v0": 10, "v1": 20,
        "rx0": 0 if args.without_receiver else counts[0],
        "rx1": 0 if args.without_second_receiver else 2*counts[1]
    })

if __name__ == "__main__":
    main()
