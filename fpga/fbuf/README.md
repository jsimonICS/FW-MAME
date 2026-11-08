# fbuf

FW-MAME milestone 3: the framebuffer. The CM0 sends frames over SPI, and the
FPGA keeps two frame buffers in its 128 KB SPRAM, maps each 8-bit pixel
through a 256-entry palette to RGB333, and drives a 15 kHz arcade monitor.
The packet format, timing and register map are in the header of
[user_core.v](user_core.v). The CM0 library is [cm0/fb/fwfb.h](../../cm0/fb/fwfb.h).

- **Video:** FW-TPG's timing: 25 MHz clock, 1589-clock lines (15.73 kHz),
  262-line fields (60.05 Hz), RS-170 equalizing and serrated vsync,
  active-low CSYNC. Same header pins and ladder board as
  [FW-TPG](https://github.com/jsimonICS/FW-TPG).
- **Picture:** any even width × height up to 65,536 pixels (256×224,
  256×240, 288×224, 320×200), each pixel 4-8 clocks wide (160-320 ns),
  placed anywhere in the 1316 × 241 active area. Black outside it.
- **Link:** SPI receiver clocked by the CM0's SCLK (as in
  [fastlink](../fastlink)), 16-bit words, chip select (GPIO21) framing each
  packet. Packets: PIXELS, PALETTE, FLIP, SETREG.
- **Double buffering:** FLIP swaps the buffers in vertical blank. One status
  byte per field, plus one after each flip, goes back to the CM0 on the UART
  (6.25 Mbaud), so the CM0 knows when the flip happened and can pace itself
  to the display.
- **Resources:** 37% of the logic, 1 block RAM (palette), all 4 SPRAMs.
  Timing met at 31.9 MHz (design clock) and 133 MHz (SCLK domain).
- **Simulation:** `tools/fwfpga sim fbuf` (about 4 minutes) plays the CM0.
  It sends geometry, a palette and frames at 50 and 66.7 MHz SCLK, and checks
  every pixel of a 16 × 8 window on the pins against a model of both buffers
  over six fields. It also covers flips sent mid-picture and during vblank,
  partial updates, unknown and odd-length packets, the test ramp, hsync
  width, the status bytes and the counters.

**v003 adds audio.** A new SPI packet, AUDIO (0x05), carries samples. The
design pushes their bytes into the shell's 4 KB stream FIFO (the `STREAM`
capability), where the FW2 display app reads them through MAIN and plays them.
Registers 0x3A-0x3F count bytes pushed and dropped. The samples are 14-bit,
in self-marking byte pairs, so a byte dropped by a full FIFO costs one sample
rather than the alignment ([cm0/fb/fwfb.h](../../cm0/fb/fwfb.h)). 51% of the
logic, 9 block RAMs, timing met at 32.9 MHz. On hardware: 16,000 samples/s
with no losses, and 1942's music and effects on the FW2 speaker
([docs/mame-results.md](../../docs/mame-results.md)).

**v002 (milestone 4) adds inputs.** MAIN writes 8 input bytes to registers
0x30-0x37, committed together on the write to 0x37. The status message to
the CM0 grows from 1 byte to an 11-byte packet: a status byte with bit 7 set,
then the 64 input bits, 7 per byte. It's sent at every vblank start and after
every flip. The display app [fw2/apps/fwmame](../../fw2/apps/fwmame) writes
the inputs and loads this design. On hardware, all 14 FW2 buttons reached the
CM0 ([docs/input-results.md](../../docs/input-results.md)). It uses 42% of the
logic and meets timing at 33.4 MHz.

**Hardware, 2026-11-07:** test card and a double-buffered animation on the
CRT, confirmed good by eye. 256×224 frames take 9.3 ms at 6.1 MB/s (50 MHz
SPI). The animation runs at 60.05 fps, a new frame every field, with no
repeated fields over 1,802 frames. 288×224, 320×200 and 256×240 also run at
60 fps. Details: [docs/fbuf-results.md](../../docs/fbuf-results.md).
