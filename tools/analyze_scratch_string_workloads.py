#!/usr/bin/env python3
"""Static string-op histogram in local Scratch projects (not dynamic counts)."""
import argparse
from collections import Counter
import json
from pathlib import Path
import zipfile

OPS = {
    "operator_join", "operator_letter_of", "operator_length",
    "operator_contains", "operator_equals", "operator_lt",
    "operator_gt", "data_listcontents",
    "data_addtolist", "data_itemoflist",
}


def read_blocks(path):
    with zipfile.ZipFile(path) as f:
        project = json.loads(f.read("project.json"))
    return [t.get("blocks", {}) for t in project.get("targets", [])]


def count_one(path):
    hist = Counter()
    for blocks in read_blocks(path):
        hist.update(
            b["opcode"] for b in blocks.values()
            if isinstance(b, dict) and b.get("opcode") in OPS
        )
        # Identify syntactic s <- join(s, suffix) (in Scratch 3 JSON, a var
        # reference can be a nested reporter or a compressed [12, name, id]).
        for b in blocks.values():
            if not isinstance(b, dict) or b.get("opcode") != "data_setvariableto":
                continue
            field = b.get("fields", {}).get("VARIABLE", [])
            value = b.get("inputs", {}).get("VALUE", [])
            if len(field) < 2 or len(value) < 2:
                continue
            item = value[1]
            if not isinstance(item, str):
                continue
            join = blocks.get(item)
            if not isinstance(join, dict) or join.get("opcode") != "operator_join":
                continue
            left = join.get("inputs", {}).get("STRING1", [])
            if len(left) < 2:
                continue
            child = left[1]
            if (isinstance(child, list) and len(child) >= 3 and
                    child[0] == 12 and child[2] == field[1]):
                hist["self_append"] += 1
            elif isinstance(child, str):
                ref = blocks.get(child, {})
                if (isinstance(ref, dict) and ref.get("opcode") == "data_variable"
                        and ref.get("fields", {}).get("VARIABLE", [None, None])[1]
                        == field[1]):
                    hist["self_append"] += 1
    return hist


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*", type=Path)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    paths = args.paths or [
        *sorted(Path("debug/projects").glob("*.sb3")),
        *sorted(Path("scratch-bench/.cache/build").glob("*.sb3")),
    ]
    rows = {}
    for path in paths:
        try:
            c = count_one(path)
        except (OSError, KeyError, ValueError, zipfile.BadZipFile) as e:
            print(f"SKIP {path}: {e}")
            continue
        rows[str(path)] = dict(c)
    totals = sum((Counter(c) for c in rows.values()), Counter())
    report = {"scope": "Static SB3 opcode and syntactic self-append counts; not runtime frequency",
              "files": len(rows), "totals": dict(totals), "per_project": rows}
    print("projects:", len(rows), "static counts:", json.dumps(totals.most_common(),
                                                             ensure_ascii=False))
    for path, counts in sorted(rows.items(), key=lambda kv: sum(kv[1].values()),
                                reverse=True)[:12]:
        print(path, dict(Counter(counts).most_common(8)))
    if args.json:
        args.json.parent.mkdir(exist_ok=True, parents=True)
        args.json.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
