# FW-MAME

Arcade emulation on the FreeWili 2, shown on a real 15 kHz arcade monitor.
The plan:
- **MAME** runs on the FW2's CM0 Linux module.
- **The CM0** sends each frame to the iCE40 FPGA.
- **The FPGA** keeps the frame in its own memory and generates 3-3-3 RGB
  plus composite sync on the 20-pin header. It's the same signal, pins and
  resistor ladder board as [FW-TPG](https://github.com/jsimonICS/FW-TPG).

Status: **milestone 1 (link test) is done on hardware** (2026-11-07). The CM0
pin map is known. SPI is error-free up to 12.5 MHz, and the UART works both
ways at 7.8125 Mbaud. Full results:
[docs/linktest-results.md](docs/linktest-results.md).

## How it fits together

```text
              frames: SPI0 (+ UART)                  RGB333 + CSYNC
 CM0 (Linux, MAME) ─────────────────► FPGA (iCE40UP5K) ─────────────► header ► ladder ► CRT
       ▲                                   ▲    │
       │ inputs: UART back from the FPGA   │    │
       └───────────────────────────────────┘    │ register writes
                                                ▼
 FW2 buttons / analog input ─► display app ─► MAIN
```

**FPGA:**
- Receives the frame and double-buffers it in the 128 KB SPRAM. That's enough
  for two 256 × 240 frames at 8 bits per pixel.
- Maps each pixel through a palette to RGB333.
- Generates the 15 kHz timing.

**Inputs:** the CM0 normally reaches MAIN through the stock FPGA gateware,
which is replaced while this design runs (see below). So inputs take a
different route: a display app reads the FW2's buttons and analog input and
writes them to FPGA registers through MAIN. The CM0 then reads them from the
FPGA.

## What we know so far

- **The CM0** is a BCM2837 module (Raspberry Pi Zero 2 class): four
  Cortex-A53 cores at about 1 GHz and 512 MB of RAM. Current MAME is too heavy
  for it. The target is **MAME 2003-Plus** (MAME 0.78 based), which runs most
  early-80s games at full speed on this class of chip.
- **CM0 ↔ FPGA wiring** (wilifpga `fw2.pcf`): 5 nets into the FPGA and 2 out.
  The stock link sends commands on SPI0 at 7 MHz and replies on the PL011 UART
  at 7.8125 Mbaud with RTS/CTS, with chip select on GPIO21 in software
  ([wilicm0bsp](https://github.com/freewili/wilicm0bsp) `agents/hardware.md`).
  The link test measured which CM0 GPIO drives which net. The net names don't
  match their functions:

  | FPGA net | CM0 GPIO | Function |
  |---|---|---|
  | CM0_SPI0_MISO (in 0) | 10 | SPI0 MOSI |
  | CM0_SPI0_CLK (in 1) | 11 | SPI0 SCLK |
  | CM0_RX1 (in 2) | 17 | UART RTS |
  | CM0_SPI1_MISO (in 3) | 14 | UART TXD |
  | CM0_SPI1_CLK (in 4) | 21 | software CS |
  | CM0_RX0 (out 0) | 15 | UART RXD |
  | CM0_CTS0 (out 1) | 16 (not tested) | UART CTS |

- **Measured link speeds:**
  - **SPI:** error-free up to 12.5 MHz (1.56 MB/s on the wire), fails at
    16 MHz.
  - **UART:** clean CM0 → FPGA at 7.8125 Mbaud. FPGA → CM0 delivered 762 KB/s
    with 204 dropped bytes in 2.3 MB.
  - **The CM0 sender is the bottleneck:** Python spidev with 4 KB writes
    managed only 0.62 MB/s at 12.5 MHz.
- **Reaching the CM0 without network or USB:** MAIN's Linux menu (`l\c`,
  `l\w`, `l\r`, `l\e`) gives a bash session through the stock FPGA's mailbox.
  [host/cm0sh.py](host/cm0sh.py) runs commands that way, and
  [host/cm0put.py](host/cm0put.py) copies files.
- **Loading our design cuts the CM0 off from MAIN.** The stock gateware
  carries the CM0's mailbox to MAIN, so while our design runs there's no
  `fwcm0-bridge`, no GUI CM0 shell and no OneWili on the CM0. Work on the CM0
  happens before loading, or in a detached job that restarts the bridge
  afterwards (see [cm0/linktest_job.py](cm0/linktest_job.py)), unless the
  CM0 has SSH or a USB gadget port. `fwfpga restore` brings the stock
  gateware back.
- **wilifpga v0.1.0 doesn't give designs the CM0 nets.** Its shell parks them,
  and its contract checks forbid a design from naming pads.
  [patches/wilifpga-cm0-link.patch](patches/wilifpga-cm0-link.patch) passes
  them to `user_core` as `cm0_in[4:0]` and `cm0_out[1:0]`. It only does so for
  designs whose design.toml sets `[host] cm0_link = true`. Directions are the
  same as the stock shell's, and the two outputs idle high until the design
  is armed. It's worth offering upstream.
- **Clock:** wilifpga's shell itself tops out around 40 MHz in this design, so
  48 MHz isn't available. The link test uses 31.25 MHz, where the UART runs at
  clk/4 = exactly 7.8125 Mbaud. Video will use 25 MHz, which divides the line
  time exactly (as FW-TPG does), or 31.25 MHz if one shared clock turns out
  simpler. 31.25 MHz has about 5 ns of cycle jitter, which is probably
  invisible on a CRT but untested.

### Bandwidth

A typical arcade frame is 256 × 224 at 60 fps:

| Format | Needed | Fits over |
|---|---|---|
| 8 bits per pixel, raw | 3.4 MB/s | SPI at about 28 MHz |
| 4 bits per pixel (16-color palette), raw | 1.7 MB/s | SPI at about 14 MHz |
| Changed lines only | Depends on the game, usually far less | Possibly the stock 7 MHz SPI (0.875 MB/s) |

Measured today, SPI at 12.5 MHz plus the UART carry 2.34 MB/s on the wire.
That's enough for 4-bit raw frames, but only if the CM0 sender keeps the
wires busy.

## Milestones

1. **Link test** ([fpga/linktest](fpga/linktest)): map the CM0 GPIOs to FPGA
   nets, then measure SPI and UART in both directions. *Done, see
   [docs/linktest-results.md](docs/linktest-results.md).*
2. **Fast link:**
   - A faster CM0 sender: larger spidev transfers or C. Today's Python sender
     reaches only 40-50% of the wire rate.
   - An FPGA receiver clocked by SCLK itself, to go past 12.5 MHz.
   - The goal is at least 1.7 MB/s sustained, ideally 3.4 MB/s, possibly
     with SPI and UART together.
3. **Framebuffer:** SPRAM double buffer, palette and RGB333 timing (ported
   from FW-TPG), with a CM0 tool that sends test images.
4. **Inputs:** a display app that writes buttons and analog input to FPGA
   registers, and a CM0 daemon that reads them and presents a Linux input
   device to MAME.
5. **MAME:** MAME 2003-Plus with its video output sent over the link. Add
   changed-line and palette compression if the bandwidth needs it.

## Repository layout

| Path | Contents |
|---|---|
| [fpga/linktest](fpga/linktest) | Link-test FPGA design and testbench |
| [cm0/linktest.py](cm0/linktest.py) | CM0 side of the link test (pin mapping, SPI and UART senders) |
| [cm0/linktest_job.py](cm0/linktest_job.py), [host/linktest_run.py](host/linktest_run.py) | Unattended full run, CM0 and PC sides |
| [host/linkstat.py](host/linkstat.py) | PC side: reads and controls the design through MAIN |
| [host/cm0sh.py](host/cm0sh.py), [host/cm0put.py](host/cm0put.py) | Shell commands and file copies to the CM0 through MAIN |
| [docs](docs) | Results |
| [patches](patches) | The wilifpga shell patch |
| [tools/setup.py](tools/setup.py) | Applies the patch and links `fpga/*` designs into wilifpga |
| `wilifpga`, `wilicm0bsp` | Submodules: FreeWili FPGA toolkit, FreeWili CM0 BSP |

## Setup

You need FW2 firmware v11-preview.1 or newer (the FPGA menu `h\f`), and the
wilifpga toolchain (`wilifpga/tools/fwfpga toolchain install`, about 1 GB).

```sh
git clone --recursive https://github.com/jsimonICS/FW-MAME
cd FW-MAME
python tools/setup.py          # patch wilifpga, link fpga/* into wilifpga/designs
cd wilifpga
tools/fwfpga sim linktest
tools/fwfpga build linktest
```

## Running the link test

Close the FreeWili GUI first, because it holds MAIN's serial port. Replace
`COM14` with your FW2's port.

**Power up:**
- **FPGA:** `h\p\s 6 1` turns on zone 6. Repeat it if it reports "not applied
  by the PIC".
- **CM0:** `l\a` powers it. Wait about a minute for Linux to boot.
- **Python packages on the CM0:** the CM0 image has `python3-spidev`.
  pyserial isn't needed, because `linktest.py` has a built-in UART driver.

### Scripted run

This is the 2026-11-07 run. It takes about 3 minutes and needs no network or
USB on the CM0.

```sh
python host/cm0put.py COM14 cm0/linktest.py /tmp/linktest.py
python host/cm0put.py COM14 cm0/linktest_job.py /tmp/linktest_job.py
(cd wilifpga && tools/fwfpga --port COM14 install linktest)
python host/cm0sh.py COM14 0.5 "sudo setsid nohup python3 /tmp/linktest_job.py >/dev/null 2>&1 </dev/null &"
python host/linktest_run.py COM14
python host/cm0sh.py COM14 1.5 "cat /tmp/lt2.log"    # once the bridge is back, ~20 s after the run
```

### By hand

Use this if the CM0 has SSH or a USB serial port.

1. **On the CM0:** `sudo systemctl kill -s KILL fwcm0-bridge`. A plain
   `stop` takes 90 s, because the bridge ignores SIGTERM.
2. **On the PC,** from `wilifpga/`: `tools/fwfpga install linktest`, then
   `tools/fwfpga load linktest`.
3. **Pin map:** on the PC, `python host/linkstat.py --port COM14 clear`. On the
   CM0, `sudo python3 linktest.py pins`. Then on the PC,
   `python host/linkstat.py --port COM14 show`. Each input's edge count ÷ 2 is
   the CM0 GPIO that drives it.
4. **SPI:** `python3 linktest.py spi --hz 12500000` on the CM0, then `show` on
   the PC. The byte count should match and errors should be 0. Clear between
   runs.
5. **UART, CM0 to FPGA:** `python3 linktest.py uart-tx`. The receiver defaults
   to input 3 (GPIO14).
6. **UART, FPGA to CM0:** on the PC, `python host/linkstat.py tx on`. On the
   CM0, `python3 linktest.py uart-rx`.
7. **Clean up:** on the PC, `tools/fwfpga restore`. On the CM0,
   `sudo systemctl start fwcm0-bridge`.

## License

MIT, see [LICENSE](LICENSE). MAME 2003-Plus has its own non-commercial
license, and game ROMs are copyrighted by their owners. Neither is included
here. Use ROMs you're licensed to use.
