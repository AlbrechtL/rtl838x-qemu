/*
 * Realtek Otto timer (RTL838x variant)
 *
 * Five independent count-up timers at 0x18003100, 0x10 bytes apart.  This is
 * not an optional peripheral: OpenWrt's realtek target drops CSRC_R4K and the
 * Otto driver registers at rating 400 against the MIPS CP0 timer's 300, so
 * this block is both the only clocksource and the clockevent.  If it does not
 * tick, jiffies never advance and every timeout in the rest of boot blocks
 * forever.
 *
 * Each bank counts up from zero at LXB / divisor.  On reaching the end marker
 * in DATA (or on wrapping past the 28-bit maximum when DATA is zero) it fires
 * and restarts at zero; in TIMER mode it keeps running, in COUNTER mode it
 * stops.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/mips/rtl838x.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/timer.h"

#define RTTM_DATA   0x0
#define RTTM_CNT    0x4
#define RTTM_CTRL   0x8
#define RTTM_INT    0xc

#define RTTM_CTRL_ENABLE    (1u << 28)
#define RTTM_CTRL_TIMER     (1u << 24)  /* 1 = repeating, 0 = one-shot */
#define RTTM_CTRL_DIV_MASK  0xffff

#define RTTM_INT_ENABLE     (1u << 20)
#define RTTM_INT_PENDING    (1u << 16)

#define RTTM_COUNT_MAX      0x10000000  /* 28 bits */

OBJECT_DECLARE_SIMPLE_TYPE(RTL838xTimerState, RTL838X_TIMER)

typedef struct RTL838xTimerBank {
    QEMUTimer timer;
    qemu_irq irq;

    uint32_t data;
    uint32_t ctrl_reg;
    uint32_t intr;

    int64_t start_ns;   /* when the current count started */
    bool running;
} RTL838xTimerBank;

struct RTL838xTimerState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    RTL838xTimerBank bank[RTL838X_TIMER_BANKS];
};

static uint64_t bank_period_ns(RTL838xTimerBank *b)
{
    uint32_t div = b->ctrl_reg & RTTM_CTRL_DIV_MASK;

    if (div == 0) {
        div = 1;
    }
    return (NANOSECONDS_PER_SECOND * (uint64_t)div) / RTL838X_LXB_HZ;
}

/* Number of ticks between restarts: the end marker, or a full 28-bit wrap. */
static uint64_t bank_limit(RTL838xTimerBank *b)
{
    uint32_t data = b->data & (RTTM_COUNT_MAX - 1);

    return data ? data : RTTM_COUNT_MAX;
}

static void bank_update_irq(RTL838xTimerBank *b)
{
    bool level = (b->intr & RTTM_INT_PENDING) && (b->intr & RTTM_INT_ENABLE);

    qemu_set_irq(b->irq, level);
}

static void bank_arm(RTL838xTimerBank *b, int64_t from_ns)
{
    b->start_ns = from_ns;
    b->running = true;
    timer_mod_ns(&b->timer, from_ns + bank_limit(b) * bank_period_ns(b));
}

static void bank_stop(RTL838xTimerBank *b)
{
    b->running = false;
    timer_del(&b->timer);
}

static uint32_t bank_count(RTL838xTimerBank *b)
{
    uint64_t elapsed, ticks, limit;

    if (!b->running) {
        return 0;
    }
    elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - b->start_ns;
    ticks = elapsed / bank_period_ns(b);
    limit = bank_limit(b);

    /* The expiry callback may not have run yet; never report past the marker. */
    return ticks >= limit ? limit - 1 : ticks;
}

static void bank_expire(void *opaque)
{
    RTL838xTimerBank *b = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    b->intr |= RTTM_INT_PENDING;

    if (b->ctrl_reg & RTTM_CTRL_TIMER) {
        bank_arm(b, now);
    } else {
        bank_stop(b);
    }
    bank_update_irq(b);
}

