/*
 * Realtek Otto GPIO controller (RTL838x variant)
 *
 * 24 lines at 0x18003500.  Nothing outside drives a line, but the board may
 * pull some up, which the pull-ups property says: such a line, configured
 * as an input, reads 1.  On the TSW2xx that is what makes an empty SFP cage
 * read as empty, the reset button as released, and a bit-banged I2C bus with
 * nothing on it see nobody acknowledge, rather than a stuck-low clock that
 * each transfer waits out.  The GS1900 has none that matter, and Zyxel's
 * firmware reads its board straps from these lines and does not recognise
 * the board with them all high.  Every other line reads back what was last
 * written to it.  Otherwise plain storage plus write-one-to-clear on the
 * status register is enough to keep gpio-realtek-otto happy.
 *
 * Netgear's GS108Tv3 family hangs an RTL8231 GPIO expander off two of these
 * lines, A2 as MDC and A3 as MDIO, which the firmware bit-bangs, and reads
 * the board's model from four of the expander's pins.  With the rtl8231
 * property the expander is there: it answers clause 22 frames at any
 * address, keeps its 32 registers, and its data registers read the
 * rtl8231-straps property for the pins its direction registers make
 * inputs.  The switch's EXT_GPIO_INDRT_ACCESS engine, which drives the same
 * two lines in hardware, reaches the same registers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/mips/rtl838x.h"
#include "migration/vmstate.h"

#define RTL838X_GPIO_SIZE   0x20

#define RTL838X_GPIO_CNR    0x00
#define RTL838X_GPIO_DIR    0x08    /* set = output */
#define RTL838X_GPIO_DATA   0x0c
#define RTL838X_GPIO_ISR    0x10
#define RTL838X_GPIO_IMR_AB 0x14
#define RTL838X_GPIO_IMR_CD 0x18

#define RTL838X_GPIO_LINES  24

/* The bit-banged MDIO bus to the RTL8231, as data register bits. */
#define RTL8231_MDC         (1u << 26)      /* A2 */
#define RTL8231_MDIO        (1u << 27)      /* A3 */

#define RTL8231_REGS        32
#define RTL8231_PIN_CTRL    0x04    /* pins 32..36: bits 9:5 set = input */
#define RTL8231_DIR0        0x05    /* pins 0..15, set = input */
#define RTL8231_DIR1        0x06    /* pins 16..31 */
#define RTL8231_DATA0       0x1c    /* pins 0..15 */
#define RTL8231_DATA2       0x1e    /* pins 32..36 */

/* A frame: at least 32 ones, then ST, OP, PHYAD and REGAD, 14 bits. */
#define MDIO_PREAMBLE       32
#define MDIO_HEADER_BITS    13      /* after the start bit's 0 */
#define MDIO_OP_WRITE       1
#define MDIO_OP_READ        2

typedef enum {
    MDIO_IDLE,
    MDIO_HEADER,
    MDIO_READ,
    MDIO_WRITE,
} MdioPhase;

struct RTL838xGpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[RTL838X_GPIO_SIZE / 4];
    uint32_t pullups;

    bool rtl8231;
    uint64_t straps;            /* what the RTL8231's input pins read */
    uint16_t rtl8231_regs[RTL8231_REGS];
    uint32_t mdio_phase;
    uint32_t mdio_count;        /* ones of the preamble, or bits of a phase */
    uint32_t mdio_frame;
    bool mdio_drive;            /* the RTL8231 drives MDIO, with mdio_out */
    bool mdio_out;
};


/* A data register reads the straps for its input pins, the latch for the rest. */
static uint16_t rtl8231_read(RTL838xGpioState *s, unsigned reg)
{
    uint16_t inputs, straps;

    if (reg < RTL8231_DATA0 || reg > RTL8231_DATA2) {
        return s->rtl8231_regs[reg];
    }
    if (reg == RTL8231_DATA2) {
        inputs = extract32(s->rtl8231_regs[RTL8231_PIN_CTRL], 5, 5);
    } else {
        inputs = s->rtl8231_regs[RTL8231_DIR0 + reg - RTL8231_DATA0];
    }
    straps = extract64(s->straps, (reg - RTL8231_DATA0) * 16, 16);
    return (s->rtl8231_regs[reg] & ~inputs) | (straps & inputs);
}

bool rtl838x_gpio_has_rtl8231(RTL838xGpioState *s)
{
    return s->rtl8231;
}

uint16_t rtl838x_gpio_rtl8231_read(RTL838xGpioState *s, unsigned reg)
{
    return rtl8231_read(s, reg);
}

void rtl838x_gpio_rtl8231_write(RTL838xGpioState *s, unsigned reg,
                                uint16_t val)
{
    s->rtl8231_regs[reg] = val;
}

/* What the MDIO line carries: the SoC, the RTL8231, or the pull-up. */
static bool rtl8231_mdio_line(RTL838xGpioState *s)
{
    if (s->regs[RTL838X_GPIO_DIR / 4] & RTL8231_MDIO) {
        return s->regs[RTL838X_GPIO_DATA / 4] & RTL8231_MDIO;
    }
    return s->mdio_drive ? s->mdio_out : true;
}

