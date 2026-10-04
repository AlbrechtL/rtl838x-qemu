/*
 * Realtek RTL838x (RTL8380M) switch SoC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MIPS_RTL838X_H
#define HW_MIPS_RTL838X_H

#include "hw/core/sysbus.h"
#include "net/net.h"
#include "qom/object.h"

/*
 * Physical memory map.  The addresses are fixed by the device tree that the
 * OpenWrt kernel carries appended to its image, so none of this is negotiable:
 * "soc" has ranges = <0x0 0x18000000 0x10000>, and the switch core sits at
 * 0x1b000000.  Note that both the clock driver and the platform's early setup
 * code reach these windows through hardcoded KSEG1 pointers (0xb8000000 /
 * 0xbb000000) rather than ioremap(), so they must exist before the kernel has
 * parsed anything.
 */
#define RTL838X_RAM_BASE        0x00000000
#define RTL838X_SRAM_BASE       0x9f000000
#define RTL838X_SRAM_SIZE       0x00010000
#define RTL838X_SRAM_KSEG0      0x1f000000  /* where the clk SRAM path lands */

#define RTL838X_SOC_BASE        0x18000000
#define RTL838X_SOC_SIZE        0x00010000
#define RTL838X_MC_BASE         0x18001000  /* memory controller */
#define RTL838X_SPI_BASE        0x18001200
#define RTL838X_UART0_BASE      0x18002000
#define RTL838X_UART1_BASE      0x18002100
#define RTL838X_INTC_BASE       0x18003000
#define RTL838X_TIMER_BASE      0x18003100
#define RTL838X_WDT_BASE        0x18003150
#define RTL838X_GPIO_BASE       0x18003500

#define RTL838X_SW_BASE         0x1b000000
#define RTL838X_SW_SIZE         0x00010000

/* Kernel entry point / uImage load address for this target. */
#define RTL838X_KERNEL_LOAD     0x80100000

/*
 * The SPI-NOR flash: its default model, which the machine's flash-model
 * property overrides, and where in it the stock bootloaders look for the
 * uImage they boot.  0x260000 is the GS1900's first image slot, the one
 * "bootpartition=0" selects and the device tree's "firmware" partition
 * starts at; 0xa0000 is where the Teltonika TSW2xx's "firmware" starts, and
 * 0x300000 the Netgear GS108Tv3's "RUNTIME".
 */
#define RTL838X_FLASH_TYPE      "mx25l12855e"
#define RTL838X_FLASH_TYPE_32M  "mx25l25635e"   /* the HPE 1920's, GS108Tv3's */
#define RTL838X_FLASH_FIRMWARE  0x260000
#define RTL838X_FLASH_FIRMWARE_TSW 0x0a0000
#define RTL838X_FLASH_FIRMWARE_NETGEAR 0x300000
/* The SPI controller's window onto the flash, 0xb4000000 through KSEG1. */
#define RTL838X_FLASH_WINDOW    0x14000000

/* SoC interrupt numbers, as used by the device tree. */
#define RTL838X_IRQ_TIMER4      15
#define RTL838X_IRQ_TIMER3      16
#define RTL838X_IRQ_TIMER2      17
#define RTL838X_IRQ_WDT_PHASE2  18
#define RTL838X_IRQ_WDT_PHASE1  19
#define RTL838X_IRQ_SWITCH      20
#define RTL838X_IRQ_GPIO        23
#define RTL838X_IRQ_ETH         24
#define RTL838X_IRQ_TIMER1      28
#define RTL838X_IRQ_TIMER0      29
#define RTL838X_IRQ_UART1       30
#define RTL838X_IRQ_UART0       31

/*
 * Clocks.  The PLL registers in the switch window are primed with the values
 * the vendor firmware leaves behind (see rtl838x_switch.c), which the OpenWrt
 * clk driver decodes back into exactly these rates.
 */
#define RTL838X_CPU_HZ          500000000
#define RTL838X_LXB_HZ          200000000

/* Interrupt controller: 32 SoC sources, 5 outputs to MIPS IP2..IP6. */
#define TYPE_RTL838X_INTC "rtl838x-intc"
OBJECT_DECLARE_SIMPLE_TYPE(RTL838xIntcState, RTL838X_INTC)

