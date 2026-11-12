#!/usr/bin/env python3
"""Integration of multiple suspended LLVM Fibers, GC, and repeated green flags."""
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from plamo_clone_stress import fixture

class Handler(BaseHTTPRequestHandler):
    count = 0
    lock = threading.Lock()
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(length)
        with Handler.lock:
            Handler.count += 1
        # The harness stops all Fibers while they are awaiting this response.
        time.sleep(0.6)
        try:
            response = b'{"choices":[{"message":{"content":"late"}}]}'
            self.send_response(200)
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)
        except (BrokenPipeError, ConnectionResetError):
            pass
    def log_message(self, *_):
        pass

def main():
    binary = Path(sys.argv[1]).resolve()
    project = Path(sys.argv[2]).resolve() / "plamo-native-multi-gc.sb3"
    project.parent.mkdir(parents=True, exist_ok=True)
    fixture(project)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        env = dict(os.environ, XYO_PLAMO_PORT=str(server.server_port))
        proc = subprocess.run([str(binary), str(project)],
                              env=env, cwd=ROOT, capture_output=True,
                              text=True, timeout=40)
        assert proc.returncode == 0, (proc.returncode, proc.stdout, proc.stderr[-2500:])
        cycles = int(env.get("XYO_PLAMO_GC_CYCLES", "128"))
        assert f"PASS {cycles} native multi-Fiber restarts" in proc.stdout, proc.stdout
        print(proc.stdout, end="")
        print(f"PASS mock HTTP requests accepted={Handler.count}")
    finally:
        server.shutdown()
        server.server_close()

if __name__ == "__main__":
    main()
