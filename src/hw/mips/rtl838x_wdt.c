/*
 * Realtek Otto watchdog (RTL838x variant)
 *
 * Two-phase watchdog at 0x18003150.  Phase 1 raises an interrupt, phase 2
 * resets the SoC.  The reset matters beyond watchdog duty: realtek_otto_wdt.c
 * implements the machine restart handler by writing CTRL = RST_MODE | ENABLE
 * and then busy-waiting, so without a working reset "reboot" inside the guest
 * just halts.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/mips/rtl838x.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "system/runstate.h"

#define RTL838X_WDT_CNTR    0x0
#define RTL838X_WDT_INTR    0x4
#define RTL838X_WDT_CTRL    0x8
#define RTL838X_WDT_SIZE    0xc

#define RTL838X_WDT_CNTR_PING       (1u << 31)

#define RTL838X_WDT_INTR_PHASE1     (1u << 31)
#define RTL838X_WDT_INTR_PHASE2     (1u << 30)

#define RTL838X_WDT_CTRL_ENABLE     (1u << 31)
#define RTL838X_WDT_CTRL_PRESCALE   (3u << 29)
#define RTL838X_WDT_CTRL_PHASE1(v)  (((v) >> 22) & 0x1f)
#define RTL838X_WDT_CTRL_PHASE2(v)  (((v) >> 15) & 0x1f)

struct RTL838xWdtState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq_phase1;
    qemu_irq irq_phase2;
    QEMUTimer timer;

    uint32_t intr;
    uint32_t ctrl;
    bool phase1_done;
};

OBJECT_DECLARE_SIMPLE_TYPE(RTL838xWdtState, RTL838X_WDT)

/* Base tick is (1 << (25 + prescale)) / clk_khz milliseconds. */
static uint64_t wdt_tick_ns(RTL838xWdtState *s)
{
    unsigned prescale = (s->ctrl >> 29) & 3;

    return ((uint64_t)1 << (25 + prescale)) * NANOSECONDS_PER_SECOND
           / RTL838X_LXB_HZ;
}

static void rtl838x_wdt_arm(RTL838xWdtState *s)
{
    uint64_t phases;

    timer_del(&s->timer);
    if (!(s->ctrl & RTL838X_WDT_CTRL_ENABLE)) {
        return;
    }
    phases = s->phase1_done ? RTL838X_WDT_CTRL_PHASE2(s->ctrl) + 1
                            : RTL838X_WDT_CTRL_PHASE1(s->ctrl) + 1;
    timer_mod_ns(&s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                            + phases * wdt_tick_ns(s));
}

static void rtl838x_wdt_expire(void *opaque)
{
    RTL838xWdtState *s = opaque;

    if (!s->phase1_done) {
        s->phase1_done = true;
        s->intr |= RTL838X_WDT_INTR_PHASE1;
        qemu_set_irq(s->irq_phase1, 1);
        rtl838x_wdt_arm(s);
        return;
    }

    s->intr |= RTL838X_WDT_INTR_PHASE2;
    qemu_set_irq(s->irq_phase2, 1);
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static uint64_t rtl838x_wdt_read(void *opaque, hwaddr addr, unsigned size)
{
    RTL838xWdtState *s = opaque;

    switch (addr) {
    case RTL838X_WDT_INTR:
        return s->intr;
    case RTL838X_WDT_CTRL:
        return s->ctrl;
    default:
        return 0;
    }
}

static void rtl838x_wdt_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    RTL838xWdtState *s = opaque;

    switch (addr) {
    case RTL838X_WDT_CNTR:
        if (val & RTL838X_WDT_CNTR_PING) {
            s->phase1_done = false;
            qemu_set_irq(s->irq_phase1, 0);
            rtl838x_wdt_arm(s);
        }
        break;

    case RTL838X_WDT_INTR:
        s->intr &= ~(uint32_t)val;
        if (val & RTL838X_WDT_INTR_PHASE1) {
            qemu_set_irq(s->irq_phase1, 0);
        }
        if (val & RTL838X_WDT_INTR_PHASE2) {
            qemu_set_irq(s->irq_phase2, 0);
        }
        break;

    case RTL838X_WDT_CTRL:
        s->ctrl = val;
        s->phase1_done = false;
        rtl838x_wdt_arm(s);
        break;
    }
}

static const MemoryRegionOps rtl838x_wdt_ops = {
    .read = rtl838x_wdt_read,
    .write = rtl838x_wdt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void rtl838x_wdt_reset(DeviceState *dev)
{
    RTL838xWdtState *s = RTL838X_WDT(dev);

    timer_del(&s->timer);
    s->intr = 0;
    s->ctrl = 0;
    s->phase1_done = false;
    qemu_set_irq(s->irq_phase1, 0);
    qemu_set_irq(s->irq_phase2, 0);
}

static void rtl838x_wdt_init(Object *obj)
{
    RTL838xWdtState *s = RTL838X_WDT(obj);

    memory_region_init_io(&s->iomem, obj, &rtl838x_wdt_ops, s,
                          TYPE_RTL838X_WDT, RTL838X_WDT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_phase1);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_phase2);
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, rtl838x_wdt_expire, s);
}

static const VMStateDescription vmstate_rtl838x_wdt = {
    .name = TYPE_RTL838X_WDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER(timer, RTL838xWdtState),
        VMSTATE_UINT32(intr, RTL838xWdtState),
        VMSTATE_UINT32(ctrl, RTL838xWdtState),
        VMSTATE_BOOL(phase1_done, RTL838xWdtState),
        VMSTATE_END_OF_LIST()
    }
};

static void rtl838x_wdt_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->vmsd = &vmstate_rtl838x_wdt;
    device_class_set_legacy_reset(dc, rtl838x_wdt_reset);
}

static const TypeInfo rtl838x_wdt_types[] = {
    {
        .name          = TYPE_RTL838X_WDT,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RTL838xWdtState),
        .instance_init = rtl838x_wdt_init,
        .class_init    = rtl838x_wdt_class_init,
    },
};

DEFINE_TYPES(rtl838x_wdt_types)
