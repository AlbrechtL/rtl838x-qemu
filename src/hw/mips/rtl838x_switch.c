/*
 * Realtek RTL838x switch core register window
 *
 * The 64 KiB window at 0x1b000000 is one big syscon that everything on this
 * SoC reaches into: SoC identification, the PLLs the clock driver decodes,
 * the thermal sensor, the pin multiplexers, the switch table access engine,
 * the MDIO command engine and the per-port MAC registers.  The DSA, MDIO,
 * table and ethernet drivers are all built into the kernel this machine is
 * meant to run, so they probe whether or not we want them to; leaving this
 * window as dumb storage does not merely lose functionality, it hangs the
 * boot.  Three registers here are busy-waited on with no timeout at all:
 *
 *   0x6168 bit 0   ACL clear, spun on by rtl838x_pie_rule_del() during DSA
 *                  probe -- the first wall anyone hits, and it is silent
 *   0x3370 bit 26  L2 table flush, spun on by rtldsa_838x_fast_age()
 *   0x003c bits2,3 NIC reset (bounded at 1 s, but wrong if it never clears)
 *
 * so every one of those has to complete instantly here.
 *
 * What is modelled: identification, clocks, thermal, the self-clearing and
 * write-one-to-clear bits, the table access engine with real backing store,
 * the MDIO command engine and eight internal PHYs reporting 1000/full.  What
 * is not (yet): the packet data path, which is what phase 2 adds.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/mips/rtl838x.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

#define SW_REGS         (RTL838X_SW_SIZE / 4)

/*
 * Set to 1 to trace every MDIO transaction.  Bringing a new PHY behaviour up
 * is mostly a matter of watching which (port, page, register) the driver asks
 * for and what it does with the answer, so keep this compiled in.
 */
#define RTL838X_MDIO_DEBUG 0

