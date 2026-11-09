/* SPDX-License-Identifier: MIT */
/*
 * DockChannel HID host for the J613 MTP (built-in keyboard + trackpad).
 *
 * Sequence and framing follow Linux drivers/hid/dockchannel-hid/dockchannel-hid.c,
 * drivers/soc/apple/dockchannel.c and U-Boot drivers/input/apple_mtp_kbd.c
 * (Copyright The Asahi Linux Contributors, GPL-2.0 OR MIT; used under MIT).
 * T8122 addresses: Linux arch/arm64/boot/dts/apple/t8122.dtsi (mtp_dockchannel).
 * J613 GPIOs: t8122-j613.dts (afe-reset = SMC GPIO 8, stm-reset = SMC GPIO 24,
 * both active low); t8122-j613-25g83.dts (multi-touch power method 2).
 */
#include "hv_dchid.h"
#include "adt.h"
#include "dchid_proto.h"
#include "hv.h"
#include "hv_mtp.h"
#include "hv_vgic.h"
#include "keyboard_types.h"
#include "malloc.h"
#include "smc.h"
#include "soc.h"
#include "string.h"
#include "utils.h"

/* T8122 DockChannel (Linux t8122.dtsi: fifo@2fab14000, ranges 0x2fab28000) */
#define DC_IRQ_BASE   0x2fab14000UL
#define DC_DATA_BASE  0x2fab28000UL
#define DC_RMT_CONFIG (DC_DATA_BASE + 0x0000)
#define DC_RMT_DATA   (DC_DATA_BASE + 0x4000)
#define DC_CONFIG     (DC_DATA_BASE + 0x8000)
#define DC_DATA       (DC_DATA_BASE + 0xc000)

#define IRQ_MASK         0x0
#define IRQ_FLAG         0x4
#define CONFIG_RX_THRESH 0x4
#define DATA_TX32        0x10
#define DATA_TX_FREE     0x14
#define DATA_RX8         0x1c
#define DATA_RX32        0x28
#define DATA_RX_COUNT    0x2c

/* SMC GPIO (Linux gpio-macsmc.c): key "gPxx", value CMD_OUTPUT | physical level */
#define SMC_GPIO_CMD_OUTPUT (1u << 24)
#define J613_GPIO_AFE_RESET 8
#define J613_GPIO_STM_RESET 24

#define CMD_TIMEOUT_MS   1000
#define ENUM_QUIET_MS    50
#define START_TIMEOUT_MS 2000

/* Guest report rings: the keyboard never loses its latest state to trackpad traffic. */
#define VHID_KBD_SLOTS 16
#define VHID_MT_SLOTS  48

struct iface {
    bool present;
    char name[DCHID_NAME_LEN + 1];
    u8 desc[DCHID_DESC_MAX];
    u16 desc_len;
    bool gpio;
    u16 gpio_id;
    char gpio_name[DCHID_GPIO_NAME + 1];
    bool init_done;
    bool enabled;
    bool enable_failed;
    bool ready;
    u8 tx_seq;
    u32 reports;
};

static struct iface ifaces[DCHID_MAX_IFACES];
static bool dc_up, id_ready, stm_present;
static int kbd_idx = -1, mt_idx = -1, stm_idx = -1;
static u16 vendor_id = 0x05ac, product_id;
static u64 last_rx_ticks;

/* receive state: one packet at a time */
static u8 *rx_pkt;
static size_t rx_have, rx_need;

/* outstanding synchronous command */
static struct {
    bool active, done;
    u8 iface, flags, report;
    u32 retcode;
    u8 *resp;
    size_t resp_cap, resp_len;
} cmd;
static bool in_cmd;

/* deferred work found while dispatching packets */
static bool gpio_pending;
static struct dchid_gpio_cmd gpio_req;
static u8 gpio_req_raw[32];
static size_t gpio_req_len;

/* trackpad */
static u8 *mt_fw;
static size_t mt_fw_size;
static bool mt_started, mt_start_failed, dims_valid;
/*
 * Linux dockchannel-hid resets multi-touch with power method 1
 * {0x40, 1, iface, state}; this J613 MTP firmware rejects method 2
 * (0xe00002c2, EXP-20261108-044), so start with method 1.
 */
static u8 mt_power_method = DCHID_POWER_METHOD_1;
static u8 dims[16];

/*
 * Guest-visible report queue: one ring per interface, oldest report dropped on
 * overflow (keyboard and trackpad reports are full-state snapshots, so the
 * newest one must survive or a key/finger stays down in Windows). The guest
 * reads RX_LEN/RX_IFACE/REPORT from a latched copy of the oldest report, so a
 * push that evicts it mid-read cannot tear the data; RX_POP releases it.
 */
static DECLARE_SPINLOCK(vhid_lock);
static struct dchid_vq vq;
static bool kbd_iso;
static u32 mt_trimmed, mt_bad;
static u32 vhid_desc_sel;
static bool vhid_irq_pending;
static u32 report_log_budget = 24;
static u32 kbd_other_reports;

