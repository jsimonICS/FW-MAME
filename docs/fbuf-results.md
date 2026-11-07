# Framebuffer results, 2026-11-07

**Setup:**
- FreeWili 2 with MAIN firmware v11, FW-TPG's ladder board on the header,
  15 kHz arcade CRT.
- `fpga/fbuf` v001. The CM0 runs [cm0/fb/fbsend](../cm0/fb/fbsend.c) over SSH
  (fwcm0-bridge stopped, spidev `bufsiz=65536`, performance governor), with
  SPI at 50 MHz.

## Results

| Test | Result |
|---|---|
| Test card, 256×224 (CPP 5) | Correct on the CRT: bars, grey ramp, hues, grid, border, circle |
| Frame transfer, 256×224 | 57,344 bytes in 9.3 ms = 6.1 MB/s |
| Animation, 256×224, 30 s | 1,802 frames in 30.01 s = 60.05 fps over 1,802 fields; 0 repeated, 0 flip timeouts. Smooth on the CRT |
| Animation, 288×224 (CPP 4) | 60.07 fps over 5 s, 0 repeated; 64,512-byte frames in 10.5 ms |
| Animation, 320×200 (CPP 4) | 60.07 fps over 5 s, 0 repeated |
| Animation, 256×240 | 60.07 fps over 5 s, 0 repeated |
| FPGA counters after card + 5 s animation | 616 packets, 0 unknown, 0 write overruns, 0 fetch underruns |

Each animation frame is sent whole (9.3 ms), then FLIP waits for the next
vertical blank (7.4 ms on average). The display runs at its own 60.05 Hz,
and the CM0 locks to it through the status bytes.

## What it means

- **The framebuffer works end to end at full frame rate** with whole frames
  sent every field. So MAME doesn't need dirty-line or palette compression to
  keep up. It just has to fit its emulation into the remaining ~7 ms of each
  frame, or overlap emulation with sending.
- **The flip wait is idle time on the CM0.** For MAME, the sender should run
  in its own thread: the emulator writes frame N+1 while frame N is sent and
  flipped.
- **Vertical games** (224×288 and similar) need 288 lines, more than the
  241 a 15 kHz field shows. Rotated to 288×224 they fit (tested above), or
  MAME can scale them down.
