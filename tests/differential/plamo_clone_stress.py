#!/usr/bin/env python3
"""Real SB3 clone concurrency regression for local PLaMo."""
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (Gen, S_change, S_if, S_set, eq, num, op,
                            text, var, make_project, write_sb3)

CLONES = 32

def fixture(path):
    g = Gen(["created", "completed", "translated", "fallback", "done", "result"], [])
    hat = g.script([S_set("created", num(0)), S_set("completed", num(0)),
                    S_set("translated", num(0)), S_set("fallback", num(0)),
                    S_set("done", num(0))])
    cursor = hat
    while g.blocks[cursor]["next"]:
        cursor = g.blocks[cursor]["next"]
    for _ in range(CLONES):
        clone = g.new("control_create_clone_of", parent=cursor)
        menu = g.new("control_create_clone_of_menu", shadow=True, parent=clone,
                     fields={"CLONE_OPTION": ["_myself_", None]})
        g.blocks[clone]["inputs"]["CLONE_OPTION"] = [1, menu]
        g.blocks[cursor]["next"] = clone
        cursor = clone
    final = g.emit_stmt(S_set("done", num(1)), cursor)
    g.blocks[cursor]["next"] = final

    started = g.new("control_start_as_clone", top=True, x=550)
    first = g.chain([
        S_change("created", num(1)),
        S_set("result", op("translate_getTranslate",
            WORDS=text("clone hello"), LANGUAGE=text("ja"))),
        S_if(eq(var("result"), text("完了")),
             [S_change("translated", num(1))]),
        S_if(eq(var("result"), text("clone hello")),
             [S_change("fallback", num(1))]),
        S_change("completed", num(1)),
    ], started)
    g.blocks[started]["next"] = first
    cursor = first
    while g.blocks[cursor]["next"]:
        cursor = g.blocks[cursor]["next"]
    deleted = g.new("control_delete_this_clone", parent=cursor)
    g.blocks[cursor]["next"] = deleted

    project = make_project(g.vars.keys(), [], g)
    stage, sprite = project["targets"]
    for key in ("created", "completed", "translated", "fallback", "done"):
        stage["variables"][g.vars[key]] = [key, 0]
        del sprite["variables"][g.vars[key]]
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), project)


class Handler(BaseHTTPRequestHandler):
    count = 0
    active = 0
    peak = 0
    lock = threading.Lock()

    def do_POST(self):
        with Handler.lock:
            Handler.count += 1
            Handler.active += 1
            Handler.peak = max(Handler.peak, Handler.active)
        try:
            size = int(self.headers.get("Content-Length", "0"))
            data = json.loads(self.rfile.read(size))
            assert data["messages"][1]["content"] == "clone hello"
            time.sleep(0.2)
            body = json.dumps({"choices": [
                {"message": {"content": "完了"}}]}).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with Handler.lock:
                Handler.active -= 1

    def log_message(self, *_):
        pass


def stage_number(state, key):
    return int(state[key])


def main():
    binary = Path(sys.argv[1]).resolve()
    project = Path(sys.argv[2]).resolve() / "plamo-clones.sb3"
    fixture(project)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for mode in ("off", "eager", "hotspot"):
            Handler.count = Handler.active = Handler.peak = 0
            env = dict(os.environ, XYO_PLAMO_TRANSLATE="1",
                       XYO_PLAMO_PORT=str(server.server_port),
                       XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0")
            result = subprocess.run([
                str(binary), "run", str(project), "--jit", mode,
                "--frames", "500", "--fps", "60",
            ], env=env, cwd=ROOT, capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, (mode, result.stderr[-1200:])
            parsed = json.loads(result.stdout[result.stdout.index("{"):
                                              result.stdout.rindex("}") + 1])
            state = parsed["Stage"]
            print(f"mode={mode} stage={state} requests={Handler.count} "
                  f"peak={Handler.peak}", flush=True)
            assert state["done"] == "1"
            assert state["created"] == str(CLONES), state
            assert state["completed"] == str(CLONES), state
            assert int(state["translated"]) + int(state["fallback"]) == CLONES
            # First four clones own the four inference slots. The other
            # 28 receive their original text, never a different result.
            assert stage_number(state, "translated") == 4, (mode, state)
            assert stage_number(state, "fallback") == CLONES - 4, (mode, state)
            assert Handler.count == 4, (mode, Handler.count)
            assert Handler.peak == 4, (mode, Handler.peak)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
