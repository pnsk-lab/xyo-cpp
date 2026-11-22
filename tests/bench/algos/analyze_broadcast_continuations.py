#!/usr/bin/env python3
"""Inspect Scratch broadcast-and-wait continuations without executing the project.

Reports the lexical control stack and the shortest static procedure call
path from a script hat to each joined broadcast. This is a diagnostic for
Mega Scheduler source-proof coverage; it does NOT certify eligibility.
"""
from __future__ import annotations

import argparse
from collections import deque
import json
from pathlib import Path
import zipfile


def load_project(path: Path) -> dict:
    with zipfile.ZipFile(path) as archive:
        return json.loads(archive.read("project.json"))


def referenced_block(value: object) -> str | None:
    if not isinstance(value, list) or len(value) < 2:
        return None
    child = value[1]
    return child if isinstance(child, str) else None


def procedure_name(block: dict) -> str | None:
    mutation = block.get("mutation", {})
    return mutation.get("proccode") if isinstance(mutation, dict) else None


def procedure_definitions(blocks: dict[str, dict]) -> dict[str, str]:
    result: dict[str, str] = {}
    for block_id, block in blocks.items():
        if not isinstance(block, dict) or block.get("opcode") != "procedures_definition":
            continue
        prototype_id = referenced_block(block.get("inputs", {}).get("custom_block"))
        prototype = blocks.get(prototype_id, {})
        name = procedure_name(prototype)
        if name:
            result[name] = block_id
    return result


def lexical_chain(blocks: dict[str, dict], block_id: str) -> list[str]:
    chain = []
    seen = set()
    current = block_id
    while current and current not in seen and current in blocks:
        seen.add(current)
        chain.append(current)
        parent = blocks[current].get("parent")
        current = parent if isinstance(parent, str) else None
    return chain


def caller_paths(blocks: dict[str, dict], definitions: dict[str, str]) -> dict[str, list[str]]:
    """Resolve only static procedures_call edges, respecting lexical owners."""
    owner_by_block: dict[str, str] = {}
    for block_id in blocks:
        chain = lexical_chain(blocks, block_id)
        if chain:
            owner_by_block[block_id] = chain[-1]

    edges: dict[str, list[tuple[str, str]]] = {}
    for block_id, block in blocks.items():
        if not isinstance(block, dict) or block.get("opcode") != "procedures_call":
            continue
        callee = procedure_name(block)
        definition = definitions.get(callee or "")
        if definition:
            edges.setdefault(owner_by_block[block_id], []).append((definition, callee))

    paths: dict[str, list[str]] = {}
    queue = deque()
    for block_id, block in blocks.items():
        if not isinstance(block, dict) or not block.get("topLevel"):
            continue
        if block.get("opcode", "").startswith("event_when"):
            label = block["opcode"]
            paths[block_id] = [label]
            queue.append(block_id)

    while queue:
        owner = queue.popleft()
        for definition, name in edges.get(owner, []):
            if definition in paths:
                continue
            paths[definition] = paths[owner] + [name]
            queue.append(definition)
    return paths


def caller_frame_paths(blocks: dict[str, dict],
                       definitions: dict[str, str]) -> dict[str, list[dict]]:
    """Record the exact call and post-return block IDs for static call chains.

    This is source metadata only: it does not prove that a dynamic callsite
    is currently active, that reporter arguments remain rooted, or that a
    generated native continuation can safely resume from a return PC.
    """
    owners: dict[str, str] = {}
    for key in blocks:
        chain = lexical_chain(blocks, key)
        if chain:
            owners[key] = chain[-1]
    edges: dict[str, list[tuple[str, dict]]] = {}
    for block_id, block in blocks.items():
        if not isinstance(block, dict) or block.get("opcode") != "procedures_call":
            continue
        name = procedure_name(block)
        definition = definitions.get(name or "")
        if not definition:
            continue
        frame = {
            "callsite_id": block_id,
            "callee": name,
            "return_block_id": block.get("next"),
        }
        edges.setdefault(owners[block_id], []).append((definition, frame))
    routes: dict[str, list[dict]] = {}
    queue = deque()
    for block_id, block in blocks.items():
        if (isinstance(block, dict) and block.get("topLevel") and
                block.get("opcode", "").startswith("event_when")):
            routes[block_id] = []
            queue.append(block_id)
    while queue:
        owner = queue.popleft()
        for definition, frame in edges.get(owner, []):
            if definition in routes:
                continue
            routes[definition] = routes[owner] + [frame]
            queue.append(definition)
    return routes