/*
 * What the guest does with the AVHD page (EXP-049 follow-up): the driver's
 * PrepareHardware reads MAGIC, its timer reads STATUS/RX_LEN, the keyboard
 * load reads DESC_LEN + the DESC window, every drained report is a POP.
 * First occurrences are printed at once, a summary every 10 s on change.
 */
static struct {
    u32 magic, status, desc_len, desc_win, dims, rx_len, pops, acks, sel;
    u32 injected;
    bool irq_enabled;
} avhd_tm, avhd_tm_logged;
static u64 avhd_tm_ticks;

static u32 get_le32p(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* ---------- SMC GPIO ---------- */

static u32 smc_gpio_key(unsigned int gpio)
{
    static const char hex[] = "0123456789abcdef";
    return ((u32)'g' << 24) | ((u32)'P' << 16) | ((u32)hex[(gpio >> 4) & 0xf] << 8) |
           (u32)hex[gpio & 0xf];
}

/*
 * Active-low reset lines (asserted = physical 0) are SMC GPIO keys. Like
 * upstream dcp.c: open the SMC, write, and shut it down again. Leaving the
 * SMC RTKit booted with nobody servicing it took the machine down shortly
 * after bring-up (SError, EXP-20261108-045).
 */
static bool smc_gpio_write(smc_dev_t *smc, unsigned int gpio, bool asserted)
{
    int ret = smc_write_u32(smc, smc_gpio_key(gpio), SMC_GPIO_CMD_OUTPUT | (asserted ? 0 : 1));
    if (ret)
        printf("dchid: SMC GPIO %u <- %s failed (%d)\n", gpio, asserted ? "assert" : "release",
               ret);
    return ret == 0;
}

/* pulse_ms 0: set the line once; otherwise assert, wait, release. */
static bool reset_line_op(unsigned int gpio, bool asserted, unsigned int pulse_ms)
{
    smc_dev_t *smc = smc_init();
    bool ok;

    if (!smc) {
        printf("dchid: smc_init failed, cannot drive GPIO %u\n", gpio);
        return false;
    }
    ok = smc_gpio_write(smc, gpio, asserted);
    if (ok && pulse_ms) {
        mdelay(pulse_ms);
        ok = smc_gpio_write(smc, gpio, false);
    }
    smc_shutdown(smc);
    return ok;
}

static bool reset_line_set(unsigned int gpio, bool asserted)
{
    return reset_line_op(gpio, asserted, 0);
}

static int gpio_for_name(const char *name)
{
    if (!strcmp(name, "afe-reset"))
        return J613_GPIO_AFE_RESET;
    if (!strcmp(name, "stm-reset"))
        return J613_GPIO_STM_RESET;
    return -1;
}

/* ---------- DockChannel FIFO ---------- */

static bool dc_write(const u8 *buf, size_t len)
{
    while (len) {
        u64 start = get_ticks();
        while (read32(DC_DATA + DATA_TX_FREE) < 4) {
            if (ticks_to_msecs(get_ticks() - start) > 100)
                return false;
        }
        write32(DC_DATA + DATA_TX32, get_le32p(buf));
        buf += 4;
        len -= 4;
    }
    return true;
}

/* ---------- guest queue ---------- */

static void vhid_push(u8 viface, const u8 *data, size_t len)
{
    spin_lock(&vhid_lock);
    dchid_vq_push(&vq, viface, data, len);
    spin_unlock(&vhid_lock);
}

static void vhid_kick(void)
{
    if (vhid_irq_pending || !vq.count)
        return;
    if (!hv_vgic3_irq_enabled(HV_AVHD_GSIV) || hv_vgic3_get_free_lr() < 0)
        return;
    vhid_irq_pending = true;
    avhd_tm.injected++;
    hv_vgic3_inject_irq(HV_AVHD_GSIV, hv_vgic3_get_priority(HV_AVHD_GSIV), false, true, false, 0);
}

static u32 vhid_status(void)
{
    u32 st = 0;
    if (kbd_idx >= 0 && ifaces[kbd_idx].ready)
        st |= BIT(0);
    if (mt_idx >= 0 && ifaces[mt_idx].ready)
        st |= BIT(1);
    if (hv_mtp_running())
        st |= BIT(2);
    if (dims_valid)
        st |= BIT(3);
    return st;
}

static const struct iface *vhid_desc_iface(void)
{
    int idx = vhid_desc_sel ? mt_idx : kbd_idx;
    return idx >= 0 ? &ifaces[idx] : NULL;
}

/* DESC_SEL 0 serves the fixed guest keyboard descriptor, 1 the raw multi-touch one. */
static const u8 *vhid_desc_bytes(u32 *len)
{
    const struct iface *i = vhid_desc_iface();

    *len = 0;
    if (!i || !i->desc_len)
        return NULL;
    if (!vhid_desc_sel) {
        *len = dchid_guest_kbd_desc_len;
        return dchid_guest_kbd_desc;
    }
    *len = i->desc_len;
    return i->desc;
}

static void avhd_first(u32 count, const char *what)
{
    if (count == 1)
        printf("dchid: avhd guest %s\n", what);
}

static u32 window_word(const u8 *buf, size_t len, u64 off)
{
    u32 v = 0;
    for (int i = 0; i < 4; i++)
        if (off + i < len)
            v |= (u32)buf[off + i] << (8 * i);
    return v;
}

static bool avhd_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    u64 off = addr - HV_AVHD_IPA;
    UNUSED(ctx);

    if (width > 3)
        return false;
    off &= ~3UL;

    if (write) {
        switch (off) {
            case 0x00c:
                vhid_irq_pending = false;
                avhd_first(++avhd_tm.acks, "IRQ ack (driver transport started)");
                break;
            case 0x010:
                vhid_desc_sel = *val ? 1 : 0;
                avhd_tm.sel++;
                break;
            case 0x028:
                avhd_first(++avhd_tm.pops, "popped its first report (driver drains input)");
                spin_lock(&vhid_lock);
                dchid_vq_pop(&vq);
                spin_unlock(&vhid_lock);
                break;
            default:
                break;
        }
        return true;
    }

    u32 v = 0;
    if (off == 0x000) {
        v = HV_AVHD_MAGIC;
        avhd_first(++avhd_tm.magic, "read MAGIC (AppleMtpInput PrepareHardware ran)");
        /* A (re)starting driver must not replay keys typed before it existed. */
        spin_lock(&vhid_lock);
        dchid_vq_flush(&vq);
        spin_unlock(&vhid_lock);
    } else if (off == 0x004) {
        v = 1;
    } else if (off == 0x008) {
        v = vhid_status();
        avhd_first(++avhd_tm.status, "read STATUS (driver readiness poll)");
    } else if (off == 0x014) {
        vhid_desc_bytes(&v);
        if (!vhid_desc_sel)
            avhd_first(++avhd_tm.desc_len, "read keyboard DESC_LEN (loading descriptor)");
    } else if (off == 0x020 || off == 0x024) {
        if (off == 0x020)
            avhd_tm.rx_len++;
        spin_lock(&vhid_lock);
        const struct dchid_vq_slot *cur = dchid_vq_latch(&vq);
        if (cur)
            v = off == 0x020 ? cur->len : cur->iface;
        spin_unlock(&vhid_lock);
    } else if (off == 0x02c) {
        v = vq.drops;
    } else if (off == 0x030) {
        v = vendor_id | ((u32)product_id << 16);
    } else if (off >= 0x040 && off < 0x050) {
        v = window_word(dims, sizeof(dims), off - 0x040);
        if (off == 0x040)
            avhd_first(++avhd_tm.dims, "read DIMS (driver loading the trackpad)");
    } else if (off >= 0x400 && off < 0x800) {
        u32 dlen;
        const u8 *d = vhid_desc_bytes(&dlen);
        if (d)
            v = window_word(d, dlen, off - 0x400);
        avhd_tm.desc_win++;
    } else if (off >= 0x800) {
        spin_lock(&vhid_lock);
        if (vq.cur_valid)
            v = window_word(vq.cur.data, vq.cur.len, off - 0x800);
        spin_unlock(&vhid_lock);
    }
    *val = v;
    return true;
}

