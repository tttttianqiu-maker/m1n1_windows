/* SPDX-License-Identifier: MIT */

#ifndef USB_H
#define USB_H

#include "iodev.h"
#include "types.h"
#include "usb_dwc3.h"

dwc3_dev_t *usb_bringup(u32 idx);

void usb_init(void);
void usb_hpm_restore_irqs(bool force);
void usb_guest_port_power_up(void);
void usb_guest_port_print_status(void);
void usb_guest_port_phy_print(const char *tag);
void usb_guest_port_phy_refresh(void);
void usb_guest_port_phy_host_mode(void);
int usb_guest_port_reset_ordered(u64 dwc3);
void usb_iodev_init(void);
void usb_iodev_shutdown(void);
void usb_iodev_vuart_setup(iodev_id_t iodev);

#endif
