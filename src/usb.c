/* SPDX-License-Identifier: MIT */

#include "usb.h"
#include "adt.h"
#include "dart.h"
#include "i2c.h"
#include "iodev.h"
#include "malloc.h"
#include "pmgr.h"
#include "string.h"
#include "spmi.h"
#include "tps6598x.h"
#include "types.h"
#include "usb_dwc3.h"
#include "usb_dwc3_regs.h"
#include "utils.h"
#include "vsprintf.h"

struct usb_drd_regs {
    uintptr_t drd_regs;
    uintptr_t drd_regs_unk3;
    uintptr_t atc;
};

#if USB_IODEV_COUNT > 100
#error "USB_IODEV_COUNT is limited to 100 to prevent overflow in ADT path names"
#endif

// length of the format string is is used as buffer size
// limits the USB instance numbers to reasonable 2 digits
#define FMT_DART_PATH        "/arm-io/dart-usb%u"
#define FMT_DART_MAPPER_PATH "/arm-io/dart-usb%u/mapper-usb%u"
#define FMT_ATC_PATH         "/arm-io/atc-phy%u"
#define FMT_DRD_PATH         "/arm-io/usb-drd%u"
// HPM_PATH string is at most
// "/arm-io/i2cX" (12) + "/" + hpmBusManagerX (14) + "/" + "hpmX" (4) + '\0'
#define MAX_HPM_PATH_LEN 40

static tps6598x_irq_state_t tps6598x_irq_state[USB_IODEV_COUNT];
static bool usb_is_initialized = false;

#define PIPEHANDLER_MUX_CTRL             0x0c
#define PIPEHANDLER_MUX_CTRL_USB3        0x08
#define PIPEHANDLER_MUX_CTRL_USB4_TUNNEL 0x11
#define PIPEHANDLER_MUX_CTRL_DUMMY       0x22

#define PIPEHANDLER_LOCK_REQ 0x10
#define PIPEHANDLER_LOCK_ACK 0x14
#define PIPEHANDLER_LOCK_EN  BIT(0)

#define PIPEHANDLER_AON_GEN                     0x1C
#define PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN BIT(4)
#define PIPEHANDLER_AON_GEN_DWC3_RESET_N        BIT(0)

#define PIPEHANDLER_NONSELECTED_OVERRIDE 0x20
#define PIPEHANDLER_NATIVE_RESET         BIT(12)
#define PIPEHANDLER_DUMMY_PHY_EN         BIT(15)
#define PIPEHANDLER_NATIVE_POWER_DOWN    GENMASK(3, 0)

static dart_dev_t *usb_dart_init(u32 idx)
{
    int mapper_offset;
    char path[sizeof(FMT_DART_MAPPER_PATH)];

    snprintf(path, sizeof(path), FMT_DART_MAPPER_PATH, idx, idx);
    mapper_offset = adt_path_offset(adt, path);
    if (mapper_offset < 0) {
        // Device not present
        return NULL;
    }

    u32 dart_idx;
    if (ADT_GETPROP(adt, mapper_offset, "reg", &dart_idx) < 0) {
        printf("usb: Error getting DART %s device index/\n", path);
        return NULL;
    }

    snprintf(path, sizeof(path), FMT_DART_PATH, idx);
    return dart_init_adt(path, 1, dart_idx, false);
}

static int usb_drd_get_regs(u32 idx, struct usb_drd_regs *regs)
{
    int adt_drd_path[8];
    int adt_drd_offset;
    int adt_phy_path[8];
    int adt_phy_offset;
    char phy_path[sizeof(FMT_ATC_PATH)];
    char drd_path[sizeof(FMT_DRD_PATH)];

    snprintf(drd_path, sizeof(drd_path), FMT_DRD_PATH, idx);
    adt_drd_offset = adt_path_offset_trace(adt, drd_path, adt_drd_path);
    if (adt_drd_offset < 0) {
        // Nonexistent device
        return -1;
    }

    snprintf(phy_path, sizeof(phy_path), FMT_ATC_PATH, idx);
    adt_phy_offset = adt_path_offset_trace(adt, phy_path, adt_phy_path);
    if (adt_phy_offset < 0) {
        printf("usb: Error getting phy node %s\n", phy_path);
        return -1;
    }

    if (adt_get_reg(adt, adt_phy_path, "reg", 0, &regs->atc, NULL) < 0) {
        printf("usb: Error getting reg with index 0 for %s.\n", phy_path);
        return -1;
    }
    if (adt_get_reg(adt, adt_drd_path, "reg", 0, &regs->drd_regs, NULL) < 0) {
        printf("usb: Error getting reg with index 0 for %s.\n", drd_path);
        return -1;
    }
    if (adt_get_reg(adt, adt_drd_path, "reg", 3, &regs->drd_regs_unk3, NULL) < 0) {
        printf("usb: Error getting reg with index 3 for %s.\n", drd_path);
        return -1;
    }

    return 0;
}

