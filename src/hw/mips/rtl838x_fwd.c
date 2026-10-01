/*
 * Realtek RTL838x forwarding engine
 *
 * What the silicon does between the ports, reconstructed from the state the
 * DSA driver has already programmed into the switch window: VLAN membership
 * and untagged-egress masks from the table engine, per-port spanning tree
 * state from the MSTI table, the port isolation matrix, the port-based VLAN
 * registers and the flood mask.  Nothing new is stored on the register side;
 * this file only reads it back.
 *
 * The one thing it does keep is the forwarding database.  The hardware layout
 * is an 8192x4 hash plus a CAM with no valid bit and a hash function nobody
 * has written down, so reproducing it would be a lot of work for no observable
 * difference: a plain software table forwards identically.  What the driver
 * programs into the hardware table is not ignored, though: a unicast address
 * nothing has been learned for is looked up among its static entries, with a
 * scan rather than the hash.  That is how a switch's own address reaches the
 * CPU when the CPU port does not learn -- the driver installs it -- and it is
 * all a static entry is consulted for.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/mips/rtl838x.h"
#include "net/eth.h"
#include "qemu/log.h"

#define FWD_PORT_ISO_CTRL(p)    (0x4100 + (p) * 4)

/*
 * A port classifies frames by one of two VLAN tags, the inner (customer) or
 * the outer (service) one: VLAN_PORT_FWD has a bit per port, set for outer.
 * Each has its own PVID in PB_VLAN and its own pair of acceptable frame type
 * bits.  The drivers differ here: Linux and RutOS stay with the inner tag
 * and leave this register at zero, while Zyxel's firmware switches every
 * port to the outer one -- which it then matches against 0x8100, so it is
 * the same tag on the wire -- and never touches an inner PVID.
 */
#define FWD_VLAN_PORT_FWD       0x3a78
#define FWD_VLAN_PORT_PB_VLAN(p) (0x3c00 + (p) * 4)
#define FWD_PB_VLAN_INNER(v)    (((v) >> 2) & 0xfff)
#define FWD_PB_VLAN_OUTER(v)    (((v) >> 16) & 0xfff)

/*
 * Acceptable frame types, two bits for each tag: [1:0] inner, [3:2] outer.
 * The low bit of a pair admits tagged frames, the high one untagged and
 * priority-tagged frames.  Out of reset a port admits both (the board sets
 * them), which is what Linux relies on: it never writes here.
 */
#define FWD_VLAN_PORT_AFT(p)    (0x3a00 + (p) * 4)
#define FWD_AFT_TAGGED          1
#define FWD_AFT_UNTAGGED        2

/*
 * Ingress filtering: what happens to a frame from a port that is not in its
 * VLAN, two bits a port, sixteen ports a register.  Zyxel's firmware leaves
 * it at forward unless told otherwise; Linux and RutOS drop.
 */
#define FWD_VLAN_PORT_IGR_FLTR(p) (0x3a7c + ((p) / 16) * 4)
#define FWD_IGR_FORWARD         0
#define FWD_IGR_DROP            1
#define FWD_IGR_TRAP            2

/*
 * Flooding is one level of indirection away.  L2_FLD_PMSK does not hold port
 * masks at all: it holds two nine-bit row numbers into the multicast port mask
 * table, one for broadcast and one for unknown unicast.  Reading it as a mask
 * gives something that looks plausible -- the driver's value is 0x3ffff -- and
 * silently keeps every flooded frame away from the CPU port.
 */
#define FWD_L2_FLD_PMSK         0x3288
#define FWD_L2_FLD_UC(v)        ((v) & 0x1ff)
#define FWD_L2_FLD_BC(v)        (((v) >> 9) & 0x1ff)

/*
 * Bit 0 has ARP requests copied to the CPU port whether or not it is in the
 * VLAN they arrived in.  The vendor firmware keeps the CPU port out of every
 * VLAN and relies on this to hear who is asking for its address.
 */
#define FWD_SPCL_TRAP_ARP_CTRL  0x698c

/* The hardware L2 table and its CAM, and the entry bits read from them. */
#define FWD_TBL_L2_WINDOW       0
#define FWD_TBL_L2_TYPE         0
#define FWD_TBL_L2_ROWS         8192
#define FWD_TBL_L2_CAM_TYPE     1
#define FWD_TBL_L2_CAM_ROWS     64
#define FWD_L2_IP_MC            (3u << 21)
#define FWD_L2_STATIC           (1u << 19)

