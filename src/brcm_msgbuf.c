/* SPDX-License-Identifier: ISC */
/*
 * brcmfmac "msgbuf" protocol for m1n1 (J613 built-in Wi-Fi, BCM4388), polled, no IRQs.
 *
 * A reduced port of Linux brcmfmac pcie.c (ring setup, scratch buffers, doorbells,
 * mailbox data), commonring.c and msgbuf.c (ioctls, buffer posting, events, rx, tx
 * flowring): host memory comes from one DMA pool mapped through the pcie0 DART (stream 1,
 * the stream wlan.c assigns to 01:00.0); ring indices live in the chip's TCM (the
 * fallback brcmfmac uses when it has no index buffer), so host and chip never share a
 * cache line. PCIe on Apple Silicon is not declared coherent (no dma-coherent in the
 * Linux device trees), so every buffer is cleaned before the chip reads it and
 * invalidated before the CPU reads what the chip wrote.
 *
 * Copyright (c) 2014 Broadcom Corporation (brcmfmac)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "brcm.h"
#include "brcm_msgbuf.h"
#include "brcm_priv.h"
#ifndef BRCM_HOST_TEST
#include "adt.h"
#include "dart.h"
#include "malloc.h"
#include "memory.h"
#include "string.h"
#include "utils.h"
#endif

/* ---------- DMA pool behind the pcie0 DART ---------- */

#define PCIE0_DART_BASE          0x594000000UL /* t8122.dtsi pcie0_dart */
#define WLAN_DART_SID            1
#define DART_T8110_ERROR         0x100
#define DART_T8110_ERROR_FLAG    BIT(31)
#define DART_T8110_ERROR_ADDR_LO 0x170
#define DART_T8110_ERROR_ADDR_HI 0x174

#define DMA_POOL_SIZE (2u << 20)
#define DMA_IOVA_BASE 0x80000000UL

static u8 *pool;
static u32 pool_off;

/*
 * Clean+invalidate every cache line touched by [p, p+len), then DSB. m1n1's
 * dc_civac_range() starts at p itself and stops once p passes the end, so for a range that
 * does not start on a line (40/24-byte ring slots, frame payloads) it skips the last line;
 * it also leaves the completion barrier to the caller.
 */
static void dma_sync(const void *p, u32 len)
{
    u64 start = (u64)p & ~63UL;
    u64 end = ALIGN_UP((u64)p + (len ? len : 1), 64);
    dc_civac_range((void *)start, end - start);
    sysop("dsb sy");
}
static dart_dev_t *dart;
static u64 dart_base;

static void *dma_alloc(u32 size, u64 *iova)
{
    size = ALIGN_UP(size, 128);
    if (pool_off + size > DMA_POOL_SIZE) {
        printf("wlan: DMA pool exhausted (%u + %u)\n", pool_off, size);
        return NULL;
    }
    u8 *p = pool + pool_off;
    *iova = DMA_IOVA_BASE + pool_off;
    pool_off += size;
    memset(p, 0, size);
    dma_sync(p, size);
    return p;
}

static int dma_init(void)
{
    if (!pool) {
        pool = memalign(SZ_16K, DMA_POOL_SIZE);
        if (!pool) {
            printf("wlan: no memory for the DMA pool\n");
            return -1;
        }
        dart_base = PCIE0_DART_BASE;
        if (adt_path_offset(adt, "/arm-io/dart-apcie0") >= 0) {
            int path[8];
            u64 base = 0;
            if (adt_path_offset_trace(adt, "/arm-io/dart-apcie0", path) >= 0 &&
                adt_get_reg(adt, path, "reg", 0, &base, NULL) >= 0 && base != PCIE0_DART_BASE)
                printf("wlan: ADT dart-apcie0 at 0x%lx, expected 0x%lx\n", base, PCIE0_DART_BASE);
            dart = dart_init_adt("/arm-io/dart-apcie0", 0, WLAN_DART_SID, false);
            if (dart && base)
                dart_base = base;
        }
        if (!dart)
            dart = dart_init(PCIE0_DART_BASE, WLAN_DART_SID, false, DART_T8110);
        if (!dart) {
            printf("wlan: pcie0 DART init failed\n");
            free(pool);
            pool = NULL;
            return -1;
        }
        if (dart_map(dart, DMA_IOVA_BASE, pool, DMA_POOL_SIZE)) {
            printf("wlan: DART map failed\n");
            return -1;
        }
        printf("wlan: DMA pool %p = IOVA 0x%lx (%u KiB), DART 0x%lx stream %d\n", pool,
               DMA_IOVA_BASE, DMA_POOL_SIZE >> 10, dart_base, WLAN_DART_SID);
    }
    pool_off = 0;
    write32(dart_base + DART_T8110_ERROR, read32(dart_base + DART_T8110_ERROR));
    return 0;
}

static u32 dart_faults;

