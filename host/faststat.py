#!/usr/bin/env python3
"""PC side of the FW-MAME fast link test: reads and controls the fpga/fastlink
design through MAIN's FPGA menu (h\\f) over the FreeWili 2's USB serial port.

    python host/faststat.py --port COM14 clear       # zero all counters
    python host/faststat.py --port COM14 show        # snapshot and print
    python host/faststat.py --port COM14 ctrl 0x01   # control register (see below)
    python host/faststat.py --port COM14 pattern 0x07  # LFSR on SPI, UART RX and UART TX

Control (0x10): bit 0 UART TX to the CM0 on, bit 1 TX waits for RTS_n,
bit 2 CTS_n level, bit 3 SPI without CS. Close the FreeWili GUI first: it
holds the port. Needs pyserial.
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from linkstat import Main  # noqa: E402

CLK_HZ = 31.25e6
REVISION = 1


def snapshot(dev):
    """Snapshot the counters and return them as a dict."""
    dev.write(0x11, 0x02)
    r = b"".join(dev.read(a, 8) for a in range(0x10, 0x48, 8))
    at = lambda a, n=1: int.from_bytes(r[a - 0x10:a - 0x10 + n], "little")
    if at(0x14) != REVISION:
        sys.exit(f"revision register reads {at(0x14)}, expected {REVISION}: is the fastlink design loaded?")
    return {
        "ctrl": at(0x10), "pattern": at(0x12), "levels": at(0x13),
        "spi_bytes": at(0x20, 4), "spi_errs": at(0x24, 4), "spi_span": at(0x28, 4) / CLK_HZ,
        "spi_maxgap": at(0x2C, 4) / CLK_HZ, "spi_bursts": at(0x30, 2), "spi_last": at(0x32),
        "rx_bytes": at(0x34, 4), "rx_errs": at(0x38, 4), "rx_ferrs": at(0x3C, 2), "rx_last": at(0x3E),
        "rx_span": at(0x40, 4) / CLK_HZ, "tx_bytes": at(0x44, 4),
    }


def rate(nbytes, span):
    return (nbytes - 1) / span / 1e6 if nbytes > 1 and span > 0 else 0.0


def format_snapshot(s):
    pat = s["pattern"]
    lines = [
        f"SPI   {s['spi_bytes']} bytes, {s['spi_errs']} sequence errors, {s['spi_bursts']} CS bursts,"
        f" {rate(s['spi_bytes'], s['spi_span']):.3f} MB/s first to last byte,"
        f" longest gap {s['spi_maxgap'] * 1e6:.1f} us, last {s['spi_last']:02X}"
        f" ({'lfsr' if pat & 1 else 'incrementing'}, {'no CS' if s['ctrl'] & 8 else 'CS'})",
        f"UART  {s['rx_bytes']} bytes, {s['rx_errs']} sequence errors, {s['rx_ferrs']} framing errors,"
        f" {rate(s['rx_bytes'], s['rx_span']) * 1e3:.1f} KB/s, last {s['rx_last']:02X}"
        f" ({'lfsr' if pat & 2 else 'incrementing'})",
        f"to CM0: UART TX {'on' if s['ctrl'] & 1 else 'off'}{', waits for RTS' if s['ctrl'] & 2 else ''},"
        f" {s['tx_bytes']} bytes sent ({'lfsr' if pat & 4 else 'incrementing'}), CTS_n {s['ctrl'] >> 2 & 1},"
        f" inputs {s['levels']:05b}",
    ]
    return "\n".join(lines)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", default="COM14", help="FreeWili 2 MAIN serial port")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("show")
    sub.add_parser("clear")
    for name in ("ctrl", "pattern"):
        sub.add_parser(name).add_argument("value")
    args = p.parse_args()
    dev = Main(args.port)
    if args.cmd == "show":
        print(format_snapshot(snapshot(dev)))
    elif args.cmd == "clear":
        dev.write(0x11, 0x01)
        print("counters cleared")
    else:
        addr = 0x10 if args.cmd == "ctrl" else 0x12
        dev.write(addr, int(args.value, 0))
        print(f"wrote {int(args.value, 0):02X} to {addr:02X}")


if __name__ == "__main__":
    main()
