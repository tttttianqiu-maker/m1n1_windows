/* SPDX-License-Identifier: ISC */
/*
 * Dongle control for m1n1's Broadcom FullMAC port (J613 BCM4388): iovars, the bring-up
 * commands Linux brcmfmac issues after the bus is up (common.c brcmf_c_preinit_dcmds:
 * MAC, revinfo, CLM/TxCap/calibration blob download, version, mpc, scan times; fweh.c
 * event mask; feature.c join interface version) and a WPA2-PSK join with the firmware
 * supplicant the way cfg80211.c brcmf_cfg80211_connect does it (wpa_auth, auth, wsec,
 * sup_wpa, wpaie, WSEC_PMK, "join" with the versioned ext-join struct, SET_SSID fallback).
 *
 * Copyright (c) 2010-2014 Broadcom Corporation (brcmfmac)
 * Copyright (c) 2023 Daniel Berlin (join_param.c, versioned join structures)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "brcm.h"
#include "brcm_msgbuf.h"
#ifndef BRCM_HOST_TEST
#include "string.h"
#include "utils.h"
#endif

#define C_UP                    2
#define C_SET_INFRA             20
#define C_GET_BSSID             23
#define C_SET_SSID              26
#define C_DISASSOC              52
#define C_GET_REVINFO           98
#define C_GET_RSSI              127
#define C_SET_SCAN_CHANNEL_TIME 185
#define C_SET_SCAN_UNASSOC_TIME 187
#define C_GET_VAR               262
#define C_SET_VAR               263
#define C_SET_WSEC_PMK          268
#define C_SET_PM                86
#define C_SET_FAKEFRAG          219
#define C_SET_SCAN_PASSIVE_TIME 258
#define PM_FAST                 2
#define WPA2_AUTH_UNSPECIFIED   0x0040

#define AES_ENABLED      0x0004
#define WPA2_AUTH_PSK    0x0080
#define WSEC_PASSPHRASE  0x0001
#define MAX_CHUNK_LEN    1400
#define DL_BEGIN         0x0002
#define DL_END           0x0004
#define DL_TYPE_CLM      2
#define DLOAD_HANDLER_VER 1
#define DLOAD_FLAG_VER_SHIFT 12

static u8 iobuf[8192];
u8 brcm_mac[6];
static u16 join_version;
static bool fw_up;

int brcm_iovar_get(const char *name, void *data, u32 len)
{
    u32 nl = strlen(name) + 1;
    if (nl + len > sizeof(iobuf))
        return -1;
    memset(iobuf, 0, nl + len);
    memcpy(iobuf, name, nl);
    if (data && len)
        memcpy(iobuf + nl, data, len);
    int ret = brcm_ioctl(C_GET_VAR, iobuf, nl + len, NULL);
    if (!ret && data)
        memcpy(data, iobuf, len);
    return ret;
}

int brcm_iovar_set(const char *name, const void *data, u32 len)
{
    u32 nl = strlen(name) + 1;
    if (nl + len > sizeof(iobuf))
        return -1;
    memcpy(iobuf, name, nl);
    if (len)
        memcpy(iobuf + nl, data, len);
    return brcm_ioctl(C_SET_VAR, iobuf, nl + len, NULL);
}

int brcm_iovar_int_set(const char *name, u32 val)
{
    return brcm_iovar_set(name, &val, 4);
}

int brcm_iovar_int_get(const char *name, u32 *val)
{
    *val = 0;
    return brcm_iovar_get(name, val, 4);
}

static int cmd_int_set(u32 cmd, u32 val)
{
    return brcm_ioctl(cmd, &val, 4, NULL);
}

/* first line of a firmware string iovar, for the log */
static void log_string_iovar(const char *name)
{
    static char buf[256];
    memset(buf, 0, sizeof(buf));
    int ret = brcm_iovar_get(name, buf, sizeof(buf));
    buf[sizeof(buf) - 1] = 0;
    for (char *p = buf; *p; p++)
        if (*p == '\n' || *p == '\r') {
            *p = 0;
            break;
        }
    if (ret)
        printf("wlan: %s: error %d\n", name, ret);
    else
        printf("wlan: %s: %s\n", name, buf);
}

