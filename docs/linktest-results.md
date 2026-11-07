# Link test results, 2026-11-07

**Setup:**
- FreeWili 2 with MAIN firmware v11.
- CM0 running Debian 13 (kernel 6.12.75+rpt-rpi-v8). It has no network and
  no USB gadget port, so it was reached only through MAIN's Linux menu
  ([host/cm0sh.py](../host/cm0sh.py)).
- `fpga/linktest` v001 at 31.25 MHz.
- The run was scripted with [cm0/linktest_job.py](../cm0/linktest_job.py) and
  [host/linktest_run.py](../host/linktest_run.py). Senders were Python on the
  CM0, using spidev with 4096-byte writes, and the UART at 7.8125 Mbaud with
  no flow control.

## Pin map

Found by toggling each CM0 GPIO 2 × its number times and reading the FPGA's
edge counters: 20, 22, 34, 28 and 42 edges.

| FPGA net (fw2.pcf) | `cm0_in`/`cm0_out` bit | CM0 GPIO | Stock use |
|---|---|---|---|
| CM0_SPI0_MISO | in 0 | 10 | SPI0 MOSI (data in) |
| CM0_SPI0_CLK | in 1 | 11 | SPI0 SCLK |
| CM0_RX1 | in 2 | 17 | UART RTS |
| CM0_SPI1_MISO | in 3 | 14 | UART TXD |
| CM0_SPI1_CLK | in 4 | 21 | Software chip select |
| CM0_RX0 | out 0 | 15 | UART RXD (confirmed by the receive test) |
| CM0_CTS0 | out 1 | 16 (not tested) | UART CTS |

The net names don't match their functions: "MISO" carries MOSI, and the UART
TX arrives on a net named for SPI1. Use this table, not the names.

## Throughput and errors

| Test | CM0 sent | FPGA received | Errors | CM0 sender rate |
|---|---|---|---|---|
| SPI 1 MHz | 225,280 | 225,280 | 0 | 0.111 MB/s |
| SPI 4 MHz | 671,744 | 671,744 | 0 | 0.336 MB/s |
| SPI 7 MHz | 937,984 | 937,984 | 0 | 0.469 MB/s |
| SPI 8 MHz | 1,011,712 | 1,011,712 | 0 | 0.505 MB/s |
| SPI 10 MHz | 1,130,496 | 1,130,496 | 0 | 0.564 MB/s |
| SPI 12.5 MHz | 1,232,896 | 1,232,896 | 0 | 0.616 MB/s |
| SPI 16 MHz | 1,335,296 | 1,279,055 | 1,178,249 | 0.666 MB/s |
| UART CM0 → FPGA | 1,228,800 | 1,228,800 | 0 | 614 KB/s |
| UART FPGA → CM0 | n/a | 2,285,803 in 3 s | 204 | 762 KB/s received |

## What it means

- **SPI is error-free up to 12.5 MHz and fails at 16 MHz.** The Pi's SPI
  clock comes from dividing its core clock, so the real rates may sit a little
  below those requested. 12.5 MHz is about the limit for a receiver that
  samples SCLK at 31.25 MHz (40 ns half-periods against a 32 ns clock).
  Faster needs a receiver clocked by SCLK itself (milestone 2).
- **The CM0 sender is the bottleneck.** Python spidev with 4 KB writes reached
  only 39-50% of the wire rate, because of gaps between transfers. Next steps:
  larger transfers (the spidev `bufsiz` module parameter), or a C sender.
- **The UART works both ways at 7.8125 Mbaud.** CM0 → FPGA was clean. FPGA →
  CM0 had 204 sequence errors in 2.29 MB, probably bytes the CM0 dropped while
  reading without flow control. Turning on RTS/CTS would show whether that's
  the cause.
- **Combined CM0 → FPGA at today's wire rates:** SPI 1.56 MB/s plus UART
  0.78 MB/s = 2.34 MB/s. That's enough for 4-bit raw frames (1.7 MB/s) if both
  links are used and the sender keeps up, but not for 8-bit raw (3.4 MB/s).

## Bench notes

- **Zone 6 failed at first:** "Zone 6 not applied by the PIC after 4000 ms".
  It worked on the second `h\p\s 6 1`. `l\a` then powered the CM0 (zone 17
  and its run line).
- **`l\u status`** answers ENOREPLY on this CM0 image (no USB-mode support),
  even with Linux up.
- **`systemctl stop fwcm0-bridge` takes 90 s,** because the bridge ignores
  SIGTERM. Use `systemctl kill -s KILL fwcm0-bridge` first.
- **Stopping the bridge ends MAIN's shell session,** so the CM0 side has to
  run detached (`setsid nohup`) and restart the bridge itself.
- **The CM0's normal pin state:** GPIO 9-11 `a0` (SPI0), 14-15 `a0` (UART),
  16-17 `a3` (CTS/RTS), 19-20 inputs, 21 output high.
