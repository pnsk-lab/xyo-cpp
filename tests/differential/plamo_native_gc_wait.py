#!/usr/bin/env python3
"""Force explicit GC while real native translation Fiber is suspended."""
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from plamo_native_fiber import expression_fixture

class Handler(BaseHTTPRequestHandler):
    count = 0
    def do_POST(self):
        size = int(self.headers["Content-Length"])
        self.rfile.read(size)
        Handler.count += 1
        time.sleep(2.0)
        value = json.dumps({"choices":[{"message":{"content":"late"}}]}).encode()
        try:
            self.send_response(200)
            self.send_header("Content-Length", str(len(value)))
            self.end_headers()
            self.wfile.write(value)
        except (BrokenPipeError,ConnectionResetError):
            pass
    def log_message(self,*_): pass

def main():
    binary=Path(sys.argv[1]).resolve()
    fixture=Path(sys.argv[2]).resolve()/"plamo-gc-wait.sb3"
    fixture.parent.mkdir(parents=True,exist_ok=True)
    expression_fixture(fixture)
    server=ThreadingHTTPServer(("127.0.0.1",0),Handler)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    try:
        env=dict(os.environ, XYO_PLAMO_PORT=str(server.server_port))
        proc=subprocess.run([str(binary),str(fixture)],env=env,cwd=ROOT,
                            capture_output=True,text=True,timeout=15)
        assert proc.returncode==0,(proc.stdout,proc.stderr[-1500:])
        assert "PASS 128 forced collections deferred" in proc.stdout,proc.stdout
        assert "PASS deferred GC drained only after native Fiber retirement" in proc.stdout,proc.stdout
        assert Handler.count==1,Handler.count
        print(proc.stdout,end="")
        print("PASS exactly one HTTP request; 128 explicit GC attempts")
    finally:
        server.shutdown()
        server.server_close()

if __name__=="__main__":
    main()