/* ---------- packet dispatch ---------- */

static void handle_event(const u8 *p, size_t len)
{
    if (!len)
        return;

    switch (p[0]) {
        case DCHID_EVENT_INIT: {
            struct dchid_init_info info;
            if (!dchid_parse_init(p, len, &info) || info.iface >= DCHID_MAX_IFACES)
                return;
            struct iface *i = &ifaces[info.iface];
            if (!i->present) {
                i->present = true;
                memcpy(i->name, info.name, sizeof(i->name));
                printf("dchid: interface %u '%s'\n", info.iface, i->name);
                if (!strcmp(i->name, "keyboard"))
                    kbd_idx = info.iface;
                else if (!strcmp(i->name, "multi-touch"))
                    mt_idx = info.iface;
                else if (!strcmp(i->name, "stm")) {
                    stm_idx = info.iface;
                    stm_present = true;
                }
            }
            if (info.descriptor && info.descriptor_len) {
                if (info.descriptor_len <= sizeof(i->desc)) {
                    memcpy(i->desc, info.descriptor, info.descriptor_len);
                    i->desc_len = info.descriptor_len;
                    unsigned int fixed = dchid_fixup_report_size(i->desc, i->desc_len);
                    printf("dchid:   %s HID descriptor %u bytes (%u oversized report-size items fixed)\n",
                           i->name, i->desc_len, fixed);
                    for (u32 o = 0; o < i->desc_len; o += 32) {
                        printf("dchid:   %s rdesc +%03x:", i->name, o);
                        for (u32 b = o; b < i->desc_len && b < o + 32; b++)
                            printf(" %02x", i->desc[b]);
                        printf("\n");
                    }
                } else {
                    printf("dchid:   %s HID descriptor too large (%u)\n", i->name,
                           info.descriptor_len);
                }
            }
            if (info.gpio_requested) {
                i->gpio = true;
                i->gpio_id = info.gpio_id;
                memcpy(i->gpio_name, info.gpio_name, sizeof(i->gpio_name));
                printf("dchid:   %s requests GPIO #%u '%s'\n", i->name, i->gpio_id, i->gpio_name);
            }
            if (!info.more_packets)
                i->init_done = true;
            break;
        }
        case DCHID_EVENT_READY:
            if (len >= 2 && p[1] < DCHID_MAX_IFACES && ifaces[p[1]].present) {
                ifaces[p[1]].ready = true;
                printf("dchid: interface %u '%s' ready\n", p[1], ifaces[p[1]].name);
            }
            break;
        case DCHID_EVENT_GPIO_CMD:
            if (len >= sizeof(struct dchid_gpio_cmd) && len <= sizeof(gpio_req_raw)) {
                memcpy(&gpio_req, p, sizeof(gpio_req));
                memcpy(gpio_req_raw, p, len);
                gpio_req_len = len;
                gpio_pending = true;
            }
            break;
        default:
            break;
    }
}

