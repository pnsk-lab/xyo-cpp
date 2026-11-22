#!/usr/bin/env python3
"""Two repeat(send broadcast and wait) Scratch scripts, fixed message inputs.

The native mega-goto backend must launch the receiver exactly once PER
repeat, wait for those Thread identities, and resume the next iteration.
"""
import argparse
import json
from pathlib import Path
import zipfile

from build_mega_broadcast_fixture import block, project

def build(sender_counts, receiver_counts, missing_receivers=False):
    data = project(receiver_counts, without_receiver=missing_receivers,
                   second_receiver=not missing_receivers)
    blocks = data["targets"][0]["blocks"]
    for i, count in enumerate(sender_counts):
        key = str(i)
        blocks["hat"+key]["next"] = "sendLoop"+key
        blocks["sendLoop"+key] = block(
            "control_repeat", "change"+key, parent="hat"+key,
            inputs={
                "TIMES":[1,[4,str(count)]],
                "SUBSTACK":[2,"send"+key],
            })
        blocks["send"+key]["parent"] = "sendLoop"+key
        blocks["send"+key]["next"] = None
        blocks["change"+key]["parent"] = "sendLoop"+key
    return data

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--send-counts", default="3,2")
    ap.add_argument("--recv-counts", default="2,3")
    ap.add_argument("--missing-receivers", action="store_true")
    a = ap.parse_args()
    sender_counts = [int(v) for v in a.send_counts.split(",")]
    receiver_counts = [int(v) for v in a.recv_counts.split(",")]
    if len(sender_counts) != 2 or len(receiver_counts) != 2 or any(
            n < 0 for n in sender_counts+receiver_counts):
        ap.error("--send-counts and --recv-counts require two nonnegative integers")
    data = build(sender_counts,receiver_counts,a.missing_receivers)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(a.out,"w",zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json",json.dumps(data))
    print("expected",{"v0":10,"v1":20,
        "rx0":0 if a.missing_receivers else sender_counts[0]*receiver_counts[0],
        "rx1":0 if a.missing_receivers else sender_counts[1]*receiver_counts[1]*2})

if __name__=="__main__":
    main()
