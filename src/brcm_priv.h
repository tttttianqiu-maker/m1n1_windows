/* SPDX-License-Identifier: ISC */
/*
 * Register map and shared state for the m1n1 brcmfmac port (brcm.c, brcm_msgbuf.c).
 * Constants from Linux brcmfmac (pcie.c, chip.c, msgbuf.h, commonring.h) and
 * include/linux/bcma, Copyright (c) 2010-2014 Broadcom Corporation, ISC.
 */
#ifndef BRCM_PRIV_H
#define BRCM_PRIV_H

#ifdef BRCM_HOST_TEST
#include "brcm_host_test.h" /* tests/c: host build of the protocol code */
#else
#include "types.h"
#endif

/* PCI config space of the device */
#define BRCM_CFG_BAR0_WINDOW         0x80
#define BRCM_CFG_INTMASK             0x94
#define BRCM_CFG_SBMBX               0x98
#define BRCM_CFG_LINK_STATUS_CTRL    0xbc
#define BRCM_LINK_STATUS_CTRL_ASPM_ENAB 3
#define BRCM_CFGREG_TLCNTRL_5        0x814

/* BAR0: 4 KiB backplane window, 0x2000 = PCIe core registers */
#define BRCM_BAR0_REG_SIZE  0x1000
#define BRCM_BAR0_PCIE_ENUM 0x2000

/* PCIe2 core registers (BAR0 + 0x2000) */
#define BRCM_PCIE_INTMASK         0x24
#define BRCM_PCIE_MAILBOXINT      0x48
#define BRCM_PCIE_MAILBOXMASK     0x4c
#define BRCM_PCIE_CONFIGADDR      0x120
#define BRCM_PCIE_CONFIGDATA      0x124
#define BRCM_PCIE_H2D_MAILBOX_0   0x140
#define BRCM_PCIE_H2D_MAILBOX_1   0x144
#define BRCM_PCIE64_INTMASK       0xc14
#define BRCM_PCIE64_MAILBOXINT    0xc30
#define BRCM_PCIE64_MAILBOXMASK   0xc34
#define BRCM_PCIE64_H2D_MAILBOX_0 0xa20
#define BRCM_PCIE64_H2D_MAILBOX_1 0xa24

/* backplane */
#define BRCM_ENUM_BASE   0x18000000
#define BRCM_MAX_MEMSIZE (4 * 1024 * 1024)

/* ChipCommon */
#define CC_CAPABILITIES 0x04
#define CC_INTSTATUS    0x20
#define CC_WATCHDOG     0x80
#define CC_EROMPTR      0xfc
#define CC_WD_SSRESET_PCIE_F0_EN     0x10000000
#define CC_WD_SSRESET_PCIE_ALL_FN_EN 0x80000000
#define CC_WD_COUNTER_MASK           0x0fffffff
#define CC_WD_ENABLE_MASK            0xf0000000

/* core ids (linux/bcma/bcma.h) */
#define BCMA_CORE_CHIPCOMMON 0x800
#define BCMA_CORE_80211      0x812
#define BCMA_CORE_PMU        0x827
#define BCMA_CORE_PCIE2      0x83c
#define BCMA_CORE_ARM_CR4    0x83e
#define BCMA_CORE_GCI        0x840
#define BCMA_CORE_ARM_CA7    0x847
#define BCMA_CORE_SYS_MEM    0x849

/* AI wrapper */
#define BCMA_IOCTL            0x408
#define BCMA_IOCTL_CLK        0x1
#define BCMA_IOCTL_FGC        0x2
#define BCMA_RESET_CTL        0x800
#define BCMA_RESET_CTL_RESET  0x1
#define ARMCR4_BCMA_IOCTL_CPUHALT 0x20
#define D11_BCMA_IOCTL_PHYCLOCKEN 0x4
#define D11_BCMA_IOCTL_PHYRESET   0x8

/* EROM */
#define DMP_DESC_TYPE_MSK      0x0000000f
#define DMP_DESC_EMPTY         0x00000000
#define DMP_DESC_VALID         0x00000001
#define DMP_DESC_COMPONENT     0x00000001
#define DMP_DESC_MASTER_PORT   0x00000003
#define DMP_DESC_ADDRESS       0x00000005
#define DMP_DESC_ADDRSIZE_GT32 0x00000008
#define DMP_DESC_EOT           0x0000000f
#define DMP_COMP_PARTNUM       0x000fff00
#define DMP_COMP_PARTNUM_S     8
#define DMP_COMP_REVISION      0xff000000
#define DMP_COMP_REVISION_S    24
#define DMP_COMP_NUM_SWRAP     0x00f80000
#define DMP_COMP_NUM_SWRAP_S   19
#define DMP_COMP_NUM_MWRAP     0x0007c000
#define DMP_COMP_NUM_MWRAP_S   14
#define DMP_SLAVE_ADDR_BASE    0xfffff000
#define DMP_SLAVE_TYPE         0x000000c0
#define DMP_SLAVE_TYPE_S       6
#define DMP_SLAVE_TYPE_SLAVE   0
#define DMP_SLAVE_TYPE_SWRAP   2
#define DMP_SLAVE_TYPE_MWRAP   3
#define DMP_SLAVE_SIZE_TYPE    0x00000030
#define DMP_SLAVE_SIZE_TYPE_S  4
#define DMP_SLAVE_SIZE_4K      0
#define DMP_SLAVE_SIZE_8K      1
#define DMP_SLAVE_SIZE_DESC    3

