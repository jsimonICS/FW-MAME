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
(Raspberry Pi OS). `spi` needs python3-spidev, the UART tests pyserial.

Every test sends an incrementing byte sequence, which the FPGA checks.
"""
import argparse
import re
import subprocess
import sys
import time

# CM0 GPIOs the stock link is known or likely to use (wilicm0bsp: SPI0 on
# 9-11, PL011 UART on 14/15 with RTS/CTS on 16/17, software CS on 21) plus
# SPI1's other pins.
CANDIDATES = (9, 10, 11, 14, 15, 16, 17, 19, 20, 21)


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


def open_uart(args):
    import serial
    return serial.Serial(args.port, args.baud, timeout=0.2, rtscts=args.rtscts)


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