#define mdio_dbg(fmt, ...)                                          \
    do {                                                            \
        if (RTL838X_MDIO_DEBUG) {                                   \
            qemu_log("rtl838x-mdio: " fmt, ## __VA_ARGS__);         \
        }                                                           \
    } while (0)

/* Identification, poked by the platform's early setup code. */
#define SW_INT_RW_CTRL          0x0058
#define SW_EXT_VERSION          0x00d0
#define SW_MODEL_NAME_INFO      0x00d4
#define SW_CHIP_INFO            0x00d8
#define SW_PLL_CML_CTRL         0x0ff8

/*
 * MODEL_NAME_INFO decodes as id = [31:16] and a suffix letter
 * 'A' + [15:11] - 1, so 0x8380 with 13 spells "RTL8380M".
 */
#define SW_MODEL_RTL8380M       0x83806800
/* CHIP_INFO reads back revision in [20:16] and cpu id in [15:0]. */
#define SW_CHIP_INFO_UNLOCK     0xa0000000
#define SW_CHIP_INFO_VALUE      0x00010000

/*
 * PLL registers, primed with the values the vendor bootloader leaves behind.
 * clk-rtl83xx.c decodes these back into 500 MHz CPU / 200 MHz LXB, and the
 * UART, Otto timer and watchdog all derive their rates from LXB.  Leaving
 * them at all-ones would divide by zero in the watchdog and cook the timers.
 */
#define SW_PLL_CPU_CTRL0        0x0fc4
#define SW_PLL_CPU_CTRL1        0x0fc8
#define SW_PLL_LXB_CTRL0        0x0fd0
#define SW_PLL_LXB_CTRL1        0x0fd4
#define SW_PLL_MEM_CTRL0        0x0fdc
#define SW_PLL_MEM_CTRL1        0x0fe0

/*
 * Thermal sensor.  The result register must not read as all-ones: that means
 * valid plus 127 degrees, which trips the device tree's 105 degree critical
 * point and powers the machine off about a second into boot.
 */
#define SW_THERMAL_CTRL0        0x0098
#define SW_THERMAL_RESULT       0x00a4
#define SW_THERMAL_VALID        (1u << 8)
#define SW_THERMAL_DEGREES      45

/* Self-clearing command bits. */
#define SW_RST_GLB_CTRL_0       0x003c
#define SW_RST_GLB_SELF_CLEAR   0x0000001c  /* NIC, queue and serdes resets */
#define SW_ACL_CLR_CTRL         0x6168
#define SW_ACL_CLR_EXEC         (1u << 0)
#define SW_L2_TBL_FLUSH_CTRL    0x3370
#define SW_L2_TBL_FLUSH_EXEC    (1u << 26)

/* Write-one-to-clear status registers. */
#define SW_ISR_GLB_SRC          0x1148
#define SW_ISR_PORT_LINK_CHG    0x114c
#define SW_DMA_IF_INTR_STS      0x9f54

/* Port state. */
#define SW_MAC_LINK_STS         0xa188
#define SW_CPU_PORT             28
#define SW_PHY_PORT_FIRST       8
#define SW_PHY_PORT_LAST        15

/* MDIO command engine. */
#define SW_SMI_GLB_CTRL         0xa100
#define SW_SMI_POLL_CTRL        0xa17c
#define SW_SMI_ACCESS_PHY_CTRL0 0xa1b8  /* destination port mask (writes) */
#define SW_SMI_ACCESS_PHY_CTRL1 0xa1bc  /* command, bit 0 = run */
#define SW_SMI_ACCESS_PHY_CTRL2 0xa1c0  /* data in [31:16], out [15:0] */
#define SW_SMI_ACCESS_PHY_CTRL3 0xa1c4  /* clause 45 address */

#define SW_SMI_RUN              (1u << 0)
#define SW_SMI_CMD_MASK         (3u << 1)
#define SW_SMI_CMD_READ_C22     (0u << 1)
#define SW_SMI_CMD_READ_C45     (1u << 1)
#define SW_SMI_CMD_WRITE_C22    (2u << 1)
#define SW_SMI_CMD_WRITE_C45    (3u << 1)

#define SW_NUM_PHYS             8
#define SW_PHY_REGS             32

/* Standard clause 22 registers. */
#define MII_BMCR                0
#define MII_BMSR                1
#define MII_ADVERTISE           4
#define MII_LPA                 5
#define MII_STAT1000            10

#define BMCR_RESET              (1u << 15)
#define BMCR_ANRESTART          (1u << 9)

/*
 * Table access engine.  Three command/data window pairs, each multiplexing
 * several tables selected by a type field.  On RTL838x the read/write bit has
 * inverted meaning (set means read) and the execute bit sits one above it.
 */
typedef struct RTL838xTableDesc {
    uint16_t type;
    uint16_t width;     /* words per row */
    uint32_t rows;
} RTL838xTableDesc;

typedef struct RTL838xTableWindow {
    uint16_t cmd_reg;
    uint16_t data_reg;
    uint8_t c_bit;      /* read/write bit; execute is c_bit + 1 */
    uint8_t t_bit;      /* table type shift; index is everything below */
    RTL838xTableDesc tables[4];
} RTL838xTableWindow;

static const RTL838xTableWindow sw_table_windows[] = {
    {
        .cmd_reg = 0x6900, .data_reg = 0x6908, .c_bit = 15, .t_bit = 13,
        .tables = {
            { 0, 3, 8192 },     /* L2 unicast / multicast / next hop */
            { 1, 3, 64 },       /* L2 CAM */
            { 2, 1, 512 },      /* multicast port masks */
        },
    },
    {
        .cmd_reg = 0x6914, .data_reg = 0x6918, .c_bit = 14, .t_bit = 12,
        .tables = {
            { 0, 2, 4096 },     /* VLAN */
            { 1, 18, 1536 },    /* ingress ACL */
            { 2, 2, 64 },       /* MSTI: the per-port STP states RSTP drives */
            { 3, 2, 128 },      /* log */
        },
    },
    {
        .cmd_reg = 0xa4c8, .data_reg = 0xa4cc, .c_bit = 14, .t_bit = 12,
        .tables = {
            { 0, 1, 4096 },     /* untagged egress port masks */
            { 2, 2, 512 },      /* routing */
        },
    },
};

#define SW_NUM_TABLE_WINDOWS ARRAY_SIZE(sw_table_windows)
#define SW_TABLES_PER_WINDOW ARRAY_SIZE(sw_table_windows[0].tables)

struct RTL838xSwitchState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[SW_REGS];
    uint16_t phy[SW_NUM_PHYS][SW_PHY_REGS];

    uint32_t *table[SW_NUM_TABLE_WINDOWS][SW_TABLES_PER_WINDOW];
};

