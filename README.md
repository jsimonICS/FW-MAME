# FW-MAME

Arcade emulation on the FreeWili 2, shown on a real 15 kHz arcade monitor.
The plan:
- **MAME** runs on the FW2's CM0 Linux module.
- **The CM0** sends each frame to the iCE40 FPGA.
- **The FPGA** keeps the frame in its own memory and generates 3-3-3 RGB
  plus composite sync on the 20-pin header. It's the same signal, pins and
  resistor ladder board as [FW-TPG](https://github.com/jsimonICS/FW-TPG).

Status: **milestone 1 (link test)** is built and passes simulation. It hasn't
run on hardware yet.

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
  Which CM0 GPIO drives which FPGA net isn't documented. The link test finds
  out.
- **Loading our design cuts the CM0 off from MAIN.** The stock gateware
  carries the CM0's mailbox to MAIN, so while our design runs there's no
  `fwcm0-bridge`, no GUI CM0 shell and no OneWili on the CM0. Reach the CM0
  over its USB gadget serial port or SSH instead. `fwfpga restore` brings
  the stock gateware back.
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

The stock link speeds are a starting point, not a limit. The link test
measures the real speeds.

## Milestones

1. **Link test** ([fpga/linktest](fpga/linktest)): map the CM0 GPIOs to FPGA
   nets, then measure SPI and UART in both directions. *Built and simulated.*
2. **Fast receiver:** capture SPI on the CM0's own clock instead of
   oversampling it, to find the top SPI rate. The goal is at least 1.7 MB/s,
   ideally 3.4 MB/s.
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
| [host/linkstat.py](host/linkstat.py) | PC side: reads and controls the design through MAIN |
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

Close the FreeWili GUI first, because it holds MAIN's serial port. MAIN must
have power zone 6 (FPGA) on. Replace `COM14` with your FW2's port.

1. **On the CM0** (USB serial or SSH), stop the bridge:
   `sudo systemctl stop fwcm0-bridge`. Copy `cm0/linktest.py` over and
   install `python3-spidev` and `python3-serial`.
2. **On the PC**, from `wilifpga/`, load the design:
   `tools/fwfpga install linktest` then `tools/fwfpga load linktest`.
3. **Pin map:** on the PC, `python host/linkstat.py --port COM14 clear`. On the
   CM0, `sudo python3 linktest.py pins`. Then on the PC,
   `python host/linkstat.py --port COM14 show`. Each input's edge count ÷ 2 is
   the CM0 GPIO that drives it.
4. **SPI:** if the map differs from the defaults (SCLK on input 1, data on
   input 0), set register 0x20. Then on the CM0 run
   `python3 linktest.py spi --hz 7000000`, and try higher rates. After each
   run, check `show` on the PC: the byte count should match and errors should
   be 0. Clear between runs.
5. **UART, CM0 to FPGA:** `python3 linktest.py uart-tx`. Set register 0x30 to
   the input the map found.
6. **UART, FPGA to CM0:** on the PC, `python host/linkstat.py tx on`. On the
   CM0, `python3 linktest.py uart-rx`.
7. **Clean up:** on the PC, `tools/fwfpga restore`. On the CM0,
   `sudo systemctl start fwcm0-bridge`.

## License

MIT, see [LICENSE](LICENSE). MAME 2003-Plus has its own non-commercial
license, and game ROMs are copyrighted by their owners. Neither is included
here. Use ROMs you're licensed to use.
