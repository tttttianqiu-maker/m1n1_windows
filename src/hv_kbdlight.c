/* SPDX-License-Identifier: MIT */
/*
 * J613 keyboard backlight, opt-in (`j613 kbdlight on` -> P_HV_KBD_BACKLIGHT before the
 * guest starts). Windows has no keyboard backlight control without a vendor driver, so
 * m1n1 drives the LEDs itself and takes Fn+F5 / Fn+F6 for darker / brighter
 * (dchid_kbd_fn_backlight; those keys never reach Windows while this is on).
 *
 * Hardware, all from Linux: the Apple Silicon laptops drive the keyboard LEDs from the
 * fpwm1 PWM (t8103-j313.dts / t8112-j413.dts: pwm-leds "kbd_backlight" on &fpwm1,
 * 40 us period); t8122.dtsi has fpwm1 at 0x2a1044000 (ps_fpwm1, 24 MHz clkref).
 * Register layout from drivers/pwm/pwm-apple.c. Used only if the ADT has
 * /arm-io/fpwm1 at exactly that address and its power domain comes up.
 */
#include "hv_kbdlight.h"
#include "adt.h"
#include "pmgr.h"
#include "utils.h"

#define FPWM_PATH     "/arm-io/fpwm1"
#define FPWM_T8122    0x2a1044000UL
#define FPWM_CONTROL  0x00
#define FPWM_OFF      0x18
#define FPWM_ON       0x1c
#define FPWM_CTRL_ENABLE        BIT(0)
#define FPWM_CTRL_UPDATE        BIT(5)
#define FPWM_CTRL_OUTPUT_ENABLE BIT(14)
#define FPWM_PERIOD   960 /* 40 us at 24 MHz, as Linux */

/* on-cycles per step, roughly perceptual; 0 = off */
static const u16 kbl_duty[] = {0, 10, 30, 70, 140, 260, 440, 680, 960};
#define KBL_STEPS ((int)ARRAY_SIZE(kbl_duty))

static u64 fpwm;
static int kbl_level;

static void kbl_apply(void)
{
    u32 on = kbl_duty[kbl_level];

    if (!on) {
        write32(fpwm + FPWM_CONTROL, 0);
        return;
    }
    write32(fpwm + FPWM_ON, on);
    write32(fpwm + FPWM_OFF, FPWM_PERIOD - on);
    write32(fpwm + FPWM_CONTROL, FPWM_CTRL_ENABLE | FPWM_CTRL_OUTPUT_ENABLE | FPWM_CTRL_UPDATE);
}

int hv_kbdlight_setup(u32 level)
{
    int path[8];
    u64 base = 0, size = 0;

    fpwm = 0;
    if (chip_id != T8122) {
        printf("kbdlight: only wired up for T8122 (J613)\n");
        return -1;
    }
    if (adt_path_offset_trace(adt, FPWM_PATH, path) < 0 ||
        adt_get_reg(adt, path, "reg", 0, &base, &size) < 0) {
        printf("kbdlight: no %s in the ADT, off\n", FPWM_PATH);
        return -1;
    }
    if (base != FPWM_T8122) {
        printf("kbdlight: %s at 0x%lx, Linux t8122.dtsi says 0x%lx: off\n", FPWM_PATH, base,
               FPWM_T8122);
        return -1;
    }
    if (pmgr_adt_power_enable(FPWM_PATH) < 0) {
        printf("kbdlight: cannot power up %s, off\n", FPWM_PATH);
        return -1;
    }
    printf("kbdlight: %s control was 0x%x on %u off %u\n", FPWM_PATH,
           read32(base + FPWM_CONTROL), read32(base + FPWM_ON), read32(base + FPWM_OFF));
    fpwm = base;
    kbl_level = level < (u32)KBL_STEPS ? (int)level : KBL_STEPS - 1;
    kbl_apply();
    printf("kbdlight: on, level %d/%d (Fn+F5 darker, Fn+F6 brighter)\n", kbl_level,
           KBL_STEPS - 1);
    return 0;
}

bool hv_kbdlight_enabled(void)
{
    return fpwm != 0;
}

void hv_kbdlight_step(int delta)
{
    if (!fpwm)
        return;
    int next = kbl_level + delta;
    if (next < 0 || next >= KBL_STEPS)
        return;
    kbl_level = next;
    kbl_apply();
    printf("kbdlight: level %d/%d\n", kbl_level, KBL_STEPS - 1);
}