OBJECT_DECLARE_SIMPLE_TYPE(RTL838xSwitchState, RTL838X_SWITCH)

/* ------------------------------------------------------------------ PHYs */

/*
 * Register file of one internal RTL8218B port, reporting an established
 * 1000BASE-T full duplex link.  The PHY ID matters: 0x001cca40 is what
 * realtek_multiport.c binds to as "Realtek RTL8218B (internal)".
 */
static void rtl838x_phy_reset_one(RTL838xSwitchState *s, unsigned port)
{
    uint16_t *p = s->phy[port - SW_PHY_PORT_FIRST];

    memset(p, 0, SW_PHY_REGS * sizeof(*p));
    p[0]  = 0x1140;     /* BMCR: 1000 Mb/s, full duplex, autoneg enabled */
    p[1]  = 0x796d;     /* BMSR: link up, autoneg complete, extended status */
    p[2]  = 0x001c;     /* PHYID1 */
    p[3]  = 0xca40;     /* PHYID2, together 0x001cca40 = RTL8218B internal */
    p[4]  = 0x01e1;     /* ADVERTISE */
    p[5]  = 0x41e1;     /* LPA */
    p[9]  = 0x0200;     /* CTRL1000: advertise 1000FD */
    p[10] = 0x0800;     /* STAT1000: link partner is 1000FD capable */
    p[15] = 0x2000;     /* ESTATUS: 1000BASE-T full duplex */
}

static void rtl838x_phy_reset(RTL838xSwitchState *s)
{
    for (unsigned port = SW_PHY_PORT_FIRST; port <= SW_PHY_PORT_LAST; port++) {
        rtl838x_phy_reset_one(s, port);
    }
}

static uint16_t rtl838x_phy_read(RTL838xSwitchState *s, unsigned port,
                                 unsigned page, unsigned reg)
{
    if (port < SW_PHY_PORT_FIRST || port > SW_PHY_PORT_LAST) {
        return 0xffff;  /* nothing answers: reads as an idle MDIO bus */
    }
    if (page != 0) {
        return 0;       /* vendor pages exist but hold nothing interesting */
    }
    return s->phy[port - SW_PHY_PORT_FIRST][reg % SW_PHY_REGS];
}

/* Registers the link partner and our own status own; writes are ignored. */
static bool rtl838x_phy_reg_ro(unsigned reg)
{
    return reg == MII_BMSR || reg == 2 || reg == 3 || reg == MII_LPA ||
           reg == MII_STAT1000 || reg == 15;
}

static void rtl838x_phy_write(RTL838xSwitchState *s, uint32_t port_mask,
                              unsigned page, unsigned reg, uint16_t val)
{
    if (page != 0 || rtl838x_phy_reg_ro(reg)) {
        return;
    }

    for (unsigned port = SW_PHY_PORT_FIRST; port <= SW_PHY_PORT_LAST; port++) {
        if (!(port_mask & (1u << port))) {
            continue;
        }

        if (reg == MII_BMCR) {
            /*
             * Reset and restart-autonegotiation are self-clearing.  Storing
             * them verbatim leaves the driver waiting for an negotiation that
             * never finishes, and the port never comes up.
             */
            if (val & BMCR_RESET) {
                rtl838x_phy_reset_one(s, port);
                continue;
            }
            val &= ~(BMCR_RESET | BMCR_ANRESTART);
        }
        s->phy[port - SW_PHY_PORT_FIRST][reg % SW_PHY_REGS] = val;
    }
}

/*
 * A command carries the page in [14:3] and the register in [24:20]; the
 * asymmetry to watch is where the data goes.  A read takes the port number
 * from CTRL2[31:16] and returns the value in CTRL2[15:0]; a write takes the
 * value from CTRL2[31:16] and the destination port mask from CTRL0.
 */
