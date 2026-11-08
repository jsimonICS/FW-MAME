// hh_usb.c - see hh_usb.h. The hub handling follows wilibsp bsp/usbhost/usb_hub.c
// (MIT, (c) 2026 Dave Robins): confirmed-connect debounce, port reset, bounded
// enumeration retries. Differences: any device is enumerated (not only mass
// storage), low-speed devices are accepted (PRE packets, hh_hcd_set_ls), and
// the hardware-polled interrupt endpoint carries the device's reports, so the
// hub's own status pipe is replaced by GetPortStatus polling.
#include "hh_usb.h"
#include "hh_hcd.h"
#include <string.h>
#include "pico/stdlib.h"
#include "platform/diag.h"

#define HUB_ADDR 1
#define DEV_ADDR 2

enum { DT_DEVICE = 1, DT_CONFIG = 2, DT_INTERFACE = 4, DT_ENDPOINT = 5 };
enum { HUB_FEAT_PORT_RESET = 4, HUB_FEAT_PORT_POWER = 8, HUB_FEAT_C_PORT_CONNECTION = 16,
       HUB_FEAT_C_PORT_RESET = 20 };
#define PORT_STAT_CONNECTION 0x0001u
#define PORT_STAT_LOW_SPEED  0x0200u
#define PORT_CHG_CONNECTION  0x0001u
#define PORT_CHG_RESET       0x0010u

#define CONFIRM_POLLS 3
#define MAX_ATTEMPTS  3
#define POLL_MS       100

typedef struct {
    uint8_t config_value;
    bool    is_hub;
    uint8_t first_class, first_subclass, first_protocol;
    hh_kind_t kind;          // the input interface found, if any
    uint8_t itf, ep, mps, interval;
} cfg_t;

static hh_status_t st;
static uint8_t streak[8], attempts[8];
static bool blocked[8];
static absolute_time_t retry_at[8], next_poll;
static uint8_t hid_itf;

// ---------------------------------------------------------------- descriptors
static hh_result_t get_descriptor(uint8_t addr, uint8_t type, void *buf, uint16_t len, uint16_t *got) {
    const uint8_t setup[8] = {0x80, 6, 0, type, 0, 0, (uint8_t)len, (uint8_t)(len >> 8)};
    uint16_t n = len;
    hh_result_t r = hh_hcd_control_xfer(addr, setup, buf, &n);
    if (got) *got = n;
    return r;
}

static bool parse_config(const uint8_t *d, uint16_t len, cfg_t *c) {
    memset(c, 0, sizeof *c);
    if (len < 9 || d[1] != DT_CONFIG) return false;
    c->config_value = d[5];
    bool first = true, in_input = false, in_hub = false;
    uint8_t itf = 0;
    hh_kind_t itf_kind = HH_KIND_NONE;
    for (uint16_t off = 0; off < len;) {
        const uint8_t dl = d[off], *p = d + off;
        if (dl < 2 || (uint32_t)off + dl > len) return false;
        if (p[1] == DT_INTERFACE && dl >= 9) {
            in_input = in_hub = false;
            if (p[3] == 0) {                                    // alternate setting 0 only
                itf = p[2];
                if (first) { c->first_class = p[5]; c->first_subclass = p[6]; c->first_protocol = p[7]; first = false; }
                in_hub = p[5] == 0x09;
                if (in_hub) c->is_hub = true;
                itf_kind = p[5] == 0x03 ? HH_KIND_HID
                         : (p[5] == 0xFF && p[6] == 0x5D && p[7] == 0x01) ? HH_KIND_XINPUT : HH_KIND_NONE;
                in_input = itf_kind != HH_KIND_NONE && c->kind == HH_KIND_NONE;
            }
        } else if (p[1] == DT_ENDPOINT && dl >= 7) {
            const uint8_t addr = p[2], attr = p[3] & 3;
            uint16_t mps = (uint16_t)(p[4] | (p[5] << 8)) & 0x07FF;
            if (mps > 64) mps = 64;
            if (attr == 3 && (addr & 0x80) && mps && in_input) {   // interrupt IN of the input interface
                c->kind = itf_kind;
                c->itf = itf;
                c->ep = addr;
                c->mps = (uint8_t)mps;
                c->interval = p[6] ? p[6] : 1;
                in_input = false;
            }
        }
        off += dl;
    }
    return true;
}

