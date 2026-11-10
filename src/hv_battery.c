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
 * starts, then every 60 s from the hypervisor tick. A failed or implausible
 * read keeps the last good values (dchid_batt_update): a battery that drops
 * to 0 % on battery power would make Windows hibernate or shut down at once.
 *
 * The guest reads the registers through ACPI (DSDT BAT0/ADP1 OperationRegion
 * on the AVHD page); the read counters below show how often Windows polls
 * _STA/_PSR, _BST and _BIX. Changes are also signalled: DSDT device BGED is an
 * ACPI Generic Event Device (ACPI0013, Windows 11 Arm handles it) on edge vSPI
 * HV_IRQ_SYNTHETIC_BATT_VINTID; its _EVT reads DCHID_BATT_EVT (read-to-clear)
 * and notifies BAT0/ADP1, so Windows re-reads at once instead of polling.
 */
#include "hv_battery.h"
#include "dchid_proto.h"
#include "hv_irq_routes.h"
#include "smc.h"
#include "string.h"
#include "utils.h" /* soc.h -> config.h: ENABLE_VGIC_MODULE, before hv_vgic.h */
#include "hv_vgic.h"

#define BATT_REFRESH_MS 60000

#define KEY(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

static bool batt_on;
static struct dchid_batt batt;
static u32 batt_fail, batt_held_logs, batt_last_flags;
static u64 batt_last, batt_boot;

/* guest reads per register (MMIO trap path, any CPU; diagnostic only) */
static u32 batt_reads[DCHID_BATT_NREGS];
static u32 batt_reads_logged[4];
static u64 batt_last_bst;

/* GED events: set by the refresh (interruptible CPU), taken by the guest's _EVT read */
static DECLARE_SPINLOCK(batt_lock);
static u32 batt_events, batt_injected;
static bool batt_irq_pending, batt_ged_seen;

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

    u32 before[DCHID_BATT_NREGS];
    memcpy(before, batt.regs, sizeof(before));
    if (!dchid_batt_update(&batt, &r)) {
        if (batt_held_logs++ < 8)
            printf("battery: read kept back (BUIC %s %u, FCC %s %u, CHIS %s), Windows keeps "
                   "%u%% (held %u)\n",
                   r.ok_buic ? "ok" : "FAILED", r.buic, r.ok_fcc ? "ok" : "FAILED", r.fcc,
                   r.ok_chis ? "ok" : "FAILED", batt.regs[DCHID_BATT_PCT], batt.held);
        return;
    }

    u32 ev = dchid_batt_events(before, batt.regs);
    if (ev) {
        spin_lock(&batt_lock);
        batt_events |= ev;
        spin_unlock(&batt_lock);
    }

    const u32 *regs = batt.regs;
    u32 flags = regs[DCHID_BATT_FLG];
    if (batt.seq == 1 || !(batt.seq % 30) || flags != batt_last_flags)
        printf("battery: #%u %u%% %s%s%s fcc %u mAh design %u mAh %d mA %u mV cycles %u cells %u "
               "(keys %d%d%d%d%d%d%d%d)\n",
               batt.seq, regs[DCHID_BATT_PCT], flags & DCHID_BATT_PRESENT ? "" : "NOT PRESENT ",
               flags & DCHID_BATT_AC ? "AC " : "battery ",
               flags & DCHID_BATT_CHARGING ? "charging" : "", regs[DCHID_BATT_FCAP],
               regs[DCHID_BATT_DCAP], r.cur, r.volt, r.cyc, r.cells, r.ok_buic, r.ok_fcc,
               r.ok_dcap, r.ok_volt, r.ok_cur, r.ok_cyc, r.ok_chis, r.ok_cells);
    batt_last_flags = flags;
}

static void batt_log_reads(void)
{
    /* BFLG: _STA, _PSR and _BST; BRCP: _BST only; BDCP: _BIX/_BIF only; BEVT: _EVT */
    u32 now[4] = {batt_reads[DCHID_BATT_FLG], batt_reads[DCHID_BATT_RCAP],
                  batt_reads[DCHID_BATT_DCAP], batt_reads[DCHID_BATT_EVT]};

    if (!memcmp(now, batt_reads_logged, sizeof(now)))
        return;
    printf("battery: Windows reads after %lu s: flags %u, _BST %u (last %lu s ago), _BIX %u, "
           "_EVT %u of %u events sent\n",
           (unsigned long)(ticks_to_msecs(get_ticks() - batt_boot) / 1000), now[0], now[1],
           batt_last_bst ? (unsigned long)(ticks_to_msecs(get_ticks() - batt_last_bst) / 1000)
                         : 0ul,
           now[2], now[3], batt_injected);
    memcpy(batt_reads_logged, now, sizeof(now));
}

/* Raise the GED interrupt for pending events once Windows has connected it. */
static void batt_kick(void)
{
    u32 vintid = HV_IRQ_SYNTHETIC_BATT_VINTID;

    if (!hv_vgic3_irq_enabled(vintid))
        return;
    if (!batt_ged_seen) {
        batt_ged_seen = true;
        printf("battery: Windows connected the battery event interrupt (GED, vINTID %u)\n",
               vintid);
    }
    if (!batt_events || batt_irq_pending || hv_vgic3_get_free_lr() < 0)
        return;
    batt_irq_pending = true;
    batt_injected++;
    hv_vgic3_inject_irq(vintid, hv_vgic3_get_priority(vintid), false, true, false, 0);
}

void hv_battery_set_mode(bool on)
{
    batt_on = on;
    printf("battery: Windows battery %s\n", on ? "ON (SMC read-only, 60 s refresh)" : "off");
    memset(&batt, 0, sizeof(batt));
    memset(batt_reads, 0, sizeof(batt_reads));
    memset(batt_reads_logged, 0, sizeof(batt_reads_logged));
    batt_fail = batt_held_logs = batt_last_flags = 0;
    batt_last_bst = 0;
    batt_events = batt_injected = 0;
    batt_irq_pending = batt_ged_seen = false;
    if (on) {
        batt_refresh();
        batt_last = batt_boot = get_ticks();
    }
}

void hv_battery_poll(void)
{
    if (!batt_on)
        return;
    batt_kick();
    if (ticks_to_msecs(get_ticks() - batt_last) < BATT_REFRESH_MS)
        return;
    batt_last = get_ticks();
    batt_refresh();
    batt_kick();
    batt_log_reads();
}

u32 hv_battery_reg(u64 offset)
{
    u64 i = offset / 4;

    if (i >= DCHID_BATT_NREGS)
        return 0;
    if (i == DCHID_BATT_EVT) {
        u32 ev;
        spin_lock(&batt_lock);
        ev = batt_events;
        batt_events = 0;
        batt_irq_pending = false;
        spin_unlock(&batt_lock);
        batt_reads[i]++;
        return batt_on ? ev : 0;
    }
    if (batt_on) {
        if (!batt_reads[i]++ && (i == DCHID_BATT_FLG || i == DCHID_BATT_RCAP))
            printf("battery: Windows read %s the first time (%u%%)\n",
                   i == DCHID_BATT_FLG ? "the battery flags (_STA)" : "_BST",
                   batt.regs[DCHID_BATT_PCT]);
        if (i == DCHID_BATT_RCAP)
            batt_last_bst = get_ticks();
    }
    return batt.regs[i];
}
