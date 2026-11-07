// fwfb.c - CM0 side of the FW-MAME framebuffer. See fwfb.h.
#define _GNU_SOURCE
#include "fwfb.h"

#include <asm/termbits.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CS_GPIO 21
#define ACT_CLOCKS 1316          // active line, 40 ns clocks
#define ACT_LINES 241

struct fwfb {
  int spi, uart;
  volatile uint32_t *gpio;
  uint32_t hz;
  size_t max_pkt;                // spidev bufsiz
  uint8_t *tx;
  int status;                    // last whole packet
  uint8_t inputs[8];
  int pk_n;                      // input bytes of the current packet so far, -1 = not in one
  int pk_status;
  uint8_t pk_raw[10];
};

static int fail(const char *what) {
  fprintf(stderr, "fwfb: %s: %s\n", what, strerror(errno));
  return -1;
}

static long spidev_bufsiz(void) {
  FILE *f = fopen("/sys/module/spidev/parameters/bufsiz", "r");
  long v = 4096;
  if (f) {
    if (fscanf(f, "%ld", &v) != 1) v = 4096;
    fclose(f);
  }
  return v;
}

static void cs(fwfb *fb, int high) {
  fb->gpio[high ? 7 : 10] = 1u << CS_GPIO;     // GPSET0 / GPCLR0
  (void)fb->gpio[13];                           // GPLEV0 read: post the write
}

static int send_packet(fwfb *fb, size_t len) {
  struct spi_ioc_transfer x;
  memset(&x, 0, sizeof x);
  x.tx_buf = (uintptr_t)fb->tx;
  x.len = (uint32_t)(len & ~(size_t)1);
  x.speed_hz = fb->hz;
  x.bits_per_word = 8;
  cs(fb, 0);
  int r = ioctl(fb->spi, SPI_IOC_MESSAGE(1), &x);
  cs(fb, 1);
  (void)fb->gpio[13];                           // keep CS high a little before the next packet
  return r < 0 ? fail("SPI_IOC_MESSAGE") : 0;
}

static void header(fwfb *fb, int cmd, int arg, unsigned param) {
  fb->tx[0] = (uint8_t)cmd;
  fb->tx[1] = (uint8_t)arg;
  fb->tx[2] = (uint8_t)param;
  fb->tx[3] = (uint8_t)(param >> 8);
}

