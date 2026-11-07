#!/usr/bin/env python3
r"""Copy a file from the PC to the CM0 through MAIN's Linux menu (base64 over
the l\ shell session, see cm0sh.py) and check its SHA-256 on arrival.

    python host/cm0put.py COM14 cm0/linktest.py /tmp/linktest.py

About 1 KB/s. Needs the stock gateware and fwcm0-bridge, like cm0sh.py.
"""
import base64, hashlib, pathlib, re, sys, time, serial
TOKEN = "C0FFEE02"
port, local, remote = sys.argv[1:4]
data = pathlib.Path(local).read_bytes()
s = serial.Serial(port, 115200, timeout=0.2)
def rd(t):
    end = time.time() + t; out = b""
    while time.time() < end:
        c = s.read(4096)
        if c:
            out += c
            if out.rstrip().endswith(b"]"): break
    return out.decode(errors="replace")
def menu(line, t=2.0):
    s.write(line.encode() + b"\r"); return rd(t)
def drain(idle):
    out, last = b"", time.time()
    while time.time() - last < idle:
        m = re.search(r"\[l\\r \S+ \d+ (\d+) ?([0-9A-Fa-f]*)", menu(f"l\\r {TOKEN} 192"))
        if m and int(m.group(1)) > 0:
            out += bytes.fromhex(m.group(2)); last = time.time()
    return out.decode(errors="replace")
def send(text):
    b = text.encode()
    for i in range(0, len(b), 192):
        r = menu(f"l\\w {TOKEN} {b[i:i + 192].hex().upper()}")
        if " 1]" not in r: sys.exit("write failed: " + r)
s.write(b"\x03"); rd(0.5)
menu(f"l\\e {TOKEN}"); menu(f"l\\c {TOKEN}"); drain(1.0)
send("stty -echo; rm -f /tmp/_put.b64\n"); drain(1.0)
b64 = base64.b64encode(data).decode()
lines = [b64[i:i + 76] for i in range(0, len(b64), 76)]
for i in range(0, len(lines), 20):                     # ~1.5 KB per heredoc
    send("cat >> /tmp/_put.b64 <<'EOF'\n" + "\n".join(lines[i:i + 20]) + "\nEOF\n")
    drain(0.5)
send(f"base64 -d /tmp/_put.b64 > {remote} && sha256sum {remote}; stty echo\n")
out = drain(1.5)
want = hashlib.sha256(data).hexdigest()
print(out.strip())
print("OK" if want in out else f"MISMATCH (want {want})")
menu(f"l\\e {TOKEN}")