/* RAM sizing */
#define ARMCR4_CAP          0x04
#define ARMCR4_BANKIDX      0x40
#define ARMCR4_BANKINFO     0x44
#define ARMCR4_TCBANB_MASK  0x0f
#define ARMCR4_TCBBNB_MASK  0xf0
#define ARMCR4_BSZ_MASK     0x7f
#define ARMCR4_BSZ_MULT     8192
#define ARMCR4_BLK_1K_MASK  0x200
#define SOCRAM_COREINFO     0x00
#define SOCRAM_BANKIDX      0x10
#define SOCRAM_BANKINFO     0x40
#define SOCRAM_BANKINFO_SZMASK 0x7f
#define SOCRAM_BANKINFO_SZBASE 8192
#define SOCRAM_MEMTYPE_RAM  0
#define SOCRAM_BANKIDX_MEMTYPE_SHIFT 8
#define SYSMEM_SRCI_ROMNB_MASK      0x3e0
#define SYSMEM_SRCI_ROMNB_SHIFT     5
#define SYSMEM_SRCI_SRNB_MASK       0x1f
#define SYSMEM_SRCI_NEW_ROMNB_MASK  0xff000000
#define SYSMEM_SRCI_NEW_ROMNB_SHIFT 24
#define SYSMEM_SRCI_NEW_SRNB_MASK   0xff0000
#define SYSMEM_SRCI_NEW_SRNB_SHIFT  16

/* firmware download */
#define RAMSIZE_MAGIC      0x534d4152 /* SMAR */
#define RAMSIZE_OFFSET     0x6c
#define BL_HEAP_START_GAP  0x1000
#define BL_HEAP_SIZE       0x10000
#define RANDOM_SEED_MAGIC  0xfeedc0de
#define RANDOM_SEED_LENGTH 0x100
#define FW_SIG_MAGIC       0xfeedfe51
#define MEMMAP_MAGIC       0xfeedfe53
#define VSTATUS_MAGIC      0xfeedfe54
#define VSTATUS_SIZE       0x28
#define END_MAGIC          0xfeed0e2d

/* shared RAM structure */
#define SHARED_FLAGS               0
#define SHARED_CONSOLE_ADDR        20
#define SHARED_MAX_RXBUFPOST       34
#define SHARED_RX_DATAOFFSET       36
#define SHARED_HTOD_MB_DATA_ADDR   40
#define SHARED_DTOH_MB_DATA_ADDR   44
#define SHARED_RING_INFO_ADDR      48
#define SHARED_DMA_SCRATCH_LEN     52
#define SHARED_DMA_SCRATCH_ADDR    56
#define SHARED_DMA_RINGUPD_LEN     64
#define SHARED_DMA_RINGUPD_ADDR    68
#define SHARED_FLAGS2              80
#define SHARED_HOST_CAP            84
#define SHARED_FLAGS3              108
#define SHARED_HOST_CAP2           112
#define SHARED_VERSION_MASK        0x00ff
#define SHARED_DMA_INDEX           0x10000
#define SHARED_DMA_2B_IDX          0x100000
#define SHARED_USE_MAILBOX         0x2000000
#define SHARED_HOSTRDY_DB1         0x10000000
#define SHARED_DAR                 0x80000000
#define HOSTCAP_H2D_ENABLE_HOSTRDY 0x400
#define HOSTCAP_DS_NO_OOB_DW       0x1000
#define HOSTCAP_H2D_DAR            0x10000

#define CONSOLE_BUFADDR  8
#define CONSOLE_BUFSIZE  12
#define CONSOLE_WRITEIDX 16

struct brcm_core {
    u16 id;
    u8 rev;
    u32 base, wrap;
};

struct brcm_console {
    u32 base, buf, size, ridx;
    char line[256];
    u32 len;
};

struct brcm {
    u64 cfg, regs, tcm, tcm_size;
    struct brcm_core cores[40];
    int ncores;
    u16 chip;
    u8 chiprev;
    bool reginfo64, attached, skip_reset_vector;
    u32 cc_caps, rambase, ramsize, fw_size;
    /* shared RAM */
    u32 shared, flags, flags2, flags3, rx_dataoffset, htod_mb, dtoh_mb, ring_info;
    u16 max_rxbufpost;
    u8 version, dma_idx_sz;
    bool mb_via_ctl;
    struct brcm_console con;
};

extern struct brcm brcm;

u32 bp_read32(u32 addr);
void bp_write32(u32 addr, u32 val);
u32 pcie_read32(u32 reg);
void pcie_write32(u32 reg, u32 val);
u32 tcm_read32(u32 off);
void tcm_write32(u32 off, u32 val);
u16 tcm_read16(u32 off);
void tcm_write16(u32 off, u16 val);
void tcm_copy_to(u32 off, const void *src, u32 len);
void tcm_copy_from(u32 off, void *dst, u32 len);
struct brcm_core *brcm_core(u16 id);

#endif
