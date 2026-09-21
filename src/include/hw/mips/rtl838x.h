/*
 * Realtek RTL838x (RTL8380M) switch SoC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MIPS_RTL838X_H
#define HW_MIPS_RTL838X_H

#include "hw/core/sysbus.h"
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

/* Watchdog. */
#define TYPE_RTL838X_WDT "rtl838x-wdt"

/* Switch core window: syscon, PLLs, table engine, MDIO, PHYs. */
#define TYPE_RTL838X_SWITCH "rtl838x-switch"

#endif /* HW_MIPS_RTL838X_H */