/*
 * Hand the guest exactly what the (J313-validated) Windows decoder accepts:
 * 38-byte header + num_fingers * 30. Linux accepts reports with spare finger
 * slots (npoints >= num_fingers) and only looks at buttons bit 0; do the same.
 */
static void mt_forward(const u8 *p, size_t len)
{
    static u8 buf[DCHID_MT_GUEST_MAX];
    size_t n = 0;
    int ret = dchid_mt_sanitize(p, len, buf, sizeof(buf), &n);

    if (ret < 0) {
        if (mt_bad++ < 4)
            printf("dchid: trackpad report len %lu (%u fingers) unusable, dropped\n", len,
                   len > DCHID_MT_NFINGERS_OFFSET ? p[DCHID_MT_NFINGERS_OFFSET] : 0);
        return;
    }
    if (ret > 0 && mt_trimmed++ < 4)
        printf("dchid: trackpad report len %lu, %u fingers: trimmed to %lu\n", len,
               p[DCHID_MT_NFINGERS_OFFSET], n);
    vhid_push(1, buf, n);
}

static void handle_report(int idx, const u8 *p, size_t len)
{
    struct iface *i = &ifaces[idx];

    i->reports++;
    if (report_log_budget && (idx == kbd_idx || i->reports <= 2)) {
        report_log_budget--;
        printf("dchid: %s report #%u len %lu:", i->name, i->reports, len);
        for (size_t n = 0; n < len && n < 24; n++)
            printf(" %02x", p[n]);
        printf("\n");
    }
    if (idx == kbd_idx) {
        /* The guest descriptor only has report 1 (dchid_guest_kbd_desc). */
        if (len && p[0] == DCHID_KBD_REPORT_ID) {
            u8 r[DCHID_KBD_REPORT_LEN] = {0};
            memcpy(r, p, len < sizeof(r) ? len : sizeof(r));
            /*
             * Apple ISO keyboards swap the key left of 1 and the one right of
             * left shift (Linux hid-apple apple_iso_keyboard, APPLE_ISO_TILDE_QUIRK).
             */
            if (kbd_iso)
                dchid_kbd_iso_swap(r);
            vhid_push(0, r, sizeof(r));
        } else if (kbd_other_reports++ < 4) {
            printf("dchid: keyboard report id 0x%02x len %lu not forwarded\n", len ? p[0] : 0,
                   len);
        }
    }
    else if (idx == mt_idx && len && p[0] == DCHID_MT_REPORT_ID)
        mt_forward(p, len);
}

static void handle_packet(const struct dchid_hdr *hdr, const u8 *body)
{
    const struct dchid_subhdr *sub = (const void *)body;
    const u8 *payload = body + sizeof(*sub);

    if (!dchid_packet_valid(hdr, body)) {
        printf("dchid: bad checksum (iface %u ch 0x%x len %u)\n", hdr->iface, hdr->channel,
               hdr->length);
        return;
    }
    if (hdr->iface >= DCHID_MAX_IFACES || hdr->length < sizeof(*sub) ||
        sub->length + sizeof(*sub) > hdr->length)
        return;

    if (hdr->channel == DCHID_CHANNEL_CMD) {
        if (cmd.active && !cmd.done && hdr->iface == cmd.iface && sub->flags == cmd.flags &&
            hdr->seq == ifaces[cmd.iface].tx_seq && sub->length >= 1 &&
            payload[0] == cmd.report) {
            size_t n = sub->length - 1;
            if (cmd.resp && n > cmd.resp_cap)
                n = cmd.resp_cap;
            if (cmd.resp && n)
                memcpy(cmd.resp, payload + 1, n);
            cmd.resp_len = n;
            cmd.retcode = sub->retcode;
            cmd.done = true;
        }
        return;
    }

    if (DCHID_FLAGS_TYPE(sub->flags) != DCHID_TYPE_INPUT)
        return;
    if (hdr->iface == DCHID_IFACE_COMM)
        handle_event(payload, sub->length);
    else if (ifaces[hdr->iface].present)
        handle_report(hdr->iface, payload, sub->length);
}

