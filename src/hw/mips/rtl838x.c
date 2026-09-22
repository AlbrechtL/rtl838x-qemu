/*
 * Realtek RTL8380M (rtl838x) switch SoC board
 *
 * A single-core big-endian MIPS32r2 SoC used in small managed switches, e.g.
 * the Zyxel GS1900-8 that OpenWrt targets.  There is no firmware to emulate:
 * the machine loads a kernel image directly and starts executing it.
 *
 * The kernel carries its device tree appended to itself
 * (CONFIG_MIPS_RAW_APPENDED_DTB=y), so this board never builds or passes a
 * DTB.  That cuts both ways: the guest tells us nothing, and the addresses
 * below have to match the device tree inside the image exactly.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/char/serial-mm.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/unimp.h"
#include "hw/mips/mips.h"
#include "hw/mips/rtl838x.h"
#include "net/net.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"
#include "elf.h"
#include "cpu.h"

/*
 * Legacy U-Boot image header.  OpenWrt's realtek images replace the magic
 * with a vendor value the stock bootloader looks for -- the device tree spells
 * it out as openwrt,ih-magic = <0x83800000> -- but the layout is unchanged.
 * The payload is not the kernel: it is OpenWrt's rt-loader with the
 * LZMA-compressed kernel appended, which relocates itself, decompresses and
 * jumps.  Running that as-is is the point, so that the shipped image boots
 * unmodified.
 */
#define UIMAGE_MAGIC        0x27051956
#define UIMAGE_MAGIC_RTL    0x83800000
#define UIMAGE_HEADER_SIZE  64

typedef struct ResetData {
    MIPSCPU *cpu;
    uint64_t vector;
} ResetData;

static void main_cpu_reset(void *opaque)
{
    ResetData *s = opaque;
    CPUMIPSState *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    env->active_tc.PC = s->vector & ~(target_ulong)1;

    /*
     * Leave the argument registers clear.  The generic MIPS platform treats
     * a0 == -2 as a UHI handoff and would then follow a1 as a DTB pointer;
     * with them zeroed it falls through to the appended DTB, which is what
     * this image expects.
     */
    env->active_tc.gpr[4] = 0;
    env->active_tc.gpr[5] = 0;
    env->active_tc.gpr[6] = 0;
    env->active_tc.gpr[7] = 0;
}

