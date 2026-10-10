#!/usr/bin/env python3
"""
Where Husk's "Send log" button lands.

The phone POSTs the log to this listener over the LAN; nothing else talks to it, and it writes
what it receives to a folder on this machine. Deliberately small: it is a convenience for handing
a log across, not a service.

    python husk_log_receiver.py                 # listens on 0.0.0.0:8765, writes to Desktop/HuskLogs
    python husk_log_receiver.py --port 9000 --dir C:/somewhere
    HUSK_LOG_TOKEN=secret python husk_log_receiver.py    # then POST to /log/secret

Privacy: no telemetry, no outbound requests, no third party. The address is whatever the phone is
configured with, which is this machine. Content is the log file Husk already writes.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import os
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

TOKEN = os.environ.get("HUSK_LOG_TOKEN", "").strip()


def lan_address() -> str:
    """The address the phone should be given: this machine's LAN IP, found without sending traffic."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.1.1", 9))          # no packets leave; it just picks the default route
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


class Handler(BaseHTTPRequestHandler):
    server_version = "husk-log-receiver/1"
    out_dir: Path

    def _plain(self, code: int, text: str) -> None:
        body = text.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:                  # a phone-side check that the address is right
        if self.path in ("/", "/log") or self.path.startswith("/log/"):
            self._plain(200, "husk log receiver ready\n")
        else:
            self._plain(404, "not found\n")

    def do_POST(self) -> None:
        path = self.path.split("?", 1)[0]
        if TOKEN and path != f"/log/{TOKEN}":
            self._plain(403, "wrong or missing token\n")
            return
        if not (path == "/log" or path.startswith("/log/")):
            self._plain(404, "not found\n")
            return

        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = 0
        if n <= 0 or n > 40 << 20:             # a log, not an upload endpoint
            self._plain(400, "expected a log body up to 40 MiB\n")
            return
        data = self.rfile.read(n)

        stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        name = self.headers.get("X-Husk-Build", "").strip().replace("/", "-")[:40]
        out = self.out_dir / f"husk-{stamp}{'-' + name if name else ''}.log"
        out.write_bytes(data)
        lines = data.count(b"\n") + 1
        print(f"[{stamp}] received {n} bytes ({lines} lines) -> {out}", flush=True)
        self._plain(200, f"saved {out.name}\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--dir", default=str(Path.home() / "Desktop" / "HuskLogs"))
    ap.add_argument("--host", default="0.0.0.0")
    a = ap.parse_args()

    out_dir = Path(a.dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    Handler.out_dir = out_dir

    print(f"writing logs to {out_dir}", flush=True)
    print(f"put this in Husk's settings:  http://{lan_address()}:{a.port}" + (f"/log/{TOKEN}" if TOKEN else "/log"), flush=True)
    if TOKEN:
        print("a token is required (HUSK_LOG_TOKEN is set)", flush=True)
    else:
        print("no token required; anyone on this LAN who knows the port can post to it", flush=True)
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
