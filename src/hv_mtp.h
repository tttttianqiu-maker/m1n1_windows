/* SPDX-License-Identifier: MIT */
#ifndef HV_MTP_H
#define HV_MTP_H

#include "types.h"

/*
 * MTP coprocessor (keyboard/trackpad on M2/M3 laptops) owned by the hypervisor.
 * The guest only talks to the DockChannel HID FIFO; m1n1 boots the MTP over
 * RTKit and keeps servicing its mailbox (syslog/crashlog/ioreport buffers).
 */
bool hv_mtp_init(void);
void hv_mtp_poll(void);
bool hv_mtp_running(void);

#endif
