#!/usr/bin/env python3
"""PC side of the FW-MAME link test: reads and controls the fpga/linktest
design through MAIN's FPGA menu (h\\f) over the FreeWili 2's USB serial port.

    python host/linkstat.py --port COM14 clear      # zero all counters
    python host/linkstat.py --port COM14 show       # snapshot and print
    python host/linkstat.py --port COM14 tx on      # FPGA -> CM0 UART sequence
    python host/linkstat.py --port COM14 set 0x20 0x01   # any register

Close the FreeWili GUI first: it holds the port. Needs pyserial.
"""
import argparse
import re
import sys
import time

import serial

# fw2.pcf net and the CM0 GPIO that drives it (measured 2026-11-07)
INPUTS = ("CM0_SPI0_MISO=GPIO10 MOSI", "CM0_SPI0_CLK=GPIO11 SCLK", "CM0_RX1=GPIO17 RTS",
          "CM0_SPI1_MISO=GPIO14 TXD", "CM0_SPI1_CLK=GPIO21 CS")


class Main:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.s.write(b"\x03")
        self._read(0.5)

    def _read(self, t):
        end, out = time.time() + t, b""
        while time.time() < end:
            out += self.s.read(4096)
        return re.sub(r"\x1b\[[0-9;]*m", "", out.decode(errors="replace"))

    def cmd(self, line, wait=0.6):
        self.s.write(line.encode() + b"\r")
        return self._read(wait)

    def write(self, addr, value):
        reply = self.cmd(f"h\\f\\w {addr:02X} {value:02X}")
        if "[h\\f\\w" not in reply:
            sys.exit(f"no reply to register write: {reply!r}")

    def read(self, addr, n):
        reply = self.cmd(f"h\\f\\r {addr:02X} {n}")
        m = re.search(r"\[h\\f\\r ([^\]]*)\]", reply)
        if not m:
            sys.exit(f"no reply to register read: {reply!r}")
        tokens = m.group(1).split()
        # [h\f\r <hash> <seq> b0 .. b(n-1) <status>]
        if len(tokens) < n + 2 or tokens[-1] != "1":
            sys.exit(f"register read failed (is the linktest design loaded?): {m.group(0)}")
        return bytes(int(t, 16) for t in tokens[-(n + 1):-1])


def le(b, i, n):
    return int.from_bytes(b[i:i + n], "little")


def cmd_show(dev, _args):
    dev.write(0x1C, 0x02)                       # snapshot
    # 0x10 .. 0x3E in pieces of <= 8, a count that reads the same in hex and decimal
    r = b"".join(dev.read(a, min(8, 0x3F - a)) for a in range(0x10, 0x3F, 8))
    at = lambda a, n=1: le(r, a - 0x10, n)
    if at(0x1D) != 1:
        sys.exit(f"revision register reads {at(0x1D)}, expected 1: is the linktest design loaded?")
    print("input                       edges  level   (edge count / 2 = CM0 GPIO after `linktest.py pins`)")
    for i, name in enumerate(INPUTS):
        print(f"  {i} {name:25} {at(0x10 + 2 * i, 2):6}  {(at(0x1A) >> i) & 1}")
    sel, cs = at(0x20), at(0x21)
    print(f"SPI   sclk=in{sel & 7} data=in{(sel >> 4) & 7} cs={'none' if cs & 0x80 else f'in{cs & 7}'}:"
          f" {at(0x24, 4)} bytes, {at(0x28, 4)} sequence errors, last {at(0x2C):02X}")
    div = at(0x31)
    print(f"UART  rx=in{at(0x30) & 7} {31.25e6 / div / 1e6:.4f} Mbaud ({div} clocks/bit):"
          f" {at(0x34, 4)} bytes, {at(0x38, 4)} sequence errors, {at(0x3C, 2)} framing errors,"
          f" last {at(0x3E):02X}")
    ctrl = at(0x1B)
    print(f"to CM0: UART TX {'on' if ctrl & 1 else 'off'}, CTS_n {ctrl >> 1 & 1}")


def cmd_clear(dev, _args):
    dev.write(0x1C, 0x01)
    print("counters cleared")


def cmd_tx(dev, args):
    ctrl = dev.read(0x1B, 1)[0]
    dev.write(0x1B, (ctrl | 1) if args.state == "on" else (ctrl & ~1))
    print(f"FPGA -> CM0 UART sequence {args.state}")


def cmd_set(dev, args):
    dev.write(int(args.addr, 0), int(args.value, 0))
    print(f"wrote {int(args.value, 0):02X} to {int(args.addr, 0):02X}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", default="COM14", help="FreeWili 2 MAIN serial port")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("show").set_defaults(func=cmd_show)
    sub.add_parser("clear").set_defaults(func=cmd_clear)
    s = sub.add_parser("tx")
    s.add_argument("state", choices=("on", "off"))
    s.set_defaults(func=cmd_tx)
    s = sub.add_parser("set")
    s.add_argument("addr")
    s.add_argument("value")
    s.set_defaults(func=cmd_set)
    args = p.parse_args()
    args.func(Main(args.port), args)


if __name__ == "__main__":
    main()
