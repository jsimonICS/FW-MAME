/* fwmame - FW-MAME display app for the FreeWili 2.
 *
 * The FW-MAME console: the CM0 runs the emulator and sends frames to the
 * iCE40 FPGA, which drives a 15 kHz arcade monitor from the header (fpga/fbuf,
 * embedded in this app at build time). This app
 *   - selects the 3.3 V VIO rail and holds power zone 6 (FPGA);
 *   - makes sure /fpga/fbuf.fwfpga on the SD card is this build's, writes it
 *     if not, and loads it with h\f\l unless that design is already running;
 *   - forwards the FW2's 14 buttons to the CM0: every change (and every
 *     500 ms) is one h\f\w writing the 8 input bytes to FPGA registers
 *     0x30-0x37, which the FPGA sends to the CM0 with every field (layout in
 *     cm0/fb/fwfb.h: bytes 0-1 buttons, bit n = UARTKBD_BTN_n; byte 7 an
 *     update counter).
 * Every 2 s it checks the design is still loaded (MAIN reboot, zone 6 drop)
 * and reloads it if not.
 *
 * All buttons go to the game, including HOME and PAGE; holding HOME for 5 s
 * still leaves the app and holding PAGE for 5 s shows About (FW2App
 * contract). The labels over the five coloured buttons show FW-MAME's
 * default mapping, which lives on the CM0. */
#include "fw2.h"
#include "platform/diag.h"
#include "pico/stdlib.h"
#include "onewili.h"
#include "onewili_fwgui.h"
#include "onewili_sd.h"
#include "input/app_recovery_onewili.h"
#include "fbuf_fwfpga.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define FPGA_DIR   "/fpga"
#define FPGA_FILE  FPGA_DIR "/" FWFPGA_NAME ".fwfpga"
#define REG_INPUTS 0x30
#define REFRESH_US 500000u

static ow_device dev;                 /* ~37 KB of link buffers: not on the stack */
static char reply[OW_RESP_MAX];

static uint16_t buttons;              /* bit n = UARTKBD_BTN_n held */
static uint16_t shown = 0xFFFF;       /* what the screen shows */
static uint8_t counter;
static bool running;
static uint32_t writes, write_fails, last_ms, worst_ms;

static inline uint16_t be16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
#define C_BLACK be16(0x0000)
#define C_WHITE be16(0xFFFF)
#define C_GREEN be16(0x07E0)
#define C_RED   be16(0xF800)
#define C_GREY  be16(0x8410)
#define C_DARK  be16(0x2104)
#define C_CYAN  be16(0x07FF)

enum { ROW_STATE, ROW_LINK, ROW_STATS, ROW_COUNT };
#define ROW_Y(r) (40 + (r) * 22)

static void line(int row, uint16_t fg, const char *fmt, ...) {
    char s[30];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    st7796_fill_rect(8, ROW_Y(row), 464, 16, C_BLACK);
    st7796_draw_text(8, ROW_Y(row), 2, fg, C_BLACK, s);
}

/* ---- button panel: each of the 14 buttons as a box, lit while held ---- */
typedef struct { int x, y, w, h; const char *label; } key_box;
static const key_box keys[UARTKBD_BTN_COUNT] = {
    [UARTKBD_BTN_GREY]       = {0},   /* the coloured five use the bottom bar */
    [UARTKBD_BTN_NAV_UP]     = {70, 112, 40, 34, "U"},
    [UARTKBD_BTN_NAV_DOWN]   = {70, 192, 40, 34, "D"},
    [UARTKBD_BTN_NAV_LEFT]   = {26, 152, 40, 34, "L"},
    [UARTKBD_BTN_NAV_RIGHT]  = {114, 152, 40, 34, "R"},
    [UARTKBD_BTN_NAV_CENTER] = {70, 152, 40, 34, "C"},
    [UARTKBD_BTN_HOME]       = {200, 130, 120, 30, "HOME"},
    [UARTKBD_BTN_OK]         = {330, 130, 120, 30, "OK"},
    [UARTKBD_BTN_CANCEL]     = {200, 170, 120, 30, "CANCEL"},
    [UARTKBD_BTN_PAGE]       = {330, 170, 120, 30, "PAGE"},
};

/* The five coloured buttons: boxes directly above them (wilibsp AGENTS.md),
 * in wire-order colours, labelled with FW-MAME's default mapping. Held =
 * white box. */
static const uint16_t bar_colour[5] = {0x9AD6, 0x06FF, 0x0012, 0xF800, 0x0780};
static const char *const bar_label[5] = {"COIN", "START", "B1", "B2", "B3"};

