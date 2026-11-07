# fastlink

FW-MAME milestone 2. A SPI receiver clocked by the CM0's own SCLK, so SPI
speed is set by the wires and the CM0 rather than by the design clock. It
also has a UART receiver, so SPI and UART can be measured together, and a
UART back to the CM0 that can wait for the CM0's RTS. Every receiver checks
an incrementing or LFSR byte sequence and reports bytes, errors, rate and the
longest gap. The register map and how it works are in the header of
[user_core.v](user_core.v).

- **Clocks:** 31.25 MHz design clock, timing met at 33.9 MHz. The SCLK
  domain closes at 112 MHz. Uses 31% of the logic.
- **Pins:** fixed to the map measured by linktest (MOSI GPIO10, SCLK GPIO11,
  CS GPIO21, UART TXD GPIO14 and RTS GPIO17 in; RXD GPIO15 and CTS out).
- **Header:** not used. Every pin keeps the stock passthrough.
- **Needs** the FW-MAME wilifpga patch (`[host] cm0_link = true`).
- **Simulation:** `tools/fwfpga sim fastlink` drives SCLK at 7, 12.5, 25,
  33.3 and 40 MHz (none related to the design clock). It checks CS framing,
  the LFSR mode with an injected error, realigning after stray bits with and
  without CS, the rate and gap measurements, UART RX at ±2%, SPI and UART at
  the same time, and UART TX pausing on RTS.

**Hardware, 2026-11-07:** SPI is byte-exact up to 66.7 MHz (8.24 MB/s
sustained) and fails at 100 MHz. SPI and UART together were clean, and
FPGA → CM0 with RTS/CTS had no errors. Details:
[docs/fastlink-results.md](../../docs/fastlink-results.md).

Run it from the PC (CM0 reachable over its USB network):

```sh
(cd wilifpga && tools/fwfpga build fastlink && tools/fwfpga --port COM14 install fastlink)
python host/fastlink_run.py --port COM14
```
