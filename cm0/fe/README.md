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
  | Nav center | X | button 4 |
  | OK (✓) / CANCEL (✗) | sent as Green / Blue by the display app | buttons 1 / 2 |

- **Game menu** (`--menu <romdir>`, what `run.sh` does with no game): a
  supervisor publishes the zips in the folder as the game list in the FPGA's
  menu memory (`fwfb_menu()`, fbuf v004), which the FW2 display app shows.
  While it waits for the app to request a game (input byte 6 = 1..63), the
  CRT shows the marquee of the game under the FW2's cursor (byte 6 =
  `0x80 | game`): `<romdir>/../marquees/<rom>.fmq` (from
  `tools/marquees.py build` and `install-cm0`) in a 320x200 picture, black
  for a game without one. It then runs the game as a child process (`--menu-id n`), which exits
  when the request changes (HOME on the FW2, or another game). A game that
  fails is reported in the list header (byte 3) and not retried until it's
  picked again. Each game runs in a fresh process, so the core never has to
  unload and reload cleanly.

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
~/fwmame/run.sh                        # game menu: pick games on the FW2 screen
~/fwmame/run.sh mappy                  # or one game; Ctrl+C to stop; --seconds N, --dump N
```

`run.sh` stops fwcm0-bridge, reloads spidev with 64 KB buffers and sets the
performance governor, none of which survives a reboot, then starts the game.
The FW2 display app must be running, so `fbuf` is loaded. Games with
recorded sound effects need their sample pack in
`~/fwmame/system/mame2003-plus/samples/`.

`--dump N` writes frame N as `/tmp/fwmame_core.ppm` (the core's image) and
`/tmp/fwmame_sent.ppm` (what was sent), for checking colors without the CRT.
Results: [docs/mame-results.md](../../docs/mame-results.md).
