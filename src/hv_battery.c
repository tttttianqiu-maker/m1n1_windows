/* SPDX-License-Identifier: MIT */
/*
 * J613 battery + AC adapter for the Windows guest. Off by default
 * (`j613 battery on` -> P_HV_BATTERY_MODE before the guest starts).
 *
 * The values come from the SMC with the same keys Linux macsmc-power reads,
 * read-only (Linux also writes charge-control keys at probe; m1n1 does not
 * touch charging). Each refresh opens the SMC, reads, and quiesces it again,
 * the pattern hv_dchid uses for its GPIO pulses: never leave the SMC RTKit
 * booted without a reader (EXP-20261108-045). First refresh before the guest
 * starts, then every 60 s from the hypervisor tick.
 */
#include "hv_battery.h"
#include "dchid_proto.h"
#include "smc.h"
#include "string.h"
#include "utils.h"

#define BATT_REFRESH_MS 60000

#define KEY(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

static bool batt_on;
static u32 batt_regs[DCHID_BATT_NREGS];
static u32 batt_seq, batt_fail;
static u64 batt_last;

static void batt_refresh(void)
{
    struct dchid_batt_raw r;
    smc_dev_t *smc = smc_init();

    if (!smc) {
        if (batt_fail++ < 4)
            printf("battery: SMC unavailable, keeping the last values\n");
        return;
    }
    memset(&r, 0, sizeof(r));
    r.ok_buic = !smc_read(smc, KEY('B', 'U', 'I', 'C'), &r.buic, 1);
    r.ok_fcc = !smc_read(smc, KEY('B', '0', 'F', 'C'), &r.fcc, 2);
    r.ok_dcap = !smc_read(smc, KEY('B', '0', 'D', 'C'), &r.dcap, 2);
    r.ok_volt = !smc_read(smc, KEY('B', '0', 'A', 'V'), &r.volt, 2);
    r.ok_cur = !smc_read(smc, KEY('B', '0', 'A', 'C'), &r.cur, 2);
    r.ok_cyc = !smc_read(smc, KEY('B', '0', 'C', 'T'), &r.cyc, 2);
    r.ok_chis = !smc_read(smc, KEY('C', 'H', 'I', 'S'), &r.chis, 4);
    r.ok_cells = !smc_read(smc, KEY('B', 'N', 'C', 'B'), &r.cells, 1);
    smc_shutdown(smc);

    u32 regs[DCHID_BATT_NREGS];
    dchid_batt_regs(&r, ++batt_seq, regs);
    memcpy(batt_regs, regs, sizeof(regs));

    if (batt_seq == 1 || !(batt_seq % 30))
        printf("battery: #%u %u%% %s%s%s fcc %u mAh design %u mAh %d mA %u mV cycles %u cells %u "
               "(keys %d%d%d%d%d%d%d%d)\n",
               batt_seq, regs[DCHID_BATT_PCT],
               regs[DCHID_BATT_FLG] & DCHID_BATT_PRESENT ? "" : "NOT PRESENT ",
               regs[DCHID_BATT_FLG] & DCHID_BATT_AC ? "AC " : "battery ",
               regs[DCHID_BATT_FLG] & DCHID_BATT_CHARGING ? "charging" : "",
               regs[DCHID_BATT_FCAP], regs[DCHID_BATT_DCAP], r.cur, r.volt, r.cyc, r.cells,
               r.ok_buic, r.ok_fcc, r.ok_dcap, r.ok_volt, r.ok_cur, r.ok_cyc, r.ok_chis,
               r.ok_cells);
}

void hv_battery_set_mode(bool on)
{
    batt_on = on;
    printf("battery: Windows battery %s\n", on ? "ON (SMC read-only, 60 s refresh)" : "off");
    memset(batt_regs, 0, sizeof(batt_regs));
    if (on) {
        batt_refresh();
        batt_last = get_ticks();
    }
}

void hv_battery_poll(void)
{
    if (!batt_on || ticks_to_msecs(get_ticks() - batt_last) < BATT_REFRESH_MS)
        return;
    batt_last = get_ticks();
    batt_refresh();
}

u32 hv_battery_reg(u64 offset)
{
    u64 i = offset / 4;
    return i < DCHID_BATT_NREGS ? batt_regs[i] : 0;
}
