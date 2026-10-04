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
 * so every one of those has to complete instantly here.  Teltonika's kernel
 * adds a fourth, 0xe3e4 bit 15.
 *
 * What is modelled here: identification, clocks, thermal, the self-clearing
 * and write-one-to-clear bits, the table access engine with real backing
 * store, the MDIO command engine, eight internal PHYs and the link state they
 * report.  The data path that runs over the same window lives next door:
 * rtl838x_eth.c owns the CPU-port DMA engine and rtl838x_fwd.c owns what the
 * silicon does between the ports.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/mips/rtl838x.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

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
 * Pin straps, which the board decides; see the properties at the end.
 * INT_MODE_CTRL says what is behind the two SerDes, ports 24 and 26, three
 * bits each from bit 0; 1 is a fibre port, which is what the Realtek SDK
 * looks for before it counts the port as there.  STRAP_DBG bit 29 has the
 * SPI flash addressed with four bytes, as a chip larger than 16 MiB must be.
 */
#define SW_INT_MODE_CTRL        0x005c
#define SW_STRAP_DBG            0x100c
#define SW_STRAP_DBG_FLASH_4B   (1u << 29)

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
 * HPE's Comware reads a revision from bits 5:1 of this register and takes
 * zero for a chip whose timers are laid out the older Realtek way, with the
 * second timer's count at 0x310c.  It then uses Otto timer 0's interrupt
 * register as its cycle counter, which never moves, and divides by the
 * difference.  Nothing else reads it.
 */
#define SW_SOC_REVISION         0x0ff0
#define SW_SOC_REVISION_VALUE   (1u << 1)

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
#define SW_RST_GLB_NIC          0x0000000c  /* of those, the two the NIC uses */
#define SW_ACL_CLR_CTRL         0x6168
#define SW_ACL_CLR_EXEC         (1u << 0)
/*
 * Moving a block of ACL rules, which HPE's Comware does when an address is
 * configured.  ACLs are not modelled, so the rules stay where they are.
 */
#define SW_ACL_MV_CTRL          0x6160
#define SW_ACL_MV_EXEC          (1u << 0)
#define SW_L2_TBL_FLUSH_CTRL    0x3370
#define SW_L2_TBL_FLUSH_EXEC    (1u << 26)
#define SW_L2_TBL_FLUSH_BY_PORT (1u << 23)
/*
 * An indirect access engine OpenWrt's upstream driver never touches, but
 * Teltonika's 5.10 kernel does during DSA setup: address and data go into
 * 0xe3e0..0xe3ec, the command into bits [13:12] of the second word, results
 * come back in 0xe408..0xe414.  It spins on bit 15 with no timeout.  Nothing
 * behind it is modelled; reads return zero.
 */
#define SW_IND_ACCESS_CTRL      0xe3e4
#define SW_IND_ACCESS_EXEC      (1u << 15)

/* Write-one-to-clear status registers. */
#define SW_ISR_GLB_SRC          0x1148
#define SW_ISR_PORT_LINK_CHG    0x114c

/*
 * Port and link state.  MAC_LINK_STS is not stored: it is derived from the
 * netdev backends on every read, so "no backend" reads as an unplugged port.
 * The driver's interrupt handler W1Cs ISR_PORT_LINK_CHG, then reads
 * MAC_LINK_STS twice because the real register is latched.
 */
#define SW_MAC_LINK_STS         0xa188
#define SW_MAC_LINK_SPD_STS     0xa190  /* two bits per port, 2 = 1000 Mb/s */
#define SW_MAC_LINK_DUP_STS     0xa19c  /* one bit per port, set = full */
#define SW_IMR_GLB              0x1100
#define SW_IMR_PORT_LINK_CHG    0x1104