static void dart_check(const char *when)
{
    if (!dart_base)
        return;
    u32 err = read32(dart_base + DART_T8110_ERROR);
    if (!(err & DART_T8110_ERROR_FLAG))
        return;
    if (dart_faults++ < 8)
        printf("wlan: DART fault %s: error 0x%08x at 0x%x%08x\n", when, err,
               read32(dart_base + DART_T8110_ERROR_ADDR_HI),
               read32(dart_base + DART_T8110_ERROR_ADDR_LO));
    write32(dart_base + DART_T8110_ERROR, err);
}

/* ---------- rings ---------- */

#define MSGBUF_TYPE_GEN_STATUS             0x1
#define MSGBUF_TYPE_RING_STATUS            0x2
#define MSGBUF_TYPE_FLOW_RING_CREATE       0x3
#define MSGBUF_TYPE_FLOW_RING_CREATE_CMPLT 0x4
#define MSGBUF_TYPE_FLOW_RING_DELETE_CMPLT 0x6
#define MSGBUF_TYPE_IOCTLPTR_REQ           0x9
#define MSGBUF_TYPE_IOCTLPTR_REQ_ACK       0xA
#define MSGBUF_TYPE_IOCTLRESP_BUF_POST     0xB
#define MSGBUF_TYPE_IOCTL_CMPLT            0xC
#define MSGBUF_TYPE_EVENT_BUF_POST         0xD
#define MSGBUF_TYPE_WL_EVENT               0xE
#define MSGBUF_TYPE_TX_POST                0xF
#define MSGBUF_TYPE_TX_STATUS              0x10
#define MSGBUF_TYPE_RXBUF_POST             0x11
#define MSGBUF_TYPE_RX_CMPLT               0x12
#define MSGBUF_TYPE_H2D_MAILBOX_DATA       0x23
#define MSGBUF_TYPE_D2H_MAILBOX_DATA       0x24

#define IOCTL_REQ_PKTID     0xFFFE
#define IOCTL_MAX_IN        1518 /* BRCMF_TX_IOCTL_MAX_MSG_SIZE */
#define CTL_BUF_SIZE        8192 /* BRCMF_MSGBUF_MAX_CTL_PKT_SIZE */
#define DATA_BUF_SIZE       2048 /* BRCMF_MSGBUF_MAX_PKT_SIZE */
#define PKT_FLAGS_FRAME_8023  0x01
#define PKT_FLAGS_FRAME_80211 0x02

#define D2H_DEV_D3_ACK        0x00000001
#define D2H_DEV_DS_ENTER_REQ  0x00000002
#define D2H_DEV_FWHALT        0x10000000
#define H2D_HOST_DS_ACK       0x00000002

/* ring info in TCM (struct brcmf_pcie_dhi_ringinfo) */
#define RI_RINGMEM          0
#define RI_H2D_W_IDX        4
#define RI_H2D_R_IDX        8
#define RI_D2H_W_IDX        12
#define RI_D2H_R_IDX        16
#define RI_MAX_FLOWRINGS    52
#define RI_MAX_SUBMISSION   54
#define RI_MAX_COMPLETION   56
#define RING_MEM_SZ         16
#define RING_MAX_ITEM_OFF   4
#define RING_LEN_ITEMS_OFF  6
#define RING_BASE_OFF       8
#define DMA_D2H_SCRATCH_LEN 8
#define DMA_D2H_RINGUPD_LEN 1024

struct ring {
    u8 *buf;
    u64 iova;
    u16 depth, item, r, w;
    u32 w_idx_addr, r_idx_addr;
    bool dirty; /* written items not announced to the chip yet */
};

enum { R_CTRL_SUBMIT, R_RXPOST, R_CTRL_CMPLT, R_TX_CMPLT, R_RX_CMPLT, NR_COMMON };
static const u16 ring_depth[NR_COMMON] = {64, 256, 64, 256, 256};
static const u16 ring_item_v7[NR_COMMON] = {40, 32, 24, 24, 40};
static const u16 ring_item_pre_v7[NR_COMMON] = {40, 32, 24, 16, 32};
#define FLOWRING_DEPTH 256
#define FLOWRING_ITEM  48
#define FLOWRING_IDSTART 2

static struct ring rings[NR_COMMON];
static struct ring flowring;
static u16 max_flowrings, max_submission, max_completion;
static u32 flow_h2d_w_idx, flow_h2d_r_idx;
static enum { FLOW_NONE, FLOW_CREATING, FLOW_OPEN, FLOW_FAILED } flow_state;

static void ring_bell(void)
{
    pcie_write32((brcm.flags & SHARED_DAR) ? BRCM_PCIE64_H2D_MAILBOX_0 : BRCM_PCIE_H2D_MAILBOX_0, 1);
}

/* copy one message into the next free slot of an H2D ring (not announced yet) */
static bool h2d_put(struct ring *r, const void *msg, u32 len)
{
    u16 avail = r->r <= r->w ? r->depth - r->w + r->r : r->r - r->w;
    if (avail <= 1) {
        r->r = tcm_read16(r->r_idx_addr);
        avail = r->r <= r->w ? r->depth - r->w + r->r : r->r - r->w;
        if (avail <= 1)
            return false;
    }
    u8 *slot = r->buf + (u32)r->w * r->item;
    memset(slot, 0, r->item);
    memcpy(slot, msg, min(len, (u32)r->item));
    dma_sync(slot, r->item);
    if (++r->w == r->depth)
        r->w = 0;
    r->dirty = true;
    return true;
}

