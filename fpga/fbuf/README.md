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

**Hardware, 2026-11-07:** test card and a double-buffered animation on the
CRT, confirmed good by eye. 256×224 frames take 9.3 ms at 6.1 MB/s (50 MHz
SPI). The animation runs at 60.05 fps, a new frame every field, with no
repeated fields over 1,802 frames. 288×224, 320×200 and 256×240 also run at
60 fps. Details: [docs/fbuf-results.md](../../docs/fbuf-results.md).
