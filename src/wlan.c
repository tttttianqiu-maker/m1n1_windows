/* SPDX-License-Identifier: MIT */
/*
 * J613 built-in Wi-Fi, stage 2: PCIe link to the Broadcom BCM4388 and its chip id.
 *
 * Windows has no driver for Apple's Broadcom Wi-Fi, so m1n1 will run the chip itself
 * (FullMAC, like Linux brcmfmac) and give the guest a virtual network card. This file
 * brings up the PCIe port the chip sits on and identifies it; the firmware boot comes next.
 *
 * Hardware, from Linux (Asahi) device trees: t8122.dtsi pcie0 (ECAM 0x580000000, port0
 * PERST# = pinctrl_ap GPIO 187 active low, DART pcie0_dart stream 1 for bus 1,
 * 32-bit MMIO PCI 0xc0000000 -> CPU 0x5c0000000) and t8122-jxxx.dtsi port00 (pwren =
 * SMC GPIO 13 active high, wifi@0,0 = pci14e4,4434). The controller and port PHYs are set
 * up by pcie.c (m1n1 upstream "apcie,t8122"); the per-port power/PERST#/refclk/link
 * sequence follows the order of Linux drivers/pci/controller/pcie-apple.c setup_link /
 * setup_port: assert PERST#, power on, refclk, 100 ms, release PERST#, 100 ms, port
 * ready, LTSSM start, link up. The chip id read follows brcmfmac (BAR0 window at config
 * 0x80 onto the backplane enumeration base 0x18000000).
 *
 * Opt-in (`j613 wifi probe` -> P_HV_WIFI); everything is logged as "wlan: ...".
 */
#include "wlan.h"
#include "adt.h"
#include "pcie.h"
#include "smc.h"
#include "string.h"
#include "utils.h"

#define WLAN_PORT       0
#define WLAN_PWREN_GPIO 13  /* SMC GPIO, active high */
#define WLAN_PERST_GPIO 187 /* AP GPIO, active low */
#define AP_GPIO_T8122   0x2c7100000UL
#define WLAN_DART_SID   1

/* port registers (Linux pcie-apple.c names) */
#define PORT_LTSSMCTL       0x080
#define PORT_LTSSMCTL_START BIT(0)
#define PORT_LINKSTS        0x208
#define PORT_LINKSTS_UP     BIT(0)
#define PORT_LINKSTS_BUSY   BIT(2)
#define PORT_APPCLK         0x800
#define PORT_APPCLK_EN      BIT(0)
#define PORT_APPCLK_CGDIS   BIT(8)
#define PORT_STATUS         0x804
#define PORT_STATUS_READY   BIT(0)
#define PORT_PERST          0x82c /* T602x/T8122 */
#define PORT_PERST_OFF      BIT(0)
#define PORT_RID2SID        0x3000 /* T602x/T8122 */
#define PORT_RID2SID_VALID  BIT(31)

/* port PHY lane config */
#define PHY_LANE_CFG            0x000
#define PHY_LANE_CFG_REFCLK0REQ BIT(0)
#define PHY_LANE_CFG_REFCLK1REQ BIT(1)
#define PHY_LANE_CFG_REFCLK0ACK BIT(2)
#define PHY_LANE_CFG_REFCLK1ACK BIT(3)
#define PHY_LANE_CFG_REFCLKEN   (BIT(9) | BIT(10))
#define PHY_LANE_CFG_REFCLKCGEN (BIT(30) | BIT(31))

/* AP GPIO pin register (Linux pinctrl-apple-gpio.c) */
#define GPIO_DATA   BIT(0)
#define GPIO_MODE   GENMASK(3, 1)
#define GPIO_OUT    (1 << 1)
#define GPIO_PERIPH GENMASK(6, 5)

