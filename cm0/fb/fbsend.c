// fbsend.c - test images and an animation benchmark for the FW-MAME framebuffer.
//
//   make -C cm0/fb            (or: gcc -O2 -Wall -o fbsend fbsend.c fwfb.c)
//   sudo systemctl kill -s KILL fwcm0-bridge
//   sudo ./fbsend card                         # test card at 256x224
//   sudo ./fbsend image pic.raw pic.pal --size 288x224   # from host/img2fb.py
//   sudo ./fbsend anim --seconds 10            # moving boxes, double buffered; prints frame stats
//   sudo ./fbsend input --seconds 60           # show the FW2 / USB pad inputs live (fbuf v002)
//   sudo ./fbsend ramp on|off                  # FPGA's built-in palette ramp
//
// Options: --size WxH (default 256x224), --cpp N (pixel width in 40 ns
// clocks, 0 = widest that fits), --hz N (SPI clock, default 50 MHz).
#define _GNU_SOURCE
#include "fwfb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static int W = 256, H = 224, CPP = 0;
static long HZ = 50000000;
static double SECONDS = 10;

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// Palette for the generated images: 0-7 the color bar colors, 8-15 a grey
// ramp, 16-255 the default 3-3-2 cube is not used, so set a hue wheel.
static void card_palette(uint16_t *pal) {
  static const int bars[8][3] = {{7, 7, 7}, {7, 7, 0}, {0, 7, 7}, {0, 7, 0}, {7, 0, 7}, {7, 0, 0}, {0, 0, 7}, {0, 0, 0}};
  for (int i = 0; i < 8; i++) pal[i] = (uint16_t)(bars[i][0] << 6 | bars[i][1] << 3 | bars[i][2]);
  for (int i = 0; i < 8; i++) pal[8 + i] = (uint16_t)(i << 6 | i << 3 | i);
  for (int i = 16; i < 256; i++) {               // 240-step hue wheel
    int h = (i - 16) * 6 * 8 / 240, seg = h / 8, f = h % 8, r, g, b;
    switch (seg) {
      case 0: r = 7; g = f; b = 0; break;
      case 1: r = 7 - f; g = 7; b = 0; break;
      case 2: r = 0; g = 7; b = f; break;
      case 3: r = 0; g = 7 - f; b = 7; break;
      case 4: r = f; g = 0; b = 7; break;
      default: r = 7; g = 0; b = 7 - f; break;
    }
    pal[i] = (uint16_t)(r << 6 | g << 3 | b);
  }
}

// Test card: color bars on top, a grey ramp, a hue band, a 16-pixel grid in
// white, a 1-pixel white border and a circle, so geometry, every color bit,
// and scaling all show.
static void make_card(uint8_t *px) {
  int cx = W / 2, cy = H / 2, rad = (H < W ? H : W) * 3 / 8;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t c;
      if (y < H / 4) c = (uint8_t)(x * 8 / W);                        // bars
      else if (y < H / 2) c = (uint8_t)(8 + x * 8 / W);               // greys
      else if (y < H * 3 / 4) c = (uint8_t)(16 + x * 240 / W);        // hues
      else c = 7;                                                     // black
      if (x % 16 == 0 || y % 16 == 0) c = 0;                          // grid
      int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
      if (d2 >= (rad - 1) * (rad - 1) && d2 <= rad * rad) c = 0;      // circle
      if (x == 0 || y == 0 || x == W - 1 || y == H - 1) c = 0;        // border
      px[y * W + x] = c;
    }
}

static fwfb *open_mode(void) {
  fwfb *fb = fwfb_open(HZ);
  if (!fb) exit(1);
  if (fwfb_mode(fb, W, H, CPP)) exit(1);
  return fb;
}

