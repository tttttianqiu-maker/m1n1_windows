/* SPDX-License-Identifier: MIT */
/*
 * Apple DockChannel HID (MTP keyboard/trackpad) wire protocol, hardware-free.
 *
 * Framing, events and commands follow Linux drivers/hid/dockchannel-hid/
 * dockchannel-hid.c and U-Boot drivers/input/apple_mtp_kbd.c (Copyright The
 * Asahi Linux Contributors, GPL-2.0 OR MIT), used here under the MIT licence.
 */
#ifndef DCHID_PROTO_H
#define DCHID_PROTO_H

#ifdef DCHID_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include "types.h"
#endif

#define DCHID_CHANNEL_CMD    0x11
#define DCHID_CHANNEL_REPORT 0x12

#define DCHID_IFACE_COMM   0
#define DCHID_MAX_IFACES   16
#define DCHID_MAX_PKT      (0xffff + 4)
#define DCHID_NAME_LEN     16
#define DCHID_GPIO_NAME    32
#define DCHID_DESC_MAX     1024

/* subheader flags: HID report type in bits 7:6, request in bits 5:0 */
#define DCHID_TYPE_INPUT   0
#define DCHID_TYPE_OUTPUT  1
#define DCHID_TYPE_FEATURE 2
#define DCHID_REQ_SET      0
#define DCHID_REQ_GET      1
#define DCHID_FLAGS(type, req) ((u8)(((type) << 6) | ((req) & 0x3f)))
#define DCHID_FLAGS_TYPE(f)    (((f) >> 6) & 3)

/* comm interface events (input reports on iface 0) */
#define DCHID_EVENT_GPIO_CMD 0xa0
#define DCHID_EVENT_INIT     0xf0
#define DCHID_EVENT_READY    0xf1

/* init blocks inside an EVENT_INIT */
#define DCHID_INIT_HID_DESCRIPTOR 0
#define DCHID_INIT_GPIO_REQUEST   1
#define DCHID_INIT_TERMINATOR     2
#define DCHID_INIT_PRODUCT_NAME   7

/* comm commands (feature SET_REPORT on iface 0) */
#define DCHID_CMD_RESET_INTERFACE 0x40
#define DCHID_CMD_SEND_FIRMWARE   0x95
#define DCHID_CMD_ACK_GPIO_CMD    0xa1
#define DCHID_CMD_ENABLE_IFACE    0xb4

#define DCHID_POWER_METHOD_1 1
#define DCHID_POWER_METHOD_2 2
#define DCHID_POWER_OFF      0
#define DCHID_POWER_ON       2

/* STM feature reports */
#define DCHID_STM_REPORT_ID     0x10
#define DCHID_STM_REPORT_SERIAL 0x11

/* multi-touch feature reports */
#define DCHID_MT_DIMENSIONS_REPORT 0xd9
#define DCHID_MT_REPORT_ID         0x75

/* "HIDF" firmware container (asahi-fwextract tpmtfw-*.bin) */
#define DCHID_FW_MAGIC 0x46444948
#define DCHID_FW_VER   1

struct dchid_hdr {
    u8 hdr_len;
    u8 channel;
    u16 length; /* payload bytes after this header, excluding the checksum */
    u8 seq;
    u8 iface;
    u16 pad;
} __attribute__((packed));

struct dchid_subhdr {
    u8 flags;
    u8 unk;
    u16 length;
    u32 retcode;
} __attribute__((packed));

struct dchid_init_hdr {
    u8 type;
    u8 unk1;
    u8 unk2;
    u8 iface;
    char name[DCHID_NAME_LEN];
    u8 more_packets;
    u8 unkpad;
} __attribute__((packed));

struct dchid_init_block_hdr {
    u16 type;
    u16 length;
} __attribute__((packed));

struct dchid_gpio_request {
    u16 unk;
    u16 id;
    char name[DCHID_GPIO_NAME];
} __attribute__((packed));

struct dchid_gpio_cmd {
    u8 type;
    u8 iface;
    u8 gpio;
    u8 unk;
    u8 cmd;
} __attribute__((packed));

struct dchid_fw_header {
    u32 magic;
    u32 version;
    u32 hdr_length;
    u32 data_length;
    u32 iface_offset;
} __attribute__((packed));

/* Result of parsing one EVENT_INIT packet. */
struct dchid_init_info {
    u8 iface;
    char name[DCHID_NAME_LEN + 1];
    bool more_packets;
    const u8 *descriptor;
    u16 descriptor_len;
    bool gpio_requested;
    u16 gpio_id;
    char gpio_name[DCHID_GPIO_NAME + 1];
};

u32 dchid_sum(const void *buf, size_t size);
/* Header + body(length + 4 checksum bytes) must sum to 0xffffffff. */
bool dchid_packet_valid(const struct dchid_hdr *hdr, const u8 *body);
/*
 * Build a command packet: header, subheader, msg (padded to 4) and checksum.
 * Returns the total number of bytes written, or 0 if it does not fit.
 */
size_t dchid_build(u8 *out, size_t cap, u8 channel, u8 iface, u8 seq, u8 flags, const void *msg,
                   size_t len);
/* Parse the payload of an EVENT_INIT (starting at the 0xf0 type byte). */
bool dchid_parse_init(const u8 *payload, size_t len, struct dchid_init_info *out);
/* Validate a HIDF firmware container; returns the data pointer/length. */
bool dchid_fw_parse(const u8 *blob, size_t size, const u8 **data, u32 *data_len,
                    u32 *iface_offset);
/* Interface power command (power method 1: 4 bytes, method 2: 9 bytes). */
size_t dchid_power_cmd(u8 *out, u8 method, u8 iface, u8 state, bool has_changed);

#endif
