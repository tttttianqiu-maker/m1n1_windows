/* SPDX-License-Identifier: MIT */
#ifndef HV_KBDLIGHT_H
#define HV_KBDLIGHT_H

#include "types.h"

/* J613 keyboard backlight (opt-in, `j613 kbdlight on`); see hv_kbdlight.c. */
int hv_kbdlight_setup(u32 level);
bool hv_kbdlight_enabled(void);
void hv_kbdlight_step(int delta);

#endif
