#!/usr/bin/env python3
"""Run xyo Dragonbox formatter and compare every output against Node/V8."""
from __future__ import annotations

import subprocess
import sys
import tempfile


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: num_to_str_dragonbox_node.py binary node script.mjs")
    binary, node, checker = sys.argv[1:]
    with tempfile.TemporaryFile() as output:
        subprocess.run([binary], stdout=output, check=True, timeout=90)
        output.seek(0)
        subprocess.run([node, checker, "-"], stdin=output, check=True, timeout=90)


if __name__ == "__main__":
    main()