int usb_phy_bringup(u32 idx)
{
    char path[24];

    if (idx >= USB_IODEV_COUNT)
        return -1;

    struct usb_drd_regs usb_regs;
    if (usb_drd_get_regs(idx, &usb_regs) < 0)
        return -1;

    snprintf(path, sizeof(path), FMT_ATC_PATH, idx);
    if (pmgr_adt_power_enable(path) < 0)
        return -1;

    snprintf(path, sizeof(path), FMT_DART_PATH, idx);
    if (pmgr_adt_power_enable(path) < 0)
        return -1;

    snprintf(path, sizeof(path), FMT_DRD_PATH, idx);
    if (pmgr_adt_power_enable(path) < 0)
        return -1;

    write32(usb_regs.atc + 0x08, 0x01c1000f);
    write32(usb_regs.atc + 0x04, 0x00000003);
    write32(usb_regs.atc + 0x04, 0x00000000);
    write32(usb_regs.atc + 0x1c, 0x008c0813);
    write32(usb_regs.atc + 0x00, 0x00000002);

    write32(usb_regs.drd_regs_unk3 + PIPEHANDLER_MUX_CTRL, PIPEHANDLER_MUX_CTRL_DUMMY);
    write32(usb_regs.drd_regs_unk3 + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_RESET_N);
    write32(usb_regs.drd_regs_unk3 + PIPEHANDLER_NONSELECTED_OVERRIDE, 0x9332);

    return 0;
}

dwc3_dev_t *usb_iodev_bringup(u32 idx)
{
    dart_dev_t *usb_dart = usb_dart_init(idx);
    if (!usb_dart)
        return NULL;

    struct usb_drd_regs usb_reg;
    if (usb_drd_get_regs(idx, &usb_reg) < 0)
        return NULL;

    return usb_dwc3_init(usb_reg.drd_regs, usb_dart);
}

#define USB_IODEV_WRAPPER(name, pipe)                                                              \
    static ssize_t usb_##name##_can_read(void *dev)                                                \
    {                                                                                              \
        return usb_dwc3_can_read(dev, pipe);                                                       \
    }                                                                                              \
                                                                                                   \
    static bool usb_##name##_can_write(void *dev)                                                  \
    {                                                                                              \
        return usb_dwc3_can_write(dev, pipe);                                                      \
    }                                                                                              \
                                                                                                   \
    static size_t usb_##name##_write_space(void *dev)                                             \
    {                                                                                              \
        return usb_dwc3_write_space(dev, pipe);                                                    \
    }                                                                                              \
                                                                                                   \
    static ssize_t usb_##name##_read(void *dev, void *buf, size_t count)                           \
    {                                                                                              \
        return usb_dwc3_read(dev, pipe, buf, count);                                               \
    }                                                                                              \
                                                                                                   \
    static ssize_t usb_##name##_write(void *dev, const void *buf, size_t count)                    \
    {                                                                                              \
        return usb_dwc3_write(dev, pipe, buf, count);                                              \
    }                                                                                              \
                                                                                                   \
    static ssize_t usb_##name##_queue(void *dev, const void *buf, size_t count)                    \
    {                                                                                              \
        return usb_dwc3_queue(dev, pipe, buf, count);                                              \
    }                                                                                              \
                                                                                                   \
    static void usb_##name##_handle_events(void *dev)                                              \
    {                                                                                              \
        usb_dwc3_handle_events(dev);                                                               \
    }                                                                                              \
                                                                                                   \
    static void usb_##name##_flush(void *dev)                                                      \
    {                                                                                              \
        usb_dwc3_flush(dev, pipe);                                                                 \
    }

USB_IODEV_WRAPPER(0, CDC_ACM_PIPE_0)
USB_IODEV_WRAPPER(1, CDC_ACM_PIPE_1)

static struct iodev_ops iodev_usb_ops = {
    .can_read = usb_0_can_read,
    .can_write = usb_0_can_write,
    .write_space = usb_0_write_space,
    .read = usb_0_read,
    .write = usb_0_write,
    .queue = usb_0_queue,
    .flush = usb_0_flush,
    .handle_events = usb_0_handle_events,
};

static struct iodev_ops iodev_usb_sec_ops = {
    .can_read = usb_1_can_read,
    .can_write = usb_1_can_write,
    .write_space = usb_1_write_space,
    .read = usb_1_read,
    .write = usb_1_write,
    .queue = usb_1_queue,
    .flush = usb_1_flush,
    .handle_events = usb_1_handle_events,
};

struct iodev iodev_usb_vuart = {
    .ops = &iodev_usb_sec_ops,
    .usage = 0,
    .lock = SPINLOCK_INIT,
};

