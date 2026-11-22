#!/usr/bin/env python3
"""Compare the Mega preset against the interpreter on real projects.

Real projects often depend on wall-clock time (`timer`, `days since 2000`)
or on unseeded randomness, so their variable dump is not a stable oracle.
This harness therefore checks determinism first:

    * run the interpreter twice with ``XYO_SEED=1234`` and identical frames;
    * if the two dumps differ, report ``SKIP (nondeterministic)``;
    * otherwise compare the Mega preset with the interpreter and fail on any
      difference.

    python3 tests/mega/run_real_projects.py --binary build/xyo [--frames 30]

Informational by default: exit status 1 is only returned with --strict.
Known finding (2026-11-19): the JIT resumes yielded Fibers within the
same frame, so loops advance more iterations per frame than under the
interpreter. This is present with `XYO_JIT_MODE=eager` (no Mega flags),
so real projects that append per iteration are not a valid Mega
regression; only their final state after a fixed number of frames differs.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

DEFAULT_PROJECTS = [
    "debug/projects/3DOcean.sb3",
    "debug/projects/voxelRenderer_fast.sb3",
    "debug/projects/linux.sb3",
    "debug/projects/3D2.sb3",
    "debug/projects/tetrisai.sb3",
]


def run(binary, project, frames, env):
    e = dict(os.environ)
    e.update(env)
    try:
        p = subprocess.run(
            [binary, "run", project, "--frames", str(frames)],
            capture_output=True, timeout=600, env=e)
        return p.returncode, p.stdout
    except subprocess.TimeoutExpired:
        return 124, b""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(ROOT, "build", "xyo"))
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--projects", nargs="*", default=DEFAULT_PROJECTS)
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        print("missing binary: %s" % args.binary)
        return 2

    failures = 0
    for rel in args.projects:
        project = os.path.join(ROOT, rel) if not os.path.isabs(rel) else rel
        if not os.path.exists(project):
            print("SKIP %s (missing)" % rel)
            continue
        base = {"XYO_DUMPVARS": "1", "XYO_SEED": "1234"}
        c1, o1 = run(args.binary, project, args.frames,
                     {**base, "XYO_JIT_MODE": "off"})
        c2, o2 = run(args.binary, project, args.frames,
                     {**base, "XYO_JIT_MODE": "off"})
        if (c1, o1) != (c2, o2):
            print("SKIP %s (nondeterministic: interpreter differs across "
                  "two runs)" % rel)
            continue
        cm, om = run(args.binary, project, args.frames,
                     {**base, "XYO_JIT_MEGA": "1"})
        same = (c1, o1) == (cm, om)
        print("%s %s" % ("SAME" if same else "DIFF", rel))
        if not same:
            failures += 1
            a = o1.decode("utf-8", "replace").splitlines()
            b = om.decode("utf-8", "replace").splitlines()
            for i, (x, y) in enumerate(zip(a, b)):
                if x != y:
                    print("  first difference at line %d:\n  interp: %s\n"
                          "  mega  : %s" % (i + 1, x[:160], y[:160]))
                    break
            else:
                print("  exit codes differ: interp=%d mega=%d" % (c1, cm))
    print("%d project(s) differed" % failures)
    return 1 if failures and args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