static int enum_step;                // last step enumerate() reached, for diagnostics

// Device in default state at address 0 -> address `addr`, configured.
static hh_result_t enumerate(uint8_t addr, cfg_t *c, uint16_t *vid, uint16_t *pid) {
    uint8_t buf[256];
    uint16_t got;
    hh_hcd_set_ep0_mps(8);
    hh_result_t r = HH_ERR_TIMEOUT;
    enum_step = 1;
    for (int tries = 0; tries < 3 && r != HH_OK; tries++) {   /* first request: some devices need a retry */
        if (tries) sleep_ms(20);
        r = get_descriptor(0, DT_DEVICE, buf, 8, &got);
    }
    if (r != HH_OK) return r;
    enum_step = 2;
    if (got < 8 || buf[1] != DT_DEVICE) return HH_ERR_DATA;
    const uint8_t mps0 = buf[7];
    if (mps0 != 8 && mps0 != 16 && mps0 != 32 && mps0 != 64) return HH_ERR_DATA;
    hh_hcd_set_ep0_mps(mps0);
    const uint8_t set_addr[8] = {0x00, 5, addr, 0, 0, 0, 0, 0};
    enum_step = 3;
    if ((r = hh_hcd_control_xfer(0, set_addr, NULL, NULL)) != HH_OK) return r;
    sleep_ms(10);
    enum_step = 4;
    if ((r = get_descriptor(addr, DT_DEVICE, buf, 18, &got)) != HH_OK) return r;
    enum_step = 5;
    if (got < 18) return HH_ERR_DATA;
    *vid = (uint16_t)(buf[8] | (buf[9] << 8));
    *pid = (uint16_t)(buf[10] | (buf[11] << 8));
    if ((r = get_descriptor(addr, DT_CONFIG, buf, 9, &got)) != HH_OK) return r;
    if (got < 9 || buf[1] != DT_CONFIG) return HH_ERR_DATA;
    uint16_t total = (uint16_t)(buf[2] | (buf[3] << 8));
    if (total > sizeof buf) total = sizeof buf;
    if ((r = get_descriptor(addr, DT_CONFIG, buf, total, &got)) != HH_OK) return r;
    if (!parse_config(buf, got, c)) return HH_ERR_DATA;
    const uint8_t set_cfg[8] = {0x00, 9, c->config_value, 0, 0, 0, 0, 0};
    return hh_hcd_control_xfer(addr, set_cfg, NULL, NULL);
}

// ---------------------------------------------------------------- hub requests
static hh_result_t port_feature(uint8_t req, uint8_t port, uint8_t feature) {
    const uint8_t setup[8] = {0x23, req, feature, 0, port, 0, 0, 0};
    return hh_hcd_control_xfer(HUB_ADDR, setup, NULL, NULL);
}
static hh_result_t port_status(uint8_t port, uint16_t *status, uint16_t *change) {
    const uint8_t setup[8] = {0xA3, 0, 0, 0, port, 0, 4, 0};
    uint8_t b[4];
    uint16_t n = 4;
    hh_result_t r = hh_hcd_control_xfer(HUB_ADDR, setup, b, &n);
    if (r != HH_OK) return r;
    if (n != 4) return HH_ERR_DATA;
    *status = (uint16_t)(b[0] | (b[1] << 8));
    *change = (uint16_t)(b[2] | (b[3] << 8));
    return HH_OK;
}

static hh_result_t hub_attach(void) {
    const uint8_t setup[8] = {0xA0, 6, 0, 0x29, 0, 0, 9, 0};
    uint8_t hd[9];
    uint16_t n = 9;
    hh_result_t r = hh_hcd_control_xfer(HUB_ADDR, setup, hd, &n);
    if (r != HH_OK) return r;
    if (n < 7) return HH_ERR_DATA;
    st.n_ports = hd[2] > 7 ? 7 : hd[2];
    for (uint8_t p = 1; p <= st.n_ports; p++)
        if ((r = port_feature(3, p, HUB_FEAT_PORT_POWER)) != HH_OK) return r;
    sleep_ms(2 * hd[5] + 20);
    memset(streak, 0, sizeof streak);
    memset(attempts, 0, sizeof attempts);
    memset(blocked, 0, sizeof blocked);
    return HH_OK;
}