static tps6598x_dev_t *hpm_init(i2c_dev_t *i2c, const char *hpm_path)
{
    tps6598x_dev_t *tps = tps6598x_init_i2c(hpm_path, i2c);
    if (!tps) {
        printf("usb: tps6598x_init failed for %s.\n", hpm_path);
        return NULL;
    }

    if (tps6598x_powerup(tps) < 0) {
        printf("usb: tps6598x_powerup failed for %s.\n", hpm_path);
        tps6598x_shutdown(tps);
        return NULL;
    }

    return tps;
}

static int usb_hpm_idx(const char *hpm_path)
{
    const char *name = strrchr(hpm_path, '/');
    if (!name || strncmp(name + 1, "hpm", 3) != 0)
        return -1;
    int idx = name[4] - '0';
    return (idx < 0 || idx >= USB_IODEV_COUNT) ? -1 : idx;
}

/*
 * Only wake the controller of the port handed to the guest (usb-drd1 / hpm1).
 * Powering up hpm0, the port carrying the m1n1 proxy, reset the machine during
 * chainload in EXP-20261106-014; upstream m1n1 also leaves the debug port's hpm
 * alone. The J613 setup requires the proxy on the usb-drd0 port (Mu skips DWC3_0).
 */
#define USB_SPMI_GUEST_HPM 1

static bool usb_spmi_hpm_match(char *hpm_path, void *data)
{
    UNUSED(data);
    return usb_hpm_idx(hpm_path) == USB_SPMI_GUEST_HPM;
}

/*
 * T8122: the USB-C port controllers sit on SPMI. Without waking and powering
 * them up the ports only work as USB devices fed by the host's VBUS (the m1n1
 * proxy); a port handed to the guest never sources VBUS or presents the host
 * role (EXP-20261106-013). Same sequence as the I2C path: power up, mask IRQs.
 */
static int usb_spmi_hpm_init_one(char *hpm_path, tps6598x_dev_t *tps, void *data)
{
    UNUSED(data);
    int idx = usb_hpm_idx(hpm_path);

    printf("usb: powering up %s\n", hpm_path);
    if (tps6598x_powerup(tps) < 0)
        printf("usb: tps6598x_powerup failed for %s.\n", hpm_path);
    else
        printf("usb: %s powered up\n", hpm_path);

    if (tps6598x_disable_irqs(tps, &tps6598x_irq_state[idx]))
        printf("usb: unable to disable IRQ masks for %s.\n", hpm_path);

    return HPM_ACTION_CONTINUE;
}

static bool usb_spmi_restore_match(char *hpm_path, void *data)
{
    bool force = *(bool *)data;
    int idx = usb_hpm_idx(hpm_path);

    if (idx < 0 || !tps6598x_irq_state[idx].valid)
        return false;
    return force || !iodev_get_usage(IODEV_USB0 + idx);
}

static int usb_spmi_restore_one(char *hpm_path, tps6598x_dev_t *tps, void *data)
{
    UNUSED(data);
    int idx = usb_hpm_idx(hpm_path);

    if (tps6598x_restore_irqs(tps, &tps6598x_irq_state[idx]))
        printf("usb: unable to restore IRQ masks for %s\n", hpm_path);
    return HPM_ACTION_CONTINUE;
}

static bool usb_has_spmi_hpm(void)
{
    return adt_path_offset(adt, "/arm-io/nub-spmi-a0/hpm0") > 0;
}

void usb_spmi_init(void)
{
    for (int idx = 0; idx < USB_IODEV_COUNT; ++idx)
        usb_phy_bringup(idx); /* Fails on missing devices, just continue */

    usb_is_initialized = true;
}

static int usb_init_i2c(const char *i2c_path)
{
    char hpm_path[MAX_HPM_PATH_LEN];

    int node = adt_path_offset(adt, i2c_path);
    if (node < 0)
        return 0;

    node = adt_first_child_offset(adt, node);
    if (node < 0)
        return 0;

    if (!adt_is_compatible(adt, node, "usbc,manager"))
        return 0;

    const char *hpm_mngr_name = adt_get_name(adt, node);
    if (!hpm_mngr_name || strnlen(hpm_mngr_name, 16) >= 16)
        return 0;

    i2c_dev_t *i2c = i2c_init(i2c_path);
    if (!i2c) {
        printf("usb: i2c init failed for %s\n", i2c_path);
        return -1;
    }

    ADT_FOREACH_CHILD(adt, node)
    {
        const char *name = adt_get_name(adt, node);
        if (!name || memcmp(name, "hpm", 3) || name[4] != '\0')
            continue; // unexpected hpm node name
        u32 idx = name[3] - '0';
        if (idx >= USB_IODEV_COUNT)
            continue; // unexpected hpm index

        snprintf(hpm_path, sizeof(hpm_path), "%s/%s/%s", i2c_path, hpm_mngr_name, name);

        tps6598x_dev_t *tps = hpm_init(i2c, hpm_path);
        if (!tps) {
            printf("usb: failed to init %s\n", name);
            continue;
        }

        if (tps6598x_disable_irqs(tps, &tps6598x_irq_state[idx]))
            printf("usb: unable to disable IRQ masks for %s\n", name);

        tps6598x_shutdown(tps);
    }

    i2c_shutdown(i2c);

    return 0;
}