/* announce written items: index to TCM, then the doorbell */
static void h2d_kick(struct ring *r)
{
    if (!r->dirty)
        return;
    r->dirty = false;
    dma_wmb();
    tcm_write16(r->w_idx_addr, r->w);
    ring_bell();
}

/* ---------- message layouts (msgbuf.c) ---------- */

struct msg_hdr {
    u8 msgtype, ifidx, flags, rsvd0;
    u32 request_id;
} PACKED;

struct msg_compl {
    u16 status, flow_ring_id;
} PACKED;

struct msg_ioctl_req {
    struct msg_hdr msg;
    u32 cmd;
    u16 trans_id, input_buf_len, output_buf_len, rsvd0[3];
    u64 req_buf_addr;
    u32 rsvd1[2];
} PACKED;

struct msg_rx_bufpost {
    struct msg_hdr msg;
    u16 metadata_buf_len, data_buf_len;
    u32 rsvd0;
    u64 metadata_buf_addr, data_buf_addr;
} PACKED;

struct msg_ctl_bufpost {
    struct msg_hdr msg;
    u16 host_buf_len, rsvd0[3];
    u64 host_buf_addr;
    u32 rsvd1[4];
} PACKED;

struct msg_ioctl_resp {
    struct msg_hdr msg;
    struct msg_compl compl;
    u16 resp_len, trans_id;
    u32 cmd, rsvd0;
} PACKED;

struct msg_rx_event {
    struct msg_hdr msg;
    struct msg_compl compl;
    u16 event_data_len, seqnum, rsvd0[4];
} PACKED;

struct msg_rx_complete {
    struct msg_hdr msg;
    struct msg_compl compl;
    u16 metadata_len, data_len, data_offset, flags;
    u32 rx_status_0, rx_status_1, rsvd0;
} PACKED;

struct msg_tx_status {
    struct msg_hdr msg;
    struct msg_compl compl;
    u16 metadata_len, tx_status;
} PACKED;

struct msg_tx_post {
    struct msg_hdr msg;
    u8 txhdr[14];
    u8 flags, seg_cnt;
    u64 metadata_buf_addr, data_buf_addr;
    u16 metadata_buf_len, data_len;
    u32 rsvd0;
} PACKED;

struct msg_flowring_create {
    struct msg_hdr msg;
    u8 da[6], sa[6];
    u8 tid, if_flags;
    u16 flow_ring_id;
    u8 tc, priority;
    u16 int_vector, max_items, len_item;
    u64 flow_ring_addr;
} PACKED;

struct msg_h2d_mb {
    struct msg_hdr msg;
    u32 data, rsvd0[7];
} PACKED;

struct msg_d2h_mb {
    struct msg_hdr msg;
    struct msg_compl compl;
    u32 data, rsvd0[2];
} PACKED;

_Static_assert(sizeof(struct msg_ioctl_req) == 40, "ioctl req");
_Static_assert(sizeof(struct msg_rx_bufpost) == 32, "rx bufpost");
_Static_assert(sizeof(struct msg_ctl_bufpost) == 40, "ctl bufpost");
_Static_assert(sizeof(struct msg_rx_complete) == 32, "rx complete");
_Static_assert(sizeof(struct msg_tx_post) == 48, "tx post");
_Static_assert(sizeof(struct msg_flowring_create) == 40, "flowring create");
_Static_assert(sizeof(struct msg_h2d_mb) == 40, "h2d mb");

/* ---------- host buffers ---------- */

enum { PKT_DATA, PKT_EVENT, PKT_IOCTL, PKT_PARKED };
#define NR_RXDATA 128
#define NR_EVENT  8
#define NR_IOCTL  4
#define NR_PKT    (NR_RXDATA + NR_EVENT + NR_IOCTL)
#define NR_TX     128

struct pkt {
    u8 *va;
    u64 iova;
    u16 size;
    u8 kind;
    bool posted;
};

static struct pkt pkts[NR_PKT];
static struct pkt txbufs[NR_TX];
static u32 tx_next, tx_inflight;

static u8 *ioctbuf;
static u64 ioctbuf_iova;
static u8 ioctl_resp[CTL_BUF_SIZE];
static u32 ioctl_resp_len;
static s16 ioctl_status;
static u16 ioctl_trans, ioctl_done_trans;
static bool ioctl_done;

static bool msgbuf_up;
static struct brcm_link link;
static brcm_rx_cb rx_cb;
static brcm_event_cb event_cb;

