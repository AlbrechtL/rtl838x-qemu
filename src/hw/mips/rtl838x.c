/*
 * Realtek RTL8380M (rtl838x) switch SoC board
 *
 * A single-core big-endian MIPS32r2 SoC used in small managed switches, e.g.
 * the Zyxel GS1900-8 that OpenWrt targets.  There is no firmware to emulate:
 * the machine either loads the kernel image given with -kernel, or does what
 * the stock bootloader does, and loads the uImage from the flash's first
 * image slot -- on every reset, so that a reboot after a firmware update
 * starts the new firmware.  Teltonika's TSW2xx, the same SoC with the same
 * eight ports, keeps its firmware elsewhere in the flash; that is looked at
 * when the GS1900's slot is empty.
 *
 * The OpenWrt kernel carries its device tree appended to itself
 * (CONFIG_MIPS_RAW_APPENDED_DTB=y), so this board never builds or passes a
 * DTB.  That cuts both ways: the guest tells us nothing, and the addresses
 * below have to match the device tree inside the image exactly.
 *
 * The vendor firmware, a Linux 2.6.19 with Realtek's SDK in modules, has no
 * device tree at all and boots on the same machine.  It wants to be booted
 * from flash, with the chip it knows: see the flash-model property.
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
#include "hw/ssi/ssi.h"
#include "exec/tb-flush.h"
#include "net/net.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/runstate.h"
#include "system/reset.h"
#include "system/system.h"
#include "elf.h"
#include "cpu.h"

#include <lzma.h>
#include <zlib.h>

/*
 * Legacy U-Boot image header.  OpenWrt's realtek images replace the magic
 * with a vendor value the stock bootloader looks for -- the device tree spells
 * it out as openwrt,ih-magic = <0x83800000> -- but the layout is unchanged.
 * The payload is not the kernel: it is OpenWrt's rt-loader with the
 * LZMA-compressed kernel appended, which relocates itself, decompresses and
 * jumps.  Running that as-is is the point, so that the shipped image boots
 * unmodified.  The vendor's images use the same magic around a gzip'ed
 * kernel, which the bootloader, and so this machine, inflates.  Teltonika's
 * RutOS images are a plain uImage around an LZMA kernel, with no rt-loader,
 * and get the same treatment.
 */
#define UIMAGE_MAGIC        0x27051956
#define UIMAGE_MAGIC_RTL    0x83800000
#define UIMAGE_HEADER_SIZE  64
#define UIMAGE_COMP_NONE    0
#define UIMAGE_COMP_GZIP    1
#define UIMAGE_COMP_LZMA    3
/* Room to unpack a payload into; the kernels here are all under 8 MiB. */
#define UIMAGE_UNPACK_MAX   (32 * MiB)

/*
 * Where the stock bootloaders keep their environment: the GS1900's at
 * 0x40000, the TSW2xx's at 0x80000.  64 KiB each, a CRC32 of the rest first.
 */
#define UBOOT_ENV_SIZE      0x10000
static const uint32_t uboot_env_offsets[] = { 0x40000, 0x80000 };

struct RTL838xMachineState {
    MachineState parent_obj;

    char *flash_model;
};

#define TYPE_RTL838X_MACHINE MACHINE_TYPE_NAME("rtl838x")
OBJECT_DECLARE_SIMPLE_TYPE(RTL838xMachineState, RTL838X_MACHINE)

typedef struct ResetData {
    MIPSCPU *cpu;
    uint64_t vector;
    BlockBackend *flash;    /* boot from here, when there is no -kernel */
} ResetData;

static uint64_t rtl838x_load_flash(BlockBackend *blk, Error **errp);

