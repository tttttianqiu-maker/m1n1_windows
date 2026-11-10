/* SPDX-License-Identifier: MIT */
#ifndef HV_VNET_H
#define HV_VNET_H

#ifdef VNET_HOST_TEST
#include "vnet_host_test.h" /* tests/c: host build of the device model */
#else
#include "types.h"
#endif

/*
 * J613 built-in Wi-Fi for Windows: a virtio-net device (virtio 1.x over PCI, "modern" /
 * non-transitional, 1af4:1041) at 00:01.0 of hv_pci.c's emulated PCIe root, bridged to the
 * BCM4388 that m1n1 drives (brcm_msgbuf.c). Windows binds Red Hat's NetKVM driver to it.
 * No MSI-X: INTA through the DSDT _PRT to vINTID HV_IRQ_SYNTHETIC_VNET_VINTID, injected
 * into the vGIC on the boot CPU like the NVMe INTx.
 */

/* present (Wi-Fi up) with this MAC; until then 00:01.0 reads as "no device" */
void hv_vnet_init(const u8 mac[6]);
bool hv_vnet_present(void);
/* PCI config space of 00:01.0, from hv_pci.c's ECAM hook */
u32 hv_vnet_cfg_read(u32 reg);
void hv_vnet_cfg_write(u32 reg, u32 boff, u32 bytes, u64 data);
/* one received 802.3 frame from the Wi-Fi (no FCS) */
void hv_vnet_rx(const u8 *frame, u32 len);
/* Wi-Fi association + keys: the guest sees VIRTIO_NET_S_LINK_UP */
void hv_vnet_set_link(bool up);
/* interrupt delivery; only acts on the boot CPU (hv_exc_exit, hv_tick) */
void hv_vnet_poll_irq(void);
void hv_vnet_irq_eoi(u32 intid);
/* hv_tick: statistics line every 60 s */
void hv_vnet_poll(void);

#endif