/*
 * A rising edge on MDC: the RTL8231 samples MDIO, and in a read moves on to
 * the next bit it drives, which the SoC samples before the next edge.  The
 * firmware gives a read one turnaround clock and then reads 16 bits.
 */
static void rtl8231_clock(RTL838xGpioState *s)
{
    bool bit = rtl8231_mdio_line(s);
    unsigned reg;

    switch (s->mdio_phase) {
    case MDIO_IDLE:
        if (bit) {
            s->mdio_count++;
        } else {
            s->mdio_phase = s->mdio_count >= MDIO_PREAMBLE ? MDIO_HEADER
                                                           : MDIO_IDLE;
            s->mdio_count = 0;
            s->mdio_frame = 0;
        }
        return;
    case MDIO_HEADER:
        s->mdio_frame = (s->mdio_frame << 1) | bit;
        if (++s->mdio_count < MDIO_HEADER_BITS) {
            return;
        }
        s->mdio_count = 0;
        if (!extract32(s->mdio_frame, 12, 1)) {
            s->mdio_phase = MDIO_IDLE;
        } else if (extract32(s->mdio_frame, 10, 2) == MDIO_OP_READ) {
            s->mdio_phase = MDIO_READ;
            s->mdio_frame = rtl8231_read(s, s->mdio_frame & 0x1f);
        } else if (extract32(s->mdio_frame, 10, 2) == MDIO_OP_WRITE) {
            s->mdio_phase = MDIO_WRITE;
        } else {
            s->mdio_phase = MDIO_IDLE;
        }
        return;
    case MDIO_READ:
        s->mdio_count++;
        if (s->mdio_count <= 17) {
            /* The turnaround's 0, then bits 15..0. */
            s->mdio_drive = true;
            s->mdio_out = s->mdio_count > 1 &&
                          extract32(s->mdio_frame, 17 - s->mdio_count, 1);
            return;
        }
        s->mdio_drive = false;
        s->mdio_phase = MDIO_IDLE;
        s->mdio_count = rtl8231_mdio_line(s);
        return;
    case MDIO_WRITE:
        /* REGAD stays in the low bits, below the turnaround and the data. */
        s->mdio_frame = (s->mdio_frame << 1) | bit;
        if (++s->mdio_count == 18) {
            reg = extract32(s->mdio_frame, 18, 5);
            s->rtl8231_regs[reg] = s->mdio_frame & 0xffff;
            s->mdio_phase = MDIO_IDLE;
            s->mdio_count = 0;
        }
        return;
    }
}

static uint64_t rtl838x_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    RTL838xGpioState *s = opaque;

    if (addr == RTL838X_GPIO_DATA) {
        uint32_t pulled = ~s->regs[RTL838X_GPIO_DIR / 4] & s->pullups;
        uint32_t val = s->regs[addr / 4] | pulled;

        if (s->rtl8231 && !(s->regs[RTL838X_GPIO_DIR / 4] & RTL8231_MDIO)) {
            val = deposit32(val, ctz32(RTL8231_MDIO), 1,
                            rtl8231_mdio_line(s));
        }
        return val;
    }
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
    if (s->rtl8231 && addr == RTL838X_GPIO_DATA &&
        !(s->regs[addr / 4] & RTL8231_MDC) && (val & RTL8231_MDC) &&
        (s->regs[RTL838X_GPIO_DIR / 4] & RTL8231_MDC)) {
        s->regs[addr / 4] = val;
        rtl8231_clock(s);
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

    /* The RTL8231 comes up with every pin an input. */
    memset(s->rtl8231_regs, 0, sizeof(s->rtl8231_regs));
    s->rtl8231_regs[RTL8231_PIN_CTRL] = 0x1f << 5;
    s->rtl8231_regs[RTL8231_DIR0] = 0xffff;
    s->rtl8231_regs[RTL8231_DIR1] = 0xffff;
    s->mdio_phase = MDIO_IDLE;
    s->mdio_count = 0;
    s->mdio_drive = false;
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
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RTL838xGpioState, RTL838X_GPIO_SIZE / 4),
        VMSTATE_UINT16_ARRAY(rtl8231_regs, RTL838xGpioState, RTL8231_REGS),
        VMSTATE_UINT32(mdio_phase, RTL838xGpioState),
        VMSTATE_UINT32(mdio_count, RTL838xGpioState),
        VMSTATE_UINT32(mdio_frame, RTL838xGpioState),
        VMSTATE_BOOL(mdio_drive, RTL838xGpioState),
        VMSTATE_BOOL(mdio_out, RTL838xGpioState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property rtl838x_gpio_properties[] = {
    DEFINE_PROP_UINT32("pull-ups", RTL838xGpioState, pullups, 0),
    DEFINE_PROP_BOOL("rtl8231", RTL838xGpioState, rtl8231, false),
    DEFINE_PROP_UINT64("rtl8231-straps", RTL838xGpioState, straps, 0),
};

static void rtl838x_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, rtl838x_gpio_properties);
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
