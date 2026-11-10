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
typedef int32_t s32;
typedef int16_t s16;
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
/*
 * MTP HID descriptors advertise Report Size 0x4000 (16384 bits) items, which
 * Linux hid-core rejects (report_size > 256). Same rewrite as the upstream
 * DockChannel HID transport / hid-apple (2026-06 series, patches 07+08):
 * 76 00 40 95 n (1 <= n < 32) -> 75 08 96 00 n*8, i.e. the same number of
 * bits as 8-bit fields. Returns the number of items rewritten.
 */
unsigned int dchid_fixup_report_size(u8 *desc, size_t len);

/*
 * Keyboard descriptor served to the guest (AVHD DESC with DESC_SEL 0): a
 * plain boot-layout keyboard matching the MTP keyboard input report 1
 * (10 bytes: id, modifiers, reserved, 6 key codes, vendor byte).
 */
#define DCHID_KBD_REPORT_ID  0x01
#define DCHID_KBD_REPORT_LEN 10
/* Consumer Control (media keys) in the same guest descriptor: id + 2 x 16-bit usages */
#define DCHID_CONSUMER_REPORT_ID  0x02
#define DCHID_CONSUMER_REPORT_LEN 5
extern const u8 dchid_guest_kbd_desc[];
extern const size_t dchid_guest_kbd_desc_len;

/* Apple ISO keyboards: swap usages 0x35 and 0x64 (Linux hid-apple apple_iso_keyboard). */
void dchid_kbd_iso_swap(u8 report[DCHID_KBD_REPORT_LEN]);

/*
 * Scan a HID report descriptor for input report `report_id`: total input bits
 * and the bit offset (after the report ID byte) of the 1-bit Apple Fn field,
 * usage 0x0003 on vendor page 0x00ff, 0xff00 or 0xff01 (Linux hid-apple
 * apple_input_mapping). *fn_bit is -1 when there is none. False on a
 * malformed descriptor.
 */
bool dchid_hid_scan(const u8 *desc, size_t len, u8 report_id, u32 *input_bits, int *fn_bit);

/*
 * Fn layer of hid-apple's magic_keyboard_2021_and_2024_fn_keys (used for
 * BUS_HOST/DockChannel keyboards) without the media keys: Fn+Backspace =
 * Delete, Fn+Enter = Insert, Fn+Up/Down/Left/Right = PgUp/PgDn/Home/End.
 * Like hid-apple, a key keeps the meaning it had when it went down, so
 * releasing Fn first does not turn a held Delete into Backspace.
 */
struct dchid_fn_state {
    u8 down[32];     /* keys currently down (bitmap by usage) */
    u8 with_fn[32];  /* ...that went down while Fn was held */
    u16 consumer[2]; /* consumer usages last sent in report 2 */
    u8 backlight;    /* Fn+F5 (bit 0) / Fn+F6 (bit 1) held */
};
void dchid_kbd_fn_translate(struct dchid_fn_state *st, u8 report[DCHID_KBD_REPORT_LEN], bool fn);
/*
 * Media layer, after dchid_kbd_fn_translate on the same report: F-keys that
 * went down with Fn become Consumer Control usages (hid-apple's 2021+ table:
 * F1/F2 brightness, F4 search, F7/F8/F9 previous/play-pause/next, F10 mute,
 * F11/F12 volume) and leave the keyboard report; F3/F5/F6 stay F-keys.
 * Returns true when report 2 must be sent; `out` gets it.
 */
bool dchid_kbd_fn_media(struct dchid_fn_state *st, u8 report[DCHID_KBD_REPORT_LEN],
                        u8 out[DCHID_CONSUMER_REPORT_LEN]);
/*
 * Keyboard backlight keys, only with `j613 kbdlight on` (m1n1 drives the LEDs,
 * Windows has no keyboard backlight control without a driver): Fn+F5 / Fn+F6
 * (dictation / do-not-disturb on the M3 Air, nothing in Windows) leave the
 * keyboard report while held. Returns -1 (F5) or +1 (F6) on a new press, else 0.
 * After dchid_kbd_fn_translate on the same report.
 */
int dchid_kbd_fn_backlight(struct dchid_fn_state *st, u8 report[DCHID_KBD_REPORT_LEN]);

/* MTP trackpad report 0x75 (Linux hid-magicmouse.c struct tp_header / tp_finger). */
#define DCHID_MT_HDR_SIZE        38
#define DCHID_MT_FINGER_SIZE     30
#define DCHID_MT_NFINGERS_OFFSET 22
#define DCHID_MT_BUTTONS_OFFSET  23
#define DCHID_MT_GUEST_FINGERS   11 /* AI_APPLE_TRACKPAD_MAX_CONTACTS of the Windows decoder */
#define DCHID_MT_GUEST_MAX (DCHID_MT_HDR_SIZE + DCHID_MT_GUEST_FINGERS * DCHID_MT_FINGER_SIZE)
/*
 * Reduce a report to header + num_fingers * 30 (Linux accepts spare finger
 * slots, the Windows decoder wants the exact length), at most 11 fingers,
 * buttons bit 0 only. Returns 0 (as is), 1 (trimmed) or -1 (unusable).
 */
int dchid_mt_sanitize(const u8 *in, size_t len, u8 *out, size_t cap, size_t *out_len);