void usb_init(void)
{
    if (usb_is_initialized)
        return;

    /*
     * M3 models do not use i2c, but instead SPMI with a new controller.
     * Wake the SPMI port controllers, then bring up the phys.
     */
    if (usb_has_spmi_hpm()) {
        usb_spmi_init();
        return;
    }

    /*
     * A7-A11 uses a custom internal otg controller with the peripheral part
     * being dwc2.
     */
    if (adt_path_offset(adt, "/arm-io/otgphyctrl") > 0 &&
        adt_path_offset(adt, "/arm-io/usb-complex") > 0) {
        /* We do not support the custom controller and dwc2 (yet). */
        return;
    }

    if (adt_is_compatible(adt, 0, "J180dAP") && usb_init_i2c("/arm-io/i2c3") < 0)
        return;
    if (usb_init_i2c("/arm-io/i2c0") < 0)
        return;

    for (int idx = 0; idx < USB_IODEV_COUNT; ++idx)
        usb_phy_bringup(idx); /* Fails on missing devices, just continue */

    usb_is_initialized = true;
}

void usb_i2c_restore_irqs(const char *i2c_path, bool force)
{
    char hpm_path[MAX_HPM_PATH_LEN];

    int node = adt_path_offset(adt, i2c_path);
    if (node < 0)
        return;

    node = adt_first_child_offset(adt, node);
    if (node < 0)
        return;

    if (!adt_is_compatible(adt, node, "usbc,manager"))
        return;

    const char *hpm_mngr_name = adt_get_name(adt, node);
    if (!hpm_mngr_name || strnlen(hpm_mngr_name, 16) >= 16)
        return;

    i2c_dev_t *i2c = i2c_init(i2c_path);
    if (!i2c) {
        printf("usb: i2c init failed.\n");
        return;
    }

    ADT_FOREACH_CHILD(adt, node)
    {
        const char *name = adt_get_name(adt, node);
        if (!name || memcmp(name, "hpm", 3) || name[4] != '\0')
            continue; // unexpected hpm node name
        u32 idx = name[3] - '0';
        if (idx >= USB_IODEV_COUNT)
            continue; // unexpected hpm index

        if (iodev_get_usage(IODEV_USB0 + idx) && !force)
            continue;

        if (tps6598x_irq_state[idx].valid) {
            snprintf(hpm_path, sizeof(hpm_path), "%s/%s/%s", i2c_path, hpm_mngr_name, name);
            tps6598x_dev_t *tps = hpm_init(i2c, hpm_path);
            if (!tps)
                continue;

            if (tps6598x_restore_irqs(tps, &tps6598x_irq_state[idx]))
                printf("usb: unable to restore IRQ masks for %s\n", name);

            tps6598x_shutdown(tps);
        }
    }

    i2c_shutdown(i2c);
}

/*
 * Called by the hypervisor right before guest entry, when the proxy link is up
 * and every message (or exception) reaches the host log. Doing this at m1n1
 * boot reset the machine before USB came up (EXP-20261106-014/015).
 */
#define USB_DIAG_STEP(...)                                                                         \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        iodev_console_flush();                                                                     \
        mdelay(100);                                                                               \
    } while (0)

static tps6598x_dev_t *usb_guest_tps;

