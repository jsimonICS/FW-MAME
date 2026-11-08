// fwmame.c - minimal libretro frontend: MAME 2003-Plus on the FW2's CRT.
//
//   make -C cm0/fe
//   sudo systemctl kill -s KILL fwcm0-bridge          # it owns SPI0, the UART and GPIO21
//   sudo ./fwmame ~/fwmame/cores/mame2003_plus_libretro.so ~/fwmame/roms/<game>.zip
//
// Two threads:
//   - emulation: retro_run() once per frame. video_refresh copies the frame
//     (as RGB565) into a hand-off buffer and waits until the sender has taken
//     the previous one, so the core runs at the display's rate.
//   - sender: maps the frame to 8-bit pixels through a palette of up to 256
//     RGB333 colours, sends it to the FPGA (cm0/fb/fwfb.c) and flips at the
//     next vblank. Sending frame N overlaps emulating frame N+1. It also picks
//     up the inputs the FPGA sends every field.
// Palette: colours are only ever added (an entry never changes while it may be
// on screen, because palette writes take effect at once). Once all 256 are in
// use, further colours map to the nearest entry.
//
// Inputs (cm0/fb/fwfb.h) -> RetroPad, player 1: FW2 nav pad = d-pad, GREY =
// SELECT (coin), YELLOW = START, GREEN = B (button 1), BLUE = A (2), RED = Y (3),
// nav centre = X (4), OK = L (5), CANCEL = R (6); a USB pad's bits add to the same.
// No audio yet. Options: --seconds N (run time, 0 = until Ctrl+C), --hz N (SPI).
#define _GNU_SOURCE
#include "fwlibretro.h"
#include "../fb/fwfb.h"

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAXW 640
#define MAXH 480

// ---------------------------------------------------------------- core symbols
static struct {
    void *h;
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*init)(void);
    void (*deinit)(void);
    unsigned (*api_version)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*run)(void);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
} core;

static volatile sig_atomic_t running = 1;
static void on_signal(int s) { (void)s; running = 0; }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ---------------------------------------------------------------- environment
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static char system_dir[256], save_dir[256];

// Overrides; every other option answers with its default (the first value the
// core listed in SET_VARIABLES, "Description; first|second|...").
static const struct retro_variable options[] = {
    {"mame2003-plus_skip_disclaimer", "enabled"},
    {"mame2003-plus_skip_warnings", "enabled"},
    {"mame2003-plus_sample_rate", "22050"},          // less to resample to the FW2's 16 kHz
    {NULL, NULL},
};

#define MAX_VARS 128
static struct { char key[64]; char def[64]; } vars[MAX_VARS];
static int n_vars;

static void store_variables(const struct retro_variable *v) {
    n_vars = 0;
    for (; v && v->key && n_vars < MAX_VARS; v++) {
        const char *semi = v->value ? strchr(v->value, ';') : NULL;
        if (!semi) continue;
        semi++;
        while (*semi == ' ') semi++;
        size_t len = strcspn(semi, "|");
        if (len >= sizeof vars[0].def) len = sizeof vars[0].def - 1;
        snprintf(vars[n_vars].key, sizeof vars[0].key, "%s", v->key);
        memcpy(vars[n_vars].def, semi, len);
        vars[n_vars].def[len] = '\0';
        n_vars++;
    }
    fprintf(stderr, "[fwmame] core declared %d options\n", n_vars);
}