/* RX diagnostics, printed when bring-up times out. */
static u32 dc_rx_bytes, dc_rx_pkts, dc_rx_resync;
static u8 dc_rx_first[32];

/* Drain the RX FIFO, dispatching complete packets. */
static void dc_pump(void)
{
    u32 avail;

    while ((avail = read32(DC_DATA + DATA_RX_COUNT)) != 0) {
        size_t want = (rx_have < sizeof(struct dchid_hdr) ? sizeof(struct dchid_hdr) : rx_need) -
                      rx_have;

        last_rx_ticks = get_ticks();
        if (avail >= 4 && want >= 4) {
            u32 w = read32(DC_DATA + DATA_RX32);
            memcpy(rx_pkt + rx_have, &w, 4);
            for (int b = 0; b < 4; b++)
                if (dc_rx_bytes + b < sizeof(dc_rx_first))
                    dc_rx_first[dc_rx_bytes + b] = w >> (8 * b);
            rx_have += 4;
            dc_rx_bytes += 4;
        } else {
            rx_pkt[rx_have] = read32(DC_DATA + DATA_RX8) >> 8;
            if (dc_rx_bytes < sizeof(dc_rx_first))
                dc_rx_first[dc_rx_bytes] = rx_pkt[rx_have];
            rx_have++;
            dc_rx_bytes++;
        }

        if (rx_have == sizeof(struct dchid_hdr)) {
            struct dchid_hdr *h = (void *)rx_pkt;
            if (h->hdr_len != sizeof(*h)) {
                printf("dchid: bad header length %u, resyncing\n", h->hdr_len);
                dc_rx_resync++;
                rx_have = 0;
                continue;
            }
            rx_need = sizeof(*h) + h->length + 4;
        }
        if (rx_have > sizeof(struct dchid_hdr) && rx_have == rx_need) {
            dc_rx_pkts++;
            handle_packet((void *)rx_pkt, rx_pkt + sizeof(struct dchid_hdr));
            rx_have = 0;
            rx_need = 0;
        }
    }
}

/* ---------- commands ---------- */

static int dc_cmd(int idx, u8 type, u8 req, const void *data, size_t len, void *resp,
                  size_t resp_cap)
{
    static u8 pkt[128];
    struct iface *i = &ifaces[idx];
    size_t n;
    int ret = 0;

    if (in_cmd || !len)
        return -1;
    n = dchid_build(pkt, sizeof(pkt), DCHID_CHANNEL_CMD, idx, i->tx_seq, DCHID_FLAGS(type, req),
                    data, len);
    if (!n)
        return -1;

    in_cmd = true;
    cmd.active = true;
    cmd.done = false;
    cmd.iface = idx;
    cmd.flags = DCHID_FLAGS(type, req);
    cmd.report = ((const u8 *)data)[0];
    cmd.resp = resp;
    cmd.resp_cap = resp_cap;
    cmd.resp_len = 0;
    cmd.retcode = 0;

    if (!dc_write(pkt, n)) {
        printf("dchid: TX FIFO stuck (iface %d report 0x%x)\n", idx, cmd.report);
        ret = -1;
        goto out;
    }
    u64 start = get_ticks();
    while (!cmd.done) {
        dc_pump();
        if (ticks_to_msecs(get_ticks() - start) > CMD_TIMEOUT_MS) {
            printf("dchid: command 0x%x to %s timed out\n", cmd.report, i->name);
            ret = -1;
            goto out;
        }
    }
    if (cmd.retcode) {
        printf("dchid: command 0x%x to %s failed 0x%x\n", cmd.report, i->name, cmd.retcode);
        ret = -1;
    } else {
        ret = cmd.resp_len;
    }
out:
    i->tx_seq++;
    cmd.active = false;
    in_cmd = false;
    return ret;
}

static int comm_cmd(const void *msg, size_t len)
{
    return dc_cmd(DCHID_IFACE_COMM, DCHID_TYPE_FEATURE, DCHID_REQ_SET, msg, len, NULL, 0);
}

static int get_feature(int idx, u8 report, void *buf, size_t cap)
{
    return dc_cmd(idx, DCHID_TYPE_FEATURE, DCHID_REQ_GET, &report, 1, buf, cap);
}