void usb_guest_port_power_up(void)
{
    char bus_path[] = "/arm-io/nub-spmi-a0";
    char hpm_path[40];

    if (!usb_has_spmi_hpm())
        return;
    snprintf(hpm_path, sizeof(hpm_path), "%s/hpm%d", bus_path, USB_SPMI_GUEST_HPM);

    /* EXP-20261106-017: one step at a time so the last line identifies a reset. */
    int bus = adt_path_offset(adt, bus_path);
    int hpm = adt_path_offset(adt, hpm_path);
    u32 len = 0;
    const u8 *reg = hpm >= 0 ? adt_getprop(adt, hpm, "reg", &len) : NULL;
    s32 gen = -1;
    if (bus >= 0)
        ADT_GETPROP(adt, bus, "gen", &gen);
    USB_DIAG_STEP("usbdiag 1 bus=%d hpm=%d compat_bus=%d/%d compat_hpm=%d gen=%d reg_len=%u "
                  "reg=%02x %02x %02x %02x\n",
                  bus, hpm, bus >= 0 && adt_is_compatible(adt, bus, "aapl,spmi"),
                  bus >= 0 && adt_is_compatible(adt, bus, "spmi,gen3"),
                  hpm >= 0 && adt_is_compatible(adt, hpm, "usbc,sn201202x,spmi"), gen, len,
                  reg && len > 0 ? reg[0] : 0xff, reg && len > 1 ? reg[1] : 0xff,
                  reg && len > 2 ? reg[2] : 0xff, reg && len > 3 ? reg[3] : 0xff);
    if (bus < 0 || hpm < 0 || !reg)
        return;

    u64 base = 0;
    int bus_path_trace[8];
    adt_path_offset_trace(adt, bus_path, bus_path_trace);
    adt_get_reg(adt, bus_path_trace, "reg", 0, &base, NULL);
    USB_DIAG_STEP("usbdiag 2 spmi base=0x%lx\n", base);
    if (!base)
        return;

    USB_DIAG_STEP("usbdiag 3 status=0x%x\n", read32(base));

    spmi_dev_t *spmi = spmi_init(bus_path);
    USB_DIAG_STEP("usbdiag 4 spmi_init=%p\n", spmi);
    if (!spmi)
        return;

    USB_DIAG_STEP("usbdiag 5 sending WAKEUP to addr %u\n", reg[0]);
    tps6598x_dev_t *tps = tps6598x_init_spmi(hpm_path, spmi);
    USB_DIAG_STEP("usbdiag 6 wakeup done tps=%p status=0x%x\n", tps, read32(base));
    if (!tps)
        return;

    USB_DIAG_STEP("usbdiag 7 powerup\n");
    int ret = tps6598x_powerup(tps);
    USB_DIAG_STEP("usbdiag 8 powerup=%d (no SHUTDOWN sent, controller left awake)\n", ret);
    usb_guest_tps = tps;
}

/* Guest-port (index 1) ATC USB2 PHY and pipehandler registers. */
void usb_guest_port_phy_print(const char *tag)
{
    struct usb_drd_regs r;
    if (usb_drd_get_regs(1, &r) < 0) {
        printf("HV: phy1 %s: no regs\n", tag);
        return;
    }
    printf("HV: phy1 %s atc[0]=0x%x [4]=0x%x [8]=0x%x [1c]=0x%x ph mux=0x%x aon=0x%x nso=0x%x\n",
           tag, read32(r.atc), read32(r.atc + 4), read32(r.atc + 8), read32(r.atc + 0x1c),
           read32(r.drd_regs_unk3 + PIPEHANDLER_MUX_CTRL),
           read32(r.drd_regs_unk3 + PIPEHANDLER_AON_GEN),
           read32(r.drd_regs_unk3 + PIPEHANDLER_NONSELECTED_OVERRIDE));
}

/*
 * EXP-20261107-031: the guest xHCI sees no connect under Windows although Mu enumerated the
 * stick. Re-enable the port's power domains and rewrite the PHY setup m1n1 did at boot,
 * right when Windows enables the xHCI interrupt (before it resets the controller).
 */
/* Register layout from Linux drivers/phy/apple/atc.c (USB2PHY_*). */
#define U2_USBCTL         0x00
#define U2_USBCTL_RUN     2
#define U2_CTL            0x04
#define U2_CTL_RESET      BIT(0)
#define U2_CTL_PORT_RESET BIT(1)
#define U2_CTL_APB_RESETN BIT(2)
#define U2_CTL_SIDDQ      BIT(3)
#define U2_SIG            0x08
#define U2_SIG_VBUS_FORCE (BIT(0) | BIT(1) | BIT(2) | BIT(3))
#define U2_SIG_HOST       (7 << 12)
#define U2_MISCTUNE       0x1c
#define U2_MISCTUNE_GATES (BIT(29) | BIT(30))

/* Host-mode USB2 PHY for the guest port; reset=true power-cycles it first. */
static void usb_guest_port_phy_host(bool reset)
{
    struct usb_drd_regs r;
    if (usb_drd_get_regs(1, &r) < 0)
        return;
    u64 p = r.atc;

    if (reset) {
        /* power off: hold the PHY in reset and low power */
        write32(p + U2_USBCTL, 0);
        set32(p + U2_CTL, U2_CTL_SIDDQ | U2_CTL_RESET | U2_CTL_PORT_RESET);
        clear32(p + U2_CTL, U2_CTL_APB_RESETN);
        udelay(20);
        /* power on */
        set32(p + U2_SIG, U2_SIG_VBUS_FORCE);
        udelay(10);
        clear32(p + U2_CTL, U2_CTL_SIDDQ);
        udelay(10);
        clear32(p + U2_CTL, U2_CTL_RESET);
        udelay(10);
        clear32(p + U2_CTL, U2_CTL_PORT_RESET);
        udelay(10);
        set32(p + U2_CTL, U2_CTL_APB_RESETN);
        udelay(10);
        clear32(p + U2_MISCTUNE, U2_MISCTUNE_GATES);
        udelay(10);
    }
    set32(p + U2_SIG, U2_SIG_VBUS_FORCE | U2_SIG_HOST);
    write32(p + U2_USBCTL, U2_USBCTL_RUN);
    udelay(50);
}

