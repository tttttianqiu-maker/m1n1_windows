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

/* What the probe found, for the host (run_uefi.py picks the firmware files with it). */
struct wlan_info {
    u32 magic; /* WLAN_INFO_MAGIC once filled */
    u16 device, chip;
    u8 chiprev, up, attached, otp_valid;
    u32 rambase, ramsize;
    char module[16], vendor[16], version[16]; /* Apple OTP (brcmfmac naming) */
};
#define WLAN_INFO_MAGIC 0x4e414c57 /* "WLAN" */

struct brcm_fw_set;

/* Network to join, from the host (`j613 wifi "<name>" "<password>"`; the host derives the
   WPA2 PMK with PBKDF2-SHA1 like wpa_supplicant does). */
struct wlan_net {
    u32 magic; /* WLAN_NET_MAGIC */
    u32 ssid_len;
    u8 ssid[32];
    u8 pmk[32];
    u32 pass_len;
    char pass[64];
};
#define WLAN_NET_MAGIC 0x54454e57 /* "WNET" */
_Static_assert(sizeof(struct wlan_net) == 140, "tools/j613/wifi_net.py WLAN_NET_FMT");

/*
 * Probe: PCIe link up, enumerate 01:00.0, chip id, then the Broadcom core scan, reset,
 * RAM size and OTP (brcm.c). Fills *info when given. Logs "wlan: ...".
 */
int wlan_probe(struct wlan_info *info);
/* Firmware boot (probes first if needed), rings + ioctl channel, dongle init (MAC, CLM,
   TxCap, calibration, version, events, interface up). */
int wlan_boot(const struct brcm_fw_set *fw);
/* WPA2-PSK join; waits up to ~30 s for link + keys. */
int wlan_join(const struct wlan_net *net);
/* from hv_tick: service the chip while the guest runs */
void wlan_poll(void);
const struct wlan_pci *wlan_pci(void);

#endif
