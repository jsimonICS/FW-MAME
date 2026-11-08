# MAME and audio results, 2026-11-07/08

**Setup:**
- FreeWili 2, MAIN firmware v11. The FW2 display app [fw2/apps/fwmame](../fw2/apps/fwmame)
  v003 loads `fpga/fbuf` v003, forwards the buttons and plays the audio.
- On the CM0, the libretro frontend [cm0/fe](../cm0/fe) runs the prebuilt
  aarch64 MAME 2003-Plus core from the libretro buildbot (git `dff0ba6`),
  started with `~/fwmame/run.sh <game>`.
- Games are the user's own ROM zips (kept out of git, `roms/` is ignored).
- SPI at 50 MHz, CPU governor set to performance.

## Games

| Game | Loads | Picture | Frame rate | Core time per frame | Colors | Sound |
|---|---|---|---|---|---|---|
| Mappy | yes | 224×288, scaled to 224×241 | 60.0 fps over 10 min, 0 flip timeouts | ~3 ms | 19 | Music and effects. 15,854 samples/s (paced to 60.05 Hz, not the game's 60.61 Hz) |
| 1942 | yes | 224×256, scaled to 224×241 | 60.1 fps over 10 min, 0 flip timeouts | 2.5 ms | 38 | Music, gunfire, explosions, confirmed by ear |
| Carnival | yes | 224×256, scaled to 224×241 | 60.1 fps | 1.7 ms | 6 | Music only: the effects are recorded samples (`samples/carnival.zip`), which weren't installed |
| Donkey Kong, Frogger, Galaga | no | | | | | ROM sets from a newer MAME: missing files (`c-2k.bpr`, `frogger.26`, `gg1-1b.3p`, and others) |

Per frame the sender thread spends 0.9 ms converting to 8-bit pixels and
8.8 ms sending, then waits about 6.7 ms for the flip. It shows a new frame
every field. The emulator thread uses 2-3 ms of each 16.7 ms frame, so the
CPU has plenty of headroom. On the CRT the colors were confirmed good, and
coin, start and joystick reached the game. A coin injected through the FW2
display app showed "CREDIT 1" in a dumped frame.

## Audio path

CM0 frontend (MAME's stereo samples resampled to 16 kHz mono) → AUDIO SPI
packets → `fbuf` v003 pushes the bytes into the shell's 4 KB stream FIFO →
the display app reads it through MAIN (`h\f\r 64` for the count, `h\f\x n`
for up to 256 bytes as hex) → a DMA ring plays it on the NAU88C10 at 16 kHz.

| Check | Result |
|---|---|
| Throughput, 440 Hz test tone (`fbsend tone`) | 31,976 bytes/s = 16,000 samples/s; `h\f\x` 1 ms typical, 5-6 ms worst |
| Lost bytes with self-marking samples | 0 resyncs in every run since the fix |
| Rate matching | The FW2 codec runs ~0.3% slow against the CM0's clock. The app drops (or repeats) one sample in 32 while the buffer is outside 37-150 ms |
| Input writes alongside | Unaffected (1-2 ms, no failures) |
| Volume | VOL-/VOL+ touch buttons, 11 levels, unity gain at the top (no boost), 250 ms debounce |

## Problems found and fixed

- **Only red and black, no sprites:** a libretro frontend must answer
  `GET_VARIABLE` with each option's default from `SET_VARIABLES`. Answering
  "no value" broke MAME 2003-Plus's video options. The frontend now records
  the defaults.
- **Static instead of sound:** the stream FIFO drops single bytes when full,
  for example while the app starts. With plain 16-bit samples every later
  sample was misaligned. Now each sample is 14 bits in two self-marking bytes
  (low byte bit 0 = 0, high byte bit 0 = 1), so the app re-locks on the next
  byte. The app also flushes the FIFO when its audio starts.
- **Buttons ignored, game looked slow:** with `fbuf` loaded, fwcm0-bridge
  can't reach its FPGA mailbox and systemd restarts it every 5 s. Each restart
  grabbed the UART and reset SPI0 and GPIO21. `run.sh` now always stops it,
  even when systemd reports it as "activating".

## Open

- **The display app stops MAIN from powering the CM0 on** (`l\a` doesn't stick
  while the app runs). Start the CM0 first, then the app. A CM0 that is already
  running stays on.
- **Vertical games are squeezed** from 288 (or 256) lines to 241, which damages
  small text. Cropping or a sideways mode would avoid it.
- **No analog inputs and no player 2 yet.**
- **The volume level isn't saved.**
- **`fwfpga reg r/w` reads a bare number as decimal.** Write `0x18`, not `18`.
