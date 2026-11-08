// hh_usb.h - one game controller on the FW2's USB host ports, through the
// display CPU's native USB controller and the on-board CH334F hub.
//
// wilibsp's native host stack (bsp/usbhost) only takes mass-storage devices,
// full speed; this is a small sibling for input devices, built on a copy of
// its controller driver (hh_hcd.c) with low-speed support added. Polled from
// the main loop: hh_usb_task() runs the root / hub / device state machine
// (each step a few control transfers; enumeration blocks up to ~0.6 s once
// per plug-in), hh_usb_report() hands over the newest input report.
//
// The caller powers the ports first: hold power zone 8 (USB_HUB) and call
// ioexp_usb_pwr(true).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { HH_KIND_NONE, HH_KIND_HID, HH_KIND_XINPUT, HH_KIND_OTHER } hh_kind_t;
typedef enum { HH_ST_ROOT_WAIT, HH_ST_HUB, HH_ST_DEVICE, HH_ST_FAILED } hh_state_t;

typedef struct {
    hh_state_t state;
    uint8_t    n_ports;
    uint16_t   port_status[8];  // wPortStatus per hub port (index 1..n_ports)
    // the device being used, or the last one seen (kind OTHER = not usable)
    hh_kind_t  kind;
    uint8_t    port;            // hub port, 0 = none
    bool       low_speed;
    uint16_t   vid, pid;
    uint8_t    itf_class, itf_subclass, itf_protocol;   // first interface
    uint8_t    ep, mps, interval;
    int        last_error;      // hh_result_t of the last failed step, 0 = none
    uint32_t   reports;
} hh_status_t;

void hh_usb_init(void);
void hh_usb_task(void);
// >0: bytes of a new report copied to buf (at most max); 0: none since the last call.
int hh_usb_report(uint8_t *buf, int max);
// HID only: ask the device for its current input report (GET_REPORT on EP0).
// >0: bytes copied; 0: not supported or failed.
int hh_usb_get_report(uint8_t *buf, int max);
const hh_status_t *hh_usb_status(void);