/* PCI config space */
#define PCI_COMMAND      0x04
#define PCI_CMD_MEM      BIT(1)
#define PCI_CMD_MASTER   BIT(2)
#define PCI_BAR(n)       (0x10 + 4 * (n))
#define PCI_BUS_NUMBERS  0x18
#define PCI_MEM_WINDOW   0x20
#define PCI_PREF_WINDOW  0x24
#define PCI_PREF_BASE_HI 0x28
#define PCI_PREF_LIM_HI  0x2c

/* 32-bit MMIO window of pcie0 (t8122.dtsi ranges) */
#define PCI_MMIO_PCI 0xc0000000UL
#define PCI_MMIO_CPU 0x5c0000000UL
#define PCI_MMIO_MAX 0x40000000UL

/* brcmfmac */
#define BRCM_BAR0_WINDOW 0x80
#define BRCM_ENUM_BASE   0x18000000

static struct wlan_pci wp;

const struct wlan_pci *wlan_pci(void)
{
    return &wp;
}

static u64 ap_gpio_base(void)
{
    static const char *const paths[] = {"/arm-io/gpio0", "/arm-io/gpio"};
    for (unsigned int i = 0; i < ARRAY_SIZE(paths); i++) {
        int path[8];
        u64 base = 0;
        if (adt_path_offset_trace(adt, paths[i], path) < 0 ||
            adt_get_reg(adt, path, "reg", 0, &base, NULL) < 0)
            continue;
        if (base == AP_GPIO_T8122)
            return base;
        printf("wlan: %s at 0x%lx, expected 0x%lx (t8122.dtsi pinctrl_ap)\n", paths[i], base,
               AP_GPIO_T8122);
    }
    return 0;
}

static void ap_gpio_out(u64 base, unsigned int pin, bool level)
{
    mask32(base + 4 * pin, GPIO_PERIPH | GPIO_MODE | GPIO_DATA, GPIO_OUT | (level ? GPIO_DATA : 0));
}

static int smc_gpio_set(unsigned int gpio, bool level)
{
    static const char hex[] = "0123456789abcdef";
    u32 key = ((u32)'g' << 24) | ((u32)'P' << 16) | ((u32)hex[(gpio >> 4) & 0xf] << 8) |
              (u32)hex[gpio & 0xf];
    smc_dev_t *smc = smc_init();
    if (!smc)
        return -1;
    int ret = smc_write_u32(smc, key, (1u << 24) | (level ? 1 : 0)); /* CMD_OUTPUT | level */
    smc_shutdown(smc);                                                /* see hv_dchid.c */
    return ret;
}

static int refclk_on(u64 phy)
{
    set32(phy + PHY_LANE_CFG, PHY_LANE_CFG_REFCLK0REQ);
    if (poll32(phy + PHY_LANE_CFG, PHY_LANE_CFG_REFCLK0ACK, PHY_LANE_CFG_REFCLK0ACK, 50000))
        return -1;
    set32(phy + PHY_LANE_CFG, PHY_LANE_CFG_REFCLK1REQ);
    if (poll32(phy + PHY_LANE_CFG, PHY_LANE_CFG_REFCLK1ACK, PHY_LANE_CFG_REFCLK1ACK, 50000))
        return -1;
    set32(phy + PHY_LANE_CFG, PHY_LANE_CFG_REFCLKEN);
    return 0;
}

/* Size a 64-bit memory BAR pair; 0 if it is not one. */
static u64 bar_size64(u64 cfg, int bar)
{
    u32 lo = read32(cfg + PCI_BAR(bar));
    if ((lo & 0x7) != 0x4) /* memory, 64-bit */
        return 0;
    u32 hi = read32(cfg + PCI_BAR(bar + 1));
    write32(cfg + PCI_BAR(bar), 0xffffffff);
    write32(cfg + PCI_BAR(bar + 1), 0xffffffff);
    u64 mask = ((u64)read32(cfg + PCI_BAR(bar + 1)) << 32) | (read32(cfg + PCI_BAR(bar)) & ~0xfUL);
    write32(cfg + PCI_BAR(bar), lo);
    write32(cfg + PCI_BAR(bar + 1), hi);
    return mask ? (~mask + 1) : 0;
}