static int show(fwfb *fb, const uint8_t *px, const uint16_t *pal) {
  if (pal && fwfb_palette(fb, 0, 256, pal)) return 1;
  double t0 = now_s();
  if (fwfb_pixels(fb, 0, px, (size_t)W * H)) return 1;
  double t1 = now_s();
  if (fwfb_flip(fb)) { fprintf(stderr, "flip timed out\n"); return 1; }
  printf("%dx%d: %d bytes sent in %.2f ms (%.2f MB/s), flipped after %.2f ms, status %02X\n", W, H, W * H,
         (t1 - t0) * 1e3, W * H / (t1 - t0) / 1e6, (now_s() - t1) * 1e3, fwfb_status(fb));
  return 0;
}

static int cmd_card(void) {
  fwfb *fb = open_mode();
  uint8_t *px = malloc((size_t)W * H);
  uint16_t pal[256];
  card_palette(pal);
  make_card(px);
  int r = show(fb, px, pal);
  fwfb_close(fb);
  return r;
}

static int load(const char *path, void *buf, size_t n) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return -1; }
  size_t got = fread(buf, 1, n, f);
  fclose(f);
  if (got != n) { fprintf(stderr, "%s: %zu bytes, expected %zu\n", path, got, n); return -1; }
  return 0;
}

static int cmd_image(const char *raw, const char *palf) {
  uint8_t *px = malloc((size_t)W * H);
  uint16_t pal[256];
  if (load(raw, px, (size_t)W * H)) return 1;
  if (palf && load(palf, pal, sizeof pal)) return 1;   // 256 x uint16 little-endian RGB333
  fwfb *fb = open_mode();
  int r = show(fb, px, palf ? pal : NULL);
  fwfb_close(fb);
  return r;
}

// Double-buffered animation: 8 boxes bouncing over the test card. Every frame
// is sent whole, then flipped, so this measures the steady frame rate.
static int cmd_anim(void) {
  fwfb *fb = open_mode();
  size_t n = (size_t)W * H;
  uint8_t *bg = malloc(n), *px = malloc(n);
  uint16_t pal[256];
  card_palette(pal);
  make_card(bg);
  if (fwfb_palette(fb, 0, 256, pal)) return 1;
  int bx[8], by[8], vx[8], vy[8], sz = H / 8;
  for (int i = 0; i < 8; i++) {
    bx[i] = (i * 37) % (W - sz); by[i] = (i * 53) % (H - sz);
    vx[i] = (i % 2 ? 1 : -1) * (1 + i % 3); vy[i] = (i % 3 ? 1 : -1) * (1 + i % 2);
  }
  int frames = 0, timeouts = 0, last_field = fwfb_status(fb) & 31, fields = 0, missed = 0;
  double send_ms = 0, wait_ms = 0, worst_send = 0, t_start = now_s();
  while (now_s() - t_start < SECONDS) {
    memcpy(px, bg, n);
    for (int i = 0; i < 8; i++) {
      bx[i] += vx[i]; by[i] += vy[i];
      if (bx[i] < 0 || bx[i] > W - sz) { vx[i] = -vx[i]; bx[i] += 2 * vx[i]; }
      if (by[i] < 0 || by[i] > H - sz) { vy[i] = -vy[i]; by[i] += 2 * vy[i]; }
      for (int y = by[i]; y < by[i] + sz; y++) memset(px + (size_t)y * W + bx[i], 16 + i * 30, (size_t)sz);
    }
    double t0 = now_s();
    if (fwfb_pixels(fb, 0, px, n)) return 1;
    double t1 = now_s();
    if (fwfb_flip(fb)) timeouts++;
    double t2 = now_s();
    send_ms += (t1 - t0) * 1e3;
    wait_ms += (t2 - t1) * 1e3;
    if ((t1 - t0) * 1e3 > worst_send) worst_send = (t1 - t0) * 1e3;
    int field = fwfb_status(fb) & 31, step = (field - last_field) & 31;
    fields += step;
    if (step > 1) missed += step - 1;
    last_field = field;
    frames++;
  }
  double dt = now_s() - t_start;
  printf("anim %dx%d at %.1f MHz SPI: %d frames in %.2f s = %.2f fps over %d fields (%.2f Hz);"
         " %d fields repeated, %d flip timeouts\n",
         W, H, HZ / 1e6, frames, dt, frames / dt, fields, fields / dt, missed, timeouts);
  printf("per frame: send %.2f ms (worst %.2f, %.2f MB/s), wait for flip %.2f ms\n", send_ms / frames, worst_send,
         n * frames / (send_ms / 1e3) / 1e6, wait_ms / frames);
  fwfb_close(fb);
  return 0;
}

