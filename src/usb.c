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
void usb_guest_port_phy_refresh(void)
{
    usb_guest_port_phy_print("before-refresh");
    int ret = usb_phy_bringup(1);
    usb_guest_port_phy_print("after-refresh");
    printf("HV: phy1 refresh ret=%d\n", ret);
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
