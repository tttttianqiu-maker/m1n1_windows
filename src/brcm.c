/* SPDX-License-Identifier: ISC */
/*
 * Broadcom FullMAC PCIe chip bring-up for m1n1 (J613 built-in Wi-Fi, BCM4388).
 *
 * A reduced port of Linux drivers/net/wireless/broadcom/brcm80211/brcmfmac (chip.c,
 * pcie.c): backplane access through the BAR0 window, EROM core scan, AI core reset,
 * RAM sizing, Apple OTP (module/vendor/version), firmware + NVRAM + random-seed footer
 * download, ARM start, shared-RAM handshake and the firmware console. Rings, msgbuf and
 * the 802.11 control path live in brcm_msgbuf.c.
 *
 * Copyright (c) 2010-2014 Broadcom Corporation (brcmfmac)
 * Copyright The Asahi Linux Contributors (Apple OTP/seed/signature support in brcmfmac)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "brcm.h"
#include "brcm_priv.h"
#include "string.h"
#include "utils.h"
#include "wlan.h"

struct brcm brcm;

/* ---------- raw access ---------- */

static void bp_window(u32 addr)
{
    write32(brcm.cfg + BRCM_CFG_BAR0_WINDOW, addr & ~(BRCM_BAR0_REG_SIZE - 1));
}

u32 bp_read32(u32 addr)
{
    bp_window(addr);
    return read32(brcm.regs + (addr & (BRCM_BAR0_REG_SIZE - 1)));
}

void bp_write32(u32 addr, u32 val)
{
    bp_window(addr);
    write32(brcm.regs + (addr & (BRCM_BAR0_REG_SIZE - 1)), val);
}

u32 pcie_read32(u32 reg)
{
    return read32(brcm.regs + BRCM_BAR0_PCIE_ENUM + reg);
}

void pcie_write32(u32 reg, u32 val)
{
    write32(brcm.regs + BRCM_BAR0_PCIE_ENUM + reg, val);
}

u32 tcm_read32(u32 off)
{
    return read32(brcm.tcm + off);
}

void tcm_write32(u32 off, u32 val)
{
    write32(brcm.tcm + off, val);
}

u16 tcm_read16(u32 off)
{
    return read16(brcm.tcm + off);
}

void tcm_write16(u32 off, u16 val)
{
    write16(brcm.tcm + off, val);
}

static u8 tcm_read8(u32 off)
{
    return read8(brcm.tcm + off);
}

/* 32-bit device writes only; len may be unaligned (tail padded with zeroes) */
void tcm_copy_to(u32 off, const void *src, u32 len)
{
    const u8 *s = src;
    u32 i = 0;
    for (; i + 4 <= len; i += 4) {
        u32 v;
        memcpy(&v, s + i, 4);
        write32(brcm.tcm + off + i, v);
    }
    if (i < len) {
        u32 v = 0;
        memcpy(&v, s + i, len - i);
        write32(brcm.tcm + off + i, v);
    }
}

void tcm_copy_from(u32 off, void *dst, u32 len)
{
    u8 *d = dst;
    for (u32 i = 0; i < len; i += 4) {
        u32 v = read32(brcm.tcm + off + i);
        memcpy(d + i, &v, min(4u, len - i));
    }
}

static void tcm_zero(u32 off, u32 len)
{
    for (u32 i = 0; i < len; i += 4)
        write32(brcm.tcm + off + i, 0);
}

/* ---------- cores ---------- */

struct brcm_core *brcm_core(u16 id)
{
    for (int i = 0; i < brcm.ncores; i++)
        if (brcm.cores[i].id == id)
            return &brcm.cores[i];
    return NULL;
}

static void select_core(u16 id)
{
    struct brcm_core *c = brcm_core(id);
    if (!c) {
        printf("wlan: core 0x%x missing\n", id);
        return;
    }
    write32(brcm.cfg + BRCM_CFG_BAR0_WINDOW, c->base);
    if (read32(brcm.cfg + BRCM_CFG_BAR0_WINDOW) != c->base)
        write32(brcm.cfg + BRCM_CFG_BAR0_WINDOW, c->base);
}

