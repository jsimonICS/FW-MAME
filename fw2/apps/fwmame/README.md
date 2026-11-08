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

The screen lights each button while it's held. It shows the write count,
failures and write time through MAIN, and on the second line the audio rate,
underruns (U), dropped (D) and repeated (P) samples, resyncs (R) and the worst
read time. The labels over the five colored buttons show FW-MAME's default
mapping (COIN, START, B1, B2, B3), which lives on the CM0. Every button goes to
the game. Holding HOME for 5 s still leaves the app, and holding PAGE for 5 s
shows About.

**Start the CM0 before the app.** While the app runs, MAIN's CM0 power-on
(`l\a`) doesn't stick, but a CM0 that is already running stays on. To be
fixed by having the app hold the CM0's power zone itself.

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
python <wilibsp>/tools/fw.py install-app build-fw2/apps/fwmame/fwmame.uf2 --folder FW-MAME --port COM14
python <wilibsp>/tools/fw.py run-app FW-MAME/fwmame.uf2 --port COM14
```

Use the prebuilt picotool. On this PC, a picotool built from source with
MinGW crashed (access violation) while building the boot stage.

Hardware results: [docs/input-results.md](../../../docs/input-results.md)
(buttons), [docs/mame-results.md](../../../docs/mame-results.md) (audio).
