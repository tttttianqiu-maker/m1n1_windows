/* SPDX-License-Identifier: ISC */
#ifndef BRCM_H
#define BRCM_H

#ifdef BRCM_HOST_TEST
#include "brcm_host_test.h" /* tests/c: host build of the protocol code */
#else
#include "types.h"
#endif

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
    u64 cal, cal_len;     /* ADT /arm-io/wlan wifi-calibration-msf ("calload"), optional */
    u64 mac;              /* ADT /chosen mac-address-wifi0 in the low 6 bytes, or 0 */
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

/* brcm_fwil.c: after brcm_boot + brcm_msgbuf_init */
extern u8 brcm_mac[6]; /* the dongle's MAC once brcm_dongle_init ran */
int brcm_dongle_init(const struct brcm_fw_set *fw);
int brcm_join(const u8 *ssid, u32 ssid_len, const u8 *pmk, const char *pass, u32 pass_len);
int brcm_iovar_get(const char *name, void *data, u32 len);
int brcm_iovar_set(const char *name, const void *data, u32 len);
int brcm_iovar_int_get(const char *name, u32 *val);
int brcm_iovar_int_set(const char *name, u32 val);

#endif
