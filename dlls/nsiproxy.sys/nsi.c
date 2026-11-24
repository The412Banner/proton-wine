/*
 * nsiproxy.sys
 *
 * Copyright 2021 Huw Davies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#if 0
#pragma makedep unix
#endif

#include "config.h"
#include <stdarg.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <limits.h>
#ifdef HAVE_LINUX_RTNETLINK_H
#include <linux/rtnetlink.h>
#endif
#ifdef __APPLE__
#include <sys/ioctl.h>
#include <sys/kern_event.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/wdm.h"
#include "ifdef.h"
#define __WINE_INIT_NPI_MODULEID
#define USE_WS_PREFIX
#include "netiodef.h"
#include "wine/nsi.h"
#include "wine/debug.h"
#include "wine/unixlib.h"
#include "unix_private.h"
#include "nsiproxy_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(nsi);

static const struct module *modules[] =
{
    &ndis_module,
    &ipv4_module,
    &ipv6_module,
    &tcp_module,
    &udp_module,
};

static const struct module_table *get_module_table( const NPI_MODULEID *id, UINT table )
{
    const struct module_table *entry;
    int i;

    for (i = 0; i < ARRAY_SIZE(modules); i++)
        if (NmrIsEqualNpiModuleId( modules[i]->module, id ))
            for (entry = modules[i]->tables; entry->table != ~0u; entry++)
                if (entry->table == table) return entry;

    return NULL;
}

NTSTATUS nsi_enumerate_all_ex( struct nsi_enumerate_all_ex *params )
{
    const struct module_table *entry = get_module_table( params->module, params->table );
    UINT sizes[4] = { params->key_size, params->rw_size, params->dynamic_size, params->static_size };
    void *data[4] = { params->key_data, params->rw_data, params->dynamic_data, params->static_data };
    int i;

    if (!entry || !entry->enumerate_all)
    {
        WARN( "table not found\n" );
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; i < ARRAY_SIZE(sizes); i++)
    {
        if (!sizes[i]) data[i] = NULL;
        else if (sizes[i] != entry->sizes[i]) return STATUS_INVALID_PARAMETER;
    }

    return entry->enumerate_all( data[0], sizes[0], data[1], sizes[1], data[2], sizes[2], data[3], sizes[3], &params->count );
}

NTSTATUS nsi_get_all_parameters_ex( struct nsi_get_all_parameters_ex *params )
{
    const struct module_table *entry = get_module_table( params->module, params->table );
    void *rw = params->rw_data;
    void *dyn = params->dynamic_data;
    void *stat = params->static_data;

    if (!entry || !entry->get_all_parameters)
    {
        WARN( "table not found\n" );
        return STATUS_INVALID_PARAMETER;
    }

    if (params->key_size != entry->sizes[0]) return STATUS_INVALID_PARAMETER;
    if (!params->rw_size) rw = NULL;
    else if (params->rw_size != entry->sizes[1]) return STATUS_INVALID_PARAMETER;
    if (!params->dynamic_size) dyn = NULL;
    else if (params->dynamic_size != entry->sizes[2]) return STATUS_INVALID_PARAMETER;
    if (!params->static_size) stat = NULL;
    else if (params->static_size != entry->sizes[3]) return STATUS_INVALID_PARAMETER;

    return entry->get_all_parameters( params->key, params->key_size, rw, params->rw_size,
                                      dyn, params->dynamic_size, stat, params->static_size );
}

NTSTATUS nsi_get_parameter_ex( struct nsi_get_parameter_ex *params )
{
    const struct module_table *entry = get_module_table( params->module, params->table );

    if (!entry || !entry->get_parameter)
    {
        WARN( "table not found\n" );
        return STATUS_INVALID_PARAMETER;
    }

    if (params->param_type > 2) return STATUS_INVALID_PARAMETER;
    if (params->key_size != entry->sizes[0]) return STATUS_INVALID_PARAMETER;
    if (params->data_offset + params->data_size > entry->sizes[params->param_type + 1])
        return STATUS_INVALID_PARAMETER;
    return entry->get_parameter( params->key, params->key_size, params->param_type,
                                 params->data, params->data_size, params->data_offset );
}

static NTSTATUS unix_nsi_enumerate_all_ex( void *args )
{
    struct nsi_enumerate_all_ex *params = (struct nsi_enumerate_all_ex *)args;
    return nsi_enumerate_all_ex( params );
}

static NTSTATUS unix_nsi_get_all_parameters_ex( void *args )
{
    struct nsi_get_all_parameters_ex *params = (struct nsi_get_all_parameters_ex *)args;
    return nsi_get_all_parameters_ex( params );
}

static NTSTATUS unix_nsi_get_parameter_ex( void *args )
{
    struct nsi_get_parameter_ex *params = (struct nsi_get_parameter_ex *)args;
    return nsi_get_parameter_ex( params );
}

#if defined(HAVE_LINUX_RTNETLINK_H) || defined(__APPLE__)
static struct
{
    const NPI_MODULEID *module;
    UINT32 table;
}
queued_notifications[256];
static unsigned int queued_notification_count;

static NTSTATUS add_notification( const NPI_MODULEID *module, UINT32 table )
{
    unsigned int i;

    for (i = 0; i < queued_notification_count; ++i)
        if (queued_notifications[i].module == module && queued_notifications[i].table == table) return STATUS_SUCCESS;
    if (queued_notification_count == ARRAY_SIZE(queued_notifications))
    {
        ERR( "Notification queue full.\n" );
        return STATUS_NO_MEMORY;
    }
    queued_notifications[i].module = module;
    queued_notifications[i].table = table;
    ++queued_notification_count;
    return STATUS_SUCCESS;
}

#if defined(HAVE_LINUX_RTNETLINK_H)
static int netlink_fd = -1;

static NTSTATUS init_events(void)
{
    struct sockaddr_nl addr;

    if (netlink_fd != -1) return STATUS_SUCCESS;
    if ((netlink_fd = socket( PF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE )) == -1)
    {
        ERR( "netlink socket creation failed, errno %d.\n", errno );
        return STATUS_UNSUCCESSFUL;
    }

    memset( &addr, 0, sizeof(addr) );
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR |
                     RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
    if (bind( netlink_fd, (struct sockaddr *)&addr, sizeof(addr) ) == -1)
    {
        ERR( "bind failed, errno %d.\n", errno );
        close( netlink_fd );
        netlink_fd = -1;
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS invalidate_notifications(void)
{
    static const UINT tables[] = { NSI_IP_UNICAST_TABLE, NSI_IP_FORWARD_TABLE, NSI_IP_INTERFACE_TABLE };
    NTSTATUS status;
    unsigned int i;

    /* Lost events can affect any table, including ones absent from this datagram. */
    if ((status = add_notification( &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE ))) return status;
    for (i = 0; i < ARRAY_SIZE(tables); ++i)
    {
        if ((status = add_notification( &NPI_MS_IPV4_MODULEID, tables[i] ))) return status;
        if ((status = add_notification( &NPI_MS_IPV6_MODULEID, tables[i] ))) return status;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS process_netlink_events( const void *buffer, size_t len )
{
    const struct nlmsghdr *nlh = buffer;
    const NPI_MODULEID *module;
    NTSTATUS status;
    unsigned int family, table;
    size_t step;

    while (len)
    {
        if (len < sizeof(*nlh) || nlh->nlmsg_len < sizeof(*nlh) || nlh->nlmsg_len > len)
            return invalidate_notifications();

        switch (nlh->nlmsg_type)
        {
        case NLMSG_OVERRUN:
            return invalidate_notifications();
        case NLMSG_ERROR:
            if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr)) ||
                ((const struct nlmsgerr *)NLMSG_DATA(nlh))->error)
                return invalidate_notifications();
            break;
        case RTM_NEWLINK:
        case RTM_DELLINK:
            if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg)))
                return invalidate_notifications();
            if ((status = add_notification( &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE ))) return status;
            if ((status = add_notification( &NPI_MS_IPV4_MODULEID, NSI_IP_INTERFACE_TABLE ))) return status;
            if ((status = add_notification( &NPI_MS_IPV6_MODULEID, NSI_IP_INTERFACE_TABLE ))) return status;
            /* IPv4 route flushes on link down do not emit RTM_DELROUTE. */
            if ((status = add_notification( &NPI_MS_IPV4_MODULEID, NSI_IP_FORWARD_TABLE ))) return status;
            if ((status = add_notification( &NPI_MS_IPV6_MODULEID, NSI_IP_FORWARD_TABLE ))) return status;
            break;
        case RTM_NEWADDR:
        case RTM_DELADDR:
            if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifaddrmsg)))
                return invalidate_notifications();
            family = ((const struct ifaddrmsg *)NLMSG_DATA(nlh))->ifa_family;
            table = NSI_IP_UNICAST_TABLE;
            goto family_notification;
        case RTM_NEWROUTE:
        case RTM_DELROUTE:
            if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(struct rtmsg)))
                return invalidate_notifications();
            family = ((const struct rtmsg *)NLMSG_DATA(nlh))->rtm_family;
            table = NSI_IP_FORWARD_TABLE;
        family_notification:
            if (family == AF_INET) module = &NPI_MS_IPV4_MODULEID;
            else if (family == AF_INET6) module = &NPI_MS_IPV6_MODULEID;
            else break;
            if ((status = add_notification( module, table ))) return status;
            /* IP interface enumeration also depends on the assigned addresses. */
            if (table == NSI_IP_UNICAST_TABLE &&
                (status = add_notification( module, NSI_IP_INTERFACE_TABLE ))) return status;
            if (nlh->nlmsg_type == RTM_DELADDR &&
                (status = add_notification( module, NSI_IP_FORWARD_TABLE ))) return status;
            break;
        }

        if (nlh->nlmsg_len == len) break;
        step = NLMSG_ALIGN(nlh->nlmsg_len);
        if (step > len) return invalidate_notifications();
        len -= step;
        nlh = (const struct nlmsghdr *)((const char *)nlh + step);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS poll_events(void)
{
    union
    {
        struct nlmsghdr align;
        char data[32768];
    } buffer;
    NTSTATUS status;

    while (1)
    {
        struct sockaddr_nl addr;
        struct iovec iov = { buffer.data, sizeof(buffer.data) };
        struct msghdr msg = {0};
        ssize_t len;

        msg.msg_name = &addr;
        msg.msg_namelen = sizeof(addr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        len = recvmsg( netlink_fd, &msg, 0 );
        if (len < 0)
        {
            if (errno == EINTR) continue;
            if (errno == ENOBUFS) return invalidate_notifications();
            ERR( "error receiving netlink events, errno %d.\n", errno );
            close( netlink_fd );
            netlink_fd = -1;
            return STATUS_UNSUCCESSFUL;
        }
        if (msg.msg_namelen != sizeof(addr) || addr.nl_family != AF_NETLINK || addr.nl_pid) continue;
        if (!len || (msg.msg_flags & MSG_TRUNC)) return invalidate_notifications();
        if ((status = process_netlink_events( buffer.data, len ))) return status;
        if (queued_notification_count) return STATUS_SUCCESS;
    }
}
#elif defined(__APPLE__)
static int sock = -1;

static NTSTATUS init_events(void)
{
    static const struct kev_request req =
    {
        .vendor_code = KEV_VENDOR_APPLE,
        .kev_class = KEV_NETWORK_CLASS,
        .kev_subclass = KEV_ANY_SUBCLASS,
    };

    if (sock != -1) return STATUS_SUCCESS;
    if ((sock = socket( PF_SYSTEM, SOCK_RAW, SYSPROTO_EVENT )) == -1)
    {
        ERR( "PF_SYSTEM socket creation failed, errno %d.\n", errno );
        return STATUS_UNSUCCESSFUL;
    }

    if (fcntl( sock, F_SETFD, FD_CLOEXEC ) == -1 || ioctl( sock, SIOCSKEVFILT, &req ) == -1)
    {
        ERR( "kernel event socket setup failed, errno %d.\n", errno );
        close( sock );
        sock = -1;
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS poll_events(void)
{
    while (1)
    {
        struct kern_event_msg msg;
        NTSTATUS status;
        int len;

        len = recv( sock, &msg, sizeof(msg), 0 );
        if (len < sizeof(msg))
        {
            if (len < 0 && errno == EINTR) continue;
            ERR( "error receiving, len %d, errno %d.\n", len, errno );
            close( sock );
            sock = -1;
            return STATUS_UNSUCCESSFUL;
        }

        if (msg.kev_subclass == KEV_INET_SUBCLASS)
        {
            switch (msg.event_code)
            {
                case KEV_INET_NEW_ADDR:
                case KEV_INET_CHANGED_ADDR:
                case KEV_INET_ADDR_DELETED:
                    if ((status = add_notification( &NPI_MS_IPV4_MODULEID, NSI_IP_UNICAST_TABLE))) return status;
                    break;
            }
        }
        else if (msg.kev_subclass == KEV_INET6_SUBCLASS)
        {
            switch (msg.event_code)
            {
                case KEV_INET6_NEW_USER_ADDR:
                case KEV_INET6_CHANGED_ADDR:
                case KEV_INET6_ADDR_DELETED:
                case KEV_INET6_NEW_LL_ADDR:
                case KEV_INET6_NEW_RTADV_ADDR:
                    if ((status = add_notification( &NPI_MS_IPV6_MODULEID, NSI_IP_UNICAST_TABLE))) return status;
                    break;
            }
        }
        if (queued_notification_count) break;
    }

    return STATUS_SUCCESS;
}
#endif

static NTSTATUS unix_nsi_get_notification( void *args )
{
    struct nsi_get_notification_params *params = (struct nsi_get_notification_params *)args;
    NTSTATUS status;

    if (!queued_notification_count && (status = poll_events())) return status;
    assert( queued_notification_count );
    params->module = *queued_notifications[0].module;
    params->table = queued_notifications[0].table;
    --queued_notification_count;
    memmove( queued_notifications, queued_notifications + 1, sizeof(*queued_notifications) * queued_notification_count );
    return STATUS_SUCCESS;
}

static NTSTATUS unix_nsi_init_notifications( void *args )
{
    struct nsi_init_notifications_params *params = args;
    NTSTATUS status;

    params->supported = 0;
    if ((status = init_events())) return status;
    params->supported = NSI_NOTIFICATION_ADDRESS;
#ifdef HAVE_LINUX_RTNETLINK_H
    params->supported |= NSI_NOTIFICATION_ROUTE | NSI_NOTIFICATION_INTERFACE;
#endif
    return STATUS_SUCCESS;
}

static NTSTATUS unix_nsi_close_notifications( void *args )
{
#ifdef HAVE_LINUX_RTNETLINK_H
    if (netlink_fd != -1) close( netlink_fd );
    netlink_fd = -1;
#else
    if (sock != -1) close( sock );
    sock = -1;
#endif
    queued_notification_count = 0;
    return STATUS_SUCCESS;
}
#else
static NTSTATUS unix_nsi_get_notification( void *args )
{
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS unix_nsi_init_notifications( void *args )
{
    struct nsi_init_notifications_params *params = args;

    params->supported = 0;
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS unix_nsi_close_notifications( void *args )
{
    return STATUS_SUCCESS;
}
#endif

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    icmp_get_reply,
    icmp_listen,
    icmp_send_echo,
    unix_nsi_enumerate_all_ex,
    unix_nsi_get_all_parameters_ex,
    unix_nsi_get_parameter_ex,
    unix_nsi_get_notification,
    unix_nsi_init_notifications,
    unix_nsi_close_notifications,
};