static void handle_gpio_request(void)
{
    struct {
        u8 type;
        u32 retcode;
        u8 cmd[32];
    } __attribute__((packed)) ack;
    u32 retcode = 0xe000f00d;
    struct iface *i = gpio_req.iface < DCHID_MAX_IFACES ? &ifaces[gpio_req.iface] : NULL;

    gpio_pending = false;
    if (i && i->gpio && gpio_req.gpio == i->gpio_id) {
        int line = gpio_for_name(i->gpio_name);
        printf("dchid: GPIO command %u for %s '%s' (line %d)\n", gpio_req.cmd, i->name,
               i->gpio_name, line);
        if (line >= 0 && gpio_req.cmd == 3) { /* pulse */
            if (reset_line_op(line, true, 10))
                retcode = 0;
        }
    } else {
        printf("dchid: GPIO command for unknown interface/GPIO\n");
    }

    ack.type = DCHID_CMD_ACK_GPIO_CMD;
    ack.retcode = retcode;
    memcpy(ack.cmd, gpio_req_raw, gpio_req_len);
    comm_cmd(&ack, 5 + gpio_req_len);
}

static bool mt_set_power(u8 state)
{
    u8 msg[9];
    size_t n;

    if (mt_power_method != DCHID_POWER_METHOD_2) {
        n = dchid_power_cmd(msg, DCHID_POWER_METHOD_1, mt_idx, state, false);
        return comm_cmd(msg, n) >= 0;
    }
    n = dchid_power_cmd(msg, DCHID_POWER_METHOD_2, mt_idx, state, false);
    if (comm_cmd(msg, n) < 0) {
        /*
         * Power method 2 (aurora's J613 "25G83" profile) is firmware-version
         * specific; this MTP firmware rejected it (0xe00002c2, bad argument,
         * EXP-20261108-044). Fall back to Linux mainline power method 1:
         * {0x40, 1, iface, state}; the MTP then sequences the trackpad itself
         * and asks for reset pulses through GPIO_CMD events.
         */
        printf("dchid: power method 2 rejected, falling back to method 1\n");
        mt_power_method = DCHID_POWER_METHOD_1;
        n = dchid_power_cmd(msg, DCHID_POWER_METHOD_1, mt_idx, state, false);
        return comm_cmd(msg, n) >= 0;
    }
    reset_line_set(J613_GPIO_AFE_RESET, state == DCHID_POWER_OFF);
    if (state == DCHID_POWER_OFF)
        mdelay(50);
    n = dchid_power_cmd(msg, DCHID_POWER_METHOD_2, mt_idx, state, true);
    return comm_cmd(msg, n) >= 0;
}

static void mt_start(void)
{
    const u8 *fw;
    u32 fw_len, iface_off;
    u64 dva;

    mt_started = true;
    if (!dchid_fw_parse(mt_fw, mt_fw_size, &fw, &fw_len, &iface_off)) {
        printf("dchid: trackpad firmware is not a valid HIDF container\n");
        mt_start_failed = true;
        return;
    }
    u8 *buf = memalign(SZ_16K, ALIGN_UP(fw_len, SZ_16K));
    if (!buf) {
        mt_start_failed = true;
        return;
    }
    memcpy(buf, fw, fw_len);
    if (iface_off)
        buf[iface_off] = mt_idx;
    if (!hv_mtp_map(buf, fw_len, &dva)) {
        printf("dchid: cannot DMA-map the trackpad firmware\n");
        mt_start_failed = true;
        return;
    }

    struct {
        u8 cmd, unk1, unk2, iface;
        u64 addr;
        u32 size;
    } __attribute__((packed)) msg = {DCHID_CMD_SEND_FIRMWARE, 2, 0, (u8)mt_idx, dva, fw_len};

    printf("dchid: sending %u bytes of trackpad firmware (dva 0x%lx, power method %u)\n", fw_len,
           dva, mt_power_method);
    if (comm_cmd(&msg, sizeof(msg)) < 0 || !mt_set_power(DCHID_POWER_OFF) ||
        !mt_set_power(DCHID_POWER_ON)) {
        printf("dchid: trackpad start failed\n");
        mt_start_failed = true;
        return;
    }

    u64 start = get_ticks();
    while (!ifaces[mt_idx].ready && ticks_to_msecs(get_ticks() - start) < START_TIMEOUT_MS) {
        hv_mtp_poll();
        dc_pump();
        /* method 1: the MTP requests AFE reset pulses while it starts the trackpad */
        if (gpio_pending && !in_cmd)
            handle_gpio_request();
    }
    if (!ifaces[mt_idx].ready) {
        printf("dchid: trackpad not ready after firmware\n");
        mt_start_failed = true;
        return;
    }

    u8 buf2[32];
    int n = get_feature(mt_idx, DCHID_MT_DIMENSIONS_REPORT, buf2, sizeof(buf2));
    if (n >= 16) {
        memcpy(dims, buf2, 16);
        dims_valid = true;
        printf("dchid: trackpad %u x %u (1/100 mm), x %d..%d y %d..%d\n", get_le32p(dims),
               get_le32p(dims + 4), (s16)(dims[8] | dims[9] << 8), (s16)(dims[12] | dims[13] << 8),
               (s16)(dims[10] | dims[11] << 8), (s16)(dims[14] | dims[15] << 8));
    } else {
        printf("dchid: trackpad dimensions unavailable (%d)\n", n);
    }
}

