/* SPDX-License-Identifier: MIT */

#include "hv_irq_routes.h"

/*
 * Physical AIC numbers and guest GIC INTIDs are separate namespaces. Keep the
 * handoff explicit even when a route currently uses the same number in both.
 * Guest INTIDs must be GIC SPIs (32..1019); physical AIC numbers may be larger.
 *
 * usb-drd1 (the Type-C port not occupied by the m1n1 proxy) is guest INTID 857
 * on every board, so the XHC1 ACPI node is the same for Windows on M1 and M3.
 */
static const struct hv_irq_route routes_t8103[] = {
    {.hw_irq = 857, .vintid = 857, .level = true},   // J313 usb-drd1
};
static const struct hv_irq_route routes_t8122[] = {
    {.hw_irq = 1030, .vintid = 857, .level = true},  // J613 dwc3_1 (usb@b02280000), t8122.dtsi
};

static const struct hv_irq_route *routes = routes_t8103;
static size_t static_route_count = sizeof(routes_t8103) / sizeof(routes_t8103[0]);
#define STATIC_ROUTE_COUNT static_route_count

void hv_irq_routes_select_t8122(bool t8122)
{
    if (t8122) {
        routes = routes_t8122;
        static_route_count = sizeof(routes_t8122) / sizeof(routes_t8122[0]);
    } else {
        routes = routes_t8103;
        static_route_count = sizeof(routes_t8103) / sizeof(routes_t8103[0]);
    }
}

static struct hv_irq_route dynamic_routes[HV_IRQ_DYNAMIC_ROUTE_CAPACITY];
static size_t dynamic_route_count;

size_t hv_irq_route_count(void)
{
    return STATIC_ROUTE_COUNT + dynamic_route_count;
}

const struct hv_irq_route *hv_irq_route_at(size_t index)
{
    if (index < STATIC_ROUTE_COUNT)
        return &routes[index];
    index -= STATIC_ROUTE_COUNT;
    if (index >= dynamic_route_count)
        return NULL;
    return &dynamic_routes[index];
}

const struct hv_irq_route *hv_irq_route_from_hw(u32 hw_irq)
{
    for (size_t i = 0; i < hv_irq_route_count(); i++) {
        const struct hv_irq_route *route = hv_irq_route_at(i);
        if (route->hw_irq == hw_irq)
            return route;
    }

    return NULL;
}

const struct hv_irq_route *hv_irq_route_from_vintid(u32 vintid)
{
    for (size_t i = 0; i < hv_irq_route_count(); i++) {
        const struct hv_irq_route *route = hv_irq_route_at(i);
        if (route->vintid == vintid)
            return route;
    }

    return NULL;
}

bool hv_irq_route_resolve_incoming(u32 hw_irq, u32 reserved_vintid, u32 *vintid)
{
    const struct hv_irq_route *route = hv_irq_route_from_hw(hw_irq);
    u32 candidate = route ? route->vintid : hw_irq;

    if (!vintid || candidate == reserved_vintid || candidate == HV_IRQ_SYNTHETIC_AVHD_VINTID)
        return false;

    *vintid = candidate;
    return true;
}

bool hv_irq_route_level_eoi_target(u32 vintid, bool enabled, u32 *hw_irq)
{
    const struct hv_irq_route *route = hv_irq_route_from_vintid(vintid);

    if (!route || !route->level || !enabled || !hw_irq)
        return false;

    *hw_irq = route->hw_irq;
    return true;
}

bool hv_irq_route_register(u32 hw_irq, u32 vintid, bool level)
{
    if (hw_irq < 32 || hw_irq > 1019 || vintid < 32 || vintid > 1019 ||
        vintid == HV_IRQ_SYNTHETIC_NVME_VINTID || vintid == HV_IRQ_SYNTHETIC_AVHD_VINTID ||
        dynamic_route_count >= HV_IRQ_DYNAMIC_ROUTE_CAPACITY ||
        hv_irq_route_from_hw(hw_irq) || hv_irq_route_from_vintid(vintid))
        return false;

    dynamic_routes[dynamic_route_count++] =
        (struct hv_irq_route){.hw_irq = hw_irq, .vintid = vintid, .level = level};
    return true;
}

void hv_irq_routes_reset_dynamic(void)
{
    dynamic_route_count = 0;
}