static void post_pkt(u32 id)
{
    struct pkt *p = &pkts[id];
    if (p->posted)
        return;
    dma_sync(p->va, p->size);
    if (p->kind == PKT_DATA) {
        struct msg_rx_bufpost m = {0};
        m.msg.msgtype = MSGBUF_TYPE_RXBUF_POST;
        m.msg.request_id = id;
        m.data_buf_len = p->size;
        m.data_buf_addr = p->iova;
        if (!h2d_put(&rings[R_RXPOST], &m, sizeof(m)))
            return;
    } else {
        struct msg_ctl_bufpost m = {0};
        m.msg.msgtype =
            p->kind == PKT_EVENT ? MSGBUF_TYPE_EVENT_BUF_POST : MSGBUF_TYPE_IOCTLRESP_BUF_POST;
        m.msg.request_id = id;
        m.host_buf_len = p->size;
        m.host_buf_addr = p->iova;
        if (!h2d_put(&rings[R_CTRL_SUBMIT], &m, sizeof(m)))
            return;
    }
    p->posted = true;
}

/* buffers whose post failed (ring full) go out on the next poll */
static void repost_all(void)
{
    for (u32 i = 0; i < NR_PKT; i++)
        if (!pkts[i].posted)
            post_pkt(i);
    h2d_kick(&rings[R_RXPOST]);
    h2d_kick(&rings[R_CTRL_SUBMIT]);
}

static struct pkt *take_pkt(u32 id, u8 kind)
{
    if (id >= NR_PKT || pkts[id].kind != kind || !pkts[id].posted) {
        static u32 bad;
        if (bad++ < 8)
            printf("wlan: completion for unknown buffer %u (kind %u)\n", id, kind);
        return NULL;
    }
    pkts[id].posted = false;
    return &pkts[id];
}

/* ---------- mailbox data ---------- */

static void send_mb_data(u32 data)
{
    if (brcm.mb_via_ctl) {
        struct msg_h2d_mb m = {0};
        m.msg.msgtype = MSGBUF_TYPE_H2D_MAILBOX_DATA;
        m.msg.ifidx = 0xff;
        m.data = data;
        if (h2d_put(&rings[R_CTRL_SUBMIT], &m, sizeof(m)))
            h2d_kick(&rings[R_CTRL_SUBMIT]);
        return;
    }
    for (int i = 0; i < 100 && tcm_read32(brcm.htod_mb); i++)
        mdelay(1);
    tcm_write32(brcm.htod_mb, data);
    write32(brcm.cfg + BRCM_CFG_SBMBX, 1);
}

static void handle_mb_data(u32 data)
{
    if (data & D2H_DEV_DS_ENTER_REQ)
        send_mb_data(H2D_HOST_DS_ACK);
    if (data & D2H_DEV_FWHALT) {
        printf("wlan: firmware halted (D2H mailbox 0x%x)\n", data);
        link.fw_halted = true;
    }
}

/* ---------- events ---------- */

