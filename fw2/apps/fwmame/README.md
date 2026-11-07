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

The screen lights each button while it's held and shows the write count,
failures, and the latest and worst write time through MAIN. The labels over
the five colored buttons show FW-MAME's default mapping (COIN, START, B1, B2,
B3), which lives on the CM0. Every button goes to the game. Holding HOME for
5 s still leaves the app, and holding PAGE for 5 s shows About.

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

Hardware results: [docs/input-results.md](../../../docs/input-results.md).