/* Spanning tree states, two bits per port in the MSTI table. */
#define STP_DISABLED            0
#define STP_BLOCKING            1
#define STP_LEARNING            2
#define STP_FORWARDING          3

/*
 * The CPU tag's reason field.  Six is "special trap", the only value that
 * makes the driver clear l2_offloaded and hand the frame to the bridge as
 * something the hardware has *not* already forwarded.
 */
#define FWD_REASON_FORWARD      0
#define FWD_REASON_TRAP         6

/* Every port this machine models, plus the CPU port. */
#define FWD_PORT_MASK                                                   \
    ((uint32_t)(((1u << RTL838X_SW_NUM_PORTS) - 1) << RTL838X_SW_PORT_FIRST) | \
     (1u << RTL838X_SW_CPU_PORT))

#define FWD_FRAME_MAX           10240

/* The shortest frame Ethernet allows on the wire, without its FCS. */
#define FWD_FRAME_MIN           60

/*
 * The longest frame a port takes, FCS included: two fourteen-bit fields, the
 * same value in both in everything seen so far -- 1526 from Linux, 10000 from
 * both vendor SDKs -- so the upper one stands for both.  Zero, before anyone
 * has set it, takes anything.
 */
#define FWD_MAC_MAX_LEN_CTRL    0xa9e0
#define FWD_MAC_MAX_LEN(v)      (((v) >> 14) & 0x3fff)

/* Set to 1 to trace forwarding decisions, including the drops. */
#define RTL838X_FWD_DEBUG 0

