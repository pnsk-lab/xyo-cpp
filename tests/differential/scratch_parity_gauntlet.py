#!/usr/bin/env python3
"""Deterministic Scratch 3 semantic gauntlet: scratch-vm vs xyo interp/eager/hotspot.

All test cases are real Scratch blocks in generated SB3 projects. The oracle
is the vendored scratch-vm interpreter, never a Python reimplementation of
Scratch coercions. Every probe has a stable case ID and is logged in order.

Usage:
  python3 tests/differential/scratch_parity_gauntlet.py
  python3 tests/differential/scratch_parity_gauntlet.py --modes off,eager,hotspot
  python3 tests/differential/scratch_parity_gauntlet.py --generate-only
  python3 tests/differential/scratch_parity_gauntlet.py --json /tmp/parity.json
"""
from __future__ import annotations

import argparse
import difflib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
from dataclasses import dataclass
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))
from gen_algo_bench import (  # noqa: E402
    Gen, S_add, S_call, S_change, S_del, S_delall, S_ifelse,
    S_repeat, S_replace, S_set, add, div, eq, gt, item, join,
    length, lt, make_project, mod, mul, num, op, round_, sub,
    text, var, write_sb3,
)

@dataclass
class Probe:
    category: str
    label: str
    statements: list[Any]
    samples: int = 1


class ExtendedGen(Gen):
    """Only the standard Scratch 'insert at' statement is missing in Gen."""

    def emit_stmt(self, statement, parent=None):
        if statement[0] != "insert":
            return super().emit_stmt(statement, parent)
        _, name, index, value = statement
        bid = self.new(
            "data_insertatlist",
            parent=parent,
            fields={"LIST": self.field("LIST", name)},
        )
        self.blocks[bid]["inputs"]["INDEX"] = self.emit_value(index, bid)
        self.blocks[bid]["inputs"]["ITEM"] = self.emit_value(value, bid)
        return bid


def insert(name, index, value):
    return ("insert", name, index, value)


def mathop(name, value):
    return op("operator_mathop", {"OPERATOR": name}, NUM=value)


def contains(a, b):
    return op("operator_contains", STRING1=a, STRING2=b)


def strlen(a):
    return op("operator_length", STRING=a)


def letter(a, index):
    return op("operator_letter_of", LETTER=index, STRING=a)


def list_index(a):
    return op("data_itemnumoflist", {"LIST": "data"}, ITEM=a)


def list_contains(a):
    return op("data_listcontainsitem", {"LIST": "data"}, ITEM=a)


def list_contents():
    return op("data_listcontents", {"LIST": "data"})


def value_probe(category, label, expression):
    return Probe(category, label, [S_add("out", expression)])


def boolean_probe(category, label, condition):
    return Probe(category, label, [
        S_ifelse(condition,
                 [S_add("out", text("true"))],
                 [S_add("out", text("false"))]),
    ])


def mutation_probe(label, statements):
    return Probe("list-mutation", label, statements, samples=5)


