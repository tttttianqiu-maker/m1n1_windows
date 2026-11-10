/* SPDX-License-Identifier: MIT */

#ifndef SMC_H
#define SMC_H

#include "asc.h"
#include "rtkit.h"
#include "types.h"

typedef struct smc_dev smc_dev_t;

int smc_write_u32(smc_dev_t *smc, u32 key, u32 value);
/* Read a key of 1-4 bytes (little-endian, as returned inline by the SMC). 0 on success. */
int smc_read(smc_dev_t *smc, u32 key, void *buf, u32 size);

smc_dev_t *smc_init(void);
void smc_shutdown(smc_dev_t *smc);

#endif
