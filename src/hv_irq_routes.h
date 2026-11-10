/* SPDX-License-Identifier: MIT */

#ifndef HV_IRQ_ROUTES_H
#define HV_IRQ_ROUTES_H

#ifdef HV_IRQ_ROUTES_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t u32;
#else
#include "types.h"
#endif

struct hv_irq_route {
    u32 hw_irq;
    u32 vintid;
    bool level;
};

#define HV_IRQ_DYNAMIC_ROUTE_CAPACITY 8u
#define HV_IRQ_SYNTHETIC_NVME_VINTID 64u
/* J613 AVHD virtual keyboard/trackpad (hv_dchid.c); never a hardware route. */
#define HV_IRQ_SYNTHETIC_AVHD_VINTID 65u
/* J613 battery events (GED ACPI0013 "BGED" in the DSDT, hv_battery.c); never a hardware route. */
#define HV_IRQ_SYNTHETIC_BATT_VINTID 66u
/* J613 Wi-Fi for the guest: INTA of the virtio-net 00:01.0 (DSDT PCI0 _PRT, hv_vnet.c). */
#define HV_IRQ_SYNTHETIC_VNET_VINTID 67u

const struct hv_irq_route *hv_irq_route_from_hw(u32 hw_irq);
const struct hv_irq_route *hv_irq_route_from_vintid(u32 vintid);
size_t hv_irq_route_count(void);
const struct hv_irq_route *hv_irq_route_at(size_t index);
bool hv_irq_route_resolve_incoming(u32 hw_irq, u32 reserved_vintid, u32 *vintid);
bool hv_irq_route_level_eoi_target(u32 vintid, bool enabled, u32 *hw_irq);
bool hv_irq_route_register(u32 hw_irq, u32 vintid, bool level);
void hv_irq_routes_reset_dynamic(void);
/* Pick the static route table for the SoC; T8103 (J313) is the default. */
void hv_irq_routes_select_t8122(bool t8122);

#endif
