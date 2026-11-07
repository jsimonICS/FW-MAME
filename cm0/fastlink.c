// fastlink.c - CM0 side of the FW-MAME fast link test (milestone 2).
// Runs on the CM0 with the fpga/fastlink design loaded; read the results on
// the PC with host/faststat.py.
//
//   gcc -O2 -pthread -o fastlink fastlink.c
//   sudo systemctl kill -s KILL fwcm0-bridge     # it owns SPI0, the UART and GPIO21
//   sudo ./fastlink send --hz 25000000 --seconds 2 --lfsr        # SPI
//   sudo ./fastlink send --hz 25000000 --seconds 2 --lfsr --uart # SPI + UART together
//   sudo ./fastlink send --hz 0 --uart --seconds 2               # UART only
//   ./fastlink recv --seconds 3 --rtscts --lfsr                  # FPGA -> CM0 UART
//
// send: writes a byte sequence (incrementing, or the LFSR sequence with
// --lfsr) to /dev/spidev0.0 in transfers of --chunk bytes (capped at the
// spidev module's bufsiz; raise it with `modprobe -r spidev; modprobe spidev
// bufsiz=65536`). Chip select is GPIO21, driven through /dev/gpiomem (root):
// --cs burst holds it low for the whole run (default), --cs xfer pulses it
// around every transfer, --cs none leaves it high (FPGA control bit 3 = no CS).
// --uart also streams the UART sequence on /dev/ttyAMA0 at 7.8125 Mbaud from a
// second thread. The FPGA counts and checks every byte.
//
// recv: reads /dev/ttyAMA0 and checks the FPGA's sequence. --rtscts turns on
// hardware flow control (pair it with FPGA control bit 1).
//
// The LFSR sequence is next = 8 steps of the Galois LFSR x^8+x^6+x^5+x^4+1
// (right shift, taps 0xB8); the same function is in fpga/fastlink/user_core.v.
#define _GNU_SOURCE
#include <asm/termbits.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CS_GPIO 21

static void die(const char *what) {
  fprintf(stderr, "fastlink: %s: %s\n", what, strerror(errno));
  exit(1);
}

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint8_t lfsr8(uint8_t s) {
  for (int k = 0; k < 8; k++) s = (s & 1) ? (uint8_t)((s >> 1) ^ 0xB8) : (uint8_t)(s >> 1);
  return s;
}

// One period of the sequence starting at `start`, repeated to fill `n` bytes.
// Period 256 (incrementing) or 255 (LFSR); a run continues across buffers by
// starting each send at offset (sent % period).
static uint8_t *make_pattern(size_t n, int lfsr, uint8_t start, int *period) {
  uint8_t *buf = malloc(n);
  if (!buf) die("malloc");
  *period = lfsr ? 255 : 256;
  uint8_t v = (lfsr && start == 0) ? 1 : start;
  for (size_t i = 0; i < n; i++) {
    buf[i] = v;
    v = lfsr ? lfsr8(v) : (uint8_t)(v + 1);
  }
  return buf;
}

// ---- GPIO21 chip select through /dev/gpiomem (BCM2837 GPIO block) ----
static volatile uint32_t *gpio;
static void cs_init(void) {
  int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
  if (fd < 0) die("/dev/gpiomem (run as root)");
  gpio = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (gpio == MAP_FAILED) die("mmap gpiomem");
  close(fd);
  gpio[7] = 1u << CS_GPIO;                         // GPSET0: high (idle) before making it an output
  uint32_t fsel = gpio[2];                         // GPFSEL2: GPIO20-29, 3 bits each
  fsel = (fsel & ~(7u << 3)) | (1u << 3);          // GPIO21 = output
  gpio[2] = fsel;
}
static inline void cs_low(void)  { gpio[10] = 1u << CS_GPIO; }   // GPCLR0
static inline void cs_high(void) { gpio[7]  = 1u << CS_GPIO; }   // GPSET0

