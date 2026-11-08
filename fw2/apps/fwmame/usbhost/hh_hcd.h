#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    HH_OK = 0,
    HH_ERR_STALL,        // endpoint returned STALL
    HH_ERR_TIMEOUT,      // device not responding / software timeout
    HH_ERR_DATA,         // CRC, bit-stuff, DATA-seq, overflow
    HH_ERR_DISCONNECT,   // device went away mid-transfer
} hh_result_t;

typedef enum {
    HH_SPEED_NONE = 0,
    HH_SPEED_LOW  = 1,
    HH_SPEED_FULL = 2,
} hh_speed_t;

void        hh_hcd_init(void);
hh_speed_t hh_hcd_port_speed(void);   // current SIE_STATUS.SPEED
void        hh_hcd_bus_reset(void);    // drive USB reset + recovery delay

// EP0 max packet size used by control transfers (8 until first descriptor read).
void hh_hcd_set_ep0_mps(uint8_t mps);
// Mark device address `addr` (0-7) as a low-speed device behind the
// full-speed hub: its transactions get a PRE packet. Mark address 0 too
// while such a device is being enumerated.
void hh_hcd_set_ls(uint8_t addr, bool low_speed);
// SIE_STATUS captured at the last transfer error (bit 31 set = software timeout).
uint32_t hh_hcd_last_error_status(void);
// RX timeouts seen and retried by the SIE (not errors; for diagnostics).
uint32_t hh_hcd_rx_timeouts(void);

// Blocking control transfer. setup = 8-byte setup packet. data/inout_len for
// the data stage (NULL/0 for none); *inout_len in = buffer size, out = actual.
// For OUT data stages the caller must supply exactly wLength bytes.
hh_result_t hh_hcd_control_xfer(uint8_t dev_addr, const uint8_t setup[8],
                              void *data, uint16_t *inout_len);

// Blocking bulk transfer on EPX, double-buffered. ep_addr bit 7 = IN.
// *toggle = DATA toggle for this pipe (0/1), updated on return.
hh_result_t hh_hcd_bulk_xfer(uint8_t dev_addr, uint8_t ep_addr, uint8_t *toggle,
                           void *buf, uint32_t len, uint32_t *actual,
                           uint32_t timeout_ms);

// Hardware-polled interrupt IN endpoint (hub status pipe). One supported.
void hh_hcd_int_ep_install(uint8_t dev_addr, uint8_t ep_addr, uint16_t mps,
                        uint8_t interval_ms);
void hh_hcd_int_ep_remove(void);
// >0: bytes copied to buf; 0: no new data (a zero-length report is indistinguishable)
int  hh_hcd_int_ep_poll(uint8_t *buf, uint8_t maxlen);
