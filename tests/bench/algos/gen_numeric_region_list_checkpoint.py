#!/usr/bin/env python3
"""Generate a one-thread readonly-list native-checkpoint fixture (no runtime).

Usage: python3 tests/bench/algos/gen_numeric_region_list_checkpoint.py --out NEWDIR
The destination must not exist. The matching C++ smoke test mutates only the
existing data SList at actual Fiber handoffs; this fixture never mutates it.
"""
import argparse
from pathlib import Path

from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set, add, item, make_project, mod,
    num, var, write_sb3,
)


def project():
    g = Gen(["i", "acc"], ["data"])
    g.define("kernel", [], [S_repeat(num(1025), [
        S_change("i", num(1)),
        S_change("acc", item("data", add(mod(var("i"), num(64)), num(1)))),
    ])], warp=True, x=340, y=30)
    g.script([
        # Bare numeric shadows are String-tagged. Arithmetic initialization
        # deliberately establishes semantic Number zero for both scalar guards.
        S_set("i", add(num(0), num(0))),
        S_set("acc", add(num(0), num(0))),
        S_call("kernel"),
    ], x=20, y=30)
    return make_project(g.vars, g.lists, g, {"data": list(range(1, 65))})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path,
                        help="new, exclusive output directory")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    fixture = out / "numeric_region_list_checkpoint.sb3"
    write_sb3(fixture, project())
    print(fixture)


if __name__ == "__main__":
    main()