static void bar_set64(u64 cfg, int bar, u64 pci_addr)
{
    write32(cfg + PCI_BAR(bar), (u32)pci_addr);
    write32(cfg + PCI_BAR(bar + 1), (u32)(pci_addr >> 32));
}

static int link_up(void)
{
    u64 port = wp.port, phy = wp.phy;

    printf("wlan: port0 0x%lx phy 0x%lx linksts 0x%x status 0x%x perst 0x%x appclk 0x%x\n", port,
           phy, read32(port + PORT_LINKSTS), read32(port + PORT_STATUS),
           read32(port + PORT_PERST), read32(port + PORT_APPCLK));

    if (!(read32(port + PORT_LINKSTS) & PORT_LINKSTS_UP)) {
        u64 gpio = ap_gpio_base();
        if (!gpio) {
            printf("wlan: AP GPIO block not found, not touching PERST#\n");
            return -1;
        }
        set32(port + PORT_APPCLK, PORT_APPCLK_EN);
        ap_gpio_out(gpio, WLAN_PERST_GPIO, false); /* assert PERST# */
        if (smc_gpio_set(WLAN_PWREN_GPIO, true)) {
            printf("wlan: SMC GPIO %d (power) failed\n", WLAN_PWREN_GPIO);
            return -1;
        }
        if (refclk_on(phy)) {
            printf("wlan: refclk did not come up (PHY_LANE_CFG 0x%x)\n", read32(phy));
            return -1;
        }
        mdelay(100); /* power-up to PERST# release, PCIe CEM Tpvperl */
        set32(port + PORT_PERST, PORT_PERST_OFF);
        ap_gpio_out(gpio, WLAN_PERST_GPIO, true); /* release PERST# */
        mdelay(100);
        if (poll32(port + PORT_STATUS, PORT_STATUS_READY, PORT_STATUS_READY, 250000)) {
            printf("wlan: port not ready (status 0x%x)\n", read32(port + PORT_STATUS));
            return -1;
        }
    }
    set32(phy + PHY_LANE_CFG, PHY_LANE_CFG_REFCLKCGEN);
    clear32(port + PORT_APPCLK, PORT_APPCLK_CGDIS);

    /* Requester 01:00.0 -> DART stream 1 (t8122.dtsi iommu-map <0x100 &pcie0_dart 1 1>) */
    write32(port + PORT_RID2SID, PORT_RID2SID_VALID | (WLAN_DART_SID << 16) | 0x100);

    if (!(read32(port + PORT_LINKSTS) & PORT_LINKSTS_UP)) {
        write32(port + PORT_LTSSMCTL, PORT_LTSSMCTL_START);
        u64 t0 = get_ticks();
        while (!(read32(port + PORT_LINKSTS) & PORT_LINKSTS_UP)) {
            if (ticks_to_msecs(get_ticks() - t0) > 500) {
                printf("wlan: link did not come up (linksts 0x%x)\n", read32(port + PORT_LINKSTS));
                return -1;
            }
            udelay(100);
        }
        printf("wlan: link up after %lu ms\n", (unsigned long)ticks_to_msecs(get_ticks() - t0));
    } else {
        printf("wlan: link already up\n");
    }
    return 0;
}

