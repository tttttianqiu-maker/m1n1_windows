/* SPDX-License-Identifier: MIT */
#ifndef HV_MTP_H
#define HV_MTP_H

#include "types.h"

/*
 * MTP coprocessor (keyboard/trackpad on M2/M3 laptops) owned by the hypervisor.
 * m1n1 boots the MTP over RTKit and keeps servicing its mailbox
 * (syslog/crashlog/ioreport buffers); hv_dchid.c speaks DockChannel HID to it
 * and hands HID reports to the guest.
 */
bool hv_mtp_init(void);
void hv_mtp_poll(void);
bool hv_mtp_running(void);
bool hv_mtp_map(void *buf, size_t size, u64 *dva);

#endif
