/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef PCIE_H
#define PCIE_H

#include "types.h"

int pcie_init(void);
int pcie_shutdown(void);
int pcie_get_port(u32 port, u64 *ecam, u64 *port_base, u64 *port_phy_base);

#endif