static void core_log(enum retro_log_level level, const char *fmt, ...) {
    static const char *const tag[] = {"debug", "info", "warn", "error"};
    if (level == RETRO_LOG_DEBUG) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[core %s] ", tag[level & 3]);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool environment(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = system_dir; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = save_dir; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
        if (f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_XRGB8888 && f != RETRO_PIXEL_FORMAT_0RGB1555)
            return false;
        pixfmt = f;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        v->value = NULL;
        for (const struct retro_variable *o = options; o->key; o++)
            if (!strcmp(o->key, v->key)) { v->value = o->value; return true; }
        for (int i = 0; i < n_vars; i++)
            if (!strcmp(vars[i].key, v->key)) { v->value = vars[i].def; return true; }
        return false;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES: store_variables(data); return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = core_log; return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return true;   // legacy variables
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: fprintf(stderr, "[core msg] %s\n", ((const struct retro_message *)data)->msg); return true;
    case RETRO_ENVIRONMENT_SHUTDOWN: running = 0; return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        return true;
    case RETRO_ENVIRONMENT_SET_ROTATION:
        fprintf(stderr, "[fwmame] core asks for rotation %u (not supported: shown unrotated)\n", *(const unsigned *)data);
        return false;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- frame hand-off
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_ready = PTHREAD_COND_INITIALIZER, cv_taken = PTHREAD_COND_INITIALIZER;
static uint16_t frames_buf[2][MAXW * MAXH];          // RGB565
static uint16_t *pending = frames_buf[0], *work = frames_buf[1];
static unsigned pend_w, pend_h;
static bool pend_ready;
static uint8_t inputs[8];                            // newest from the FPGA (under mx)
static unsigned long emu_frames, dup_frames;
static double t_handoff;                             // time the core thread waited for the sender

static inline uint16_t px565(const void *row, unsigned x) {
    switch (pixfmt) {
    case RETRO_PIXEL_FORMAT_RGB565: return ((const uint16_t *)row)[x];
    case RETRO_PIXEL_FORMAT_XRGB8888: {
        uint32_t p = ((const uint32_t *)row)[x];
        return (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
    }
    default: {                                       // 0RGB1555
        uint16_t p = ((const uint16_t *)row)[x];
        return (uint16_t)(((p << 1) & 0xFFC0) | (p & 0x1F));
    }
    }
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch) {
    emu_frames++;
    if (!data) { dup_frames++; return; }             // frame unchanged
    if (w > MAXW) w = MAXW;
    if (h > MAXH) h = MAXH;
    const double tw = now_s();
    pthread_mutex_lock(&mx);
    while (pend_ready && running) pthread_cond_wait(&cv_taken, &mx);   // pace to the display
    t_handoff += now_s() - tw;
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)data + y * pitch;
        uint16_t *dst = pending + y * w;
        if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) memcpy(dst, row, w * 2);
        else for (unsigned x = 0; x < w; x++) dst[x] = px565(row, x);
    }
    pend_w = w;
    pend_h = h;
    pend_ready = true;
    pthread_cond_signal(&cv_ready);
    pthread_mutex_unlock(&mx);
}

// ---------------------------------------------------------------- audio
// The core's stereo samples (at av.timing.sample_rate) -> mono 16 kHz by linear
// interpolation, queued for the sender thread, which sends them to the FPGA
// before each frame (fbuf v003+: the FW2 display app plays them).
#define AQ_SIZE 8192
static int16_t aq[AQ_SIZE];
static unsigned aq_n;                                // queued samples (under mx)
static unsigned long aq_dropped;
static double in_rate = 48000.0, rs_pos;             // resampler: position between in samples
static int16_t rs_prev;

static void resample_push(const int16_t *stereo, size_t frames) {
    const double step = in_rate / 16000.0;           // input samples per output sample
    pthread_mutex_lock(&mx);
    for (size_t i = 0; i < frames; i++) {
        const int16_t cur = (int16_t)(((int)stereo[2 * i] + stereo[2 * i + 1]) / 2);
        while (rs_pos < 1.0) {                       // output samples falling between prev and cur
            const int s = (int)(rs_prev + (cur - rs_prev) * rs_pos);
            if (aq_n < AQ_SIZE) aq[aq_n++] = (int16_t)s;
            else aq_dropped++;
            rs_pos += step;
        }
        rs_pos -= 1.0;
        rs_prev = cur;
    }
    pthread_mutex_unlock(&mx);
}

static size_t audio_batch(const int16_t *d, size_t frames) { resample_push(d, frames); return frames; }
static void audio_sample(int16_t l, int16_t r) { const int16_t s[2] = {l, r}; resample_push(s, 1); }

// ---------------------------------------------------------------- inputs
static uint16_t joy_bits;                            // RetroPad bits for player 1, set in input_poll