static void main_cpu_reset(void *opaque)
{
    ResetData *s = opaque;
    CPUMIPSState *env = &s->cpu->env;

    if (s->flash) {
        Error *err = NULL;

        s->vector = rtl838x_load_flash(s->flash, &err);
        if (err) {
            /* The stock bootloader would stop at its prompt; stop instead. */
            error_report_err(err);
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
    }

    cpu_reset(CPU(s->cpu));
    /*
     * Forget the code the previous boot ran.  Its translations are still
     * cached, so every page they came from is write-protected for TCG, and
     * the next boot, which rewrites that memory from rt-loader onwards,
     * takes the slow path on every store: freeing the kernel's init memory
     * alone took over ten seconds after a reboot.
     */
    queue_tb_flush(CPU(s->cpu));
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
 * What the bootloader does with "ethaddr": it programs the switch's MAC
 * address registers with it, and the firmware takes its own address from
 * there.  Teltonika's relies on that address being the device's, which its
 * bridge also uses: the driver installs the one it found as the static L2
 * entry that brings frames for the switch itself to the CPU.  Returns false
 * if the flash holds no environment with a usable ethaddr.
 */
static bool rtl838x_flash_ethaddr(BlockBackend *blk, MACAddr *mac)
{
    g_autofree uint8_t *env = g_malloc(UBOOT_ENV_SIZE + 1);

    for (unsigned i = 0; i < ARRAY_SIZE(uboot_env_offsets); i++) {
        const char *var;

        if (blk_pread(blk, uboot_env_offsets[i], UBOOT_ENV_SIZE, env, 0) < 0 ||
            be32_at(env) != crc32(0, env + 4, UBOOT_ENV_SIZE - 4)) {
            continue;
        }
        env[UBOOT_ENV_SIZE] = 0;
        for (var = (char *)env + 4; *var; var += strlen(var) + 1) {
            unsigned a[6];

            if (sscanf(var, "ethaddr=%x:%x:%x:%x:%x:%x", &a[0], &a[1],
                       &a[2], &a[3], &a[4], &a[5]) == 6) {
                for (unsigned b = 0; b < 6; b++) {
                    mac->a[b] = a[b];
                }
                return !(mac->a[0] & 1);
            }
        }
    }
    return false;
}

/*
 * Checks a legacy uImage header against the size of what it came from.
 * Returns the payload size, or 0 with errp set.
 */
static uint32_t rtl838x_check_uimage(const uint8_t *hdr, uint64_t len,
                                     const char *what, Error **errp)
{
    uint32_t payload = be32_at(hdr + 12);

    if (payload > len - UIMAGE_HEADER_SIZE) {
        error_setg(errp, "uImage in %s is truncated: header claims %u bytes",
                   what, payload);
        return 0;
    }
    if (hdr[31] != UIMAGE_COMP_NONE && hdr[31] != UIMAGE_COMP_GZIP &&
        hdr[31] != UIMAGE_COMP_LZMA) {
        error_setg(errp, "uImage in %s has a compressed payload (type %u); "
                   "only uncompressed, gzip and lzma payloads are supported",
                   what, hdr[31]);
        return 0;
    }
    return payload;
}

/*
 * An LZMA payload, as U-Boot's bootm takes it: the 13-byte header of the
 * "alone" format, then the stream.  Returns the unpacked size, or -1.
 */
static ssize_t rtl838x_unlzma(uint8_t *out, size_t out_size,
                              const uint8_t *in, size_t in_size)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_ret ret;

    if (lzma_alone_decoder(&strm, UINT64_MAX) != LZMA_OK) {
        return -1;
    }
    strm.next_in = in;
    strm.avail_in = in_size;
    strm.next_out = out;
    strm.avail_out = out_size;
    ret = lzma_code(&strm, LZMA_FINISH);
    lzma_end(&strm);
    return ret == LZMA_STREAM_END ? (ssize_t)strm.total_out : -1;
}

/*
 * What the bootloader's bootm does with the payload before jumping to it.
 * OpenWrt's is the uncompressed rt-loader and is used as it is; the vendor
 * firmware's is a gzip'ed kernel and Teltonika's an LZMA one, which the
 * bootloader unpacks to the load address.  Returns the bytes to place
 * there, which the caller frees.
 */
static uint8_t *rtl838x_unpack_uimage(const uint8_t *hdr, const uint8_t *payload,
                                      uint32_t *size, const char *what,
                                      Error **errp)
{
    g_autofree uint8_t *out = NULL;
    ssize_t len;

    if (hdr[31] == UIMAGE_COMP_NONE) {
        return g_memdup2(payload, *size);
    }

    out = g_malloc(UIMAGE_UNPACK_MAX);
    if (hdr[31] == UIMAGE_COMP_LZMA) {
        len = rtl838x_unlzma(out, UIMAGE_UNPACK_MAX, payload, *size);
    } else {
        len = gunzip(out, UIMAGE_UNPACK_MAX, (uint8_t *)payload, *size);
    }
    if (len < 0) {
        error_setg(errp, "could not unpack the uImage in %s", what);
        return NULL;
    }
    *size = len;
    return g_realloc(g_steal_pointer(&out), len);
}

/*
 * The stock bootloader's part: the uImage at the start of the flash's first
 * image slot, copied to its load address.  The GS1900's slot is tried
 * first, then the TSW2xx's.  Reads the flash through the block layer, so a
 * firmware update the guest wrote is what the next reset boots.  Returns
 * the entry point.
 */
/*
 * Where in the flash the firmware is: the GS1900's first image slot, else
 * the TSW2xx's, with its uImage header in hdr.  0 if neither holds one.
 */
static uint32_t rtl838x_flash_slot(BlockBackend *blk,
                                   uint8_t hdr[UIMAGE_HEADER_SIZE])
{
    static const uint32_t slots[] = {
        RTL838X_FLASH_FIRMWARE, RTL838X_FLASH_FIRMWARE_TSW,
    };

    for (unsigned i = 0; i < ARRAY_SIZE(slots); i++) {
        uint32_t magic;

        if (blk_pread(blk, slots[i], UIMAGE_HEADER_SIZE, hdr, 0) < 0) {
            continue;
        }
        magic = be32_at(hdr);
        if (magic == UIMAGE_MAGIC || magic == UIMAGE_MAGIC_RTL) {
            return slots[i];
        }
    }
    return 0;
}

static uint64_t rtl838x_load_flash(BlockBackend *blk, Error **errp)
{
    uint8_t hdr[UIMAGE_HEADER_SIZE];
    g_autofree uint8_t *payload = NULL;
    g_autofree uint8_t *image = NULL;
    uint32_t size, slot;
    uint64_t load, ep;

    /* Writes the flash model has started but not finished. */
    blk_drain(blk);

    slot = rtl838x_flash_slot(blk, hdr);
    if (!slot) {
        error_setg(errp, "no uImage in flash at 0x%x or 0x%x: nothing to "
                   "boot; install a firmware first, or boot one with -kernel",
                   RTL838X_FLASH_FIRMWARE, RTL838X_FLASH_FIRMWARE_TSW);
        return 0;
    }
    size = rtl838x_check_uimage(hdr, RTL838X_FLASH_SIZE - slot, "flash",
                                errp);
    if (!size) {
        return 0;
    }
    load = be32_at(hdr + 16);
    ep = be32_at(hdr + 20);

    payload = g_malloc(size);
    if (blk_pread(blk, slot + UIMAGE_HEADER_SIZE, size, payload, 0) < 0) {
        error_setg(errp, "could not read the flash");
        return 0;
    }
    image = rtl838x_unpack_uimage(hdr, payload, &size, "flash", errp);
    if (!image) {
        return 0;
    }
    address_space_write(&address_space_memory,
                        cpu_mips_kseg0_to_phys(NULL, load),
                        MEMTXATTRS_UNSPECIFIED, image, size);
    return ep;
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
            uint64_t load = be32_at(hdr + 16);
            uint64_t ep = be32_at(hdr + 20);
            g_autofree char *name = g_strndup(buf + 32, 32);
            uint32_t payload = rtl838x_check_uimage(hdr, len, filename,
                                                    &error_fatal);
            g_autofree uint8_t *image =
                rtl838x_unpack_uimage(hdr, hdr + UIMAGE_HEADER_SIZE, &payload,
                                      filename, &error_fatal);

            info_report("loading uImage '%s' (%u bytes) at 0x%" PRIx64
                        ", entry 0x%" PRIx64, name, payload, load, ep);
            rom_add_blob_fixed("rtl838x.kernel", image, payload,
                               cpu_mips_kseg0_to_phys(NULL, load));
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
    RTL838xMachineState *rms = RTL838X_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    MemoryRegion *sram_alias = g_new(MemoryRegion, 1);
    ResetData *reset_info;
    DeviceState *intc, *dev, *flash;
    DriveInfo *flash_dinfo;
    MACAddr mac;
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

    /*
     * The SPI-NOR flash on chip select 0 of the controller in that window,
     * backed by "-drive if=mtd" when there is one.
     */
    flash_dinfo = drive_get(IF_MTD, 0, 0);
    if (!object_class_by_name(rms->flash_model)) {
        error_report("flash-model '%s' is not a flash chip QEMU models",
                     rms->flash_model);
        exit(1);
    }
    flash = qdev_new(rms->flash_model);
    if (flash_dinfo) {
        qdev_prop_set_drive_err(flash, "drive",
                                blk_by_legacy_dinfo(flash_dinfo), &error_fatal);
    }
    qdev_realize_and_unref(flash, qdev_get_child_bus(dev, "spi"), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0));

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
    /*
     * The board's pull-ups.  A flash with the firmware where the TSW2xx
     * keeps it is taken for one, and that board pulls its GPIO lines up; see
     * rtl838x_gpio.c.  The GS1900 is left as it was.
     */
    if (flash_dinfo) {
        uint8_t hdr[UIMAGE_HEADER_SIZE];

        if (rtl838x_flash_slot(blk_by_legacy_dinfo(flash_dinfo), hdr) ==
            RTL838X_FLASH_FIRMWARE_TSW) {
            qdev_prop_set_uint32(dev, "pull-ups", RTL838X_GPIO_TSW_PULLUPS);
        }
    }
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
    /*
     * The switch's MAC address.  The real switch has its own in the U-Boot
     * environment, and so does a flash image that carries one.  Otherwise
     * it is a random, locally administered one, so that switches started
     * side by side differ, but it is fixed for the life of the QEMU process,
     * so that a reboot does not change it.
     */
    if (!flash_dinfo ||
        !rtl838x_flash_ethaddr(blk_by_legacy_dinfo(flash_dinfo), &mac)) {
        for (unsigned i = 0; i < 6; i++) {
            mac.a[i] = g_random_int_range(0, 256);
        }
        mac.a[0] = (mac.a[0] & 0xfc) | 0x02;
    }
    qdev_prop_set_macaddr(dev, "macaddr", mac.a);
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
    } else if (flash_dinfo) {
        reset_info->flash = blk_by_legacy_dinfo(flash_dinfo);
        /* Fail now rather than at the first reset, with no output. */
        rtl838x_load_flash(reset_info->flash, &error_fatal);
    } else {
        error_report("nothing to boot: give a kernel image with -kernel, or "
                     "a flash image with -drive if=mtd,format=raw,file=...");
        exit(1);
    }
}