def make_probes(seed):
    probes = []
    vals = [
        "", " ", "  ", "\t", "\n", "0", "-0", "+0", "00",
        "0.0", "-0.0", "0.5", "-.5", ".5", "+.5",
        "1", "1.0", "01", "1e3", "1e-3", "1e309", "-1e309",
        "0x10", "0Xff", "0b101", "0o77", "0x", "0b",
        "Infinity", "-Infinity", "+Infinity", "NaN", "nan",
        "false", "true", "null", "undefined", "0foo",
        "1_000", "1,5", "\u00a0", "\u3000", "\u200b",
        "\u22121", "3.141592653589793", "9007199254740993",
    ]
    for i, value in enumerate(vals):
        a = text(value)
        for name, fn in [
            ("plus0", lambda x: add(x, num(0))),
            ("minus1", lambda x: sub(x, num(1))),
            ("times2", lambda x: mul(x, num(2))),
            ("div2", lambda x: div(x, num(2))),
            ("round", round_),
            ("floor", lambda x: mathop("floor", x)),
        ]:
            probes.append(value_probe("coercion", f"v{i:02d}-{name}", fn(a)))

    fp = [
        div(num(0), num(0)), div(num(1), num(0)),
        div(num(-1), num(0)), div(num(0), num(-1)),
        div(num(1), sub(num(0), num(0))),
        div(num(1e-300), num(1e300)),
        div(num(1e300), num(1e-300)),
        div(num(1), num(-1e300)),
        num(1e21), num(1e20), num(9007199254740992),
        num(0.49999999999999994), num(-0.5), num(-1.5),
        num(1.5), num(2.5), num(-2.5),
        num(5e-324), num(-5e-324),
    ]
    for i, x in enumerate(fp):
        for opname, fn in [
            ("identity", lambda a: a),
            ("round", round_), ("mod3", lambda a: mod(a, num(3))),
            ("abs", lambda a: mathop("abs", a)),
            ("sqrt", lambda a: mathop("sqrt", a)),
            ("ln", lambda a: mathop("ln", a)),
            ("log", lambda a: mathop("log", a)),
            ("sin", lambda a: mathop("sin", a)),
        ]:
            probes.append(value_probe("floating-point", f"fp{i:02d}-{opname}", fn(x)))
    for i, a in enumerate((-100, -7, -3, -1, 0, 1, 3, 7, 100)):
        for j, b in enumerate((-7, -3, -1, 0, 1, 3, 7)):
            probes.append(value_probe("modulo", f"mod{i:02d}-{j:02d}",
                                      mod(num(a), num(b))))

    pairs = [
        ("0", "0"), ("", "0"), (" ", "0"), ("01", "1"),
        ("1.0", "1"), ("Infinity", "infinity"), ("NaN", "nan"),
        ("001", " 1 "), ("true", "TRUE"), ("false", "0"),
        ("-0", "0"), ("1e3", "1000"), ("0x10", "16"),
        ("a", "A"), ("ß", "SS"), ("ẞ", "ß"),
        ("Σ", "ς"), ("İ", "i"), ("I", "ı"),
        ("é", "e\u0301"), ("K", "k"), ("Å", "å"),
        ("ﬁ", "fi"), ("𐐀", "𐐨"), ("😀", "😀"),
        ("東京", "東京"), ("a\u200bb", "ab"), ("\u00a0", " "),
    ]
    for i, (a, b) in enumerate(pairs):
        for op_name, fn in [
            ("eq", eq), ("lt", lt), ("gt", gt),
            ("contains_ab", contains),
            ("contains_ba", lambda x, y: contains(y, x)),
        ]:
            probes.append(boolean_probe(
                "comparisons", f"cmp{i:02d}-{op_name}", fn(text(a), text(b))))

    strings = [
        "", "a", "ABC", "Straße", "Σςσ", "İstanbul",
        "é", "e\u0301", "漢字", "寿司🍣", "😀", "👨‍👩‍👧‍👦",
        "𝄞", "a\u0000b", "a\nb", "a\tb", "𐐀𐐨",
        "a" * 1024 + "😀", "　", "\u200b",
    ]
    indices = ["-2", "-1", "0", "0.5", "1", "1.5", "2",
               "3", "4", "100", "Infinity", "NaN", "last"]
    for i, s in enumerate(strings):
        probes.append(value_probe("unicode", f"str{i:02d}-len", strlen(text(s))))
        for j, idx in enumerate(indices):
            probes.append(value_probe(
                "unicode", f"str{i:02d}-letter{j:02d}", letter(text(s), text(idx))))
        probes.append(value_probe("unicode", f"str{i:02d}-join", join(text(s), text("x"))))
    for i, x in enumerate(strings):
        for j in (0, 1, 3, 7, 12, 19):
            probes.append(boolean_probe(
                "unicode-contains", f"substr{i:02d}-{j:02d}",
                contains(text(x), text(strings[j]))))

    initial = ["A", "a", "01", "1", "", " ", "NaN", "😀", "東京"]
    list_indices = ["-10", "-1", "0", "0.5", "1", "1.5",
                    "2", "2.5", "9", "10", "100", "last",
                    "Infinity", "-Infinity", "NaN", "hello"]
    for idxno, idx in enumerate(list_indices):
        prefix = [S_delall("data")] + [S_add("data", text(v)) for v in initial]
        for opname, action in [
            ("read", []),
            ("delete", [S_del("data", text(idx))]),
            ("replace", [S_replace("data", text(idx), text("Z"))]),
            ("insert", [insert("data", text(idx), text("Z"))]),
        ]:
            probes.append(mutation_probe(
                f"list-{opname}-{idxno:02d}",
                prefix + action + [
                    S_add("out", length("data")),
                    S_add("out", list_contents()),
                    S_add("out", item("data", num(1))),
                    S_add("out", item("data", num(2))),
                    S_add("out", item("data", num(9))),
                ],
            ))
    for i, x in enumerate(["a", "A", "01", "1", "1.0", "NaN",
                            "😀", "", "0", "Infinity", "TOKYO", "東京"]):
        probes.append(Probe("list-search", f"find{i:02d}", [
            S_delall("data"),
            *[S_add("data", text(v)) for v in initial],
            S_add("out", list_index(text(x))),
            S_ifelse(list_contains(text(x)), [
                S_add("out", text("true")),
            ], [
                S_add("out", text("false")),
            ]),
        ], samples=2))

    # HotSpot/JIT SSA must survive repeated transitions between boxed numbers
    # and nonnumeric strings. Include boundary trip counts and both warp modes.
    for warp in (True, False):
        for n in (0, 1, 2, 31, 63, 64, 65, 127, 255):
            statements = [
                S_set("slot", num(-1)),
                S_set("sum", num(0)),
                S_set("i", num(0)),
                S_repeat(num(n), [
                    S_set("slot", join(text("0"), var("i"))),
                    S_change("sum", var("slot")),
                    S_set("slot", add(var("slot"), num(1))),
                    S_set("slot", join(text("prefix"), var("slot"))),
                    S_set("slot", add(var("slot"), num(1))),
                    S_change("i", num(1)),
                ]),
                S_add("out", var("slot")),
                S_add("out", var("sum")),
                S_add("out", var("i")),
            ]
            probes.append(Probe("type-transition-warp" if warp else "type-transition-yield",
                                f"type-{int(warp)}-{n:03d}", statements, samples=3))
    for warp in (True, False):
        for n in (0, 1, 2, 3, 7, 31, 32, 63, 64, 65, 127, 255):
            branch = [
                S_ifelse(eq(mod(var("i"), num(3)), num(0)), [
                    S_set("slot", text("0x10")),
                    S_change("sum", var("slot")),
                ], [
                    S_ifelse(eq(mod(var("i"), num(3)), num(1)), [
                        S_set("slot", text("scratch")),
                        S_change("sum", var("slot")),
                    ], [
                        S_set("slot", div(num(1), num(0))),
                        S_change("sum", var("slot")),
                    ]),
                ]),
                S_set("slot", add(var("slot"), num(1))),
                S_change("i", num(1)),
            ]
            probes.append(Probe(
                "type-cfg-warp" if warp else "type-cfg-yield",
                f"cfg-{int(warp)}-{n:03d}",
                [S_set("i", num(0)), S_set("slot", num(-1)),
                 S_set("sum", num(0)),
                 S_repeat(num(n), branch),
                 S_add("out", var("slot")),
                 S_add("out", var("sum")),
                 S_add("out", var("i")),
                 S_add("out", add(var("slot"), text("0")))],
                samples=4,
            ))
    rng = random.Random(seed)
    choices = vals[:36] + ["🍣", "İ", "𐐀"]
    for n in range(512):
        a, b = rng.choice(choices), rng.choice(choices)
        method = n % 5
        if method == 0:
            expr = add(text(a), text(b))
        elif method == 1:
            expr = div(text(a), add(text(b), num(1)))
        elif method == 2:
            expr = mod(text(a), add(text(b), num(1)))
        elif method == 3:
            expr = join(round_(text(a)), round_(text(b)))
        else:
            expr = letter(join(text(a), text(b)), num((n % 9) - 2))
        probes.append(value_probe("seeded-fuzz", f"fuzz{n:03d}", expr))
    return probes


