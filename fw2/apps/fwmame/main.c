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
 * USB controller (input bytes 2-5): a joystick or pad on one of the FW2's USB
 * host ports. Those sit behind the on-board CH334F hub on this CPU's native
 * USB controller (zone 8, ioexp_usb_pwr); usbhost/hh_usb.c drives it, low and
 * full speed. (wilibsp's PIO-USB host is on GPIO42/43, which the FW2 wires as
 * a USB device port, not to these ports.) Xbox 360-style XInput pads map
 * directly. Any other HID joystick or arcade encoder
 * needs a map from its raw report to the 12 pad controls, learnt once on the
 * FW2 (prompts on screen, GREY skips a control the stick doesn't have) and
 * saved per VID:PID as /appdata/fwmame/pad_VVVVPPPP.map. Learning starts by
 * itself for a stick without a map; the LEARN touch button redoes it.
 *
 * All buttons go to the game, including HOME and PAGE; holding HOME for 5 s
 * still leaves the app and holding PAGE for 5 s shows About (FW2App
 * contract). The labels over the five coloured buttons show FW-MAME's
 * default mapping, which lives on the CM0. */
#include "fw2.h"
#include "platform/diag.h"
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/watchdog.h"
#include "onewili.h"
#include "onewili_fwgui.h"
#include "onewili_sd.h"
#include "input/app_recovery_onewili.h"
#include "input/ft6336.h"
#include "usbhost/hh_usb.h"
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

enum { ROW_STATE, ROW_LINK, ROW_STATS, ROW_USB, ROW_RAW, ROW_COUNT };
#define ROW_Y(r) (26 + (r) * 18)

static void line(int row, uint16_t fg, const char *fmt, ...) {
    char s[40];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    st7796_fill_rect(8, ROW_Y(row), 464, 16, C_BLACK);
    st7796_draw_text(8, ROW_Y(row), 2, fg, C_BLACK, s);
}

static void line_at(int y, uint16_t fg, const char *fmt, ...) {
    char s[30];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    st7796_fill_rect(8, y, 464, 16, C_BLACK);
    st7796_draw_text(8, y, 2, fg, C_BLACK, s);
}

/* ---- game menu state (the list itself comes from the CM0 through the FPGA) ---- */
static uint8_t requested;             /* game asked for (input byte 6), 0 = menu */
static uint8_t cm0_running;           /* game the CM0 reports running, 0 = none */
static int game_count = -1;           /* -1 = list not read yet */
static uint8_t game_serial;
static char game_names[63][32];
static int cursor, list_top;
static uint8_t failed_game;           /* last game the CM0 couldn't start, shown until the next pick */
#define LIST_Y     118                 /* header line, then LIST_ROWS games */
#define LIST_ROW_H 20
#define LIST_ROWS  5

/* Touch target: learn the USB stick's controls again (shown with a HID stick). */
#define LEARN_X 8
#define LEARN_Y 250
#define LEARN_W 180
#define LEARN_H 26

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

/* Only the five coloured buttons are shown (the menu takes the rest of the screen). */
static void draw_buttons(void) {
    for (int b = 0; b <= UARTKBD_BTN_RED; b++)
        if (((shown ^ buttons) >> b) & 1u) draw_bar_button(b, (buttons >> b) & 1u);
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

/* ======================================================= USB controller */
/* Pad controls in FWFB_PAD_* order (cm0/fb/fwfb.h), named as on the screen. */
#define PAD_COUNT 12
static const char *const pad_name[PAD_COUNT] = {"UP", "DOWN", "LEFT", "RIGHT", "B1", "B2",
                                                "B3", "B4", "B5", "B6", "COIN", "START"};

/* How one control shows in a raw HID report. */
enum { OP_NONE, OP_BIT, OP_BIT0, OP_LT, OP_GT, OP_EQ, OP_HAT };
typedef struct { uint8_t byte, op, mask, value; } pad_rule;
#define MAP_MAGIC 0x31504D46u   /* "FMP1" */
typedef struct {
    uint32_t magic;
    uint16_t vid, pid;
    uint16_t len, reserved;
    pad_rule rule[PAD_COUNT];
} pad_map;

static uint8_t hid_rep[64], hid_rest[64];
static uint16_t hid_len, hid_rest_len, hid_vid, hid_pid;
static bool hid_new, hid_present;
typedef struct { uint16_t buttons; int16_t lx, ly; } xinput_pad;   /* XINPUT_GAMEPAD_* bits */
static xinput_pad xpad;
static bool x_new, x_present;
static pad_map map;
static bool map_ok;
static uint16_t pad_bits;
static int8_t stick_x, stick_y;
static int learn = -1;                /* control being learnt, -1 = not learning */
static bool learn_wait;               /* learning starts at the stick's next report */
static bool learn_release;            /* waiting for the stick to return to rest */
static bool usb_ok;

/* Newest report from the controller, if any (call every loop pass). */
static void usb_read_reports(void) {
    uint8_t b[64];
    const hh_status_t *s = hh_usb_status();
    const int n = hh_usb_report(b, sizeof b);
    if (n <= 0) return;
    if (s->kind == HH_KIND_HID) {
        memcpy(hid_rep, b, (size_t)n);
        hid_len = (uint16_t)n;
        hid_new = true;
        if (s->reports <= 20)
            DIAG("fwmame: report %u len %d: %02x %02x %02x %02x %02x %02x %02x %02x\n", (unsigned)s->reports, n,
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    } else if (s->kind == HH_KIND_XINPUT && n >= 10 && b[0] == 0x00 && b[1] >= 0x14) {   /* input message */
        xpad.buttons = (uint16_t)(b[2] | (b[3] << 8));
        xpad.lx = (int16_t)(b[6] | (b[7] << 8));
        xpad.ly = (int16_t)(b[8] | (b[9] << 8));
        x_new = true;
    }
}

static void map_path(char *p, size_t n) {
    snprintf(p, n, "/appdata/fwmame/pad_%04X%04X.map", hid_vid, hid_pid);
}

static bool map_load(void) {
    char path[48];
    ow_sd_file f;
    size_t got = 0;
    map_path(path, sizeof path);
    if (ow_sd_open(&dev, &f, path, OW_SD_READ) != OW_OK) return false;
    const ow_status s = ow_sd_read(&f, &map, sizeof map, &got);
    (void)ow_sd_close(&f);
    return s == OW_OK && got == sizeof map && map.magic == MAP_MAGIC && map.vid == hid_vid && map.pid == hid_pid;
}

static bool map_save(void) {
    char path[48];
    map_path(path, sizeof path);
    (void)ow_sd_mkdir(&dev, "/appdata");
    (void)ow_sd_mkdir(&dev, "/appdata/fwmame");
    return ow_sd_put_mem(&dev, path, (const uint8_t *)&map, sizeof map, false) == OW_OK;
}

static bool axis_like(uint8_t rest) { return rest >= 0x40 && rest <= 0xC0; }

/* Hat value h (0 = up, clockwise, 8+ = centred) against a direction control. */
static bool hat_hit(unsigned h, int control) {
    if (h > 7) return false;
    switch (control) {
        case 0: return h == 7 || h <= 1;   /* up */
        case 1: return h >= 3 && h <= 5;   /* down */
        case 2: return h >= 5;             /* left */
        case 3: return h >= 1 && h <= 3;   /* right */
        default: return false;
    }
}

static bool rule_hit(const pad_rule *r, int control, const uint8_t *rep, uint16_t len) {
    if (r->op == OP_NONE || r->byte >= len) return false;
    const uint8_t b = rep[r->byte];
    switch (r->op) {
        case OP_BIT:  return (b & r->mask) != 0;
        case OP_BIT0: return (b & r->mask) == 0;
        case OP_LT:   return b < r->value;
        case OP_GT:   return b > r->value;
        case OP_EQ:   return (b & r->mask) == r->value;
        case OP_HAT:  return hat_hit(r->mask == 0xF0 ? (unsigned)(b >> 4) : (unsigned)(b & 0x0F), control);
        default:      return false;
    }
}

/* The report is back at rest (axes within 0x20 of centre, everything else exact). */
static bool at_rest(const uint8_t *rep) {
    for (uint16_t i = 0; i < hid_rest_len; i++) {
        const int d = (int)rep[i] - (int)hid_rest[i];
        if (axis_like(hid_rest[i]) ? (d > 0x20 || d < -0x20) : d != 0) return false;
    }
    return true;
}

/* What changed from rest, as a rule for `control`; false while nothing clear has. */
static bool classify(const uint8_t *rep, int control, pad_rule *out) {
    for (uint16_t i = 0; i < hid_rest_len; i++) {
        const uint8_t r0 = hid_rest[i], v = rep[i], d = (uint8_t)(r0 ^ v);
        const int delta = (int)v - (int)r0;
        if (!d) continue;
        if (axis_like(r0) && delta < 0x40 && delta > -0x40) continue;   /* axis noise */
        out->byte = (uint8_t)i;
        /* Directions: a hat first (centred 8+, 0-7 = direction). Up is 8 -> 0, a
         * one-bit change, so this test must come before the one-bit one. */
        if (control < 4 && !(d & 0xF0) && (r0 & 0x0F) >= 8 && (v & 0x0F) <= 7) {
            out->op = OP_HAT; out->mask = 0x0F;                       /* hat in the low nibble */
        } else if (control < 4 && !(d & 0x0F) && (r0 >> 4) >= 8 && (v >> 4) <= 7) {
            out->op = OP_HAT; out->mask = 0xF0;
        } else if ((d & (d - 1)) == 0) {                              /* one bit */
            out->op = (v & d) ? OP_BIT : OP_BIT0;
            out->mask = d;
        } else if (axis_like(r0)) {
            out->op = delta < 0 ? OP_LT : OP_GT;                      /* axis: threshold halfway */
            out->value = (uint8_t)((r0 + v) / 2);
        } else {
            out->op = OP_EQ; out->mask = d; out->value = (uint8_t)(v & d);
        }
        return true;
    }
    return false;
}

static void show_learn(void) {
    if (learn < 0) return;
    line(ROW_RAW, C_CYAN, "PRESS %s  (GREY SKIPS)", pad_name[learn]);
}

static void learn_start(void) {
    if (!hid_present) return;
    /* The rest state: the stick's newest interrupt report. Without one, ask
     * it (GET_REPORT), but only trust an answer of the report's size: some
     * sticks answer GET_REPORT in another layout (050D:0803: 3 bytes for a
     * 7-byte report). */
    learn_wait = false;
    if (hid_len) {
        memcpy(hid_rest, hid_rep, hid_len);
        hid_rest_len = hid_len;
    } else {
        const int n = hh_usb_get_report(hid_rest, sizeof hid_rest);
        DIAG("fwmame: learn start, GET_REPORT %d bytes (endpoint %u)\n", n, hh_usb_status()->mps);
        if (n <= 0 || n != hh_usb_status()->mps) {
            learn_wait = true;                       /* start at the first interrupt report */
            line(ROW_RAW, C_WHITE, "WAITING FOR THE STICK...");
            return;
        }
        hid_rest_len = (uint16_t)n;
        memcpy(hid_rep, hid_rest, (size_t)n);
        hid_len = (uint16_t)n;
    }
    DIAG("fwmame: learn start, rest %u bytes: %02x %02x %02x %02x %02x %02x %02x\n", hid_rest_len, hid_rest[0],
         hid_rest[1], hid_rest[2], hid_rest[3], hid_rest[4], hid_rest[5], hid_rest[6]);
    memset(&map, 0, sizeof map);
    map.magic = MAP_MAGIC;
    map.vid = hid_vid;
    map.pid = hid_pid;
    map.len = hid_len;
    map_ok = false;
    learn = 0;
    learn_release = false;
    show_learn();
}

static void learn_next(void) {
    learn_release = false;
    if (++learn < PAD_COUNT) { show_learn(); return; }
    learn = -1;
    map_ok = true;
    const bool saved = map_save();
    line(ROW_RAW, saved ? C_GREEN : C_RED, saved ? "MAP SAVED" : "MAP SAVE FAILED");
}

static void learn_report(void) {
    if (hid_len < hid_rest_len) hid_rest_len = hid_len;   /* compare only what both carry */
    if (hid_len == 0) return;
    if (learn_release) {
        if (at_rest(hid_rep)) learn_next();
        return;
    }
    pad_rule r = {0};
    if (classify(hid_rep, learn, &r)) {
        map.rule[learn] = r;
        learn_release = true;
        line(ROW_RAW, C_GREEN, "%s: BYTE %u OP %u %02X %02X", pad_name[learn], r.byte, r.op, r.mask, r.value);
    }
}

static void show_usb(void) {
    char raw[30] = "";
    const hh_status_t *s = hh_usb_status();
    if (!x_present && !hid_present) {                    /* diagnostics: hub ports, last device */
        if (!usb_ok) {
            line(ROW_USB, C_RED, "USB PORTS NOT POWERED");
        } else if (s->state == HH_ST_ROOT_WAIT || s->state == HH_ST_FAILED) {
            line(ROW_USB, C_RED, "USB HUB %s ERR %d", s->state == HH_ST_FAILED ? "FAILED" : "WAIT", s->last_error);
        } else {
            int k = snprintf(raw, sizeof raw, "HUB");
            for (int p = 1; p <= s->n_ports && p <= 4; p++)
                k += snprintf(raw + k, sizeof raw - (size_t)k, " %u:%04X", p, s->port_status[p]);
            line(ROW_USB, C_GREY, "%s", raw);
        }
        if (s->port && s->kind == HH_KIND_OTHER)
            line(ROW_RAW, C_RED, "%04X:%04X CL %02X/%02X/%02X ?", s->vid, s->pid, s->itf_class, s->itf_subclass,
                 s->itf_protocol);
        else if (s->port && s->last_error)
            line(ROW_RAW, C_RED, "PORT %u %s ERR %d", s->port, s->low_speed ? "LS" : "FS", s->last_error);
        else
            line(ROW_RAW, C_BLACK, "");
        return;
    }
    if (x_present) {
        line(ROW_USB, C_GREY, "USB XINPUT PAD %03X", pad_bits);
    } else if (learn >= 0) {                             /* learning: live report count and bytes */
        int k = snprintf(raw, sizeof raw, "R%lu L%u/%u", (unsigned long)s->reports, hid_len, hid_rest_len);
        for (uint16_t i = 0; i < hid_len && i < 5 && k < (int)sizeof raw - 3; i++)
            k += snprintf(raw + k, sizeof raw - (size_t)k, " %02X", hid_rep[i]);
        line(ROW_USB, C_WHITE, "%s", raw);
    } else {
        if (map_ok) line(ROW_USB, C_GREY, "%04X:%04X %s PAD %03X", hid_vid, hid_pid, s->low_speed ? "LS" : "FS", pad_bits);
        else line(ROW_USB, C_RED, "%04X:%04X %s NO MAP", hid_vid, hid_pid, s->low_speed ? "LS" : "FS");
        for (uint16_t i = 0; i < hid_len && i < 9; i++) snprintf(raw + 3 * i, sizeof raw - 3 * i, "%02X ", hid_rep[i]);
        if (learn < 0) line(ROW_RAW, C_GREY, "%s", raw);
    }
}

static void draw_learn_button(void) {
    const bool on = hid_present && !x_present;
    st7796_fill_rect(LEARN_X, LEARN_Y, LEARN_W, LEARN_H, on ? C_GREY : C_BLACK);
    if (on) st7796_draw_text(LEARN_X + 10, LEARN_Y + 5, 2, C_BLACK, C_GREY, "LEARN STICK");
}

/* Pad bits and stick from whichever controller is present. */
static void pad_update(void) {
    uint16_t bits = 0;
    int8_t sx = 0, sy = 0;
    if (x_present) {
        static const uint16_t xbit[PAD_COUNT] = {0x0001, 0x0002, 0x0004, 0x0008, 0x1000, 0x2000,
                                                 0x4000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0010};
        for (int i = 0; i < PAD_COUNT; i++) if (xpad.buttons & xbit[i]) bits |= (uint16_t)(1u << i);
        sx = (int8_t)(xpad.lx >> 8);
        sy = (int8_t)(-(xpad.ly >> 8) < 127 ? -(xpad.ly >> 8) : 127);
        if (xpad.ly > 16000) bits |= 1u << 0;
        if (xpad.ly < -16000) bits |= 1u << 1;
        if (xpad.lx < -16000) bits |= 1u << 2;
        if (xpad.lx > 16000) bits |= 1u << 3;
    } else if (hid_present && map_ok && learn < 0) {
        for (int i = 0; i < PAD_COUNT; i++)
            if (rule_hit(&map.rule[i], i, hid_rep, hid_len)) bits |= (uint16_t)(1u << i);
    }
    pad_bits = bits;
    stick_x = sx;
    stick_y = sy;
}

/* One h\f\w with all 8 input bytes (bare hex tokens); the write to 0x37 commits.
 * In a game, OK (the check mark) also presses B1 (GREEN) and CANCEL (the X) also
 * presses B2 (BLUE). Byte 6 is the game requested from the menu. */
static bool write_inputs(void) {
    char cmd[64];
    uint16_t b = buttons;
    if (requested) {
        if (b >> UARTKBD_BTN_OK & 1u) b |= 1u << UARTKBD_BTN_GREEN;
        if (b >> UARTKBD_BTN_CANCEL & 1u) b |= 1u << UARTKBD_BTN_BLUE;
    }
    counter++;
    snprintf(cmd, sizeof cmd, "h\\f\\w %02X %02X %02X %02X %02X %02X %02X %02X %02X", REG_INPUTS, b & 0xFFu,
             b >> 8, pad_bits & 0xFFu, pad_bits >> 8, (uint8_t)stick_x, (uint8_t)stick_y, requested, counter);
    const uint64_t t0 = time_us_64();
    const bool ok = call("INPUTS", cmd, 500);
    last_ms = (uint32_t)((time_us_64() - t0) / 1000u);
    if (last_ms > worst_ms) worst_ms = last_ms;
    if (ok) writes++;
    else write_fails++;
    return ok;
}

/* ======================================================= audio
 * The CM0 sends 16-bit mono samples at 16 kHz in AUDIO packets; the FPGA
 * (fbuf v003+) puts them in the shell's 4 KB stream FIFO. This pulls them
 * through MAIN (h\f\r 64 = byte count, h\f\x n = up to 256 bytes as hex) and
 * writes them into a ring that a looping DMA plays to the NAU88C10 at 16 kHz.
 * The write position is kept ~75 ms ahead of the DMA: one sample in 32 is
 * repeated when the ring runs low and dropped when it runs high, which takes
 * up the rate difference between the emulator and this CPU's codec clock.
 * Silence is kept ahead of the write position, so an underrun is quiet
 * instead of replaying the ring. */
#define AU_FRAMES  4096u               /* 256 ms; power of two, ring-aligned */
#define AU_TARGET  1200u               /* 75 ms ahead of the DMA */
#define AU_GUARD   256u                /* frames of silence kept ahead */
/* Volume: 11 levels set by the VOL-/VOL+ touch buttons, gain = vol_gain[level]/16.
 * The top is unity (full-scale samples, no boost), so the 0.5 W speaker never
 * gets more than the codec setup allows. Level 6 = half, the old fixed level. */
static const uint8_t vol_gain[11] = {0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 16};
static int vol_level = 6;
#define VOL_Y   250
#define VOL_H   26
#define VOLM_X  200                    /* VOL- box */
#define VOLP_X  370                    /* VOL+ box */
#define VOL_W   80

static void draw_volume(void) {
    st7796_fill_rect(VOLM_X, VOL_Y, VOL_W, VOL_H, C_GREY);
    st7796_draw_text(VOLM_X + 16, VOL_Y + 5, 2, C_BLACK, C_GREY, "VOL-");
    st7796_fill_rect(VOLP_X, VOL_Y, VOL_W, VOL_H, C_GREY);
    st7796_draw_text(VOLP_X + 16, VOL_Y + 5, 2, C_BLACK, C_GREY, "VOL+");
    char s[8];
    snprintf(s, sizeof s, "%2d", vol_level);
    st7796_fill_rect(VOLM_X + VOL_W, VOL_Y, VOLP_X - VOLM_X - VOL_W, VOL_H, C_BLACK);
    st7796_draw_text(VOLM_X + VOL_W + 26, VOL_Y + 5, 2, C_WHITE, C_BLACK, s);
}
static uint32_t au_ring[AU_FRAMES] __attribute__((aligned(AU_FRAMES * 4)));
static int au_ch[2] = {-1, -1};        /* the two chained DMA channels reading the ring */
static uint32_t au_wp;                 /* next frame index to write */
static bool audio_ok, au_have_low;
static uint8_t au_low;                 /* a sample's low byte left over from the last read */
static uint32_t au_bytes, au_reads, au_under, au_drops, au_dups, au_last_ms, au_worst_ms, au_n, au_resync;
static uint64_t au_next_us, au_rate_t0;
static uint32_t au_rate_bytes, au_rate;   /* bytes/s over the last second */
static int au_lo = 32767, au_hi = -32768, au_pp;   /* sample range; au_pp = peak-to-peak over the last second */

static uint32_t au_pos(void) {
    for (int i = 0; i < 2; i++) {
        if (au_ch[i] < 0 || !dma_channel_is_busy((uint)au_ch[i])) continue;
        return (uint32_t)((dma_hw->ch[au_ch[i]].read_addr - (uintptr_t)au_ring) / 4u) & (AU_FRAMES - 1u);
    }
    return au_wp;
}

static void audio_init(void) {
    const uint32_t bit = picpwr_zone_bit(PICPWR_ZONE_AUDIO);
    picpwr_keep_awake(bit);
    bool up = false;
    for (uint32_t waited = 0; waited < 3000 && !up; waited += 25) {
        uint32_t rails = 0;
        up = picpwr_rails(&rails) && (rails & bit);
        if (!up) { picpwr_task(); fw2_app_recovery_sleep_ms(25); }
    }
    if (!up) { DIAG("fwmame: audio zone not up\n"); return; }
    codec_nau88c10_init();
    codec_nau88c10_dac_mute(false);
    audio_i2s_duplex_init(16000);
    codec_nau88c10_set_output(CODEC_OUT_SPEAKER);
    memset(au_ring, 0, sizeof au_ring);
    audio_i2s_duplex_play_loop(au_ring, AU_FRAMES);
    int k = 0;                                   /* find the DMA channels reading the ring */
    for (uint ch = 0; ch < NUM_DMA_CHANNELS && k < 2; ch++) {
        const uintptr_t ra = dma_hw->ch[ch].read_addr;
        if (ra >= (uintptr_t)au_ring && ra <= (uintptr_t)(au_ring + AU_FRAMES)) au_ch[k++] = (int)ch;
    }
    au_wp = (au_pos() + AU_TARGET) & (AU_FRAMES - 1u);
    audio_ok = k > 0;
    DIAG("fwmame: audio %s, DMA channels %d %d\n", audio_ok ? "up" : "DMA not found", au_ch[0], au_ch[1]);
}

static void au_put(int16_t s) {
    const int16_t v = (int16_t)((s * vol_gain[vol_level]) / 16);
    au_ring[au_wp] = ((uint32_t)(uint16_t)v << 16) | (uint16_t)v;
    au_wp = (au_wp + 1u) & (AU_FRAMES - 1u);
}

/* Each sample is 14 bits in two self-marking bytes (cm0/fb/fwfb.h): low byte
 * bit 0 = 0, high byte bit 0 = 1. A byte lost in a full FIFO costs one sample;
 * the next low byte re-locks the stream. */
static void au_samples(const uint8_t *b, int n) {
    for (int i = 0; i < n; i++) {
        if (!(b[i] & 1u)) { au_low = b[i] >> 1; au_have_low = true; continue; }
        if (!au_have_low) { au_resync++; continue; }            /* a high byte without its low byte */
        au_have_low = false;
        int v = au_low | ((b[i] >> 1) << 7);                     /* 14-bit two's complement */
        if (v & 0x2000) v -= 0x4000;
        const int16_t s = (int16_t)(v << 2);
        if (s < au_lo) au_lo = s;
        if (s > au_hi) au_hi = s;
        const uint32_t level = (au_wp - au_pos()) & (AU_FRAMES - 1u);
        au_n++;
        if (level > AU_FRAMES - AU_GUARD) {            /* the DMA passed us: start over ahead of it */
            au_under++;
            au_wp = (au_pos() + AU_TARGET) & (AU_FRAMES - 1u);
        } else if (level > 2 * AU_TARGET && (au_n & 31u) == 0) {
            au_drops++;
            continue;                                  /* too far ahead: drop this one */
        } else if (level < AU_TARGET / 2 && (au_n & 31u) == 0) {
            au_dups++;
            au_put(s);                                 /* running low: play this one twice */
        }
        au_put(s);
    }
    for (uint32_t k = 0, w = au_wp; k < AU_GUARD; k++, w = (w + 1u) & (AU_FRAMES - 1u)) au_ring[w] = 0;
}

static int hex_bytes(const char *s, uint8_t *out, int max) {
    int n = 0, nib = -1;
    for (; *s && n < max; s++) {
        int v = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10
              : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 : -1;
        if (v < 0) continue;
        if (nib < 0) nib = v;
        else { out[n++] = (uint8_t)(nib << 4 | v); nib = -1; }
    }
    return n;
}

/* The DMA loops over the whole ring forever, so when the samples stop (HOME
 * stops the game, QUIT, a link hiccup) it would replay the last 256 ms over and
 * over. On a tick that brings no samples, zero everything the DMA has already
 * played: from the write point up to just behind the read point. Those frames
 * are stale, so this is always safe. Once the DMA overtakes the write point,
 * all of the ring is stale. */
static bool au_clear_stale(void) {                /* true: the whole ring is silent now */
    const uint32_t pos = au_pos(), level = (au_wp - pos) & (AU_FRAMES - 1u);
    if (level > AU_FRAMES - AU_GUARD) { memset(au_ring, 0, sizeof au_ring); return true; }
    const uint32_t margin = 8u;                    /* stay clear of the frames being read */
    const uint32_t stale = AU_FRAMES - level;
    for (uint32_t k = 0, w = au_wp; k + margin < stale; k++, w = (w + 1u) & (AU_FRAMES - 1u)) au_ring[w] = 0;
    return false;                                  /* samples still queued: clear again next tick */
}

static void audio_poll(uint64_t now);

static void audio_task(void) {
    const uint64_t now = time_us_64();
    if (!audio_ok || now < au_next_us) return;
    au_next_us = now + 4000u;
    static bool clean = true;                      /* the whole ring is silent */
    const uint32_t n0 = au_n;
    audio_poll(now);
    if (au_n != n0) clean = false;
    else if (!clean) clean = au_clear_stale();
}

static void audio_poll(uint64_t now) {
    if (!running) return;
    static bool flushed;
    if (!flushed) flushed = call("AUFLUSH", "h\\f\\w 63 01", 300);   /* start clean: drop what piled up */
    uint8_t b[260];
    if (!call("AUCNT", "h\\f\\r 64 2", 200) || hex_bytes(reply, b, 2) != 2) return;
    unsigned avail = b[0] | (unsigned)b[1] << 8;
    for (int chunk = 0; chunk < 4 && avail > 0; chunk++) {
        const unsigned n = avail > 256 ? 256 : avail;
        char cmd[24];
        snprintf(cmd, sizeof cmd, "h\\f\\x %u", n);
        const uint64_t t0 = time_us_64();
        if (!call("AUX", cmd, 300)) return;
        au_last_ms = (uint32_t)((time_us_64() - t0) / 1000u);
        if (au_last_ms > au_worst_ms) au_worst_ms = au_last_ms;
        const int got = hex_bytes(reply, b, sizeof b);
        au_reads++;
        au_bytes += (uint32_t)got;
        au_rate_bytes += (uint32_t)got;
        au_samples(b, got);
        avail -= n;
    }
    if (now - au_rate_t0 >= 1000000u) {
        au_rate = au_rate_bytes;
        au_rate_bytes = 0;
        au_pp = au_hi >= au_lo ? au_hi - au_lo : 0;
        au_lo = 32767;
        au_hi = -32768;
        au_rate_t0 = now;
    }
}

/* ======================================================= game menu
 * The CM0 (cm0/fe, `run.sh` with no game) writes its game list into the FPGA's
 * menu memory (fbuf v004): page 0 = {count, serial, running game}, page n =
 * game n's name. Reading a page: write its number to 0x39, read 0x40-0x5F.
 * Picking a game sets input byte 6 to its number; HOME sets it back to 0. */
static bool menu_page(int page, uint8_t out[32]) {
    char cmd[24];
    snprintf(cmd, sizeof cmd, "h\\f\\w 39 %02X", page);
    if (!call("MPAGE", cmd, 300)) return false;
    sleep_us(50);                                     /* the window fills ~1 us after the write */
    return call("MREAD", "h\\f\\r 40 32", 300) && hex_bytes(reply, out, 32) == 32;
}

static const char *game_name(int id) {               /* 1-based */
    return (id >= 1 && id <= game_count) ? game_names[id - 1] : "?";
}

/* The games, then QUIT FW-MAME (once the list has been read). */
static int menu_items(void) { return game_count < 0 ? 0 : game_count + 1; }

static void draw_menu(void) {
    st7796_fill_rect(0, LIST_Y, ST7796_W, LIST_ROW_H * (LIST_ROWS + 1), C_BLACK);
    if (requested && cm0_running == requested)
        line_at(LIST_Y, C_GREEN, "PLAYING %s  HOME=MENU", game_name(requested));
    else if (requested)
        line_at(LIST_Y, C_WHITE, "STARTING %s...", game_name(requested));
    else if (cm0_running)
        line_at(LIST_Y, C_WHITE, "STOPPING %s...", game_name(cm0_running));
    else if (game_count < 0)
        line_at(LIST_Y, C_WHITE, "READING GAME LIST...");
    else if (failed_game)
        line_at(LIST_Y, C_RED, "%s FAILED", game_name(failed_game));
    else if (game_count == 0)
        line_at(LIST_Y, C_RED, "NO GAMES IN ~/FWMAME/ROMS");
    else
        line_at(LIST_Y, C_CYAN, "SELECT GAME, OK TO PLAY");
    const int items = menu_items();
    for (int i = 0; i < LIST_ROWS; i++) {
        const int idx = list_top + i, y = LIST_Y + LIST_ROW_H * (i + 1);
        if (idx >= items) break;
        const bool sel = idx == cursor && !requested;
        const bool quit = idx == game_count;         /* the last item */
        const bool on = requested == idx + 1;
        const uint16_t bg = sel ? C_CYAN : C_BLACK, fg = sel ? C_BLACK : on ? C_GREEN : quit ? C_GREY : C_WHITE;
        st7796_fill_rect(0, y - 2, ST7796_W, LIST_ROW_H, bg);
        char s[30];
        snprintf(s, sizeof s, "%c %s", on ? '>' : ' ', quit ? "QUIT FW-MAME" : game_names[idx]);
        st7796_draw_text(8, y, 2, fg, bg, s);
    }
}

static void menu_move(int delta) {
    const int items = menu_items();
    if (items <= 0) return;
    cursor += delta;
    if (cursor < 0) cursor = 0;
    if (cursor >= items) cursor = items - 1;
    if (cursor < list_top) list_top = cursor;
    if (cursor >= list_top + LIST_ROWS) list_top = cursor - LIST_ROWS + 1;
    draw_menu();
}

/* Re-read the list when the CM0 republishes it (new serial) and follow the running game. */
static void menu_poll(void) {
    uint8_t p[32];
    if (!menu_page(0, p)) return;
    bool redraw = false;
    if (game_count < 0 || p[1] != game_serial) {
        const int n = p[0] > 63 ? 63 : p[0];
        int got = 0;
        for (; got < n; got++) {
            uint8_t name[32];
            if (!menu_page(got + 1, name)) break;
            int k = 0;
            for (; k < 26 && name[k]; k++) game_names[got][k] = (name[k] >= 32 && name[k] < 127) ? (char)name[k] : '?';
            game_names[got][k] = '\0';
        }
        if (got == n) {
            game_count = n;
            game_serial = p[1];
            if (cursor > game_count) cursor = 0;       /* game_count = the QUIT item */
            if (list_top > cursor) list_top = cursor;
            redraw = true;
        }
    }
    if (p[2] != cm0_running) { cm0_running = p[2]; redraw = true; }
    if (requested && p[3] == requested && !cm0_running) {   /* the CM0 couldn't start it */
        failed_game = requested;
        requested = 0;
        redraw = true;
        write_inputs();
    }
    if (redraw) draw_menu();
}

/* ======================================================= CM0 power and the game service
 * Launching the app from the FW2's Apps menu has to bring up the CM0 side too:
 *   1. the CM0 on: MAIN's l\a (a toggle, so only when h\p\g shows it off; the
 *      app also holds zone 17, so its power frames don't switch it back off);
 *   2. while the stock FPGA and fwcm0-bridge still run, a MAIN Linux-shell
 *      session (l\c / l\w / l\r / l\e) runs `systemctl start --no-block
 *      fwmame` (cm0/fe/fwmame.service: stops the bridge, runs the game menu
 *      supervisor, and stops by itself once the FPGA goes quiet);
 *   3. then fbuf is loaded. */
#define SHELL_TOKEN "F3A3E001"

/* h\p\g: is rail 17 (the CM0) on? Only the rail: the CM0_RUNPG readback said
 * "held in reset" while Linux was running (wilibsp: that readback is not
 * established), so Linux itself is proven by the shell check that follows. */
static bool cm0_running_now(void) {
    if (!call("ZONES", "h\\p\\g", 2000)) return false;
    const char *z = strstr(reply, "17:CM0");
    const char *eol = z ? strpbrk(z, "\r\n") : NULL;
    const char *on = z ? strstr(z, " on") : NULL;
    const bool up = on && (!eol || on < eol);
    DIAG("fwmame: CM0 rail %s\n", z ? (up ? "on" : "off") : "not in reply");
    return up;
}

static bool cm0_power_on(void) {
    picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_COMPUTE));
    if (cm0_running_now()) return true;
    line(ROW_STATE, C_WHITE, "TURNING ON THE CM0...");
    for (int attempt = 0; attempt < 2; attempt++) {  /* l\a toggles: if it answers "off", toggle again */
        if (call("CM0ON", "l\\a", 3000) && strstr(reply, "ON")) break;
    }
    for (int waited = 0; waited < 10000; waited += 250) {
        if (cm0_running_now()) return true;
        fw2_app_recovery_sleep_ms(250);
    }
    return false;
}

static void shell_write(const char *text) {
    char cmd[24 + 2 * 192];
    int k = snprintf(cmd, sizeof cmd, "l\\w " SHELL_TOKEN " ");
    for (const char *p = text; *p && k < (int)sizeof cmd - 3; p++) k += snprintf(cmd + k, sizeof cmd - (size_t)k, "%02X", (uint8_t)*p);
    call("SHW", cmd, 2000);
}

/* Wait for the CM0's shell through MAIN (Linux boots in ~30 s) and start the service. */
static bool start_game_service(void) {
    line(ROW_STATE, C_WHITE, "WAITING FOR CM0 LINUX...");
    for (int waited = 0; waited < 150000; waited += 3000) {
        call("SHE", "l\\e " SHELL_TOKEN, 1000);
        const bool opened = call("SHC", "l\\c " SHELL_TOKEN, 2000);
        DIAG("fwmame: shell open %d: '%.60s'\n", (int)opened, reply);
        if (opened) {
            shell_write("echo FWMAME_\"\"OK\n");     /* the echo, not the typed command, matches */
            static char acc[1600];                   /* hex of the shell output so far (reads may split it) */
            acc[0] = '\0';
            for (int i = 0; i < 8; i++) {
                fw2_app_recovery_sleep_ms(250);
                if (!call("SHR", "l\\r " SHELL_TOKEN " 192", 2000)) continue;
                DIAG("fwmame: shell read '%.48s'\n", reply);
                const char *hex = strchr(reply, ' ');   /* body: "<n> <hex>" */
                size_t k = strlen(acc);
                for (const char *p = hex ? hex + 1 : ""; *p && k < sizeof acc - 1; p++)
                    if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f'))
                        acc[k++] = (char)(*p >= 'a' ? *p - 32 : *p);
                acc[k] = '\0';
                if (strstr(acc, "46574D414D455F4F4B")) {             /* "FWMAME_OK": the shell answers */
                    line(ROW_STATE, C_WHITE, "STARTING THE GAME SERVICE...");
                    /* Detached, so closing this session (a hangup) can't kill it
                     * before the job is queued; then wait for "FWMAME_GO". */
                    shell_write("setsid sudo systemctl start --no-block fwmame </dev/null >/dev/null 2>&1 & "
                                "sleep 1; echo FWMAME_\"\"GO\n");
                    acc[0] = '\0';
                    bool go = false;
                    for (int j = 0; j < 40 && !go; j++) {   /* the login banner comes first */
                        fw2_app_recovery_sleep_ms(250);
                        if (!call("SHR", "l\\r " SHELL_TOKEN " 192", 2000)) continue;
                        const char *h2 = strchr(reply, ' ');
                        size_t m = strlen(acc);
                        for (const char *p = h2 ? h2 + 1 : ""; *p && m < sizeof acc - 1; p++)
                            if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f'))
                                acc[m++] = (char)(*p >= 'a' ? *p - 32 : *p);
                        acc[m] = '\0';
                        go = strstr(acc, "46574D414D455F474F") != NULL;
                    }
                    DIAG("fwmame: game service start %s\n", go ? "confirmed" : "NOT confirmed");
                    call("SHE", "l\\e " SHELL_TOKEN, 1000);
                    return go;
                }
            }
        }
        fw2_app_recovery_sleep_ms(3000);
    }
    return false;
}

static void start(void) {
    line(ROW_STATE, C_WHITE, "STARTING...");
    bool already = fbuf_running();
    static bool first = true;
    if (already && first) {
        /* fbuf left loaded by an earlier run (an older app, or a HOME-hold exit):
         * is the CM0's game service publishing? If no list appears in ~3 s,
         * go back to the stock FPGA and start everything properly. */
        bool alive = false;
        for (int i = 0; i < 6 && !alive; i++) {
            uint8_t p[32];
            alive = menu_page(0, p) && p[1] != 0;     /* serial 0 = never published */
            if (!alive) fw2_app_recovery_sleep_ms(500);
        }
        if (!alive) {
            DIAG("fwmame: fbuf loaded but no game service: restarting from the stock FPGA\n");
            line(ROW_STATE, C_WHITE, "RESTARTING FROM STOCK FPGA...");
            call("RESTORE", "h\\f\\t", 5000);
            already = false;
        }
    }
    first = false;
    DIAG("fwmame: start: fbuf %s\n", already ? "already running" : "not running");
    if (!already) {                                  /* a fresh launch: bring up the CM0 side first */
        const bool cm0 = cm0_power_on();
        const bool svc = cm0 && start_game_service();
        DIAG("fwmame: start: CM0 %d, game service %d\n", (int)cm0, (int)svc);
        if (!cm0) line(ROW_STATE, C_RED, "CM0 DID NOT TURN ON");
        else if (!svc) line(ROW_STATE, C_RED, "CM0 SHELL NOT REACHED");
        running = (sd_file_current() || write_sd_file()) && load_design();
    } else {
        running = true;
    }
    if (running) running = write_inputs();
    line(ROW_STATE, running ? C_GREEN : C_RED, running ? "FBUF V%u RUNNING" : "FPGA START FAILED", FWFPGA_VERSION);
}

/* QUIT: give the FW2 back. The stock FPGA returns; the CM0's game service sees
 * the FPGA go quiet, stops, and restarts fwcm0-bridge. Then leave the app the
 * way HOME-hold does (a watchdog reboot into the stock display firmware). */
static void quit_app(void) {
    line(ROW_STATE, C_WHITE, "RESTORING THE STOCK FPGA...");
    requested = 0;
    call("RESTORE", "h\\f\\t", 5000);
    line(ROW_STATE, C_GREY, "BYE");
    fw2_app_recovery_sleep_ms(300);
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
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

    audio_init();
    const bool touch_ok = ft6336_init();
    line(ROW_USB, C_WHITE, "USB PORTS...");
    {   /* zone 8 (USB hub) up, then port power, then the native host controller */
        const uint32_t bit = picpwr_zone_bit(PICPWR_ZONE_USB_HUB);
        picpwr_keep_awake(bit);
        for (uint32_t waited = 0; waited < 5000 && !usb_ok; waited += 25) {
            uint32_t rails = 0;
            usb_ok = picpwr_rails(&rails) && (rails & bit);
            if (!usb_ok) { picpwr_task(); fw2_app_recovery_sleep_ms(25); }
        }
        if (usb_ok) {
            ioexp_usb_pwr(true);
            hh_usb_init();
        }
    }
    DIAG("fwmame: touch %d, usb ports %d\n", (int)touch_ok, (int)usb_ok);
    show_usb();
    draw_learn_button();
    if (audio_ok) draw_volume();
    draw_menu();

    uint64_t next_report_us = 0, next_check_us = time_us_64() + 2000000u, next_refresh_us = 0;
    uint64_t next_usb_show_us = 0, next_menu_us = 0;
    bool was_touch = false, was_hid = false, was_x = false;
    for (;;) {
        fw2_app_recovery_task();
        agentio_task();
        picpwr_task();
        if (usb_ok) { hh_usb_task(); usb_read_reports(); }

        bool changed = false;
        uartkbd_event_t ev;
        while (uartkbd_next_event(&ev)) {
            if (ev.btn >= UARTKBD_BTN_COUNT) continue;
            const uint16_t bit = (uint16_t)(1u << ev.btn);
            const uint16_t next = ev.pressed ? (uint16_t)(buttons | bit) : (uint16_t)(buttons & ~bit);
            if (next != buttons) { buttons = next; changed = true; }
            if (learn >= 0 && ev.pressed && ev.btn == UARTKBD_BTN_GREY) {   /* skip this control */
                map.rule[learn].op = OP_NONE;
                learn_next();
            }
            if (!ev.pressed) continue;
            if (!requested) {                                   /* menu: d-pad picks, OK plays */
                if (ev.btn == UARTKBD_BTN_NAV_UP) menu_move(-1);
                else if (ev.btn == UARTKBD_BTN_NAV_DOWN) menu_move(1);
                else if (ev.btn == UARTKBD_BTN_NAV_LEFT) menu_move(-LIST_ROWS);
                else if (ev.btn == UARTKBD_BTN_NAV_RIGHT) menu_move(LIST_ROWS);
                else if (ev.btn == UARTKBD_BTN_OK && game_count >= 0 && cursor == game_count) {
                    quit_app();                                 /* QUIT FW-MAME */
                } else if (ev.btn == UARTKBD_BTN_OK && game_count > 0 && !cm0_running) {
                    requested = (uint8_t)(cursor + 1);
                    failed_game = 0;
                    changed = true;
                    draw_menu();
                }
            } else if (ev.btn == UARTKBD_BTN_HOME) {            /* in a game: HOME goes back to the menu */
                requested = 0;
                changed = true;
                draw_menu();
            }
        }

        /* USB controller: plug / unplug, reports, learning */
        if (usb_ok) {
            const hh_status_t *us = hh_usb_status();
            const bool dev = us->state == HH_ST_DEVICE;
            hid_present = dev && us->kind == HH_KIND_HID;
            x_present = dev && us->kind == HH_KIND_XINPUT;
            if (!hid_present && was_hid) { map_ok = false; learn = -1; learn_wait = false; hid_len = 0; }
            if (!x_present && was_x) { memset(&xpad, 0, sizeof xpad); x_new = true; }
            if (hid_present != was_hid || x_present != was_x) {
                if (hid_present && !was_hid) {
                    hid_vid = us->vid;
                    hid_pid = us->pid;
                    map_ok = map_load();
                    DIAG("fwmame: HID %04x:%04x, map %d\n", hid_vid, hid_pid, (int)map_ok);
                    if (!map_ok) learn_start();
                }
                was_hid = hid_present;
                was_x = x_present;
                draw_learn_button();
                show_usb();
                x_new = hid_new = true;
            }
            if (hid_new || x_new) {
                if (hid_new && learn_wait) learn_start();
                else if (hid_new && learn >= 0) learn_report();
                hid_new = x_new = false;
                const uint16_t old_bits = pad_bits;
                const int8_t old_x = stick_x, old_y = stick_y;
                pad_update();
                if (pad_bits != old_bits || stick_x != old_x || stick_y != old_y) changed = true;
            }
        }
        if (touch_ok) {
            uint16_t tx, ty;
            const bool down = ft6336_poll(&tx, &ty);
            if (down && !was_touch && hid_present && !x_present && tx >= LEARN_X && tx < LEARN_X + LEARN_W &&
                ty >= LEARN_Y && ty < LEARN_Y + LEARN_H)
                learn_start();
            static uint64_t vol_last_us;                 /* one step per tap: ignore bounces for 250 ms */
            if (down && !was_touch && ty + 4 >= VOL_Y && ty < VOL_Y + VOL_H + 4 &&
                time_us_64() - vol_last_us > 250000u) {   /* volume buttons */
                if (tx >= VOLM_X && tx < VOLM_X + VOL_W && vol_level > 0) { vol_level--; draw_volume(); }
                if (tx >= VOLP_X && tx < VOLP_X + VOL_W && vol_level < 10) { vol_level++; draw_volume(); }
                vol_last_us = time_us_64();
            }
            was_touch = down;
        }

        const uint64_t now = time_us_64();
        if (now >= next_usb_show_us) {   /* raw report and pad state, 4 times a second */
            show_usb();
            next_usb_show_us = now + 250000u;
        }
        if (changed || now >= next_refresh_us) {
            if (changed) draw_buttons();
            if (running && !write_inputs()) DIAG("fwmame: input write failed\n");
            next_refresh_us = time_us_64() + REFRESH_US;
            line(ROW_STATS, C_GREY, "WR %lu  FAIL %lu  %lu/%lu MS", (unsigned long)writes,
                 (unsigned long)write_fails, (unsigned long)last_ms, (unsigned long)worst_ms);
        }
        audio_task();
        if (running && now >= next_menu_us) {             /* game list and running game, twice a second */
            menu_poll();
            next_menu_us = time_us_64() + 500000u;
        }
        if (now >= next_report_us && audio_ok)          /* audio stats on the link line, once a second */
            line(ROW_LINK, C_GREY, "L%d AU %lu U%lu D%lu P%lu R%lu %luMS", au_pp, (unsigned long)au_rate,
                 (unsigned long)au_under, (unsigned long)au_drops, (unsigned long)au_dups,
                 (unsigned long)au_resync, (unsigned long)au_worst_ms);
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
        fw2_app_recovery_sleep_ms(1);
    }
}
