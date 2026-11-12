#!/usr/bin/env python3
"""Cancellation of an opt-in native async reporter with live variable roots."""
import json
import os
import subprocess
import sys
import threading
import time
from http.server import ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from plamo_native_stop_all import Handler
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_set, join, var, num, op, text, make_project, write_sb3
)


def fixture(path):
    gen = Gen(["source", "translated", "after", "stopped"], [])
    # A boxed source string must remain live AFTER the suspended reporter.
    gen.script([
        S_set("source", join(text("before"), text("cancel"))),
        S_set("translated", join(
            var("source"), op("translate_getTranslate",
                              WORDS=text("never complete"),
                              LANGUAGE=text("ja")))),
        S_set("after", num(1)),
    ], x=0)
    hat = gen.script([], x=550)
    delay = gen.new("control_wait", parent=hat)
    gen.blocks[delay]["inputs"]["DURATION"] = gen.emit_value(num(0.04), delay)
    gen.blocks[hat]["next"] = delay
    stopped = gen.emit_stmt(S_set("stopped", num(1)), delay)
    gen.blocks[delay]["next"] = stopped
    stop = gen.new("control_stop", parent=stopped,
                   fields={"STOP_OPTION": ["all", None]})
    gen.blocks[stopped]["next"] = stop
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(gen.vars.keys(), [], gen))


def main():
    binary = Path(sys.argv[1]).resolve()
    project = Path(sys.argv[2]).resolve() / "native-root-cancel.sb3"
    fixture(project)
    Handler.requests = 0
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        env = dict(os.environ, XYO_JIT_PLAMO_FIBER="1",
                   XYO_RT_REQUIRE_GENERATED_SSA_ROOTS="1",
                   XYO_JIT_PLAMO_ROOT_LIVE_VARS="1",
                   XYO_RT_TRACE_ASYNC_VAR_ROOTS="1",
                   XYO_PLAMO_TRANSLATE="1",
                   XYO_PLAMO_PORT=str(server.server_port),
                   XYO_JIT_VERBOSE="1",
                   XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0")
        begin = time.monotonic()
        proc = subprocess.run(
            [str(binary), "run", str(project),
             "--jit", "eager", "--frames", "100", "--fps", "60"],
            cwd=ROOT, env=env, capture_output=True, text=True, timeout=8)
        elapsed = time.monotonic() - begin
        assert proc.returncode == 0, proc.stderr[-2400:]
        state = json.loads(
            proc.stdout[proc.stdout.index("{"):proc.stdout.rindex("}")+1]
        )["Bench"]
        assert state["translated"] == "0" and state["after"] == "0"
        assert state["stopped"] == "1", state
        assert elapsed < 1.5, elapsed
        assert Handler.requests == 1, Handler.requests
        assert "[plamo-async-var-root-codegen]" in proc.stderr
        assert "count=1 rooted=1 immediate=0 valid=1 gc_safe=0" in proc.stderr
        assert "[native-async-var-wait] owners=1 heap_roots=1 gc_safe=0" in proc.stderr
        assert "[native-async-var-retire] owners=1 heap_roots=1 gc_safe=0" in proc.stderr, (
            [line for line in proc.stderr.splitlines()
             if "native-async-var-" in line])
        print("PASS RT-001 async variable root cancellation: "
              f"request={Handler.requests} elapsed={elapsed:.3f}s, "
              "protected wait and no post-cancel continuation")
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
