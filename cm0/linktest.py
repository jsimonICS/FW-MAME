#!/usr/bin/env python3
"""CM0 side of the FW-MAME link test. Runs on the CM0 (Linux), with the
fpga/linktest design loaded on the FPGA. Read the results on the PC with
host/linkstat.py.

    sudo systemctl stop fwcm0-bridge      # it owns SPI0 and the UART
    sudo python3 linktest.py pins         # which CM0 GPIO drives which FPGA net
    python3 linktest.py spi --hz 7000000 --seconds 5
    python3 linktest.py uart-tx --baud 7812500 --seconds 5
    python3 linktest.py uart-rx --baud 7812500 --seconds 5   # host: linkstat.py tx on
    sudo systemctl start fwcm0-bridge     # after `fwfpga restore` on the PC

`pins` drives each candidate GPIO as an output and toggles it 2 x (GPIO
number) times, so on the FPGA an input's edge count / 2 is the GPIO that
drives it. It restores each pin's function afterwards. Needs `pinctrl`
(Raspberry Pi OS). `spi` needs python3-spidev. The UART tests use pyserial
if it is installed, else a built-in termios2 driver.

Every test sends an incrementing byte sequence, which the FPGA checks.
"""
import argparse
import re
import subprocess
import sys
import time

# Only the CM0 GPIOs that are already outputs in the stock link (wilicm0bsp:
# SPI0 MOSI 10 and SCLK 11, PL011 TXD 14 and RTS 17, software CS 21): the
# FPGA has exactly 5 inputs from the CM0, and these 5 cannot be wired to one
# of its 2 outputs. Never drive a pin the FPGA may be driving (e.g. RXD 15,
# CTS 16, MISO 9): two outputs on one wire is contention.
CANDIDATES = (10, 11, 14, 17, 21)


def bridge_running():
    r = subprocess.run(["systemctl", "is-active", "--quiet", "fwcm0-bridge"])
    return r.returncode == 0


def pinctrl(*args):
    return subprocess.run(["pinctrl", *map(str, args)], capture_output=True, text=True, check=True).stdout


def pin_state(gpio):
    """(function, level) from `pinctrl get`, e.g. ('a0', 'lo') or ('op', 'hi')."""
    line = pinctrl("get", gpio)
    m = re.match(r"\s*\d+:\s+(\S+).*\|\s*(hi|lo)", line)
    if not m:
        sys.exit(f"cannot parse pinctrl output: {line!r}")
    return m.group(1), m.group(2)


def cmd_pins(args):
    saved = {g: pin_state(g) for g in args.gpios}
    print("GPIO  function  edges")
    try:
        for g in args.gpios:
            func, level = saved[g]
            pinctrl("set", g, "op", "dh" if level == "hi" else "dl")
            state = level == "hi"
            for _ in range(2 * g):
                state = not state
                pinctrl("set", g, "dh" if state else "dl")
                time.sleep(0.001)
            print(f"{g:4}  {func:8}  {2 * g}")
    finally:
        for g, (func, level) in saved.items():
            if func == "op":
                pinctrl("set", g, "op", "dh" if level == "hi" else "dl")
            else:
                pinctrl("set", g, func)
    print("restored. On the PC: python host/linkstat.py show  (edge count / 2 = GPIO)")


def pattern(start, n):
    return bytes((start + i) & 0xFF for i in range(n))


def run_for(seconds, step):
    t0 = time.perf_counter()
    total = 0
    while time.perf_counter() - t0 < seconds:
        total += step(total)
    return total, time.perf_counter() - t0


def cmd_spi(args):
    import spidev
    spi = spidev.SpiDev()
    spi.open(args.bus, args.dev)
    spi.mode = 0
    spi.bits_per_word = 8
    spi.max_speed_hz = args.hz

    def step(total):
        spi.writebytes2(pattern(total, args.chunk))
        return args.chunk

    total, dt = run_for(args.seconds, step)
    spi.close()
    print(f"SPI {args.hz / 1e6:.2f} MHz: sent {total} bytes in {dt:.2f} s = {total / dt / 1e6:.3f} MB/s"
          f" (wire rate {args.hz / 8e6:.3f} MB/s)")
    print("On the PC: python host/linkstat.py show  (SPI bytes should match, errors 0)")


