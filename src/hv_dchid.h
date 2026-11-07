/* SPDX-License-Identifier: MIT */
#ifndef HV_DCHID_H
#define HV_DCHID_H

#include "types.h"
#include "hv_irq_routes.h"

/*
 * J613 (T8122) built-in keyboard and trackpad.
 *
 * The MTP coprocessor (hv_mtp.c) speaks Apple's DockChannel HID protocol.
 * m1n1 owns it completely: interface discovery, enabling, trackpad firmware
 * upload and power sequencing (AFE reset over SMC GPIO), GPIO requests. HID
 * reports and descriptors reach Windows through a small virtual device
 * ("AVHD", see below) that the J613 transport of drivers/apple-input reads.
 *
 * Guest-visible AVHD device (4 KiB MMIO page at HV_AVHD_IPA, edge vSPI HV_AVHD_GSIV):
 *   0x000 MAGIC     'AVHD' (0x44485641)
 *   0x004 VERSION   1
 *   0x008 STATUS    bit0 keyboard ready, bit1 trackpad ready, bit2 MTP up,
 *                   bit3 trackpad dimensions valid
 *   0x00c IRQ_ACK   write any value: re-arm the interrupt
 *   0x010 DESC_SEL  write 0 = keyboard, 1 = trackpad descriptor
 *   0x014 DESC_LEN  length of the selected HID report descriptor
 *   0x020 RX_LEN    length of the oldest queued report, 0 if none
 *   0x024 RX_IFACE  0 = keyboard, 1 = trackpad (raw MTP report 0x75)
 *   0x028 RX_POP    write any value: drop the oldest queued report
 *   0x02c RX_DROPS  reports dropped because the queue was full
 *   0x030 IDS       vendor id (15:0), product id (31:16) from the STM
 *   0x040 DIMS      16 bytes: width, height (u32, 1/100 mm), min_x, min_y,
 *                   max_x, max_y (s16) from multi-touch feature report 0xd9
 *   0x400 DESC      1 KiB window: selected descriptor bytes
 *   0x800 REPORT    2 KiB window: oldest queued report bytes
 */
#define HV_AVHD_IPA   0x5300000000UL
#define HV_AVHD_GSIV  HV_IRQ_SYNTHETIC_AVHD_VINTID
#define HV_AVHD_MAGIC 0x44485641

bool hv_dchid_init(void);
void hv_dchid_poll(void);
/* Trackpad firmware ("HIDF" container, asahi tpmtfw-j613.bin) staged by the proxy. */
bool hv_dchid_set_mt_firmware(void *blob, size_t size);

#endif