static void input_poll(void) {
    uint8_t in[8];
    pthread_mutex_lock(&mx);
    memcpy(in, inputs, 8);
    pthread_mutex_unlock(&mx);
    const unsigned b = in[0] | in[1] << 8, p = in[2] | in[3] << 8;
    uint16_t j = 0;
#define MAP(cond, id) if (cond) j |= (uint16_t)(1u << (id))
    MAP(b >> FWFB_BTN_NAV_UP & 1 || p >> FWFB_PAD_UP & 1, RETRO_DEVICE_ID_JOYPAD_UP);
    MAP(b >> FWFB_BTN_NAV_DOWN & 1 || p >> FWFB_PAD_DOWN & 1, RETRO_DEVICE_ID_JOYPAD_DOWN);
    MAP(b >> FWFB_BTN_NAV_LEFT & 1 || p >> FWFB_PAD_LEFT & 1, RETRO_DEVICE_ID_JOYPAD_LEFT);
    MAP(b >> FWFB_BTN_NAV_RIGHT & 1 || p >> FWFB_PAD_RIGHT & 1, RETRO_DEVICE_ID_JOYPAD_RIGHT);
    MAP(b >> FWFB_BTN_GREY & 1 || p >> FWFB_PAD_SELECT & 1, RETRO_DEVICE_ID_JOYPAD_SELECT);
    MAP(b >> FWFB_BTN_YELLOW & 1 || p >> FWFB_PAD_START & 1, RETRO_DEVICE_ID_JOYPAD_START);
    MAP(b >> FWFB_BTN_GREEN & 1 || p >> FWFB_PAD_A & 1, RETRO_DEVICE_ID_JOYPAD_B);
    MAP(b >> FWFB_BTN_BLUE & 1 || p >> FWFB_PAD_B & 1, RETRO_DEVICE_ID_JOYPAD_A);
    MAP(b >> FWFB_BTN_RED & 1 || p >> FWFB_PAD_X & 1, RETRO_DEVICE_ID_JOYPAD_Y);
    MAP(b >> FWFB_BTN_NAV_CENTER & 1 || p >> FWFB_PAD_Y & 1, RETRO_DEVICE_ID_JOYPAD_X);
    MAP(b >> FWFB_BTN_OK & 1 || p >> FWFB_PAD_L & 1, RETRO_DEVICE_ID_JOYPAD_L);
    MAP(b >> FWFB_BTN_CANCEL & 1 || p >> FWFB_PAD_R & 1, RETRO_DEVICE_ID_JOYPAD_R);
#undef MAP
    if (j != joy_bits) fprintf(stderr, "[fwmame] RetroPad bits %04x (FW2 %04x, pad %04x)\n", j, b, p);
    joy_bits = j;
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)joy_bits;
    return id < 16 ? (int16_t)(joy_bits >> id & 1) : 0;
}

// ---------------------------------------------------------------- palette
static uint16_t lut333[65536];                       // RGB565 -> RGB333 code
static uint8_t idx_of[512];                          // RGB333 code -> palette index, 0xFF = none
static uint16_t pal[256];
static int pal_used, pal_sent;
static unsigned long pal_misses;

static void palette_init(void) {
    for (unsigned c = 0; c < 65536; c++) {
        unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        r = (r * 7 + 15) / 31; g = (g * 7 + 31) / 63; b = (b * 7 + 15) / 31;
        lut333[c] = (uint16_t)(r << 6 | g << 3 | b);
    }
    memset(idx_of, 0xFF, sizeof idx_of);
}

static uint8_t palette_index(uint16_t code) {
    uint8_t i = idx_of[code];
    if (i != 0xFF) return i;
    if (pal_used < 256) {
        i = (uint8_t)pal_used++;
        pal[i] = code;
    } else {                                         // full: nearest existing colour, remembered
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 256; k++) {
            int dr = (code >> 6) - (pal[k] >> 6), dg = (code >> 3 & 7) - (pal[k] >> 3 & 7), db = (code & 7) - (pal[k] & 7);
            int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bd) { bd = d; best = k; }
        }
        i = (uint8_t)best;
        pal_misses++;
    }
    idx_of[code] = i;
    return i;
}

// ---------------------------------------------------------------- frame dump (--dump N)
static unsigned long dump_at;        // frame number to dump, 0 = never
static unsigned long sent, flip_timeouts, audio_sent;

