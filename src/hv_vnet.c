/* SPDX-License-Identifier: MIT */
/*
 * virtio-net for the guest, backed by the J613 built-in Wi-Fi (see hv_vnet.h).
 *
 * Device side of the virtio 1.2 spec (OASIS): 4.1 virtio over PCI (vendor capabilities,
 * common/ISR/device/notify structures, INTx and the ISR status), 2.7 split virtqueues,
 * 5.1 network device (virtio_net_hdr_v1, VIRTIO_NET_F_MAC / _STATUS). The guest driver is
 * Red Hat's NetKVM (virtio-win, kvm-guest-drivers-windows NetKVM + VirtIO/VirtIOPCIModern.c):
 * no MSI-X capability -> CM_RESOURCE_INTERRUPT_MESSAGE not set -> its line-based ISR reads
 * the ISR status byte and runs the DPC, which also re-reads the link state.
 *
 * Offered features: VERSION_1, MAC (the Wi-Fi MAC: a station may only send from its own
 * address), STATUS (link = associated and keyed). No offloads, no CTRL_VQ, no MRG_RXBUF, no
 * EVENT_IDX, no indirect descriptors: every frame is a plain 12-byte header + 802.3 frame.
 *
 * Locking: BAR traps and config accesses run under the hypervisor lock (bhl) through
 * hv_exc_entry, and so do hv_tick and hv_exc_exit, so the device state needs no lock of its
 * own. The guest writes the rings concurrently from any vCPU: avail idx is read with acquire,
 * the used ring is published with release ordering. INTx is injected only on the boot CPU
 * (the vCPU the SPI is delivered to), like hv_nvme.c; other CPUs send it an IPI.
 */
#include "hv_vnet.h"

#ifdef VNET_HOST_TEST
#include "vnet_host_test.h"
#else
#include "brcm_msgbuf.h"
#include "hv.h"
#include "hv_irq_routes.h"
#include "smp.h"
#include "string.h"
#include "utils.h" /* soc.h -> config.h: ENABLE_VGIC_MODULE, before hv_vgic.h */
#include "hv_vgic.h"
u64 hv_ipa_to_pa(u64 ipa);
#endif

/* PCI identity: virtio 1.x network device as QEMU presents it (NetKVM's INF matches
   PCI\VEN_1AF4&DEV_1041, with SUBSYS_11001AF4&REV_01 the most specific id) */
#define VNET_VENDOR      0x1af4
#define VNET_DEVICE      0x1041
#define VNET_SUBSYS      0x11001af4
#define VNET_CLASS       0x020000 /* network, ethernet */
#define VNET_REVISION    0x01
#define VNET_BAR_SIZE    0x4000 /* one 16 KiB stage-2 page */
#define VNET_BAR_TYPE    0x4    /* 64-bit, non-prefetchable memory */
#define CMD_MEM_SPACE    BIT(1)
#define CAP_LIST         BIT(4) /* status register */

/* BAR layout */
#define OFF_COMMON  0x0000
#define LEN_COMMON  0x38
#define OFF_ISR     0x1000
#define LEN_ISR     0x4
#define OFF_DEVCFG  0x2000
#define LEN_DEVCFG  0x10
#define OFF_NOTIFY  0x3000
#define LEN_NOTIFY  0x8
#define NOTIFY_MULT 4

/* virtio_pci_cap cfg_type */
#define CAP_COMMON 1
#define CAP_NOTIFY 2
#define CAP_ISR    3
#define CAP_DEVICE 4

/* features */
#define F_MAC       BIT(5)
#define F_STATUS    BIT(16)
#define F_VERSION_1 (1ULL << 32)
#define VNET_FEATURES (F_MAC | F_STATUS | F_VERSION_1)

/* device status */
#define S_ACKNOWLEDGE BIT(0)
#define S_DRIVER      BIT(1)
#define S_DRIVER_OK   BIT(2)
#define S_FEATURES_OK BIT(3)
#define S_NEEDS_RESET BIT(6)
#define S_FAILED      BIT(7)