/* MDIO command engine. */
#define SW_SMI_GLB_CTRL         0xa100
#define SW_SMI_POLL_CTRL        0xa17c
#define SW_SMI_ACCESS_PHY_CTRL0 0xa1b8  /* destination port mask (writes) */
#define SW_SMI_ACCESS_PHY_CTRL1 0xa1bc  /* command, bit 0 = run */
#define SW_SMI_ACCESS_PHY_CTRL2 0xa1c0  /* data in [31:16], out [15:0] */
#define SW_SMI_ACCESS_PHY_CTRL3 0xa1c4  /* clause 45 address */
#define SW_SMI_PORT0_5_ADDR     0xa1c8  /* PHY address per port, 5 bits each */
#define SW_SMI_PORTS_PER_REG    6
#define SW_SMI_PORTS            28

/* Reset values the forwarding engine reads; see rtl838x_switch_reset(). */
#define SW_PORT_ISO_CTRL(p)     (0x4100 + (p) * 4)
#define SW_ALL_PORTS            0x1fffffff
#define SW_MC_PMSK_LAST_ROW     511

#define SW_SMI_RUN              (1u << 0)
#define SW_SMI_CMD_MASK         (3u << 1)
#define SW_SMI_CMD_READ_C22     (0u << 1)
#define SW_SMI_CMD_READ_C45     (1u << 1)
#define SW_SMI_CMD_WRITE_C22    (2u << 1)
#define SW_SMI_CMD_WRITE_C45    (3u << 1)

/* Standard clause 22 registers. */
#define MII_BMCR                0
#define MII_BMSR                1
#define MII_ADVERTISE           4
#define MII_LPA                 5
#define MII_STAT1000            10

#define BMCR_RESET              (1u << 15)
#define BMCR_PDOWN              (1u << 11)
#define BMCR_ANRESTART          (1u << 9)

#define BMSR_LSTATUS            (1u << 2)
#define BMSR_ANEGCOMPLETE       (1u << 5)

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

QEMU_BUILD_BUG_ON(ARRAY_SIZE(sw_table_windows) != RTL838X_SW_TABLE_WINDOWS);
QEMU_BUILD_BUG_ON(ARRAY_SIZE(sw_table_windows[0].tables) !=
                  RTL838X_SW_TABLES_PER_WINDOW);

/* ------------------------------------------------------------------ PHYs */

/*
 * Register file of one internal RTL8218B port, reporting an established
 * 1000BASE-T full duplex link.  The PHY ID matters: 0x001cca40 is what
 * realtek_multiport.c binds to as "Realtek RTL8218B (internal)".
 */
static void rtl838x_phy_reset_one(RTL838xSwitchState *s, unsigned port)
{
    uint16_t *p = s->phy[port - RTL838X_SW_PORT_FIRST];

    memset(p, 0, RTL838X_SW_PHY_REGS * sizeof(*p));
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
    for (unsigned port = RTL838X_SW_PORT_FIRST;
         port <= RTL838X_SW_PORT_LAST; port++) {
        rtl838x_phy_reset_one(s, port);
    }
}

