# Fast link results, 2026-11-07

**Setup:**
- FreeWili 2 with MAIN firmware v11. CM0 running Debian 13 (kernel
  6.12.75+rpt-rpi-v8), reached over its USB network
  ([README](../README.md#reaching-the-cm0)), which keeps working while the
  FPGA design is loaded.
- `fpga/fastlink` v001: the SPI shift register is clocked by the CM0's SCLK,
  so the 31.25 MHz design clock no longer limits SPI.
- Sender: [cm0/fastlink.c](../cm0/fastlink.c), spidev with 64 KB transfers
  (module reloaded with `bufsiz=65536`), chip select on GPIO21 held low for
  the whole 2 s run, LFSR byte sequence. CPU governor set to performance.
- Run with [host/fastlink_run.py](../host/fastlink_run.py). Full output:
  [fastlink-run-2026-11-07.txt](fastlink-run-2026-11-07.txt).

"FPGA MB/s" is measured by the FPGA from its first byte to its last byte over
the 2 s run, so it includes the gaps between transfers.

## SPI, CM0 → FPGA

| SCLK | CM0 sent | FPGA received | Errors | FPGA MB/s | Wire busy | Longest gap |
|---|---|---|---|---|---|---|
| 12.5 MHz, 4 KB transfers | 3,076,096 | 3,076,096 | 0 | 1.537 | 98% | 66 µs |
| 12.5 MHz | 3,145,728 | 3,145,728 | 0 | 1.560 | 100% | 85 µs |
| 16.7 MHz | 4,194,304 | 4,194,304 | 0 | 2.078 | 100% | 84 µs |
| 20 MHz | 5,046,272 | 5,046,272 | 0 | 2.492 | 100% | 83 µs |
| 25 MHz | 6,225,920 | 6,225,920 | 0 | 3.113 | 100% | 77 µs |
| 28.6 MHz | 7,143,424 | 7,143,424 | 0 | 3.555 | 100% | 90 µs |
| 33.3 MHz | 8,323,072 | 8,323,072 | 0 | 4.145 | 99% | 81 µs |
| 40 MHz | 9,961,472 | 9,961,472 | 0 | 4.968 | 99% | 84 µs |
| 50 MHz | 12,451,840 | 12,451,840 | 0 | 6.200 | 99% | 86 µs |
| **66.7 MHz** | 16,515,072 | 16,515,072 | **0** | **8.242** | 99% | 95 µs |
| 100 MHz | 24,641,536 | 24,641,536 | 24,565,129 | 12.293 | 98% | 71 µs |
| 25 MHz, CS per 64 KB transfer | 6,225,920 | 6,225,920 | 0 | 3.113 | 100% | 75 µs |
| 25 MHz, no CS (idle realign) | 6,225,920 | 6,225,920 | 0 | 3.113 | 100% | 83 µs |

## SPI and UART together, and UART alone

| Test | SPI sent / received / errors | UART sent / received / errors | Total |
|---|---|---|---|
| SPI 25 MHz + UART | 6,225,920 / same / 0 | 1,572,864 / same / 0 | 3.89 MB/s |
| SPI 40 MHz + UART | 9,961,472 / same / 0 | 1,576,960 / same / 0 | 5.74 MB/s |
| UART alone (CM0 → FPGA) | n/a | 1,568,768 / same / 0, 781 KB/s | |

## UART, FPGA → CM0

| Test | FPGA sent | CM0 received | Errors |
|---|---|---|---|
| No flow control | 3,670,433 | 2,263,031 in 3 s (754 KB/s) | 50,513 |
| RTS/CTS (FPGA waits for the CM0's RTS) | 1,929,114 | 1,929,049 in 3 s (643 KB/s) | 0 |

The FPGA counts from the moment its transmitter is turned on, before the CM0
starts reading, so its count is higher than the CM0's. With RTS/CTS the
difference is the 65 bytes still in flight when the CM0 stopped.

## What it means

- **SPI is error-free up to 66.7 MHz: 8.24 MB/s sustained, measured by the
  FPGA.** That's 2.4× the 3.4 MB/s that raw 8-bit 256 × 224 frames at 60 fps
  need. At 100 MHz every byte arrives but almost every one is wrong, so the
  limit is between 66.7 and 100 MHz (the CM0 only offers 400 MHz / even
  dividers, so nothing in between). For the framebuffer, 40-50 MHz
  (5-6.2 MB/s) leaves a wide margin.
- **The CM0 sender is no longer the bottleneck.** The C sender keeps the wire
  99-100% busy. Between 64 KB transfers the line is idle for 70-95 µs, about
  1% at 50 MHz. Even 4 KB transfers reach 98% from C, so milestone 1's
  40-50% was Python's per-call overhead, not spidev.
- **The SCLK-clocked receiver works.** linktest's oversampling receiver failed
  at 16 MHz. This one is byte-exact to 66.7 MHz, and in nextpnr its clock
  domain closes at 112 MHz.
- **Framing:** a CS burst lasting 2 s, CS per transfer, and no CS at all
  (realigning after idle gaps) all worked at 25 MHz.
- **The UART adds 0.78 MB/s alongside SPI with no errors,** but SPI alone
  goes far past it, so the framebuffer doesn't need the UART for frames.
- **FPGA → CM0 needs RTS/CTS.** Without it the CM0 drops bytes, 50,513 in this
  run (the CM0's receive side can't keep up at 7.8125 Mbaud). With flow
  control there were no errors at 643 KB/s, far more than inputs need.

## Bench notes

- **SPI clock = CM0 core clock / even divider,** and the core clock follows
  the CPU governor: 287.5 MHz at idle under `ondemand`, 400 MHz under
  `performance`. All the rates above are 400 MHz ones. MAME has to run with
  the core clock pinned (performance governor, or `core_freq_min=400` in
  config.txt), or SPI slows down whenever the CPU idles.
- **Round the requested rate up:** the bcm2835 driver rounds the divider up,
  so asking for 33,333,333 Hz gets divider 14 (28.6 MHz). 33,333,334 gets 12.
- **Reloading spidev** (`modprobe -r spidev; modprobe spidev bufsiz=65536`)
  works once fwcm0-bridge is stopped. It lasts until the next reboot.
- With the USB network, nothing has to run detached any more: stopping
  fwcm0-bridge and loading the design don't touch the SSH session.
