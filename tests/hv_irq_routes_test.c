#include <assert.h>
#include <stdio.h>

#include "../src/hv_irq_routes.h"

int main(void)
{
    const struct hv_irq_route *route;
    u32 hw_irq = 0;
    u32 vintid = 0;

    hv_irq_routes_reset_dynamic();

    route = hv_irq_route_from_hw(857);
    assert(route != NULL);
    assert(route->hw_irq == 857);
    assert(route->vintid == 857);
    assert(route->level);

    assert(hv_irq_route_from_vintid(857) == route);
    assert(hv_irq_route_from_hw(64) == NULL);
    assert(hv_irq_route_from_vintid(64) == NULL);

    /* Guest EOIs may only unmask a physical line through an explicit route. */
    assert(!hv_irq_route_level_eoi_target(64, true, &hw_irq));
    assert(!hv_irq_route_level_eoi_target(857, false, &hw_irq));
    assert(hv_irq_route_level_eoi_target(857, true, &hw_irq));
    assert(hw_irq == 857);

    /* A raw AIC IRQ must not alias a synthetic guest-only interrupt. */
    assert(!hv_irq_route_resolve_incoming(64, 64, &vintid));
    assert(hv_irq_route_resolve_incoming(123, 64, &vintid));
    assert(vintid == 123);
    assert(hv_irq_route_resolve_incoming(857, 64, &vintid));
    assert(vintid == 857);

    /* Launch-contract capture enumerates the installed route table itself. */
    assert(hv_irq_route_count() == 1);
    route = hv_irq_route_at(0);
    assert(route != NULL);
    assert(route->hw_irq == 857);
    assert(route->vintid == 857);
    assert(route->level);
    assert(hv_irq_route_at(hv_irq_route_count()) == NULL);

    /* Dynamic physical routes are bounded and cannot shadow either namespace. */
    assert(hv_irq_route_register(333, 865, true));
    assert(hv_irq_route_from_hw(333)->vintid == 865);
    assert(hv_irq_route_from_vintid(865)->hw_irq == 333);
    assert(!hv_irq_route_register(334, 865, true));
    assert(!hv_irq_route_register(333, 866, true));
    assert(!hv_irq_route_register(857, 900, true));
    assert(!hv_irq_route_register(334, 31, true));
    assert(!hv_irq_route_register(334, 1020, true));
    assert(!hv_irq_route_register(334, HV_IRQ_SYNTHETIC_NVME_VINTID, true));

    for (u32 i = 1; i < HV_IRQ_DYNAMIC_ROUTE_CAPACITY; i++)
        assert(hv_irq_route_register(333 + i, 865 + i, true));
    assert(!hv_irq_route_register(700, 900, true));
    assert(hv_irq_route_count() == 1 + HV_IRQ_DYNAMIC_ROUTE_CAPACITY);

    hv_irq_routes_reset_dynamic();
    assert(hv_irq_route_count() == 1);
    assert(hv_irq_route_from_hw(333) == NULL);

    puts("hv_irq_routes_test: ok");
    /* T8122 (J613): physical AIC 1030 lands on the same guest INTID 857. */
    hv_irq_routes_reset_dynamic();
    hv_irq_routes_select_t8122(true);
    assert(hv_irq_route_count() == 1);
    route = hv_irq_route_from_hw(1030);
    assert(route != NULL && route->vintid == 857 && route->level);
    assert(hv_irq_route_from_vintid(857) == route);
    assert(hv_irq_route_from_hw(857) == NULL);
    assert(hv_irq_route_level_eoi_target(857, true, &hw_irq));
    assert(hw_irq == 1030);
    for (size_t i = 0; i < hv_irq_route_count(); i++)
        assert(hv_irq_route_at(i)->vintid >= 32 && hv_irq_route_at(i)->vintid <= 1019);
    hv_irq_routes_select_t8122(false);
    assert(hv_irq_route_from_hw(1030) == NULL);

    return 0;
}