static char *rtl838x_get_flash_model(Object *obj, Error **errp)
{
    return g_strdup(RTL838X_MACHINE(obj)->flash_model);
}

static void rtl838x_set_flash_model(Object *obj, const char *value,
                                    Error **errp)
{
    RTL838xMachineState *rms = RTL838X_MACHINE(obj);

    g_free(rms->flash_model);
    rms->flash_model = g_strdup(value);
}

static void rtl838x_machine_instance_init(Object *obj)
{
    RTL838X_MACHINE(obj)->flash_model = g_strdup(RTL838X_FLASH_TYPE);
}

static void rtl838x_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    /*
     * Which chip the 16 MiB flash identifies as.  The two firmwares disagree:
     * a current kernel needs SFDP tables from a chip with the real one's
     * JEDEC ID, which QEMU's model of it lacks, hence the default; the vendor
     * kernel knows nothing but the real ID, c2 20 18, "mx25l12805d" here.
     */
    object_class_property_add_str(oc, "flash-model", rtl838x_get_flash_model,
                                  rtl838x_set_flash_model);
    object_class_property_set_description(oc, "flash-model",
        "m25p80 model of the SPI-NOR flash (default " RTL838X_FLASH_TYPE ")");

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

static const TypeInfo rtl838x_machine_types[] = {
    {
        .name          = TYPE_RTL838X_MACHINE,
        .parent        = TYPE_MACHINE,
        .instance_size = sizeof(RTL838xMachineState),
        .instance_init = rtl838x_machine_instance_init,
        .class_init    = rtl838x_machine_class_init,
    },
};

DEFINE_TYPES(rtl838x_machine_types)