/* hv_init: the guest port is a host port for the guest, not m1n1's gadget. */
void usb_guest_port_phy_host_mode(void)
{
    usb_guest_port_phy_host(false);
    usb_guest_port_phy_print("host-mode");
}

/*
 * When Windows enables the xHCI interrupt, before it resets the controller: reset the USB2
 * PHY (Linux does the same at probe because boot stages leave it in an unknown state) and
 * bring it up in host mode.
 */
void usb_guest_port_phy_refresh(void)
{
    /* EXP-20261107-032: the full power cycle reset the Air; only switch to host mode. */
    usb_guest_port_phy_print("before-refresh");
    iodev_console_flush();
    usb_guest_port_phy_host(false);
    usb_guest_port_phy_print("after-refresh");
    iodev_console_flush();
}

/*
 * Full guest-port reset in the order Linux uses (drivers/phy/apple/atc.c
 * atcphy_dwc3_reset_assert/_deassert, atcphy_usb2_power_off/_on, and
 * drivers/usb/dwc3/dwc3-apple.c), followed by Mu's DWC3 core init
 * (AppleUsbTypeCBringupDxe). Opt-in, for the guest OS's controller reset.
 *
 * Why: dwc3-apple.c documents that after PHY + DWC3 init only a single "new device
 * connected" event is ever reported ("XHCI's port status register actually does show the
 * correct state but no interrupt ever comes in"), because the USB2 lines run through an
 * eUSB2 repeater the USB-PD chip resets on every CC change; the PHY must be reset to
 * resynchronise. Mu consumes that one event, Windows never gets another (EXP-029..037).
 * EXP-20261107-032 power-cycled the USB2 PHY while the DWC3 was running and the SoC reset:
 * Linux always holds the DWC3 in reset with its clamps on (pipehandler AON_GEN) first.
 */
#define GP_DWC3_GSBUSCFG0          0xc100
#define GP_DWC3_GSBUSCFG1          0xc104
#define GP_DWC3_GCTL               0xc110
#define GP_DWC3_GCTL_CORESOFTRESET BIT(11)
#define GP_DWC3_GSNPSID            0xc120
#define GP_DWC3_GHWPARAMS1         0xc144
#define GP_DWC3_GUSB2PHYCFG        0xc200
#define GP_DWC3_GUSB3PIPECTL       0xc2c0
#define GP_DWC3_GFLADJ             0xc630
#define GP_DWC3_PHYSOFTRST         BIT(31)
#define GP_APPLE_CIO_LFPS          0xcd38
#define GP_APPLE_CIO_BW_NGT        0xcd3c
#define GP_APPLE_CIO_LINK_TIMER    0xcd40
#define U2_USBCTL_ISOLATION     4

/*
 * T8122 (M3) only: Linux drivers/phy/apple/atc.c atcphy_usb2_power_on() sets
 * ATCPHY_EVT_USB2_CTL (PHY "core" block + 0) to EVT_EN | LOAD_CNT after releasing the
 * USB2 PHY resets, on every power-on; T8103 has no such register. m1n1 finds the USB2 PHY
 * as ADT atc-phy reg 0; Linux t8122.dtsi puts the core block 0x570000 above it
 * (0xb02a90000 -> 0xb03000000 for atc-phy1). Only used when the ADT lists that block.
 */
#define ATC_EVT_USB2_CTL          0x0
#define ATC_EVT_USB2_CTL_EVT_EN   BIT(0)
#define ATC_EVT_USB2_CTL_LOAD_CNT BIT(3)

static u64 usb_atc_core_t8122(u32 idx)
{
    char path[sizeof(FMT_ATC_PATH)];
    int adt_path[8];
    u64 u2, base, size;

    if (chip_id != T8122)
        return 0;
    snprintf(path, sizeof(path), FMT_ATC_PATH, idx);
    if (adt_path_offset_trace(adt, path, adt_path) < 0 ||
        adt_get_reg(adt, adt_path, "reg", 0, &u2, NULL) < 0)
        return 0;
    for (int i = 1; i < 16; i++) {
        if (adt_get_reg(adt, adt_path, "reg", i, &base, &size) < 0)
            break;
        if (base == u2 + 0x570000)
            return base;
    }
    printf("usb: %s: no ADT reg at 0x%lx (USB2 PHY 0x%lx + 0x570000), EVT step skipped\n", path,
           u2 + 0x570000, u2);
    return 0;
}