static void fail_port(uint8_t p, hh_result_t r) {
    st.last_error = (int)r;
    if (++attempts[p] >= MAX_ATTEMPTS) blocked[p] = true;
    else retry_at[p] = make_timeout_time_ms(500u << (attempts[p] - 1));
}

static void drop_device(void) {
    hh_hcd_int_ep_remove();
    hh_hcd_set_ls(0, false);
    hh_hcd_set_ls(DEV_ADDR, false);
    st.kind = HH_KIND_NONE;
    st.port = 0;
    st.state = HH_ST_HUB;
}

// Reset port p and set up the device on it. True when it is a usable controller.
static bool try_port(uint8_t p, bool low_speed) {
    sleep_ms(100);                                         // connect debounce
    hh_result_t r = port_feature(3, p, HUB_FEAT_PORT_RESET);
    if (r != HH_OK) { fail_port(p, r); return false; }
    uint16_t s = 0, c = 0;
    bool reset_done = false;
    for (absolute_time_t end = make_timeout_time_ms(500); !time_reached(end);) {
        if (port_status(p, &s, &c) != HH_OK) break;
        if (c & PORT_CHG_RESET) { port_feature(1, p, HUB_FEAT_C_PORT_RESET); reset_done = true; break; }
        sleep_ms(10);
    }
    if (!reset_done) { fail_port(p, HH_ERR_TIMEOUT); return false; }
    sleep_ms(50);                                          // reset recovery (spec 10 ms; LS devices like more)
    low_speed = (s & PORT_STAT_LOW_SPEED) != 0;            // speed as seen after the reset
    hh_hcd_set_ls(0, low_speed);
    hh_hcd_set_ls(DEV_ADDR, low_speed);
    cfg_t cfg;
    uint16_t vid = 0, pid = 0;
    r = enumerate(DEV_ADDR, &cfg, &vid, &pid);
    hh_hcd_set_ls(0, false);
    st.port = p;
    st.low_speed = low_speed;
    st.vid = vid;
    st.pid = pid;
    st.itf_class = cfg.first_class;
    st.itf_subclass = cfg.first_subclass;
    st.itf_protocol = cfg.first_protocol;
    if (r != HH_OK) {
        DIAG("hh_usb: port %u enumeration failed (%d) at step %d, %s speed, port status %04x, SIE %08x\n", p, (int)r,
             enum_step, low_speed ? "low" : "full", s, (unsigned)hh_hcd_last_error_status());
        hh_hcd_set_ls(DEV_ADDR, false);
        st.kind = HH_KIND_NONE;
        fail_port(p, r);
        return false;
    }
    DIAG("hh_usb: port %u %04x:%04x %s speed, itf %02x/%02x/%02x, input %d ep %02x mps %u int %u, rx timeouts %u\n", p,
         vid, pid, low_speed ? "low" : "full", cfg.first_class, cfg.first_subclass, cfg.first_protocol, (int)cfg.kind,
         cfg.ep, cfg.mps, cfg.interval, (unsigned)hh_hcd_rx_timeouts());
    if (cfg.kind == HH_KIND_NONE) {                        // keyboard? mass storage? not ours
        st.kind = HH_KIND_OTHER;
        hh_hcd_set_ls(DEV_ADDR, false);
        blocked[p] = true;
        return false;
    }
    if (cfg.kind == HH_KIND_HID) {                         // SET_IDLE 0: report only on change (may STALL)
        const uint8_t idle[8] = {0x21, 0x0A, 0, 0, cfg.itf, 0, 0, 0};
        (void)hh_hcd_control_xfer(DEV_ADDR, idle, NULL, NULL);
    }
    hid_itf = cfg.itf;
    st.kind = cfg.kind;
    st.ep = cfg.ep;
    st.mps = cfg.mps;
    st.interval = cfg.interval;
    st.reports = 0;
    hh_hcd_int_ep_install(DEV_ADDR, cfg.ep, cfg.mps, cfg.interval);
    st.state = HH_ST_DEVICE;
    return true;
}

