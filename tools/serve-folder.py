#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Serve a folder to Husk's Downloads tab over the local network.

    python3 tools/serve-folder.py <folder> [port] [bind-address]

Husk is given the address this prints. GET / is the manifest, a JSON list of every file under the folder with its size;
GET /f/<path> is one file, with Range requests honoured so an interrupted download resumes where it stopped. Husk puts the
files in its Shared Storage folder at the same paths, so serve the folder whose contents belong at the top of /sdcard.

The address this serves on is the address it prints, by default: see serve_address() for why that matters on a
machine with more than one interface. Pass a bind address to override it.
"""
import json
import os
import socket
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.realpath(sys.argv[1] if len(sys.argv) > 1 else ".")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8642
BIND = sys.argv[3] if len(sys.argv) > 3 else ""


def manifest():
    files = []
    for base, dirs, names in os.walk(ROOT):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for n in sorted(names):
            if n.startswith("."):
                continue
            full = os.path.join(base, n)
            files.append({"path": os.path.relpath(full, ROOT).replace(os.sep, "/"), "size": os.path.getsize(full)})
    return {"name": os.path.basename(ROOT), "files": files}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def do_HEAD(self):
        self.do_GET(head=True)

    def do_GET(self, head=False):
        path = urllib.parse.unquote(urllib.parse.urlparse(self.path).path)
        if path in ("/", "/manifest.json"):
            body = json.dumps(manifest()).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if not head:
                self.wfile.write(body)
            return
        # Short, meaningful links. The slug comes from the file name, so /g/gta.apk is guessable, the
        # map is whatever is in the folder right now (nothing is hardcoded), and a slug never shifts
        # when files are added or removed the way a number does. Numbers still work for quick typing,
        # and /list prints the whole map as plain text -- open it in a browser, never in Husk (a JSON
        # listing there would be taken as a folder).
        if path.startswith("/g/") or path == "/list":
            names = sorted(f["path"] for f in manifest()["files"])

            def slug_of(name):
                base = os.path.basename(name).rsplit(".", 1)[0].lower()
                parts = [p for p in "".join(c if c.isalnum() else "-" for c in base).split("-") if p]
                if parts and parts[-1] in ("com", "net", "org", "apk"):
                    parts = parts[:-1]
                keep = []
                for p in parts:                       # drop the version tail: 1.87.0-an1.com, .97.11, -0-2-42
                    if p.isdigit():
                        break
                    keep.append(p)
                return "-".join(keep[:6]) or "-".join(parts[:6])

            host = lan_address()
            if path == "/list":
                lines = ["%d. %s" % (i, n) for i, n in enumerate(names, 1)]
                lines += ["", "short links (type any of these into Husk's Downloads):", ""]
                lines += ["  http://%s:%d/g/%s.apk" % (host, self.server.server_address[1], slug_of(n)) for n in names]
                body = ("\n".join(lines) + "\n").encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if not head:
                    self.wfile.write(body)
                return

            tag = path[3:]
            hit = None
            want = tag[:-4] if tag.endswith(".apk") else tag
            if want.isdigit():
                n = int(want)
                if 1 <= n <= len(names):
                    hit = names[n - 1]
            elif want:
                for cand in names:                       # exact slug, else the first that contains it
                    if want == slug_of(cand):
                        hit = cand
                        break
                if hit is None:
                    for cand in names:
                        if want in slug_of(cand):
                            hit = cand
                            break
            if hit:
                path = "/f/" + hit
            else:
                self.send_error(404)
                return
        if not path.startswith("/f/"):
            self.send_error(404)
            return
        full = os.path.realpath(os.path.join(ROOT, path[3:]))
        if not full.startswith(ROOT + os.sep) or not os.path.isfile(full):
            self.send_error(404)
            return
        size = os.path.getsize(full)
        start, end = 0, size - 1
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            a, _, b = rng[6:].split(",")[0].partition("-")
            if a:
                start = int(a)
                end = int(b) if b else size - 1
            elif b:
                start = max(0, size - int(b))
            if start >= size or start > end:
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % size)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            end = min(end, size - 1)
            self.send_response(206)
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        else:
            self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        if head:
            return
        self.wfile.flush()
        t0, sent = time.time(), 0
        with open(full, "rb") as f:
            # socket.sendfile() copies from the file to the socket, using the kernel's sendfile where
            # the platform has one. os.sendfile() is Unix-only: on Windows it raises AttributeError and
            # every download died here, which the client reports as "network connection lost".
            sock = self.connection
            offset, left = start, end - start + 1
            try:
                while left > 0:
                    n = sock.sendfile(f, offset, min(left, 64 << 20))
                    if not n:
                        break
                    offset += n
                    left -= n
                    sent += n
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
        dt = max(time.time() - t0, 0.001)
        sys.stderr.write("  %s: %.0f MB in %.0f s, %.1f MB/s\n" % (path[3:], sent / 1e6, dt, sent / 1e6 / dt))


def lan_address():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))   # no packet is sent; this only picks the interface a LAN route would use
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def serve_address():
    """The address to serve on -- the one this script prints, on purpose.

    Binding 0.0.0.0 answers on every interface, which on a machine with more than
    one (Ethernet plus Wi-Fi, a docked laptop, a VPN adapter) also exposes it on
    the slow one. A client that happens to find that address gets the slow link,
    and Husk's sweep takes the first address that answers, so which one it finds
    is a coin flip: on one machine a 1 Gbps Ethernet and a 58 Mbps Wi-Fi sat on
    the same subnet and measured 17 MB/s against 2.8 MB/s for the same file.
    Serving only on the advertised address is what stops the two disagreeing.
    Pass a bind address on the command line to override it.
    """
    return BIND or lan_address()


if __name__ == "__main__":
    m = manifest()
    total = sum(f["size"] for f in m["files"])
    print("Serving %s: %d files, %.1f GB" % (ROOT, len(m["files"]), total / 1e9))

    # Announce ourselves on the LAN so the app can fill the address in instead of the user typing it.
    # A plain UDP broadcast on a fixed port (8766), not mDNS: nothing for the phone to resolve and no
    # Bonjour service to register. The app listens on 8766, and the address the PC is *actually* using
    # travels in the packet -- which is what makes a DHCP re-lease a non-event.
    def announce_forever():
        import threading as _t   # noqa: F401  (kept local so the module needs nothing new at import time)
        beacon = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        beacon.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        while True:
            try:
                host = serve_address()
                msg = json.dumps({"husk": "serve-folder", "v": 1, "name": os.path.basename(ROOT),
                                  "host": host, "port": int(PORT), "files": len(manifest()["files"]),
                                  "url": "http://%s:%d/" % (host, int(PORT))}).encode()
                for dest in ("255.255.255.255", "224.0.0.167"):
                    try:
                        beacon.sendto(msg, (dest, 8766))
                    except OSError:
                        pass
            except Exception:
                pass
            time.sleep(2)

    import threading
    threading.Thread(target=announce_forever, daemon=True).start()
    host = serve_address()
    print("In Husk, Downloads > Add, enter:  http://%s:%d/" % (host, PORT))
    class Server(ThreadingHTTPServer):
        daemon_threads = True
        def server_bind(self):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 << 20)
            super().server_bind()
    try:
        server = Server((host, PORT), Handler)
    except OSError as exc:
        # The address can move between the check above and the bind (a DHCP
        # re-lease, a Wi-Fi drop). 0.0.0.0 keeps the folder reachable, at the
        # cost of answering on interfaces we did not advertise -- say so rather
        # than failing, because a server nobody can reach is worse.
        if BIND:
            raise
        print("Could not bind %s: %s" % (host, exc))
        print("Serving on 0.0.0.0 instead: every interface, including any slow one.")
        server = Server(("0.0.0.0", PORT), Handler)
    server.serve_forever()