int usb_guest_port_reset_ordered(u64 dwc3)
{
    struct usb_drd_regs r;
    if (usb_drd_get_regs(1, &r) < 0)
        return -1;
    u64 ph = r.drd_regs_unk3, u2 = r.atc;
    u32 mux = read32(ph + PIPEHANDLER_MUX_CTRL);

    /* m1n1 only ever runs this port's PIPE on the dummy backend (USB2 only). */
    if (mux != PIPEHANDLER_MUX_CTRL_DUMMY) {
        printf("usb: ordered reset skipped, PIPE mux 0x%x is not dummy\n", mux);
        return -1;
    }
    u64 core = usb_atc_core_t8122(1);
    printf("usb: ordered guest-port reset (Linux atcphy order%s)\n",
           core ? ", T8122 EVT_USB2_CTL" : "");
    if (core) {
        /* read-only: ATC core power state of the guest port and of m1n1's own port 0
           (Linux atcphy_power_on wakes it: ATCPHY_POWER_CTRL/STAT/MISC at +0x20000) */
        u64 core0 = usb_atc_core_t8122(0);
        printf("usb: atc1 core 0x%lx EVT=0x%x PWR_CTRL=0x%x PWR_STAT=0x%x MISC=0x%x\n", core,
               read32(core), read32(core + 0x20000), read32(core + 0x20004),
               read32(core + 0x20008));
        if (core0)
            printf("usb: atc0 core 0x%lx EVT=0x%x PWR_CTRL=0x%x PWR_STAT=0x%x MISC=0x%x\n", core0,
                   read32(core0), read32(core0 + 0x20000), read32(core0 + 0x20004),
                   read32(core0 + 0x20008));
    }
    iodev_console_flush();

    /* 1. DWC3 into reset with its clamps on */
    clear32(ph + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_RESET_N);
    set32(ph + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
    udelay(10);

    /* 2. USB2 PHY off */
    write32(u2 + U2_USBCTL, U2_USBCTL_ISOLATION);
    udelay(10);
    set32(u2 + U2_CTL, U2_CTL_SIDDQ);
    udelay(10);
    set32(u2 + U2_CTL, U2_CTL_PORT_RESET);
    udelay(10);
    set32(u2 + U2_CTL, U2_CTL_RESET);
    udelay(10);
    clear32(u2 + U2_CTL, U2_CTL_APB_RESETN);
    udelay(10);
    set32(u2 + U2_MISCTUNE, U2_MISCTUNE_GATES);
    udelay(100);

    /* 3. USB2 PHY on, host mode */
    set32(u2 + U2_SIG, U2_SIG_VBUS_FORCE);
    udelay(10);
    clear32(u2 + U2_CTL, U2_CTL_SIDDQ);
    udelay(10);
    clear32(u2 + U2_CTL, U2_CTL_RESET);
    udelay(10);
    clear32(u2 + U2_CTL, U2_CTL_PORT_RESET);
    udelay(10);
    if (core) {
        u32 evt = read32(core + ATC_EVT_USB2_CTL);
        set32(core + ATC_EVT_USB2_CTL, ATC_EVT_USB2_CTL_LOAD_CNT | ATC_EVT_USB2_CTL_EVT_EN);
        udelay(10);
        printf("usb: T8122 EVT_USB2_CTL 0x%x -> 0x%x\n", evt, read32(core + ATC_EVT_USB2_CTL));
    }
    set32(u2 + U2_CTL, U2_CTL_APB_RESETN);
    udelay(10);
    clear32(u2 + U2_MISCTUNE, U2_MISCTUNE_GATES);
    write32(u2 + U2_USBCTL, U2_USBCTL_RUN);
    set32(u2 + U2_SIG, U2_SIG_HOST);
    udelay(50);

    /* 4. DWC3 out of reset */
    clear32(ph + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
    set32(ph + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_RESET_N);
    udelay(100);

    u32 snpsid = read32(dwc3 + GP_DWC3_GSNPSID) & 0xffff0000;
    if (snpsid != 0x33310000 && snpsid != 0x55330000) {
        printf("usb: ordered reset: DWC3 ID 0x%x unexpected, stopping\n",
               read32(dwc3 + GP_DWC3_GSNPSID));
        return -1;
    }

    /* 5. Mu's Dwc3XhciCoreInit + AppleUsbTypeCBringupDxeInitializeUsbController */
    set32(dwc3 + GP_DWC3_GCTL, GP_DWC3_GCTL_CORESOFTRESET);
    set32(dwc3 + GP_DWC3_GUSB3PIPECTL, GP_DWC3_PHYSOFTRST);
    set32(dwc3 + GP_DWC3_GUSB2PHYCFG, GP_DWC3_PHYSOFTRST);
    mdelay(100);
    clear32(dwc3 + GP_DWC3_GUSB3PIPECTL, GP_DWC3_PHYSOFTRST);
    clear32(dwc3 + GP_DWC3_GUSB2PHYCFG, GP_DWC3_PHYSOFTRST);
    mdelay(100);
    clear32(dwc3 + GP_DWC3_GCTL, GP_DWC3_GCTL_CORESOFTRESET);
    /* SCALEDOWN=0, DISSCRAMBLE=0, PRTCAPDIR=host; DSBLCLKGTNG=0 only with clock power-opt */
    u32 gctl_clear = (3 << 4) | BIT(3) | (3 << 12);
    if (((read32(dwc3 + GP_DWC3_GHWPARAMS1) >> 24) & 3) == 1)
        gctl_clear |= BIT(0);
    mask32(dwc3 + GP_DWC3_GCTL, gctl_clear, 1 << 12);
    write32(dwc3 + GP_APPLE_CIO_LFPS, 0x0f800f80);
    write32(dwc3 + GP_APPLE_CIO_BW_NGT, 0x0fc00fc0);
    mask32(dwc3 + GP_APPLE_CIO_LINK_TIMER, 0x00ffffff, 0x00140a10);
    write32(dwc3 + GP_DWC3_GUSB2PHYCFG, read32(dwc3 + GP_DWC3_GUSB2PHYCFG));
    set32(dwc3 + GP_DWC3_GFLADJ, BIT(7) | 0x20);
    mask32(dwc3 + GP_DWC3_GSBUSCFG0, 0xff, 0x0f);
    set32(dwc3 + GP_DWC3_GSBUSCFG1, 0xf00);

    printf("usb: ordered reset done: GCTL=0x%x GUSB2PHYCFG=0x%x SIG=0x%x aon=0x%x\n",
           read32(dwc3 + GP_DWC3_GCTL), read32(dwc3 + GP_DWC3_GUSB2PHYCFG), read32(u2 + U2_SIG),
           read32(ph + PIPEHANDLER_AON_GEN));
    return 0;
}

/* One-line Type-C controller state of the guest port, for the periodic xHCI trace. */
void usb_guest_port_print_status(void)
{
    u8 mode[5] = {0}, status[5] = {0}, power = 0xff, ev[11] = {0};

    if (!usb_guest_tps) {
        printf("HV: hpm1 not initialised\n");
        return;
    }
    int r1 = tps6598x_read(usb_guest_tps, 0x03, mode, 4);
    int r2 = tps6598x_read(usb_guest_tps, 0x1a, status, 4);
    int r3 = tps6598x_read(usb_guest_tps, 0x20, &power, 1);
    int r4 = tps6598x_read(usb_guest_tps, 0x14, ev, 9);
    printf("HV: hpm1 mode='%c%c%c%c'(%d) status=%02x%02x%02x%02x(%d) power=%u(%d) "
           "int_event=%02x%02x%02x%02x%02x%02x%02x%02x%02x(%d)\n",
           mode[0] ? mode[0] : '?', mode[1] ? mode[1] : '?', mode[2] ? mode[2] : '?',
           mode[3] ? mode[3] : '?', r1, status[3], status[2], status[1], status[0], r2, power,
           r3, ev[8], ev[7], ev[6], ev[5], ev[4], ev[3], ev[2], ev[1], ev[0], r4);
}

void usb_hpm_restore_irqs(bool force)
{
    if (usb_has_spmi_hpm()) {
        tps6598x_foreach_hpm(usb_spmi_restore_match, usb_spmi_restore_one, &force, true);
        return;
    }
    if (adt_is_compatible(adt, 0, "J180dAP"))
        usb_i2c_restore_irqs("/arm-io/i2c3", force);
    usb_i2c_restore_irqs("/arm-io/i2c0", force);
}

void usb_iodev_init(void)
{
    for (int i = 0; i < USB_IODEV_COUNT; i++) {
        dwc3_dev_t *opaque;
        struct iodev *usb_iodev;

        opaque = usb_iodev_bringup(i);
        if (!opaque)
            continue;

        usb_iodev = memalign(SPINLOCK_ALIGN, sizeof(*usb_iodev));
        if (!usb_iodev)
            continue;

        usb_iodev->ops = &iodev_usb_ops;
        usb_iodev->opaque = opaque;
        usb_iodev->usage = USAGE_CONSOLE | USAGE_UARTPROXY;
        spin_init(&usb_iodev->lock);

        iodev_register_device(IODEV_USB0 + i, usb_iodev);
        printf("USB%d: initialized at %p\n", i, opaque);
    }
}

void usb_iodev_shutdown(void)
{
    for (int i = 0; i < USB_IODEV_COUNT; i++) {
        struct iodev *usb_iodev = iodev_unregister_device(IODEV_USB0 + i);
        if (!usb_iodev)
            continue;

        printf("USB%d: shutdown\n", i);
        usb_dwc3_shutdown(usb_iodev->opaque);
        free(usb_iodev);
    }
}

void usb_iodev_vuart_setup(iodev_id_t iodev)
{
    if (iodev < IODEV_USB0 || iodev >= IODEV_USB0 + USB_IODEV_COUNT)
        return;

    iodev_usb_vuart.opaque = iodev_get_opaque(iodev);
}
