/*
 * Realtek Otto GPIO controller (RTL838x variant)
 *
 * 24 lines at 0x18003500.  Nothing is polled here and no line is driven by
 * the board, so plain storage plus write-one-to-clear on the status register
 * is enough to keep gpio-realtek-otto happy.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/mips/rtl838x.h"
#include "migration/vmstate.h"

#define RTL838X_GPIO_SIZE   0x20

#define RTL838X_GPIO_CNR    0x00
#define RTL838X_GPIO_DIR    0x08
#define RTL838X_GPIO_DATA   0x0c
#define RTL838X_GPIO_ISR    0x10
#define RTL838X_GPIO_IMR_AB 0x14
#define RTL838X_GPIO_IMR_CD 0x18

#define RTL838X_GPIO_LINES  24

struct RTL838xGpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[RTL838X_GPIO_SIZE / 4];
};

OBJECT_DECLARE_SIMPLE_TYPE(RTL838xGpioState, RTL838X_GPIO)

static uint64_t rtl838x_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    RTL838xGpioState *s = opaque;

    return s->regs[addr / 4];
}

static void rtl838x_gpio_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    RTL838xGpioState *s = opaque;

    if (addr == RTL838X_GPIO_ISR) {
        s->regs[addr / 4] &= ~(uint32_t)val;
        qemu_set_irq(s->irq, s->regs[addr / 4] != 0);
        return;
    }
    s->regs[addr / 4] = val;
}

static const MemoryRegionOps rtl838x_gpio_ops = {
    .read = rtl838x_gpio_read,
    .write = rtl838x_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void rtl838x_gpio_reset(DeviceState *dev)
{
    RTL838xGpioState *s = RTL838X_GPIO(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void rtl838x_gpio_init(Object *obj)
{
    RTL838xGpioState *s = RTL838X_GPIO(obj);

    memory_region_init_io(&s->iomem, obj, &rtl838x_gpio_ops, s,
                          TYPE_RTL838X_GPIO, RTL838X_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_rtl838x_gpio = {
    .name = TYPE_RTL838X_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RTL838xGpioState, RTL838X_GPIO_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void rtl838x_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->vmsd = &vmstate_rtl838x_gpio;
    device_class_set_legacy_reset(dc, rtl838x_gpio_reset);
}

static const TypeInfo rtl838x_gpio_types[] = {
    {
        .name          = TYPE_RTL838X_GPIO,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RTL838xGpioState),
        .instance_init = rtl838x_gpio_init,
        .class_init    = rtl838x_gpio_class_init,
    },
};

DEFINE_TYPES(rtl838x_gpio_types)
