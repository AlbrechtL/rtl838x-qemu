/*
 * Realtek RTL838x memory controller and SPI-NOR controller stub
 *
 * Covers 0x18001000..0x180012ff: the DRAM configuration registers that both
 * rt-loader and the kernel use to size memory, and the SPI-NOR controller.
 *
 * The SPI part is a stub with one load-bearing detail.  spi-realtek-rtl.c
 * spins on the SFCSR ready bit with no timeout at all:
 *
 *     while (!(readl(REG(RTL_SPI_SFCSR)) & RTL_SPI_SFCSR_RDY)) cpu_relax();
 *
 * so a window that reads back as zero wedges the kernel during spi-nor probe,
 * with no output.  Reporting "always ready" and returning all-ones data makes
 * the JEDEC ID read fail cleanly instead, and boot continues.  Modelling the
 * flash for real is left for when persistent config / sysupgrade is wanted.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/mips/rtl838x.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

#define RTL838X_SOCMISC_SIZE    0x300

/* Memory controller, relative to 0x18001000. */
#define RTL838X_MC_MCR          0x000
#define RTL838X_MC_DCR          0x004

/*
 * DRAM config decoded by the kernel as
 *   bits = ((d>>28)&3) + ((d>>24)&3) + ((d>>20)&0xf) + ((d>>16)&0xf) + 20
 * so 4 + 3 + 20 = 27 reports 128 MiB, matching the device tree's memory node.
 */
#define RTL838X_MC_DCR_128MB    0x00430000

/* SPI-NOR controller, relative to 0x18001000. */
#define RTL838X_SPI_SFCR        0x200
#define RTL838X_SPI_SFCR2       0x204
#define RTL838X_SPI_SFCSR       0x208
#define RTL838X_SPI_SFDR        0x20c
#define RTL838X_SPI_SFCSR_RDY   (1u << 27)

struct RTL838xSocMiscState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[RTL838X_SOCMISC_SIZE / 4];
};

OBJECT_DECLARE_SIMPLE_TYPE(RTL838xSocMiscState, RTL838X_SOCMISC)

static uint64_t rtl838x_socmisc_read(void *opaque, hwaddr addr, unsigned size)
{
    RTL838xSocMiscState *s = opaque;

    switch (addr) {
    case RTL838X_SPI_SFCSR:
        /* Never report busy; see the comment at the top of this file. */
        return s->regs[addr / 4] | RTL838X_SPI_SFCSR_RDY;
    case RTL838X_SPI_SFDR:
        return 0xffffffff;
    default:
        return s->regs[addr / 4];
    }
}

static void rtl838x_socmisc_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    RTL838xSocMiscState *s = opaque;

    switch (addr) {
    case RTL838X_MC_MCR:
    case RTL838X_MC_DCR:
        /* Read-only to the guest: the board decides how much DRAM there is. */
        break;
    default:
        s->regs[addr / 4] = val;
        break;
    }
}

static const MemoryRegionOps rtl838x_socmisc_ops = {
    .read = rtl838x_socmisc_read,
    .write = rtl838x_socmisc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void rtl838x_socmisc_reset(DeviceState *dev)
{
    RTL838xSocMiscState *s = RTL838X_SOCMISC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[RTL838X_MC_DCR / 4] = RTL838X_MC_DCR_128MB;
}

static void rtl838x_socmisc_init(Object *obj)
{
    RTL838xSocMiscState *s = RTL838X_SOCMISC(obj);

    memory_region_init_io(&s->iomem, obj, &rtl838x_socmisc_ops, s,
                          TYPE_RTL838X_SOCMISC, RTL838X_SOCMISC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_rtl838x_socmisc = {
    .name = TYPE_RTL838X_SOCMISC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RTL838xSocMiscState,
                             RTL838X_SOCMISC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void rtl838x_socmisc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->vmsd = &vmstate_rtl838x_socmisc;
    device_class_set_legacy_reset(dc, rtl838x_socmisc_reset);
}

static const TypeInfo rtl838x_socmisc_types[] = {
    {
        .name          = TYPE_RTL838X_SOCMISC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RTL838xSocMiscState),
        .instance_init = rtl838x_socmisc_init,
        .class_init    = rtl838x_socmisc_class_init,
    },
};

DEFINE_TYPES(rtl838x_socmisc_types)