#define ISR_QUEUE  BIT(0)
#define ISR_CONFIG BIT(1)
#define NO_VECTOR  0xffff
#define LINK_UP    1 /* VIRTIO_NET_S_LINK_UP */

/* split virtqueue */
#define DESC_NEXT     1
#define DESC_WRITE    2
#define DESC_INDIRECT 4
#define AVAIL_NO_INTERRUPT 1
#define QMAX 256
#define NQ   2 /* 0 = receiveq1, 1 = transmitq1 */
#define RXQ  0
#define TXQ  1
#define HDR_LEN   12 /* struct virtio_net_hdr_v1 */
#define FRAME_MAX 2048

struct vq {
    u16 size;
    bool enabled;
    u64 desc, avail, used; /* guest physical (IPA) */
    u16 last_avail, used_idx;
};

static struct {
    bool present;
    u8 mac[6];
    /* PCI */
    u16 command;
    u32 bar_lo, bar_hi;
    u8 intr_line;
    u64 bar_base; /* 0 = BAR trap not armed */
    /* virtio */
    u32 dev_feat_sel, drv_feat_sel;
    u64 drv_features;
    u8 status;
    u8 generation;
    u16 qsel;
    struct vq q[NQ];
    u16 link;
    /* INTx */
    u32 isr;              /* atomic */
    bool irq_outstanding; /* atomic: injected, not EOId yet */
    /* stats */
    u64 rx, rx_bytes, rx_nobuf, rx_notready, rx_bad, tx, tx_bytes, tx_dropped, tx_bad, tx_busy, irqs;
    bool tx_pending; /* TX entries left in the ring (chip busy): retry from hv_vnet_poll */
    u64 last_log;
} vn;

static u8 frame_buf[FRAME_MAX + HDR_LEN];

static u32 vnet_vintid(void)
{
    return HV_IRQ_SYNTHETIC_VNET_VINTID;
}

/* ---------- guest memory ---------- */

/* host pointer for [ipa, ipa+len) when it is guest RAM inside one 4 KiB page, else NULL */
static void *gptr(u64 ipa, u32 len)
{
    if (!len || ((ipa & 0xfff) + len) > 0x1000)
        return NULL;
    u64 pa = hv_ipa_to_pa(ipa);
    return pa ? (void *)(uintptr_t)pa : NULL;
}

static bool gcopy_from(void *dst, u64 ipa, u32 len)
{
    u8 *d = dst;
    while (len) {
        u32 n = min(len, 0x1000 - (u32)(ipa & 0xfff));
        void *s = gptr(ipa, n);
        if (!s)
            return false;
        memcpy(d, s, n);
        d += n, ipa += n, len -= n;
    }
    return true;
}

static bool gcopy_to(u64 ipa, const void *src, u32 len)
{
    const u8 *s = src;
    while (len) {
        u32 n = min(len, 0x1000 - (u32)(ipa & 0xfff));
        void *d = gptr(ipa, n);
        if (!d)
            return false;
        memcpy(d, s, n);
        s += n, ipa += n, len -= n;
    }
    return true;
}

struct vdesc {
    u64 addr;
    u32 len;
    u16 flags, next;
};

static bool read_desc(const struct vq *q, u16 i, struct vdesc *d)
{
    return i < q->size && gcopy_from(d, q->desc + 16ULL * i, sizeof(*d));
}

static u16 *avail_idx_ptr(const struct vq *q)
{
    return gptr(q->avail + 2, 2);
}

/* ---------- interrupts ---------- */

/* Inject INTx when the ISR is set, nothing is outstanding and Windows enabled the SPI. */
void hv_vnet_poll_irq(void)
{
    if (!vn.present || smp_id() != boot_cpu_idx)
        return;
    if (!__atomic_load_n(&vn.isr, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&vn.irq_outstanding, __ATOMIC_ACQUIRE))
        return;
    u32 irq = vnet_vintid();
    if (!hv_vgic3_irq_enabled(irq) || hv_vgic3_get_free_lr() < 0)
        return;
    __atomic_store_n(&vn.irq_outstanding, true, __ATOMIC_RELEASE);
    vn.irqs++;
    hv_vgic3_inject_irq(irq, hv_vgic3_get_priority(irq), false, true, false, 0);
}