static uint32_t be32_at(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/*
 * Accepts, in order of preference: a uImage (either magic), an ELF vmlinux,
 * or a raw kernel binary.  Returns the entry point.
 */
static uint64_t rtl838x_load_kernel(MachineState *machine)
{
    const char *filename = machine->kernel_filename;
    g_autofree char *buf = NULL;
    g_autoptr(GError) gerr = NULL;
    gsize len = 0;
    uint64_t entry;
    ssize_t size;

    if (!g_file_get_contents(filename, &buf, &len, &gerr)) {
        error_report("could not read kernel '%s': %s", filename,
                     gerr->message);
        exit(1);
    }

    if (len >= UIMAGE_HEADER_SIZE) {
        const uint8_t *hdr = (const uint8_t *)buf;
        uint32_t magic = be32_at(hdr);

        if (magic == UIMAGE_MAGIC || magic == UIMAGE_MAGIC_RTL) {
            uint32_t payload = be32_at(hdr + 12);
            uint64_t load = be32_at(hdr + 16);
            uint64_t ep = be32_at(hdr + 20);
            g_autofree char *name = g_strndup(buf + 32, 32);

            if (payload > len - UIMAGE_HEADER_SIZE) {
                error_report("uImage '%s' is truncated: header claims %u bytes",
                             filename, payload);
                exit(1);
            }
            if (hdr[31] != 0) {
                error_report("uImage '%s' payload is compressed (type %u); "
                             "this machine expects the uncompressed rt-loader",
                             filename, hdr[31]);
                exit(1);
            }

            info_report("loading uImage '%s' (%u bytes) at 0x%" PRIx64
                        ", entry 0x%" PRIx64, name, payload, load, ep);
            rom_add_blob_fixed("rtl838x.kernel", buf + UIMAGE_HEADER_SIZE,
                               payload, cpu_mips_kseg0_to_phys(NULL, load));
            return ep;
        }
    }

    size = load_elf(filename, NULL, cpu_mips_kseg0_to_phys, NULL, &entry, NULL,
                    NULL, NULL, ELFDATA2MSB, EM_MIPS, 1, 0);
    if (size >= 0) {
        return entry;
    }

    /* A raw kernel binary, as produced by unpacking the vendor image. */
    rom_add_blob_fixed("rtl838x.kernel", buf, len,
                       cpu_mips_kseg0_to_phys(NULL, RTL838X_KERNEL_LOAD));
    return RTL838X_KERNEL_LOAD;
}

static void rtl838x_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    MemoryRegion *sram_alias = g_new(MemoryRegion, 1);
    ResetData *reset_info;
    DeviceState *intc, *dev;
    MIPSCPU *cpu;
    CPUMIPSState *env;
    Clock *cpuclk;

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, RTL838X_CPU_HZ);

    cpu = mips_cpu_create_with_clock(machine->cpu_type, cpuclk, true);
    env = &cpu->env;

    reset_info = g_new0(ResetData, 1);
    reset_info->cpu = cpu;
    reset_info->vector = env->active_tc.PC;
    qemu_register_reset(main_cpu_reset, reset_info);

    memory_region_add_subregion(sysmem, RTL838X_RAM_BASE, machine->ram);

    /*
     * On-chip SRAM.  The device tree places it at 0x9f000000, but the clock
     * driver's rate-setting path hands that address straight to a pointer,
     * which on a 32-bit MIPS lands in KSEG0 at 0x1f000000 instead, so back
     * both addresses with the same memory.
     */
    memory_region_init_ram(sram, NULL, "rtl838x.sram", RTL838X_SRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, RTL838X_SRAM_BASE, sram);
    memory_region_init_alias(sram_alias, NULL, "rtl838x.sram-kseg0", sram, 0,
                             RTL838X_SRAM_SIZE);
    memory_region_add_subregion(sysmem, RTL838X_SRAM_KSEG0, sram_alias);

    cpu_mips_irq_init_cpu(cpu);
    cpu_mips_clock_init(cpu);

    /* Catch-alls, so a stray poke is logged with -d unimp instead of read as 0. */
    create_unimplemented_device("rtl838x.soc", RTL838X_SOC_BASE,
                                RTL838X_SOC_SIZE);

    intc = qdev_new(TYPE_RTL838X_INTC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(intc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(intc), 0, RTL838X_INTC_BASE);
    for (unsigned i = 0; i < RTL838X_INTC_NUM_OUT; i++) {
        /* Output i drives MIPS IP(i + 2), i.e. hardware interrupt i. */
        sysbus_connect_irq(SYS_BUS_DEVICE(intc), i, env->irq[i + 2]);
    }

    dev = qdev_new(TYPE_RTL838X_SOCMISC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, RTL838X_MC_BASE);

    dev = qdev_new(TYPE_RTL838X_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, RTL838X_TIMER_BASE);
    static const int timer_irq[RTL838X_TIMER_BANKS] = {
        RTL838X_IRQ_TIMER0, RTL838X_IRQ_TIMER1, RTL838X_IRQ_TIMER2,
        RTL838X_IRQ_TIMER3, RTL838X_IRQ_TIMER4,
    };
    for (unsigned i = 0; i < RTL838X_TIMER_BANKS; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i,
                           qdev_get_gpio_in(intc, timer_irq[i]));
    }

    dev = qdev_new(TYPE_RTL838X_WDT);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, RTL838X_WDT_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(intc, RTL838X_IRQ_WDT_PHASE1));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1,
                       qdev_get_gpio_in(intc, RTL838X_IRQ_WDT_PHASE2));

    dev = qdev_new(TYPE_RTL838X_GPIO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, RTL838X_GPIO_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(intc, RTL838X_IRQ_GPIO));

    /*
     * The switch core, and with it the CPU-port DMA engine: the device tree
     * gives the ethernet node no reg of its own, so both live in this one
     * window -- but not on one interrupt.  The core raises INTC 20 when a link
     * changes, the DMA engine INTC 24 when a ring needs attention.
     */
    dev = qdev_new(TYPE_RTL838X_SWITCH);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, RTL838X_SW_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(intc, RTL838X_IRQ_SWITCH));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1,
                       qdev_get_gpio_in(intc, RTL838X_IRQ_ETH));

    /*
     * The eight front-panel ports the device tree labels lan1..lan8.  Each
     * claims the next unused -nic, so backends bind in command-line order; a
     * port that claims nothing has no cable in it and reports no carrier.
     */
    for (unsigned i = 0; i < RTL838X_SW_NUM_PORTS; i++) {
        DeviceState *port = qdev_new(TYPE_RTL838X_PORT);

        qdev_prop_set_uint8(port, "port", RTL838X_SW_PORT_FIRST + i);
        object_property_set_link(OBJECT(port), "switch", OBJECT(dev),
                                 &error_abort);
        qemu_configure_nic_device(port, true, NULL);
        qdev_realize_and_unref(port, NULL, &error_fatal);
    }

    /*
     * ns16550a, reg-shift 2, byte-wide registers, clocked from LXB.
     *
     * The endianness argument is counter-intuitive here.  The registers are
     * one byte wide at a four byte stride, so on this big-endian bus a 32-bit
     * read returns the register in the *top* lane -- which is what rt-loader
     * relies on when it polls the line status register for 0x20000000 rather
     * than 0x20.  DEVICE_LITTLE_ENDIAN is how that placement is spelled in
     * QEMU: it byte-swaps the 8-bit value the model returns into the high
     * byte of the word.  Byte accesses, which is what Linux uses given the
     * device tree's reg-io-width = <1>, are unaffected either way.
     */
    serial_mm_init(sysmem, RTL838X_UART0_BASE, 2,
                   qdev_get_gpio_in(intc, RTL838X_IRQ_UART0),
                   RTL838X_LXB_HZ / 16, serial_hd(0), DEVICE_LITTLE_ENDIAN);
    if (serial_hd(1)) {
        serial_mm_init(sysmem, RTL838X_UART1_BASE, 2,
                       qdev_get_gpio_in(intc, RTL838X_IRQ_UART1),
                       RTL838X_LXB_HZ / 16, serial_hd(1),
                       DEVICE_LITTLE_ENDIAN);
    }

    if (machine->kernel_filename) {
        reset_info->vector = rtl838x_load_kernel(machine);
    }
}

static void rtl838x_machine_init(MachineClass *mc)
{
    mc->desc = "Realtek RTL8380M switch SoC";
    mc->init = rtl838x_init;
    /* 4KEc plus MIPS16e; see patches/rtl838x.patch for why that matters. */
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("rtl8380");
    /*
     * QEMU adds a default NIC when the command line asks for no networking at
     * all, and lan1 claims it, so a bare boot comes up with user networking on
     * the first port.  Naming the model here is what makes that deliberate
     * rather than incidental; "-nic none" gives a switch with nothing plugged
     * into it.
     */
    mc->default_nic = TYPE_RTL838X_PORT;
    mc->default_ram_id = "rtl838x.ram";
    mc->default_ram_size = 128 * MiB;
    mc->max_cpus = 1;
}

DEFINE_MACHINE("rtl838x", rtl838x_machine_init)
