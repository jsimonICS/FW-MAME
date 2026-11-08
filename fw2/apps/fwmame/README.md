# fwmame (FW2 display app)

The FW-MAME console app for the FreeWili 2's display CPU. It:

- selects the 3.3 V VIO rail and holds power zone 6 (FPGA);
- writes the embedded `fbuf` gateware to `/fpga/fbuf.fwfpga` if the SD card
  doesn't already have this build's, and loads it unless it's already
  running. Every 2 s it checks the design is still loaded, so it recovers
  after MAIN reboots or zone 6 drops;
- forwards the 14 buttons to the CM0. Every change, and every 500 ms, is one
  `h\f\w 30 <8 bytes>` to FPGA registers 0x30-0x37, which the FPGA sends to
  the CM0 every field. The layout is in
  [cm0/fb/fwfb.h](../../../cm0/fb/fwfb.h): bytes 0-1 hold the buttons (bit n =
  `UARTKBD_BTN_n`), and byte 7 is an update counter.

- plays the game's audio (v003, `fbuf` v003). It pulls the samples the CM0
  sends from the FPGA's stream FIFO through MAIN (`h\f\r 64` count, `h\f\x n`),
  every 4 ms. It plays them at 16 kHz on the NAU88C10 and the speaker from a
  looping DMA ring, about 75 ms ahead. One sample in 32 is dropped or repeated
  to follow the CM0's rate. The VOL-/VOL+ touch buttons set 11 levels (0 mute
  to 10 = unity gain, no boost for the 0.5 W speaker). The level resets to 6
  when the app starts.
- reads a USB joystick or pad on the FW2's USB host ports (work in progress,
  see below).

- shows the **game menu** (v004, `fbuf` v004). The CM0's supervisor (`run.sh`
  with no game) publishes its ROM list in the FPGA's menu memory. The app reads
  it (`h\f\w 39 <page>`, then `h\f\r 40 32`) and lists the games:
  - **In the menu:** nav up/down moves (left/right jumps 5 games), and **OK**
    (✓) plays the selected game. Its number goes in input byte 6, and the CM0
    starts it.
  - **In a game:** **HOME** stops it and returns to the menu. Holding HOME for
    5 s still leaves the app. **OK** (✓) also presses **B1**, and **CANCEL**
    (✗) also presses **B2**.
  - A game the CM0 can't start (a ROM set that doesn't match) shows as
    "<GAME> FAILED", and the menu stays.

From the top, the screen has:
- **The marquee** of the game under the cursor (or the one playing), with
  the state line in small text under it. A game without a marquee shows its
  name. Tap **PAGE** to switch this band to the five status lines and back.
  The status lines also show while the app starts and while it learns a USB
  stick.
- The game list.
- VOL-/VOL+ (and LEARN STICK when a HID stick is plugged in).
- The five colored buttons, lit while held. Their labels show FW-MAME's
  default mapping (COIN, START, B1, B2, B3), which lives on the CM0.

Status lines: FPGA state; audio (L = level, then rate, underruns U, dropped D
and repeated P samples, resyncs R, worst read time); input writes; USB; raw
report or learn prompt. Holding PAGE for 5 s shows About.

**Starting from the Apps menu:** the app switches the CM0's Linux on if it is
off (MAIN's `l\a`, only after MAIN's shell answers `EPOWERZONE`) and starts
`fwmame.service` through MAIN's Linux shell. It never touches power zone 17:
that rail is MAIN's (wilibsp `docs/drivers/power.md`). On a cold CM0 this can
take a few minutes: MAIN's shell sometimes returns no output for a while.

## Marquees

