/* SPDX-License-Identifier: MIT */
#ifndef HV_BATTERY_H
#define HV_BATTERY_H

#include "types.h"

/*
 * J613 battery for Windows (`j613 battery on`): read-only SMC battery keys,
 * served to the guest's ACPI BAT0/ADP1 through the AVHD page at +0x100.
 */
void hv_battery_set_mode(bool on);
void hv_battery_poll(void);
u32 hv_battery_reg(u64 offset);

#endif