void hv_vnet_irq_eoi(u32 intid)
{
    if (intid != vnet_vintid())
        return;
    /* re-injected (if the ISR is set again) by hv_vnet_poll_irq on the way out */
    __atomic_store_n(&vn.irq_outstanding, false, __ATOMIC_RELEASE);
}

static void raise(u32 bits)
{
    __atomic_or_fetch(&vn.isr, bits, __ATOMIC_ACQ_REL);
    if (smp_id() == boot_cpu_idx)
        hv_vnet_poll_irq();
    else if (!__atomic_load_n(&vn.irq_outstanding, __ATOMIC_ACQUIRE))
        smp_send_ipi(boot_cpu_idx); /* its exception exit runs hv_vnet_poll_irq */
}

/* after adding used entries: interrupt unless the driver suppressed it */
static void queue_interrupt(const struct vq *q)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    u16 *flags = gptr(q->avail, 2);
    if (flags && (__atomic_load_n(flags, __ATOMIC_ACQUIRE) & AVAIL_NO_INTERRUPT))
        return;
    raise(ISR_QUEUE);
}

static void push_used(struct vq *q, u16 head, u32 len)
{
    u64 slot = q->used + 4 + 8ULL * (q->used_idx % q->size);
    u32 *e = gptr(slot, 8);
    if (e) {
        e[0] = head;
        e[1] = len;
    }
    q->used_idx++;
    u16 *idx = gptr(q->used + 2, 2);
    if (idx)
        __atomic_store_n(idx, q->used_idx, __ATOMIC_RELEASE);
}

static bool queue_live(const struct vq *q)
{
    return (vn.status & S_DRIVER_OK) && !(vn.status & S_NEEDS_RESET) && q->enabled && q->size &&
           q->desc && q->avail && q->used;
}

static void needs_reset(const char *why)
{
    printf("vnet: device error (%s): NEEDS_RESET\n", why);
    vn.status |= S_NEEDS_RESET;
    vn.generation++;
    raise(ISR_CONFIG);
}

/* ---------- RX: Wi-Fi -> guest ---------- */

void hv_vnet_rx(const u8 *frame, u32 len)
{
    if (!vn.present)
        return;
    struct vq *q = &vn.q[RXQ];
    if (!queue_live(q)) {
        vn.rx_notready++;
        return;
    }
    if (len < 14 || len > FRAME_MAX) {
        vn.rx_bad++;
        return;
    }
    u16 *aidx = avail_idx_ptr(q);
    if (!aidx) {
        needs_reset("RX avail ring not in guest RAM");
        return;
    }
    u16 avail = __atomic_load_n(aidx, __ATOMIC_ACQUIRE);
    if (avail == q->last_avail) {
        vn.rx_nobuf++;
        return;
    }
    u16 *ring = gptr(q->avail + 4 + 2ULL * (q->last_avail % q->size), 2);
    if (!ring) {
        needs_reset("RX avail entry");
        return;
    }
    u16 head = *ring;

    /* virtio_net_hdr_v1: no checksum/GSO information, num_buffers = 1 */
    u8 hdr[HDR_LEN] = {0};
    hdr[10] = 1;

    u32 done = 0, total = HDR_LEN + len;
    u16 i = head;
    for (u32 n = 0; done < total; n++) {
        struct vdesc d;
        if (n >= q->size || !read_desc(q, i, &d) || !(d.flags & DESC_WRITE) ||
            (d.flags & DESC_INDIRECT)) {
            needs_reset("RX descriptor chain");
            return;
        }
        u32 take = min(d.len, total - done);
        for (u32 k = 0; k < take;) { /* header bytes first, then the frame */
            u32 pos = done + k;
            const u8 *src = pos < HDR_LEN ? hdr + pos : frame + (pos - HDR_LEN);
            u32 n2 = pos < HDR_LEN ? min(take - k, HDR_LEN - pos) : take - k;
            if (!gcopy_to(d.addr + k, src, n2)) {
                needs_reset("RX buffer not in guest RAM");
                return;
            }
            k += n2;
        }
        done += take;
        if (done < total) {
            if (!(d.flags & DESC_NEXT)) {
                vn.rx_bad++; /* buffer too small: drop the frame, keep the buffer */
                return;
            }
            i = d.next;
        }
    }
    q->last_avail++;
    push_used(q, head, total);
    vn.rx++;
    vn.rx_bytes += len;
    queue_interrupt(q);
}

