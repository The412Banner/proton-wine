/*
 * Network connectivity state
 *
 * Copyright 2026 Erhan Bilgili
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

#include <stdlib.h>
#include "winsock2.h"
#include "ws2ipdef.h"
#include "windef.h"
#include "winbase.h"
#include "iphlpapi.h"
#include "netiodef.h"
#include "wininet.h"
#include "wine/nsi.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wininet);

static SRWLOCK connectivity_lock = SRWLOCK_INIT;
static BOOL initialized, monitoring, valid;
static DWORD connected_state;

static struct connectivity_notification
{
    const NPI_MODULEID *module;
    DWORD table;
    HANDLE device;
    OVERLAPPED overlapped;
    BOOL pending;
} notifications[] =
{
    {&NPI_MS_IPV4_MODULEID, NSI_IP_UNICAST_TABLE},
    {&NPI_MS_IPV6_MODULEID, NSI_IP_UNICAST_TABLE},
    {&NPI_MS_IPV4_MODULEID, NSI_IP_FORWARD_TABLE},
    {&NPI_MS_IPV6_MODULEID, NSI_IP_FORWARD_TABLE},
    {&NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE},
};

static void stop_notifications(void)
{
    unsigned int i;
    DWORD bytes;

    for (i = 0; i < ARRAY_SIZE(notifications); ++i)
    {
        struct connectivity_notification *notification = &notifications[i];

        if (notification->pending)
        {
            NsiCancelChangeNotification(&notification->overlapped);
            /* Cancellation can return before the IOSB and event are updated. */
            WaitForSingleObject(notification->overlapped.hEvent, INFINITE);
            GetOverlappedResult(notification->device, &notification->overlapped, &bytes, FALSE);
            notification->pending = FALSE;
        }
        if (notification->overlapped.hEvent) CloseHandle(notification->overlapped.hEvent);
        notification->overlapped.hEvent = NULL;
        /* The notification device handle belongs to NSI and is shared. */
    }
    monitoring = valid = FALSE;
}

static BOOL arm_notification(struct connectivity_notification *notification)
{
    DWORD err;

    ResetEvent(notification->overlapped.hEvent);
    err = NsiRequestChangeNotification(0, notification->module, notification->table,
                                     &notification->overlapped, &notification->device);
    notification->pending = !err || err == ERROR_IO_PENDING;
    return notification->pending;
}

static void start_notifications(void)
{
    unsigned int i;

    initialized = TRUE;
    for (i = 0; i < ARRAY_SIZE(notifications); ++i)
    {
        struct connectivity_notification *notification = &notifications[i];

        if (!(notification->overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL)) ||
            !arm_notification(notification))
        {
            WARN("Network change monitoring unavailable, querying connectivity directly.\n");
            stop_notifications();
            return;
        }
    }
    monitoring = TRUE;
}

static void check_notifications(void)
{
    unsigned int i;
    DWORD bytes, err;

    for (i = 0; i < ARRAY_SIZE(notifications); ++i)
    {
        struct connectivity_notification *notification = &notifications[i];

        /* No server calls on the unchanged fast path. */
        if (!HasOverlappedIoCompleted(&notification->overlapped)) continue;
        /* IOSB publication precedes the event signal. Drain that signal before
         * resetting the event and reusing the OVERLAPPED for another request. */
        WaitForSingleObject(notification->overlapped.hEvent, INFINITE);
        valid = FALSE;
        notification->pending = FALSE;
        err = GetOverlappedResult(notification->device, &notification->overlapped, &bytes, FALSE) ?
              ERROR_SUCCESS : GetLastError();
        /* The thread which issued the request may have exited. Rearming also
         * covers any network changes while that request was cancelled. */
        if ((err && err != ERROR_OPERATION_ABORTED) || !arm_notification(notification))
        {
            WARN("Network change monitoring failed, querying connectivity directly.\n");
            stop_notifications();
            return;
        }
    }
}

static DWORD query_connected_state(DWORD *status)
{
    IP_ADAPTER_ADDRESSES *buf, *aa;
    ULONG size = 15000;
    DWORD err;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
                        GAA_FLAG_SKIP_FRIENDLY_NAME | GAA_FLAG_INCLUDE_GATEWAYS;

    /* Avoid enumerating all adapters just to determine the buffer size. */
    if (!(buf = malloc(size))) return ERROR_NOT_ENOUGH_MEMORY;
    while ((err = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, buf, &size)))
    {
        free(buf);
        if (err != ERROR_BUFFER_OVERFLOW)
        {
            if (err != ERROR_NO_DATA) return err;
            buf = NULL;
            break;
        }
        if (!(buf = malloc(size))) return ERROR_NOT_ENOUGH_MEMORY;
    }

    *status = INTERNET_RAS_INSTALLED;
    for (aa = buf; aa; aa = aa->Next)
    {
        if (aa->FirstUnicastAddress) *status |= INTERNET_CONNECTION_OFFLINE;
        if (aa->FirstGatewayAddress)
        {
            *status &= ~INTERNET_CONNECTION_OFFLINE;
            *status |= INTERNET_CONNECTION_LAN;
            break;
        }
    }
    free(buf);
    return ERROR_SUCCESS;
}

DWORD get_connected_state(DWORD *status)
{
    DWORD err = ERROR_SUCCESS;

    AcquireSRWLockExclusive(&connectivity_lock);
    if (!initialized) start_notifications();
    if (monitoring) check_notifications();
    if (!monitoring)
    {
        ReleaseSRWLockExclusive(&connectivity_lock);
        return query_connected_state(status);
    }
    if (!valid)
    {
        /* All watches are armed before the snapshot. Changes during the query
         * leave a completed watch and invalidate this snapshot on the next call. */
        err = query_connected_state(&connected_state);
        valid = !err && monitoring;
    }
    if (!err) *status = connected_state;
    ReleaseSRWLockExclusive(&connectivity_lock);
    return err;
}

void free_connected_state(void)
{
    /* No callback or worker executing Wininet code can race DLL unloading. */
    stop_notifications();
}
