#!/usr/bin/env python3
"""Full fast-link run (FW-MAME milestone 2), driven from the PC: the CM0 over
SSH on its USB network (see README "Reaching the CM0"), the FPGA through
MAIN's FPGA menu. The CM0 stays reachable while fastlink is loaded, so each
step runs in order: set up the FPGA, run the CM0 sender, read the FPGA.

    python host/fastlink_run.py --port COM14 [--log docs/fastlink-run.txt]

Needs /fpga/fastlink.fwfpga on the SD card (`fwfpga install fastlink`) and
an SSH key for pi@10.12.194.1. Steps:
  1. copy cm0/fastlink.c to the CM0 and compile it;
  2. stop fwcm0-bridge (it owns SPI0, the UART and GPIO21), set the CPU
     governor to performance, reload spidev with bufsiz=65536;
  3. load fastlink, run PLAN, restore the stock gateware;
  4. restart the bridge, restore the governor, print a summary.
"""
import argparse
import math
import pathlib
import re
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import faststat  # noqa: E402
import linkstat  # noqa: E402

LFSR_ALL = 0x07       # FPGA pattern register: LFSR on SPI, UART RX and UART TX
CORE_HZ = 400e6       # CM0 core clock; SPI = core / even divider


def spi(div, *extra):
    """SPI at CORE_HZ / div. Round the request up: the driver rounds the divider up."""
    return ["send", "--hz", str(math.ceil(CORE_HZ / div)), "--seconds", "2", "--lfsr", *extra]


# (label, fastlink arguments on the CM0, FPGA control register)
PLAN = [
    ("SPI 12.5 MHz, 4 KB transfers", spi(32, "--chunk", "4096"), 0x00),
    ("SPI 12.5 MHz", spi(32), 0x00),
    ("SPI 16.7 MHz", spi(24), 0x00),
    ("SPI 20 MHz", spi(20), 0x00),
    ("SPI 25 MHz", spi(16), 0x00),
    ("SPI 28.6 MHz", spi(14), 0x00),
    ("SPI 33.3 MHz", spi(12), 0x00),
    ("SPI 40 MHz", spi(10), 0x00),
    ("SPI 50 MHz", spi(8), 0x00),
    ("SPI 66.7 MHz", spi(6), 0x00),
    ("SPI 100 MHz", spi(4), 0x00),
    ("SPI 25 MHz, CS per 64 KB transfer", spi(16, "--cs", "xfer"), 0x00),
    ("SPI 25 MHz, no CS", spi(16, "--cs", "none"), 0x08),
    ("SPI 25 MHz + UART", spi(16, "--uart"), 0x00),
    ("SPI 40 MHz + UART", spi(10, "--uart"), 0x00),
    ("UART CM0 -> FPGA", ["send", "--hz", "0", "--uart", "--seconds", "2", "--lfsr"], 0x00),
    ("UART FPGA -> CM0, no flow control", ["recv", "--seconds", "3", "--lfsr"], 0x01),
    ("UART FPGA -> CM0, RTS/CTS", ["recv", "--seconds", "3", "--lfsr", "--rtscts"], 0x03),
]

GOV = "/sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor"


class Cm0:
    def __init__(self, host, key):
        self.base = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5"] + (["-i", key] if key else []) + [host]
        self.host, self.key = host, key

    def run(self, cmd, check=True):
        r = subprocess.run(self.base + [cmd], capture_output=True, text=True)
        # drop the image's SSH banner about setting up a user
        err = "\n".join(l for l in r.stderr.splitlines() if l and "newuser" not in l and "valid user" not in l)
        if check and r.returncode != 0:
            sys.exit(f"CM0 command failed ({r.returncode}): {cmd}\n{r.stdout}{err}")
        return r.stdout + (err + "\n" if err else "")

    def put(self, local, remote):
        subprocess.run(["scp", "-q"] + (["-i", self.key] if self.key else []) + [str(local), f"{self.host}:{remote}"],
                       check=True)


