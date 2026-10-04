/*
 * Realtek RTL838x memory controller and SPI-NOR controller
 *
 * Covers 0x18001000..0x180012ff: the DRAM configuration registers that both
 * rt-loader and the kernel use to size memory, and the SPI-NOR controller
 * with the board's flash chip behind it.
 *
 * The SPI controller is the one spi-realtek-rtl.c drives: a chip select bit
 * per chip in SFCSR, a length field saying whether the next SFDR access moves
 * one byte or four, and SFDR itself, which shifts that many bytes out or in,
 * most significant byte first.  Transfers complete at once, so the ready bit
 * always reads as set.  That bit is load-bearing: the driver spins on it with
 * no timeout at all,
 *
 *     while (!(readl(REG(RTL_SPI_SFCSR)) & RTL_SPI_SFCSR_RDY)) cpu_relax();
 *
 * so a controller that reads back as busy wedges the kernel during spi-nor
 * probe, with no output.
 *
 * The flash is QEMU's m25p80 model of a Macronix MX25L12855E, 16 MiB with
 * 64 KiB sectors, the geometry the GS1900's partition map is laid out for,
 * or the 32 MiB MX25L25635E of an HPE 1920, unless the machine's
 * flash-model property names another chip.  Its contents come from
 * "-drive if=mtd", which must be exactly the chip's size; without one the
 * flash starts erased and forgets everything when QEMU exits.
 *
 * The controller also maps the flash into the address space, 32 MiB of it
 * at 0x14000000 (0xb4000000 through KSEG1), and turns each load from that
 * window into a read on the bus.  Linux never uses it; HPE's Comware reads
 * its BootWare data and its file system through it, from a 32 MiB chip.
 * Going through the bus rather than the backing file means the window sees
 * whatever the guest has written.  The read always carries a four-byte
 * address, which a 32 MiB chip needs for its upper half and takes whether
 * or not the guest has switched it to four-byte mode; a 16 MiB chip wraps
 * it, so it repeats through the upper half as it does on the hardware,
 * which drops the top bits of a three-byte address.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/mips/rtl838x.h"
#include "hw/ssi/ssi.h"
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
#define RTL838X_SPI_SFCSR_CSB0  (1u << 31)  /* chip select 0, active low */
#define RTL838X_SPI_SFCSR_LEN_SHIFT 28      /* bytes per SFDR access - 1 */
#define RTL838X_SPI_SFCSR_RDY   (1u << 27)

#define SPI_NOR_OP_READ4        0x13

#define RTL838X_FLASH_WINDOW_SIZE (32 * 1024 * 1024)

struct RTL838xSocMiscState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion flash_window;
    SSIBus *spi;
    qemu_irq spi_cs;
    uint32_t regs[RTL838X_SOCMISC_SIZE / 4];
};

OBJECT_DECLARE_SIMPLE_TYPE(RTL838xSocMiscState, RTL838X_SOCMISC)

/* How many bytes the next SFDR access moves: SFCSR's length field plus one. */
static unsigned rtl838x_spi_len(RTL838xSocMiscState *s)
{
    return ((s->regs[RTL838X_SPI_SFCSR / 4] >> RTL838X_SPI_SFCSR_LEN_SHIFT)
            & 3) + 1;
}

/*
 * Clocks len bytes through the bus, the most significant byte of the word
 * first -- the order the driver asks for with SFCR's RBO and WBO bits, and
 * the only one modelled.
 */
static uint32_t rtl838x_spi_xfer(RTL838xSocMiscState *s, uint32_t out)
{
    unsigned len = rtl838x_spi_len(s);
    uint32_t in = 0;

    for (unsigned i = 0; i < len; i++) {
        unsigned shift = 24 - 8 * i;

        in |= (ssi_transfer(s->spi, (out >> shift) & 0xff) & 0xff) << shift;
    }
    return in;
}

static uint64_t rtl838x_socmisc_read(void *opaque, hwaddr addr, unsigned size)
{
    RTL838xSocMiscState *s = opaque;

    switch (addr) {
    case RTL838X_SPI_SFCSR:
        /* Never report busy; see the comment at the top of this file. */
        return s->regs[addr / 4] | RTL838X_SPI_SFCSR_RDY;
    case RTL838X_SPI_SFDR:
        return rtl838x_spi_xfer(s, 0);
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
    case RTL838X_SPI_SFCSR:
        s->regs[addr / 4] = val;
        /* The bit is the level of the line, which the flash reads active low. */
        qemu_set_irq(s->spi_cs, !!(val & RTL838X_SPI_SFCSR_CSB0));
        break;
    case RTL838X_SPI_SFDR:
        rtl838x_spi_xfer(s, val);
        break;
    default:
        s->regs[addr / 4] = val;
        break;
    }
}

/*
 * A load from the flash window: one read transaction for the bytes it
 * covers, most significant first, as the bus is big-endian.  A register
 * transfer still holding the chip selected is cut short by it; nothing has
 * been seen to do that, so it is only logged.
 */
static uint64_t rtl838x_flash_window_read(void *opaque, hwaddr addr,
                                          unsigned size)
{
    RTL838xSocMiscState *s = opaque;
    bool selected = !(s->regs[RTL838X_SPI_SFCSR / 4] & RTL838X_SPI_SFCSR_CSB0);
    uint64_t val = 0;

    if (selected) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: flash window read at 0x%"
                      HWADDR_PRIx " while chip select 0 is asserted\n",
                      __func__, addr);
        qemu_set_irq(s->spi_cs, 1);
    }
    qemu_set_irq(s->spi_cs, 0);
    ssi_transfer(s->spi, SPI_NOR_OP_READ4);
    ssi_transfer(s->spi, (addr >> 24) & 0xff);
    ssi_transfer(s->spi, (addr >> 16) & 0xff);
    ssi_transfer(s->spi, (addr >> 8) & 0xff);
    ssi_transfer(s->spi, addr & 0xff);
    for (unsigned i = 0; i < size; i++) {
        val = (val << 8) | (ssi_transfer(s->spi, 0) & 0xff);
    }
    qemu_set_irq(s->spi_cs, 1);
    if (selected) {
        qemu_set_irq(s->spi_cs, 0);
    }
    return val;
}

static void rtl838x_flash_window_write(void *opaque, hwaddr addr,
                                       uint64_t val, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: write to the read-only flash window "
                  "at 0x%" HWADDR_PRIx "\n", __func__, addr);
}

static const MemoryRegionOps rtl838x_flash_window_ops = {
    .read = rtl838x_flash_window_read,
    .write = rtl838x_flash_window_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

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
    /* Both chips deselected. */
    s->regs[RTL838X_SPI_SFCSR / 4] = RTL838X_SPI_SFCSR_CSB0 | (1u << 30);
    qemu_set_irq(s->spi_cs, 1);
}

static void rtl838x_socmisc_init(Object *obj)
{
    RTL838xSocMiscState *s = RTL838X_SOCMISC(obj);

    memory_region_init_io(&s->iomem, obj, &rtl838x_socmisc_ops, s,
                          TYPE_RTL838X_SOCMISC, RTL838X_SOCMISC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    memory_region_init_io(&s->flash_window, obj, &rtl838x_flash_window_ops, s,
                          "rtl838x.flash-window", RTL838X_FLASH_WINDOW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->flash_window);

    s->spi = ssi_create_bus(DEVICE(obj), "spi");
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->spi_cs);
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