/* common.c brcmf_c_download_blob: "clmload"/"txcapload"/"calload" in 1400-byte chunks */
static int download_blob(const char *loadvar, const char *statvar, const u8 *data, u32 size)
{
    static u8 chunk[12 + MAX_CHUNK_LEN];
    u16 flag = DL_BEGIN;
    u32 done = 0;
    int ret = 0;

    while (done < size) {
        u32 n = size - done;
        if (n > MAX_CHUNK_LEN)
            n = MAX_CHUNK_LEN;
        else
            flag |= DL_END;
        u16 f = flag | (DLOAD_HANDLER_VER << DLOAD_FLAG_VER_SHIFT), type = DL_TYPE_CLM;
        u32 crc = 0;
        memcpy(chunk + 0, &f, 2);
        memcpy(chunk + 2, &type, 2);
        memcpy(chunk + 4, &n, 4);
        memcpy(chunk + 8, &crc, 4);
        memcpy(chunk + 12, data + done, n);
        ret = brcm_iovar_set(loadvar, chunk, 12 + n);
        if (ret)
            break;
        flag &= ~DL_BEGIN;
        done += n;
    }
    if (ret) {
        u32 status = 0;
        brcm_iovar_int_get(statvar, &status);
        printf("wlan: %s (%u bytes) failed at %u: error %d, %s %u\n", loadvar, size, done, ret,
               statvar, status);
        return ret;
    }
    printf("wlan: %s: %u bytes loaded\n", loadvar, size);
    return 0;
}

static void enable_events(void)
{
    /* the events the join and link tracking need; fweh.c always adds IF */
    static const u8 wanted[] = {BRCM_E_SET_SSID,     BRCM_E_JOIN,  BRCM_E_AUTH,
                                BRCM_E_DEAUTH,       BRCM_E_DEAUTH_IND, BRCM_E_ASSOC,
                                BRCM_E_REASSOC,      BRCM_E_DISASSOC,   BRCM_E_DISASSOC_IND,
                                BRCM_E_LINK,         BRCM_E_PSK_SUP,    BRCM_E_IF};
    /*
     * fweh.c brcmf_fweh_init_events + brcmf_fweh_activate_events: read the firmware's mask
     * length with event_msgs_ext (version 1), then set a mask built from zero, so the
     * firmware's default events do not flood the log. Fallback: plain event_msgs.
     */
    u8 ext[4 + 32];
    memset(ext, 0, sizeof(ext));
    ext[0] = 1;  /* EVENTMSGS_VER */
    ext[1] = 0;  /* EVENTMSGS_NONE */
    ext[2] = 32; /* len */
    ext[3] = 32; /* maxgetsize */
    if (!brcm_iovar_get("event_msgs_ext", ext, sizeof(ext))) {
        u8 len = ext[2] ? ext[2] : 27; /* 27 = BRCMF_WCC_E_LAST (213) bits */
        if (len > 32)
            len = 32;
        memset(ext, 0, sizeof(ext));
        for (u32 i = 0; i < sizeof(wanted); i++)
            if (wanted[i] / 8 < len)
                ext[4 + wanted[i] / 8] |= 1 << (wanted[i] % 8);
        ext[0] = 1;
        ext[1] = 3; /* EVENTMSGS_SET_MASK */
        ext[2] = len;
        int ret = brcm_iovar_set("event_msgs_ext", ext, 4 + len);
        printf("wlan: event_msgs_ext (%u bytes): %s\n", len, ret ? "failed" : "set");
        if (!ret)
            return;
    }
    u8 mask[18];
    memset(mask, 0, sizeof(mask));
    for (u32 i = 0; i < sizeof(wanted); i++)
        mask[wanted[i] / 8] |= 1 << (wanted[i] % 8);
    printf("wlan: event_msgs: %s\n", brcm_iovar_set("event_msgs", mask, sizeof(mask)) ? "failed" : "set");
}

/* common.c brcmf_c_set_joinpref_default: pick by RSSI, 5 GHz gets an 8 dB boost */
static void set_joinpref_default(void)
{
    static const u8 pref[] = {
        4, 2, 8, 1, /* BRCMF_JOIN_PREF_RSSI_DELTA, len 2, BRCMF_JOIN_PREF_RSSI_BOOST, 5 GHz */
        1, 2, 0, 0, /* BRCMF_JOIN_PREF_RSSI, len 2 */
    };
    if (brcm_iovar_set("join_pref", pref, sizeof(pref)))
        printf("wlan: join_pref failed\n");
}

static bool have_mfp;