// Writes /tmp/fwmame_core.ppm (the core's RGB565 frame) and /tmp/fwmame_sent.ppm
// (what was sent: palette indices through the RGB333 palette), and the palette.
static void dump_frame(const uint16_t *src, unsigned w, unsigned h, const uint8_t *px, unsigned fw, unsigned fh) {
    FILE *f = fopen("/tmp/fwmame_core.ppm", "wb");
    if (f) {
        fprintf(f, "P6 %u %u 255\n", w, h);
        for (unsigned i = 0; i < w * h; i++) {
            const uint16_t c = src[i];
            const uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3)};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    f = fopen("/tmp/fwmame_sent.ppm", "wb");
    if (f) {
        fprintf(f, "P6 %u %u 255\n", fw, fh);
        for (unsigned i = 0; i < fw * fh; i++) {
            const uint16_t c = pal[px[i]];
            const uint8_t rgb[3] = {(uint8_t)((c >> 6) * 255 / 7), (uint8_t)((c >> 3 & 7) * 255 / 7), (uint8_t)((c & 7) * 255 / 7)};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    fprintf(stderr, "[fwmame] dumped frame %lu (%ux%u -> %ux%u), %d colours:", sent, w, h, fw, fh, pal_used);
    for (int i = 0; i < pal_used && i < 32; i++) fprintf(stderr, " %03o", pal[i]);
    fprintf(stderr, "\n");
}

// ---------------------------------------------------------------- sender thread
static long spi_hz = 50000000;
static double t_conv, t_send, t_flip;

static void *sender(void *arg) {
    (void)arg;
    fwfb *fb = fwfb_open(spi_hz);
    if (!fb) { running = 0; pthread_cond_broadcast(&cv_taken); return NULL; }
    static uint8_t px[65536];
    unsigned cur_w = 0, cur_h = 0;
    while (running) {
        pthread_mutex_lock(&mx);
        while (!pend_ready && running) pthread_cond_wait(&cv_ready, &mx);
        if (!running) { pthread_mutex_unlock(&mx); break; }
        uint16_t *t = work; work = pending; pending = t;   // take the frame
        unsigned w = pend_w, h = pend_h;
        pend_ready = false;
        static int16_t audio[AQ_SIZE];                      // and the audio queued so far
        const unsigned na = aq_n;
        memcpy(audio, aq, na * sizeof aq[0]);
        aq_n = 0;
        pthread_cond_signal(&cv_taken);
        pthread_mutex_unlock(&mx);
        if (na) fwfb_audio(fb, audio, na);
        audio_sent += na;

        // Fit the CRT and the FPGA's buffer: at most 241 lines (a 15 kHz field)
        // and 320 pixels, even width, at most 65536 pixels. Larger frames are
        // scaled down by dropping rows / columns (nearest neighbour), so a
        // vertical game rotated to 224x288 shows whole, upright.
        unsigned fw = w > 320 ? 320 : (w & ~1u), fh = h > 241 ? 241 : h;
        while (fw * fh > 65536) fh--;
        if (fw != cur_w || fh != cur_h) {
            fwfb_mode(fb, (int)fw, (int)fh, 0);
            cur_w = fw; cur_h = fh;
            fprintf(stderr, "[fwmame] picture %ux%u (core %ux%u%s)\n", fw, fh, w, h,
                    fw != (w & ~1u) || fh != h ? ", scaled" : "");
        }
        double t0 = now_s();
        for (unsigned y = 0; y < fh; y++) {
            const uint16_t *src = work + (fh == h ? y : y * h / fh) * w;
            uint8_t *dst = px + y * fw;
            if (fw == (w & ~1u))
                for (unsigned x = 0; x < fw; x++) dst[x] = palette_index(lut333[src[x]]);
            else
                for (unsigned x = 0; x < fw; x++) dst[x] = palette_index(lut333[src[x * w / fw]]);
        }
        if (dump_at && sent == dump_at) dump_frame(work, w, h, px, fw, fh);
        if (pal_used != pal_sent) {                  // new entries only: never a visible change
            fwfb_palette(fb, 0, pal_used, pal);
            pal_sent = pal_used;
        }
        double t1 = now_s();
        fwfb_pixels(fb, 0, px, (size_t)fw * fh);
        double t2 = now_s();
        if (fwfb_flip(fb)) flip_timeouts++;
        double t3 = now_s();
        uint8_t in[8];
        fwfb_inputs(fb, in);
        pthread_mutex_lock(&mx);
        memcpy(inputs, in, 8);
        pthread_mutex_unlock(&mx);
        t_conv += t1 - t0; t_send += t2 - t1; t_flip += t3 - t2;
        sent++;
    }
    fwfb_close(fb);
    return NULL;
}

// ---------------------------------------------------------------- main
static void *sym(const char *name) {
    void *p = dlsym(core.h, name);
    if (!p) { fprintf(stderr, "core has no %s\n", name); exit(1); }
    return p;
}

int main(int argc, char **argv) {
    const char *core_path = NULL, *rom = NULL;
    double seconds = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--hz") && i + 1 < argc) spi_hz = atol(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_at = strtoul(argv[++i], NULL, 0);
        else if (!core_path) core_path = argv[i];
        else if (!rom) rom = argv[i];
    }
    if (!core_path || !rom) {
        fprintf(stderr, "usage: fwmame <core.so> <rom.zip> [--seconds N] [--hz N]\n");
        return 2;
    }
    const char *home = getenv("SUDO_USER") ? "/home/pi" : getenv("HOME");
    snprintf(system_dir, sizeof system_dir, "%s/fwmame/system", home ? home : ".");
    snprintf(save_dir, sizeof save_dir, "%s/fwmame/saves", home ? home : ".");
    mkdir(system_dir, 0755);
    mkdir(save_dir, 0755);

    core.h = dlopen(core_path, RTLD_NOW);
    if (!core.h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    *(void **)&core.set_environment = sym("retro_set_environment");
    *(void **)&core.set_video_refresh = sym("retro_set_video_refresh");
    *(void **)&core.set_audio_sample = sym("retro_set_audio_sample");
    *(void **)&core.set_audio_sample_batch = sym("retro_set_audio_sample_batch");
    *(void **)&core.set_input_poll = sym("retro_set_input_poll");
    *(void **)&core.set_input_state = sym("retro_set_input_state");
    *(void **)&core.init = sym("retro_init");
    *(void **)&core.deinit = sym("retro_deinit");
    *(void **)&core.api_version = sym("retro_api_version");
    *(void **)&core.get_system_info = sym("retro_get_system_info");
    *(void **)&core.get_system_av_info = sym("retro_get_system_av_info");
    *(void **)&core.set_controller_port_device = sym("retro_set_controller_port_device");
    *(void **)&core.run = sym("retro_run");
    *(void **)&core.load_game = sym("retro_load_game");
    *(void **)&core.unload_game = sym("retro_unload_game");
    if (core.api_version() != RETRO_API_VERSION) { fprintf(stderr, "core API %u\n", core.api_version()); return 1; }

    struct retro_system_info si = {0};
    core.get_system_info(&si);
    fprintf(stderr, "[fwmame] core %s %s\n", si.library_name, si.library_version);
    core.set_environment(environment);
    core.set_video_refresh(video_refresh);
    core.set_audio_sample(audio_sample);
    core.set_audio_sample_batch(audio_batch);
    core.set_input_poll(input_poll);
    core.set_input_state(input_state);
    core.init();
    struct retro_game_info gi = {.path = rom};
    if (!core.load_game(&gi)) { fprintf(stderr, "load_game failed: %s\n", rom); return 1; }
    struct retro_system_av_info av = {0};
    core.get_system_av_info(&av);
    fprintf(stderr, "[fwmame] %s: %ux%u (max %ux%u) at %.2f fps, pixel format %d\n", rom, av.geometry.base_width,
            av.geometry.base_height, av.geometry.max_width, av.geometry.max_height, av.timing.fps, (int)pixfmt);
    core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    if (av.timing.sample_rate > 1000) in_rate = av.timing.sample_rate;
    fprintf(stderr, "[fwmame] audio %.0f Hz -> 16000 Hz mono\n", in_rate);

    palette_init();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    pthread_t th;
    pthread_create(&th, NULL, sender, NULL);

    double t_start = now_s(), t_report = t_start + 5, t_run = 0;
    unsigned long last_emu = 0, last_sent = 0;
    while (running && (seconds <= 0 || now_s() - t_start < seconds)) {
        double t0 = now_s();
        core.run();
        t_run += now_s() - t0;
        if (now_s() >= t_report) {
            double dt = now_s() - t_report + 5;
            fprintf(stderr, "[fwmame] emu %.1f fps (core %.1f ms/frame), sent %.1f fps, dupes %lu, colours %d (misses %lu),"
                    " convert %.1f ms, send %.1f ms, flip wait %.1f ms, flip timeouts %lu\n",
                    (emu_frames - last_emu) / dt,
                    emu_frames > last_emu ? (t_run - t_handoff) * 1e3 / (emu_frames - last_emu) : 0,
                    (sent - last_sent) / dt, dup_frames, pal_used, pal_misses,
                    sent > last_sent ? t_conv * 1e3 / (sent - last_sent) : 0,
                    sent > last_sent ? t_send * 1e3 / (sent - last_sent) : 0,
                    sent > last_sent ? t_flip * 1e3 / (sent - last_sent) : 0, flip_timeouts);
            last_emu = emu_frames; last_sent = sent; t_run = t_conv = t_send = t_flip = t_handoff = 0;
            t_report = now_s() + 5;
        }
    }
    running = 0;
    pthread_mutex_lock(&mx);
    pthread_cond_broadcast(&cv_ready);
    pthread_cond_broadcast(&cv_taken);
    pthread_mutex_unlock(&mx);
    pthread_join(th, NULL);
    core.unload_game();
    core.deinit();
    fprintf(stderr, "[fwmame] %lu frames emulated, %lu sent, %lu audio samples sent (%lu dropped)\n", emu_frames,
            sent, audio_sent, aq_dropped);
    return 0;
}