static void rtl838x_mdio_exec(RTL838xSwitchState *s, uint32_t cmd)
{
    unsigned reg = (cmd >> 20) & 0x1f;
    unsigned page = (cmd >> 3) & 0xfff;
    uint32_t data = s->regs[SW_SMI_ACCESS_PHY_CTRL2 / 4];

    switch (cmd & SW_SMI_CMD_MASK) {
    case SW_SMI_CMD_READ_C22: {
        unsigned port = (data >> 16) & 0x1f;
        uint16_t val = rtl838x_phy_read(s, port, page, reg);

        mdio_dbg("read  port %u page 0x%x reg %u -> 0x%04x\n",
                 port, page, reg, val);
        s->regs[SW_SMI_ACCESS_PHY_CTRL2 / 4] = (data & 0xffff0000) | val;
        break;
    }
    case SW_SMI_CMD_WRITE_C22:
        mdio_dbg("write mask 0x%x page 0x%x reg %u = 0x%04x\n",
                 s->regs[SW_SMI_ACCESS_PHY_CTRL0 / 4], page, reg,
                 (unsigned)(data >> 16));
        rtl838x_phy_write(s, s->regs[SW_SMI_ACCESS_PHY_CTRL0 / 4], page, reg,
                          data >> 16);
        break;

    default:
        /* Clause 45 is only used for SFP cages, which this board has none of. */
        s->regs[SW_SMI_ACCESS_PHY_CTRL2 / 4] = data & 0xffff0000;
        break;
    }
}

/* ---------------------------------------------------------- table engine */

static void rtl838x_table_exec(RTL838xSwitchState *s, unsigned w, uint32_t cmd)
{
    const RTL838xTableWindow *win = &sw_table_windows[w];
    bool is_read = cmd & (1u << win->c_bit);   /* inverted on RTL838x */
    /* The type field sits between the index and the read/write bit. */
    unsigned type_bits = win->c_bit - win->t_bit;
    unsigned type = (cmd >> win->t_bit) & ((1u << type_bits) - 1);
    uint32_t idx = cmd & ((1u << win->t_bit) - 1);
    const RTL838xTableDesc *desc = NULL;
    unsigned t;

    for (t = 0; t < SW_TABLES_PER_WINDOW; t++) {
        if (win->tables[t].width && win->tables[t].type == type) {
            desc = &win->tables[t];
            break;
        }
    }
    if (!desc || idx >= desc->rows) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rtl838x-switch: table access to type %u index %u of "
                      "window 0x%04x is out of range\n",
                      type, idx, win->cmd_reg);
        return;
    }

    uint32_t *row = &s->table[w][t][(size_t)idx * desc->width];
    uint32_t *data = &s->regs[win->data_reg / 4];

    if (is_read) {
        memcpy(data, row, desc->width * sizeof(uint32_t));
    } else {
        memcpy(row, data, desc->width * sizeof(uint32_t));
    }
}