int brcm_dongle_init(const struct brcm_fw_set *fw)
{
    fw_up = false;

    /* ---- common.c brcmf_c_preinit_dcmds ---- */
    u8 mac[6];
    memcpy(mac, &fw->mac, 6);
    bool have_mac = mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5];
    if (have_mac && brcm_iovar_set("cur_etheraddr", mac, 6))
        printf("wlan: setting the MAC failed\n");
    memset(mac, 0, sizeof(mac));
    if (brcm_iovar_get("cur_etheraddr", mac, 6)) {
        printf("wlan: no answer to the first ioctl (cur_etheraddr): the control path is not "
               "working\n");
        return -1;
    }
    printf("wlan: MAC %02x:%02x:%02x:%02x:%02x:%02x%s\n", mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5], have_mac ? " (from the ADT)" : "");
    memcpy(brcm_mac, mac, 6);

    u32 rev[17];
    memset(rev, 0, sizeof(rev));
    if (!brcm_ioctl(C_GET_REVINFO, rev, sizeof(rev), NULL))
        printf("wlan: revinfo: chip %x rev %u, board 0x%x/0x%x rev 0x%x, radio 0x%x, phy %u/%u, "
               "ucode 0x%x\n",
               rev[11], rev[3], rev[5], rev[6], rev[7], rev[2], rev[12], rev[13], rev[9]);

    if (fw->clm && fw->clm_len)
        download_blob("clmload", "clmload_status", (const u8 *)fw->clm, fw->clm_len);
    else
        printf("wlan: no CLM blob: the firmware may allow only a few channels\n");
    if (fw->txcap && fw->txcap_len)
        download_blob("txcapload", "txcapload_status", (const u8 *)fw->txcap, fw->txcap_len);
    if (fw->cal && fw->cal_len)
        download_blob("calload", "calload_status", (const u8 *)fw->cal, fw->cal_len);
    else
        printf("wlan: no calibration blob (ADT wifi-calibration-msf)\n");

    log_string_iovar("ver");
    log_string_iovar("clmver");
    if (brcm_iovar_int_set("mpc", 1))
        printf("wlan: mpc failed\n");
    set_joinpref_default();
    cmd_int_set(C_SET_SCAN_CHANNEL_TIME, 40);
    cmd_int_set(C_SET_SCAN_UNASSOC_TIME, 40);
    brcm_iovar_int_set("txbf", 1); /* errors ignored, as in Linux */

    /* ---- feature.c brcmf_feat_attach: mfp, sup_wpa, join interface version ---- */
    u32 v = 0;
    have_mfp = !brcm_iovar_int_get("mfp", &v);
    u32 sup = 0;
    if (brcm_iovar_int_get("sup_wpa", &sup))
        printf("wlan: no firmware supplicant (sup_wpa): WPA2 join will not work\n");
    u8 jv[8];
    memset(jv, 0, sizeof(jv));
    if (!brcm_iovar_get("join_ver", jv, sizeof(jv))) {
        join_version = jv[4] | (jv[5] << 8);
        printf("wlan: join interface version %u (join_ver)%s\n", join_version,
               join_version > 1 ? ": newer than Linux knows, using v1 layout" : "");
    } else {
        u16 wv[8];
        memset(wv, 0, sizeof(wv));
        join_version = 0;
        if (!brcm_iovar_get("wlc_ver", wv, sizeof(wv))) {
            u16 major = wv[6], minor = wv[7];
            if ((major == 14 && (minor == 2 || minor == 4)) || (major == 16 && minor >= 1) ||
                major >= 17)
                join_version = 1;
            printf("wlan: wlc_ver %u.%u -> join interface version %u\n", major, minor,
                   join_version);
        }
    }

    /* ---- fweh.c events ---- */
    enable_events();

    char cc[12];
    memset(cc, 0, sizeof(cc));
    if (!brcm_iovar_get("country", cc, sizeof(cc)))
        printf("wlan: country %.4s rev %d (%.4s)\n", cc, *(s32 *)(cc + 4), cc + 8);

    /* ---- cfg80211.c brcmf_config_dongle (interface up) ---- */
    int ret = cmd_int_set(C_UP, 0);
    printf("wlan: interface up: %s\n", ret ? "failed" : "OK");
    if (ret)
        return -1;
    cmd_int_set(C_SET_SCAN_CHANNEL_TIME, 40);
    cmd_int_set(C_SET_SCAN_UNASSOC_TIME, 40);
    cmd_int_set(C_SET_SCAN_PASSIVE_TIME, 120);
    if (cmd_int_set(C_SET_PM, PM_FAST))
        printf("wlan: power save mode failed\n");
    if (cmd_int_set(C_SET_INFRA, 1))
        printf("wlan: infrastructure mode failed\n");
    if (cmd_int_set(C_SET_FAKEFRAG, 1))
        printf("wlan: frameburst failed\n");
    /* core.c _brcmf_set_multicast_list: Windows' multicast list does not reach m1n1 (no
       virtio control queue), so the firmware passes all multicast (IPv6 ND, mDNS, LLMNR) */
    if (brcm_iovar_int_set("allmulti", 1))
        printf("wlan: allmulti failed\n");
    fw_up = true;
    return 0;
}

