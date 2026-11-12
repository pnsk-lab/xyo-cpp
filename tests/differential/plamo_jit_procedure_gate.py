#!/usr/bin/env python3
"""JIT must refuse reachable async reporters inside nested procedures."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (Gen, S_set, S_call, S_change, S_repeat,
                            num, text, op, make_project, write_sb3)

def fixture(path):
    g = Gen(["translation", "done", "plain", "plainDone"], [])
    g.define("inner", [], [
        S_set("translation", op("translate_getTranslate",
                              WORDS=text("nested procedure"),
                              LANGUAGE=text("ja"))),
    ], warp=False)
    g.define("outer", [], [S_call("inner")], warp=False)
    g.script([S_call("outer"), S_set("done", num(1))], x=0)
    g.script([S_repeat(num(1000), [S_change("plain", num(1))]),
              S_set("plainDone", num(1))], x=600)
    project = make_project(g.vars.keys(), [], g)
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), project)


def main():
    binary = Path(sys.argv[1]).resolve()
    project = Path(sys.argv[2]).resolve() / "nested-async-translate.sb3"
    fixture(project)
    for jit in ("off", "eager", "hotspot"):
        env = dict(os.environ, XYO_DUMPVARS="json", XYO_GPU="0",
                   XYO_PAR="0", XYO_JIT_VERBOSE="1", XYO_JIT_ASYNC="0",
                   XYO_PLAMO_TRANSLATE="0")
        cmd = [str(binary), "run", str(project), "--jit", jit,
               "--frames", "300"]
        result = subprocess.run(cmd, cwd=ROOT, env=env,
                                capture_output=True, text=True, timeout=45)
        assert result.returncode == 0, (jit, result.stderr[-2000:])
        state = json.loads(result.stdout[result.stdout.index("{"):
                                             result.stdout.rindex("}")+1])["Bench"]
        assert state["translation"] == "nested procedure", (jit, state)
        assert state["done"] == "1", (jit, state)
        assert state["plain"] == "1000", (jit, state)
        assert state["plainDone"] == "1", (jit, state)
        if jit == "eager":
            assert "async translate or invalid procedure graph -> interpreter fallback" in result.stderr, result.stderr[-2000:]
            assert "xyo-jit: compiled " in result.stderr, (
                "independent numeric script must remain native-JIT eligible",
                result.stderr[-2000:])
        print(f"PASS {jit} nested async-procedure fallback; independent script runs",flush=True)
        if jit == "eager":
            # GC protection is mandatory, even if the user explicitly
            # enables the experimental native async reporter.
            unsafe_env = dict(env, XYO_JIT_PLAMO_FIBER="1",
                              XYO_RT_REQUIRE_GENERATED_SSA_ROOTS="0")
            unsafe = subprocess.run(cmd, cwd=ROOT, env=unsafe_env,
                                    capture_output=True, text=True, timeout=45)
            assert unsafe.returncode == 0, unsafe.stderr[-1500:]
            unsafe_state = json.loads(unsafe.stdout[
                unsafe.stdout.index("{"):unsafe.stdout.rindex("}")+1])["Bench"]
            assert unsafe_state == state, (unsafe_state, state)
            assert ("async translate or invalid procedure graph -> "
                    "interpreter fallback") in unsafe.stderr
            print("PASS native async flag without GC veto safely falls back",
                  flush=True)


if __name__ == "__main__":
    main()
