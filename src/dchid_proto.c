/* SPDX-License-Identifier: MIT */
/* See dchid_proto.h for provenance (Asahi Linux dockchannel-hid, MIT). */

#include "dchid_proto.h"

#ifdef DCHID_HOST_TEST
#include <string.h>
#else
#include "string.h"
#endif

static u32 get_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void put_le32(u8 *p, u32 v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

u32 dchid_sum(const void *buf, size_t size)
{
    const u8 *p = buf;
    u32 sum = 0;

    for (; size >= 4; size -= 4, p += 4)
        sum += get_le32(p);
    return sum;
}

bool dchid_packet_valid(const struct dchid_hdr *hdr, const u8 *body)
{
    if (!hdr || !body || hdr->hdr_len != sizeof(*hdr) || (hdr->length & 3))
        return false;
    return dchid_sum(hdr, sizeof(*hdr)) + dchid_sum(body, (size_t)hdr->length + 4) == 0xffffffff;
}

size_t dchid_build(u8 *out, size_t cap, u8 channel, u8 iface, u8 seq, u8 flags, const void *msg,
                   size_t len)
{
    struct dchid_hdr hdr;
    struct dchid_subhdr sub;
    size_t padded = (len + 3) & ~(size_t)3;
    size_t total = sizeof(hdr) + sizeof(sub) + padded + 4;
    u32 checksum;

    if (!out || total > cap || len > 0xffff - sizeof(sub) - 3)
        return 0;

    memset(&hdr, 0, sizeof(hdr));
    memset(&sub, 0, sizeof(sub));
    hdr.hdr_len = sizeof(hdr);
    hdr.channel = channel;
    hdr.length = (u16)(padded + sizeof(sub));
    hdr.seq = seq;
    hdr.iface = iface;
    sub.flags = flags;
    sub.length = (u16)len;

    memset(out, 0, total);
    memcpy(out, &hdr, sizeof(hdr));
    memcpy(out + sizeof(hdr), &sub, sizeof(sub));
    if (len)
        memcpy(out + sizeof(hdr) + sizeof(sub), msg, len);
    checksum = 0xffffffff - dchid_sum(out, total - 4);
    put_le32(out + total - 4, checksum);
    return total;
}

bool dchid_parse_init(const u8 *payload, size_t len, struct dchid_init_info *out)
{
    const struct dchid_init_hdr *hdr = (const void *)payload;
    const u8 *p;

    if (!payload || !out || len < sizeof(*hdr) || payload[0] != DCHID_EVENT_INIT)
        return false;

    memset(out, 0, sizeof(*out));
    out->iface = hdr->iface;
    memcpy(out->name, hdr->name, DCHID_NAME_LEN);
    out->name[DCHID_NAME_LEN] = 0;
    out->more_packets = hdr->more_packets != 0;

    p = payload + sizeof(*hdr);
    len -= sizeof(*hdr);
    while (len >= sizeof(struct dchid_init_block_hdr)) {
        struct dchid_init_block_hdr blk;

        memcpy(&blk, p, sizeof(blk));
        p += sizeof(blk);
        len -= sizeof(blk);
        if (blk.length > len)
            break;

        switch (blk.type) {
            case DCHID_INIT_HID_DESCRIPTOR:
                out->descriptor = p;
                out->descriptor_len = blk.length;
                break;
            case DCHID_INIT_GPIO_REQUEST:
                if (blk.length >= sizeof(struct dchid_gpio_request)) {
                    const struct dchid_gpio_request *req = (const void *)p;
                    out->gpio_requested = true;
                    out->gpio_id = req->id;
                    memcpy(out->gpio_name, req->name, DCHID_GPIO_NAME);
                    out->gpio_name[DCHID_GPIO_NAME] = 0;
                }
                break;
            default:
                break;
        }

        p += blk.length;
        len -= blk.length;
        if (blk.type == DCHID_INIT_TERMINATOR)
            break;
    }
    return true;
}

bool dchid_fw_parse(const u8 *blob, size_t size, const u8 **data, u32 *data_len,
                    u32 *iface_offset)
{
    struct dchid_fw_header hdr;

    if (!blob || size < sizeof(hdr))
        return false;
    memcpy(&hdr, blob, sizeof(hdr));
    if (hdr.magic != DCHID_FW_MAGIC || hdr.version != DCHID_FW_VER ||
        hdr.hdr_length < sizeof(hdr) || hdr.hdr_length > size ||
        (size_t)hdr.hdr_length + hdr.data_length > size || hdr.iface_offset >= hdr.data_length)
        return false;
    if (data)
        *data = blob + hdr.hdr_length;
    if (data_len)
        *data_len = hdr.data_length;
    if (iface_offset)
        *iface_offset = hdr.iface_offset;
    return true;
}

size_t dchid_power_cmd(u8 *out, u8 method, u8 iface, u8 state, bool has_changed)
{
    if (!out)
        return 0;
    if (method == DCHID_POWER_METHOD_2) {
        memset(out, 0, 9);
        out[0] = DCHID_CMD_RESET_INTERFACE;
        out[1] = DCHID_POWER_METHOD_2;
        out[2] = iface;
        out[3] = state;
        out[4] = has_changed;
        return 9;
    }
    out[0] = DCHID_CMD_RESET_INTERFACE;
    out[1] = DCHID_POWER_METHOD_1;
    out[2] = iface;
    out[3] = state;
    return 4;
}

unsigned int dchid_fixup_report_size(u8 *desc, size_t len)
{
    unsigned int fixed = 0;

    if (!desc || len < 5)
        return 0;
    for (size_t i = 0; i + 5 <= len; i++) {
        u8 count = desc[i + 4];
        if (desc[i] == 0x76 && desc[i + 1] == 0x00 && desc[i + 2] == 0x40 &&
            desc[i + 3] == 0x95 && count > 0 && count < 32) {
            desc[i] = 0x75;
            desc[i + 1] = 0x08;
            desc[i + 2] = 0x96;
            desc[i + 3] = 0x00;
            desc[i + 4] = (u8)(count * 8);
            fixed++;
        }
    }
    return fixed;
}

const u8 dchid_guest_kbd_desc[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xa1, 0x01,       /* Collection (Application) */
    0x85, DCHID_KBD_REPORT_ID,
    0x05, 0x07,       /*   Usage Page (Keyboard/Keypad) */
    0x19, 0xe0,       /*   Usage Minimum (Left Control) */
    0x29, 0xe7,       /*   Usage Maximum (Right GUI) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x08,       /*   Report Count (8) */
    0x81, 0x02,       /*   Input (Data, Var, Abs): modifiers */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x01,       /*   Input (Const): reserved */
    0x19, 0x00,       /*   Usage Minimum (0) */
    0x2a, 0xff, 0x00, /*   Usage Maximum (255) */
    0x26, 0xff, 0x00, /*   Logical Maximum (255) */
    0x95, 0x06,       /*   Report Count (6) */
    0x81, 0x00,       /*   Input (Data, Array): key codes */
    0x06, 0x00, 0xff, /*   Usage Page (Vendor 0xff00) */
    0x09, 0x03,       /*   Usage (0x03): Apple Fn byte */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x02,       /*   Input (Data, Var, Abs) */
    0xc0,             /* End Collection */
    0x05, 0x0c,       /* Usage Page (Consumer) */
    0x09, 0x01,       /* Usage (Consumer Control) */
    0xa1, 0x01,       /* Collection (Application) */
    0x85, DCHID_CONSUMER_REPORT_ID,
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xff, 0x03, /*   Logical Maximum (0x3ff) */
    0x19, 0x00,       /*   Usage Minimum (0) */
    0x2a, 0xff, 0x03, /*   Usage Maximum (0x3ff) */
    0x75, 0x10,       /*   Report Size (16) */
    0x95, 0x02,       /*   Report Count (2) */
    0x81, 0x00,       /*   Input (Data, Array, Abs): media keys */
    0xc0,             /* End Collection */
};
const size_t dchid_guest_kbd_desc_len = sizeof(dchid_guest_kbd_desc);

void dchid_kbd_iso_swap(u8 report[DCHID_KBD_REPORT_LEN])
{
    for (int b = 3; b < 9; b++) {
        if (report[b] == 0x35)
            report[b] = 0x64;
        else if (report[b] == 0x64)
            report[b] = 0x35;
    }
}

int dchid_mt_sanitize(const u8 *in, size_t len, u8 *out, size_t cap, size_t *out_len)
{
    size_t nf, need;

    if (!in || !out || !out_len || len < DCHID_MT_HDR_SIZE)
        return -1;
    nf = in[DCHID_MT_NFINGERS_OFFSET];
    if (nf > DCHID_MT_GUEST_FINGERS)
        nf = DCHID_MT_GUEST_FINGERS;
    need = DCHID_MT_HDR_SIZE + nf * DCHID_MT_FINGER_SIZE;
    if (len < need || need > cap)
        return -1;
    memcpy(out, in, need);
    out[DCHID_MT_NFINGERS_OFFSET] = (u8)nf;
    out[DCHID_MT_BUTTONS_OFFSET] &= 1;
    *out_len = need;
    return len != need || nf != in[DCHID_MT_NFINGERS_OFFSET] ? 1 : 0;
}

void dchid_vq_push(struct dchid_vq *q, u8 iface, const u8 *data, size_t len)
{
    struct dchid_vq_ring *r = &q->ring[iface ? 1 : 0];

    if (!r->slot || !r->cap || !len)
        return;
    if (len > DCHID_VQ_SLOT_SIZE)
        len = DCHID_VQ_SLOT_SIZE;
    if (r->count == r->cap) { /* full-state reports: keep the newest */
        r->head = (r->head + 1) % r->cap;
        r->count--;
        q->count--;
        q->drops++;
    }
    struct dchid_vq_slot *sl = &r->slot[(r->head + r->count) % r->cap];
    sl->seq = ++q->seq;
    sl->len = (u16)len;
    sl->iface = iface ? 1 : 0;
    memcpy(sl->data, data, len);
    r->count++;
    q->count++;
}

const struct dchid_vq_slot *dchid_vq_latch(struct dchid_vq *q)
{
    struct dchid_vq_ring *best = NULL;

    if (q->cur_valid)
        return &q->cur;
    for (int i = 0; i < 2; i++) {
        struct dchid_vq_ring *r = &q->ring[i];
        /* seq comparison by difference: wrap-safe */
        if (r->count && (!best || (s32)(r->slot[r->head].seq - best->slot[best->head].seq) < 0))
            best = r;
    }
    if (!best)
        return NULL;
    const struct dchid_vq_slot *sl = &best->slot[best->head];
    q->cur.seq = sl->seq;
    q->cur.len = sl->len;
    q->cur.iface = sl->iface;
    memcpy(q->cur.data, sl->data, sl->len);
    q->cur_valid = true;
    return &q->cur;
}

void dchid_vq_pop(struct dchid_vq *q)
{
    if (!q->cur_valid)
        return;
    struct dchid_vq_ring *r = &q->ring[q->cur.iface ? 1 : 0];
    /* Unless it was evicted meanwhile, the latched report is its ring's head. */
    if (r->count && r->slot[r->head].seq == q->cur.seq) {
        r->head = (r->head + 1) % r->cap;
        r->count--;
        q->count--;
    }
    q->cur_valid = false;
}

void dchid_vq_flush(struct dchid_vq *q)
{
    for (int i = 0; i < 2; i++)
        q->ring[i].head = q->ring[i].count = 0;
    q->count = 0;
    q->cur_valid = false;
}

static u32 hid_item_value(const u8 *p, unsigned int size)
{
    u32 v = 0;
    for (unsigned int i = 0; i < size; i++)
        v |= (u32)p[i] << (8 * i);
    return v;
}

bool dchid_hid_scan(const u8 *desc, size_t len, u8 report_id, u32 *input_bits, int *fn_bit)
{
    struct {
        u32 page, size, count, id;
    } g = {0, 0, 0, 0}, stack[4];
    unsigned int sp = 0;
    /*
     * Usages of 1-2 bytes take the usage page in effect at the main item
     * (Linux hid_concatenate_last_usage_page); 4-byte usages carry their own.
     */
    u32 usages[16];
    bool short_usage[16];
    unsigned int nusages = 0;
    u32 umin = 0, umax = 0;
    bool umin_short = false, umax_short = false, range = false;
    u64 bits = 0;
    size_t i = 0;

    *input_bits = 0;
    *fn_bit = -1;
    if (!desc)
        return false;
    while (i < len) {
        u8 b = desc[i];
        if (b == 0xfe) { /* long item */
            if (i + 2 >= len)
                return false;
            i += 3 + desc[i + 1];
            continue;
        }
        unsigned int size = (b & 3) == 3 ? 4 : (b & 3);
        unsigned int type = (b >> 2) & 3, tag = b >> 4;
        if (i + 1 + size > len)
            return false;
        u32 v = hid_item_value(desc + i + 1, size);
        i += 1 + size;

        if (type == 1) { /* global */
            switch (tag) {
                case 0: g.page = v; break;
                case 7: g.size = v; break;
                case 8: g.id = v; break;
                case 9: g.count = v; break;
                case 10:
                    if (sp == 4)
                        return false;
                    stack[sp++] = g;
                    break;
                case 11:
                    if (!sp)
                        return false;
                    g = stack[--sp];
                    break;
                default: break;
            }
        } else if (type == 2) { /* local */
            if (tag == 0 && nusages < 16) {
                usages[nusages] = v;
                short_usage[nusages++] = size < 4;
            } else if (tag == 1) {
                umin = v;
                umin_short = size < 4;
                range = true;
            } else if (tag == 2) {
                umax = v;
                umax_short = size < 4;
                range = true;
            }
        } else if (type == 0) { /* main */
            if (tag == 8 && g.id == report_id) { /* Input */
                u64 field = (u64)g.size * g.count;
                if (field > 0x80000)
                    return false;
                if (v & 2) { /* variable: one usage per element */
                    u32 lo = umin_short ? (g.page << 16) | umin : umin;
                    u32 hi = umax_short ? (g.page << 16) | umax : umax;
                    for (u32 e = 0; e < g.count && e < 64; e++) {
                        u32 u;
                        if (e < nusages)
                            u = short_usage[e] ? (g.page << 16) | usages[e] : usages[e];
                        else if (range && lo + e <= hi)
                            u = lo + e;
                        else if (nusages)
                            u = short_usage[nusages - 1] ? (g.page << 16) | usages[nusages - 1]
                                                         : usages[nusages - 1];
                        else
                            u = 0;
                        u32 page = u >> 16, id = u & 0xffff;
                        if (g.size == 1 && id == 0x0003 && *fn_bit < 0 &&
                            (page == 0x00ff || page == 0xff00 || page == 0xff01))
                            *fn_bit = (int)(bits + (u64)e * g.size);
                    }
                }
                bits += field;
            }
            if (tag == 8 || tag == 9 || tag == 10 || tag == 11) {
                nusages = 0;
                range = false;
                umin = umax = 0;
            }
        }
    }
    if (bits > 0x80000)
        return false;
    *input_bits = (u32)bits;
    return true;
}

static bool fn_bit_get(const u8 *map, u8 key)
{
    return map[key >> 3] & (1 << (key & 7));
}

static void fn_bit_set(u8 *map, u8 key, bool on)
{
    if (on)
        map[key >> 3] |= 1 << (key & 7);
    else
        map[key >> 3] &= ~(1 << (key & 7));
}

static u8 fn_layer(u8 key)
{
    switch (key) {
        case 0x2a: return 0x4c; /* Backspace -> Delete */
        case 0x28: return 0x49; /* Enter -> Insert */
        case 0x52: return 0x4b; /* Up -> Page Up */
        case 0x51: return 0x4e; /* Down -> Page Down */
        case 0x50: return 0x4a; /* Left -> Home */
        case 0x4f: return 0x4d; /* Right -> End */
        default: return 0;
    }
}

void dchid_kbd_fn_translate(struct dchid_fn_state *st, u8 report[DCHID_KBD_REPORT_LEN], bool fn)
{
    u8 now[32] = {0};

    for (int b = 3; b < 9; b++) {
        u8 k = report[b];
        if (k < 4) /* 0 none, 1-3 error/rollover codes */
            continue;
        fn_bit_set(now, k, true);
        if (!fn_bit_get(st->down, k)) /* new press: remember the Fn state */
            fn_bit_set(st->with_fn, k, fn);
        u8 to = fn_layer(k);
        if (to && fn_bit_get(st->with_fn, k))
            report[b] = to;
    }
    for (unsigned int i = 0; i < sizeof(now); i++) {
        st->with_fn[i] &= now[i];
        st->down[i] = now[i];
    }
}

static u16 fn_media(u8 key)
{
    switch (key) {
        case 0x3a: return 0x070; /* F1  -> Display Brightness Decrement */
        case 0x3b: return 0x06f; /* F2  -> Display Brightness Increment */
        case 0x3d: return 0x221; /* F4  -> AC Search */
        case 0x40: return 0x0b6; /* F7  -> Scan Previous Track */
        case 0x41: return 0x0cd; /* F8  -> Play/Pause */
        case 0x42: return 0x0b5; /* F9  -> Scan Next Track */
        case 0x43: return 0x0e2; /* F10 -> Mute */
        case 0x44: return 0x0ea; /* F11 -> Volume Decrement */
        case 0x45: return 0x0e9; /* F12 -> Volume Increment */
        default: return 0;
    }
}

bool dchid_kbd_fn_media(struct dchid_fn_state *st, u8 report[DCHID_KBD_REPORT_LEN],
                        u8 out[DCHID_CONSUMER_REPORT_LEN])
{
    u16 cons[2] = {0, 0};
    unsigned int n = 0;
    int w = 3;

    for (int b = 3; b < 9; b++) {
        u8 k = report[b];
        u16 u = k >= 4 ? fn_media(k) : 0;
        if (u && fn_bit_get(st->with_fn, k)) {
            if (n < 2)
                cons[n++] = u;
            continue; /* leaves the keyboard report */
        }
        report[w++] = k;
    }
    while (w < 9)
        report[w++] = 0;

    if (cons[0] == st->consumer[0] && cons[1] == st->consumer[1])
        return false;
    st->consumer[0] = cons[0];
    st->consumer[1] = cons[1];
    out[0] = DCHID_CONSUMER_REPORT_ID;
    out[1] = cons[0] & 0xff;
    out[2] = cons[0] >> 8;
    out[3] = cons[1] & 0xff;
    out[4] = cons[1] >> 8;
    return true;
}