/* ---------- join ---------- */

static void put_le16(u8 *p, u16 v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static void put_le32(u8 *p, u32 v)
{
    put_le16(p, v);
    put_le16(p + 2, v >> 16);
}

/* brcmf_ssid_le (36) + brcmf_join_scan_params_le (20) + assoc params (v0 12, v1 16) */
static u32 build_ext_join(u8 *b, const u8 *ssid, u32 ssid_len, u8 version)
{
    u32 o = 0;
    memset(b, 0, 96);
    if (version >= 1) {
        put_le16(b, 1);
        o = 4;
    }
    put_le32(b + o, ssid_len);
    memcpy(b + o + 4, ssid, ssid_len);
    o += 36;
    b[o] = 0; /* scan_type default */
    put_le32(b + o + 4, 0xffffffff);  /* nprobes */
    put_le32(b + o + 8, 0xffffffff);  /* active_time */
    put_le32(b + o + 12, 0xffffffff); /* passive_time */
    put_le32(b + o + 16, 0xffffffff); /* home_time */
    o += 20;
    if (version >= 1) {
        put_le16(b + o, 1); /* assoc params version */
        o += 4;
    }
    memset(b + o, 0xff, 6); /* any BSSID */
    o += 6 + 2 + 4;         /* bssid_cnt 0, chanspec_num 0: all channels */
    return o;
}

/* brcmf_join_params: ssid + assoc params (no scan params), for WLC_SET_SSID */
static u32 build_join(u8 *b, const u8 *ssid, u32 ssid_len, u8 version)
{
    memset(b, 0, 64);
    put_le32(b, ssid_len);
    memcpy(b + 4, ssid, ssid_len);
    u32 o = 36;
    if (version >= 1) {
        put_le16(b + o, 1);
        o += 4;
    }
    memset(b + o, 0xff, 6);
    return o + 12;
}

static int set_pmk(const u8 *key, u32 len, u16 flags)
{
    /* brcmf_wsec_pmk_le as in the Asahi tree: key_len, flags, key[256], opt_len */
    static u8 pmk[2 + 2 + 256 + 2];
    memset(pmk, 0, sizeof(pmk));
    put_le16(pmk, len);
    put_le16(pmk + 2, flags);
    memcpy(pmk + 4, key, len);
    return brcm_ioctl(C_SET_WSEC_PMK, pmk, sizeof(pmk), NULL);
}

/* fweh.h BRCMF_E_STATUS_NO_NETWORKS / BRCMF_E_STATUS_FWSUP_TIMEOUT */
#define STATUS_NO_NETWORKS 3
#define FWSUP_TIMEOUT      7

static int join_once(const u8 *ssid, u32 ssid_len, const u8 *key, u32 key_len, u16 key_flags,
                     u32 timeout_ms)
{
    /* RSN IE for WPA2-PSK/CCMP the way wpa_supplicant builds it (pmf off, no PMKID) */
    static const u8 rsn_ie[] = {0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                                0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,
                                0x00, 0x0f, 0xac, 0x02, 0x00, 0x00};
    /* cfg80211.c brcmf_cfg80211_connect, in its order */
    int err = 0;
    err |= brcm_iovar_set("wpaie", rsn_ie, sizeof(rsn_ie)) ? 1 : 0;
    err |= brcm_iovar_int_set("wpa_auth", WPA2_AUTH_PSK | WPA2_AUTH_UNSPECIFIED) ? 2 : 0;
    err |= brcm_iovar_int_set("auth", 0) ? 4 : 0;          /* open system */
    err |= brcm_iovar_int_set("wsec", AES_ENABLED) ? 8 : 0; /* pairwise | group CCMP */
    if (have_mfp)
        err |= brcm_iovar_int_set("mfp", 0) ? 16 : 0; /* our RSN IE: no MFPC/MFPR */
    err |= brcm_iovar_int_set("wpa_auth", WPA2_AUTH_PSK) ? 32 : 0;
    err |= brcm_iovar_int_set("sup_wpa", 1) ? 64 : 0; /* firmware 4-way handshake */
    if (err)
        printf("wlan: some security settings failed (mask 0x%x)\n", err);
    int ret = set_pmk(key, key_len, key_flags);
    if (ret) {
        printf("wlan: WSEC_PMK (%s) failed: %d\n", key_flags ? "passphrase" : "PMK", ret);
        return -1;
    }

    brcm_link_clear();
    static u8 jp[96];
    u32 n = build_ext_join(jp, ssid, ssid_len, join_version);
    ret = brcm_iovar_set("join", jp, n);
    if (ret) {
        printf("wlan: join iovar (v%u) failed: %d, trying SET_SSID\n", join_version, ret);
        n = build_join(jp, ssid, ssid_len, join_version);
        ret = brcm_ioctl(C_SET_SSID, jp, n, NULL);
        if (ret) {
            printf("wlan: SET_SSID failed: %d\n", ret);
            return -1;
        }
    }

    /* cfg80211.c brcmf_is_linkup with the firmware supplicant: SET_SSID success + keyed */
    const struct brcm_link *l = brcm_link_state();
    u64 t0 = get_ticks();
    while (ticks_to_msecs(get_ticks() - t0) < timeout_ms) {
        brcm_poll();
        bool assoc_ok = l->link || (l->assoc_done && l->assoc_status == 0);
        if (l->keyed && assoc_ok)
            return 0;
        if (l->fw_halted)
            return -1;
        if (l->assoc_done && l->assoc_status != 0)
            return -1;
        if (l->sup_status == FWSUP_TIMEOUT)
            return -1;
        udelay(100);
    }
    printf("wlan: join timed out after %u ms\n", timeout_ms);
    return -1;
}

int brcm_join(const u8 *ssid, u32 ssid_len, const u8 *pmk, const char *pass, u32 pass_len)
{
    if (!fw_up) {
        printf("wlan: firmware not initialised, cannot join\n");
        return -1;
    }
    if (!ssid_len || ssid_len > 32)
        return -1;
    printf("wlan: joining \"%.*s\" (WPA2-PSK, firmware supplicant)\n", (int)ssid_len, ssid);
    int ret = join_once(ssid, ssid_len, pmk, 32, 0, 15000);
    const struct brcm_link *l0 = brcm_link_state();
    bool not_found = l0->assoc_done && l0->assoc_status == STATUS_NO_NETWORKS;
    if (not_found)
        printf("wlan: \"%.*s\" was not found (out of range, hidden, 6 GHz only or not WPA2)\n",
               (int)ssid_len, ssid);
    if (ret && !not_found && pass && pass_len >= 8 && pass_len <= 63 && !l0->fw_halted) {
        printf("wlan: no key with the PMK; retrying with the passphrase (firmware computes the "
               "PMK)\n");
        brcm_ioctl(C_DISASSOC, NULL, 0, NULL);
        mdelay(200);
        ret = join_once(ssid, ssid_len, (const u8 *)pass, pass_len, WSEC_PASSPHRASE, 15000);
    }
    const struct brcm_link *l = brcm_link_state();
    if (!ret) {
        u8 bssid[6];
        memset(bssid, 0, sizeof(bssid));
        brcm_ioctl(C_GET_BSSID, bssid, 6, NULL);
        s32 scbval[3] = {0}; /* struct brcmf_scb_val_le: val, ea[6] */
        brcm_ioctl(C_GET_RSSI, scbval, sizeof(scbval), NULL);
        s32 rssi = scbval[0];
        printf("wlan: CONNECTED to %02x:%02x:%02x:%02x:%02x:%02x, RSSI %d dBm, %lu frames "
               "received so far\n",
               bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], rssi,
               (unsigned long)l->rx_frames);
    } else {
        printf("wlan: join failed: link %u, keyed %u, SET_SSID status %u%s, supplicant status "
               "%u, last reason %u\n",
               l->link, l->keyed, l->assoc_status, l->assoc_done ? "" : " (no event)",
               l->sup_status, l->last_reason);
        brcm_console_poll();
    }
    return ret;
}