/* Work that needs synchronous commands; never runs from inside a dispatch. */
static void process_actions(void)
{
    if (in_cmd)
        return;

    if (gpio_pending)
        handle_gpio_request();

    /* Like U-Boot: enable only after the announcements have gone quiet. */
    if (ticks_to_msecs(get_ticks() - last_rx_ticks) >= ENUM_QUIET_MS) {
        for (int idx = 1; idx < DCHID_MAX_IFACES; idx++) {
            struct iface *i = &ifaces[idx];
            if (!i->present || !i->init_done || i->enabled || i->enable_failed)
                continue;
            /* STM first: it reports the device IDs (Linux defers the rest). */
            if (stm_present && !id_ready && idx != stm_idx)
                continue;
            u8 msg[2] = {DCHID_CMD_ENABLE_IFACE, (u8)idx};
            if (comm_cmd(msg, sizeof(msg)) >= 0) {
                i->enabled = true;
                printf("dchid: enabled '%s'\n", i->name);
            } else {
                i->enable_failed = true;
            }
        }
    }

    if (stm_idx >= 0 && ifaces[stm_idx].ready && !id_ready) {
        u8 id[16];
        int n = get_feature(stm_idx, DCHID_STM_REPORT_ID, id, sizeof(id));
        if (n >= 7) {
            vendor_id = id[1] | (id[2] << 8);
            product_id = id[3] | (id[4] << 8);
            printf("dchid: STM vendor 0x%04x product 0x%04x keyboard type %u\n", vendor_id,
                   product_id, n > 9 ? id[9] : 0);
        }
        id_ready = true;
    }

    if (mt_idx >= 0 && ifaces[mt_idx].enabled && !mt_started && mt_fw)
        mt_start();
}

/* ---------- public ---------- */

/* Set from the host (P_HV_MTP_FIRMWARE with addr 0, size 0): skip the MTP. */
static bool mtp_disabled;

bool hv_dchid_mtp_disabled(void)
{
    return mtp_disabled;
}

bool hv_dchid_set_mt_firmware(void *blob, size_t size)
{
    if (!blob && !size) {
        /* Stability switch (EXP-20261108-047): no MTP/DockChannel bring-up at
         * all; the AVHD page is still mapped and reads STATUS 0. */
        mtp_disabled = true;
        printf("dchid: MTP bring-up disabled by the host\n");
        return true;
    }
    if (!blob || !size)
        return false;
    mt_fw = blob;
    mt_fw_size = size;
    printf("dchid: trackpad firmware staged (%lu bytes)\n", size);
    return true;
}

/* Same source and table as kboot.c dt_set_ipd() (hid-country-code for Linux). */
static bool kbd_layout_is_iso(void)
{
    int node = adt_path_offset(adt, "/arm-io/dockchannel-mtp/mtp-transport/keyboard");
    u32 len = 0;
    const u8 *kblang = node >= 0 ? adt_getprop(adt, node, "kblang-calibration", &len) : NULL;

    if (!kblang || len < 2) {
        printf("dchid: keyboard layout unknown (no kblang-calibration), no ISO swap\n");
        return false;
    }
    u8 code = kblang[1];
    /* code 0 is "Unknown": do not guess, an ANSI keyboard would lose its ` key. */
    bool iso = code && code < ARRAY_SIZE(keyboard_types) && keyboard_types[code] == LAYOUT_ISO;
    printf("dchid: keyboard layout code %u (%s)%s\n", code,
           code < ARRAY_SIZE(keyboard_types)
               ? (keyboard_types[code] == LAYOUT_ISO   ? "ISO"
                  : keyboard_types[code] == LAYOUT_JIS ? "JIS"
                                                       : "ANSI")
               : "?",
           iso ? ", swapping ` and <> like Linux hid-apple" : "");
    return iso;
}

