// fwfb.h - CM0 side of the FW-MAME framebuffer (fpga/fbuf).
//
// Frames go to the FPGA over SPI0 (spidev, mode 0) as packets framed by chip
// select on GPIO21. The FPGA answers on the UART (/dev/ttyAMA0, 6.25 Mbaud)
// with an 11-byte packet at every vblank start and after every flip: a status
// byte (bit 7 set; bit 6 front buffer, bit 5 flip pending, bits 4:0 field
// count) and the 8 input bytes below. Needs root (/dev/gpiomem) and
// fwcm0-bridge stopped (it owns SPI0, the UART and GPIO21). Formats:
// fpga/fbuf/user_core.v (fbuf v002 or newer).
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { FWFB_WIDTH = 0, FWFB_HEIGHT = 1, FWFB_XSTART = 2, FWFB_YSTART = 3, FWFB_CPP = 4, FWFB_CONTROL = 5 };

// Input state, 8 bytes, written by the FW2 display app (fw2/apps/fwmame):
//   bytes 0-1  FW2 buttons, a set bit = held (FWFB_BTN_*)
//   bytes 2-3  USB controller buttons (FWFB_PAD_*), 0 when none is plugged in
//   bytes 4-5  USB controller left stick X, Y (int8, 0 = centre), 0 when none
//   byte 6     reserved, 0
//   byte 7     update counter: the app adds 1 on every write
enum {
  FWFB_BTN_GREY = 0, FWFB_BTN_YELLOW, FWFB_BTN_GREEN, FWFB_BTN_BLUE, FWFB_BTN_RED,
  FWFB_BTN_NAV_CENTER, FWFB_BTN_NAV_UP, FWFB_BTN_NAV_DOWN, FWFB_BTN_NAV_LEFT, FWFB_BTN_NAV_RIGHT,
  FWFB_BTN_HOME, FWFB_BTN_OK, FWFB_BTN_CANCEL, FWFB_BTN_PAGE, FWFB_BTN_COUNT
};
enum {
  FWFB_PAD_UP = 0, FWFB_PAD_DOWN, FWFB_PAD_LEFT, FWFB_PAD_RIGHT, FWFB_PAD_A, FWFB_PAD_B, FWFB_PAD_X,
  FWFB_PAD_Y, FWFB_PAD_L, FWFB_PAD_R, FWFB_PAD_SELECT, FWFB_PAD_START, FWFB_PAD_COUNT
};

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
// Audio for the FW2's speaker: 16-bit signed mono samples at 16 kHz, into the
// FPGA's stream FIFO (fbuf v003+), which the FW2 display app reads and plays.
// Send about 16000 samples per second (267 per 60 Hz frame); the app evens
// out small rate differences. On the wire each sample is cut to 14 bits and
// sent as two self-marking bytes: low = bits 6:0 << 1 (bit 0 = 0), high =
// bits 13:7 << 1 | 1. The FIFO drops single bytes when it is full; with the
// marks the app re-locks on the next byte instead of turning the rest into noise.
int fwfb_audio(fwfb *fb, const int16_t *samples, size_t n);
// Show the back buffer: sends FLIP and waits until the FPGA reports it done
// (at the next vertical blank, at once if already in it). Returns 0, or -1 on timeout.
int fwfb_flip(fwfb *fb);
// Waits for the next whole packet (a vblank start or a flip). Returns its
// status byte, or -1 on timeout.
int fwfb_wait_status(fwfb *fb, int timeout_ms);
// Reads whatever packets have arrived, without waiting. Returns the number of whole packets.
int fwfb_poll(fwfb *fb);
// Status byte of the last whole packet, or -1.
int fwfb_status(const fwfb *fb);
// Input bytes of the last whole packet (all 0 before the first).
void fwfb_inputs(const fwfb *fb, uint8_t out[8]);
// RGB888 -> RGB333
static inline uint16_t fwfb_rgb(int r, int g, int b) {
  return (uint16_t)(((r >> 5) << 6) | ((g >> 5) << 3) | (b >> 5));
}
