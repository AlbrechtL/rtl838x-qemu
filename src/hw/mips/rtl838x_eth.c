/*
 * Realtek RTL838x CPU-port DMA engine
 *
 * The device tree gives the "realtek,rtl8380-eth" node no reg of its own: its
 * registers are a handful of offsets inside the switch window at 0x1b000000,
 * so this file is reached from rtl838x_switch.c rather than owning any MMIO.
 * What it implements is the path between the switch fabric and the guest's
 * memory, in both directions.
 *
 * The layout is two levels deep.  A ring is an array of 32-bit entries, each
 * holding the physical address of a 32-byte descriptor plus two flag bits in
 * the low bits the alignment leaves free:
 *
 *      bit 0  OWN   set means the hardware owns the entry
 *      bit 1  WRAP  set on the last entry; the walk restarts at zero
 *
 * The descriptor carries the buffer pointer, its capacity, the frame length
 * and a 20-byte CPU tag.  The tag is not part of the frame: the source port,
 * the queue and the reason a frame was handed to the CPU live only there, and
 * the driver reads them back out of the descriptor after the fact.
 *
 * Two conventions in the driver are easy to get wrong and silent when wrong:
 *
 *  - Every buffer ends in four bytes of FCS space.  The driver trims them on
 *    receive without looking at them, and on transmit it counts them in "len"
 *    while expecting the hardware to replace them -- on a DSA frame they are
 *    the [port, 0xab, 0xcd, 0xef] trailer the tag driver deliberately left
 *    there for exactly this purpose.  So: strip four bytes on the way out,
 *    append four on the way in.  The exception is a frame too short to be
 *    on a wire at all, which the hardware pads instead; the vendor SDK
 *    queues those without the four bytes.
 *
 *  - Reason 6 ("special trap") is the only value that makes the driver clear
 *    l2_offloaded, which is what stops the bridge from assuming the switch has
 *    already forwarded the frame.  Trapped frames must carry it; ordinary ones
 *    must not.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/mips/rtl838x.h"
#include "net/eth.h"
#include "qemu/log.h"
#include "system/address-spaces.h"
#include "system/dma.h"

/* Registers, all offsets inside the switch window. */
#define ETH_DMA_RX_BASE         0x9f00  /* + ring * 4 */
#define ETH_DMA_RX_CUR          0x9f20  /* + ring * 4, read only */
#define ETH_DMA_TX_BASE         0x9f40  /* + ring * 4 */
#define ETH_DMA_TX_CUR          0x9f48  /* + ring * 4, read only */
#define ETH_DMA_IF_INTR_MSK     0x9f50
#define ETH_DMA_IF_INTR_STS     0x9f54
#define ETH_DMA_IF_CTRL         0x9f58

/*
 * CTRL bits.  The driver enables the engine by setting 0xc, then pokes bit 1
 * every time it queues a frame; bit 1 is a trigger, not a state, and reads
 * back clear.  Bit 5 asks the hardware to pad short frames (it already does)
 * and bit 4 would truncate long receives (the driver clears it).
 */
#define ETH_CTRL_RX_RESTART     (1u << 0)
#define ETH_CTRL_TX_FETCH       (1u << 1)
#define ETH_CTRL_ENABLE         (3u << 2)

/* Interrupt status: receive ran out in [7:0], received in [15:8], one bit a
 * ring; then transmitted and "everything queued was transmitted" per ring. */
#define ETH_INTR_TX_DONE(r)     (1u << (16 + (r)))
#define ETH_INTR_TX_ALL_DONE(r) (1u << (18 + (r)))

/* Ring entry. */
#define RING_OWN_HW             (1u << 0)
#define RING_WRAP               (1u << 1)
#define RING_ADDR_MASK          (~3u)

/* Descriptor. */
#define FRAG_DMA                0x00    /* u32, buffer address */
#define FRAG_SIZE               0x06    /* u16, buffer capacity */
#define FRAG_FLAGS              0x08    /* u16, more in bit 15, offset below */
#define FRAG_LEN                0x0a    /* u16, bytes in this fragment */
#define FRAG_CPU_TAG            0x0c    /* u16[6], then 8 bytes of the driver's */
#define FRAG_CPU_TAG_WORDS      6
#define FRAG_MORE               (1u << 15)

/* CPU tag fields the driver puts there (transmit) and reads back (receive). */
#define TAG_TX_MARKER           (1u << 10)  /* in word 1 */
#define TAG_TX_AS_DPM           (1u << 9)   /* in word 2 */

/*
 * The largest frame this family switches, per its datasheet, plus the tag and
 * FCS the driver counts.  Anything beyond this is a descriptor the guest got
 * wrong rather than a frame.
 */
#define ETH_FRAME_MAX           10240
#define ETH_MAX_FRAGS           64

