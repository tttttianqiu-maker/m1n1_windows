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