def reporter_tree(blocks: dict[str, dict], block_id: str, limit: int = 64) -> list[str]:
    visited: set[str] = set()
    opcodes: list[str] = []
    work = [block_id]
    while work and len(visited) < limit:
        current = work.pop()
        if current in visited or current not in blocks:
            continue
        visited.add(current)
        node = blocks[current]
        opcodes.append(node.get("opcode", ""))
        for value in node.get("inputs", {}).values():
            child = referenced_block(value)
            if child:
                work.append(child)
    return opcodes


def broadcast_name(block: dict, blocks: dict[str, dict]) -> str | None:
    value = block.get("inputs", {}).get("BROADCAST_INPUT")
    if not isinstance(value, list) or len(value) < 2:
        return None
    raw = value[1]
    if isinstance(raw, list) and len(raw) >= 3 and raw[0] == 11:
        return str(raw[1])
    if isinstance(raw, str):
        menu = blocks.get(raw, {})
        fields = menu.get("fields", {}).get("BROADCAST_OPTION")
        if menu.get("opcode") == "event_broadcast_menu" and isinstance(fields, list):
            return str(fields[0])
    return None


def analyze_target(target: dict) -> list[dict]:
    blocks = {k: v for k, v in target.get("blocks", {}).items()
              if isinstance(v, dict)}
    definitions = procedure_definitions(blocks)
    paths = caller_paths(blocks, definitions)
    frames = caller_frame_paths(blocks, definitions)
    results = []
    for block_id, block in blocks.items():
        if block.get("opcode") != "event_broadcastandwait":
            continue
        chain = lexical_chain(blocks, block_id)
        ancestors = [blocks[key].get("opcode", "") for key in chain[1:]]
        owner = chain[-1] if chain else ""
        loops = []
        reporters = []
        for key in chain[1:]:
            parent = blocks[key]
            op = parent.get("opcode", "")
            if op not in ("control_repeat", "control_repeat_until",
                          "control_forever", "control_while"):
                continue
            record = {
                "id": key,
                "opcode": op,
                "after_loop_id": parent.get("next"),
                "body_entry_id": referenced_block(
                    parent.get("inputs", {}).get("SUBSTACK")),
            }
            if op == "control_repeat":
                record["times"] = parent.get("inputs", {}).get("TIMES")
            elif op in ("control_repeat_until", "control_while"):
                source = referenced_block(parent.get("inputs", {}).get("CONDITION"))
                record["condition_reporters"] = reporter_tree(blocks, source) if source else []
                reporters.extend(record["condition_reporters"])
            loops.append(record)

        reason = []
        if any(loop["opcode"] != "control_repeat" for loop in loops):
            reason.append("dynamic_or_unbounded_control_loop")
        if any(loop["opcode"] == "control_repeat"
               and referenced_block(loop.get("times")) for loop in loops):
            reason.append("reporter_based_repeat_count")
        if "procedures_definition" in ancestors:
            reason.append("procedure_call_continuation")
        if len(loops) > 1:
            reason.append("nested_loop_state")
        if block.get("next"):
            reason.append("post_broadcast_statements")
        if not broadcast_name(block, blocks):
            reason.append("dynamic_broadcast_message")
        if "sensing_dayssince2000" in reporters:
            reason.append("wall_clock_dependent_condition")
        results.append({
            "target": target["name"],
            "block_id": block_id,
            "message": broadcast_name(block, blocks),
            "lexical_opcodes": ancestors,
            "procedure_path": paths.get(owner, []),
            "caller_frames": frames.get(owner, []),
            "loops": loops,
            "blockers": reason,
        })
    return results


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("--message", action="append",
                        help="Filter by broadcast message; repeatable")
    parser.add_argument("--json", type=Path, help="Also save a JSON report")
    args = parser.parse_args()
    project = load_project(args.project)
    reports = [item for target in project["targets"]
               for item in analyze_target(target)]
    if args.message:
        reports = [r for r in reports if r["message"] in args.message]
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(reports, indent=2, ensure_ascii=False) + "\n")
    for report in reports:
        print(f'{report["target"]} {report["message"] or "<dynamic>"} '
              f'block={report["block_id"]}')
        print("  call path:", " -> ".join(report["procedure_path"]) or "(not statically reachable)")
        print("  controls:", " -> ".join(loop["opcode"] for loop in report["loops"]) or "(none)")
        for frame in report["caller_frames"]:
            print(f'  call PC: {frame["callsite_id"]} {frame["callee"]} '
                  f'-> return {frame["return_block_id"] or "<end>"}')
        for loop in report["loops"]:
            print(f'  loop PC: {loop["id"]} body={loop["body_entry_id"]} '
                  f'after={loop["after_loop_id"] or "<return>"}')
        print("  blockers:", ", ".join(report["blockers"]) or "(none detected)")


if __name__ == "__main__":
    main()
