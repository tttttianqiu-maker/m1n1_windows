/* SPDX-License-Identifier: MIT */
#ifndef WLAN_H
#define WLAN_H

#include "types.h"

/* J613 built-in Wi-Fi (Broadcom BCM4388 on PCIe port 0); see wlan.c. */

struct wlan_pci {
    u64 ecam;      /* PCIe ECAM base (bus 0 = root ports) */
    u64 port;      /* port 0 registers */
    u64 phy;       /* port 0 PHY lane registers */
    u64 cfg;       /* config space of 01:00.0 (the Wi-Fi function) */
    u64 bar0;      /* CPU address of BAR0 (backplane register window) */
    u64 bar0_size;
    u64 bar2;      /* CPU address of BAR2 (chip TCM) */
    u64 bar2_size;
    u16 device;    /* PCI device id (0x4434 = BCM4388) */
    u16 chip;      /* backplane chip id (0x4388) */
    u8 chiprev;
    bool up;
};

/* Stage 2 probe: PCIe link up, enumerate 01:00.0, read the chip id. Logs "wlan: ...". */
int wlan_probe(void);
const struct wlan_pci *wlan_pci(void);

#endif
