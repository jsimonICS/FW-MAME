// fwfb.h - CM0 side of the FW-MAME framebuffer (fpga/fbuf).
//
// Frames go to the FPGA over SPI0 (spidev, mode 0) as packets framed by chip
// select on GPIO21; the FPGA answers on the UART (/dev/ttyAMA0, 6.25 Mbaud)
// with one status byte per field and after every flip:
//   bit 7 front buffer, bit 6 flip pending, bits 5:0 field count.
// Needs root (/dev/gpiomem) and fwcm0-bridge stopped (it owns SPI0, the UART
// and GPIO21). Packet format: fpga/fbuf/user_core.v.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { FWFB_WIDTH = 0, FWFB_HEIGHT = 1, FWFB_XSTART = 2, FWFB_YSTART = 3, FWFB_CPP = 4, FWFB_CONTROL = 5 };

typedef struct fwfb fwfb;

// Opens the link at spi_hz (0 = 50 MHz). Returns NULL and prints why on failure.
fwfb *fwfb_open(long spi_hz);
void fwfb_close(fwfb *fb);

// Set a geometry register (FWFB_*). Takes effect at the next field.
int fwfb_setreg(fwfb *fb, int reg, int value);
// Width x height pixels, centered, with pixel width cpp (0 = widest that fits, 4-8).
int fwfb_mode(fwfb *fb, int width, int height, int cpp);
// Palette entries first..first+n-1, each {R[2:0], G[2:0], B[2:0]} in bits 8:0.
int fwfb_palette(fwfb *fb, int first, int n, const uint16_t *rgb333);
// Pixels into the back buffer at byte address addr (even); n is rounded down to even.
int fwfb_pixels(fwfb *fb, unsigned addr, const uint8_t *px, size_t n);
// Show the back buffer: sends FLIP and waits until the FPGA reports it done
// (at the next vertical blank, at once if already in it). Returns 0, or -1 on timeout.
int fwfb_flip(fwfb *fb);
// Waits for the next status byte (a vblank start or a flip). Returns it, or -1 on timeout.
int fwfb_wait_status(fwfb *fb, int timeout_ms);
// Last status byte seen, or -1.
int fwfb_status(const fwfb *fb);
// RGB888 -> RGB333
static inline uint16_t fwfb_rgb(int r, int g, int b) {
  return (uint16_t)(((r >> 5) << 6) | ((g >> 5) << 3) | (b >> 5));
}