// ---------------------------------------------------------------- public
void hh_usb_init(void) {
    hh_hcd_init();
    memset(&st, 0, sizeof st);
    st.state = HH_ST_ROOT_WAIT;
    next_poll = get_absolute_time();
}

const hh_status_t *hh_usb_status(void) { return &st; }

void hh_usb_task(void) {
    if (!time_reached(next_poll)) return;
    next_poll = make_timeout_time_ms(POLL_MS);

    if (st.state != HH_ST_ROOT_WAIT && hh_hcd_port_speed() == HH_SPEED_NONE) {   // hub gone
        if (st.state == HH_ST_DEVICE) drop_device();
        st.state = HH_ST_ROOT_WAIT;
        st.n_ports = 0;
        return;
    }
    switch (st.state) {
    case HH_ST_ROOT_WAIT: {
        if (hh_hcd_port_speed() != HH_SPEED_FULL) return;
        sleep_ms(100);
        hh_hcd_bus_reset();
        cfg_t cfg;
        uint16_t vid, pid;
        hh_result_t r = enumerate(HUB_ADDR, &cfg, &vid, &pid);
        if (r == HH_OK && cfg.is_hub) r = hub_attach();
        else if (r == HH_OK) r = HH_ERR_DATA;              // the root port should be the on-board hub
        DIAG("hh_usb: root %04x:%04x hub %d ports %u result %d\n", vid, pid, (int)cfg.is_hub, st.n_ports, (int)r);
        st.last_error = (int)r;
        st.state = r == HH_OK ? HH_ST_HUB : HH_ST_FAILED;
        if (r != HH_OK) next_poll = make_timeout_time_ms(2000);   // then try the hub again
        return;
    }
    case HH_ST_HUB:
        for (uint8_t p = 1; p <= st.n_ports; p++) {
            uint16_t s, c;
            if (port_status(p, &s, &c) != HH_OK) continue;
            st.port_status[p] = s;
            if (c & PORT_CHG_CONNECTION) {
                port_feature(1, p, HUB_FEAT_C_PORT_CONNECTION);
                streak[p] = 0; attempts[p] = 0; blocked[p] = false;
            }
            if (!(s & PORT_STAT_CONNECTION)) { streak[p] = 0; attempts[p] = 0; blocked[p] = false; continue; }
            if (blocked[p]) continue;
            if (attempts[p] && !time_reached(retry_at[p])) continue;
            if (++streak[p] < CONFIRM_POLLS) continue;
            streak[p] = 0;
            if (try_port(p, (s & PORT_STAT_LOW_SPEED) != 0)) return;
        }
        return;
    case HH_ST_DEVICE: {
        uint16_t s, c;
        if (port_status(st.port, &s, &c) != HH_OK) return;
        st.port_status[st.port] = s;
        if ((c & PORT_CHG_CONNECTION) || !(s & PORT_STAT_CONNECTION)) {
            if (c & PORT_CHG_CONNECTION) port_feature(1, st.port, HUB_FEAT_C_PORT_CONNECTION);
            DIAG("hh_usb: port %u unplugged\n", st.port);
            drop_device();
        }
        return;
    }
    case HH_ST_FAILED:
        hh_hcd_init();                                     // start over: reset the controller
        st.state = HH_ST_ROOT_WAIT;
        return;
    }
}

int hh_usb_get_report(uint8_t *buf, int max) {
    if (st.state != HH_ST_DEVICE || st.kind != HH_KIND_HID) return 0;
    if (max > 64) max = 64;
    const uint8_t setup[8] = {0xA1, 0x01, 0, 0x01, hid_itf, 0, (uint8_t)max, 0};   // GET_REPORT input, id 0
    uint16_t n = (uint16_t)max;
    return hh_hcd_control_xfer(DEV_ADDR, setup, buf, &n) == HH_OK ? (int)n : 0;
}

int hh_usb_report(uint8_t *buf, int max) {
    if (st.state != HH_ST_DEVICE) return 0;
    const int n = hh_hcd_int_ep_poll(buf, (uint8_t)(max > 64 ? 64 : max));
    if (n > 0) st.reports++;
    return n;
}