// ---- input viewer: the FW2's controls on the CRT, lit while held ----
static void box(uint8_t *px, int x, int y, int w, int h, uint8_t c) {
  for (int j = y; j < y + h && j < H; j++)
    if (j >= 0) memset(px + (size_t)j * W + x, c, (size_t)(x + w <= W ? w : W - x));
}

static void draw_inputs(uint8_t *px, const uint8_t in[8]) {
  unsigned btn = in[0] | in[1] << 8, pad = in[2] | in[3] << 8;
  const uint8_t OFF = 9, BLACK = 7;
  memset(px, BLACK, (size_t)W * H);
  box(px, 8, 8, in[7], 4, 2);                                   // update counter (0-255 px)
  // nav pad: up, down, left, right, centre
  static const int nav[5][3] = {{FWFB_BTN_NAV_UP, 60, 50}, {FWFB_BTN_NAV_DOWN, 60, 110},
                                {FWFB_BTN_NAV_LEFT, 30, 80}, {FWFB_BTN_NAV_RIGHT, 90, 80},
                                {FWFB_BTN_NAV_CENTER, 60, 80}};
  for (int i = 0; i < 5; i++) box(px, nav[i][1], nav[i][2], 26, 26, (btn >> nav[i][0] & 1) ? 0 : OFF);
  // HOME, OK, CANCEL, PAGE
  for (int i = 0; i < 4; i++) box(px, 140 + i * 28, 40, 22, 16, (btn >> (FWFB_BTN_HOME + i) & 1) ? 2 : OFF);
  // the five coloured buttons, left to right as on the FW2
  static const uint8_t lit[5] = {0, 1, 3, 6, 5};
  for (int i = 0; i < 5; i++) box(px, 8 + i * 50, 170, 40, 30, (btn >> i & 1) ? lit[i] : OFF);
  // USB controller: d-pad, 8 buttons, stick
  for (int i = 0; i < FWFB_PAD_COUNT; i++) box(px, 140 + (i % 6) * 18, 80 + (i / 6) * 18, 14, 14, (pad >> i & 1) ? 4 : OFF);
  int sx = 220 + (int8_t)in[4] * 16 / 128, sy = 140 + (int8_t)in[5] * 16 / 128;
  box(px, 204, 124, 34, 34, 8);
  box(px, sx - 2, sy - 2, 5, 5, 0);
}

static const char *btn_name[FWFB_BTN_COUNT] = {"GREY", "YELLOW", "GREEN", "BLUE", "RED", "NAV-C", "UP",
                                               "DOWN", "LEFT", "RIGHT", "HOME", "OK", "CANCEL", "PAGE"};