/* ---------- TX: guest -> Wi-Fi ---------- */

static void process_tx(void)
{
    struct vq *q = &vn.q[TXQ];
    if (!queue_live(q))
        return;
    u16 *aidx = avail_idx_ptr(q);
    if (!aidx) {
        needs_reset("TX avail ring not in guest RAM");
        return;
    }
    bool used = false;
    for (u32 guard = 0; guard < q->size; guard++) {
        u16 avail = __atomic_load_n(aidx, __ATOMIC_ACQUIRE);
        if (avail == q->last_avail)
            break;
        u16 *ring = gptr(q->avail + 4 + 2ULL * (q->last_avail % q->size), 2);
        if (!ring) {
            needs_reset("TX avail entry");
            return;
        }
        u16 head = *ring, i = head;
        u32 len = 0;
        bool ok = true;
        for (u32 n = 0;; n++) {
            struct vdesc d;
            if (n >= q->size || !read_desc(q, i, &d) || (d.flags & (DESC_WRITE | DESC_INDIRECT))) {
                needs_reset("TX descriptor chain");
                return;
            }
            if (len + d.len > sizeof(frame_buf))
                ok = false; /* oversized: drop, but walk the chain to its end */
            else if (d.len && !gcopy_from(frame_buf + len, d.addr, d.len))
                ok = false;
            else
                len += d.len;
            if (!(d.flags & DESC_NEXT))
                break;
            i = d.next;
        }
        int r = -1;
        if (!ok || len < HDR_LEN + 14) {
            vn.tx_bad++;
        } else if (!(vn.link & LINK_UP)) {
            vn.tx_dropped++;
        } else if ((r = brcm_tx(frame_buf + HDR_LEN, len - HDR_LEN)) == BRCM_TX_BUSY) {
            /* the chip has no room: leave the frame in the guest's ring (flow control),
               hv_vnet_poll retries every tick */
            vn.tx_pending = true;
            vn.tx_busy++;
            break;
        } else if (r) {
            vn.tx_dropped++;
        } else {
            vn.tx++;
            vn.tx_bytes += len - HDR_LEN;
        }
        q->last_avail++;
        push_used(q, head, 0);
        used = true;
        if (guard + 1 == q->size)
            vn.tx_pending = true; /* more than a ring's worth: continue next tick */
    }
    if (used)
        queue_interrupt(q);
}

/* ---------- link ---------- */

void hv_vnet_set_link(bool up)
{
    u16 link = up ? LINK_UP : 0;
    if (!vn.present || link == vn.link)
        return;
    vn.link = link;
    vn.generation++;
    printf("vnet: link %s for the guest\n", up ? "up" : "down");
    if (vn.status & S_DRIVER_OK)
        raise(ISR_CONFIG);
}

/* ---------- virtio reset ---------- */

static void vq_reset(struct vq *q)
{
    memset(q, 0, sizeof(*q));
    q->size = QMAX;
}

static void device_reset(void)
{
    vn.dev_feat_sel = vn.drv_feat_sel = 0;
    vn.drv_features = 0;
    vn.status = 0;
    vn.qsel = 0;
    for (int i = 0; i < NQ; i++)
        vq_reset(&vn.q[i]);
    __atomic_store_n(&vn.isr, 0, __ATOMIC_RELEASE);
}

/* ---------- BAR: common / ISR / device / notify ---------- */