/*
 * Battery + AC adapter for the ACPI devices BAT0/ADP1 (J613 DSDT), served
 * from the AVHD page at DCHID_BATT_BASE: one u32 per register. Built from
 * SMC keys read like Linux macsmc-power (read-only: BUIC %, B0FC/B0DC mAh,
 * B0AV mV, B0AC mA, B0CT, CHIS AC online, BNCB cells). An m1n1 without
 * battery support reads 0 there, which hides both devices (BFLG bit 0).
 */
#define DCHID_BATT_BASE  0x100
#define DCHID_BATT_MAGIC 0x54544142 /* "BATT" */
enum {
    DCHID_BATT_MAG,  /* +0x00 */
    DCHID_BATT_FLG,  /* +0x04 bit0 present, 1 AC online, 2 charging, 3 discharging, 4 critical */
    DCHID_BATT_DCAP, /* +0x08 design capacity, mAh */
    DCHID_BATT_FCAP, /* +0x0c last full charge capacity, mAh */
    DCHID_BATT_RCAP, /* +0x10 remaining capacity, mAh */
    DCHID_BATT_RATE, /* +0x14 present rate, mA (0xffffffff unknown) */
    DCHID_BATT_VOLT, /* +0x18 present voltage, mV (0xffffffff unknown) */
    DCHID_BATT_CYC,  /* +0x1c cycle count (0xffffffff unknown) */
    DCHID_BATT_DVOLT,/* +0x20 design voltage, mV */
    DCHID_BATT_PCT,  /* +0x24 percent */
    DCHID_BATT_SEQ,  /* +0x28 refresh counter */
    DCHID_BATT_EVT,  /* +0x2c read-to-clear: DCHID_BATT_EVT_* since the last read */
    DCHID_BATT_NREGS
};
/* Events for the battery GED (DSDT BGED _EVT -> Notify), vINTID HV_IRQ_SYNTHETIC_BATT_VINTID */
#define DCHID_BATT_EVT_BST (1u << 0) /* _BST data changed: Notify(BAT0, 0x80) */
#define DCHID_BATT_EVT_PSR (1u << 1) /* AC plugged/unplugged: Notify(ADP1, 0x80) */
#define DCHID_BATT_EVT_BIX (1u << 2) /* capacity/cycles changed: Notify(BAT0, 0x81) */
#define DCHID_BATT_PRESENT     (1u << 0)
#define DCHID_BATT_AC          (1u << 1)
#define DCHID_BATT_CHARGING    (1u << 2)
#define DCHID_BATT_DISCHARGING (1u << 3)
#define DCHID_BATT_CRITICAL    (1u << 4)
struct dchid_batt_raw {
    bool ok_buic, ok_fcc, ok_dcap, ok_volt, ok_cur, ok_cyc, ok_chis, ok_cells;
    u8 buic, cells;
    u16 fcc, dcap, volt, cyc;
    s16 cur;
    u32 chis;
};
void dchid_batt_regs(const struct dchid_batt_raw *raw, u32 seq, u32 regs[DCHID_BATT_NREGS]);

/*
 * What the guest sees across refreshes. A failed or implausible SMC read never
 * replaces a good snapshot: a battery that suddenly reads 0 % on battery power
 * is critical to Windows, which hibernates or shuts down at once. AC state
 * unknown (CHIS read failed) keeps the last one, and before the first good
 * read counts as AC (no critical action). A jump of more than
 * DCHID_BATT_MAX_JUMP points between two reads (60 s apart; a real battery
 * moves about 2 % a minute at most) is held back twice before it is believed.
 * Returns true when regs took the new reading.
 */
#define DCHID_BATT_MAX_JUMP 20
struct dchid_batt {
    u32 regs[DCHID_BATT_NREGS];
    u32 seq;
    u32 held; /* consecutive reads kept back */
};
bool dchid_batt_update(struct dchid_batt *b, const struct dchid_batt_raw *raw);
/* What changed between two register snapshots, as DCHID_BATT_EVT_* bits. */
u32 dchid_batt_events(const u32 old[DCHID_BATT_NREGS], const u32 now[DCHID_BATT_NREGS]);

/*
 * Guest report queue: one ring per interface (0 keyboard, 1 trackpad), the
 * oldest report dropped on overflow; the guest reads a latched copy of the
 * oldest report and pops it. No locking here (callers hold their lock).
 */
#define DCHID_VQ_SLOT_SIZE 1024
struct dchid_vq_slot {
    u32 seq;
    u16 len;
    u8 iface;
    u8 data[DCHID_VQ_SLOT_SIZE];
};
struct dchid_vq_ring {
    struct dchid_vq_slot *slot;
    u32 cap, head, count;
};
struct dchid_vq {
    struct dchid_vq_ring ring[2];
    struct dchid_vq_slot cur;
    bool cur_valid;
    u32 seq, count, drops;
};
void dchid_vq_push(struct dchid_vq *q, u8 iface, const u8 *data, size_t len);
/* Latch the oldest report if none is latched; returns the latched one or NULL. */
const struct dchid_vq_slot *dchid_vq_latch(struct dchid_vq *q);
void dchid_vq_pop(struct dchid_vq *q);
void dchid_vq_flush(struct dchid_vq *q);

#endif