#define RTL838X_INTC_NUM_IRQ    32
#define RTL838X_INTC_NUM_OUT    5

struct RTL838xIntcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq out[RTL838X_INTC_NUM_OUT];

    uint32_t gimr;      /* global interrupt mask */
    uint32_t gisr;      /* global interrupt status (level of the inputs) */
    uint32_t irr[4];    /* routing, 4 bits per source, inverted numbering */
};

/* Otto timer: five independent count-up timers. */
#define TYPE_RTL838X_TIMER "rtl838x-timer"
#define RTL838X_TIMER_BANKS 5

/* Memory controller + SPI-NOR controller stub. */
#define TYPE_RTL838X_SOCMISC "rtl838x-socmisc"

/* GPIO controller. */
#define TYPE_RTL838X_GPIO "rtl838x-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(RTL838xGpioState, RTL838X_GPIO)
/* Every line: the TSW2xx's I2C buses, SFP cage signals and reset button. */
#define RTL838X_GPIO_TSW_PULLUPS 0xffffffffu
/* The GS108Tv3's reset button, line 0 (A0), active low. */
#define RTL838X_GPIO_GS108TV3_PULLUPS (1u << 24)
/*
 * What the GS108Tv3's RTL8231 pins read as inputs: high, as pulled up,
 * but for the board ID Netgear's firmware reads its model from.  Pins
 * 0..3 are bits 1, 0, 2 and 3 of it: 1 is the GS108Tv3, 3 the GS110TPv3,
 * 15 the GS110TPP.
 */
#define RTL838X_RTL8231_GS108TV3 (0x1fffffffffull & ~0xdull)

/* Watchdog. */
#define TYPE_RTL838X_WDT "rtl838x-wdt"

/*
 * Switch core window: syscon, PLLs, table engine, MDIO, PHYs, the CPU-port
 * DMA engine and the forwarding engine.  The device tree gives the ethernet
 * node no "reg" of its own -- every register the NIC driver touches lives in
 * this one window -- so all of it is modelled by a single device, split over
 * rtl838x_switch.c (the window itself), rtl838x_eth.c (the CPU-port DMA
 * engine) and rtl838x_fwd.c (what the silicon does between the ports).
 */
#define TYPE_RTL838X_SWITCH "rtl838x-switch"
OBJECT_DECLARE_SIMPLE_TYPE(RTL838xSwitchState, RTL838X_SWITCH)

/*
 * One front-panel port: an internal PHY plus a QEMU network backend.  The
 * board creates eight of them, and each claims the next unused -nic, so
 * backends bind to lan1, lan2, ... in command-line order.
 */
#define TYPE_RTL838X_PORT "rtl838x-port"
typedef struct RTL838xPortState RTL838xPortState;

#define RTL838X_SW_REGS         (RTL838X_SW_SIZE / 4)

/*
 * Port numbering is the hardware's, not Linux's: the eight RTL8218B ports the
 * device tree labels lan1..lan8 are ports 8..15, and the CPU port is 28.
 */
#define RTL838X_SW_PORT_FIRST   8
#define RTL838X_SW_PORT_LAST    15
#define RTL838X_SW_NUM_PORTS    (RTL838X_SW_PORT_LAST - RTL838X_SW_PORT_FIRST + 1)
#define RTL838X_SW_CPU_PORT     28
#define RTL838X_SW_PHY_REGS     32

/* Table access engine: three command/data window pairs, four tables each. */
#define RTL838X_SW_TABLE_WINDOWS        3
#define RTL838X_SW_TABLES_PER_WINDOW    4

/* Table identifiers, as (window, type) pairs of the array in rtl838x_switch.c. */
#define RTL838X_TBL_VLAN_WINDOW         1
#define RTL838X_TBL_VLAN_TYPE           0
#define RTL838X_TBL_MSTI_WINDOW         1
#define RTL838X_TBL_MSTI_TYPE           2
#define RTL838X_TBL_UNTAG_WINDOW        2
#define RTL838X_TBL_UNTAG_TYPE          0
#define RTL838X_TBL_MC_PMSK_WINDOW      0
#define RTL838X_TBL_MC_PMSK_TYPE        2