/*
 * Set to 1 to trace every frame crossing the CPU port.  Bringing the data path
 * up is mostly a matter of seeing which direction stopped working.
 */
#define RTL838X_ETH_DEBUG 0

#define eth_dbg(fmt, ...)                                           \
    do {                                                            \
        if (RTL838X_ETH_DEBUG) {                                    \
            qemu_log("rtl838x-eth: " fmt, ## __VA_ARGS__);          \
        }                                                           \
    } while (0)

static uint32_t eth_ldl(hwaddr addr)
{
    return address_space_ldl_be(&address_space_memory, addr,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

static uint16_t eth_lduw(hwaddr addr)
{
    return address_space_lduw_be(&address_space_memory, addr,
                                 MEMTXATTRS_UNSPECIFIED, NULL);
}

static void eth_stl(hwaddr addr, uint32_t val)
{
    address_space_stl_be(&address_space_memory, addr, val,
                         MEMTXATTRS_UNSPECIFIED, NULL);
}

static void eth_stw(hwaddr addr, uint16_t val)
{
    address_space_stw_be(&address_space_memory, addr, val,
                         MEMTXATTRS_UNSPECIFIED, NULL);
}

static bool eth_enabled(RTL838xSwitchState *s)
{
    return (s->regs[ETH_DMA_IF_CTRL / 4] & ETH_CTRL_ENABLE) == ETH_CTRL_ENABLE;
}

static void rtl838x_eth_update_irq(RTL838xSwitchState *s)
{
    uint32_t pending = s->regs[ETH_DMA_IF_INTR_STS / 4] &
                       s->regs[ETH_DMA_IF_INTR_MSK / 4];

    qemu_set_irq(s->eth_irq, !!pending);
}

/* ------------------------------------------------------------------- send */

/*
 * Pulls one frame off a transmit ring and hands it to the forwarding engine.
 * Returns false once the ring holds nothing the hardware owns.
 */
static bool rtl838x_eth_tx_frame(RTL838xSwitchState *s, unsigned ring,
                                 uint32_t base)
{
    g_autofree uint8_t *frame = NULL;
    bool as_dpm = false, more = true;
    unsigned frags = 0;
    uint32_t dpm = 0;
    size_t len = 0;

    if (!(eth_ldl(base + s->tx_cursor[ring] * 4) & RING_OWN_HW)) {
        return false;
    }

    frame = g_malloc(ETH_FRAME_MAX);

    while (more && frags < ETH_MAX_FRAGS) {
        uint32_t idx = s->tx_cursor[ring];
        hwaddr entry_addr = base + idx * 4;
        uint32_t entry = eth_ldl(entry_addr);
        hwaddr frag;
        uint16_t flen;

        if (!(entry & RING_OWN_HW)) {
            /* A chain the guest has not finished queueing; wait for the rest. */
            break;
        }

        frag = entry & RING_ADDR_MASK;
        flen = eth_lduw(frag + FRAG_LEN);
        more = eth_lduw(frag + FRAG_FLAGS) & FRAG_MORE;

        if (frags == 0) {
            /*
             * Only the first descriptor of a frame carries a CPU tag.  With
             * AS_DPM the destination port mask in it is an order, not a hint:
             * that is how the guest reaches a specific port, and how BPDUs
             * leave a port that spanning tree is otherwise blocking.
             */
            if (eth_lduw(frag + FRAG_CPU_TAG + 1 * 2) & TAG_TX_MARKER) {
                as_dpm = eth_lduw(frag + FRAG_CPU_TAG + 2 * 2) & TAG_TX_AS_DPM;
                dpm = (uint32_t)eth_lduw(frag + FRAG_CPU_TAG + 4 * 2) << 16 |
                      eth_lduw(frag + FRAG_CPU_TAG + 5 * 2);
            }
        }

        if (len + flen <= ETH_FRAME_MAX) {
            dma_memory_read(&address_space_memory,
                            eth_ldl(frag + FRAG_DMA), frame + len, flen,
                            MEMTXATTRS_UNSPECIFIED);
            len += flen;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "rtl838x-eth: transmit frame longer than %u bytes\n",
                          ETH_FRAME_MAX);
            len = 0;
        }

        /* Handing the entry back is what lets the driver reclaim the skb. */
        eth_stl(entry_addr, entry & ~RING_OWN_HW);
        s->tx_cursor[ring] = (entry & RING_WRAP) ? 0 : idx + 1;
        frags++;
    }

    if (len < ETH_HLEN + ETH_FCS_LEN) {
        return true;    /* runt, or a frame we gave up on; the slot is freed */
    }

    if (len >= ETH_ZLEN + ETH_FCS_LEN) {
        len -= ETH_FCS_LEN;
    } else {
        /*
         * Shorter than the wire allows: no room was left for an FCS, and the
         * hardware pads what it was given.  Linux never gets here, it pads
         * and adds the four bytes itself.  The vendor driver adds them only
         * to frames that are long enough already, so its ARP requests arrive
         * as 42 bytes, all of them payload.
         */
        memset(frame + len, 0, ETH_ZLEN - len);
        len = ETH_ZLEN;
    }

    eth_dbg("tx ring %u: %zu bytes, dpm 0x%08x%s\n", ring, len,
            dpm, as_dpm ? "" : " (no tag)");
    rtl838x_fwd_from_cpu(s, frame, len, dpm, as_dpm);

    return true;
}

static void rtl838x_eth_tx(RTL838xSwitchState *s)
{
    if (!eth_enabled(s)) {
        return;
    }

    for (unsigned ring = 0; ring < RTL838X_ETH_TX_RINGS; ring++) {
        uint32_t base = s->regs[(ETH_DMA_TX_BASE + ring * 4) / 4];
        bool sent = false;

        if (!base) {
            continue;
        }
        while (rtl838x_eth_tx_frame(s, ring, base)) {
            /* Drain the ring: one trigger may cover several queued frames. */
            sent = true;
        }
        if (sent) {
            /*
             * The Linux driver reclaims its buffers from the ownership bits
             * and only acknowledges this; the vendor SDK frees a transmitted
             * packet from the interrupt and leaks it without one.
             */
            s->regs[ETH_DMA_IF_INTR_STS / 4] |= ETH_INTR_TX_DONE(ring) |
                                                ETH_INTR_TX_ALL_DONE(ring);
        }
    }
    rtl838x_eth_update_irq(s);
}

/* ---------------------------------------------------------------- receive */

/*
 * Hands one frame to the guest, splitting it over as many descriptors as its
 * buffers need.  A partially delivered frame would poison the driver's
 * reassembly state, so the ring is walked once to check it fits before
 * anything is written.
 */
void rtl838x_eth_to_cpu(RTL838xSwitchState *s, unsigned src_port,
                        unsigned reason, const uint8_t *buf, size_t len)
{
    const unsigned ring = 0;
    size_t total = len + ETH_FCS_LEN;
    uint32_t base = s->regs[(ETH_DMA_RX_BASE + ring * 4) / 4];
    unsigned frags = 0;
    size_t space = 0;
    uint32_t idx;
    size_t done;

    if (!base || !eth_enabled(s) || len < ETH_HLEN || total > ETH_FRAME_MAX) {
        return;
    }

    idx = s->rx_cursor[ring];
    while (space < total) {
        uint32_t entry = eth_ldl(base + idx * 4);
        uint16_t size;

        if (!(entry & RING_OWN_HW) || frags++ >= ETH_MAX_FRAGS) {
            eth_dbg("rx ring %u: out of buffers, dropping %zu bytes\n",
                    ring, len);
            return;
        }

        size = eth_lduw((entry & RING_ADDR_MASK) + FRAG_SIZE);
        if (!size) {
            return;     /* a descriptor with no room behind it */
        }
        space += size;
        idx = (entry & RING_WRAP) ? 0 : idx + 1;
    }

    for (done = 0; done < total; ) {
        uint32_t entry = eth_ldl(base + s->rx_cursor[ring] * 4);
        hwaddr frag = entry & RING_ADDR_MASK;
        hwaddr dma = eth_ldl(frag + FRAG_DMA);
        size_t chunk = MIN(eth_lduw(frag + FRAG_SIZE), total - done);
        bool last;

        /* The frame first, then its FCS padding if it reaches into here. */
        if (done < len) {
            size_t payload = MIN(chunk, len - done);

            dma_memory_write(&address_space_memory, dma, buf + done, payload,
                             MEMTXATTRS_UNSPECIFIED);
            if (chunk > payload) {
                uint8_t fcs[ETH_FCS_LEN] = { 0 };

                dma_memory_write(&address_space_memory, dma + payload, fcs,
                                 chunk - payload, MEMTXATTRS_UNSPECIFIED);
            }
        } else {
            uint8_t fcs[ETH_FCS_LEN] = { 0 };

            dma_memory_write(&address_space_memory, dma, fcs, chunk,
                             MEMTXATTRS_UNSPECIFIED);
        }

        done += chunk;
        last = done >= total;

        eth_stw(frag + FRAG_FLAGS, last ? 0 : FRAG_MORE);
        eth_stw(frag + FRAG_LEN, chunk);

        /*
         * The tag goes on every fragment; the driver only reads the first
         * one's, but leaving stale values behind in the others is asking for
         * confusion later.  Word 1 carries the source port and queue, word 4
         * the reason.
         *
         * The tag is six words.  Linux declares ten, which takes the
         * descriptor to a round 32 bytes, but the last eight bytes are the
         * driver's to use: the vendor SDK keeps two pointers of its own
         * there and follows them from its interrupt handler.
         */
        for (unsigned w = 0; w < FRAG_CPU_TAG_WORDS; w++) {
            eth_stw(frag + FRAG_CPU_TAG + w * 2, 0);
        }
        eth_stw(frag + FRAG_CPU_TAG + 1 * 2, src_port & 0x1f);
        eth_stw(frag + FRAG_CPU_TAG + 4 * 2, reason & 0xf);

        /* Ownership last: everything above must be visible before the CPU
         * is allowed to look at the descriptor. */
        eth_stl(base + s->rx_cursor[ring] * 4, entry & ~RING_OWN_HW);
        s->rx_cursor[ring] = (entry & RING_WRAP) ? 0 : s->rx_cursor[ring] + 1;
    }

    eth_dbg("rx ring %u: %zu bytes from port %u, reason %u\n",
            ring, len, src_port, reason);

    /*
     * The driver's handler folds the "ring has work" and "ring ran out of
     * buffers" halves of the status word together, and unmasks both, so raise
     * both bits.  The interrupt is a level: it stays up until the status is
     * cleared or the mask is taken away.
     */
    s->regs[ETH_DMA_IF_INTR_STS / 4] |= (1u << ring) | (1u << (ring + 8));
    rtl838x_eth_update_irq(s);
}

/* --------------------------------------------------------------- registers */

/*
 * Where the engine is in each ring, as the address of the entry it will look
 * at next.  The vendor SDK reads the transmit one to find out how far the
 * hardware got.
 */
bool rtl838x_eth_read(RTL838xSwitchState *s, hwaddr addr, uint32_t *val)
{
    if (addr >= ETH_DMA_RX_CUR &&
        addr < ETH_DMA_RX_CUR + RTL838X_ETH_RX_RINGS * 4) {
        unsigned ring = (addr - ETH_DMA_RX_CUR) / 4;

        *val = s->regs[(ETH_DMA_RX_BASE + ring * 4) / 4] +
               s->rx_cursor[ring] * 4;
        return true;
    }
    if (addr >= ETH_DMA_TX_CUR &&
        addr < ETH_DMA_TX_CUR + RTL838X_ETH_TX_RINGS * 4) {
        unsigned ring = (addr - ETH_DMA_TX_CUR) / 4;

        *val = s->regs[(ETH_DMA_TX_BASE + ring * 4) / 4] +
               s->tx_cursor[ring] * 4;
        return true;
    }
    return false;
}

bool rtl838x_eth_write(RTL838xSwitchState *s, hwaddr addr, uint32_t val)
{
    switch (addr) {
    case ETH_DMA_IF_INTR_STS:
        s->regs[addr / 4] &= ~val;      /* write one to clear */
        rtl838x_eth_update_irq(s);
        return true;

    case ETH_DMA_IF_INTR_MSK:
        s->regs[addr / 4] = val;
        rtl838x_eth_update_irq(s);
        return true;

    case ETH_DMA_IF_CTRL:
        s->regs[addr / 4] = val & ~ETH_CTRL_TX_FETCH;
        if (val & ETH_CTRL_TX_FETCH) {
            rtl838x_eth_tx(s);
        }
        return true;

    default:
        break;
    }

    /* Programming a ring base restarts the walk of that ring. */
    if (addr >= ETH_DMA_RX_BASE &&
        addr < ETH_DMA_RX_BASE + RTL838X_ETH_RX_RINGS * 4) {
        s->regs[addr / 4] = val;
        s->rx_cursor[(addr - ETH_DMA_RX_BASE) / 4] = 0;
        return true;
    }
    if (addr >= ETH_DMA_TX_BASE &&
        addr < ETH_DMA_TX_BASE + RTL838X_ETH_TX_RINGS * 4) {
        s->regs[addr / 4] = val;
        s->tx_cursor[(addr - ETH_DMA_TX_BASE) / 4] = 0;
        return true;
    }

    return false;
}

void rtl838x_eth_reset(RTL838xSwitchState *s)
{
    memset(s->rx_cursor, 0, sizeof(s->rx_cursor));
    memset(s->tx_cursor, 0, sizeof(s->tx_cursor));

    for (unsigned r = 0; r < RTL838X_ETH_RX_RINGS; r++) {
        s->regs[(ETH_DMA_RX_BASE + r * 4) / 4] = 0;
    }
    for (unsigned r = 0; r < RTL838X_ETH_TX_RINGS; r++) {
        s->regs[(ETH_DMA_TX_BASE + r * 4) / 4] = 0;
    }

    s->regs[ETH_DMA_IF_CTRL / 4] = 0;
    s->regs[ETH_DMA_IF_INTR_STS / 4] = 0;
    s->regs[ETH_DMA_IF_INTR_MSK / 4] = 0;
    qemu_set_irq(s->eth_irq, 0);
}
