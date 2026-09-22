/*
 * Realtek RTL838x front-panel port
 *
 * One of these per port 8..15, the ports the device tree labels lan1..lan8.
 * Each holds a QEMU NIC, so a port is an ordinary network device on the host
 * side: it takes any backend, shows up in "info network" and responds to
 * "set_link".  The board creates all eight and lets each claim the next unused
 * -nic, which binds backends to lan1, lan2, ... in command-line order.
 *
 * A port with no backend reports no carrier, which is the honest answer -- it
 * is a socket with no cable in it -- and is what the guest's phylink sees
 * through the PHY's BMSR.
 *
 * There is deliberately no MAC filtering here.  Switch ports are not addressed;
 * the "mac" property exists only because QEMU NICs have one, and every frame
 * the backend offers goes straight into the forwarding engine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/mips/rtl838x.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qom/object.h"

struct RTL838xPortState {
    DeviceState parent_obj;

    NICState *nic;
    NICConf conf;

    uint8_t port;               /* hardware port number, 8..15 */
    RTL838xSwitchState *sw;
};

OBJECT_DECLARE_SIMPLE_TYPE(RTL838xPortState, RTL838X_PORT)

bool rtl838x_port_link_up(RTL838xPortState *p)
{
    NetClientState *nc = qemu_get_queue(p->nic);

    /*
     * No peer means no -nic claimed this port.  QEMU leaves link_down clear on
     * such a NIC, so asking only that would report every unused port as a live
     * 1 Gbps link.
     */
    return nc->peer && !nc->link_down;
}

void rtl838x_port_send(RTL838xPortState *p, const uint8_t *buf, size_t len)
{
    qemu_send_packet(qemu_get_queue(p->nic), buf, len);
}

static ssize_t rtl838x_port_receive(NetClientState *nc, const uint8_t *buf,
                                    size_t size)
{
    RTL838xPortState *p = qemu_get_nic_opaque(nc);

    rtl838x_fwd_ingress(p->sw, p->port, buf, size);

    /*
     * Always claim the frame.  Returning short here would throttle the backend
     * on our behalf, and a switch that cannot place a frame drops it rather
     * than pushing back on the wire.
     */
    return size;
}

static void rtl838x_port_set_link(NetClientState *nc)
{
    RTL838xPortState *p = qemu_get_nic_opaque(nc);

    rtl838x_switch_link_changed(p->sw, p->port);
}

static NetClientInfo net_rtl838x_port_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = rtl838x_port_receive,
    .link_status_changed = rtl838x_port_set_link,
};

static void rtl838x_port_realize(DeviceState *dev, Error **errp)
{
    RTL838xPortState *p = RTL838X_PORT(dev);
    g_autofree char *name = NULL;

    if (p->port < RTL838X_SW_PORT_FIRST || p->port > RTL838X_SW_PORT_LAST) {
        error_setg(errp, "port must be between %u and %u",
                   RTL838X_SW_PORT_FIRST, RTL838X_SW_PORT_LAST);
        return;
    }
    if (!p->sw) {
        error_setg(errp, "the 'switch' link is not set");
        return;
    }

    /* Named for the label the device tree gives the port, so that the monitor
     * talks about the same lanN the guest does. */
    name = g_strdup_printf("lan%u", p->port - RTL838X_SW_PORT_FIRST + 1);

    qemu_macaddr_default_if_unset(&p->conf.macaddr);
    p->nic = qemu_new_nic(&net_rtl838x_port_info, &p->conf, TYPE_RTL838X_PORT,
                          name, &dev->mem_reentrancy_guard, p);
    qemu_format_nic_info_str(qemu_get_queue(p->nic), p->conf.macaddr.a);

    rtl838x_switch_attach_port(p->sw, p->port, p);
}

static const Property rtl838x_port_properties[] = {
    DEFINE_PROP_UINT8("port", RTL838xPortState, port, 0),
    DEFINE_PROP_LINK("switch", RTL838xPortState, sw, TYPE_RTL838X_SWITCH,
                     RTL838xSwitchState *),
    DEFINE_NIC_PROPERTIES(RTL838xPortState, conf),
};

static void rtl838x_port_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rtl838x_port_realize;
    device_class_set_props(dc, rtl838x_port_properties);
    /* Not user-creatable: the board makes exactly the eight the SoC has. */
    dc->user_creatable = false;
}

static const TypeInfo rtl838x_port_types[] = {
    {
        .name          = TYPE_RTL838X_PORT,
        .parent        = TYPE_DEVICE,
        .instance_size = sizeof(RTL838xPortState),
        .class_init    = rtl838x_port_class_init,
    },
};

DEFINE_TYPES(rtl838x_port_types)