// ---- UART at any baud rate (termios2 BOTHER) ----
static int uart_open(const char *port, int baud, int rtscts) {
  int fd = open(port, O_RDWR | O_NOCTTY);
  if (fd < 0) die(port);
  struct termios2 t;
  if (ioctl(fd, TCGETS2, &t) < 0) die("TCGETS2");
  t.c_iflag = 0;
  t.c_oflag = 0;
  t.c_lflag = 0;
  t.c_cflag &= ~(CBAUD | CSIZE | PARENB | CSTOPB | CRTSCTS);
  t.c_cflag |= BOTHER | CS8 | CREAD | CLOCAL | (rtscts ? CRTSCTS : 0);
  t.c_ispeed = t.c_ospeed = baud;
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 2;                               // reads return after 0.2 s with no data
  if (ioctl(fd, TCSETS2, &t) < 0) die("TCSETS2");
  return fd;
}

// ---- options ----
static struct {
  long hz, chunk, baud;
  double seconds;
  int lfsr, uart, rtscts, rt;
  const char *cs, *spidev, *port;
} opt = {25000000, 65536, 7812500, 2.0, 0, 0, 0, 0, "burst", "/dev/spidev0.0", "/dev/ttyAMA0"};

static volatile int stop;

// ---- UART sender thread ----
static long long uart_sent;
static double uart_secs;
static void *uart_thread(void *arg) {
  (void)arg;
  int fd = uart_open(opt.port, opt.baud, 0);
  int period;
  size_t n = 4096;
  uint8_t *buf = make_pattern(n + 256, opt.lfsr, 0x80, &period);
  double t0 = now_s();
  while (!stop) {
    ssize_t w = write(fd, buf + uart_sent % period, n);
    if (w < 0) die("uart write");
    uart_sent += w;
  }
  ioctl(fd, TCSBRK, 1);                            // tcdrain: let the last bytes go out
  uart_secs = now_s() - t0;
  close(fd);
  free(buf);
  return NULL;
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

static void realtime(void) {
  struct sched_param sp = {.sched_priority = 50};
  if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) perror("mlockall");
  if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0) perror("SCHED_FIFO");
}

static int cmd_send(void) {
  int cs_mode = !strcmp(opt.cs, "burst") ? 1 : !strcmp(opt.cs, "xfer") ? 2 : !strcmp(opt.cs, "none") ? 0 : -1;
  if (cs_mode < 0) { fprintf(stderr, "--cs must be burst, xfer or none\n"); return 2; }
  if (opt.rt) realtime();

  pthread_t th;
  if (opt.uart && pthread_create(&th, NULL, uart_thread, NULL)) die("pthread_create");

  long long spi_sent = 0;
  double spi_secs = 0;
  long chunk = opt.chunk, bufsiz = spidev_bufsiz();
  if (opt.hz > 0) {
    if (chunk > bufsiz) chunk = bufsiz;
    int fd = open(opt.spidev, O_RDWR);
    if (fd < 0) die(opt.spidev);
    uint8_t mode = SPI_MODE_0, bits = 8;
    uint32_t hz = opt.hz;
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) die("SPI_IOC_WR_MODE");
    if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) die("SPI_IOC_WR_BITS_PER_WORD");
    if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) die("SPI_IOC_WR_MAX_SPEED_HZ");
    int period;
    uint8_t *buf = make_pattern(chunk + 256, opt.lfsr, 0x01, &period);
    struct spi_ioc_transfer x;
    memset(&x, 0, sizeof x);
    x.len = chunk;
    x.speed_hz = hz;
    x.bits_per_word = 8;
    cs_init();
    if (cs_mode == 1) cs_low();
    usleep(10);
    double t0 = now_s(), end = t0 + opt.seconds;
    do {
      x.tx_buf = (uintptr_t)(buf + spi_sent % period);
      if (cs_mode == 2) cs_low();
      if (ioctl(fd, SPI_IOC_MESSAGE(1), &x) < 0) die("SPI_IOC_MESSAGE");
      if (cs_mode == 2) cs_high();
      spi_sent += chunk;
    } while (now_s() < end);
    spi_secs = now_s() - t0;
    usleep(10);
    cs_high();
    close(fd);
    free(buf);
  } else {
    double t0 = now_s();
    while (now_s() - t0 < opt.seconds) usleep(10000);
  }
  stop = 1;
  if (opt.uart) pthread_join(th, NULL);

  if (opt.hz > 0)
    printf("SPI %.2f MHz requested, chunk %ld (bufsiz %ld), cs %s, %s: sent %lld bytes in %.3f s = %.3f MB/s"
           " (%.0f%% of %.3f MB/s)\n",
           opt.hz / 1e6, chunk, bufsiz, opt.cs, opt.lfsr ? "lfsr" : "incrementing", spi_sent, spi_secs,
           spi_sent / spi_secs / 1e6, 100.0 * spi_sent * 8 / spi_secs / opt.hz, opt.hz / 8e6);
  if (opt.uart)
    printf("UART %ld baud, %s: sent %lld bytes in %.3f s = %.1f KB/s\n", opt.baud,
           opt.lfsr ? "lfsr" : "incrementing", uart_sent, uart_secs, uart_sent / uart_secs / 1e3);
  printf("result spi_bytes=%lld spi_secs=%.4f uart_bytes=%lld uart_secs=%.4f\n",
         spi_sent, spi_secs, uart_sent, uart_secs);
  return 0;
}

