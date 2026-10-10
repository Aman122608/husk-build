#!/usr/bin/env python3
"""Send a file to a LocalSend receiver (protocol v2.2) using only the standard library.

LocalSend serves HTTPS with a self-signed certificate, so verification is off and the
fingerprint is not checked -- this talks to whatever answers on the LAN, which is the
point for a one-off push to a phone on the same Wi-Fi.

Usage:
    ls_send.py discover
    ls_send.py send <ip> <file>
"""
import concurrent.futures
import http.client
import json
import os
import socket
import ssl
import sys
import uuid

PORT = 53317
ALIAS = "husk-build (windows)"
FINGERPRINT = "huskbuild" + uuid.getnode().to_bytes(6, "big").hex()
# The client certificate LocalSend demands. It used to live only in %TEMP%, which anything can clean --
# losing it means the handshake dies with TLSV13_ALERT_CERTIFICATE_REQUIRED and no build can be sent at
# all. It now lives beside the repo (gitignored: a private key does not belong in a public repo), with
# the old temp location kept as a fallback.
CERT_DIR = r"C:/Users/Aiden/Desktop/HuskApp/.local/lscert"
if not os.path.isfile(CERT_DIR + "/cert.pem"):
    CERT_DIR = r"C:/Users/Aiden/AppData/Local/Temp/lscert"

# LocalSend's server demands a client certificate: without one the handshake ends in
# TLSV13_ALERT_CERTIFICATE_REQUIRED before any HTTP is spoken. Any self-signed cert will do,
# which is what lsq's ~/.config/lsq certificate is for as well.
CTX = ssl.create_default_context()
CTX.check_hostname = False
CTX.verify_mode = ssl.CERT_NONE
CTX.load_cert_chain(f"{CERT_DIR}/cert.pem", f"{CERT_DIR}/key.pem")


def conn(ip, timeout):
    return http.client.HTTPSConnection(ip, PORT, timeout=timeout, context=CTX)


def probe(ip, timeout=1.2):
    """A LocalSend device answers /api/localsend/v2/info with its own description."""
    try:
        c = conn(ip, timeout)
        c.request("GET", "/api/localsend/v2/info")
        r = c.getresponse()
        if r.status == 200:
            return ip, json.loads(r.read().decode())
    except Exception:
        return None
    return None


def local_ipv4s():
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except Exception:
        pass
    # The address the OS would use to leave the machine is the one the LAN sees.
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ips.add(s.getsockname()[0])
        s.close()
    except Exception:
        pass
    return [i for i in ips if not i.startswith("127.")]


def discover():
    found = []
    for ip in local_ipv4s():
        print(f"this machine: {ip}")
        base = ip.rsplit(".", 1)[0]
        targets = [f"{base}.{n}" for n in range(1, 255)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=96) as ex:
            for res in ex.map(probe, targets):
                if res:
                    found.append(res)
                    ip_, d = res
                    print(f"  found {ip_}  alias={d.get('alias')!r} device={d.get('deviceModel')!r} "
                          f"type={d.get('deviceType')} proto={d.get('protocol')} v={d.get('version')}")
    if not found:
        print("no LocalSend device answered on this subnet")
    return found


def send(ip, path):
    path = os.path.abspath(path)
    size = os.path.getsize(path)
    name = os.path.basename(path)
    info = {
        "alias": ALIAS, "version": "2.2", "deviceModel": "Windows", "deviceType": "desktop",
        "fingerprint": FINGERPRINT, "port": PORT, "protocol": "https", "download": False,
    }
    # Announce, best effort: some receivers like knowing who is calling, none require it.
    try:
        c = conn(ip, 5)
        c.request("POST", "/api/localsend/v2/register",
                  body=json.dumps(info).encode(), headers={"Content-Type": "application/json"})
        c.getresponse().read()
    except Exception:
        pass

    fid = "huskfile"
    files = {fid: {"id": fid, "fileName": name, "size": size, "fileType": "application/octet-stream"}}
    c = conn(ip, 30)
    c.request("POST", "/api/localsend/v2/prepare-upload",
              body=json.dumps({"info": info, "files": files}).encode(),
              headers={"Content-Type": "application/json"})
    r = c.getresponse()
    raw = r.read().decode()
    if r.status != 200:
        raise SystemExit(f"prepare-upload failed: HTTP {r.status} {raw[:300]}")
    d = json.loads(raw)
    session = d.get("sessionId")
    token = (d.get("files") or {}).get(fid)
    if not session or not token:
        raise SystemExit(f"prepare-upload gave no session/token: {raw[:300]}")
    print(f"session {session}; the phone should be asking to accept {name} ({size:,} bytes)")

    with open(path, "rb") as f:
        payload = f.read()
    c = conn(ip, 900)
    q = f"/api/localsend/v2/upload?sessionId={session}&fileId={fid}&token={token}"
    c.request("POST", q, body=payload,
              headers={"Content-Type": "application/octet-stream", "Content-Length": str(len(payload))})
    r = c.getresponse()
    body = r.read().decode(errors="replace")
    print(f"upload -> HTTP {r.status} {body[:200]}")
    if r.status == 200:
        print("SENT OK")
    else:
        raise SystemExit("upload refused")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    if sys.argv[1] == "discover":
        discover()
    elif sys.argv[1] == "send":
        send(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)
