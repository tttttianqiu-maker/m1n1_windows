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
 * UNTESTED ON HARDWARE.
 */
#include "hv_mtp.h"
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

bool hv_mtp_init(void)
{
    if (chip_id != T8122)
        return true; /* J313 uses SPI HID; nothing to do */
    if (mtp_up)
        return true;

    if (adt_path_offset(adt, MTP_ASC_PATH) < 0) {
        printf("hv_mtp: %s not in ADT, skipping (no built-in keyboard)\n", MTP_ASC_PATH);
        return false;
    }

    mtp_asc = asc_init(MTP_ASC_PATH);
    if (!mtp_asc) {
        printf("hv_mtp: asc_init failed\n");
        return false;
    }

    mtp_dart = dart_init_adt(MTP_DART_PATH, 0, MTP_DART_SID, true);
    if (!mtp_dart) {
        printf("hv_mtp: dart_init_adt(%s) failed\n", MTP_DART_PATH);
        goto out_asc;
    }
    {
        u64 vm_base = dart_vm_base(mtp_dart);
        mtp_iovad = iovad_init(vm_base + 0x10000000, vm_base + 0x20000000);
    }
    if (!mtp_iovad)
        goto out_dart;

    mtp_rtkit = rtkit_init("mtp", mtp_asc, mtp_dart, mtp_iovad, NULL, false);
    if (!mtp_rtkit)
        goto out_iovad;

    /* rtkit_boot() starts the ASC CPU (CPU_CONTROL |= RUN) and does the
     * hello/epmap/power-state handshake; it also covers the "already running,
     * just wake it" case like apple_rtkit_wake() in Linux. */
    if (!rtkit_boot(mtp_rtkit)) {
        printf("hv_mtp: RTKit boot failed\n");
        goto out_rtkit;
    }

    mtp_up = true;
    printf("hv_mtp: MTP running (DockChannel HID can now be driven by the guest)\n");
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
