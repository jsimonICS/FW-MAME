#!/usr/bin/env python3
"""PC side of a full link-test run: loads linktest, sends the go, reads the
counters on the same timeline as cm0/linktest_job.py, then restores the stock
gateware. Start it right after launching the CM0 job (via host/cm0sh.py),
with /fpga/linktest.fwfpga already on the SD card (`fwfpga install linktest`).

    python host/linktest_run.py COM14
"""
import pathlib, sys, time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import linkstat
start = time.time()
def stamp(msg):
    print(f"[{time.time() - start:6.1f}] {msg}", flush=True)
dev = linkstat.Main(sys.argv[1] if len(sys.argv) > 1 else "COM14")
time.sleep(4)                                    # let the CM0 kill its bridge
stamp("load: " + dev.cmd('h\\f\\l "/fpga/linktest.fwfpga"', 3.0).strip().splitlines()[-1])
dev.write(0x1C, 0x01)
dev.write(0x1B, 0x01)                            # go
t0 = time.time()
stamp("go (FPGA TX on)")
time.sleep(2)
dev.write(0x1B, 0x00)
def at(t):
    time.sleep(max(0.0, t0 + t - time.time()))
def show(label):
    stamp(f"--- {label}")
    linkstat.cmd_show(dev, None)
def clear():
    dev.write(0x1C, 0x01)
at(6); clear()
at(16); show("after pins (edge count / 2 = CM0 GPIO)")
dev.write(0x1C, 0x02)
r = dev.read(0x10, 8) + dev.read(0x18, 2)
counts = [int.from_bytes(r[2 * i:2 * i + 2], "little") for i in range(5)]
gpio = {c // 2: i for i, c in enumerate(counts) if c}
stamp(f"edges {counts} -> GPIO:net {gpio}")
sclk, mosi, txd = gpio.get(11, 1), gpio.get(10, 0), gpio.get(14, 3)
dev.write(0x20, (mosi << 4) | sclk)
dev.write(0x21, 0x80)
dev.write(0x30, txd)
stamp(f"SPI sclk=in{sclk} data=in{mosi}, UART rx=in{txd}")
for i, hz in enumerate((1, 4, 7, 8, 10, 12.5, 16)):
    at(25 + 15 * i); clear()
    at(36 + 15 * i); show(f"after SPI {hz} MHz")
at(135); clear()
at(146); show("after UART CM0 -> FPGA")
at(155); dev.write(0x1B, 0x01); stamp("FPGA TX on")
at(168); dev.write(0x1B, 0x00); stamp("FPGA TX off")
stamp("restore: " + dev.cmd("h\\f\\t", 3.0).strip().splitlines()[-1])
