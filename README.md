# FW-MAME

Arcade emulation on the FreeWili 2, shown on a real 15 kHz arcade monitor.
The plan:
- **MAME** runs on the FW2's CM0 Linux module.
- **The CM0** sends each frame to the iCE40 FPGA.
- **The FPGA** keeps the frame in its own memory and generates 3-3-3 RGB
  plus composite sync on the 20-pin header. It's the same signal, pins and
  resistor ladder board as [FW-TPG](https://github.com/jsimonICS/FW-TPG).

Status (2026-11-08): **MAME 2003-Plus plays on the CRT at 60 fps with the
FW2's buttons, and its sound plays on the FW2's speaker.**
- **Link test:** the CM0 pin map is known.
- **Fast link:** SPI carries 8.24 MB/s error-free at 66.7 MHz.
- **Framebuffer:** the CM0 draws double-buffered 256×224 frames on the CRT at
  a steady 60.05 fps, a new frame every field.
- **Inputs, FW2 buttons:** all 14 buttons reach the CM0 in under one field.
  The FW2 display app ([fw2/apps/fwmame](fw2/apps/fwmame)) also loads the
  FPGA design itself.
- **Inputs, USB stick:** in progress. A low-speed HID controller enumerates
  through the FW2's USB hub; the button learn mode isn't finished.
- **MAME:** a libretro frontend ([cm0/fe](cm0/fe)) runs Mappy, 1942 and
  Carnival at 60 fps. Audio goes through the FPGA and MAIN to the display app,
  16 kHz mono, with on-screen volume buttons.

Full results: [docs/linktest-results.md](docs/linktest-results.md),
[docs/fastlink-results.md](docs/fastlink-results.md),
[docs/fbuf-results.md](docs/fbuf-results.md),
[docs/input-results.md](docs/input-results.md),
[docs/mame-results.md](docs/mame-results.md).

## How it fits together

```text
              frames + audio: SPI0                   RGB333 + CSYNC
 CM0 (Linux, MAME) ─────────────────► FPGA (iCE40UP5K) ─────────────► header ► ladder ► CRT
       ▲                                   ▲    │
       │ vsync + inputs: UART              │    │ audio: stream FIFO
       └───────────────────────────────────┘    │ inputs: register writes
                                                ▼
 FW2 buttons, USB stick ─► display app ◄────► MAIN
                               │
                               └─► codec ─► FW2 speaker
```

**FPGA:**
- Receives the frame and double-buffers it in the 128 KB SPRAM. That's enough
  for two 256 × 240 frames at 8 bits per pixel.
- Maps each pixel through a palette to RGB333.
- Generates the 15 kHz timing.

**Inputs:** the CM0 normally reaches MAIN through the stock FPGA gateware,
which is replaced while this design runs (see below). So inputs take a
different route. The display app reads the FW2's buttons and writes them to
FPGA registers through MAIN, one `h\f\w` per change, taking 1-2 ms. The FPGA
sends them to the CM0 every field, in the same UART packet that carries the
vsync status. A USB controller on the FW2's own USB host ports, read by the
display CPU through the on-board hub, takes the same route (in progress).

**Audio:** the CM0 sends MAME's sound, resampled to 16 kHz mono, as SPI
packets. The FPGA queues them in the shell's 4 KB stream FIFO. The display
app reads them through MAIN about every 4 ms and plays them on the FW2's codec
and speaker, about 75 ms behind the picture.

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

- **Measured link speeds** ([fpga/fastlink](fpga/fastlink),
  [cm0/fastlink.c](cm0/fastlink.c)):
  - **SPI:** error-free up to 66.7 MHz, 8.24 MB/s sustained as counted by the
    FPGA. It fails at 100 MHz, and the CM0 offers nothing in between
    (400 MHz core / even divider). The C sender with 64 KB transfers keeps the
    wire 99-100% busy.
  - **UART:** 0.78 MB/s CM0 → FPGA, clean even alongside SPI. FPGA → CM0
    needs RTS/CTS: without it the CM0 drops bytes, with it 643 KB/s and no
    errors.
  - **The SPI clock follows the CM0's core clock,** which drops to 287.5 MHz
    at idle under the `ondemand` governor. Pin it at 400 MHz (performance
    governor or `core_freq_min=400`) for a steady rate.
  - Milestone 1's oversampling receiver stopped at 12.5 MHz, and its Python
    sender reached only 40-50% of the wire rate.
- **Reaching the CM0 over USB** (the usual way, see
  [Reaching the CM0](#reaching-the-cm0)): the CM0's USB port shows up on the
  PC as a network adapter and a COM port, through the FW2's internal hub. SSH
  and scp work at 10.12.194.1, about 16 MB/s. This doesn't go through the
  FPGA, so it keeps working while our designs are loaded.
- **Reaching the CM0 through MAIN** (fallback): MAIN's Linux menu (`l\c`,
  `l\w`, `l\r`, `l\e`) gives a bash session through the stock FPGA's mailbox.
  [host/cm0sh.py](host/cm0sh.py) runs commands that way, and
  [host/cm0put.py](host/cm0put.py) copies files at about 1 KB/s.
- **Loading our design cuts the CM0 off from MAIN.** The stock gateware
  carries the CM0's mailbox to MAIN, so while our design runs there's no
  `fwcm0-bridge`, no GUI CM0 shell, no OneWili and no MAIN Linux menu on the
  CM0. The USB link isn't affected. `fwfpga restore` brings the stock
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
| 8 bits per pixel, raw | 3.4 MB/s | SPI at 28.6 MHz or more |
| 4 bits per pixel (16-color palette), raw | 1.7 MB/s | SPI at 16.7 MHz or more |
| Changed lines only | Depends on the game, usually far less | Any of the above |

Measured: SPI alone sustains 6.2 MB/s at 50 MHz and 8.24 MB/s at 66.7 MHz,
so raw 8-bit frames fit with room to spare and frame compression is
optional. The plan is 40-50 MHz for margin.

## Milestones

1. **Link test** ([fpga/linktest](fpga/linktest)): map the CM0 GPIOs to FPGA
   nets, then measure SPI and UART in both directions. *Done, see
   [docs/linktest-results.md](docs/linktest-results.md).*
2. **Fast link** ([fpga/fastlink](fpga/fastlink),
   [cm0/fastlink.c](cm0/fastlink.c)): an FPGA receiver clocked by SCLK and a
   C sender. The goal was at least 1.7 MB/s, ideally 3.4 MB/s. *Done:
   8.24 MB/s error-free, see
   [docs/fastlink-results.md](docs/fastlink-results.md).* Along the way the
   CM0 got a USB network link ([cm0/usbnet](cm0/usbnet)) that works while our
   designs are loaded.
3. **Framebuffer** ([fpga/fbuf](fpga/fbuf), [cm0/fb](cm0/fb)): SPRAM double
   buffer, 256-color palette and FW-TPG's 15 kHz RGB333 timing, plus a CM0
   library and test tool. *Done: 60.05 fps with whole frames, see
   [docs/fbuf-results.md](docs/fbuf-results.md).*
4. **Inputs** ([fw2/apps/fwmame](fw2/apps/fwmame), `fbuf` v002+, `fwfb_inputs()`):
   - *Phase 1, FW2 buttons: done, see
     [docs/input-results.md](docs/input-results.md).* The 14 buttons go from
     the display app through MAIN and the FPGA to the CM0. No CM0 daemon is
     needed: the MAME frontend reads them from `fwfb`.
   - Phase 2, USB joystick: *in progress, paused.* A HID gamepad, arcade
     encoder or XInput controller on the FW2's USB host ports goes in input
     bytes 2-5. The ports turned out to be behind an on-board hub on the
     display CPU's native USB, not PIO-USB, so the app has its own host code,
     with low-speed support. A HID controller enumerates and reports; the
     learn mode for mapping its buttons isn't finished
     ([fw2/apps/fwmame](fw2/apps/fwmame#usb-controller-work-in-progress)).
5. **MAME** ([cm0/fe](cm0/fe)): MAME 2003-Plus through a minimal libretro
   frontend. *Running:* Mappy, 1942 and Carnival at 60 fps with whole frames,
   so no compression is needed. Sound plays on the FW2 speaker through
   `fbuf` v003 and the display app. See
   [docs/mame-results.md](docs/mame-results.md). Still to do: a better
   vertical-game display, analog controls, player 2, saving the volume
   level, and the display app holding the CM0's power.

## Repository layout

| Path | Contents |
|---|---|
| [fpga/linktest](fpga/linktest) | Link-test FPGA design and testbench |
| [cm0/linktest.py](cm0/linktest.py) | CM0 side of the link test (pin mapping, SPI and UART senders) |
| [cm0/linktest_job.py](cm0/linktest_job.py), [host/linktest_run.py](host/linktest_run.py) | Unattended full run, CM0 and PC sides |
| [host/linkstat.py](host/linkstat.py) | PC side: reads and controls the design through MAIN |
| [fpga/fastlink](fpga/fastlink) | Fast-link FPGA design (SCLK-clocked SPI receiver) and testbench |
| [cm0/fastlink.c](cm0/fastlink.c) | CM0 sender and UART receiver for the fast link test (C) |
| [host/fastlink_run.py](host/fastlink_run.py), [host/faststat.py](host/faststat.py) | Full fast-link run over SSH + MAIN; reads and controls the design |
| [fpga/fbuf](fpga/fbuf) | Framebuffer FPGA design (SPRAM double buffer, palette, 15 kHz video) and testbench |
| [cm0/fb](cm0/fb) | CM0 framebuffer library (`fwfb.h`) and `fbsend` (test card, images, animation) |
| [host/img2fb.py](host/img2fb.py) | Converts an image to framebuffer pixels + RGB333 palette |
| [fw2/apps/fwmame](fw2/apps/fwmame) | FW2 display app: loads `fbuf`, sends the buttons (built against wilibsp) |
| [tools/gen_fwfpga_header.py](tools/gen_fwfpga_header.py) | Builds an FPGA design and embeds it in a display app |
| [cm0/fe](cm0/fe) | MAME 2003-Plus libretro frontend, `run.sh` game launcher |
| [fw2/apps/fwmame/usbhost](fw2/apps/fwmame/usbhost) | USB host for controllers on the FW2's hub (native USB, low speed) |
| `roms/` | Your game ROMs (git-ignored, never committed) |
| [host/cm0sh.py](host/cm0sh.py), [host/cm0put.py](host/cm0put.py) | Shell commands and file copies to the CM0 through MAIN (fallback) |
| [cm0/usbnet](cm0/usbnet), [cm0/usbnet_test.sh](cm0/usbnet_test.sh) | CM0 USB port as network + COM port (permanent install, live test) |
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

## Reaching the CM0

The FreeWili CM0 image already puts the CM0's USB controller in device mode
(`dtoverlay=dwc2,dr_mode=peripheral`, GPIO2 low, GPIO3 high in config.txt)
and loads `g_serial`, so out of the box the PC sees a COM port with an
autologin `pi` shell (USB ID 0525:a4a7). [cm0/usbnet](cm0/usbnet) replaces
`g_serial` with a composite device that is both:

- **A network adapter** (RNDIS, `usb0`): the CM0 is 10.12.194.1/28 and hands
  the PC an address in 10.12.194.2-14. Its DHCP server sends no gateway and
  no DNS, so the PC never routes internet traffic through the CM0. Microsoft
  OS descriptors let Windows bind an RNDIS driver without an .inf. (The test
  PC bound Intrepid's "neoECU AVB USB RNDIS" driver; a PC without it should
  get Windows' own RNDIS driver, not yet confirmed.)
- **A COM port** (ACM, `ttyGS0`) with the same autologin shell (USB ID
  1d6b:0104).

Install it once, from the COM port or MAIN's Linux menu, then reboot:

```sh
# copy cm0/usbnet to the CM0 (scp, or host/cm0put.py file by file), then on the CM0:
sudo sh usbnet/install.sh                # sudo sh usbnet/install.sh --uninstall undoes it
sudo reboot
```

It edits only `cmdline.txt`, removing `g_serial` from `modules-load`, and
keeps the original as `cmdline.txt.fwmame-orig`. Don't use Raspberry Pi's
`rpi-usb-gadget` on this image: it loads `g_ether` alongside the `g_serial`
in `cmdline.txt` (only one can own the port), and its `off` deletes the
dwc2 line from config.txt. [cm0/usbnet_test.sh](cm0/usbnet_test.sh) runs the
same setup live, without changing anything permanently, and reverts after
10 minutes.

For SSH, put a public key in `~pi/.ssh/authorized_keys` (the image's `pi` has
no known password, but the COM port shell is already logged in):

```sh
ssh -i ~/.ssh/fw2_cm0 pi@10.12.194.1
scp -i ~/.ssh/fw2_cm0 cm0/fastlink.c pi@10.12.194.1:/tmp/
```

## Running MAME

1. **CM0 on first.** MAIN's `l\a` toggles the CM0's power, so check the
   state in MAIN's Linux menu (`l`) before you use it.
2. **Start the display app:** `python <wilibsp>/tools/fw.py run-app
   FW-MAME/fwmame.uf2 --port COM14`. It loads `fbuf` and handles the buttons
   and audio.
3. **On the CM0:** `~/fwmame/run.sh <game>`, with your ROM zips in
   `~/fwmame/roms/`.

The game's picture goes to the CRT and its sound to the FW2's speaker, with
the volume on the FW2's touch screen. Setting up the CM0 side (core, ROMs,
building the frontend) is in [cm0/fe](cm0/fe). Use ROM sets that match MAME
2003-Plus (MAME 0.78 era), not newer MAME sets.

## Running the framebuffer

Connect FW-TPG's ladder board and the monitor to the header. Then, from the
PC:

```sh
(cd wilifpga && tools/fwfpga build fbuf && tools/fwfpga --port COM14 install fbuf)
scp -i ~/.ssh/fw2_cm0 -r cm0/fb pi@10.12.194.1:/tmp/
ssh -i ~/.ssh/fw2_cm0 pi@10.12.194.1 "make -C /tmp/fb && sudo systemctl kill -s KILL fwcm0-bridge && \
    sudo modprobe -r spidev && sudo modprobe spidev bufsiz=65536"
(cd wilifpga && tools/fwfpga --port COM14 load fbuf)
```

On the CM0:

```sh
sudo /tmp/fb/fbsend card                     # test card, 256x224
sudo /tmp/fb/fbsend anim --seconds 30        # bouncing boxes, prints fps and timing
sudo /tmp/fb/fbsend card --size 288x224      # other sizes: any even W x H <= 65536
```

To show your own picture, run `python host/img2fb.py pic.png pic --size 256x224`
on the PC, copy `pic.raw` and `pic.pal` over, then run
`sudo /tmp/fb/fbsend image pic.raw pic.pal`. The FPGA keeps showing the last
frame after `fbsend` exits. To finish, run `tools/fwfpga restore` on the PC
and `sudo systemctl start fwcm0-bridge` on the CM0.

## Running the fast link test

With the CM0 on its USB network and an SSH key for `pi`, one PC script does
everything. It compiles the sender on the CM0, stops `fwcm0-bridge`, pins the
governor, reloads spidev with 64 KB buffers, loads `fastlink`, and runs every
step. Afterwards it restores the stock gateware, the governor and the bridge.
About 3.5 minutes.

```sh
(cd wilifpga && tools/fwfpga build fastlink && tools/fwfpga --port COM14 install fastlink)
python host/fastlink_run.py --port COM14 --log docs/fastlink-run.txt
```

By hand, on the CM0: `sudo /tmp/fastlink send --hz 50000000 --lfsr` (add
`--uart` for both links). On the PC: `python host/faststat.py pattern 0x07`
first, then `python host/faststat.py show`.

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