static uint16_t rtl838x_phy_read(RTL838xSwitchState *s, unsigned port,
                                 unsigned page, unsigned reg)
{
    uint16_t val;

    if (port < RTL838X_SW_PORT_FIRST || port > RTL838X_SW_PORT_LAST) {
        return 0xffff;  /* nothing answers: reads as an idle MDIO bus */
    }
    if (page != 0) {
        return 0;       /* vendor pages exist but hold nothing interesting */
    }

    val = s->phy[port - RTL838X_SW_PORT_FIRST][reg % RTL838X_SW_PHY_REGS];

    /*
     * The link is the netdev backend's, not ours: a port with nothing plugged
     * into it reports no carrier and no completed negotiation, which is how
     * phylink ends up marking lanN down.  Everything else in the register file
     * keeps describing a port that would negotiate 1000/full if it were.
     */
    if (reg == MII_BMSR && !rtl838x_switch_link_up(s, port)) {
        val &= ~(BMSR_LSTATUS | BMSR_ANEGCOMPLETE);
    }

    return val;
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

    for (unsigned port = RTL838X_SW_PORT_FIRST;
         port <= RTL838X_SW_PORT_LAST; port++) {
        if (!(port_mask & (1u << port))) {
            continue;
        }

        if (reg == MII_BMCR) {
            bool was_up = rtl838x_switch_link_up(s, port);
            bool renegotiate = val & (BMCR_RESET | BMCR_ANRESTART);

            /*
             * Reset and restart-autonegotiation are self-clearing.  Storing
             * them verbatim leaves the driver waiting for an negotiation that
             * never finishes, and the port never comes up.
             */
            if (val & BMCR_RESET) {
                rtl838x_phy_reset_one(s, port);
            } else {
                s->phy[port - RTL838X_SW_PORT_FIRST][MII_BMCR] =
                    val & ~BMCR_ANRESTART;
            }

            /*
             * Powering the PHY down takes the link with it, and a reset or a
             * new negotiation drops it and brings it back.  Either way the
             * MAC sees its link change.  Linux follows the PHY and does not
             * care; the vendor firmware resets every PHY as the last thing
             * its bring-up does and waits for exactly this interrupt.
             */
            if (was_up != rtl838x_switch_link_up(s, port) ||
                (was_up && renegotiate)) {
                rtl838x_switch_link_changed(s, port);
            }
            continue;
        }
        s->phy[port - RTL838X_SW_PORT_FIRST][reg % RTL838X_SW_PHY_REGS] = val;
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

/* ----------------------------------------------------------- link state */

/*
 * The board creates one rtl838x-port device per front-panel port, each holding
 * a QEMU NIC, and each registers itself here as it is realized.
 */
void rtl838x_switch_attach_port(RTL838xSwitchState *s, unsigned port,
                                RTL838xPortState *p)
{
    assert(port >= RTL838X_SW_PORT_FIRST && port <= RTL838X_SW_PORT_LAST);
    s->port[port - RTL838X_SW_PORT_FIRST] = p;
}

bool rtl838x_switch_link_up(RTL838xSwitchState *s, unsigned port)
{
    RTL838xPortState *p;

    if (port == RTL838X_SW_CPU_PORT) {
        return true;    /* the CPU port is wired to the SoC, not to a cable */
    }
    if (port < RTL838X_SW_PORT_FIRST || port > RTL838X_SW_PORT_LAST) {
        return false;
    }

    /* A powered-down PHY has no link, whatever is plugged into the port. */
    if (s->phy[port - RTL838X_SW_PORT_FIRST][MII_BMCR] & BMCR_PDOWN) {
        return false;
    }

    p = s->port[port - RTL838X_SW_PORT_FIRST];
    return p && rtl838x_port_link_up(p);
}

static uint32_t rtl838x_switch_link_mask(RTL838xSwitchState *s)
{
    uint32_t mask = 1u << RTL838X_SW_CPU_PORT;

    for (unsigned p = RTL838X_SW_PORT_FIRST; p <= RTL838X_SW_PORT_LAST; p++) {
        if (rtl838x_switch_link_up(s, p)) {
            mask |= 1u << p;
        }
    }
    return mask;
}

/* The core interrupt is level driven and gated by both the per-port and the
 * global mask, which the DSA driver enables once at probe. */
static void rtl838x_switch_update_irq(RTL838xSwitchState *s)
{
    bool pending = s->regs[SW_ISR_PORT_LINK_CHG / 4] &
                   s->regs[SW_IMR_PORT_LINK_CHG / 4];

    qemu_set_irq(s->irq, pending && (s->regs[SW_IMR_GLB / 4] & 1));
}

void rtl838x_switch_link_changed(RTL838xSwitchState *s, unsigned port)
{
    if (!rtl838x_switch_link_up(s, port)) {
        /*
         * Real silicon drops what it learned behind a port that went away;
         * without this a stale entry black-holes traffic until the station
         * speaks again from wherever it moved to.
         */
        rtl838x_fwd_flush(s, port);
    }

    s->regs[SW_ISR_PORT_LINK_CHG / 4] |= 1u << port;
    rtl838x_switch_update_irq(s);
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

    for (t = 0; t < RTL838X_SW_TABLES_PER_WINDOW; t++) {
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

/*
 * Row `index` of table `type` in window `window`, or NULL if that table does
 * not exist or the row is out of range.  The forwarding engine reads the VLAN,
 * untagged-egress and per-port STP tables back out of here.
 */
uint32_t *rtl838x_switch_table_row(RTL838xSwitchState *s, unsigned window,
                                   unsigned type, uint32_t index)
{
    const RTL838xTableWindow *win;

    assert(window < RTL838X_SW_TABLE_WINDOWS);
    win = &sw_table_windows[window];

    for (unsigned t = 0; t < RTL838X_SW_TABLES_PER_WINDOW; t++) {
        const RTL838xTableDesc *desc = &win->tables[t];

        if (desc->width && desc->type == type) {
            if (index >= desc->rows) {
                return NULL;
            }
            return &s->table[window][t][(size_t)index * desc->width];
        }
    }
    return NULL;
}

/* Returns the index of the table window whose command register this is. */
static int rtl838x_table_window(hwaddr addr)
{
    for (unsigned w = 0; w < RTL838X_SW_TABLE_WINDOWS; w++) {
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
    uint32_t val;

    switch (addr) {
    case SW_MAC_LINK_STS:
        return rtl838x_switch_link_mask(s);
    case SW_MAC_LINK_DUP_STS:
        return rtl838x_switch_link_mask(s);
    case SW_MAC_LINK_SPD_STS: {
        /*
         * What the MACs resolved the links to, which is what the PHYs say
         * they negotiated: 1000 Mb/s, full duplex.  Only ports 0..15 fit in
         * this register, and the front panel is within them.  The Linux
         * driver learns this from the PHYs; the vendor SDK asks here.
         */
        uint32_t up = rtl838x_switch_link_mask(s), spd = 0;

        for (unsigned p = 0; p < 16; p++) {
            if (up & (1u << p)) {
                spd |= 2u << (2 * p);
            }
        }
        return spd;
    }
    default:
        break;
    }

    if (rtl838x_eth_read(s, addr, &val)) {
        return val;
    }

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
        if (val & SW_RST_GLB_NIC) {
            rtl838x_eth_reset(s);
        }
        return;

    case SW_ACL_CLR_CTRL:
        s->regs[addr / 4] = val & ~SW_ACL_CLR_EXEC;
        return;

    case SW_ACL_MV_CTRL:
        s->regs[addr / 4] = val & ~SW_ACL_MV_EXEC;
        return;

    case SW_IND_ACCESS_CTRL:
        s->regs[addr / 4] = val & ~SW_IND_ACCESS_EXEC;
        return;

    case SW_L2_TBL_FLUSH_CTRL:
        s->regs[addr / 4] = val & ~SW_L2_TBL_FLUSH_EXEC;
        /*
         * Bit 23 selects "by port", with the port in bits [9:5]; otherwise the
         * whole table goes.  The driver walks every port in turn on shutdown.
         */
        if (val & SW_L2_TBL_FLUSH_EXEC) {
            rtl838x_fwd_flush(s, (val & SW_L2_TBL_FLUSH_BY_PORT)
                                 ? (int)((val >> 5) & 0x1f) : -1);
        }
        return;

    case SW_ISR_GLB_SRC:
        s->regs[addr / 4] &= ~(uint32_t)val;
        return;

    case SW_ISR_PORT_LINK_CHG:
        s->regs[addr / 4] &= ~(uint32_t)val;
        rtl838x_switch_update_irq(s);
        return;

    case SW_IMR_GLB:
    case SW_IMR_PORT_LINK_CHG:
        s->regs[addr / 4] = val;
        rtl838x_switch_update_irq(s);
        return;

    case SW_MAC_LINK_STS:
    case SW_MAC_LINK_SPD_STS:
    case SW_MAC_LINK_DUP_STS:
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

    if (rtl838x_eth_write(s, addr, val)) {
        return;
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

/* MAC_ADDR_CTRL, MAC_ADDR_CTRL_ALE, MAC_ADDR_CTRL_MAC: high 16 bits, low 32 */
static const hwaddr sw_mac_regs[] = { 0xa9ec, 0x6b04, 0xa320 };

/* Acceptable frame types, one register a port; see rtl838x_fwd.c. */
#define SW_VLAN_PORT_AFT        0x3a00
#define SW_VLAN_PORT_AFT_ALL    0xf

static void rtl838x_switch_reset(DeviceState *dev)
{
    RTL838xSwitchState *s = RTL838X_SWITCH(dev);

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
    s->regs[SW_SOC_REVISION / 4] = SW_SOC_REVISION_VALUE;

    s->regs[SW_INT_MODE_CTRL / 4] = s->int_mode_ctrl;
    s->regs[SW_STRAP_DBG / 4] = s->flash_4byte ? SW_STRAP_DBG_FLASH_4B : 0;

    s->regs[SW_THERMAL_RESULT / 4] = SW_THERMAL_VALID | SW_THERMAL_DEGREES;

    /*
     * Every port admits tagged and untagged frames on either tag until told
     * otherwise.  Linux never writes these, and relies on that.
     */
    for (unsigned p = 0; p <= RTL838X_SW_CPU_PORT; p++) {
        s->regs[(SW_VLAN_PORT_AFT + p * 4) / 4] = SW_VLAN_PORT_AFT_ALL;
    }

    /*
     * The stock bootloader programs the switch's MAC address into the three
     * places the driver keeps it, and the driver takes it from the first
     * when the device tree has none -- which the GS1900's does not.  Without
     * it the driver picks a random one on every boot, and whatever sits on
     * the other side of a port, QEMU's user network included, keeps sending
     * to the address from before the reboot.
     */
    for (unsigned i = 0; i < ARRAY_SIZE(sw_mac_regs); i++) {
        const uint8_t *a = s->macaddr.a;

        s->regs[sw_mac_regs[i] / 4] = (a[0] << 8) | a[1];
        s->regs[sw_mac_regs[i] / 4 + 1] =
            ((uint32_t)a[2] << 24) | (a[3] << 16) | (a[4] << 8) | a[5];
    }

    /*
     * Which address on the MDIO bus each port's PHY answers at: its own port
     * number, on this board.  The Linux driver passes the port and never
     * looks, but the vendor SDK reads the address back out of here for every
     * access, and with the registers at zero it would ask for the PHY at
     * address 0 eight times over and find no PHYs at all.
     */
    for (unsigned p = 0; p < SW_SMI_PORTS; p++) {
        s->regs[SW_SMI_PORT0_5_ADDR / 4 + p / SW_SMI_PORTS_PER_REG] |=
            p << (5 * (p % SW_SMI_PORTS_PER_REG));
    }

    for (unsigned w = 0; w < RTL838X_SW_TABLE_WINDOWS; w++) {
        for (unsigned t = 0; t < RTL838X_SW_TABLES_PER_WINDOW; t++) {
            const RTL838xTableDesc *d = &sw_table_windows[w].tables[t];

            if (d->width) {
                memset(s->table[w][t], 0,
                       (size_t)d->rows * d->width * sizeof(uint32_t));
            }
        }
    }

    /*
     * Out of reset nothing is isolated from anything, and every row of the
     * multicast port mask table floods to every port -- including the last
     * one, which L2_FLD_PMSK points at until told otherwise.  Linux
     * overwrites both before it lets a frame through.  The vendor SDK leaves
     * the isolation matrix alone, and builds its flood mask by copying the
     * last row.  Realtek's SDK in Teltonika's firmware relies on the other
     * rows: it points broadcast flooding at row 504, by shifting the row
     * number to where the RTL839x keeps it, which OpenWrt did too until
     * 1b7fd8464c2b ("realtek: rtl838x: fix broadcast flooding with many
     * multicast entries").  It works on the hardware because row 504 is
     * never written.
     */
    for (unsigned p = 0; p <= RTL838X_SW_CPU_PORT; p++) {
        s->regs[SW_PORT_ISO_CTRL(p) / 4] = SW_ALL_PORTS;
    }
    for (unsigned r = 0; r <= SW_MC_PMSK_LAST_ROW; r++) {
        *rtl838x_switch_table_row(s, RTL838X_TBL_MC_PMSK_WINDOW,
                                  RTL838X_TBL_MC_PMSK_TYPE, r) = SW_ALL_PORTS;
    }

    rtl838x_phy_reset(s);
    rtl838x_eth_reset(s);
    rtl838x_fwd_reset(s);

    /*
     * Out of reset every link is down, and the ports with a cable in them
     * come up a moment later: a change, which is latched here as it is on
     * the real switch.  The vendor firmware depends on it -- it only looks
     * at a port's link after being told that it changed.
     */
    s->regs[SW_ISR_PORT_LINK_CHG / 4] =
        rtl838x_switch_link_mask(s) & ~(1u << RTL838X_SW_CPU_PORT);
    qemu_set_irq(s->irq, 0);
}

static void rtl838x_switch_realize(DeviceState *dev, Error **errp)
{
    RTL838xSwitchState *s = RTL838X_SWITCH(dev);

    for (unsigned w = 0; w < RTL838X_SW_TABLE_WINDOWS; w++) {
        for (unsigned t = 0; t < RTL838X_SW_TABLES_PER_WINDOW; t++) {
            const RTL838xTableDesc *d = &sw_table_windows[w].tables[t];

            if (d->width) {
                s->table[w][t] = g_new0(uint32_t, (size_t)d->rows * d->width);
            }
        }
    }

    rtl838x_fwd_realize(s);
}

static void rtl838x_switch_init(Object *obj)
{
    RTL838xSwitchState *s = RTL838X_SWITCH(obj);

    memory_region_init_io(&s->iomem, obj, &rtl838x_switch_ops, s,
                          TYPE_RTL838X_SWITCH, RTL838X_SW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);

    /*
     * Two interrupts, and they are not the same line: the switch core raises
     * INTC 20 for link changes, while the CPU-port DMA engine raises INTC 24,
     * which is what the ethernet node in the device tree asks for.
     */
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->eth_irq);
}

static const VMStateDescription vmstate_rtl838x_switch = {
    .name = TYPE_RTL838X_SWITCH,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RTL838xSwitchState, RTL838X_SW_REGS),
        VMSTATE_UINT16_2DARRAY(phy, RTL838xSwitchState, RTL838X_SW_NUM_PORTS,
                               RTL838X_SW_PHY_REGS),
        VMSTATE_UINT32_ARRAY(rx_cursor, RTL838xSwitchState,
                             RTL838X_ETH_RX_RINGS),
        VMSTATE_UINT32_ARRAY(tx_cursor, RTL838xSwitchState,
                             RTL838X_ETH_TX_RINGS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property rtl838x_switch_properties[] = {
    DEFINE_PROP_MACADDR("macaddr", RTL838xSwitchState, macaddr),
    DEFINE_PROP_UINT32("int-mode-ctrl", RTL838xSwitchState, int_mode_ctrl, 0),
    DEFINE_PROP_BOOL("flash-4byte", RTL838xSwitchState, flash_4byte, false),
};

static void rtl838x_switch_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, rtl838x_switch_properties);
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