class RawUart:
    """Just enough of pyserial for these tests, from the standard library:
    raw 8N1 at any baud rate through Linux's termios2 (BOTHER). The CM0 image
    has no pyserial and may have no network to install it."""
    TCGETS2, TCSETS2 = 0x802C542A, 0x402C542B      # _IOR/_IOW('T', 0x2A/0x2B, struct termios2)
    BOTHER, CBAUD, CRTSCTS = 0o010000, 0o010017, 0o20000000000
    FMT = "4I B 19s 2I"                            # iflag oflag cflag lflag line cc[19] ispeed ospeed

    def __init__(self, port, baud, rtscts=False):
        import fcntl, os, struct, termios
        self.os, self.termios = os, termios
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        buf = bytearray(struct.calcsize(self.FMT))
        fcntl.ioctl(self.fd, self.TCGETS2, buf)
        _, _, cflag, _, line, cc, _, _ = struct.unpack(self.FMT, buf)
        cc = bytearray(cc)
        cc[termios.VMIN], cc[termios.VTIME] = 0, 2   # reads return after 0.2 s with no data
        cflag = (cflag & ~(self.CBAUD | termios.CSIZE | termios.PARENB | termios.CSTOPB | self.CRTSCTS)) \
            | self.BOTHER | termios.CS8 | termios.CREAD | termios.CLOCAL | (self.CRTSCTS if rtscts else 0)
        fcntl.ioctl(self.fd, self.TCSETS2, struct.pack(self.FMT, 0, 0, cflag, 0, line, bytes(cc), baud, baud))

    def write(self, data):
        view = memoryview(data)
        while view:
            view = view[self.os.write(self.fd, view):]

    def read(self, n):
        return self.os.read(self.fd, n)

    def flush(self):
        self.termios.tcdrain(self.fd)

    def reset_input_buffer(self):
        self.termios.tcflush(self.fd, self.termios.TCIFLUSH)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.os.close(self.fd)


def open_uart(args):
    try:
        import serial
        return serial.Serial(args.port, args.baud, timeout=0.2, rtscts=args.rtscts)
    except ImportError:
        return RawUart(args.port, args.baud, args.rtscts)


def cmd_uart_tx(args):
    with open_uart(args) as s:
        def step(total):
            s.write(pattern(total, args.chunk))
            return args.chunk
        total, dt = run_for(args.seconds, step)
        s.flush()
    print(f"UART TX {args.baud} baud: sent {total} bytes in {dt:.2f} s = {total / dt / 1e3:.1f} KB/s"
          f" (wire rate {args.baud / 10e3:.1f} KB/s)")
    print("On the PC: python host/linkstat.py show  (UART bytes should match, errors 0)")


def cmd_uart_rx(args):
    with open_uart(args) as s:
        s.reset_input_buffer()
        state = {"expect": None, "errors": 0}

        def step(total):
            data = s.read(args.chunk)
            for b in data:
                if state["expect"] is not None and b != state["expect"]:
                    state["errors"] += 1
                state["expect"] = (b + 1) & 0xFF
            return len(data)

        total, dt = run_for(args.seconds, step)
    print(f"UART RX {args.baud} baud: {total} bytes in {dt:.2f} s = {total / dt / 1e3:.1f} KB/s,"
          f" {state['errors']} sequence errors")
    if total == 0:
        print("nothing received: on the PC run `python host/linkstat.py tx on` first")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("pins", help="toggle each candidate GPIO 2 x its number times")
    s.add_argument("--gpios", type=int, nargs="+", default=list(CANDIDATES))
    s.set_defaults(func=cmd_pins)

    s = sub.add_parser("spi", help="send an incrementing sequence on SPI")
    s.add_argument("--hz", type=int, default=7_000_000)
    s.add_argument("--seconds", type=float, default=5.0)
    s.add_argument("--chunk", type=int, default=4096, help="bytes per transfer (spidev bufsiz, default 4096)")
    s.add_argument("--bus", type=int, default=0)
    s.add_argument("--dev", type=int, default=0)
    s.set_defaults(func=cmd_spi)

    for name, func, helptext in (("uart-tx", cmd_uart_tx, "send an incrementing sequence on the UART"),
                                 ("uart-rx", cmd_uart_rx, "receive and check the FPGA's sequence")):
        s = sub.add_parser(name, help=helptext)
        s.add_argument("--baud", type=int, default=7_812_500)
        s.add_argument("--seconds", type=float, default=5.0)
        s.add_argument("--chunk", type=int, default=4096)
        s.add_argument("--port", default="/dev/ttyAMA0")
        s.add_argument("--rtscts", action="store_true", help="hardware flow control (FPGA holds CTS low)")
        s.set_defaults(func=func)

    args = p.parse_args()
    if bridge_running():
        sys.exit("fwcm0-bridge is running and owns SPI0 and the UART: sudo systemctl stop fwcm0-bridge")
    args.func(args)


if __name__ == "__main__":
    main()
