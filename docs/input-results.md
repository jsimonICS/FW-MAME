# Input results, phase 1 (FW2 buttons), 2026-11-07

**Setup:**
- FreeWili 2 with MAIN firmware v11.
- The display app [fw2/apps/fwmame](../fw2/apps/fwmame) v001, installed at
  `/apps/FW-MAME/fwmame.uf2` and started with `fw run-app`.
- `fpga/fbuf` v002, loaded by the app itself: on its first run the app wrote
  the embedded gateware to `/fpga/fbuf.fwfpga` and loaded it.
- On the CM0, `fbsend input` (over SSH) printed every change and drew the
  controls on the CRT.

**Path:** button → display app (`uartkbd` events) → one `h\f\w 30 <8 bytes>`
through MAIN → FPGA registers 0x30-0x37 → the 11-byte UART packet sent every
field → `fwfb` on the CM0.

## Results

| Check | Result |
|---|---|
| Link with no buttons pressed, 45 s | 2,612 packets (58/s, one per field). The app's update counter arrived every 500 ms |
| All 14 buttons, pressed by hand, 120 s | Every button seen with press and release. Presses per button: grey 2, yellow 2, green 2, blue 2, red 12, nav centre 2, up 23, down 23, left 13, right 38, HOME 4, OK 2, CANCEL 15, PAGE 8 |
| Several buttons held at once | Correct, for example nav centre + up + right held, then released one at a time |
| Lost events | None: the update counter steps by one per change, so every event became one write and arrived |
| Writes through MAIN (app's own count) | 1,434 writes, 0 failures, 1 ms latest, 2 ms worst |
| Packets during the test | 7,021 in 120 s, none missing |
| Without a debug probe | `fw press` (agentio) needs the CMSIS-DAP probe, and none was attached, so the buttons were pressed by hand |

## What it means

- **Input lag is at most ~2 ms + one field.** The app's write through MAIN
  takes 1-2 ms. The FPGA sends the inputs to the CM0 once per field (every
  16.7 ms), the same rate at which MAME reads inputs, so the effective delay
  is less than one frame.
- **No CM0 input daemon is needed.** The inputs arrive in the same UART packet
  the framebuffer already uses for vsync, so the MAME frontend gets them from
  `fwfb_inputs()`.
- **The FW2 is now a standalone console:** starting the app brings up the
  FPGA design and the inputs. The CM0 only has to send frames.
