#!/usr/bin/env python3
r"""Run shell commands on the CM0 from the PC, through MAIN's Linux menu.

    python host/cm0sh.py COM14 2 "uname -a" "ls /tmp"

The CM0 needs no network or USB for this: MAIN's l\c / l\w / l\r / l\e
commands open a bash session over the stock FPGA's mailbox. So it only works
while the STOCK gateware is loaded and fwcm0-bridge runs on the CM0. The
second argument is how long to wait for output to go quiet (seconds).
Protocol (FW2 v11): l\c <token> opens, l\w <token> <hex, 1..192 bytes> writes,
l\r <token> <max> returns [l\r <id> <seq> <n> <hex> ...], l\e <token> closes.
Close the FreeWili GUI first (it holds the port).
"""
import serial, time, re, sys
TOKEN = "C0FFEE01"
s = serial.Serial(sys.argv[1], 115200, timeout=0.2)
def rd(t):
    end = time.time() + t; out = b""
    while time.time() < end:
        c = s.read(4096)
        if c:
            out += c
            if out.rstrip().endswith(b"]"):
                break
    return re.sub(r"\x1b\[[0-9;]*m", "", out.decode(errors="replace"))
def menu(line, t=2.0):
    s.write(line.encode() + b"\r")
    return rd(t)
def drain(idle):
    out, last = b"", time.time()
    while time.time() - last < idle:
        r = menu(f"l\\r {TOKEN} 192")
        m = re.search(r"\[l\\r \S+ \d+ (\d+) ?([0-9A-Fa-f]*)", r)
        if m and int(m.group(1)) > 0:
            out += bytes.fromhex(m.group(2))
            last = time.time()
        else:
            time.sleep(0.2)
    return re.sub(r"\x1b(\[[0-9;?]*[A-Za-z]|\][^\x07]*\x07)", "", out.decode(errors="replace"))
s.write(b"\x03"); rd(0.5)
menu(f"l\\e {TOKEN}")
menu(f"l\\c {TOKEN}")
drain(1.0)                                   # banner / old output
idle = float(sys.argv[2])
for c in sys.argv[3:]:
    data = (c + "\n").encode()
    for i in range(0, len(data), 192):
        r = menu(f"l\\w {TOKEN} {data[i:i + 192].hex().upper()}")
        if " 1]" not in r:
            print("write failed:", r.strip())
    sys.stdout.write(drain(idle).replace("\r\n", "\n"))
    sys.stdout.flush()
menu(f"l\\e {TOKEN}")
print()
s.close()