static int enumerate(void)
{
    u64 rp = wp.ecam + (WLAN_PORT << 15); /* root port 00:0N.0 */

    /* bus 1 behind the root port; MMIO window filled in once the BAR sizes are known */
    write32(rp + PCI_BUS_NUMBERS, (read32(rp + PCI_BUS_NUMBERS) & 0xff000000) | 0x010100);
    write32(rp + PCI_PREF_WINDOW, 0x0000fff0); /* prefetchable window off */
    write32(rp + PCI_PREF_BASE_HI, 0);
    write32(rp + PCI_PREF_LIM_HI, 0);

    wp.cfg = wp.ecam + (1 << 20); /* 01:00.0 */
    u32 id = read32(wp.cfg);
    u32 class = read32(wp.cfg + 0x08);
    printf("wlan: 01:00.0 %04x:%04x class %06x rev %02x, 01:00.1 %08x\n", id & 0xffff, id >> 16,
           class >> 8, class & 0xff, read32(wp.cfg + (1 << 12)));
    if ((id & 0xffff) != 0x14e4) {
        printf("wlan: no Broadcom function behind port 0\n");
        return -1;
    }
    wp.device = id >> 16;

    wp.bar0_size = bar_size64(wp.cfg, 0);
    wp.bar2_size = bar_size64(wp.cfg, 2);
    printf("wlan: BAR0 %lu KiB, BAR2 %lu KiB\n", (unsigned long)(wp.bar0_size >> 10),
           (unsigned long)(wp.bar2_size >> 10));
    if (!wp.bar0_size || !wp.bar2_size || wp.bar0_size + wp.bar2_size > PCI_MMIO_MAX / 2) {
        printf("wlan: unexpected BAR layout\n");
        return -1;
    }

    /* BAR2 (the big TCM window) first at the window base, BAR0 right after it */
    u64 pci2 = PCI_MMIO_PCI;
    u64 pci0 = ALIGN_UP(pci2 + wp.bar2_size, wp.bar0_size);
    u64 end = ALIGN_UP(pci0 + wp.bar0_size, 0x100000);
    bar_set64(wp.cfg, 2, pci2);
    bar_set64(wp.cfg, 0, pci0);
    write32(rp + PCI_MEM_WINDOW, ((u32)(end - 1) & 0xfff00000) | ((u32)PCI_MMIO_PCI >> 16));
    write16(rp + PCI_COMMAND, read16(rp + PCI_COMMAND) | PCI_CMD_MEM | PCI_CMD_MASTER);
    write16(wp.cfg + PCI_COMMAND, read16(wp.cfg + PCI_COMMAND) | PCI_CMD_MEM | PCI_CMD_MASTER);

    wp.bar0 = PCI_MMIO_CPU + (pci0 - PCI_MMIO_PCI);
    wp.bar2 = PCI_MMIO_CPU + (pci2 - PCI_MMIO_PCI);
    printf("wlan: BAR0 at 0x%lx, BAR2 at 0x%lx (bridge window 0x%x)\n", wp.bar0, wp.bar2,
           read32(rp + PCI_MEM_WINDOW));
    return 0;
}

static void read_chip_id(void)
{
    write32(wp.cfg + BRCM_BAR0_WINDOW, BRCM_ENUM_BASE);
    u32 window = read32(wp.cfg + BRCM_BAR0_WINDOW);
    u32 cid = read32(wp.bar0);
    wp.chip = cid & 0xffff;
    wp.chiprev = (cid >> 16) & 0xf;
    printf("wlan: chip %x rev %u (chipid 0x%08x, %s, window 0x%x)\n", wp.chip, wp.chiprev, cid,
           ((cid >> 28) & 0xf) == 1 ? "AXI" : "SB?", window);
}

int wlan_probe(void)
{
    memset(&wp, 0, sizeof(wp));
    if (chip_id != T8122) {
        printf("wlan: only wired up for T8122 (J613)\n");
        return -1;
    }
    printf("wlan: stage 2 probe (PCIe link + chip id)\n");
    if (pcie_init() < 0) {
        printf("wlan: pcie_init failed\n");
        return -1;
    }
    if (pcie_get_port(WLAN_PORT, &wp.ecam, &wp.port, &wp.phy) < 0) {
        printf("wlan: PCIe port %d not initialised by pcie_init\n", WLAN_PORT);
        return -1;
    }
    if (link_up() < 0 || enumerate() < 0)
        return -1;
    read_chip_id();
    wp.up = wp.chip == 0x4388;
    printf("wlan: probe %s\n", wp.up ? "OK (BCM4388)" : "found an unexpected chip");
    return wp.up ? 0 : -1;
}