static int cmd_input(void) {
  fwfb *fb = open_mode();
  uint16_t pal[256];
  card_palette(pal);
  if (fwfb_palette(fb, 0, 256, pal)) return 1;
  uint8_t *px = malloc((size_t)W * H), in[8] = {0}, last[8] = {1};
  double t0 = now_s();
  int packets = 0, changes = 0, updates = 0;
  while (now_s() - t0 < SECONDS) {
    if (fwfb_wait_status(fb, 100) < 0) { fprintf(stderr, "no packet in 100 ms\n"); return 1; }
    packets++;
    fwfb_inputs(fb, in);
    if (in[7] != last[7]) updates++;           // the app writes on every change and every 500 ms
    if (!memcmp(in, last, 7)) { last[7] = in[7]; continue; }
    memcpy(last, in, 8);
    changes++;
    unsigned btn = in[0] | in[1] << 8;
    printf("[%7.3f] %02X%02X %02X%02X %02X %02X %02X #%3u :", now_s() - t0, in[1], in[0], in[3], in[2], in[4], in[5],
           in[6], in[7]);
    for (int i = 0; i < FWFB_BTN_COUNT; i++)
      if (btn >> i & 1) printf(" %s", btn_name[i]);
    printf("\n");
    fflush(stdout);
    draw_inputs(px, in);
    if (fwfb_pixels(fb, 0, px, (size_t)W * H) || fwfb_flip(fb)) return 1;
  }
  printf("input: %d packets, %d changes, %d app updates in %.1f s\n", packets, changes, updates, now_s() - t0);
  fwfb_close(fb);
  return 0;
}

// Test tone for the FW2 speaker: a sine at --freq Hz (default 440), 16-bit mono
// at 16 kHz by the CM0's clock, sent once per field (fbuf v003+).
static double tone_freq = 440.0;
static int cmd_tone(void) {
    fwfb *fb = fwfb_open(HZ);
    if (!fb) return 1;
    static int16_t buf[2048];
    double t0 = now_s(), phase = 0;
    long long due_total = 0, sent = 0;
    while (now_s() - t0 < SECONDS) {
        if (fwfb_wait_status(fb, 100) < 0) { fprintf(stderr, "no packet in 100 ms\n"); return 1; }
        const long long due = (long long)((now_s() - t0) * 16000.0);
        int n = (int)(due - sent);
        if (n > 2048) n = 2048;
        for (int i = 0; i < n; i++) {
            buf[i] = (int16_t)(8000.0 * sin(phase));
            phase += 2.0 * M_PI * tone_freq / 16000.0;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        if (n > 0 && fwfb_audio(fb, buf, (size_t)n)) return 1;
        sent += n;
        due_total = due;
    }
    printf("tone %.0f Hz: %lld samples in %.1f s (%.0f/s)\n", tone_freq, sent, now_s() - t0,
           sent / (now_s() - t0));
    (void)due_total;
    fwfb_close(fb);
    return 0;
}

static int usage(void) {
  fprintf(stderr, "usage: fbsend card | image RAW [PAL] | anim | input | ramp on|off   [--size WxH] [--cpp N] [--hz N] [--seconds S]\n");
  return 2;
}

int main(int argc, char **argv) {
  const char *pos[3] = {0};
  int npos = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--size") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &W, &H) != 2) return usage(); }
    else if (!strcmp(argv[i], "--cpp") && i + 1 < argc) CPP = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--hz") && i + 1 < argc) HZ = atol(argv[++i]);
    else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) SECONDS = atof(argv[++i]);
    else if (!strcmp(argv[i], "--freq") && i + 1 < argc) tone_freq = atof(argv[++i]);
    else if (npos < 3) pos[npos++] = argv[i];
    else return usage();
  }
  if (!pos[0] || W < 2 || H < 1 || (long)W * H > 65536 || W % 2) {
    if (pos[0]) fprintf(stderr, "size must be even width, width x height <= 65536\n");
    return usage();
  }
  if (!strcmp(pos[0], "card")) return cmd_card();
  if (!strcmp(pos[0], "image") && pos[1]) return cmd_image(pos[1], pos[2]);
  if (!strcmp(pos[0], "anim")) return cmd_anim();
  if (!strcmp(pos[0], "input")) return cmd_input();
  if (!strcmp(pos[0], "tone")) return cmd_tone();
  if (!strcmp(pos[0], "ramp") && pos[1]) {
    fwfb *fb = fwfb_open(HZ);
    if (!fb) return 1;
    int r = fwfb_setreg(fb, FWFB_CONTROL, !strcmp(pos[1], "on"));
    fwfb_close(fb);
    return r ? 1 : 0;
  }
  return usage();
}