static u16 be16(const u8 *p)
{
    return ((u16)p[0] << 8) | p[1];
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static const char *event_name(u32 t)
{
    switch (t) {
        case BRCM_E_SET_SSID: return "SET_SSID";
        case BRCM_E_JOIN: return "JOIN";
        case BRCM_E_AUTH: return "AUTH";
        case BRCM_E_DEAUTH: return "DEAUTH";
        case BRCM_E_DEAUTH_IND: return "DEAUTH_IND";
        case BRCM_E_ASSOC: return "ASSOC";
        case BRCM_E_REASSOC: return "REASSOC";
        case BRCM_E_DISASSOC: return "DISASSOC";
        case BRCM_E_DISASSOC_IND: return "DISASSOC_IND";
        case BRCM_E_LINK: return "LINK";
        case BRCM_E_PSK_SUP: return "PSK_SUP";
        case BRCM_E_IF: return "IF";
        case BRCM_E_ESCAN_RESULT: return "ESCAN_RESULT";
        default: return "?";
    }
}

/* fweh.h BRCMF_E_STATUS_* (generic) and BRCMF_E_STATUS_FWSUP_* (PSK_SUP) */
static const char *event_status(u32 type, u32 status)
{
    static const char *const generic[] = {"success", "fail",        "timeout", "no network",
                                          "abort",   "no ack",      "unsolicited", "attempt",
                                          "partial", "new scan",    "new assoc", "11h quiet",
                                          "suppress", "no channels"};
    static const char *const fwsup[] = {"", "", "", "", "waiting for M1", "sending M2",
                                        "keyed", "handshake timeout (wrong password?)",
                                        "waiting for M3", "sending M4", "waiting for G1",
                                        "sending G2"};
    if (type == BRCM_E_PSK_SUP)
        return status < ARRAY_SIZE(fwsup) && fwsup[status][0] ? fwsup[status] : "?";
    return status < ARRAY_SIZE(generic) ? generic[status] : "?";
}

static void handle_event(const u8 *p, u32 len)
{
    /* struct brcmf_event: ethhdr (14), brcm_ethhdr (10), brcmf_event_msg_be (48), data */
    if (len < 72 || be16(p + 12) != 0x886c) {
        static u32 odd;
        if (odd++ < 4)
            printf("wlan: event frame of %u bytes, ethertype 0x%04x ignored\n", len,
                   len >= 14 ? be16(p + 12) : 0);
        return;
    }
    const u8 *m = p + 24;
    struct brcm_event ev = {
        .type = be32(m + 4),
        .status = be32(m + 8),
        .reason = be32(m + 12),
        .flags = be16(m + 2),
        .datalen = be32(m + 20),
        .ifidx = m[46],
    };
    memcpy(ev.addr, m + 24, 6);
    ev.data = p + 72;
    if (ev.datalen > len - 72)
        ev.datalen = len - 72;
    link.events++;
    /* the connection events are rare and always logged; anything else only at first */
    bool known = event_name(ev.type)[0] != '?' && ev.type != BRCM_E_ESCAN_RESULT;
    static u32 other;
    if (known || other++ < 16)
        printf("wlan: event %s (%u) status %u (%s) reason %u flags 0x%x from "
               "%02x:%02x:%02x:%02x:%02x:%02x, %u data bytes\n",
               event_name(ev.type), ev.type, ev.status, event_status(ev.type, ev.status),
               ev.reason, ev.flags, ev.addr[0], ev.addr[1], ev.addr[2], ev.addr[3], ev.addr[4],
               ev.addr[5], ev.datalen);
    switch (ev.type) {
        case BRCM_E_LINK:
            link.link = ev.flags & 1;
            if (!link.link)
                link.keyed = false;
            break;
        case BRCM_E_SET_SSID:
            link.assoc_status = ev.status;
            link.assoc_done = true;
            break;
        case BRCM_E_PSK_SUP:
            link.sup_status = ev.status;
            if (ev.status == 6) /* WLC_SUP_KEYED */
                link.keyed = true;
            break;
        case BRCM_E_DEAUTH:
        case BRCM_E_DEAUTH_IND:
        case BRCM_E_DISASSOC_IND:
            link.link = link.keyed = false;
            link.last_reason = ev.reason;
            break;
    }
    if (event_cb)
        event_cb(&ev);
}

/* ---------- completions ---------- */

static void handle_msg(const u8 *item)
{
    const struct msg_hdr *h = (const void *)item;
    switch (h->msgtype) {
        case MSGBUF_TYPE_IOCTLPTR_REQ_ACK: {
            const struct msg_ioctl_resp *r = (const void *)item;
            if (r->compl.status)
                printf("wlan: ioctl request ack status %d\n", (s16)r->compl.status);
            break;
        }
        case MSGBUF_TYPE_IOCTL_CMPLT: {
            const struct msg_ioctl_resp *r = (const void *)item;
            struct pkt *p = take_pkt(r->msg.request_id, PKT_IOCTL);
            ioctl_status = (s16)r->compl.status;
            ioctl_resp_len = 0;
            if (p) {
                u32 n = min((u32)r->resp_len, (u32)sizeof(ioctl_resp));
                dma_sync(p->va, n);
                memcpy(ioctl_resp, p->va, n);
                ioctl_resp_len = n;
                post_pkt(p - pkts);
            }
            ioctl_done_trans = r->trans_id;
            ioctl_done = true;
            break;
        }
        case MSGBUF_TYPE_WL_EVENT: {
            const struct msg_rx_event *e = (const void *)item;
            struct pkt *p = take_pkt(e->msg.request_id, PKT_EVENT);
            if (!p)
                break;
            u32 len = e->event_data_len;
            u32 off = brcm.rx_dataoffset;
            if (off + len <= p->size) {
                dma_sync(p->va, off + len);
                handle_event(p->va + off, len);
            }
            post_pkt(p - pkts);
            break;
        }
        case MSGBUF_TYPE_RX_CMPLT: {
            const struct msg_rx_complete *c = (const void *)item;
            struct pkt *p = take_pkt(c->msg.request_id, PKT_DATA);
            if (!p)
                break;
            u32 off = c->data_offset ? c->data_offset : brcm.rx_dataoffset;
            u32 len = c->data_len;
            /* like msgbuf.c: everything that is not an 802.11 (monitor) frame is 802.3 */
            if (off + len <= p->size && len >= 14 && (c->flags & 7) != PKT_FLAGS_FRAME_80211) {
                dma_sync(p->va, off + len);
                link.rx_frames++;
                link.rx_bytes += len;
                if (rx_cb)
                    rx_cb(p->va + off, len);
            } else {
                link.rx_dropped++;
            }
            post_pkt(p - pkts);
            break;
        }
        case MSGBUF_TYPE_TX_STATUS: {
            const struct msg_tx_status *s = (const void *)item;
            u32 id = s->msg.request_id - 1;
            if (id < NR_TX && txbufs[id].posted) {
                txbufs[id].posted = false;
                tx_inflight--;
            }
            if (s->tx_status)
                link.tx_errors++;
            break;
        }
        case MSGBUF_TYPE_FLOW_RING_CREATE_CMPLT: {
            const struct msg_d2h_mb *f = (const void *)item; /* only compl is used */
            flow_state = f->compl.status ? FLOW_FAILED : FLOW_OPEN;
            printf("wlan: TX flowring %u %s (status %u)\n", f->compl.flow_ring_id,
                   f->compl.status ? "refused" : "open", f->compl.status);
            break;
        }
        case MSGBUF_TYPE_D2H_MAILBOX_DATA: {
            const struct msg_d2h_mb *mb = (const void *)item;
            handle_mb_data(mb->data);
            break;
        }
        case MSGBUF_TYPE_GEN_STATUS:
        case MSGBUF_TYPE_RING_STATUS: {
            const struct msg_d2h_mb *s = (const void *)item;
            if (s->compl.status)
                printf("wlan: firmware %s error %d (ring %u)\n",
                       h->msgtype == MSGBUF_TYPE_GEN_STATUS ? "general" : "ring",
                       (s16)s->compl.status, s->compl.flow_ring_id);
            break;
        }
        case MSGBUF_TYPE_FLOW_RING_DELETE_CMPLT:
            flow_state = FLOW_NONE;
            break;
        default: {
            static u32 unknown;
            if (unknown++ < 8)
                printf("wlan: unhandled message type 0x%x\n", h->msgtype);
        }
    }
}

static u32 d2h_drain(struct ring *r)
{
    u16 w = tcm_read16(r->w_idx_addr);
    u32 n = 0;
    if (w >= r->depth) {
        static u32 bad;
        if (bad++ < 4)
            printf("wlan: bad D2H write index %u (depth %u)\n", w, r->depth);
        return 0;
    }
    while (r->r != w && n < r->depth) {
        u8 *item = r->buf + (u32)r->r * r->item;
        dma_sync(item, r->item);
        if (brcm.rx_dataoffset < r->item)
            handle_msg(item + brcm.rx_dataoffset);
        if (++r->r == r->depth)
            r->r = 0;
        n++;
    }
    if (n)
        tcm_write16(r->r_idx_addr, r->r);
    return n;
}

static u64 last_slow;

void brcm_poll(void)
{
    if (!msgbuf_up)
        return;
    d2h_drain(&rings[R_RX_CMPLT]);
    d2h_drain(&rings[R_TX_CMPLT]);
    d2h_drain(&rings[R_CTRL_CMPLT]);
    repost_all();
    if (ticks_to_msecs(get_ticks() - last_slow) >= 100) {
        last_slow = get_ticks();
        if (!brcm.mb_via_ctl && brcm.dtoh_mb) {
            u32 mb = tcm_read32(brcm.dtoh_mb);
            if (mb) {
                tcm_write32(brcm.dtoh_mb, 0);
                handle_mb_data(mb);
            }
        }
        brcm_console_poll();
        dart_check("while running");
    }
}

const struct brcm_link *brcm_link_state(void)
{
    return &link;
}

void brcm_link_clear(void)
{
    link.link = link.keyed = link.assoc_done = false;
    link.assoc_status = link.sup_status = link.last_reason = 0;
}

void brcm_set_callbacks(brcm_rx_cb rx, brcm_event_cb ev)
{
    rx_cb = rx;
    event_cb = ev;
}

/* ---------- ioctls ---------- */

int brcm_ioctl(u32 cmd, void *buf, u32 len, u32 *resp_len)
{
    if (!msgbuf_up)
        return -1;
    u32 in = min(len, (u32)IOCTL_MAX_IN);
    if (buf)
        memcpy(ioctbuf, buf, in);
    else
        memset(ioctbuf, 0, in);
    dma_sync(ioctbuf, IOCTL_MAX_IN);

    struct msg_ioctl_req m = {0};
    m.msg.msgtype = MSGBUF_TYPE_IOCTLPTR_REQ;
    m.msg.request_id = IOCTL_REQ_PKTID;
    m.cmd = cmd;
    m.trans_id = ++ioctl_trans;
    m.input_buf_len = in;
    m.output_buf_len = min(len, (u32)CTL_BUF_SIZE);
    m.req_buf_addr = ioctbuf_iova;
    ioctl_done = false;
    if (!h2d_put(&rings[R_CTRL_SUBMIT], &m, sizeof(m))) {
        printf("wlan: control ring full (ioctl %u)\n", cmd);
        return -1;
    }
    h2d_kick(&rings[R_CTRL_SUBMIT]);

    /* MSGBUF_IOCTL_RESP_TIMEOUT is 2 s; a late answer to an earlier, timed-out request is
       dropped instead of being taken for this one */
    u64 t0 = get_ticks();
    for (;;) {
        brcm_poll();
        if (ioctl_done) {
            if (ioctl_done_trans == m.trans_id)
                break;
            printf("wlan: ioctl %u: late answer for transaction %u dropped\n", cmd,
                   ioctl_done_trans);
            ioctl_done = false;
        }
        if (link.fw_halted || ticks_to_msecs(get_ticks() - t0) > 2000) {
            printf("wlan: ioctl %u %s\n", cmd,
                   link.fw_halted ? "aborted: firmware halted" : "timed out");
            brcm_console_poll();
            dart_check("ioctl timeout");
            return -1;
        }
        udelay(20);
    }
    if (buf && len)
        memcpy(buf, ioctl_resp, min(len, ioctl_resp_len));
    if (resp_len)
        *resp_len = ioctl_resp_len;
    return ioctl_status ? (ioctl_status < 0 ? ioctl_status : -ioctl_status) : 0;
}

/* ---------- TX (one STA flowring, fifo 0) ---------- */

static void flowring_create(const u8 *da, const u8 *sa)
{
    struct msg_flowring_create m = {0};
    m.msg.msgtype = MSGBUF_TYPE_FLOW_RING_CREATE;
    memcpy(m.da, da, 6);
    memcpy(m.sa, sa, 6);
    m.tid = 0;
    m.flow_ring_id = FLOWRING_IDSTART;
    m.max_items = FLOWRING_DEPTH;
    m.len_item = FLOWRING_ITEM;
    m.flow_ring_addr = flowring.iova;
    /* commonring.c brcmf_commonring_config: both indices to 0 before the create request */
    flowring.r = flowring.w = 0;
    flowring.dirty = false;
    tcm_write16(flowring.r_idx_addr, 0);
    tcm_write16(flowring.w_idx_addr, 0);
    if (h2d_put(&rings[R_CTRL_SUBMIT], &m, sizeof(m))) {
        h2d_kick(&rings[R_CTRL_SUBMIT]);
        flow_state = FLOW_CREATING;
    }
}

int brcm_tx(const u8 *frame, u32 len)
{
    if (!msgbuf_up || len < 14 || len > DATA_BUF_SIZE || !link.keyed)
        return -1;
    if (flow_state == FLOW_NONE)
        flowring_create(frame, frame + 6);
    if (flow_state != FLOW_OPEN) {
        link.tx_dropped++;
        return -1;
    }
    struct pkt *t = NULL;
    u32 id = 0;
    for (u32 i = 0; i < NR_TX; i++) {
        id = (tx_next + i) % NR_TX;
        if (!txbufs[id].posted) {
            t = &txbufs[id];
            break;
        }
    }
    if (!t) {
        link.tx_dropped++;
        return -1;
    }
    tx_next = id + 1;
    memcpy(t->va, frame, len);
    dma_sync(t->va, len);

    struct msg_tx_post m = {0};
    m.msg.msgtype = MSGBUF_TYPE_TX_POST;
    m.msg.request_id = id + 1;
    memcpy(m.txhdr, frame, 14);
    m.flags = PKT_FLAGS_FRAME_8023;
    m.seg_cnt = 1;
    m.data_buf_addr = t->iova + 14;
    m.data_len = len - 14;
    if (!h2d_put(&flowring, &m, sizeof(m))) {
        link.tx_dropped++;
        return -1;
    }
    t->posted = true;
    tx_inflight++;
    link.tx_frames++;
    h2d_kick(&flowring);
    return 0;
}

/* ---------- bring-up ---------- */

static int rings_init(void)
{
    u32 ri = brcm.ring_info;
    u32 ringmem = tcm_read32(ri + RI_RINGMEM);
    u32 h2d_w = tcm_read32(ri + RI_H2D_W_IDX), h2d_r = tcm_read32(ri + RI_H2D_R_IDX);
    u32 d2h_w = tcm_read32(ri + RI_D2H_W_IDX), d2h_r = tcm_read32(ri + RI_D2H_R_IDX);

    if (brcm.version >= 6) {
        max_submission = tcm_read16(ri + RI_MAX_SUBMISSION);
        max_flowrings = tcm_read16(ri + RI_MAX_FLOWRINGS);
        max_completion = tcm_read16(ri + RI_MAX_COMPLETION);
    } else {
        max_submission = tcm_read16(ri + RI_MAX_FLOWRINGS);
        max_flowrings = max_submission - 2;
        max_completion = 3;
    }
    printf("wlan: rings: mem 0x%x, idx h2d w/r 0x%x/0x%x d2h w/r 0x%x/0x%x, %u flowrings, "
           "%u submission, %u completion (TCM indices)\n",
           ringmem, h2d_w, h2d_r, d2h_w, d2h_r, max_flowrings, max_submission, max_completion);
    if (!ringmem || max_flowrings < 1 || max_flowrings > 512 || ringmem >= brcm.tcm_size) {
        printf("wlan: ring info looks wrong\n");
        return -1;
    }

    const u16 *isz = brcm.version >= 7 ? ring_item_v7 : ring_item_pre_v7;
    for (int i = 0; i < NR_COMMON; i++) {
        struct ring *r = &rings[i];
        memset(r, 0, sizeof(*r));
        r->depth = ring_depth[i];
        r->item = isz[i];
        r->buf = dma_alloc((u32)r->depth * r->item, &r->iova);
        if (!r->buf)
            return -1;
        u32 desc = ringmem + i * RING_MEM_SZ;
        tcm_write32(desc + RING_BASE_OFF, (u32)r->iova);
        tcm_write32(desc + RING_BASE_OFF + 4, (u32)(r->iova >> 32));
        tcm_write16(desc + RING_MAX_ITEM_OFF, r->depth);
        tcm_write16(desc + RING_LEN_ITEMS_OFF, r->item);
        if (i < 2) { /* H2D: control submit, rx post */
            r->w_idx_addr = h2d_w + 4 * i;
            r->r_idx_addr = h2d_r + 4 * i;
        } else { /* D2H: control, tx, rx complete */
            r->w_idx_addr = d2h_w + 4 * (i - 2);
            r->r_idx_addr = d2h_r + 4 * (i - 2);
        }
    }
    /* the first flowring's indices follow the two common H2D rings */
    flow_h2d_w_idx = h2d_w + 4 * FLOWRING_IDSTART;
    flow_h2d_r_idx = h2d_r + 4 * FLOWRING_IDSTART;
    memset(&flowring, 0, sizeof(flowring));
    flowring.depth = FLOWRING_DEPTH;
    flowring.item = FLOWRING_ITEM;
    flowring.w_idx_addr = flow_h2d_w_idx;
    flowring.r_idx_addr = flow_h2d_r_idx;
    flowring.buf = dma_alloc(FLOWRING_DEPTH * FLOWRING_ITEM, &flowring.iova);
    flow_state = FLOW_NONE;
    return flowring.buf ? 0 : -1;
}

static int scratch_init(void)
{
    u64 iova;
    if (!dma_alloc(DMA_D2H_SCRATCH_LEN, &iova))
        return -1;
    tcm_write32(brcm.shared + SHARED_DMA_SCRATCH_ADDR, (u32)iova);
    tcm_write32(brcm.shared + SHARED_DMA_SCRATCH_ADDR + 4, (u32)(iova >> 32));
    tcm_write32(brcm.shared + SHARED_DMA_SCRATCH_LEN, DMA_D2H_SCRATCH_LEN);
    if (!dma_alloc(DMA_D2H_RINGUPD_LEN, &iova))
        return -1;
    tcm_write32(brcm.shared + SHARED_DMA_RINGUPD_ADDR, (u32)iova);
    tcm_write32(brcm.shared + SHARED_DMA_RINGUPD_ADDR + 4, (u32)(iova >> 32));
    tcm_write32(brcm.shared + SHARED_DMA_RINGUPD_LEN, DMA_D2H_RINGUPD_LEN);
    return 0;
}

static int buffers_init(void)
{
    for (u32 i = 0; i < NR_PKT; i++) {
        struct pkt *p = &pkts[i];
        p->kind = i < NR_RXDATA ? PKT_DATA : i < NR_RXDATA + NR_EVENT ? PKT_EVENT : PKT_IOCTL;
        p->size = p->kind == PKT_DATA ? DATA_BUF_SIZE : CTL_BUF_SIZE;
        p->posted = false;
        p->va = dma_alloc(p->size, &p->iova);
        if (!p->va)
            return -1;
    }
    for (u32 i = 0; i < NR_TX; i++) {
        txbufs[i].size = DATA_BUF_SIZE;
        txbufs[i].posted = false;
        txbufs[i].va = dma_alloc(DATA_BUF_SIZE, &txbufs[i].iova);
        if (!txbufs[i].va)
            return -1;
    }
    tx_next = tx_inflight = 0;
    ioctbuf = dma_alloc(2048, &ioctbuf_iova);
    return ioctbuf ? 0 : -1;
}

int brcm_msgbuf_init(void)
{
    msgbuf_up = false;
    memset(&link, 0, sizeof(link));
    if (!brcm.shared || !brcm.ring_info) {
        printf("wlan: firmware is not up\n");
        return -1;
    }
    if (dma_init() < 0 || rings_init() < 0 || scratch_init() < 0 || buffers_init() < 0)
        return -1;

    /* pcie.c brcmf_pcie_setup order: hostready, then brcmf_attach posts the buffers */
    if (brcm.flags & SHARED_HOSTRDY_DB1)
        pcie_write32((brcm.flags & SHARED_DAR) ? BRCM_PCIE64_H2D_MAILBOX_1 : BRCM_PCIE_H2D_MAILBOX_1,
                     1);
    msgbuf_up = true;
    /* never more data buffers than the firmware's max_rxbufpost (msgbuf.c rxbuf_data_fill) */
    u32 ndata = min((u32)NR_RXDATA, (u32)brcm.max_rxbufpost);
    for (u32 i = 0; i < NR_PKT; i++) {
        if (pkts[i].kind == PKT_DATA && i >= ndata) {
            pkts[i].kind = PKT_PARKED; /* never posted, never taken back */
            pkts[i].posted = true;
            continue;
        }
        post_pkt(i);
    }
    h2d_kick(&rings[R_RXPOST]);
    h2d_kick(&rings[R_CTRL_SUBMIT]);

    printf("wlan: msgbuf up: %u rx, %u event, %u ioctl buffers posted, %u KiB DMA used%s%s\n",
           ndata, NR_EVENT, NR_IOCTL, pool_off >> 10,
           (brcm.flags & SHARED_HOSTRDY_DB1) ? ", hostready sent" : "",
           (brcm.flags & SHARED_DAR) ? ", DAR doorbells" : "");
    mdelay(10);
    brcm_poll();
    dart_check("after buffer posting");
    return 0;
}