static void draw_bar_button(int i, bool held) {
    const int w = (ST7796_W - 12) / 5, h = 40, x = i * 96, y = ST7796_H - h;
    const uint16_t bg = held ? C_WHITE : bar_colour[i];
    const uint16_t fg = (held || i <= 1) ? C_BLACK : C_WHITE;
    st7796_fill_rect(x, y, w, h, bg);
    st7796_draw_text(x + 10, y + 12, 2, fg, bg, bar_label[i]);
}

static void draw_buttons(void) {
    for (int b = 0; b < UARTKBD_BTN_COUNT; b++) {
        const bool held = (buttons >> b) & 1u;
        if (!(((shown ^ buttons) >> b) & 1u)) continue;
        if (b <= UARTKBD_BTN_RED) {
            draw_bar_button(b, held);
        } else {
            const key_box *k = &keys[b];
            const uint16_t bg = held ? C_CYAN : C_DARK, fg = held ? C_BLACK : C_GREY;
            st7796_fill_rect(k->x, k->y, k->w, k->h, bg);
            st7796_draw_text(k->x + 6, k->y + (k->h - 16) / 2, 2, fg, bg, k->label);
        }
    }
    shown = buttons;
}

/* ---- MAIN menu commands through the raw path; the reply body lands in `reply` ---- */
static bool call(const char *label, const char *cmd, uint32_t timeout_ms) {
    int ok = 0;
    reply[0] = '\0';
    ow_raw_stash_clear();
    if (ow_raw_send(&dev, cmd) < 0) {
        DIAG("fwmame: %s send failed\n", label);
        return false;
    }
    ow_status s = ow_raw_next_response(&dev, reply, sizeof reply, &ok, timeout_ms);
    if (s != OW_OK) {
        DIAG("fwmame: %s no response, status %d\n", label, (int)s);
        reply[0] = '\0';
        return false;
    }
    if (!ok) DIAG("fwmame: %s failed: '%s'\n", label, reply);
    return ok != 0;
}

/* MAIN gates the FPGA and header on zone 6 using the zone state DISPLAY
 * reports; a RAM app must keep reporting it or MAIN refuses (EPOWERZONE 6). */
static void report_zones(void) {
    uint32_t rails;
    if (picpwr_rails(&rails)) ow_fwgui_send_power_zones(rails);
}

/* h\f\s: `source armed cdone id version ...`; true when this build's fbuf is armed. */
static bool fbuf_running(void) {
    char source[16];
    int armed = 0, cdone = 0;
    unsigned id = 0, version = 0;
    if (!call("STATUS", "h\\f\\s", 2000)) return false;
    if (sscanf(reply, "%15s %d %d %u %u", source, &armed, &cdone, &id, &version) != 5) return false;
    return armed == 1 && id == FWFPGA_DESIGN_ID && version == FWFPGA_VERSION;
}

static bool sd_file_current(void) {
    bool is_dir = false;
    uint32_t size = 0;
    if (ow_sd_stat(&dev, FPGA_FILE, &is_dir, &size) != OW_OK || is_dir || size != FWFPGA_LEN) return false;
    static uint8_t head[FWFPGA_HEADER_LEN];
    ow_sd_file f;
    size_t got = 0;
    if (ow_sd_open(&dev, &f, FPGA_FILE, OW_SD_READ) != OW_OK) return false;
    const ow_status s = ow_sd_read(&f, head, sizeof head, &got);
    (void)ow_sd_close(&f);
    return s == OW_OK && got == sizeof head && memcmp(head, FWFPGA, sizeof head) == 0;
}

/* The embedded file in single-chunk pieces, each confirmed on its own (a
 * multi-chunk put can drop a chunk on the display link; FW-TPG). */
static bool write_sd_file(void) {
    (void)ow_sd_mkdir(&dev, FPGA_DIR);
    for (int attempt = 0; attempt < 3; attempt++) {
        size_t off = 0;
        ow_status s = OW_OK;
        do {
            const size_t left = FWFPGA_LEN - off, n = left < SDFS_MAX_PAYLOAD ? left : SDFS_MAX_PAYLOAD;
            if ((off & 0x3FFF) == 0) line(ROW_STATE, C_WHITE, "WRITING GATEWARE %u%%", (unsigned)(off * 100u / FWFPGA_LEN));
            s = ow_sd_put_mem(&dev, FPGA_FILE, FWFPGA + off, n, off != 0);
            off += n;
        } while (s == OW_OK && off < FWFPGA_LEN);
        if (s == OW_OK) return sd_file_current();
        DIAG("fwmame: put attempt %d failed at %u: status %d\n", attempt + 1, (unsigned)off, (int)s);
    }
    return false;
}

