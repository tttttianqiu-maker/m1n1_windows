/* SPDX-License-Identifier: ISC */
#ifndef BRCM_MSGBUF_H
#define BRCM_MSGBUF_H

#ifdef BRCM_HOST_TEST
#include "brcm_host_test.h" /* tests/c: host build of the protocol code */
#else
#include "types.h"
#endif

/* firmware event codes (brcmfmac fweh.h) */
#define BRCM_E_SET_SSID     0
#define BRCM_E_JOIN         1
#define BRCM_E_AUTH         3
#define BRCM_E_DEAUTH       5
#define BRCM_E_DEAUTH_IND   6
#define BRCM_E_ASSOC        7
#define BRCM_E_REASSOC      9
#define BRCM_E_DISASSOC     11
#define BRCM_E_DISASSOC_IND 12
#define BRCM_E_LINK         16
#define BRCM_E_PSK_SUP      46
#define BRCM_E_IF           54
#define BRCM_E_ESCAN_RESULT 69

struct brcm_event {
    u32 type, status, reason, datalen;
    u16 flags;
    u8 ifidx;
    u8 addr[6];
    const u8 *data;
};

/* what the polling loop learned; read by wlan.c and the status log */
struct brcm_link {
    bool link, keyed, assoc_done, fw_halted;
    u32 assoc_status, sup_status, last_reason;
    u32 events;
    u64 rx_frames, rx_bytes, rx_dropped;
    u64 tx_frames, tx_errors, tx_dropped;
};

typedef void (*brcm_rx_cb)(const u8 *frame, u32 len);
typedef void (*brcm_event_cb)(const struct brcm_event *ev);

/* rings, buffers, hostready (after brcm_boot) */
int brcm_msgbuf_init(void);
/* service the rings: completions, events, rx, buffer reposts, console */
void brcm_poll(void);
/* synchronous dongle ioctl through the control ring; 0 or the firmware's (negative) error */
int brcm_ioctl(u32 cmd, void *buf, u32 len, u32 *resp_len);
/* one 802.3 frame out through the STA flowring (created on first use) */
int brcm_tx(const u8 *frame, u32 len);
const struct brcm_link *brcm_link_state(void);
/* forget association state before a new join */
void brcm_link_clear(void);
void brcm_set_callbacks(brcm_rx_cb rx, brcm_event_cb ev);

#endif