static uint64_t rtl838x_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    RTL838xTimerState *s = opaque;
    RTL838xTimerBank *b = &s->bank[addr / 0x10];

    switch (addr % 0x10) {
    case RTTM_DATA:
        return b->data;
    case RTTM_CNT:
        return bank_count(b);
    case RTTM_CTRL:
        return b->ctrl_reg;
    case RTTM_INT:
        return b->intr;
    default:
        return 0;
    }
}

static void rtl838x_timer_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    RTL838xTimerState *s = opaque;
    RTL838xTimerBank *b = &s->bank[addr / 0x10];

    switch (addr % 0x10) {
    case RTTM_DATA:
        b->data = val & (RTTM_COUNT_MAX - 1);
        if (b->running) {
            bank_arm(b, b->start_ns);
        }
        break;

    case RTTM_CNT:
        /* Counter is read-only. */
        break;

    case RTTM_CTRL:
        /* Any write to CTRL restarts the count from zero. */
        b->ctrl_reg = val;
        if (val & RTTM_CTRL_ENABLE) {
            bank_arm(b, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        } else {
            bank_stop(b);
        }
        break;

    case RTTM_INT: {
        /* Write-one-to-clear on the pending bit; a zero leaves it alone. */
        uint32_t pending = b->intr & RTTM_INT_PENDING;

        if (val & RTTM_INT_PENDING) {
            pending = 0;
        }
        b->intr = (val & RTTM_INT_ENABLE) | pending;
        bank_update_irq(b);
        break;
    }
    }
}

static const MemoryRegionOps rtl838x_timer_ops = {
    .read = rtl838x_timer_read,
    .write = rtl838x_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void rtl838x_timer_reset(DeviceState *dev)
{
    RTL838xTimerState *s = RTL838X_TIMER(dev);

    for (unsigned i = 0; i < RTL838X_TIMER_BANKS; i++) {
        RTL838xTimerBank *b = &s->bank[i];

        bank_stop(b);
        b->data = 0;
        b->ctrl_reg = 0;
        b->intr = 0;
        b->start_ns = 0;
        bank_update_irq(b);
    }
}

static void rtl838x_timer_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RTL838xTimerState *s = RTL838X_TIMER(obj);

    memory_region_init_io(&s->iomem, obj, &rtl838x_timer_ops, s,
                          TYPE_RTL838X_TIMER, RTL838X_TIMER_BANKS * 0x10);
    sysbus_init_mmio(sbd, &s->iomem);

    for (unsigned i = 0; i < RTL838X_TIMER_BANKS; i++) {
        timer_init_ns(&s->bank[i].timer, QEMU_CLOCK_VIRTUAL, bank_expire,
                      &s->bank[i]);
        sysbus_init_irq(sbd, &s->bank[i].irq);
    }
}

static const VMStateDescription vmstate_rtl838x_timer_bank = {
    .name = "rtl838x-timer-bank",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER(timer, RTL838xTimerBank),
        VMSTATE_UINT32(data, RTL838xTimerBank),
        VMSTATE_UINT32(ctrl_reg, RTL838xTimerBank),
        VMSTATE_UINT32(intr, RTL838xTimerBank),
        VMSTATE_INT64(start_ns, RTL838xTimerBank),
        VMSTATE_BOOL(running, RTL838xTimerBank),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_rtl838x_timer = {
    .name = TYPE_RTL838X_TIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(bank, RTL838xTimerState, RTL838X_TIMER_BANKS, 1,
                             vmstate_rtl838x_timer_bank, RTL838xTimerBank),
        VMSTATE_END_OF_LIST()
    }
};

static void rtl838x_timer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->vmsd = &vmstate_rtl838x_timer;
    device_class_set_legacy_reset(dc, rtl838x_timer_reset);
}

static const TypeInfo rtl838x_timer_types[] = {
    {
        .name          = TYPE_RTL838X_TIMER,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RTL838xTimerState),
        .instance_init = rtl838x_timer_init,
        .class_init    = rtl838x_timer_class_init,
    },
};

DEFINE_TYPES(rtl838x_timer_types)