static bool load_design(void) {
    line(ROW_STATE, C_WHITE, "LOADING FPGA...");
    report_zones();
    if (!call("LOAD", "h\\f\\l \"" FPGA_FILE "\"", 10000)) return false;
    unsigned id = 0, version = 0;
    int cdone = 0;
    return sscanf(reply, "%u %u %d", &id, &version, &cdone) == 3 && id == FWFPGA_DESIGN_ID &&
           version == FWFPGA_VERSION && cdone == 1;
}

/* One h\f\w with all 8 input bytes (bare hex tokens); the write to 0x37 commits. */
static bool write_inputs(void) {
    char cmd[64];
    counter++;
    snprintf(cmd, sizeof cmd, "h\\f\\w %02X %02X %02X 00 00 00 00 00 %02X", REG_INPUTS, buttons & 0xFFu,
             buttons >> 8, counter);
    const uint64_t t0 = time_us_64();
    const bool ok = call("INPUTS", cmd, 500);
    last_ms = (uint32_t)((time_us_64() - t0) / 1000u);
    if (last_ms > worst_ms) worst_ms = last_ms;
    if (ok) writes++;
    else write_fails++;
    return ok;
}

static void start(void) {
    line(ROW_STATE, C_WHITE, "STARTING...");
    running = fbuf_running() || ((sd_file_current() || write_sd_file()) && load_design());
    if (running) running = write_inputs();
    line(ROW_STATE, running ? C_GREEN : C_RED, running ? "FBUF V%u RUNNING" : "FPGA START FAILED", FWFPGA_VERSION);
}

static bool fpga_zone_up(void) {
    const uint32_t bit = picpwr_zone_bit(PICPWR_ZONE_FPGA);
    picpwr_keep_awake(bit);
    for (uint32_t waited = 0; waited < 5000; waited += 10) {
        uint32_t rails = 0;
        if (picpwr_rails(&rails) && (rails & bit)) return true;
        fw2_app_recovery_sleep_ms(10);
    }
    return false;
}

int main(void) {
    board_init();
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_use_lcd();
    agentio_init();
    st7796_fill_screen(C_BLACK);
    st7796_draw_text(8, 8, 2, C_WHITE, C_BLACK, "FW-MAME");
    draw_buttons();
    board_backlight_set(1);
    ioexp_vref(VREF_3V3);   /* the header pins carry the video: no VIO, no picture */

    line(ROW_STATE, C_WHITE, "FPGA ZONE 6...");
    const bool fpga = fpga_zone_up();
    picpwr_release_unused();
    line(ROW_STATE, fpga ? C_GREY : C_RED, "FPGA ZONE 6 %s", fpga ? "ON" : "NOT ON");

    line(ROW_LINK, C_WHITE, "CONNECTING TO MAIN...");
    while (fw2_app_recovery_open_onewili(&dev) != OW_OK) {
        fw2_app_recovery_task();
        line(ROW_LINK, C_RED, "MAIN LINK FAILED, RETRY");
        fw2_app_recovery_sleep_ms(1000);
    }
    line(ROW_LINK, fw2_app_recovery_wrap_sd() == OW_OK ? C_GREY : C_RED, "MAIN LINK UP");
    report_zones();
    fw2_app_recovery_sleep_ms(100);
    start();

    uint64_t next_report_us = 0, next_check_us = time_us_64() + 2000000u, next_refresh_us = 0;
    for (;;) {
        fw2_app_recovery_task();
        agentio_task();

        bool changed = false;
        uartkbd_event_t ev;
        while (uartkbd_next_event(&ev)) {
            if (ev.btn >= UARTKBD_BTN_COUNT) continue;
            const uint16_t bit = (uint16_t)(1u << ev.btn);
            const uint16_t next = ev.pressed ? (uint16_t)(buttons | bit) : (uint16_t)(buttons & ~bit);
            if (next != buttons) { buttons = next; changed = true; }
        }
        const uint64_t now = time_us_64();
        if (changed || now >= next_refresh_us) {
            if (changed) draw_buttons();
            if (running && !write_inputs()) DIAG("fwmame: input write failed\n");
            next_refresh_us = time_us_64() + REFRESH_US;
            line(ROW_STATS, C_GREY, "WR %lu  FAIL %lu  %lu/%lu MS", (unsigned long)writes,
                 (unsigned long)write_fails, (unsigned long)last_ms, (unsigned long)worst_ms);
        }
        if (now >= next_report_us) {
            report_zones();
            next_report_us = now + 1000000u;
        }
        if (now >= next_check_us) {
            if (!running || !fbuf_running()) {
                DIAG("fwmame: fbuf not running, restarting\n");
                start();
            }
            next_check_us = time_us_64() + 2000000u;
        }
        fw2_app_recovery_sleep_ms(2);
    }
}
