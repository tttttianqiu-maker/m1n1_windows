/* SPDX-License-Identifier: ISC */
#ifndef BRCM_H
#define BRCM_H

#include "types.h"

/*
 * Broadcom FullMAC chip over PCIe (BCM4388 on J613), the parts of Linux brcmfmac m1n1 needs:
 * backplane/core handling, OTP, firmware download, shared-memory handshake, console.
 */

struct wlan_pci;

/* One firmware set, handed over by the host (run_uefi.py) for P_HV_WIFI stage 1. */
struct brcm_fw_set {
    u64 fw, fw_len;       /* brcmfmac<chip><rev>-pcie.apple,<board>.bin (macOS .trx) */
    u64 nvram, nvram_len; /* processed NVRAM blob (host does brcmfmac's text -> binary) */
    u64 clm, clm_len;     /* .clm_blob (macOS .clmb), optional */
    u64 txcap, txcap_len; /* .txcap_blob (macOS .txcb), optional */
    u64 sig, sig_len;     /* .sig, optional */
    u64 seed;             /* 256 random bytes from the host (Apple firmware wants a seed) */
};

struct brcm_otp {
    char module[16], vendor[16], version[16];
    bool valid;
};

int brcm_attach(const struct wlan_pci *pci);
const struct brcm_otp *brcm_otp(void);
void brcm_raminfo(u32 *rambase, u32 *ramsize);
int brcm_boot(const struct brcm_fw_set *fw);
void brcm_console_poll(void);

#endif
