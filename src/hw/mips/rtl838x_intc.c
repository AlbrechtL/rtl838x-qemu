/*
 * Realtek RTL838x SoC interrupt controller
 *
 * Models the "realtek,rtl8380-intc" block at 0x18003000.  Thirty-two SoC
 * interrupt sources are multiplexed onto five outputs, which the board wires
 * to the MIPS core's IP2..IP6 lines.  Each source has a 4-bit routing nibble
 * in one of the four IRR registers; the driver writes (parent_hwirq - 1)
 * there, so nibble value v selects CPU line IP(v+1), i.e. our output v - 1.
 * A nibble of 0 means "not routed".
 *
 * The IRR registers use an inverted numbering that is easy to get wrong:
 * source i lives in IRR[3 - (i * 4) / 32] at bit (i * 4) % 32, so source 31
 * is in IRR0 bits [3:0] and source 0 is in IRR3 bits [31:28].
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/mips/rtl838x.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

#define RTL838X_INTC_GIMR   0x00
#define RTL838X_INTC_GISR   0x04
#define RTL838X_INTC_IRR0   0x08
#define RTL838X_INTC_IRR3   0x14

static inline unsigned intc_irr_index(unsigned src)
{
    return 3 - (src * 4) / 32;
}

static inline unsigned intc_irr_shift(unsigned src)
{
    return (src * 4) % 32;
}

static unsigned intc_route(RTL838xIntcState *s, unsigned src)
{
    return (s->irr[intc_irr_index(src)] >> intc_irr_shift(src)) & 0xf;
}

static void rtl838x_intc_update(RTL838xIntcState *s)
{
    uint32_t active = s->gimr & s->gisr;
    bool level[RTL838X_INTC_NUM_OUT] = { };

    for (unsigned src = 0; src < RTL838X_INTC_NUM_IRQ; src++) {
        unsigned route;

        if (!(active & (1u << src))) {
            continue;
        }
        route = intc_route(s, src);
        if (route >= 1 && route <= RTL838X_INTC_NUM_OUT) {
            level[route - 1] = true;
        }
    }

    for (unsigned i = 0; i < RTL838X_INTC_NUM_OUT; i++) {
        qemu_set_irq(s->out[i], level[i]);
    }
}

/*
 * Sources are level triggered: the status bit follows the input line and is
 * cleared by the device model, not by a write from the guest.
 */
static void rtl838x_intc_set_irq(void *opaque, int src, int level)
{
    RTL838xIntcState *s = opaque;

    if (level) {
        s->gisr |= 1u << src;
    } else {
        s->gisr &= ~(1u << src);
    }
    rtl838x_intc_update(s);
}

static uint64_t rtl838x_intc_read(void *opaque, hwaddr addr, unsigned size)
{
    RTL838xIntcState *s = opaque;

    switch (addr) {
    case RTL838X_INTC_GIMR:
        return s->gimr;
    case RTL838X_INTC_GISR:
        return s->gisr;
    case RTL838X_INTC_IRR0 ... RTL838X_INTC_IRR3:
        return s->irr[(addr - RTL838X_INTC_IRR0) / 4];
    default:
        qemu_log_mask(LOG_UNIMP, "rtl838x-intc: read from 0x%" HWADDR_PRIx "\n",
                      addr);
        return 0;
    }
}

static void rtl838x_intc_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    RTL838xIntcState *s = opaque;

    switch (addr) {
    case RTL838X_INTC_GIMR:
        s->gimr = val;
        break;
    case RTL838X_INTC_GISR:
        /* Status is driven by the sources; writes are ignored. */
        break;
    case RTL838X_INTC_IRR0 ... RTL838X_INTC_IRR3:
        s->irr[(addr - RTL838X_INTC_IRR0) / 4] = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "rtl838x-intc: write 0x%" PRIx64 " to 0x%" HWADDR_PRIx "\n",
                      val, addr);
        return;
    }
    rtl838x_intc_update(s);
}

static const MemoryRegionOps rtl838x_intc_ops = {
    .read = rtl838x_intc_read,
    .write = rtl838x_intc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void rtl838x_intc_reset(DeviceState *dev)
{
    RTL838xIntcState *s = RTL838X_INTC(dev);

    s->gimr = 0;
    s->gisr = 0;
    memset(s->irr, 0, sizeof(s->irr));
    rtl838x_intc_update(s);
}

static void rtl838x_intc_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RTL838xIntcState *s = RTL838X_INTC(obj);

    memory_region_init_io(&s->iomem, obj, &rtl838x_intc_ops, s,
                          TYPE_RTL838X_INTC, 0x18);
    sysbus_init_mmio(sbd, &s->iomem);

    for (unsigned i = 0; i < RTL838X_INTC_NUM_OUT; i++) {
        sysbus_init_irq(sbd, &s->out[i]);
    }
    qdev_init_gpio_in(DEVICE(obj), rtl838x_intc_set_irq, RTL838X_INTC_NUM_IRQ);
}

static const VMStateDescription vmstate_rtl838x_intc = {
    .name = TYPE_RTL838X_INTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gimr, RTL838xIntcState),
        VMSTATE_UINT32(gisr, RTL838xIntcState),
        VMSTATE_UINT32_ARRAY(irr, RTL838xIntcState, 4),
        VMSTATE_END_OF_LIST()
    }
};

static void rtl838x_intc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->vmsd = &vmstate_rtl838x_intc;
    device_class_set_legacy_reset(dc, rtl838x_intc_reset);
}

static const TypeInfo rtl838x_intc_types[] = {
    {
        .name          = TYPE_RTL838X_INTC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RTL838xIntcState),
        .instance_init = rtl838x_intc_init,
        .class_init    = rtl838x_intc_class_init,
    },
};

DEFINE_TYPES(rtl838x_intc_types)