#define fwd_dbg(fmt, ...)                                           \
    do {                                                            \
        if (RTL838X_FWD_DEBUG) {                                    \
            qemu_log("rtl838x-fwd: " fmt, ## __VA_ARGS__);          \
        }                                                           \
    } while (0)

/* ------------------------------------------------- switch state, read back */

static uint32_t fwd_vlan_members(RTL838xSwitchState *s, uint16_t vid)
{
    uint32_t *row = rtl838x_switch_table_row(s, RTL838X_TBL_VLAN_WINDOW,
                                             RTL838X_TBL_VLAN_TYPE, vid);

    return row ? row[0] : 0;
}

static uint32_t fwd_vlan_untagged(RTL838xSwitchState *s, uint16_t vid)
{
    uint32_t *row = rtl838x_switch_table_row(s, RTL838X_TBL_UNTAG_WINDOW,
                                             RTL838X_TBL_UNTAG_TYPE, vid);

    return row ? row[0] : 0;
}

/*
 * The set of ports a flooded frame may reach, resolved through the multicast
 * port mask table.  An unprogrammed register means the driver has not got to
 * its L2 setup yet, at which point nothing is being filtered.
 */
static uint32_t fwd_flood_mask(RTL838xSwitchState *s, bool multicast)
{
    uint32_t fld = s->regs[FWD_L2_FLD_PMSK / 4];
    uint32_t *row;

    if (!fld) {
        return ~0u;
    }

    row = rtl838x_switch_table_row(s, RTL838X_TBL_MC_PMSK_WINDOW,
                                   RTL838X_TBL_MC_PMSK_TYPE,
                                   multicast ? FWD_L2_FLD_BC(fld)
                                             : FWD_L2_FLD_UC(fld));
    return row ? row[0] : 0;
}

/* The filtering database id a VLAN's addresses are learned under. */
static unsigned fwd_vlan_fid(RTL838xSwitchState *s, uint16_t vid)
{
    uint32_t *row = rtl838x_switch_table_row(s, RTL838X_TBL_VLAN_WINDOW,
                                             RTL838X_TBL_VLAN_TYPE, vid);

    return row ? (row[1] >> 5) & 0x3f : 0;
}

/*
 * Per-port spanning tree state.  Only the common instance is modelled: plain
 * STP and RSTP both live in MSTI 0, which is what the bridge drives, and a
 * multiple-spanning-tree setup would need the VLAN-to-instance mapping too.
 * The word index runs backwards, and the CPU port is past the end of it.
 */
static unsigned fwd_stp_state(RTL838xSwitchState *s, unsigned port)
{
    uint32_t *row = rtl838x_switch_table_row(s, RTL838X_TBL_MSTI_WINDOW,
                                             RTL838X_TBL_MSTI_TYPE, 0);

    if (port == RTL838X_SW_CPU_PORT || !row) {
        return STP_FORWARDING;
    }
    return (row[1 - port / 16] >> (2 * (port % 16))) & 3;
}

static bool fwd_port_outer(RTL838xSwitchState *s, unsigned port)
{
    return s->regs[FWD_VLAN_PORT_FWD / 4] & (1u << port);
}

static uint16_t fwd_port_pvid(RTL838xSwitchState *s, unsigned port)
{
    uint32_t pb = s->regs[FWD_VLAN_PORT_PB_VLAN(port) / 4];

    return fwd_port_outer(s, port) ? FWD_PB_VLAN_OUTER(pb) : FWD_PB_VLAN_INNER(pb);
}

/* Whether the port admits a frame that is VLAN-tagged, or one that is not. */
static bool fwd_port_admits(RTL838xSwitchState *s, unsigned port, bool vlan_tagged)
{
    uint32_t aft = s->regs[FWD_VLAN_PORT_AFT(port) / 4] >>
                   (fwd_port_outer(s, port) ? 2 : 0);

    return aft & (vlan_tagged ? FWD_AFT_TAGGED : FWD_AFT_UNTAGGED);
}

static unsigned fwd_port_igr_filter(RTL838xSwitchState *s, unsigned port)
{
    return (s->regs[FWD_VLAN_PORT_IGR_FLTR(port) / 4] >> (2 * (port % 16))) & 3;
}

/* --------------------------------------------------- forwarding database */

static uint64_t fwd_mac(const uint8_t *mac)
{
    uint64_t v = 0;

    for (unsigned i = 0; i < ETH_ALEN; i++) {
        v = (v << 8) | mac[i];
    }
    return v;
}

static void fwd_learn(RTL838xSwitchState *s, unsigned fid, const uint8_t *mac,
                      unsigned port)
{
    guint64 key = ((guint64)fid << 48) | fwd_mac(mac);
    gpointer val = GUINT_TO_POINTER(port + 1);

    if (g_hash_table_lookup(s->fdb, &key) == val) {
        return;
    }
    g_hash_table_replace(s->fdb, g_memdup2(&key, sizeof(key)), val);
}

/*
 * The port of a static unicast entry for mac in the hardware L2 table or its
 * CAM, or -1.  An entry is three words: the MAC is spread over the second
 * and third, and the first holds the port in [16:12], "static" in bit 19 and
 * the two IP multicast type bits in [22:21].  The VLAN is not compared: the
 * drivers install their own addresses once per VLAN, for the same port.
 */
static int fwd_lookup_static(RTL838xSwitchState *s, uint64_t mac)
{
    static const struct {
        unsigned type, rows;
    } tables[] = {
        { FWD_TBL_L2_TYPE, FWD_TBL_L2_ROWS },
        { FWD_TBL_L2_CAM_TYPE, FWD_TBL_L2_CAM_ROWS },
    };

    for (unsigned t = 0; t < ARRAY_SIZE(tables); t++) {
        for (uint32_t i = 0; i < tables[t].rows; i++) {
            uint32_t *r = rtl838x_switch_table_row(s, FWD_TBL_L2_WINDOW,
                                                   tables[t].type, i);
            uint64_t entry;

            if (!r || (r[0] & FWD_L2_IP_MC) || !(r[0] & FWD_L2_STATIC)) {
                continue;
            }
            entry = ((uint64_t)(r[1] & 0x0fffffff) << 20) | (r[2] >> 12);
            if (entry == mac) {
                return (r[0] >> 12) & 0x1f;
            }
        }
    }
    return -1;
}

/*
 * The port an address was last seen on or is statically bound to, or -1 if
 * neither.
 */
static int fwd_lookup(RTL838xSwitchState *s, unsigned fid, const uint8_t *mac)
{
    guint64 key = ((guint64)fid << 48) | fwd_mac(mac);
    gpointer val = g_hash_table_lookup(s->fdb, &key);

    if (val) {
        return (int)GPOINTER_TO_UINT(val) - 1;
    }
    return fwd_lookup_static(s, fwd_mac(mac));
}

static gboolean fwd_flush_port(gpointer key, gpointer value, gpointer user)
{
    return GPOINTER_TO_UINT(value) == GPOINTER_TO_UINT(user);
}

void rtl838x_fwd_flush(RTL838xSwitchState *s, int port)
{
    if (!s->fdb) {
        return;
    }
    if (port < 0) {
        g_hash_table_remove_all(s->fdb);
    } else {
        g_hash_table_foreach_remove(s->fdb, fwd_flush_port,
                                    GUINT_TO_POINTER(port + 1));
    }
}

/* --------------------------------------------------------- frame handling */

static bool fwd_is_multicast(const uint8_t *mac)
{
    return mac[0] & 1;
}

/*
 * Reserved multicast addresses -- 01:80:C2:00:00:00 through :0F, which is
 * where spanning tree's BPDUs live -- and EAPOL.  These go to the CPU and
 * nowhere else, whatever spanning tree thinks of the port they arrived on:
 * a bridge that never sees a BPDU never converges, and one whose BPDUs the
 * hardware also floods sees its own frames come back.
 */
static bool fwd_is_trapped(const uint8_t *buf, size_t len)
{
    static const uint8_t rma[5] = { 0x01, 0x80, 0xc2, 0x00, 0x00 };

    if (!memcmp(buf, rma, sizeof(rma)) && buf[5] <= 0x0f) {
        return true;
    }
    return len >= ETH_HLEN && lduw_be_p(buf + 12) == 0x888e;
}

static bool fwd_is_arp_request(const uint8_t *buf, size_t len, bool tagged)
{
    size_t l2 = ETH_HLEN + (tagged ? 4 : 0);

    return len >= l2 + 8 && lduw_be_p(buf + l2 - 2) == ETH_P_ARP &&
           lduw_be_p(buf + l2 + 6) == 1;
}

/*
 * The VLAN a frame belongs to, whether it arrived carrying a tag, and whether
 * that tag names a VLAN -- a priority tag does not, and counts as untagged
 * for the acceptable frame types.
 */
static uint16_t fwd_classify(RTL838xSwitchState *s, unsigned port,
                             const uint8_t *buf, size_t len, bool *tagged,
                             uint16_t *pcp, bool *vlan_tagged)
{
    uint16_t tci;

    *tagged = false;
    *vlan_tagged = false;
    *pcp = 0;

    if (len < ETH_HLEN + 4 || lduw_be_p(buf + 12) != ETH_P_VLAN) {
        return fwd_port_pvid(s, port);
    }

    tci = lduw_be_p(buf + 14);
    *tagged = true;
    *vlan_tagged = tci & 0xfff;
    *pcp = tci >> 13;

    /* A priority tag carries no VLAN of its own; the port's applies. */
    return (tci & 0xfff) ? (tci & 0xfff) : fwd_port_pvid(s, port);
}

static size_t fwd_untag(const uint8_t *in, size_t len, uint8_t *out)
{
    memcpy(out, in, 12);
    memcpy(out + 12, in + 16, len - 16);
    len -= 4;

    /* Taking four bytes out can put a minimum-sized frame under the limit. */
    if (len < FWD_FRAME_MIN) {
        memset(out + len, 0, FWD_FRAME_MIN - len);
        len = FWD_FRAME_MIN;
    }
    return len;
}

static size_t fwd_tag(const uint8_t *in, size_t len, uint16_t tci, uint8_t *out)
{
    memcpy(out, in, 12);
    stw_be_p(out + 12, ETH_P_VLAN);
    stw_be_p(out + 14, tci);
    memcpy(out + 16, in + 12, len - 12);

    return len + 4;
}

/*
 * Hands the frame to every port in the mask, in the tagging state that port's
 * membership asks for.  The other form of the frame is built at most once.
 */
static void fwd_egress(RTL838xSwitchState *s, uint32_t mask, unsigned src,
                       uint16_t vid, uint16_t pcp, bool tagged,
                       const uint8_t *buf, size_t len, unsigned reason)
{
    uint32_t untag = fwd_vlan_untagged(s, vid);
    g_autofree uint8_t *alt = NULL;
    size_t alt_len = 0;

    for (unsigned i = 0; i <= RTL838X_SW_NUM_PORTS; i++) {
        /* The front-panel ports, then the CPU port as the last round. */
        unsigned p = (i < RTL838X_SW_NUM_PORTS) ? RTL838X_SW_PORT_FIRST + i
                                                : RTL838X_SW_CPU_PORT;
        const uint8_t *frame = buf;
        size_t frame_len = len;

        if (!(mask & (1u << p))) {
            continue;
        }

        if (tagged != !(untag & (1u << p))) {
            if (!alt) {
                alt = g_malloc(MAX(len + 4, FWD_FRAME_MIN));
                alt_len = tagged ? fwd_untag(buf, len, alt)
                                 : fwd_tag(buf, len, (pcp << 13) | vid, alt);
            }
            frame = alt;
            frame_len = alt_len;
        }

        if (p == RTL838X_SW_CPU_PORT) {
            rtl838x_eth_to_cpu(s, src, reason, frame, frame_len);
        } else {
            rtl838x_port_send(s->port[i], frame, frame_len);
        }
    }
}

/*
 * Drops from the mask every port that cannot have the frame: the one it came
 * from, dark ports, ports spanning tree is not forwarding on, and ports the
 * ingress port is isolated from.  That last one matters more than it looks:
 * the driver leaves a standalone port's row as "the CPU port only", so before
 * a bridge exists this is what keeps ports from talking to each other.
 */
static uint32_t fwd_filter(RTL838xSwitchState *s, uint32_t mask, unsigned src)
{
    mask &= FWD_PORT_MASK;
    mask &= ~(1u << src);
    mask &= s->regs[FWD_PORT_ISO_CTRL(src) / 4];

    for (unsigned p = RTL838X_SW_PORT_FIRST; p <= RTL838X_SW_PORT_LAST; p++) {
        if (!(mask & (1u << p))) {
            continue;
        }
        if (!rtl838x_switch_link_up(s, p) ||
            fwd_stp_state(s, p) != STP_FORWARDING) {
            mask &= ~(1u << p);
        }
    }

    return mask;
}

/* The mask a frame should reach: one port for a known address, the VLAN's
 * members intersected with the flood mask for anything else. */
static uint32_t fwd_destination(RTL838xSwitchState *s, uint16_t vid,
                                unsigned fid, const uint8_t *dst)
{
    uint32_t members = fwd_vlan_members(s, vid);
    int port;

    /*
     * Group addresses flood; there is a per-group mask in the hardware that
     * IGMP snooping drives, which is not modelled, so they follow broadcast.
     */
    if (fwd_is_multicast(dst)) {
        return members & fwd_flood_mask(s, true);
    }

    port = fwd_lookup(s, fid, dst);
    if (port == RTL838X_SW_CPU_PORT) {
        /*
         * An address of the switch's own.  VLAN egress filtering is about
         * which wires a frame may leave on, and this one is not leaving.
         */
        return 1u << port;
    }
    if (port >= 0) {
        return members & (1u << port);
    }

    return members & fwd_flood_mask(s, false);
}

void rtl838x_fwd_ingress(RTL838xSwitchState *s, unsigned port,
                         const uint8_t *buf, size_t len)
{
    uint16_t vid, pcp;
    uint32_t members, mask;
    unsigned state, fid;
    bool tagged, vlan_tagged;

    uint32_t max = FWD_MAC_MAX_LEN(s->regs[FWD_MAC_MAX_LEN_CTRL / 4]);

    /*
     * What the MAC throws away before anything else looks at it: a runt,
     * shorter than a wire allows, and a frame longer than the port takes.
     * The backends carry no FCS, so it is counted in.
     */
    if (len < FWD_FRAME_MIN || len > FWD_FRAME_MAX ||
        (max && len + ETH_FCS_LEN > max)) {
        fwd_dbg("port %u: dropped by the MAC, %zu bytes\n", port, len);
        return;
    }

    /* Trapped frames bypass the whole pipeline, spanning tree included. */
    if (fwd_is_trapped(buf, len)) {
        fwd_dbg("port %u: trapped to CPU\n", port);
        rtl838x_eth_to_cpu(s, port, FWD_REASON_TRAP, buf, len);
        return;
    }

    state = fwd_stp_state(s, port);
    if (state < STP_LEARNING) {
        fwd_dbg("port %u: dropped, spanning tree state %u\n", port, state);
        return;
    }

    vid = fwd_classify(s, port, buf, len, &tagged, &pcp, &vlan_tagged);
    if (!fwd_port_admits(s, port, vlan_tagged)) {
        fwd_dbg("port %u: dropped, does not admit %s frames\n",
                port, vlan_tagged ? "tagged" : "untagged");
        return;
    }
    members = fwd_vlan_members(s, vid);
    if (!(members & (1u << port))) {
        switch (fwd_port_igr_filter(s, port)) {
        case FWD_IGR_FORWARD:
            break;      /* filtering off: it goes where its VLAN goes */
        case FWD_IGR_TRAP:
            rtl838x_eth_to_cpu(s, port, FWD_REASON_TRAP, buf, len);
            return;
        default:
            fwd_dbg("port %u: dropped, not a member of vlan %u (members 0x%08x)\n",
                    port, vid, members);
            return;
        }
    }

    fid = fwd_vlan_fid(s, vid);
    if (!fwd_is_multicast(buf + ETH_ALEN)) {
        fwd_learn(s, fid, buf + ETH_ALEN, port);
    }

    if (state != STP_FORWARDING) {
        return;     /* learning only */
    }

    mask = fwd_filter(s, fwd_destination(s, vid, fid, buf), port);
    if (fwd_is_arp_request(buf, len, tagged) &&
        (s->regs[FWD_SPCL_TRAP_ARP_CTRL / 4] & 1)) {
        mask |= 1u << RTL838X_SW_CPU_PORT;
    }
    fwd_dbg("port %u: vlan %u members 0x%08x iso 0x%08x -> mask 0x%08x\n",
            port, vid, members, s->regs[FWD_PORT_ISO_CTRL(port) / 4], mask);
    fwd_egress(s, mask, port, vid, pcp, tagged, buf, len, FWD_REASON_FORWARD);
}

void rtl838x_fwd_from_cpu(RTL838xSwitchState *s, const uint8_t *buf, size_t len,
                          uint32_t dpm, bool as_dpm)
{
    uint16_t vid, pcp;
    uint32_t mask;
    unsigned fid;
    bool tagged, vlan_tagged;

    if (len < ETH_HLEN || len > FWD_FRAME_MAX) {
        return;
    }

    if (as_dpm) {
        /*
         * The guest named the ports, which is how DSA reaches a specific one
         * and how the bridge gets a BPDU out of a port it is blocking.  The
         * frame is already in the form it wants, so it goes out untouched --
         * no lookup, no spanning tree, no tag rewriting.
         */
        for (unsigned p = RTL838X_SW_PORT_FIRST; p <= RTL838X_SW_PORT_LAST;
             p++) {
            if ((dpm & (1u << p)) && rtl838x_switch_link_up(s, p)) {
                rtl838x_port_send(s->port[p - RTL838X_SW_PORT_FIRST], buf, len);
            }
        }
        return;
    }

    /* Untagged conduit traffic: the switch treats it like any other ingress. */
    vid = fwd_classify(s, RTL838X_SW_CPU_PORT, buf, len, &tagged, &pcp,
                       &vlan_tagged);
    fid = fwd_vlan_fid(s, vid);
    if (!fwd_is_multicast(buf + ETH_ALEN)) {
        fwd_learn(s, fid, buf + ETH_ALEN, RTL838X_SW_CPU_PORT);
    }

    mask = fwd_filter(s, fwd_destination(s, vid, fid, buf),
                      RTL838X_SW_CPU_PORT);
    fwd_dbg("cpu port: vlan %u members 0x%08x iso 0x%08x -> mask 0x%08x\n",
            vid, fwd_vlan_members(s, vid),
            s->regs[FWD_PORT_ISO_CTRL(RTL838X_SW_CPU_PORT) / 4], mask);
    fwd_egress(s, mask, RTL838X_SW_CPU_PORT, vid, pcp, tagged, buf, len,
               FWD_REASON_FORWARD);
}

void rtl838x_fwd_realize(RTL838xSwitchState *s)
{
    s->fdb = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
}

void rtl838x_fwd_reset(RTL838xSwitchState *s)
{
    rtl838x_fwd_flush(s, -1);
}