/*
 * CPU-port DMA engine: eight receive rings, one per priority, and two
 * transmit rings.  The Linux driver programs two of the eight and the vendor
 * SDK all of them; received frames all go to the first.
 */
#define RTL838X_ETH_RX_RINGS    8
#define RTL838X_ETH_TX_RINGS    2

struct RTL838xSwitchState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;       /* INTC 20: switch core, link change */
    qemu_irq eth_irq;   /* INTC 24: CPU-port DMA */

    uint32_t regs[RTL838X_SW_REGS];
    uint16_t phy[RTL838X_SW_NUM_PORTS][RTL838X_SW_PHY_REGS];

    /* The switch's own address, as the bootloader leaves it programmed. */
    MACAddr macaddr;

    /* Pin straps: what is behind the SerDes, how the flash is addressed. */
    uint32_t int_mode_ctrl;
    bool flash_4byte;

    uint32_t *table[RTL838X_SW_TABLE_WINDOWS][RTL838X_SW_TABLES_PER_WINDOW];

    /* Front-panel ports, indexed from RTL838X_SW_PORT_FIRST. */
    RTL838xPortState *port[RTL838X_SW_NUM_PORTS];

    /* The GPIO controller, which may have an RTL8231 on its lines. */
    RTL838xGpioState *gpio;

    /* Where the DMA engine left off in each ring, in entries. */
    uint32_t rx_cursor[RTL838X_ETH_RX_RINGS];
    uint32_t tx_cursor[RTL838X_ETH_TX_RINGS];

    /*
     * Forwarding database.  The hardware layout is an 8192x4 hash plus a CAM
     * with no valid bit and a bespoke hash function, so this is a plain
     * software table instead; see rtl838x_fwd.c.
     */
    GHashTable *fdb;
};

/* rtl838x_switch.c */
void rtl838x_switch_attach_port(RTL838xSwitchState *s, unsigned port,
                                RTL838xPortState *p);
void rtl838x_switch_link_changed(RTL838xSwitchState *s, unsigned port);
bool rtl838x_switch_link_up(RTL838xSwitchState *s, unsigned port);
uint32_t *rtl838x_switch_table_row(RTL838xSwitchState *s, unsigned window,
                                   unsigned type, uint32_t index);

/* rtl838x_gpio.c: the RTL8231, which the switch's own engine reaches too. */
bool rtl838x_gpio_has_rtl8231(RTL838xGpioState *s);
uint16_t rtl838x_gpio_rtl8231_read(RTL838xGpioState *s, unsigned reg);
void rtl838x_gpio_rtl8231_write(RTL838xGpioState *s, unsigned reg,
                                uint16_t val);

/* rtl838x_port.c */
bool rtl838x_port_link_up(RTL838xPortState *p);
void rtl838x_port_send(RTL838xPortState *p, const uint8_t *buf, size_t len);

/* rtl838x_eth.c: the CPU-port DMA engine. */
bool rtl838x_eth_write(RTL838xSwitchState *s, hwaddr addr, uint32_t val);
bool rtl838x_eth_read(RTL838xSwitchState *s, hwaddr addr, uint32_t *val);
void rtl838x_eth_reset(RTL838xSwitchState *s);
void rtl838x_eth_to_cpu(RTL838xSwitchState *s, unsigned src_port,
                        unsigned reason, uint16_t vid, const uint8_t *buf,
                        size_t len);

/* rtl838x_fwd.c: the forwarding engine. */
void rtl838x_fwd_realize(RTL838xSwitchState *s);
void rtl838x_fwd_reset(RTL838xSwitchState *s);
void rtl838x_fwd_flush(RTL838xSwitchState *s, int port);
void rtl838x_fwd_ingress(RTL838xSwitchState *s, unsigned port,
                         const uint8_t *buf, size_t len);
void rtl838x_fwd_from_cpu(RTL838xSwitchState *s, const uint8_t *buf, size_t len,
                          uint32_t dpm, bool as_dpm, bool learn);

#endif /* HW_MIPS_RTL838X_H */
