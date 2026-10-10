import ssl, http.client
D = r"C:/Users/Aiden/AppData/Local/Temp/lscert"
ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
ctx.load_cert_chain(rf"{D}/cert.pem", rf"{D}/key.pem")
import sys as _sys

# Addresses to check: whatever is passed, or the phone's last known one. This file used to have a
# single address baked in and ignore its arguments, so a check of a new address reported on the
# old one -- which sent me looking for the wrong problem for several rounds.
for ip in (_sys.argv[1:] or ["192.168.1.243"]):
    try:
        c = http.client.HTTPSConnection(ip, 53317, timeout=6, context=ctx)
        c.request("GET", "/api/localsend/v2/info")
        r = c.getresponse()
        print(f"{ip} -> HTTP {r.status} {r.read()[:300].decode(errors='replace')}")
    except Exception as e:
        print(f"{ip} -> {type(e).__name__}: {e}")