static int cmd_recv(void) {
  int fd = uart_open(opt.port, opt.baud, opt.rtscts);
  ioctl(fd, TCFLSH, TCIFLUSH);
  static uint8_t buf[4096];
  long long total = 0, errs = 0;
  int expect = -1;
  double t0 = now_s();
  while (now_s() - t0 < opt.seconds) {
    ssize_t r = read(fd, buf, sizeof buf);
    if (r < 0) die("uart read");
    for (ssize_t i = 0; i < r; i++) {
      if (expect >= 0 && buf[i] != expect) errs++;
      expect = opt.lfsr ? lfsr8(buf[i]) : (uint8_t)(buf[i] + 1);
    }
    total += r;
  }
  double dt = now_s() - t0;
  close(fd);
  printf("UART RX %ld baud%s, %s: %lld bytes in %.2f s = %.1f KB/s, %lld sequence errors\n", opt.baud,
         opt.rtscts ? " rtscts" : "", opt.lfsr ? "lfsr" : "incrementing", total, dt, total / dt / 1e3, errs);
  printf("result rx_bytes=%lld rx_errs=%lld\n", total, errs);
  return 0;
}

static void usage(void) {
  fprintf(stderr,
          "usage: fastlink send [--hz N] [--seconds S] [--chunk N] [--cs burst|xfer|none] [--lfsr] [--uart] [--rt]\n"
          "       fastlink recv [--seconds S] [--rtscts] [--lfsr]\n"
          "       (also --baud N, --spidev PATH, --port PATH; --hz 0 = no SPI)\n");
  exit(2);
}

int main(int argc, char **argv) {
  if (argc < 2) usage();
  for (int i = 2; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "--lfsr")) opt.lfsr = 1;
    else if (!strcmp(a, "--uart")) opt.uart = 1;
    else if (!strcmp(a, "--rtscts")) opt.rtscts = 1;
    else if (!strcmp(a, "--rt")) opt.rt = 1;
    else if (!v) usage();
    else if (!strcmp(a, "--hz")) { opt.hz = atol(v); i++; }
    else if (!strcmp(a, "--chunk")) { opt.chunk = atol(v); i++; }
    else if (!strcmp(a, "--baud")) { opt.baud = atol(v); i++; }
    else if (!strcmp(a, "--seconds")) { opt.seconds = atof(v); i++; }
    else if (!strcmp(a, "--cs")) { opt.cs = v; i++; }
    else if (!strcmp(a, "--spidev")) { opt.spidev = v; i++; }
    else if (!strcmp(a, "--port")) { opt.port = v; i++; }
    else usage();
  }
  if (opt.chunk < 1) opt.chunk = 1;
  if (!strcmp(argv[1], "send")) return cmd_send();
  if (!strcmp(argv[1], "recv")) return cmd_recv();
  usage();
}
