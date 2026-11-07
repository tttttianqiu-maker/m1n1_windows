/* SPDX-License-Identifier: MIT */
/*
 * Boot the MTP (multitouch/keyboard coprocessor) for J613 / T8122.
 *
 * Mirrors Asahi Linux drivers/soc/apple/rtkit-helper.c ("apple,rtk-helper-asc4"):
 * set the ASC CPU run bit and do a normal RTKit boot/wake. Shared-memory
 * buffers the firmware asks for are DART-backed (mtp_dart, stream 1 per
 * t8122.dtsi). Nothing here is board specific beyond the ADT paths, which
 * upstream m1n1 already uses (dapf.c "dart-mtp", kboot.c "dockchannel-mtp").
 *
 * EXP-20261107-031 showed the first version failing in iovad_init (unaligned base).
 */
#include "hv_mtp.h"
#include "pmgr.h"
#include "adt.h"
#include "asc.h"
#include "dart.h"
#include "iova.h"
#include "rtkit.h"
#include "soc.h"
#include "utils.h"

#define MTP_ASC_PATH  "/arm-io/mtp"
#define MTP_DART_PATH "/arm-io/dart-mtp"
#define MTP_DART_SID  1

static asc_dev_t *mtp_asc;
static dart_dev_t *mtp_dart;
static iova_domain_t *mtp_iovad;
static rtkit_dev_t *mtp_rtkit;
static bool mtp_up;

bool hv_mtp_running(void)
{
    return mtp_up;
}

/*
 * T8122 MTP SRAM, from Asahi Linux t8122.dtsi (mtp reg-names "asc", "sram").
 * The ADT's second reg is a different block (0x2fa050000 + 0x4000 on J613,
 * EXP-20261107-040), so the ADT is only logged, never used for the SRAM.
 */
#define T8122_MTP_SRAM_BASE 0x2fac00000UL
#define T8122_MTP_SRAM_SIZE 0x100000UL

bool hv_mtp_init(void)
{
    if (chip_id != T8122)
        return true; /* J313 uses SPI HID; nothing to do */
    if (mtp_up)
        return true;

    int node = adt_path_offset(adt, MTP_ASC_PATH);
    if (node < 0) {
        printf("hv_mtp: %s not in ADT, skipping (no built-in keyboard)\n", MTP_ASC_PATH);
        return false;
    }

    u64 sram_base = 0, sram_size = 0;
    {
        int path[8];
        adt_path_offset_trace(adt, MTP_ASC_PATH, path);
        for (int i = 0; i < 4; i++) {
            u64 base = 0, size = 0;
            if (adt_get_reg(adt, path, "reg", i, &base, &size) < 0)
                break;
            printf("hv_mtp: ADT %s reg[%d] = 0x%lx + 0x%lx\n", MTP_ASC_PATH, i, base, size);
        }
    }
    sram_base = T8122_MTP_SRAM_BASE;
    sram_size = T8122_MTP_SRAM_SIZE;

    /*
     * Power the MTP domain (ADT clock-gates of /arm-io/mtp, with parents)
     * before touching the ASC. After macOS shuts down it is off and the
     * coprocessor never sends HELLO (EXP-20261107-041). Linux/U-Boot get
     * the same via power-domains.
     */
    int pwr = pmgr_adt_power_enable(MTP_ASC_PATH);
    printf("hv_mtp: power domain enable %s (%d)\n", pwr < 0 ? "FAILED" : "ok", pwr);

    mtp_asc = asc_init(MTP_ASC_PATH);
    if (!mtp_asc) {
        printf("hv_mtp: asc_init failed\n");
        return false;
    }
    printf("hv_mtp: ASC CPU %s before boot\n", asc_cpu_running(mtp_asc) ? "running" : "stopped");

    mtp_dart = dart_init_adt(MTP_DART_PATH, 0, MTP_DART_SID, false);
    if (!mtp_dart) {
        printf("hv_mtp: dart_init_adt(%s) failed\n", MTP_DART_PATH);
        goto out_asc;
    }
    {
        u64 vm_base = dart_vm_base(mtp_dart);
        u64 base = ALIGN_UP(vm_base + 0x10000000, SZ_32M);
        mtp_iovad = iovad_init(base, base + 0x10000000);
        printf("hv_mtp: DART vm_base=0x%lx iova window 0x%lx..0x%lx\n", vm_base, base,
               base + 0x10000000);
    }
    if (!mtp_iovad) {
        printf("hv_mtp: iovad_init failed\n");
        goto out_dart;
    }

    mtp_rtkit = rtkit_init("mtp", mtp_asc, mtp_dart, mtp_iovad, NULL, false);
    if (!mtp_rtkit) {
        printf("hv_mtp: rtkit_init failed\n");
        goto out_iovad;
    }
    rtkit_set_sram_window(mtp_rtkit, sram_base, sram_size);
    printf("hv_mtp: SRAM window 0x%lx + 0x%lx\n", sram_base, sram_size);

    /* rtkit_boot() starts the ASC CPU (CPU_CONTROL |= RUN) and does the
     * hello/epmap/power-state handshake; it also covers the "already running,
     * just wake it" case like apple_rtkit_wake() in Linux. */
    if (!rtkit_boot(mtp_rtkit)) {
        printf("hv_mtp: RTKit boot failed\n");
        goto out_rtkit;
    }

    mtp_up = true;
    printf("hv_mtp: MTP running\n");
    return true;

out_rtkit:
    rtkit_free(mtp_rtkit);
    mtp_rtkit = NULL;
out_iovad:
    iovad_shutdown(mtp_iovad, mtp_dart);
    mtp_iovad = NULL;
out_dart:
    dart_shutdown(mtp_dart);
    mtp_dart = NULL;
out_asc:
    asc_free(mtp_asc);
    mtp_asc = NULL;
    return false;
}

/* DMA-map a host buffer for the MTP (e.g. the trackpad firmware); returns the device address. */
bool hv_mtp_map(void *buf, size_t size, u64 *dva)
{
    if (!mtp_up || !mtp_rtkit)
        return false;
    return rtkit_map(mtp_rtkit, buf, ALIGN_UP(size, SZ_16K), dva);
}

/* Service the MTP mailbox from the hypervisor tick: buffer requests, syslog
 * and crashlog messages must be answered or the coprocessor stalls. */
void hv_mtp_poll(void)
{
    struct rtkit_message msg;
    int budget = 16;

    if (!mtp_up)
        return;
    while (budget-- > 0 && rtkit_can_recv(mtp_rtkit)) {
        int ret = rtkit_recv(mtp_rtkit, &msg);
        if (ret < 0) {
            printf("hv_mtp: RTKit error %d, MTP stopped\n", ret);
            mtp_up = false;
            return;
        }
        /* ret > 0: a message for an endpoint we did not start; MTP's HID traffic
         * goes over DockChannel, not the mailbox, so there is nothing to forward. */
    }
}