def parse_result(text):
    m = re.search(r"^result (.*)$", text, re.M)
    return {k: float(v) for k, v in (kv.split("=") for kv in m.group(1).split())} if m else {}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", default="COM14", help="FreeWili 2 MAIN serial port")
    p.add_argument("--host", default="pi@10.12.194.1")
    p.add_argument("--key", default=str(pathlib.Path.home() / ".ssh" / "fw2_cm0"))
    p.add_argument("--log", help="also write the output to this file")
    args = p.parse_args()

    out = open(args.log, "w", encoding="utf-8") if args.log else None
    start = time.time()

    def say(msg):
        line = f"[{time.time() - start:6.1f}] {msg}"
        print(line, flush=True)
        if out:
            out.write(line + "\n")

    cm0 = Cm0(args.host, args.key)
    cm0.put(HERE.parent / "cm0" / "fastlink.c", "/tmp/fastlink.c")
    cm0.run("gcc -O2 -Wall -pthread -o /tmp/fastlink /tmp/fastlink.c")
    say("CM0: fastlink compiled. " + cm0.run("uname -r; vcgencmd measure_clock core arm | tr '\\n' ' '").strip())
    old_gov = cm0.run("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor").strip()
    cm0.run("sudo systemctl kill -s KILL fwcm0-bridge; sudo systemctl stop fwcm0-bridge; "
            f"for c in 0 1 2 3; do echo performance | sudo tee {GOV} >/dev/null; done; "
            "sudo modprobe -r spidev && sudo modprobe spidev bufsiz=65536 && sleep 1")
    say(f"CM0: bridge stopped, governor {old_gov} -> performance, spidev bufsiz "
        + cm0.run("cat /sys/module/spidev/parameters/bufsiz").strip())

    dev = linkstat.Main(args.port)
    results = []
    try:
        say("load: " + dev.cmd('h\\f\\l "/fpga/fastlink.fwfpga"', 3.0).strip().splitlines()[-1])
        say("CM0 still reachable with fastlink loaded: " + cm0.run("hostname").strip())
        for label, action, ctrl in PLAN:
            dev.write(0x12, LFSR_ALL)
            dev.write(0x10, ctrl)
            dev.write(0x11, 0x01)                    # clear
            text = cm0.run("sudo /tmp/fastlink " + " ".join(action))
            if ctrl & 1:
                dev.write(0x10, ctrl & ~1)           # FPGA TX off before reading
            s = faststat.snapshot(dev)
            say(f"--- {label}\nCM0: {text.strip()}\nFPGA:\n{faststat.format_snapshot(s)}")
            results.append((label, action, parse_result(text), s))
    finally:
        dev.write(0x10, 0x00)
        say("restore: " + dev.cmd("h\\f\\t", 3.0).strip().splitlines()[-1])
        cm0.run(f"for c in 0 1 2 3; do echo {old_gov} | sudo tee {GOV} >/dev/null; done; "
                "sudo systemctl start fwcm0-bridge", check=False)
        say("CM0: governor restored, bridge started")

    say("\nstep                                 CM0 sent    FPGA got   errors   MB/s  gap us |"
        " UART sent   got  errors  KB/s")
    for label, action, r, s in results:
        if action[0] == "recv":
            sent = f"{'(CM0 got ' + str(int(r.get('rx_bytes', 0))) + ', ' + str(int(r.get('rx_errs', 0))) + ' errors)':>39}"
            say(f"{label:36} {sent} | FPGA sent {s['tx_bytes']}")
            continue
        say(f"{label:36} {int(r.get('spi_bytes', 0)):9} {s['spi_bytes']:10} {s['spi_errs']:8}"
            f" {faststat.rate(s['spi_bytes'], s['spi_span']):6.3f} {s['spi_maxgap'] * 1e6:7.1f} |"
            f" {int(r.get('uart_bytes', 0)):9} {s['rx_bytes']:8} {s['rx_errs']:6}"
            f" {faststat.rate(s['rx_bytes'], s['rx_span']) * 1e3:5.0f}")


if __name__ == "__main__":
    main()
