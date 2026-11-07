#!/usr/bin/env python3
"""Unattended CM0 side of a full link-test run (PC side: host/linktest_run.py).
Start it detached as root, because killing fwcm0-bridge also ends the shell
session that started it:

    sudo setsid nohup python3 /tmp/linktest_job.py >/dev/null 2>&1 </dev/null &

Kills the bridge (a plain `systemctl stop` takes 90 s: it ignores SIGTERM),
waits up to 180 s for the PC's go (the FPGA's UART sequence on ttyAMA0), runs
every test on a fixed timeline, restarts the bridge, and leaves its log in
/tmp/lt2.log. Expects linktest.py in /tmp.
"""
import subprocess, sys, time
sys.path.insert(0, "/tmp")
from linktest import RawUart
log = open("/tmp/lt2.log", "w", buffering=1)
t0 = time.time()
def L(m):
    log.write(f"[{time.time() - t0:6.1f}] {m}\n")
def lt(*args):
    L(" ".join(args))
    r = subprocess.run(["python3", "/tmp/linktest.py", *args], capture_output=True, text=True)
    log.write(r.stdout + r.stderr)
subprocess.run(["systemctl", "kill", "-s", "KILL", "fwcm0-bridge"])
subprocess.run(["systemctl", "stop", "fwcm0-bridge"])
L("bridge stopped; waiting for go")
go = False
with RawUart("/dev/ttyAMA0", 7_812_500) as u:
    u.reset_input_buffer()
    end = time.time() + 180
    while time.time() < end:
        if len(u.read(4096)) > 64:
            go = True
            break
if go:
    t0 = time.time()
    L("go")
    def at(t):
        time.sleep(max(0.0, t0 + t - time.time()))
    at(10); lt("pins")
    for i, hz in enumerate((1_000_000, 4_000_000, 7_000_000, 8_000_000, 10_000_000, 12_500_000, 16_000_000)):
        at(30 + 15 * i); lt("spi", "--hz", str(hz), "--seconds", "2")
    at(140); lt("uart-tx", "--baud", "7812500", "--seconds", "2")
    at(160); lt("uart-rx", "--baud", "7812500", "--seconds", "3")
    at(185)
else:
    L("no go within 180 s")
subprocess.run(["systemctl", "start", "fwcm0-bridge"])
L("bridge started; done")