static u64 common_read(u32 off, u32 bytes)
{
    struct vq *q = vn.qsel < NQ ? &vn.q[vn.qsel] : NULL;
    u64 v;
    switch (off) {
        case 0x00: v = vn.dev_feat_sel; break;
        case 0x04: v = vn.dev_feat_sel < 2 ? (u32)(VNET_FEATURES >> (32 * vn.dev_feat_sel)) : 0; break;
        case 0x08: v = vn.drv_feat_sel; break;
        case 0x0c: v = vn.drv_feat_sel < 2 ? (u32)(vn.drv_features >> (32 * vn.drv_feat_sel)) : 0; break;
        case 0x10: v = NO_VECTOR; break; /* msix_config: no MSI-X */
        case 0x12: v = NQ; break;
        case 0x14: v = vn.status | (bytes > 1 ? (u32)vn.generation << 8 : 0); break;
        case 0x15: v = vn.generation; break;
        case 0x16: v = vn.qsel; break;
        case 0x18: v = q ? q->size : 0; break;
        case 0x1a: v = NO_VECTOR; break;
        case 0x1c: v = q ? q->enabled : 0; break;
        case 0x1e: v = q ? vn.qsel : 0; break; /* queue_notify_off */
        case 0x20: v = q ? q->desc : 0; break;
        case 0x24: v = q ? q->desc >> 32 : 0; break;
        case 0x28: v = q ? q->avail : 0; break;
        case 0x2c: v = q ? q->avail >> 32 : 0; break;
        case 0x30: v = q ? q->used : 0; break;
        case 0x34: v = q ? q->used >> 32 : 0; break;
        default: v = 0;
    }
    return bytes >= 8 ? v : v & ((1ULL << (8 * bytes)) - 1);
}

static void set_addr(u64 *field, u32 off, u64 val, u32 bytes)
{
    if (bytes == 8)
        *field = val;
    else if (off & 4)
        *field = (*field & 0xffffffffULL) | ((u64)(u32)val << 32);
    else
        *field = (*field & ~0xffffffffULL) | (u32)val;
}

static void write_status(u8 val)
{
    if (!val) {
        device_reset();
        return;
    }
    if ((val & S_FEATURES_OK) && !(vn.status & S_FEATURES_OK)) {
        /* 3.1.1: accept only a subset of what we offered, and VERSION_1 (non-transitional) */
        if ((vn.drv_features & ~VNET_FEATURES) || !(vn.drv_features & F_VERSION_1)) {
            printf("vnet: driver features 0x%lx refused (offered 0x%lx)\n",
                   (unsigned long)vn.drv_features, (unsigned long)VNET_FEATURES);
            val &= ~S_FEATURES_OK;
        }
    }
    if ((val & S_DRIVER_OK) && !(vn.status & S_DRIVER_OK))
        printf("vnet: guest driver up (features 0x%lx, rx queue %u, tx queue %u entries)\n",
               (unsigned long)vn.drv_features, vn.q[RXQ].enabled ? vn.q[RXQ].size : 0,
               vn.q[TXQ].enabled ? vn.q[TXQ].size : 0);
    vn.status = val;
}

static void common_write(u32 off, u64 val, u32 bytes)
{
    struct vq *q = vn.qsel < NQ ? &vn.q[vn.qsel] : NULL;
    switch (off) {
        case 0x00: vn.dev_feat_sel = val; return;
        case 0x08: vn.drv_feat_sel = val; return;
        case 0x0c:
            if (vn.status & S_FEATURES_OK)
                return; /* frozen once accepted */
            if (vn.drv_feat_sel == 0)
                vn.drv_features = (vn.drv_features & ~0xffffffffULL) | (u32)val;
            else if (vn.drv_feat_sel == 1)
                vn.drv_features = (vn.drv_features & 0xffffffffULL) | ((u64)(u32)val << 32);
            return;
        case 0x10: return; /* msix_config: stays NO_VECTOR */
        case 0x14: write_status(val); return;
        case 0x16: vn.qsel = val; return;
        case 0x1a: return; /* queue_msix_vector */
    }
    if (!q || q->enabled)
        return; /* queue fields are read-only once the queue is enabled */
    switch (off) {
        case 0x18:
            /* power of two, at most what we offered */
            if (val && val <= QMAX && !(val & (val - 1)))
                q->size = val;
            return;
        case 0x1c:
            if (val & 1) {
                q->enabled = true;
                q->last_avail = q->used_idx = 0;
            }
            return;
        case 0x20: case 0x24: set_addr(&q->desc, off, val, bytes); return;
        case 0x28: case 0x2c: set_addr(&q->avail, off, val, bytes); return;
        case 0x30: case 0x34: set_addr(&q->used, off, val, bytes); return;
    }
}