bool hv_dchid_init(void)
{
    static bool avhd_mapped;

    if (chip_id != T8122 || dc_up)
        return dc_up;
    /*
     * Always back the AVHD page on J613, even if the MTP failed: the DSDT
     * advertises it unconditionally and an unbacked IPA would fault Windows.
     * STATUS then reads 0 and the driver simply publishes nothing.
     */
    if (!avhd_mapped) {
        if (hv_map_hook(HV_AVHD_IPA, avhd_hook, 0x1000))
            printf("dchid: cannot map the AVHD device at 0x%lx\n", HV_AVHD_IPA);
        else
            printf("dchid: AVHD device at 0x%lx, GSIV %d\n", HV_AVHD_IPA, HV_AVHD_GSIV);
        avhd_mapped = true;
    }
    if (!hv_mtp_running()) {
        printf("dchid: MTP not running, no built-in keyboard/trackpad\n");
        return false;
    }

    rx_pkt = malloc(DCHID_MAX_PKT + sizeof(struct dchid_hdr));
    vq.ring[0].cap = VHID_KBD_SLOTS;
    vq.ring[0].slot = malloc(sizeof(struct dchid_vq_slot) * VHID_KBD_SLOTS);
    vq.ring[1].cap = VHID_MT_SLOTS;
    vq.ring[1].slot = malloc(sizeof(struct dchid_vq_slot) * VHID_MT_SLOTS);
    kbd_iso = kbd_layout_is_iso();
    if (!rx_pkt || !vq.ring[0].slot || !vq.ring[1].slot) {
        printf("dchid: out of memory\n");
        return false;
    }
    rx_have = rx_need = 0;

    /* Polled: no DockChannel interrupts; RX threshold = one header (as Linux/U-Boot). */
    write32(DC_IRQ_BASE + IRQ_MASK, 0);
    write32(DC_IRQ_BASE + IRQ_FLAG, ~0U);
    write32(DC_CONFIG + CONFIG_RX_THRESH, sizeof(struct dchid_hdr));
    ifaces[0].present = true;
    strcpy(ifaces[0].name, "comm");
    dc_up = true;
    last_rx_ticks = get_ticks();

    /* Service the MTP until the keyboard is ready (U-Boot waits up to 1 s). */
    u64 start = get_ticks();
    while (ticks_to_msecs(get_ticks() - start) < 3000) {
        hv_mtp_poll();
        dc_pump();
        process_actions();
        if (kbd_idx >= 0 && ifaces[kbd_idx].ready && (!mt_fw || mt_started))
            break;
    }
    printf("dchid: bring-up %s after %lu ms (keyboard %s, trackpad %s)\n",
           kbd_idx >= 0 && ifaces[kbd_idx].ready ? "done" : "incomplete",
           ticks_to_msecs(get_ticks() - start),
           kbd_idx >= 0 ? (ifaces[kbd_idx].ready ? "ready" : "not ready") : "absent",
           mt_idx < 0 ? "absent" : (ifaces[mt_idx].ready ? "ready" : "waiting for firmware"));
    printf("dchid: rx %u bytes, %u packets, %u resyncs; RX_COUNT=%u IRQ_FLAG=0x%x TX_FREE=%u\n",
           dc_rx_bytes, dc_rx_pkts, dc_rx_resync, read32(DC_DATA + DATA_RX_COUNT),
           read32(DC_IRQ_BASE + IRQ_FLAG), read32(DC_DATA + DATA_TX_FREE));
    if (dc_rx_bytes) {
        printf("dchid: first rx bytes:");
        for (u32 b = 0; b < dc_rx_bytes && b < sizeof(dc_rx_first); b++)
            printf(" %02x", dc_rx_first[b]);
        printf("\n");
    }

    return true;
}

/* Hypervisor tick (boot CPU). */
static void avhd_telemetry(void)
{
    u64 now = get_ticks();

    if (ticks_to_msecs(now - avhd_tm_ticks) < 1000)
        return;
    avhd_tm_ticks = now;
    bool en = hv_vgic3_irq_enabled(HV_AVHD_GSIV);
    if (en != avhd_tm.irq_enabled) {
        avhd_tm.irq_enabled = en;
        printf("dchid: avhd guest %s GSIV %d in the vGIC\n", en ? "enabled" : "disabled",
               HV_AVHD_GSIV);
    }
    static u32 seconds;
    if (++seconds % 10)
        return;
    if (!memcmp(&avhd_tm, &avhd_tm_logged, sizeof(avhd_tm)))
        return;
    avhd_tm_logged = avhd_tm;
    printf("dchid: avhd stats magic=%u status=%u desc_len=%u desc_win=%u dims=%u rx_len=%u "
           "pops=%u acks=%u sel=%u irq_en=%d injected=%u queued=%u drops=%u mt_trim=%u "
           "mt_bad=%u\n",
           avhd_tm.magic, avhd_tm.status, avhd_tm.desc_len, avhd_tm.desc_win, avhd_tm.dims,
           avhd_tm.rx_len, avhd_tm.pops, avhd_tm.acks, avhd_tm.sel, avhd_tm.irq_enabled,
           avhd_tm.injected, vq.count, vq.drops, mt_trimmed, mt_bad);
}

void hv_dchid_poll(void)
{
    if (!dc_up || in_cmd)
        return;
    dc_pump();
    process_actions();
    vhid_kick();
    avhd_telemetry();
}
