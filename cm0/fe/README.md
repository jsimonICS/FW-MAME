# cm0/fe: MAME 2003-Plus frontend

A minimal libretro frontend that runs a core on the CM0 and shows it through
the FW-MAME framebuffer ([cm0/fb](../fb), [fpga/fbuf](../../fpga/fbuf)):

- **Video:** the emulator thread runs `retro_run()` once per frame. The sender
  thread maps each frame to 8-bit pixels through a palette of up to 256 RGB333
  colors, sends it over SPI and flips at the next vblank. Colors are only ever
  added to the palette, never changed while on screen. Frames larger than
  320×241 are scaled down by dropping rows or columns.
- **Input:** the FW2 buttons and a USB pad's bits (from the FPGA's per-field
  packet) map to RetroPad player 1:

  | FW2 button | RetroPad | MAME 2003-Plus |
  |---|---|---|
  | Nav pad | d-pad | joystick |
  | Grey / Yellow | SELECT / START | coin / start |
  | Green / Blue / Red | B / A / Y | buttons 1 / 2 / 3 |
  | Nav center / OK / CANCEL | X / L / R | buttons 4 / 5 / 6 |

- **Audio:** the core's stereo samples are resampled to 16 kHz mono and sent
  before each frame. The FW2 display app plays them (fbuf v003+). The core is
  asked for 22,050 Hz.
- **Options:** every core option answers with its default, except
  `skip_disclaimer`, `skip_warnings` and `sample_rate` (see `options[]`).

## Install and run

```sh
# on the PC
scp -i ~/.ssh/fw2_cm0 -r cm0/fe cm0/fb pi@10.12.194.1:fwmame/src/
scp -i ~/.ssh/fw2_cm0 cm0/fe/run.sh pi@10.12.194.1:fwmame/run.sh
scp -i ~/.ssh/fw2_cm0 roms/mappy.zip pi@10.12.194.1:fwmame/roms/
# the core: mame2003_plus_libretro.so from
# buildbot.libretro.com/nightly/linux/aarch64/latest/, into ~/fwmame/cores/

# on the CM0
make -C ~/fwmame/src/fe
~/fwmame/run.sh mappy                  # Ctrl+C to stop; --seconds N, --dump N
```

`run.sh` stops fwcm0-bridge, reloads spidev with 64 KB buffers and sets the
performance governor, none of which survives a reboot, then starts the game.
The FW2 display app must be running, so `fbuf` is loaded. Games with
recorded sound effects need their sample pack in
`~/fwmame/system/mame2003-plus/samples/`.

`--dump N` writes frame N as `/tmp/fwmame_core.ppm` (the core's image) and
`/tmp/fwmame_sent.ppm` (what was sent), for checking colors without the CRT.
Results: [docs/mame-results.md](../../docs/mame-results.md).