static bool ai_iscoreup(struct brcm_core *c)
{
    u32 io = bp_read32(c->wrap + BCMA_IOCTL);
    u32 rst = bp_read32(c->wrap + BCMA_RESET_CTL);
    return (io & (BCMA_IOCTL_FGC | BCMA_IOCTL_CLK)) == BCMA_IOCTL_CLK && !(rst & BCMA_RESET_CTL_RESET);
}

static void ai_coredisable(struct brcm_core *c, u32 prereset, u32 reset)
{
    if (!(bp_read32(c->wrap + BCMA_RESET_CTL) & BCMA_RESET_CTL_RESET)) {
        bp_write32(c->wrap + BCMA_IOCTL, prereset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
        bp_read32(c->wrap + BCMA_IOCTL);
        bp_write32(c->wrap + BCMA_RESET_CTL, BCMA_RESET_CTL_RESET);
        udelay(20);
        for (int i = 0; i < 300 && bp_read32(c->wrap + BCMA_RESET_CTL) != BCMA_RESET_CTL_RESET; i++)
            udelay(1);
    }
    bp_write32(c->wrap + BCMA_IOCTL, reset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
    bp_read32(c->wrap + BCMA_IOCTL);
}

static void ai_resetcore(struct brcm_core *c, u32 prereset, u32 reset, u32 postreset)
{
    ai_coredisable(c, prereset, reset);
    for (int count = 0; bp_read32(c->wrap + BCMA_RESET_CTL) & BCMA_RESET_CTL_RESET; count++) {
        bp_write32(c->wrap + BCMA_RESET_CTL, 0);
        if (count > 50)
            break;
        udelay(50);
    }
    bp_write32(c->wrap + BCMA_IOCTL, postreset | BCMA_IOCTL_CLK);
    bp_read32(c->wrap + BCMA_IOCTL);
}

/* ---------- EROM ---------- */

static u32 erom_desc(u32 *addr, u8 *type)
{
    u32 val = bp_read32(*addr);
    *addr += 4;
    if (type) {
        *type = val & DMP_DESC_TYPE_MSK;
        if ((*type & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS)
            *type = DMP_DESC_ADDRESS;
    }
    return val;
}

static int erom_regaddr(u32 *addr, u32 *regbase, u32 *wrapbase)
{
    u8 desc, wraptype;
    u32 val = erom_desc(addr, &desc);

    *regbase = *wrapbase = 0;
    if (desc == DMP_DESC_MASTER_PORT) {
        wraptype = DMP_SLAVE_TYPE_MWRAP;
    } else if (desc == DMP_DESC_ADDRESS) {
        *addr -= 4;
        wraptype = DMP_SLAVE_TYPE_SWRAP;
    } else {
        *addr -= 4;
        return -1;
    }

    do {
        do {
            val = erom_desc(addr, &desc);
            if (desc == DMP_DESC_EOT) {
                *addr -= 4;
                return -1;
            }
        } while (desc != DMP_DESC_ADDRESS && desc != DMP_DESC_COMPONENT);
        if (desc == DMP_DESC_COMPONENT) {
            *addr -= 4;
            return 0;
        }
        if (val & DMP_DESC_ADDRSIZE_GT32)
            erom_desc(addr, NULL);
        u8 sztype = (val & DMP_SLAVE_SIZE_TYPE) >> DMP_SLAVE_SIZE_TYPE_S;
        if (sztype == DMP_SLAVE_SIZE_DESC) {
            u32 szdesc = erom_desc(addr, NULL);
            if (szdesc & DMP_DESC_ADDRSIZE_GT32)
                erom_desc(addr, NULL);
        }
        if (sztype != DMP_SLAVE_SIZE_4K && sztype != DMP_SLAVE_SIZE_8K)
            continue;
        u8 stype = (val & DMP_SLAVE_TYPE) >> DMP_SLAVE_TYPE_S;
        if (*regbase == 0 && stype == DMP_SLAVE_TYPE_SLAVE)
            *regbase = val & DMP_SLAVE_ADDR_BASE;
        if (*wrapbase == 0 && stype == wraptype)
            *wrapbase = val & DMP_SLAVE_ADDR_BASE;
    } while (*regbase == 0 || *wrapbase == 0);
    return 0;
}

static int erom_scan(void)
{
    u32 addr = bp_read32(BRCM_ENUM_BASE + CC_EROMPTR);
    u8 type = 0;

    brcm.ncores = 0;
    for (int guard = 0; type != DMP_DESC_EOT && guard < 4096; guard++) {
        u32 val = erom_desc(&addr, &type);
        if (!(val & DMP_DESC_VALID) || type == DMP_DESC_EMPTY || type != DMP_DESC_COMPONENT)
            continue;
        u16 id = (val & DMP_COMP_PARTNUM) >> DMP_COMP_PARTNUM_S;
        val = erom_desc(&addr, &type);
        if ((val & DMP_DESC_TYPE_MSK) != DMP_DESC_COMPONENT)
            return -1;
        u8 nmw = (val & DMP_COMP_NUM_MWRAP) >> DMP_COMP_NUM_MWRAP_S;
        u8 nsw = (val & DMP_COMP_NUM_SWRAP) >> DMP_COMP_NUM_SWRAP_S;
        u8 rev = (val & DMP_COMP_REVISION) >> DMP_COMP_REVISION_S;
        if (nmw + nsw == 0 && id != BCMA_CORE_PMU && id != BCMA_CORE_GCI)
            continue;
        u32 base, wrap;
        if (erom_regaddr(&addr, &base, &wrap))
            continue;
        if (brcm.ncores >= (int)ARRAY_SIZE(brcm.cores))
            break;
        brcm.cores[brcm.ncores++] = (struct brcm_core){.id = id, .rev = rev, .base = base, .wrap = wrap};
    }
    return brcm.ncores ? 0 : -1;
}

/* ---------- passive / active ---------- */

static u16 arm_core_id(void)
{
    if (brcm_core(BCMA_CORE_ARM_CR4))
        return BCMA_CORE_ARM_CR4;
    if (brcm_core(BCMA_CORE_ARM_CA7))
        return BCMA_CORE_ARM_CA7;
    return 0;
}

static void set_passive(void)
{
    struct brcm_core *arm = brcm_core(arm_core_id());
    if (!arm)
        return;
    /* clear all IOCTL bits except HALT, hold the CPU halted */
    u32 val = bp_read32(arm->wrap + BCMA_IOCTL) & ARMCR4_BCMA_IOCTL_CPUHALT;
    ai_resetcore(arm, val, ARMCR4_BCMA_IOCTL_CPUHALT, ARMCR4_BCMA_IOCTL_CPUHALT);
    /* d11 cores: disable only, the firmware enables them (brcmfmac: releasing reset
       ourselves breaks BCM4387) */
    for (int i = 0; i < brcm.ncores; i++)
        if (brcm.cores[i].id == BCMA_CORE_80211)
            ai_coredisable(&brcm.cores[i], D11_BCMA_IOCTL_PHYRESET | D11_BCMA_IOCTL_PHYCLOCKEN,
                           D11_BCMA_IOCTL_PHYCLOCKEN);
}

static void set_active(u32 rstvec)
{
    if (!brcm.skip_reset_vector)
        tcm_write32(0, rstvec);
    ai_resetcore(brcm_core(arm_core_id()), ARMCR4_BCMA_IOCTL_CPUHALT, 0, 0);
}

/* ---------- reset ---------- */

static void reset_device(void)
{
    struct brcm_core *cc = brcm_core(BCMA_CORE_CHIPCOMMON);
    struct brcm_core *pcie = brcm_core(BCMA_CORE_PCIE2);

    select_core(BCMA_CORE_PCIE2);
    u32 lsc = read32(brcm.cfg + BRCM_CFG_LINK_STATUS_CTRL);
    write32(brcm.cfg + BRCM_CFG_LINK_STATUS_CTRL, lsc & ~BRCM_LINK_STATUS_CTRL_ASPM_ENAB);

    select_core(BCMA_CORE_CHIPCOMMON);
    if (cc->rev >= 65) {
        u32 mask = CC_WD_SSRESET_PCIE_F0_EN;
        if (pcie->rev < 66)
            mask |= CC_WD_SSRESET_PCIE_ALL_FN_EN;
        u32 val = read32(brcm.regs + CC_WATCHDOG);
        val = (val & ~CC_WD_ENABLE_MASK) | mask;
        write32(brcm.regs + CC_WATCHDOG, val);
        val = (val & ~CC_WD_COUNTER_MASK) | 4;
        write32(brcm.regs + CC_WATCHDOG, val);
        mdelay(10);
        write32(brcm.regs + CC_INTSTATUS, read32(brcm.regs + CC_INTSTATUS) | mask);
    } else {
        write32(brcm.regs + CC_WATCHDOG, 4);
        mdelay(100);
    }

    select_core(BCMA_CORE_PCIE2);
    write32(brcm.cfg + BRCM_CFG_LINK_STATUS_CTRL, lsc);

    /* brcmf_pcie_buscore_reset: ack the mailbox through the window (still on PCIe2) */
    u32 reg = brcm.reginfo64 ? BRCM_PCIE64_MAILBOXINT : BRCM_PCIE_MAILBOXINT;
    u32 val = read32(brcm.regs + reg);
    if (val != 0xffffffff)
        write32(brcm.regs + reg, val);
}

/* ---------- RAM ---------- */

static u32 socram_banksize(struct brcm_core *c, u8 idx)
{
    u32 bankidx = (SOCRAM_MEMTYPE_RAM << SOCRAM_BANKIDX_MEMTYPE_SHIFT) | idx;
    bp_write32(c->base + SOCRAM_BANKIDX, bankidx);
    u32 info = bp_read32(c->base + SOCRAM_BANKINFO);
    return ((info & SOCRAM_BANKINFO_SZMASK) + 1) * SOCRAM_BANKINFO_SZBASE;
}

static int get_raminfo(void)
{
    struct brcm_core *c;

    if ((c = brcm_core(BCMA_CORE_ARM_CR4))) {
        u32 cap = bp_read32(c->base + ARMCR4_CAP);
        u32 totb = (cap & ARMCR4_TCBANB_MASK) + ((cap & ARMCR4_TCBBNB_MASK) >> 4);
        brcm.ramsize = 0;
        for (u32 i = 0; i < totb; i++) {
            bp_write32(c->base + ARMCR4_BANKIDX, i);
            u32 bx = bp_read32(c->base + ARMCR4_BANKINFO);
            u32 blk = ARMCR4_BSZ_MULT;
            if (bx & ARMCR4_BLK_1K_MASK)
                blk >>= 3;
            brcm.ramsize += ((bx & ARMCR4_BSZ_MASK) + 1) * blk;
        }
    } else if ((c = brcm_core(BCMA_CORE_SYS_MEM))) {
        if (!ai_iscoreup(c))
            ai_resetcore(c, 0, 0, 0);
        u32 ci = bp_read32(c->base + SOCRAM_COREINFO), nrb, nb;
        if (c->rev >= 12) {
            nrb = (ci & SYSMEM_SRCI_NEW_ROMNB_MASK) >> SYSMEM_SRCI_NEW_ROMNB_SHIFT;
            nb = (ci & SYSMEM_SRCI_NEW_SRNB_MASK) >> SYSMEM_SRCI_NEW_SRNB_SHIFT;
        } else {
            nrb = (ci & SYSMEM_SRCI_ROMNB_MASK) >> SYSMEM_SRCI_ROMNB_SHIFT;
            nb = ci & SYSMEM_SRCI_SRNB_MASK;
        }
        brcm.ramsize = 0;
        for (u32 i = 0; i < nb; i++)
            brcm.ramsize += socram_banksize(c, i + nrb);
    } else {
        printf("wlan: no CR4/SYSMEM core for RAM sizing\n");
        return -1;
    }

    switch (brcm.chip) {
        case 0x4388: brcm.rambase = 0x200000; break;
        case 0x4387: brcm.rambase = 0x740000; break;
        case 0x4378: brcm.rambase = 0x352000; break;
        default:
            printf("wlan: no RAM base for chip %x\n", brcm.chip);
            return -1;
    }
    if (!brcm.ramsize || brcm.ramsize > BRCM_MAX_MEMSIZE * 4) {
        printf("wlan: bad RAM size 0x%x\n", brcm.ramsize);
        return -1;
    }
    return 0;
}

/* ---------- OTP (Apple module/vendor/version) ---------- */

static struct brcm_otp otp;

const struct brcm_otp *brcm_otp(void)
{
    return &otp;
}

void brcm_raminfo(u32 *rambase, u32 *ramsize)
{
    *rambase = brcm.rambase;
    *ramsize = brcm.ramsize;
}

static void otp_copy(char *dst, const char *p, size_t len)
{
    if (len > 15)
        len = 15;
    memcpy(dst, p, len);
    dst[len] = 0;
}

static int otp_sys_vendor(const u8 *data, size_t size)
{
    size_t idx = 4;
    u32 hdr;

    if (size < 6)
        return -1;
    memcpy(&hdr, data, 4);
    if (hdr != 0x00000008)
        return -1;
    idx += strnlen((const char *)data + idx, size - idx) + 1; /* chip params */
    if (idx >= size)
        return -1;
    const char *board = (const char *)data + idx;
    size_t blen = strnlen(board, size - idx);
    if (idx + blen >= size)
        return -1;
    printf("wlan: OTP board params '%s'\n", board);

    const char *p = board;
    while (*p == ' ')
        p++;
    while (*p) {
        char tag = *p++;
        if (*p++ != '=')
            return -1;
        const char *end = p;
        while (*end && *end != ' ')
            end++;
        if (tag == 'M')
            otp_copy(otp.module, p, end - p);
        else if (tag == 'V')
            otp_copy(otp.vendor, p, end - p);
        else if (tag == 'm')
            otp_copy(otp.version, p, end - p);
        p = end;
        while (*p == ' ')
            p++;
    }
    otp.valid = otp.module[0] && otp.vendor[0] && otp.version[0];
    return otp.valid ? 0 : -1;
}

static int read_otp(void)
{
    u32 off, words;
    struct brcm_core *gci = brcm_core(BCMA_CORE_GCI);

    switch (brcm.chip) {
        case 0x4388: off = 0x115c; words = 0x150; break;
        case 0x4387: off = 0x113c; words = 0x170; break;
        case 0x4378: off = 0x1120; words = 0x170; break;
        default: return 0;
    }
    if (!gci) {
        printf("wlan: no GCI core for OTP\n");
        return -1;
    }

    static u16 buf[0x170];
    u32 addr = gci->base + off;
    bp_window(addr);
    for (u32 i = 0; i < words; i++)
        buf[i] = read16(brcm.regs + (addr & (BRCM_BAR0_REG_SIZE - 1)) + 2 * i);

    const u8 *o = (const u8 *)buf;
    size_t size = 2 * words;
    int ret = -1;
    for (size_t p = 0; p + 1 < size;) {
        u8 type = o[p], len = o[p + 1];
        if (!type || p + 2 + len > size)
            break;
        if (type == 0x15) /* SYS_VENDOR */
            ret = otp_sys_vendor(o + p + 2, len);
        p += 2 + len;
    }
    if (otp.valid)
        printf("wlan: OTP module=%s vendor=%s version=%s\n", otp.module, otp.vendor, otp.version);
    else
        printf("wlan: OTP has no usable SYS_VENDOR record\n");
    return ret;
}

/* ---------- attach ---------- */

int brcm_attach(const struct wlan_pci *pci)
{
    memset(&brcm, 0, sizeof(brcm));
    memset(&otp, 0, sizeof(otp));
    brcm.cfg = pci->cfg;
    brcm.regs = pci->bar0;
    brcm.tcm = pci->bar2;
    brcm.tcm_size = pci->bar2_size;

    u32 cid = bp_read32(BRCM_ENUM_BASE);
    brcm.chip = cid & 0xffff;
    brcm.chiprev = (cid >> 16) & 0xf;
    if (((cid >> 28) & 0xf) != 1) {
        printf("wlan: chip 0x%08x is not an AXI backplane\n", cid);
        return -1;
    }
    if (erom_scan()) {
        printf("wlan: EROM scan failed\n");
        return -1;
    }
    for (int i = 0; i < brcm.ncores; i++)
        printf("wlan:   core 0x%03x rev %2u base 0x%08x wrap 0x%08x\n", brcm.cores[i].id,
               brcm.cores[i].rev, brcm.cores[i].base, brcm.cores[i].wrap);
    if (!arm_core_id() || !brcm_core(BCMA_CORE_PCIE2) || !brcm_core(BCMA_CORE_CHIPCOMMON)) {
        printf("wlan: missing ARM/PCIe2/ChipCommon core\n");
        return -1;
    }
    brcm.reginfo64 = brcm_core(BCMA_CORE_PCIE2)->rev >= 64;

    set_passive();
    reset_device();
    set_passive();
    if (get_raminfo())
        return -1;

    struct brcm_core *cc = brcm_core(BCMA_CORE_CHIPCOMMON);
    brcm.cc_caps = bp_read32(cc->base + CC_CAPABILITIES);
    printf("wlan: chip %x rev %u, %s CPU, PCIe2 rev %u%s, RAM 0x%x at 0x%x, BAR2 %lu KiB\n",
           brcm.chip, brcm.chiprev, arm_core_id() == BCMA_CORE_ARM_CA7 ? "CA7" : "CR4",
           brcm_core(BCMA_CORE_PCIE2)->rev, brcm.reginfo64 ? " (64-bit mailbox regs)" : "",
           brcm.ramsize, brcm.rambase, (unsigned long)(brcm.tcm_size >> 10));
    if (brcm.rambase + brcm.ramsize > brcm.tcm_size) {
        printf("wlan: RAM does not fit the BAR2 window\n");
        return -1;
    }
    read_otp();
    brcm.attached = true;
    return 0;
}

/* ---------- console ---------- */

static void console_init(void)
{
    struct brcm_console *con = &brcm.con;
    con->base = tcm_read32(brcm.shared + SHARED_CONSOLE_ADDR);
    if (!con->base || con->base >= brcm.tcm_size)
        return;
    con->buf = tcm_read32(con->base + CONSOLE_BUFADDR);
    con->size = tcm_read32(con->base + CONSOLE_BUFSIZE);
    if (con->buf >= brcm.tcm_size || !con->size || con->size > 0x100000)
        con->base = 0;
}

void brcm_console_poll(void)
{
    struct brcm_console *con = &brcm.con;
    if (!con->base)
        return;
    u32 widx = tcm_read32(con->base + CONSOLE_WRITEIDX);
    for (int n = 0; widx != con->ridx && widx < con->size && n < 4096; n++) {
        u8 ch = tcm_read8(con->buf + con->ridx);
        if (++con->ridx == con->size)
            con->ridx = 0;
        if (ch == '\r')
            continue;
        if (ch != '\n' && con->len < sizeof(con->line) - 1) {
            con->line[con->len++] = ch;
            continue;
        }
        con->line[con->len] = 0;
        if (con->len)
            printf("wlan: fw: %s\n", con->line);
        con->len = 0;
    }
}

/* ---------- firmware download ---------- */

static int add_rtlv(u32 *address, u32 type, u32 length)
{
    u32 ram_start = ALIGN_UP(brcm.rambase + brcm.fw_size + BL_HEAP_START_GAP, 4);
    u32 ram_end = ram_start + BL_HEAP_SIZE;
    length = ALIGN_UP(length, 4);
    u32 start = *address - length - 8;
    if (length > 0xffff || start > *address || start < ram_end) {
        printf("wlan: no room for rTLV 0x%x (%u bytes)\n", type, length);
        return -1;
    }
    u32 footer[2] = {type == RANDOM_SEED_MAGIC ? length : (length | ((length ^ 0xffff) << 16)),
                     type};
    tcm_copy_to(*address - 8, footer, 8);
    *address = start;
    return 0;
}

static int add_signature(u32 *address, const void *sig, u32 sig_len)
{
    struct {
        u32 start, end;
    } mm[11]; /* reset_vec, int_vec, rom, mmap, vstatus, fw, sig, heap, stack, prng, nvram */
    memset(mm, 0, sizeof(mm));

    mm[6].end = *address;
    if (add_rtlv(address, FW_SIG_MAGIC, sig_len))
        return -1;
    mm[6].start = *address;
    mm[4].end = *address;
    if (add_rtlv(address, VSTATUS_MAGIC, VSTATUS_SIZE))
        return -1;
    mm[4].start = *address;
    if (add_rtlv(address, MEMMAP_MAGIC, sizeof(mm)))
        return -1;
    mm[5].start = brcm.rambase;
    mm[5].end = brcm.rambase + brcm.fw_size;
    mm[7].start = ALIGN_UP(mm[5].end + BL_HEAP_START_GAP, 4);
    mm[7].end = mm[7].start + BL_HEAP_SIZE;
    if (mm[7].end > *address)
        return -1;
    tcm_copy_to(mm[6].start, sig, sig_len);
    tcm_zero(mm[4].start, VSTATUS_SIZE);
    tcm_copy_to(*address, mm, sizeof(mm));
    if (add_rtlv(address, END_MAGIC, 0))
        return -1;
    brcm.skip_reset_vector = true;
    return 0;
}

static u32 ram_read32(u32 off)
{
    return tcm_read32(brcm.rambase + off);
}

static void ram_write32(u32 off, u32 val)
{
    tcm_write32(brcm.rambase + off, val);
}

static int init_shared(u32 addr)
{
    brcm.shared = addr;
    brcm.flags = tcm_read32(addr + SHARED_FLAGS);
    brcm.version = brcm.flags & SHARED_VERSION_MASK;
    if (brcm.version < 5 || brcm.version > 7) {
        printf("wlan: unsupported PCIe shared version %u\n", brcm.version);
        return -1;
    }
    if (brcm.flags & SHARED_DMA_INDEX)
        brcm.dma_idx_sz = (brcm.flags & SHARED_DMA_2B_IDX) ? 2 : 4;
    brcm.max_rxbufpost = tcm_read16(addr + SHARED_MAX_RXBUFPOST);
    if (!brcm.max_rxbufpost)
        brcm.max_rxbufpost = 255;
    brcm.rx_dataoffset = tcm_read32(addr + SHARED_RX_DATAOFFSET);
    brcm.htod_mb = tcm_read32(addr + SHARED_HTOD_MB_DATA_ADDR);
    brcm.dtoh_mb = tcm_read32(addr + SHARED_DTOH_MB_DATA_ADDR);
    brcm.ring_info = tcm_read32(addr + SHARED_RING_INFO_ADDR);
    console_init();
    brcm_console_poll();
    if (brcm.version >= 6) {
        brcm.flags2 = tcm_read32(addr + SHARED_FLAGS2);
        brcm.flags3 = tcm_read32(addr + SHARED_FLAGS3);
        brcm.mb_via_ctl = !(brcm.flags & SHARED_USE_MAILBOX);
        u32 host_cap = brcm.version | HOSTCAP_DS_NO_OOB_DW;
        if (brcm.flags & SHARED_HOSTRDY_DB1)
            host_cap |= HOSTCAP_H2D_ENABLE_HOSTRDY;
        if (brcm.flags & SHARED_DAR)
            host_cap |= HOSTCAP_H2D_DAR;
        tcm_write32(addr + SHARED_HOST_CAP, host_cap);
        tcm_write32(addr + SHARED_HOST_CAP2, 0);
    }
    printf("wlan: firmware up: shared at 0x%x, PCIe protocol v%u, flags 0x%x/0x%x, dma idx %u, "
           "rxbufpost %u, rx offset %u, console %s\n",
           addr, brcm.version, brcm.flags, brcm.flags2, brcm.dma_idx_sz, brcm.max_rxbufpost,
           brcm.rx_dataoffset, brcm.con.base ? "yes" : "no");
    return 0;
}

int brcm_boot(const struct brcm_fw_set *fw)
{
    if (!brcm.attached)
        return -1;
    if (!fw->fw || fw->fw_len < 0x100 || !fw->nvram || !fw->nvram_len || !fw->seed) {
        printf("wlan: firmware set incomplete (fw %lu nvram %lu seed %s)\n",
               (unsigned long)fw->fw_len, (unsigned long)fw->nvram_len, fw->seed ? "yes" : "no");
        return -1;
    }
    const u8 *code = (const u8 *)fw->fw;

    /* BAR1 window may not be sized properly (brcmf_pcie_attach) */
    select_core(BCMA_CORE_PCIE2);
    pcie_write32(BRCM_PCIE_CONFIGADDR, 0x4e0);
    pcie_write32(BRCM_PCIE_CONFIGDATA, pcie_read32(BRCM_PCIE_CONFIGDATA));

    /* firmware may carry its own RAM size ("SMAR" at 0x6c) */
    if (fw->fw_len >= RAMSIZE_OFFSET + 8) {
        u32 magic, size;
        memcpy(&magic, code + RAMSIZE_OFFSET, 4);
        memcpy(&size, code + RAMSIZE_OFFSET + 4, 4);
        if (magic == RAMSIZE_MAGIC) {
            printf("wlan: firmware sets RAM size 0x%x\n", size);
            brcm.ramsize = size;
        }
    }
    if (brcm.rambase + brcm.ramsize > brcm.tcm_size || fw->fw_len + fw->nvram_len >= brcm.ramsize) {
        printf("wlan: firmware does not fit (RAM 0x%x, fw %lu)\n", brcm.ramsize,
               (unsigned long)fw->fw_len);
        return -1;
    }

    /* enter download state: no IRQs, no early hostready */
    u32 mbmask = brcm.reginfo64 ? BRCM_PCIE64_MAILBOXMASK : BRCM_PCIE_MAILBOXMASK;
    u32 mbint = brcm.reginfo64 ? BRCM_PCIE64_MAILBOXINT : BRCM_PCIE_MAILBOXINT;
    pcie_write32(mbmask, 0);
    pcie_write32(mbint, 0xffffffff);
    write32(brcm.cfg + BRCM_CFG_INTMASK, 0);
    pcie_write32(BRCM_PCIE_CONFIGADDR, BRCM_CFGREG_TLCNTRL_5);
    pcie_write32(BRCM_PCIE_CONFIGDATA, 0xffffffff);

    printf("wlan: downloading firmware (%lu bytes) to 0x%x\n", (unsigned long)fw->fw_len,
           brcm.rambase);
    tcm_copy_to(brcm.rambase, code, fw->fw_len);
    u32 resetintr;
    memcpy(&resetintr, code, 4);
    brcm.fw_size = fw->fw_len;

    ram_write32(brcm.ramsize - 4, 0);
    u32 address = brcm.rambase + brcm.ramsize - fw->nvram_len;
    tcm_copy_to(address, (const void *)fw->nvram, fw->nvram_len);

    /* Apple firmware: random seed rTLV below the NVRAM, then the optional signature */
    if (add_rtlv(&address, RANDOM_SEED_MAGIC, RANDOM_SEED_LENGTH) == 0)
        tcm_copy_to(address, (const void *)fw->seed, RANDOM_SEED_LENGTH);
    if (fw->sig && fw->sig_len && add_signature(&address, (const void *)fw->sig, fw->sig_len))
        printf("wlan: firmware signature footer failed\n");

    tcm_zero(brcm.rambase + brcm.fw_size, address - brcm.fw_size - brcm.rambase);
    u32 written = ram_read32(brcm.ramsize - 4);

    printf("wlan: starting the ARM (reset vector 0x%x%s)\n", resetintr,
           brcm.skip_reset_vector ? ", signed boot" : "");
    set_active(resetintr);

    u32 shared = written;
    u64 t0 = get_ticks();
    while (shared == written && ticks_to_msecs(get_ticks() - t0) < 5000) {
        mdelay(50);
        shared = ram_read32(brcm.ramsize - 4);
    }
    if (shared == written) {
        printf("wlan: firmware did not start (no shared RAM address after 5 s)\n");
        brcm.shared = 0;
        return -1;
    }
    if (shared < brcm.rambase || shared >= brcm.rambase + brcm.ramsize) {
        printf("wlan: bad shared RAM address 0x%08x\n", shared);
        return -1;
    }
    printf("wlan: firmware started after %lu ms\n", (unsigned long)ticks_to_msecs(get_ticks() - t0));
    return init_shared(shared);
}