/* Returns the index of the table window whose command register this is. */
static int rtl838x_table_window(hwaddr addr)
{
    for (unsigned w = 0; w < SW_NUM_TABLE_WINDOWS; w++) {
        if (sw_table_windows[w].cmd_reg == addr) {
            return w;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- access */

static uint64_t rtl838x_switch_read(void *opaque, hwaddr addr, unsigned size)
{
    RTL838xSwitchState *s = opaque;

    return s->regs[addr / 4];
}

static void rtl838x_switch_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    RTL838xSwitchState *s = opaque;
    int window;

    switch (addr) {
    case SW_CHIP_INFO:
        /* Reads only return anything after the magic unlock value. */
        s->regs[addr / 4] = (val == SW_CHIP_INFO_UNLOCK) ? SW_CHIP_INFO_VALUE
                                                         : val;
        return;

    case SW_RST_GLB_CTRL_0:
        /* Resets complete immediately; the driver polls for them to clear. */
        s->regs[addr / 4] = val & ~SW_RST_GLB_SELF_CLEAR;
        return;

    case SW_ACL_CLR_CTRL:
        s->regs[addr / 4] = val & ~SW_ACL_CLR_EXEC;
        return;

    case SW_L2_TBL_FLUSH_CTRL:
        s->regs[addr / 4] = val & ~SW_L2_TBL_FLUSH_EXEC;
        return;

    case SW_ISR_GLB_SRC:
    case SW_ISR_PORT_LINK_CHG:
    case SW_DMA_IF_INTR_STS:
        s->regs[addr / 4] &= ~(uint32_t)val;
        return;

    case SW_MAC_LINK_STS:
        return;     /* driven by the PHYs, not by the guest */

    case SW_SMI_ACCESS_PHY_CTRL1:
        s->regs[addr / 4] = val & ~SW_SMI_RUN;
        if (val & SW_SMI_RUN) {
            rtl838x_mdio_exec(s, val);
        }
        return;

    case SW_THERMAL_RESULT:
        return;     /* sensor output */

    default:
        break;
    }

    window = rtl838x_table_window(addr);
    if (window >= 0) {
        const RTL838xTableWindow *win = &sw_table_windows[window];
        uint32_t exec = 1u << (win->c_bit + 1);

        s->regs[addr / 4] = val & ~exec;
        if (val & exec) {
            rtl838x_table_exec(s, window, val);
        }
        return;
    }

    s->regs[addr / 4] = val;
}

static const MemoryRegionOps rtl838x_switch_ops = {
    .read = rtl838x_switch_read,
    .write = rtl838x_switch_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void rtl838x_switch_reset(DeviceState *dev)
{
    RTL838xSwitchState *s = RTL838X_SWITCH(dev);
    uint32_t link = 1u << SW_CPU_PORT;

    memset(s->regs, 0, sizeof(s->regs));

    s->regs[SW_MODEL_NAME_INFO / 4] = SW_MODEL_RTL8380M;
    s->regs[SW_CHIP_INFO / 4] = SW_CHIP_INFO_VALUE;
    s->regs[SW_EXT_VERSION / 4] = 0;

    s->regs[SW_PLL_CPU_CTRL0 / 4] = 0x00004748;
    s->regs[SW_PLL_CPU_CTRL1 / 4] = 0x0c14530e;
    s->regs[SW_PLL_LXB_CTRL0 / 4] = 0x000047c8;
    s->regs[SW_PLL_LXB_CTRL1 / 4] = 0x001ad30e;
    s->regs[SW_PLL_MEM_CTRL0 / 4] = 0x000041bc;
    s->regs[SW_PLL_MEM_CTRL1 / 4] = 0x14018c80;

    s->regs[SW_THERMAL_RESULT / 4] = SW_THERMAL_VALID | SW_THERMAL_DEGREES;

    for (unsigned p = SW_PHY_PORT_FIRST; p <= SW_PHY_PORT_LAST; p++) {
        link |= 1u << p;
    }
    s->regs[SW_MAC_LINK_STS / 4] = link;

    for (unsigned w = 0; w < SW_NUM_TABLE_WINDOWS; w++) {
        for (unsigned t = 0; t < SW_TABLES_PER_WINDOW; t++) {
            const RTL838xTableDesc *d = &sw_table_windows[w].tables[t];

            if (d->width) {
                memset(s->table[w][t], 0,
                       (size_t)d->rows * d->width * sizeof(uint32_t));
            }
        }
    }

    rtl838x_phy_reset(s);
    qemu_set_irq(s->irq, 0);
}

static void rtl838x_switch_realize(DeviceState *dev, Error **errp)
{
    RTL838xSwitchState *s = RTL838X_SWITCH(dev);

    for (unsigned w = 0; w < SW_NUM_TABLE_WINDOWS; w++) {
        for (unsigned t = 0; t < SW_TABLES_PER_WINDOW; t++) {
            const RTL838xTableDesc *d = &sw_table_windows[w].tables[t];

            if (d->width) {
                s->table[w][t] = g_new0(uint32_t, (size_t)d->rows * d->width);
            }
        }
    }
}

static void rtl838x_switch_init(Object *obj)
{
    RTL838xSwitchState *s = RTL838X_SWITCH(obj);

    memory_region_init_io(&s->iomem, obj, &rtl838x_switch_ops, s,
                          TYPE_RTL838X_SWITCH, RTL838X_SW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_rtl838x_switch = {
    .name = TYPE_RTL838X_SWITCH,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RTL838xSwitchState, SW_REGS),
        VMSTATE_UINT16_2DARRAY(phy, RTL838xSwitchState, SW_NUM_PHYS,
                               SW_PHY_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void rtl838x_switch_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rtl838x_switch_realize;
    dc->vmsd = &vmstate_rtl838x_switch;
    device_class_set_legacy_reset(dc, rtl838x_switch_reset);
}

static const TypeInfo rtl838x_switch_types[] = {
    {
        .name          = TYPE_RTL838X_SWITCH,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RTL838xSwitchState),
        .instance_init = rtl838x_switch_init,
        .class_init    = rtl838x_switch_class_init,
    },
};

DEFINE_TYPES(rtl838x_switch_types)
