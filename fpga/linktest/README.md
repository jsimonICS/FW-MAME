# linktest

FW-MAME milestone 1. Finds out how the CM0 is wired to the FPGA and how fast
each wire goes. The register map and how it works are in the header of
[user_core.v](user_core.v). How to run it is in the
[top-level README](../../README.md#running-the-link-test).

- **Clock:** 31.25 MHz, timing met at 33.2 MHz. Uses 23.5% of the logic.
- **Header:** not used. Every pin keeps the stock passthrough.
- **Needs** the FW-MAME wilifpga patch (`[host] cm0_link = true`).
- **Simulation:** `tools/fwfpga sim linktest` checks the edge counters, SPI
  at 7 and 7.8 MHz (including error counting and realignment after a stray
  clock), UART reception at 7.8125 Mbaud ±2% with framing errors, and UART
  transmission to the CM0.