The marquee of the game under the cursor shows on the FW2 screen and, while
the menu is up, full width on the CRT. `tools/marquees.py` makes both sets
from `roms/marquees/<name>.png` (the parent set's name works for a clone):

```sh
python tools/marquees.py build        # roms/*.zip -> build-marquees/{fw2,crt}/*.fmq (needs Pillow)
python tools/marquees.py install --wilibsp <wilibsp> --port COM14   # FW2 SD card
python tools/marquees.py install-cm0  # CRT set -> CM0 ~/fwmame/marquees (scp)
```

Quit FW-MAME on the FW2 before `install`: it hands the SD card to the PC.
The FW2 set goes to `/appdata/fwmame/marquees/<rom>.fmq` on the SD card. For
the CRT, the app sends the cursor's game in input byte 6 (`0x80 | game` while
in the menu) and the CM0's menu supervisor draws `~/fwmame/marquees/<rom>.fmq`
in a 320x200 picture: up to 320x200, dithered to one fixed 256-colour
palette that the FPGA's RGB333 palette shows exactly.
Each file is scaled to fit 472x100, in 256 colours, run-length compressed
(13-39 KB). Art that isn't a banner (Mappy's is cabinet art) is cropped to a
band from its top. The app reads the SD card through MAIN in small pieces
from its main loop: the marquee under the cursor first, then the rest while
the menu is idle, keeping each in PSRAM. Nothing is read while a game runs,
since MAIN's link carries the game's audio then.

## USB controller (work in progress)

The FW2's external USB host ports sit behind an on-board CH334F hub
(`1A86:8091`, 4 ports) on the display CPU's **native** USB controller.
wilibsp's PIO-USB host uses GPIO42/43, which the FW2 wires as a USB *device*
port, so it never sees these ports. wilibsp's native host stack only takes
full-speed mass-storage devices. [usbhost/](usbhost) is the app's own:

- [hh_hcd.c](usbhost/hh_hcd.c) is wilibsp's native controller driver (MIT),
  renamed `hh_*`, with two changes. PRE packets for low-speed devices behind
  the hub. RX timeouts are retried by the SIE (as in TinyUSB) instead of being
  treated as fatal.
- [hh_usb.c](usbhost/hh_usb.c) brings up the hub, enumerates any device,
  finds a HID or Xbox 360-style XInput interface with an interrupt IN
  endpoint, and polls it.
- `main.c` maps XInput pads directly, and HID sticks through a map learned on
  the FW2 and saved per VID:PID in `/appdata/fwmame/`.

**Verified:** a low-speed HID controller (`050D:0803`) enumerates through the
hub, and its 7-byte reports arrive.

**Not finished:** learn mode. That stick answers GET_REPORT with 3 bytes in
another layout, so the rest state must come from an interrupt report. A hat
switch's "up" (8 → 0) is a one-bit change, so it must be tested before
single-bit buttons. Both fixes are in the source but untested. The user's
arcade stick hasn't been tried on the native host yet.

## Build and install

Needs the Pico SDK 2.3.0, the Arm GNU toolchain 14.2, Ninja, the wilifpga
toolchain (the app embeds `fbuf`, built at app build time), and a wilibsp
checkout with `libs/onewili` initialized. By default it's looked for at
`<repo>/wilibsp`; pass `-DWILIBSP_DIR=` to use another checkout.

```sh
cmake -S fw2 -B build-fw2 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWILIBSP_DIR=<wilibsp> -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 \
  -DPICO_TOOLCHAIN_PATH="<Arm GNU Toolchain 14.2 rel1>" \
  -Dpicotool_DIR=~/.pico-sdk/picotool-prebuilt/picotool
cmake --build build-fw2 --target fwmame
python <wilibsp>/tools/fw.py install-app build-fw2/apps/fwmame/fwmame.uf2 --folder Games --port COM14
python <wilibsp>/tools/fw.py run-app Games/fwmame.uf2 --port COM14
```

Use the prebuilt picotool. On this PC, a picotool built from source with
MinGW crashed (access violation) while building the boot stage.

Hardware results: [docs/input-results.md](../../../docs/input-results.md)
(buttons), [docs/mame-results.md](../../../docs/mame-results.md) (audio).