def shard_probes(probes, max_items):
    groups = {}
    for p in probes:
        groups.setdefault(p.category, []).append(p)
    shards = []
    for category, items in groups.items():
        for start in range(0, len(items), max_items):
            shards.append((f"{category}-{start // max_items:02d}", items[start:start+max_items]))
    return shards


def build_shard(path, probes, warp):
    gen = ExtendedGen(["done", "slot", "sum", "i"], ["out", "data"])
    body = [S_delall("out")]
    manifest = []
    for p in probes:
        marker = p.label
        body.append(S_add("out", text(marker)))
        body.extend(p.statements)
        manifest.append({"id": marker, "category": p.category, "outputs": p.samples})
    body.append(S_set("done", num(1)))
    # An explicit repeat guarantees the HotSpot loop heuristic is exercised.
    gen.define("gauntlet", [], [S_repeat(num(1), body)], warp=warp, x=0, y=500)
    gen.script([S_call("gauntlet")])
    write_sb3(path, make_project(gen.vars.keys(), gen.lists.keys(), gen))
    return manifest


def run(command, env, timeout):
    try:
        p = subprocess.run(
            command, cwd=ROOT, env=env, capture_output=True,
            text=True, encoding="utf-8", errors="surrogatepass",
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as ex:
        raise RuntimeError(f"timeout ({timeout}s): {command}") from ex
    if p.returncode:
        raise RuntimeError(
            f"exit={p.returncode}: {command}\n"
            f"stderr: {p.stderr[-1800:]}\nstdout: {p.stdout[-1000:]}"
        )
    start, end = p.stdout.find("{"), p.stdout.rfind("}")
    if start == -1 or end == -1:
        raise RuntimeError(f"missing JSON: {command}\n{p.stdout[-1500:]}")
    return json.loads(p.stdout[start:end+1])


def check_done(data, mode, shard):
    bench = data.get("Bench", {})
    if bench.get("done") != "1":
        raise RuntimeError(
            f"{shard} {mode}: did not finish (done={bench.get('done')!r})"
        )
    return bench.get("out", [])


def compare(expected, actual, manifest):
    if expected == actual:
        return []
    # Output list entries alternate a case marker and 1-5 observations.
    def unpack(items):
        rows = {}
        offset = 0
        for p in manifest:
            count = p["outputs"]
            rows[p["id"]] = items[offset:offset+1+count]
            offset += 1+count
        if offset != len(items):
            rows["__tail__"] = items[offset:]
        return rows
    a, b = unpack(expected), unpack(actual)
    problems = []
    for key in a.keys() | b.keys():
        if a.get(key) != b.get(key):
            problems.append({
                "id": key,
                "expected": a.get(key),
                "actual": b.get(key),
            })
    return sorted(problems, key=lambda x: x["id"])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--xyo", type=Path, default=ROOT / "build" / "xyo")
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "parity-gauntlet")
    ap.add_argument("--modes", default="off,eager,hotspot")
    ap.add_argument("--seed", type=int, default=1618)
    ap.add_argument("--max-probes", type=int, default=90)
    ap.add_argument("--max-shards", type=int, default=0)
    ap.add_argument("--generate-only", action="store_true")
    ap.add_argument("--json", type=Path, default=None)
    ap.add_argument("--timeout", type=int, default=90)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    probes = make_probes(args.seed)
    shards = shard_probes(probes, args.max_probes)
    if args.max_shards:
        shards = shards[:args.max_shards]

    # No implicit scratch-vm compiler: the oracle is the interpreter.
    print(f"{len(probes)} probes, {len(shards)} selected shards, seed={args.seed}", flush=True)
    records = []
    failures = 0
    for index, (name, group) in enumerate(shards):
        path = out / (name + ".sb3")
        # The yield-specific script tests non-warp scheduling.
        warp = not (name.startswith("type-transition-yield") or
                    name.startswith("type-cfg-yield"))
        manifest = build_shard(path, group, warp)
        (out / (name + ".json")).write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        if args.generate_only:
            continue
        ref = run(["node", str(ROOT / "tests/differential/scratch_vm_dump.js"),
                   str(path), "15000"], dict(os.environ), args.timeout)
        expected = check_done(ref, "scratch-vm", name)
        record = {"shard": name, "probes": len(group), "modes": {}}
        for mode in args.modes.split(","):
            env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
            if mode in ("turbowarp", "compiled"):
                command = ["node", str(ROOT / "tests/differential/scratch_vm_dump.js"),
                           str(path), "15000", "compiled"]
            else:
                env.update(XYO_DUMPVARS="json", XYO_PAR="0", XYO_GPU="0",
                           XYO_PARFOR="0")
                command = [str(args.xyo.resolve()), "run", str(path), "--jit",
                           mode, "--frames", "3000"]
            result = run(command, env, args.timeout)
            actual = check_done(result, mode, name)
            problems = compare(expected, actual, manifest)
            failures += len(problems)
            record["modes"][mode] = {"mismatches": problems}
            print(f" [{index+1:02d}/{len(shards):02d}] {name:29s} "
                  f"{mode:7s} {'PASS' if not problems else 'DIFF '+str(len(problems))}",
                  flush=True)
            for item in problems[:4]:
                print("  ", json.dumps(item, ensure_ascii=False)[:500], flush=True)
        records.append(record)
    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({
            "seed": args.seed, "probes": len(probes),
            "shards": records, "mismatches": failures,
        }, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    if args.generate_only:
        print(f"Generated {len(shards)} SB3 shards in {out}")
        return 0
    print(f"TOTAL {sum(r['probes'] for r in records)} probes, "
          f"{failures} divergences", flush=True)
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