static u64 devcfg_read(u32 off, u32 bytes)
{
    u8 cfg[LEN_DEVCFG] = {0};
    memcpy(cfg, vn.mac, 6);
    cfg[6] = vn.link; /* status */
    cfg[8] = 1;       /* max_virtqueue_pairs */
    u64 v = 0;
    for (u32 i = 0; i < bytes && off + i < LEN_DEVCFG; i++)
        v |= (u64)cfg[off + i] << (8 * i);
    return v;
}

static bool handle_vnet_bar(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    u32 off = addr - vn.bar_base;
    u32 bytes = 1u << width;
    if (off < OFF_COMMON + LEN_COMMON) { /* OFF_COMMON is 0 */
        if (write)
            common_write(off - OFF_COMMON, *val, bytes);
        else
            *val = common_read(off - OFF_COMMON, bytes);
    } else if (off >= OFF_ISR && off < OFF_ISR + LEN_ISR) {
        if (write)
            return true;
        /* reading the ISR status acknowledges it (4.1.4.5) */
        *val = off == OFF_ISR ? __atomic_exchange_n(&vn.isr, 0, __ATOMIC_ACQ_REL) : 0;
    } else if (off >= OFF_DEVCFG && off < OFF_DEVCFG + LEN_DEVCFG) {
        if (!write)
            *val = devcfg_read(off - OFF_DEVCFG, bytes);
    } else if (off >= OFF_NOTIFY && off < OFF_NOTIFY + LEN_NOTIFY) {
        if (!write) {
            *val = 0;
        } else {
            u32 qi = (off - OFF_NOTIFY) / NOTIFY_MULT;
            if (qi == TXQ)
                process_tx();
            /* RX notify: new buffers; the next received frame uses them */
        }
    } else if (!write) {
        *val = 0;
    }
    return true;
}

/* ---------- PCI config space of 00:01.0 ---------- */

static u64 bar_addr(void)
{
    return ((u64)vn.bar_hi << 32) | (vn.bar_lo & ~(u32)(VNET_BAR_SIZE - 1));
}

static void bar_reeval(void)
{
    u64 addr = bar_addr();
    bool sizing = vn.bar_hi == 0xffffffff ||
                  (vn.bar_lo & ~(u32)(VNET_BAR_SIZE - 1)) == ~(u32)(VNET_BAR_SIZE - 1);
    bool want = (vn.command & CMD_MEM_SPACE) && addr && !sizing;
    if (want && vn.bar_base != addr) {
        if (vn.bar_base)
            hv_unmap(vn.bar_base, VNET_BAR_SIZE);
        vn.bar_base = addr;
        hv_map_hook(addr, handle_vnet_bar, VNET_BAR_SIZE);
        printf("vnet: BAR0 at 0x%lx (virtio-net 00:01.0, INTA -> vINTID %u)\n",
               (unsigned long)addr, vnet_vintid());
    } else if (!want && vn.bar_base) {
        hv_unmap(vn.bar_base, VNET_BAR_SIZE);
        vn.bar_base = 0;
    }
}

/* virtio_pci_cap (16 bytes) and virtio_pci_notify_cap (20) as config dwords */
static u32 cap_dword(u32 reg)
{
    static const struct {
        u8 at, next, len, type;
        u32 offset, length;
    } caps[] = {
        {0x40, 0x50, 16, CAP_COMMON, OFF_COMMON, LEN_COMMON},
        {0x50, 0x60, 16, CAP_ISR, OFF_ISR, LEN_ISR},
        {0x60, 0x70, 16, CAP_DEVICE, OFF_DEVCFG, LEN_DEVCFG},
        {0x70, 0x00, 20, CAP_NOTIFY, OFF_NOTIFY, LEN_NOTIFY},
    };
    for (u32 i = 0; i < ARRAY_SIZE(caps); i++) {
        if (reg < caps[i].at || reg >= caps[i].at + caps[i].len)
            continue;
        switch (reg - caps[i].at) {
            case 0x0: /* cap_vndr 0x09, cap_next, cap_len, cfg_type */
                return 0x09 | (caps[i].next << 8) | (caps[i].len << 16) | (caps[i].type << 24);
            case 0x4: return 0; /* bar 0, id 0, padding */
            case 0x8: return caps[i].offset;
            case 0xc: return caps[i].length;
            case 0x10: return NOTIFY_MULT; /* notify_off_multiplier */
        }
    }
    return 0;
}