fwfb *fwfb_open(long spi_hz) {
  fwfb *fb = calloc(1, sizeof *fb);
  if (!fb) return NULL;
  fb->hz = spi_hz > 0 ? (uint32_t)spi_hz : 50000000u;
  fb->status = -1;
  fb->pk_n = -1;
  fb->max_pkt = (size_t)spidev_bufsiz();
  fb->tx = malloc(fb->max_pkt);

  int g = open("/dev/gpiomem", O_RDWR | O_SYNC);
  if (g < 0) { fail("/dev/gpiomem (run as root)"); goto bad; }
  fb->gpio = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, g, 0);
  close(g);
  if (fb->gpio == MAP_FAILED) { fail("mmap gpiomem"); goto bad; }
  fb->gpio[7] = 1u << CS_GPIO;                                   // CS high, then output
  fb->gpio[2] = (fb->gpio[2] & ~(7u << 3)) | (1u << 3);

  fb->spi = open("/dev/spidev0.0", O_RDWR);
  if (fb->spi < 0) { fail("/dev/spidev0.0"); goto bad; }
  uint8_t mode = SPI_MODE_0, bits = 8;
  if (ioctl(fb->spi, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(fb->spi, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ioctl(fb->spi, SPI_IOC_WR_MAX_SPEED_HZ, &fb->hz) < 0) { fail("spidev setup"); goto bad; }

  fb->uart = open("/dev/ttyAMA0", O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fb->uart < 0) { fail("/dev/ttyAMA0"); goto bad; }
  struct termios2 t;
  if (ioctl(fb->uart, TCGETS2, &t) < 0) { fail("TCGETS2"); goto bad; }
  t.c_iflag = t.c_oflag = t.c_lflag = 0;
  t.c_cflag &= ~(CBAUD | CSIZE | PARENB | CSTOPB | CRTSCTS);
  t.c_cflag |= BOTHER | CS8 | CREAD | CLOCAL;
  t.c_ispeed = t.c_ospeed = 6250000;
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 0;
  if (ioctl(fb->uart, TCSETS2, &t) < 0) { fail("TCSETS2"); goto bad; }
  ioctl(fb->uart, TCFLSH, TCIFLUSH);
  if (fwfb_wait_status(fb, 100) < 0) {
    fprintf(stderr, "fwfb: no status byte from the FPGA in 100 ms (is fbuf loaded?)\n");
    goto bad;
  }
  return fb;
bad:
  fwfb_close(fb);
  return NULL;
}

void fwfb_close(fwfb *fb) {
  if (!fb) return;
  if (fb->spi > 0) close(fb->spi);
  if (fb->uart > 0) close(fb->uart);
  if (fb->gpio && fb->gpio != MAP_FAILED) munmap((void *)fb->gpio, 4096);
  free(fb->tx);
  free(fb);
}

int fwfb_setreg(fwfb *fb, int reg, int value) {
  header(fb, 0x04, reg, (unsigned)value);
  return send_packet(fb, 4);
}

int fwfb_mode(fwfb *fb, int width, int height, int cpp) {
  if (cpp <= 0) cpp = ACT_CLOCKS / (width > 0 ? width : 1);
  if (cpp < 4) cpp = 4;
  if (cpp > 8) cpp = 8;
  int x = (ACT_CLOCKS - width * cpp) / 2, y = (ACT_LINES - height) / 2;
  return fwfb_setreg(fb, FWFB_WIDTH, width) | fwfb_setreg(fb, FWFB_HEIGHT, height) |
         fwfb_setreg(fb, FWFB_CPP, cpp) | fwfb_setreg(fb, FWFB_XSTART, x < 0 ? 0 : x) |
         fwfb_setreg(fb, FWFB_YSTART, y < 0 ? 0 : y);
}

int fwfb_palette(fwfb *fb, int first, int n, const uint16_t *rgb333) {
  header(fb, 0x02, 0, (unsigned)first);
  if (n > 256) n = 256;
  for (int i = 0; i < n; i++) {
    fb->tx[4 + 2 * i] = (uint8_t)rgb333[i];
    fb->tx[5 + 2 * i] = (uint8_t)(rgb333[i] >> 8) & 1;
  }
  return send_packet(fb, 4 + 2 * (size_t)n);
}

int fwfb_pixels(fwfb *fb, unsigned addr, const uint8_t *px, size_t n) {
  size_t chunk = (fb->max_pkt - 4) & ~(size_t)1;
  n &= ~(size_t)1;
  while (n) {
    size_t k = n < chunk ? n : chunk;
    header(fb, 0x01, 0, addr);
    memcpy(fb->tx + 4, px, k);
    if (send_packet(fb, 4 + k)) return -1;
    addr += (unsigned)k;
    px += k;
    n -= k;
  }
  return 0;
}

// Feed received bytes to the packet parser; returns the number of whole packets.
static int parse(fwfb *fb, const uint8_t *b, ssize_t n) {
  int done = 0;
  for (ssize_t i = 0; i < n; i++) {
    if (b[i] & 0x80) {                          // status byte: a packet starts
      fb->pk_status = b[i];
      fb->pk_n = 0;
    } else if (fb->pk_n >= 0) {
      fb->pk_raw[fb->pk_n++] = b[i];
      if (fb->pk_n == 10) {                     // 70 bits, 7 per byte, LSB first -> 64 input bits
        uint64_t v = 0;
        for (int k = 9; k >= 0; k--) v = (v << 7) | fb->pk_raw[k];
        for (int k = 0; k < 8; k++) fb->inputs[k] = (uint8_t)(v >> (8 * k));
        fb->status = fb->pk_status;
        fb->pk_n = -1;
        done++;
      }
    }
  }
  return done;
}

int fwfb_poll(fwfb *fb) {
  uint8_t b[256];
  int done = 0;
  ssize_t n;
  while ((n = read(fb->uart, b, sizeof b)) > 0) done += parse(fb, b, n);
  return done;
}

int fwfb_wait_status(fwfb *fb, int timeout_ms) {
  struct pollfd p = {.fd = fb->uart, .events = POLLIN};
  struct timespec t0, t;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    clock_gettime(CLOCK_MONOTONIC, &t);
    int left = timeout_ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
    if (left < 0 || poll(&p, 1, left) <= 0) return -1;
    if (fwfb_poll(fb) > 0) return fb->status;
  }
}

int fwfb_status(const fwfb *fb) { return fb->status; }

void fwfb_inputs(const fwfb *fb, uint8_t out[8]) { memcpy(out, fb->inputs, 8); }

int fwfb_flip(fwfb *fb) {
  int want = (fb->status >= 0) ? !((fb->status >> 6) & 1) : -1;
  header(fb, 0x03, 0, 0);
  if (send_packet(fb, 4)) return -1;
  for (int tries = 0; tries < 4; tries++) {     // at most a field away; status every field
    int s = fwfb_wait_status(fb, 40);
    if (s < 0) return -1;
    if (want < 0 || ((s >> 6) & 1) == want) return 0;
  }
  return -1;
}