u32 hv_vnet_cfg_read(u32 reg)
{
    switch (reg) {
        case 0x00: return (VNET_DEVICE << 16) | VNET_VENDOR;
        case 0x04: return ((u32)CAP_LIST << 16) | vn.command;
        case 0x08: return (VNET_CLASS << 8) | VNET_REVISION;
        case 0x0c: return 0; /* header type 0, single function */
        case 0x10: return (vn.bar_lo & ~(u32)(VNET_BAR_SIZE - 1)) | VNET_BAR_TYPE;
        case 0x14: return vn.bar_hi;
        case 0x2c: return VNET_SUBSYS;
        case 0x34: return 0x40; /* capabilities */
        case 0x3c: return (0x01 << 8) | vn.intr_line; /* INTA */
    }
    return reg >= 0x40 && reg < 0x84 ? cap_dword(reg) : 0;
}

void hv_vnet_cfg_write(u32 reg, u32 boff, u32 bytes, u64 data)
{
    u32 cur = hv_vnet_cfg_read(reg);
    u32 mask = bytes >= 4 ? 0xffffffffu : (((1u << (bytes * 8)) - 1) << (boff * 8));
    u32 v = (cur & ~mask) | ((u32)(data << (boff * 8)) & mask);
    switch (reg) {
        case 0x04:
            vn.command = v & 0xffff;
            bar_reeval();
            break;
        case 0x10:
            vn.bar_lo = v & 0xfffffff0;
            bar_reeval();
            break;
        case 0x14:
            vn.bar_hi = v;
            bar_reeval();
            break;
        case 0x3c:
            vn.intr_line = v & 0xff;
            break;
    }
}

/* ---------- life cycle ---------- */

bool hv_vnet_present(void)
{
    return vn.present;
}

void hv_vnet_init(const u8 mac[6])
{
    if (vn.bar_base)
        hv_unmap(vn.bar_base, VNET_BAR_SIZE);
    memset(&vn, 0, sizeof(vn));
    memcpy(vn.mac, mac, 6);
    device_reset();
    vn.intr_line = 0xff;
    vn.last_log = get_ticks();
    vn.present = true;
    printf("vnet: virtio-net 00:01.0 for the guest, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void hv_vnet_poll(void)
{
    if (!vn.present)
        return;
    if (vn.tx_pending) {
        vn.tx_pending = false;
        process_tx();
    }
    hv_vnet_poll_irq();
    if (ticks_to_msecs(get_ticks() - vn.last_log) < 60000)
        return;
    vn.last_log = get_ticks();
    printf("vnet: guest %s, link %s, rx %lu (%lu bytes; dropped: %lu no buffer, %lu not ready, "
           "%lu bad), tx %lu (%lu bytes; %lu dropped, %lu bad, %lu waits for the chip), "
           "%lu interrupts\n",
           (vn.status & S_DRIVER_OK) ? "driver up" : "no driver", (vn.link & LINK_UP) ? "up" : "down",
           (unsigned long)vn.rx, (unsigned long)vn.rx_bytes, (unsigned long)vn.rx_nobuf,
           (unsigned long)vn.rx_notready, (unsigned long)vn.rx_bad, (unsigned long)vn.tx,
           (unsigned long)vn.tx_bytes, (unsigned long)vn.tx_dropped, (unsigned long)vn.tx_bad,
           (unsigned long)vn.tx_busy, (unsigned long)vn.irqs);
}
