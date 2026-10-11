/*
 * Bluetooth bus driver
 *
 * Copyright 2024-2026 Vibhav Pant
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

#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winternl.h>
#include <winnls.h>
#include <wtypes.h>
#include <initguid.h>
#include <devpkey.h>
#include <propkey.h>
#include <bthsdpdef.h>
#include <bluetoothapis.h>
#include <bthdef.h>
#include <bthledef.h>
#include <winioctl.h>
#include <bthioctl.h>
#include <ddk/wdm.h>
#include <ddk/bthguid.h>

#include <wine/winebth.h>
#include <wine/debug.h>
#include <wine/list.h>

#include "winebth_priv.h"

WINE_DEFAULT_DEBUG_CHANNEL( winebth );

static DRIVER_OBJECT *driver_obj;

static DEVICE_OBJECT *bus_fdo, *bus_pdo, *device_auth;

#define DECLARE_CRITICAL_SECTION( cs )                                                             \
    static CRITICAL_SECTION cs;                                                                    \
    static CRITICAL_SECTION_DEBUG cs##_debug = {                                                   \
        0,                                                                                         \
        0,                                                                                         \
        &( cs ),                                                                                   \
        { &cs##_debug.ProcessLocksList, &cs##_debug.ProcessLocksList },                            \
        0,                                                                                         \
        0,                                                                                         \
        { (DWORD_PTR)( __FILE__ ": " #cs ) } };                                                    \
    static CRITICAL_SECTION cs = { &cs##_debug, -1, 0, 0, 0, 0 };

DECLARE_CRITICAL_SECTION( device_list_cs );
/* Discovery calls wait for the event loop to dispatch a BlueZ reply. They must not hold
 * device_list_cs, which that same loop needs when processing device property changes. */
DECLARE_CRITICAL_SECTION( discovery_cs );

static struct list device_list = LIST_INIT( device_list );

struct bluetooth_radio
{
    struct list entry;
    BOOL removed;

    DEVICE_OBJECT *device_obj;
    winebluetooth_radio_props_mask_t props_mask; /* Guarded by device_list_cs */
    struct winebluetooth_radio_properties props; /* Guarded by device_list_cs */
    BOOL started; /* Guarded by device_list_cs */
    winebluetooth_radio_t radio;
    WCHAR *hw_name;
    UNICODE_STRING bthport_symlink_name;
    UNICODE_STRING bthradio_symlink_name;

    struct list remote_devices; /* Guarded by device_list_cs */

    /* Guarded by device_list_cs */
    LIST_ENTRY irp_list;
    LONG le_discovery_refs; /* Callers that have LE discovery running. Guarded by discovery_cs */
};

struct bluetooth_remote_device
{
    struct list entry;

    DEVICE_OBJECT *device_obj;
    struct bluetooth_radio *radio; /* The radio associated with this remote device. */
    winebluetooth_device_t device;
    CRITICAL_SECTION props_cs;
    winebluetooth_device_props_mask_t props_mask; /* Guarded by props_cs */
    struct winebluetooth_device_properties props; /* Guarded by props_cs */
    BOOL started; /* Whether the device has been started. Guarded by props_cs */
    BOOL removed;

    BOOL le; /* Guarded by props_cs */
    UNICODE_STRING bthle_symlink_name; /* Guarded by props_cs */
    struct list gatt_services; /* Guarded by props_cs */
    LIST_ENTRY gatt_irp_list; /* GATT service requests waiting for a connection. Guarded by props_cs */
    BOOL connecting; /* A BlueZ Connect call is in flight. Guarded by props_cs */
    unsigned int connect_attempts; /* Guarded by props_cs */
    LONG open_handles; /* Handles on this device and its GATT services. Guarded by props_cs */
    ULONGLONG last_adv_report; /* Tick count of the last advertisement event. Guarded by props_cs */
};

struct bluetooth_gatt_service
{
    struct list entry;
    BOOL removed;

    DEVICE_OBJECT *device_obj;
    struct bluetooth_remote_device *remote_device; /* The remote device this service exists on. */
    winebluetooth_gatt_service_t service;
    GUID uuid;
    unsigned int primary : 1;
    UINT16 handle;
    UNICODE_STRING service_symlink_name;

    CRITICAL_SECTION chars_cs;
    struct list characteristics; /* Guarded by chars_cs */

    LIST_ENTRY irp_list; /* Guarded by chars_cs */
};

struct bluetooth_gatt_characteristic
{
    struct list entry;

    winebluetooth_gatt_characteristic_t characteristic;
    BTH_LE_GATT_CHARACTERISTIC props;
    BTH_LE_GATT_CHARACTERISTIC_VALUE *value;
    BOOL notifying; /* Whether a BlueZ notify session was requested. Guarded by chars_cs */
};

enum bluetooth_pdo_ext_type
{
    BLUETOOTH_PDO_EXT_RADIO,
    BLUETOOTH_PDO_EXT_REMOTE_DEVICE,
    BLUETOOTH_PDO_EXT_GATT_SERVICE,
};

struct bluetooth_pdo_ext
{
    enum bluetooth_pdo_ext_type type;
    union {
        struct bluetooth_radio radio;
        struct bluetooth_remote_device remote_device;
        struct bluetooth_gatt_service gatt_service;
    };
};

static NTSTATUS WINAPI dispatch_auth( DEVICE_OBJECT *device, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS status = irp->IoStatus.Status;

    TRACE( "device %p irp %p code %#lx\n", device, irp, code );

    switch (code)
    {
    case IOCTL_WINEBTH_AUTH_REGISTER:
        status = winebluetooth_auth_agent_enable_incoming();
        break;
    default:
        break;
    }

    irp->IoStatus.Status = status;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return status;
}

static void le_to_uuid( const BTH_LE_UUID *le_uuid, GUID *uuid )
{
    if (le_uuid->IsShortUuid)
    {
        *uuid = BTH_LE_ATT_BLUETOOTH_BASE_GUID;
        uuid->Data1 = le_uuid->Value.ShortUuid;
    }
    else
        *uuid = le_uuid->Value.LongUuid;
}

/* Caller should hold props_cs */
static struct bluetooth_gatt_service *find_gatt_service( struct list *services, const GUID *uuid, UINT16 handle )
{
    struct bluetooth_gatt_service *service;
    LIST_FOR_EACH_ENTRY( service, services, struct bluetooth_gatt_service, entry )
    {
        if (IsEqualGUID( &service->uuid, uuid ) && service->handle == handle)
            return service;
    }
    return NULL;
}

/* Called should hold chars_cs */
static struct bluetooth_gatt_characteristic *find_gatt_characteristic( struct list *chars, const BTH_LE_UUID *uuid,
                                                                       UINT16 handle )
{
    struct bluetooth_gatt_characteristic *chrc;

    LIST_FOR_EACH_ENTRY( chrc, chars, struct bluetooth_gatt_characteristic, entry )
    {
        if (IsBthLEUuidMatch( chrc->props.CharacteristicUuid, *uuid ) && chrc->props.AttributeHandle == handle)
            return chrc;
    }
    return NULL;
}

static NTSTATUS bluetooth_gatt_service_get_characteristics( struct bluetooth_gatt_service *service, IRP *irp )
{
    const SIZE_T min_size = offsetof( struct winebth_le_device_get_gatt_characteristics_params, characteristics[0] );
    struct winebth_le_device_get_gatt_characteristics_params *chars = irp->AssociatedIrp.SystemBuffer;
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    ULONG outsize = stack->Parameters.DeviceIoControl.OutputBufferLength;
    struct bluetooth_gatt_characteristic *chrc;
    NTSTATUS status;
    SIZE_T rem;

    if (outsize < min_size)
        return STATUS_INVALID_USER_BUFFER;

    rem = (outsize - min_size)/sizeof( *chars->characteristics );
    status = STATUS_SUCCESS;
    chars->count = 0;

    EnterCriticalSection( &service->chars_cs );
    LIST_FOR_EACH_ENTRY( chrc, &service->characteristics, struct bluetooth_gatt_characteristic, entry )
    {
        chars->count++;
        if (rem > 0)
        {
            chars->characteristics[chars->count - 1] = chrc->props;
            rem--;
        }
    }
    LeaveCriticalSection( &service->chars_cs );

    irp->IoStatus.Information = offsetof( struct winebth_le_device_get_gatt_characteristics_params, characteristics[chars->count] );
    if (chars->count > rem)
        status = STATUS_MORE_ENTRIES;
    return status;
}

static NTSTATUS bluetooth_gatt_service_dispatch( DEVICE_OBJECT *device, struct bluetooth_gatt_service *ext, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    ULONG outsize = stack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS status = irp->IoStatus.Status;

    TRACE( "device=%p, ext=%p, irp=%p, code=%#lx\n", device, ext, irp, code );
    switch (code)
    {
    case IOCTL_WINEBTH_LE_DEVICE_GET_GATT_CHARACTERISTICS:
    {
        struct winebth_le_device_get_gatt_characteristics_params *params = irp->AssociatedIrp.SystemBuffer;

        if (!params)
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        status = bluetooth_gatt_service_get_characteristics( ext, irp );
        break;
    }
    case IOCTL_WINEBTH_GATT_SERVICE_READ_CHARACTERISITIC_VALUE:
    {
        struct winebth_gatt_service_read_characterisitic_value_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_gatt_characteristic *chrc;

        if (!params || outsize < sizeof( *params ))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        EnterCriticalSection( &ext->chars_cs );
        chrc = find_gatt_characteristic( &ext->characteristics, &params->uuid, params->handle );
        if (!chrc)
        {
            status = STATUS_NOT_FOUND;
            LeaveCriticalSection( &ext->chars_cs );
            break;
        }
        if (!chrc->props.IsReadable)
        {
            status = STATUS_PRIVILEGE_NOT_HELD;
            LeaveCriticalSection( &ext->chars_cs );
            break;
        }
        if (params->from_device || !chrc->value)
        {
            status = winebluetooth_gatt_characteristic_read_async( chrc->characteristic, irp );
            if (status == STATUS_PENDING)
            {
                IoMarkIrpPending( irp );
                InsertTailList( &ext->irp_list, &irp->Tail.Overlay.ListEntry );
            }
        }
        else
        {
            ULONG needed = offsetof( struct winebth_gatt_service_read_characterisitic_value_params, buf[chrc->value->DataSize] );

            params->size = chrc->value->DataSize;
            if (outsize >= needed)
            {
                status = STATUS_SUCCESS;
                memcpy( params->buf, chrc->value->Data, params->size );
                irp->IoStatus.Information = needed;
            }
            else
            {
                status = STATUS_MORE_ENTRIES;
                irp->IoStatus.Information = sizeof( *params );
            }
        }
        LeaveCriticalSection( &ext->chars_cs );
        break;
    }
    case IOCTL_WINEBTH_GATT_SERVICE_WRITE_CHARACTERISTIC_VALUE:
    {
        struct winebth_gatt_service_write_characteristic_value_params *params = irp->AssociatedIrp.SystemBuffer;
        ULONG insize = stack->Parameters.DeviceIoControl.InputBufferLength;
        struct bluetooth_gatt_characteristic *chrc;

        if (!params || insize < sizeof( *params ) ||
            insize < offsetof( struct winebth_gatt_service_write_characteristic_value_params, buf[params->size] ))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }
        EnterCriticalSection( &ext->chars_cs );
        chrc = find_gatt_characteristic( &ext->characteristics, &params->uuid, params->handle );
        if (!chrc)
            status = STATUS_NOT_FOUND;
        else if (!chrc->props.IsWritable && !chrc->props.IsWritableWithoutResponse)
            status = STATUS_PRIVILEGE_NOT_HELD;
        else
        {
            status = winebluetooth_gatt_characteristic_write_async( chrc->characteristic, irp, params->buf, params->size,
                                                                    !!params->without_response );
            if (status == STATUS_PENDING)
            {
                IoMarkIrpPending( irp );
                InsertTailList( &ext->irp_list, &irp->Tail.Overlay.ListEntry );
            }
        }
        LeaveCriticalSection( &ext->chars_cs );
        break;
    }
    case IOCTL_WINEBTH_GATT_SERVICE_SET_CHARACTERISTIC_NOTIFY:
    {
        struct winebth_gatt_service_set_characteristic_notify_params *params = irp->AssociatedIrp.SystemBuffer;
        ULONG insize = stack->Parameters.DeviceIoControl.InputBufferLength;
        struct bluetooth_gatt_characteristic *chrc;

        if (!params || insize < sizeof( *params ))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }
        EnterCriticalSection( &ext->chars_cs );
        chrc = find_gatt_characteristic( &ext->characteristics, &params->uuid, params->handle );
        if (!chrc)
            status = STATUS_NOT_FOUND;
        else if (!chrc->props.IsNotifiable && !chrc->props.IsIndicatable)
            status = STATUS_PRIVILEGE_NOT_HELD;
        else if (!!params->enable == chrc->notifying)
            /* BlueZ rejects stopping a session that was never started, Windows does not. */
            status = STATUS_SUCCESS;
        else
        {
            status = winebluetooth_gatt_characteristic_set_notify_async( chrc->characteristic, irp, !!params->enable );
            if (status == STATUS_PENDING)
            {
                chrc->notifying = !!params->enable;
                IoMarkIrpPending( irp );
                InsertTailList( &ext->irp_list, &irp->Tail.Overlay.ListEntry );
            }
        }
        LeaveCriticalSection( &ext->chars_cs );
        break;
    }
    default:
        FIXME( "Unimplemented IOCTL code: %#lx\n", code );
    }
    if (status != STATUS_PENDING)
    {
        irp->IoStatus.Status = status;
        IoCompleteRequest( irp, IO_NO_INCREMENT );
    }
    return status;
}

/* Caller must hold ext->props_cs. */
static NTSTATUS bluetooth_device_fill_gatt_services( struct bluetooth_remote_device *ext, IRP *irp )
{
    const SIZE_T min_size = offsetof( struct winebth_le_device_get_gatt_services_params, services[0] );
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    ULONG outsize = stack->Parameters.DeviceIoControl.OutputBufferLength;
    struct winebth_le_device_get_gatt_services_params *services = irp->AssociatedIrp.SystemBuffer;
    struct bluetooth_gatt_service *svc;
    NTSTATUS status = STATUS_SUCCESS;
    SIZE_T capacity;

    capacity = (outsize - min_size)/sizeof( *services->services );
    services->count = 0;
    LIST_FOR_EACH_ENTRY( svc, &ext->gatt_services, struct bluetooth_gatt_service, entry )
    {
        if (!svc->primary)
            continue;
        services->count++;
        if (services->count <= capacity)
        {
            BTH_LE_GATT_SERVICE *info;

            info = &services->services[services->count - 1];
            memset( info, 0, sizeof( *info ) );
            uuid_to_le( &svc->uuid, &info->ServiceUuid );
            info->AttributeHandle = svc->handle;
        }
    }
    irp->IoStatus.Information = offsetof( struct winebth_le_device_get_gatt_services_params, services[services->count] );
    if (services->count > capacity)
        status = STATUS_MORE_ENTRIES;
    return status;
}

/* Complete every pending GATT service request for the device. Caller must hold ext->props_cs. */
static void bluetooth_device_complete_gatt_irps( struct bluetooth_remote_device *ext, NTSTATUS status )
{
    while (!IsListEmpty( &ext->gatt_irp_list ))
    {
        LIST_ENTRY *entry = RemoveHeadList( &ext->gatt_irp_list );
        IRP *irp = CONTAINING_RECORD( entry, IRP, Tail.Overlay.ListEntry );
        irp->IoStatus.Status = status ? status : bluetooth_device_fill_gatt_services( ext, irp );
        IoCompleteRequest( irp, IO_NO_INCREMENT );
    }
}

/* Whether the device has been seen advertising over LE. */
static BOOL bluetooth_device_is_le( winebluetooth_device_props_mask_t mask, const struct winebluetooth_device_properties *props )
{
    if (mask & (WINEBLUETOOTH_DEVICE_PROPERTY_RSSI | WINEBLUETOOTH_DEVICE_PROPERTY_MANUFACTURER_DATA |
                WINEBLUETOOTH_DEVICE_PROPERTY_SERVICE_DATA | WINEBLUETOOTH_DEVICE_PROPERTY_APPEARANCE |
                WINEBLUETOOTH_DEVICE_PROPERTY_TX_POWER))
        return TRUE;
    return mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS_TYPE && props->le.flags & WINEBTH_LE_ADV_FLAG_RANDOM_ADDRESS;
}

static void bluetooth_device_enable_le_iface( struct bluetooth_remote_device *device );

static NTSTATUS bluetooth_remote_device_dispatch( DEVICE_OBJECT *device, struct bluetooth_remote_device *ext, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    ULONG outsize = stack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS status = irp->IoStatus.Status;

    TRACE( "device=%p, ext=%p, irp=%p, code=%#lx\n", device, ext, irp, code );

    switch (code)
    {
    case IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES:
    {
        const SIZE_T min_size = offsetof( struct winebth_le_device_get_gatt_services_params, services[0] );

        if (!irp->AssociatedIrp.SystemBuffer || outsize < min_size)
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }
        EnterCriticalSection( &ext->props_cs );
        if (ext->props.connected && ext->props.services_resolved)
            status = bluetooth_device_fill_gatt_services( ext, irp );
        else
        {
            /* Windows connects on demand when services are requested. Keep the request pending until BlueZ
             * has resolved the device's services. */
            status = STATUS_PENDING;
            if (!ext->props.connected && !ext->connecting)
            {
                winebluetooth_device_dup( ext->device );
                status = winebluetooth_device_connect( ext->device, irp );
                ext->connecting = status == STATUS_PENDING;
                ext->connect_attempts = 1;
                if (status != STATUS_PENDING) winebluetooth_device_free( ext->device );
            }
            if (status == STATUS_PENDING)
            {
                IoMarkIrpPending( irp );
                InsertTailList( &ext->gatt_irp_list, &irp->Tail.Overlay.ListEntry );
            }
        }
        LeaveCriticalSection( &ext->props_cs );
        break;
    }
    case IOCTL_WINEBTH_LE_DEVICE_GET_GATT_CHARACTERISTICS:
    {
        const SIZE_T min_size = offsetof( struct winebth_le_device_get_gatt_characteristics_params, characteristics[0] );
        struct winebth_le_device_get_gatt_characteristics_params *chars = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_gatt_service *service;
        GUID uuid;

        if (!chars || outsize < min_size)
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        le_to_uuid( &chars->service.ServiceUuid, &uuid );
        EnterCriticalSection( &ext->props_cs );
        service = find_gatt_service( &ext->gatt_services, &uuid, chars->service.AttributeHandle );
        if (!service)
        {
            status = STATUS_INVALID_PARAMETER;
            LeaveCriticalSection( &ext->props_cs );
            break;
        }

        status = bluetooth_gatt_service_get_characteristics( service, irp );
        LeaveCriticalSection( &ext->props_cs );
        break;
    }
    default:
        FIXME( "Unimplemented IOCTL code: %#lx\n", code );
    }

    if (status != STATUS_PENDING)
    {
        irp->IoStatus.Status = status;
        IoCompleteRequest( irp, IO_NO_INCREMENT );
    }
    return status;
}

static void bluetooth_device_fill_le_advertisement( struct bluetooth_remote_device *device,
                                                    struct winebth_le_advertisement *adv );

static BOOL bluetooth_radio_check_power( struct bluetooth_radio *radio );

static NTSTATUS bluetooth_radio_dispatch( DEVICE_OBJECT *device, struct bluetooth_radio *ext, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG insize = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outsize = stack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status = irp->IoStatus.Status;

    TRACE( "device=%p, ext=%p, irp=%p code=%#lx\n", device, ext, irp, code );

    switch (code)
    {
    case IOCTL_BTH_GET_LOCAL_INFO:
    {
        BTH_LOCAL_RADIO_INFO *info = (BTH_LOCAL_RADIO_INFO *)irp->AssociatedIrp.SystemBuffer;

        if (!info || outsize < sizeof(*info))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        memset( info, 0, sizeof( *info ) );

        EnterCriticalSection( &device_list_cs );
        if (ext->props_mask & WINEBLUETOOTH_RADIO_PROPERTY_ADDRESS)
        {
            info->localInfo.flags |= BDIF_ADDRESS;
            info->localInfo.address = RtlUlonglongByteSwap( ext->props.address.ullLong ) >> 16;
        }
        if (ext->props_mask & WINEBLUETOOTH_RADIO_PROPERTY_NAME)
        {
            info->localInfo.flags |= BDIF_NAME;
            strcpy( info->localInfo.name, ext->props.name );
        }
        if (ext->props_mask & WINEBLUETOOTH_RADIO_PROPERTY_CLASS)
        {
            info->localInfo.flags |= BDIF_COD;
            info->localInfo.classOfDevice = ext->props.class;
        }
        if (ext->props_mask & WINEBLUETOOTH_RADIO_PROPERTY_VERSION)
            info->hciVersion = info->radioInfo.lmpVersion = ext->props.version;
        if (ext->props.connectable)
            info->flags |= LOCAL_RADIO_CONNECTABLE;
        if (ext->props.discoverable)
            info->flags |= LOCAL_RADIO_DISCOVERABLE;
        if (ext->props_mask & WINEBLUETOOTH_RADIO_PROPERTY_MANUFACTURER)
            info->radioInfo.mfg = ext->props.manufacturer;
        LeaveCriticalSection( &device_list_cs );

        irp->IoStatus.Information = sizeof( *info );
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_BTH_GET_DEVICE_INFO:
    {
        BTH_DEVICE_INFO_LIST *list = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        SIZE_T rem_devices;

        if (!list)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (outsize < sizeof( *list ))
        {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        rem_devices = (outsize - sizeof( *list ))/sizeof(BTH_DEVICE_INFO) + 1;
        status = STATUS_SUCCESS;
        irp->IoStatus.Information = 0;
        list->numOfDevices = 0;

        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            list->numOfDevices++;
            if (rem_devices > 0)
            {
                BTH_DEVICE_INFO *info;

                info = &list->deviceList[list->numOfDevices - 1];
                memset( info, 0, sizeof( *info ) );

                EnterCriticalSection( &device->props_cs );
                winebluetooth_device_properties_to_info( device->props_mask, &device->props, info );
                LeaveCriticalSection( &device->props_cs );

                irp->IoStatus.Information += sizeof( *info );
                rem_devices--;
            }
        }
        LeaveCriticalSection( &device_list_cs );

        irp->IoStatus.Information += sizeof( *list );
        if (list->numOfDevices)
            irp->IoStatus.Information -= sizeof( BTH_DEVICE_INFO );

        /* The output buffer needs to be exactly sized. */
        if (rem_devices)
            status = STATUS_INVALID_BUFFER_SIZE;
        break;
    }
    case IOCTL_BTH_DISCONNECT_DEVICE:
    {
        const BTH_ADDR *param = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        winebluetooth_device_t device_handle;
        BTH_ADDR device_addr;
        BOOL found = FALSE;

        if (!param || insize < sizeof( *param ))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        device_addr = RtlUlonglongByteSwap( *param ) >> 16;
        status = STATUS_DEVICE_NOT_CONNECTED;

        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            EnterCriticalSection( &device->props_cs );
            found = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                    device_addr == device->props.address.ullLong;
            LeaveCriticalSection( &device->props_cs );
            if (found)
            {
                winebluetooth_device_dup(( device_handle = device->device ));
                break;
            }
        }
        LeaveCriticalSection( &device_list_cs );
        if (found)
        {
            status = winebluetooth_device_disconnect( device_handle );
            winebluetooth_device_free( device_handle );
        }
        break;
    }
    case IOCTL_WINEBTH_RADIO_SET_FLAG:
    {
        const struct winebth_radio_set_flag_params *params = irp->AssociatedIrp.SystemBuffer;
        union winebluetooth_property prop_value = {0};

        if (!params)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (insize < sizeof( *params ))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        prop_value.boolean = !!params->enable;
        status = winebluetooth_radio_set_property( ext->radio, params->flag, &prop_value );
        break;
    }
    case IOCTL_WINEBTH_RADIO_START_DISCOVERY:
    {
        const struct winebth_radio_start_discovery_params *params = irp->AssociatedIrp.SystemBuffer;
        BOOL le = params && insize >= sizeof( *params ) && params->le;

        EnterCriticalSection( &device_list_cs );
        if (bluetooth_radio_check_power( ext ))
        {
            LeaveCriticalSection( &device_list_cs );
            status = STATUS_DEVICE_NOT_READY;
            break;
        }
        LeaveCriticalSection( &device_list_cs );
        /* Several LE watchers may run at once. BlueZ keeps one discovery session per client, so only the
         * first start and the last stop reach it. */
        if (!le)
        {
            status = winebluetooth_radio_start_discovery( ext->radio, FALSE );
            break;
        }
        EnterCriticalSection( &discovery_cs );
        if (ext->le_discovery_refs++)
            status = STATUS_SUCCESS;
        else if ((status = winebluetooth_radio_start_discovery( ext->radio, TRUE )))
            ext->le_discovery_refs--;
        LeaveCriticalSection( &discovery_cs );
        break;
    }
    case IOCTL_WINEBTH_RADIO_GET_LE_ADVERTISEMENTS:
    {
        struct winebth_radio_get_le_advertisements_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        SIZE_T capacity;

        if (!params || outsize < sizeof( *params ))
        {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }
        capacity = (outsize - offsetof( struct winebth_radio_get_le_advertisements_params, advertisements ))
                   / sizeof( params->advertisements[0] );
        params->count = 0;
        status = STATUS_SUCCESS;
        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            EnterCriticalSection( &device->props_cs );
            /* BlueZ only reports an RSSI for devices it has recently heard from. */
            if (device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_RSSI)
            {
                if (params->count < capacity)
                    bluetooth_device_fill_le_advertisement( device, &params->advertisements[params->count] );
                else
                    status = STATUS_BUFFER_OVERFLOW;
                params->count++;
            }
            LeaveCriticalSection( &device->props_cs );
        }
        LeaveCriticalSection( &device_list_cs );
        irp->IoStatus.Information = offsetof( struct winebth_radio_get_le_advertisements_params,
                                              advertisements[min( params->count, capacity )] );
        break;
    }
    case IOCTL_WINEBTH_RADIO_STOP_DISCOVERY:
        EnterCriticalSection( &discovery_cs );
        if (ext->le_discovery_refs > 0 && --ext->le_discovery_refs)
            status = STATUS_SUCCESS;
        else
            status = winebluetooth_radio_stop_discovery( ext->radio );
        LeaveCriticalSection( &discovery_cs );
        break;
    case IOCTL_WINEBTH_RADIO_SEND_AUTH_RESPONSE:
    {
        struct winebth_radio_send_auth_response_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;

        if (!params)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (insize < sizeof( *params ))
        {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }
        if (outsize < sizeof( *params ))
        {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        status = STATUS_DEVICE_NOT_CONNECTED;
        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            BOOL matches;
            EnterCriticalSection( &device->props_cs );
            matches = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                      device->props.address.ullLong == params->address;
            LeaveCriticalSection( &device->props_cs );
            if (matches)
            {
                BOOL authenticated = FALSE;
                status = winebluetooth_auth_send_response( device->device, params->method,
                                                           params->numeric_value_or_passkey, params->negative,
                                                           &authenticated );
                params->authenticated = !!authenticated;
                break;
            }
        }
        if (!status)
            irp->IoStatus.Information = sizeof( *params );
        LeaveCriticalSection( &device_list_cs );
        break;
    }
    case IOCTL_WINEBTH_RADIO_START_AUTH:
    {
        const struct winebth_radio_start_auth_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;

        if (!params)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (insize < sizeof( *params ))
        {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        status = STATUS_DEVICE_DOES_NOT_EXIST;
        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            BOOL matches;
            EnterCriticalSection( &device->props_cs );
            matches = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                device->props.address.ullLong == params->address;
            LeaveCriticalSection( &device->props_cs );
            if (matches)
            {
                status = winebluetooth_device_start_pairing( device->device, irp );
                if (status == STATUS_PENDING)
                {
                    IoMarkIrpPending( irp );
                    InsertTailList( &ext->irp_list, &irp->Tail.Overlay.ListEntry );
                }
                break;
            }
        }
        LeaveCriticalSection( &device_list_cs );
        break;
    }
    case IOCTL_WINEBTH_RADIO_REMOVE_DEVICE:
    {
        const BTH_ADDR *param = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        winebluetooth_device_t device_handle;
        winebluetooth_radio_t radio_handle;
        BOOL found = FALSE;

        if (!param)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (insize < sizeof( *param ))
        {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        status = STATUS_NOT_FOUND;
        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            EnterCriticalSection( &device->props_cs );
            found = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                    device->props.address.ullLong == *param && device->props.paired;
            LeaveCriticalSection( &device->props_cs );

            if (found)
            {
                winebluetooth_device_dup(( device_handle = device->device ));
                winebluetooth_radio_dup(( radio_handle = ext->radio ));
                break;
            }
        }
        LeaveCriticalSection( &device_list_cs );
        if (found)
        {
            status = winebluetooth_radio_remove_device( radio_handle, device_handle );
            winebluetooth_device_free( device_handle );
            winebluetooth_radio_free( radio_handle );
        }
        break;
    }
    default:
        FIXME( "Unimplemented IOCTL code: %#lx\n", code );
        break;
    }

    if (status != STATUS_PENDING)
    {
        irp->IoStatus.Status = status;
        IoCompleteRequest( irp, IO_NO_INCREMENT );
    }
    return status;
}

static NTSTATUS WINAPI dispatch_bluetooth( DEVICE_OBJECT *device, IRP *irp )
{
    struct bluetooth_pdo_ext *ext = device->DeviceExtension;
    TRACE( "(%p, %p)\n", device, irp );

    if (device == device_auth)
        return dispatch_auth( device, irp );

    switch (ext->type)
    {
    case BLUETOOTH_PDO_EXT_RADIO:
        return bluetooth_radio_dispatch( device, &ext->radio, irp );
    case BLUETOOTH_PDO_EXT_REMOTE_DEVICE:
        return bluetooth_remote_device_dispatch( device, &ext->remote_device, irp );
    case BLUETOOTH_PDO_EXT_GATT_SERVICE:
        return bluetooth_gatt_service_dispatch( device, &ext->gatt_service, irp );
    DEFAULT_UNREACHABLE;
    }
}

void WINAPIV append_id( struct string_buffer *buffer, const WCHAR *format, ... )
{
    va_list args;
    WCHAR *string;
    int len;

    va_start( args, format );

    len = _vsnwprintf( NULL, 0, format, args ) + 1;
    if (!(string = ExAllocatePool( PagedPool, (buffer->len + len) * sizeof( WCHAR ) )))
    {
        if (buffer->string)
            ExFreePool( buffer->string );
        buffer->string = NULL;
        return;
    }
    if (buffer->string)
    {
        memcpy( string, buffer->string, buffer->len * sizeof( WCHAR ) );
        ExFreePool( buffer->string );
    }
    _vsnwprintf( string + buffer->len, len, format, args );
    buffer->string = string;
    buffer->len += len;

    va_end( args );
}


static HANDLE event_loop_thread;
static NTSTATUS radio_get_hw_name_w( winebluetooth_radio_t radio, WCHAR **name )
{
    char *name_a;
    SIZE_T size = sizeof( char ) *  256;
    NTSTATUS status;

    name_a = malloc( size );
    if (!name_a)
        return STATUS_NO_MEMORY;

    status = winebluetooth_radio_get_unique_name( radio, name_a, &size );
    if (status == STATUS_BUFFER_TOO_SMALL)
    {
        void *ptr = realloc( name_a, size );
        if (!ptr)
        {
            free( name_a );
            return STATUS_NO_MEMORY;
        }
        name_a = ptr;
        status = winebluetooth_radio_get_unique_name( radio, name_a, &size );
    }
    if (status != STATUS_SUCCESS)
    {
        free( name_a );
        return status;
    }

    *name = malloc( (mbstowcs( NULL, name_a, 0 ) + 1) * sizeof( WCHAR ));
    if (!*name)
    {
        free( name_a );
        return status;
    }

    mbstowcs( *name, name_a, strlen( name_a ) + 1 );
    free( name_a );
    return STATUS_SUCCESS;
}

static void bluetooth_remove_all_radios( void )
{
    struct bluetooth_radio *radio, *radio2;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY_SAFE( radio, radio2, &device_list, struct bluetooth_radio, entry )
    {
        radio->removed = TRUE;
        list_remove( &radio->entry );
    }
    LeaveCriticalSection( &device_list_cs );

    IoInvalidateDeviceRelations( bus_pdo, BusRelations );
}

static void add_bluetooth_radio( struct winebluetooth_watcher_event_radio_added event )
{
    struct bluetooth_pdo_ext *ext;
    struct bluetooth_radio *radio;
    DEVICE_OBJECT *device_obj;
    UNICODE_STRING string;
    NTSTATUS status;
    WCHAR name[256];
    WCHAR *hw_name;
    static unsigned int radio_index;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        if (winebluetooth_radio_equal( radio->radio, event.radio ))
        {
            WARN( "Radio %#Ix already exists, skipping.\n", event.radio.handle );
            LeaveCriticalSection( &device_list_cs );
            winebluetooth_radio_free( event.radio );
            return;
        }
    }

    swprintf( name, ARRAY_SIZE( name ), L"\\Device\\WINEBTH-RADIO-%d", radio_index++ );
    TRACE( "Adding new bluetooth radio %p: %s\n", (void *)event.radio.handle, debugstr_w( name ) );

    status = radio_get_hw_name_w( event.radio, &hw_name );
    if (status)
    {
        ERR( "Failed to get hardware name for radio %p, status %#lx\n", (void *)event.radio.handle, status );
        LeaveCriticalSection( &device_list_cs );
        winebluetooth_radio_free( event.radio );
        return;
    }

    RtlInitUnicodeString( &string, name );
    status = IoCreateDevice( driver_obj, sizeof( *ext ), &string, FILE_DEVICE_BLUETOOTH, 0,
                             FALSE, &device_obj );
    if (status)
    {
        ERR( "Failed to create device, status %#lx\n", status );
        LeaveCriticalSection( &device_list_cs );
        winebluetooth_radio_free( event.radio );
        return;
    }

    ext = device_obj->DeviceExtension;
    ext->type = BLUETOOTH_PDO_EXT_RADIO;
    ext->radio.device_obj = device_obj;
    ext->radio.radio = event.radio;
    ext->radio.removed = FALSE;
    ext->radio.hw_name = hw_name;
    ext->radio.props = event.props;
    ext->radio.props_mask = event.props_mask;
    ext->radio.started = FALSE;
    list_init( &ext->radio.remote_devices );

    InitializeListHead( &ext->radio.irp_list );

    list_add_tail( &device_list, &ext->radio.entry );
    bluetooth_radio_check_power( &ext->radio );
    LeaveCriticalSection( &device_list_cs );

    IoInvalidateDeviceRelations( bus_pdo, BusRelations );
}

static void remove_bluetooth_radio( winebluetooth_radio_t radio )
{
    struct bluetooth_radio *device;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( device, &device_list, struct bluetooth_radio, entry )
    {
        if (winebluetooth_radio_equal( radio, device->radio ) && !device->removed)
        {
            TRACE( "Removing bluetooth radio %p\n", (void *)radio.handle );
            device->removed = TRUE;
            list_remove( &device->entry );
            break;
        }
    }
    LeaveCriticalSection( &device_list_cs );

    IoInvalidateDeviceRelations( bus_pdo, BusRelations );
    winebluetooth_radio_free( radio );
}

static void bluetooth_radio_set_properties( DEVICE_OBJECT *obj,
                                            winebluetooth_radio_props_mask_t mask,
                                            struct winebluetooth_radio_properties *props );

/* Whether the radio is known to be off, with the fix in the log. Caller must hold device_list_cs. */
static BOOL bluetooth_radio_check_power( struct bluetooth_radio *radio )
{
    if (!(radio->props_mask & WINEBLUETOOTH_RADIO_PROPERTY_POWERED) || radio->props.powered)
        return FALSE;
    if (!strcmp( radio->props.power_state, "off-blocked" ))
        ERR( "Bluetooth adapter %s is blocked by rfkill, no sensor will be found. Run: rfkill unblock bluetooth\n",
             debugstr_w( radio->hw_name ) );
    else
        ERR( "Bluetooth adapter %s is powered off, no sensor will be found. Run: bluetoothctl power on\n",
             debugstr_w( radio->hw_name ) );
    return TRUE;
}

static void update_bluetooth_radio_properties( struct winebluetooth_watcher_event_radio_props_changed event )
{
    struct bluetooth_radio *device;
    winebluetooth_radio_t radio = event.radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( device, &device_list, struct bluetooth_radio, entry )
    {
        if (winebluetooth_radio_equal( radio, device->radio ) && !device->removed)
        {
            device->props_mask |= event.changed_props_mask;
            device->props_mask &= ~event.invalid_props_mask;

            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_NAME)
                memcpy( device->props.name, event.props.name, sizeof( event.props.name ));
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_ADDRESS)
                device->props.address.ullLong = event.props.address.ullLong;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_DISCOVERABLE)
                device->props.discoverable = event.props.discoverable;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_CONNECTABLE)
                device->props.connectable = event.props.connectable;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_CLASS)
                device->props.class = event.props.class;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_MANUFACTURER)
                device->props.manufacturer = event.props.manufacturer;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_VERSION)
                device->props.version = event.props.version;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_DISCOVERING)
                device->props.discovering = event.props.discovering;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_PAIRABLE)
                device->props.pairable = event.props.pairable;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_POWER_STATE)
                memcpy( device->props.power_state, event.props.power_state, sizeof( event.props.power_state ) );
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_POWERED)
            {
                device->props.powered = event.props.powered;
                if (!bluetooth_radio_check_power( device ))
                    TRACE( "Bluetooth adapter %s is powered on\n", debugstr_w( device->hw_name ) );
            }
            if (device->started)
                bluetooth_radio_set_properties( device->device_obj, device->props_mask,
                                                &device->props );
            break;
        }
    }
    LeaveCriticalSection( &device_list_cs );
    winebluetooth_radio_free( radio );
}

static void bluetooth_radio_report_radio_in_range_event( DEVICE_OBJECT *radio_obj, ULONG remote_device_old_flags,
                                                         const BTH_DEVICE_INFO *new_device_info )
{
    TARGET_DEVICE_CUSTOM_NOTIFICATION *notification;
    BTH_RADIO_IN_RANGE *buffer;
    SIZE_T notif_size;
    NTSTATUS ret;

    notif_size = offsetof( TARGET_DEVICE_CUSTOM_NOTIFICATION, CustomDataBuffer[sizeof( *buffer )]);
    notification = ExAllocatePool( PagedPool, notif_size );
    if (!notification)
        return;

    notification->Version = 1;
    notification->Size = notif_size;
    notification->Event = GUID_BLUETOOTH_RADIO_IN_RANGE;
    notification->FileObject = NULL;
    notification->NameBufferOffset = -1;
    buffer = (BTH_RADIO_IN_RANGE *)notification->CustomDataBuffer;
    memset( buffer, 0, sizeof( *buffer ) );
    buffer->previousDeviceFlags = remote_device_old_flags;
    buffer->deviceInfo = *new_device_info;

    ret = IoReportTargetDeviceChange( radio_obj, notification );
    if (ret)
        ERR("IoReportTargetDeviceChange failed: %#lx\n", ret );
    ExFreePool( notification );
}

/* Caller must hold device->props_cs. */
static void bluetooth_device_fill_le_advertisement( struct bluetooth_remote_device *device,
                                                    struct winebth_le_advertisement *adv )
{
    *adv = device->props.le;
    adv->address = RtlUlonglongByteSwap( device->props.address.ullLong ) >> 16;
    adv->flags &= WINEBTH_LE_ADV_FLAG_RANDOM_ADDRESS;
    if (device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_RSSI)
        adv->flags |= WINEBTH_LE_ADV_FLAG_RSSI;
    if (device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_TX_POWER)
        adv->flags |= WINEBTH_LE_ADV_FLAG_TX_POWER;
    if (device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_APPEARANCE)
        adv->flags |= WINEBTH_LE_ADV_FLAG_APPEARANCE;
    if (device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_NAME)
    {
        adv->flags |= WINEBTH_LE_ADV_FLAG_NAME;
        memcpy( adv->name, device->props.name, sizeof( adv->name ) );
    }
    else
        adv->name[0] = '\0';
    if (!(device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_UUIDS))
        adv->uuid_count = 0;
    if (!(device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_MANUFACTURER_DATA))
        adv->manufacturer_data_count = 0;
    if (!(device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_SERVICE_DATA))
        adv->service_data_count = 0;
}

static void bluetooth_radio_report_le_advertisement( DEVICE_OBJECT *radio_obj, const struct winebth_le_advertisement *adv )
{
    TARGET_DEVICE_CUSTOM_NOTIFICATION *notification;
    SIZE_T notif_size;
    NTSTATUS ret;

    notif_size = offsetof( TARGET_DEVICE_CUSTOM_NOTIFICATION, CustomDataBuffer[sizeof( *adv )] );
    notification = ExAllocatePool( PagedPool, notif_size );
    if (!notification)
        return;

    notification->Version = 1;
    notification->Size = notif_size;
    notification->Event = GUID_WINEBTH_LE_ADVERTISEMENT;
    notification->FileObject = NULL;
    notification->NameBufferOffset = -1;
    memcpy( notification->CustomDataBuffer, adv, sizeof( *adv ) );

    ret = IoReportTargetDeviceChange( radio_obj, notification );
    if (ret)
        ERR( "IoReportTargetDeviceChange failed: %#lx\n", ret );
    ExFreePool( notification );
}

static void bluetooth_radio_add_remote_device( struct winebluetooth_watcher_event_device_added event )
{
    struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        if (winebluetooth_radio_equal( event.radio, radio->radio ))
        {
            struct bluetooth_remote_device *device;
            struct bluetooth_pdo_ext *ext;
            DEVICE_OBJECT *device_obj;
            NTSTATUS status;

            LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
            {
                if (winebluetooth_device_equal( device->device, event.device ))
                {
                    WARN( "Remote device %#Ix already exists, skipping.\n", event.device.handle );
                    winebluetooth_device_free( event.device );
                    goto done;
                }
            }

            status = IoCreateDevice( driver_obj, sizeof( *ext ), NULL, FILE_DEVICE_BLUETOOTH,
                                     FILE_AUTOGENERATED_DEVICE_NAME, FALSE, &device_obj );
            if (status)
            {
                ERR( "Failed to create remote device, status %#lx\n", status );
                winebluetooth_device_free( event.device );
                break;
            }

            ext = device_obj->DeviceExtension;
            ext->type = BLUETOOTH_PDO_EXT_REMOTE_DEVICE;
            ext->remote_device.radio = radio;

            ext->remote_device.device_obj = device_obj;
            InitializeCriticalSectionEx( &ext->remote_device.props_cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
            ext->remote_device.props_cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": bluetooth_pdo_ext.props_cs");
            ext->remote_device.device = event.device;
            ext->remote_device.props_mask = event.known_props_mask;
            ext->remote_device.props = event.props;
            ext->remote_device.removed = FALSE;
            ext->remote_device.started = FALSE;

            ext->remote_device.le = bluetooth_device_is_le( event.known_props_mask, &event.props );
            list_init( &ext->remote_device.gatt_services );
            InitializeListHead( &ext->remote_device.gatt_irp_list );

            if (!event.init_entry)
            {
                BTH_DEVICE_INFO device_info = {0};
                winebluetooth_device_properties_to_info( ext->remote_device.props_mask, &ext->remote_device.props, &device_info );
                bluetooth_radio_report_radio_in_range_event( radio->device_obj, 0, &device_info );
                if (ext->remote_device.props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_RSSI)
                {
                    struct winebth_le_advertisement adv;
                    bluetooth_device_fill_le_advertisement( &ext->remote_device, &adv );
                    bluetooth_radio_report_le_advertisement( radio->device_obj, &adv );
                }
            }

            list_add_tail( &radio->remote_devices, &ext->remote_device.entry );
            if (radio->started)
                IoInvalidateDeviceRelations( radio->device_obj, BusRelations );
            break;
        }
    }
done:
    LeaveCriticalSection( &device_list_cs );

    winebluetooth_radio_free( event.radio );
}

static void bluetooth_radio_remove_remote_device( struct winebluetooth_watcher_event_device_removed event )
{
    struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device, *next;

        LIST_FOR_EACH_ENTRY_SAFE( device, next, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            if (winebluetooth_device_equal( event.device, device->device ))
            {
                BOOL has_addr;

                TRACE( "Removing bluetooth remote device %p\n", (void *)device->device.handle );

                device->removed = TRUE;
                list_remove( &device->entry );

                EnterCriticalSection( &device->props_cs );
                has_addr = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS;
                LeaveCriticalSection( &device->props_cs );

                if (has_addr)
                {
                    TARGET_DEVICE_CUSTOM_NOTIFICATION *notification;
                    BLUETOOTH_ADDRESS *addr;
                    SIZE_T notif_size;
                    NTSTATUS ret;

                    notif_size = offsetof( TARGET_DEVICE_CUSTOM_NOTIFICATION, CustomDataBuffer[sizeof( *addr )] );
                    if ((notification = ExAllocatePool( PagedPool, notif_size )))
                    {
                        notification->Version = 1;
                        notification->Size = notif_size;
                        notification->Event = GUID_BLUETOOTH_RADIO_OUT_OF_RANGE;
                        notification->FileObject = NULL;
                        notification->NameBufferOffset = -1;
                        addr = (BLUETOOTH_ADDRESS *)notification->CustomDataBuffer;
                        addr->ullLong = RtlUlonglongByteSwap( device->props.address.ullLong ) >> 16;

                        ret = IoReportTargetDeviceChange( radio->device_obj, notification );
                        if (ret)
                            ERR( "IoReportTargetDeviceChange failed: %#lx\n", ret );
                        ExFreePool( notification );
                    }
                }
                if (radio->started)
                    IoInvalidateDeviceRelations( radio->device_obj, BusRelations );
                LeaveCriticalSection( &device_list_cs );
                winebluetooth_device_free( event.device );
                return;
            }
        }
    }
    LeaveCriticalSection( &device_list_cs );
    winebluetooth_device_free( event.device );
}

/* Caller should hold device->props_cs. */
static void bluetooth_device_set_properties( struct bluetooth_remote_device *device,
                                             const BYTE *adapter_addr,
                                             const struct winebluetooth_device_properties *props,
                                             winebluetooth_device_props_mask_t mask )
{
    if (mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS)
    {
        WCHAR addr_str[18], aep_id[59];
        const WCHAR *connection;
        const BYTE *device_addr = device->props.address.rgBytes;

        connection = device->bthle_symlink_name.Buffer ? L"BluetoothLE#BluetoothLE" : L"Bluetooth#Bluetooth";
        swprintf( aep_id, ARRAY_SIZE( aep_id ), L"%s#%s%02x:%02x:%02x:%02x:%02x:%02x-%02x:%02x:%02x:%02x:%02x:%02x",
                  connection, connection, adapter_addr[0], adapter_addr[1], adapter_addr[2], adapter_addr[3],
                  adapter_addr[4], adapter_addr[5], device_addr[0], device_addr[1], device_addr[2], device_addr[3],
                  device_addr[4], device_addr[5] );
        IoSetDevicePropertyData( device->device_obj, (DEVPROPKEY *)&PKEY_Devices_Aep_AepId, LOCALE_NEUTRAL, 0,
                                 DEVPROP_TYPE_STRING, sizeof( aep_id ), aep_id );

        swprintf( addr_str, ARRAY_SIZE( addr_str ), L"%02x%02x%02x%02x%02x%02x", device_addr[0], device_addr[1],
                  device_addr[2], device_addr[3], device_addr[4], device_addr[5] );
        IoSetDevicePropertyData( device->device_obj, &DEVPKEY_Bluetooth_DeviceAddress, LOCALE_NEUTRAL, 0,
                                 DEVPROP_TYPE_STRING, 26, addr_str );
        if (device->bthle_symlink_name.Buffer)
            IoSetDeviceInterfacePropertyData( &device->bthle_symlink_name,
                                              (DEVPROPKEY *)&PKEY_DeviceInterface_Bluetooth_DeviceAddress,
                                              LOCALE_NEUTRAL, 0, DEVPROP_TYPE_STRING, 26, addr_str );

        swprintf( addr_str, ARRAY_SIZE( addr_str ), L"%02x:%02x:%02x:%02x:%02x:%02x", device_addr[0], device_addr[1],
                  device_addr[2], device_addr[3], device_addr[4], device_addr[5] );
        IoSetDevicePropertyData( device->device_obj, (DEVPROPKEY *)&PKEY_Devices_Aep_DeviceAddress, LOCALE_NEUTRAL, 0,
                                 DEVPROP_TYPE_STRING, sizeof( addr_str ), addr_str );
    }
    if (mask & WINEBLUETOOTH_DEVICE_PROPERTY_CLASS)
         IoSetDevicePropertyData( device->device_obj, &DEVPKEY_Bluetooth_ClassOfDevice, LOCALE_NEUTRAL, 0,
                                  DEVPROP_TYPE_UINT32, sizeof( props->class ), (void *)&props->class );
    if (mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS_TYPE)
    {
        BYTE addr_type = props->le.flags & WINEBTH_LE_ADV_FLAG_RANDOM_ADDRESS ? 1 : 0;

        IoSetDevicePropertyData( device->device_obj, (DEVPROPKEY *)&PKEY_Devices_Aep_Bluetooth_Le_AddressType,
                                 LOCALE_NEUTRAL, 0, DEVPROP_TYPE_BYTE, sizeof( addr_type ), &addr_type );
    }
    if (mask & WINEBLUETOOTH_DEVICE_PROPERTY_CONNECTED && props->connected)
    {
        FILETIME time = {0};

        GetSystemTimeAsFileTime( &time );
        IoSetDevicePropertyData( device->device_obj, &DEVPKEY_Bluetooth_LastConnectedTime, LOCALE_NEUTRAL, 0,
                                 DEVPROP_TYPE_FILETIME, sizeof( time ), (void *)&time );
        if (device->bthle_symlink_name.Buffer)
            IoSetDeviceInterfacePropertyData( &device->bthle_symlink_name,
                                              (DEVPROPKEY *)&PKEY_DeviceInterface_Bluetooth_LastConnectedTime,
                                              LOCALE_NEUTRAL, 0, DEVPROP_TYPE_FILETIME, sizeof( time ), (void *)&time );
    }
}

static void bluetooth_radio_update_device_props( struct winebluetooth_watcher_event_device_props_changed event )
{
    BTH_DEVICE_INFO device_new_info = {0};
    DEVICE_OBJECT *radio_obj = NULL; /* The radio PDO the remote device exists on. */
    struct winebth_le_advertisement adv;
    BOOL report_adv = FALSE;
    struct bluetooth_radio *radio;
    ULONG device_old_flags = 0;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            if (winebluetooth_device_equal( event.device, device->device ))
            {
                BTH_DEVICE_INFO old_info = {0};
                BLUETOOTH_ADDRESS adapter_addr;
                winebluetooth_device_props_mask_t property_mask = event.changed_props_mask;
                BOOL had_le_iface;

                radio_obj = radio->device_obj;
                adapter_addr = radio->props.address;

                EnterCriticalSection( &device->props_cs );
                had_le_iface = !!device->bthle_symlink_name.Buffer;
                winebluetooth_device_properties_to_info( device->props_mask, &device->props, &old_info );

                device->props_mask |= event.changed_props_mask;
                device->props_mask &= ~event.invalid_props_mask;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_NAME)
                    memcpy( device->props.name, event.props.name, sizeof( event.props.name ));
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS)
                    device->props.address = event.props.address;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_CONNECTED)
                    device->props.connected = event.props.connected;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_PAIRED)
                    device->props.paired = event.props.paired;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_LEGACY_PAIRING)
                    device->props.legacy_pairing = event.props.legacy_pairing;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_TRUSTED)
                    device->props.trusted = event.props.trusted;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_CLASS)
                    device->props.class = event.props.class;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_RSSI)
                    device->props.le.rssi = event.props.le.rssi;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_TX_POWER)
                    device->props.le.tx_power = event.props.le.tx_power;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_APPEARANCE)
                    device->props.le.appearance = event.props.le.appearance;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS_TYPE)
                    device->props.le.flags = event.props.le.flags & WINEBTH_LE_ADV_FLAG_RANDOM_ADDRESS;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_UUIDS)
                {
                    device->props.le.uuid_count = event.props.le.uuid_count;
                    memcpy( device->props.le.uuids, event.props.le.uuids, sizeof( device->props.le.uuids ) );
                }
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_MANUFACTURER_DATA)
                {
                    device->props.le.manufacturer_data_count = event.props.le.manufacturer_data_count;
                    memcpy( device->props.le.manufacturer_data, event.props.le.manufacturer_data,
                            sizeof( device->props.le.manufacturer_data ) );
                }
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_SERVICE_DATA)
                {
                    device->props.le.service_data_count = event.props.le.service_data_count;
                    memcpy( device->props.le.service_data, event.props.le.service_data,
                            sizeof( device->props.le.service_data ) );
                }
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_SERVICES_RESOLVED)
                    device->props.services_resolved = event.props.services_resolved;
                if (bluetooth_device_is_le( device->props_mask, &device->props ))
                    bluetooth_device_enable_le_iface( device );
                winebluetooth_device_properties_to_info( device->props_mask, &device->props, &device_new_info );
                /* A new LE interface needs its initial properties. RSSI updates need no registry writes. */
                if (!had_le_iface && device->bthle_symlink_name.Buffer) property_mask = device->props_mask;
                bluetooth_device_set_properties( device, adapter_addr.rgBytes, &device->props, property_mask );
                if (device->props.connected && device->props.services_resolved)
                    bluetooth_device_complete_gatt_irps( device, STATUS_SUCCESS );
                /* A failed Connect can emit Connected=false before or after its reply. Keep requests
                 * queued while Connect (including a retry) owns them, so this signal cannot abort it. */
                else if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_CONNECTED &&
                         !device->props.connected && !device->connecting)
                    bluetooth_device_complete_gatt_irps( device, STATUS_DEVICE_NOT_CONNECTED );
                /* Any change to advertisement data while the device is in range counts as a new advertisement.
                 * Devices advertise many times a second, and each event crosses into user space, so signal
                 * strength changes on their own are rate limited. */
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_LE_PROPERTIES &&
                    device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_RSSI)
                {
                    ULONGLONG now = GetTickCount64();

                    if (now - device->last_adv_report >= 500)
                    {
                        device->last_adv_report = now;
                        bluetooth_device_fill_le_advertisement( device, &adv );
                        report_adv = TRUE;
                    }
                }
                LeaveCriticalSection( &device->props_cs );

                device_old_flags = old_info.flags;
                goto done;
            }
        }
    }
done:
    winebluetooth_device_free( event.device );

    if (radio_obj)
    {
        /* Signal strength and advertisement data do not appear in BTH_DEVICE_INFO, so only changes to the
         * classic properties are worth an in-range event. */
        if (event.changed_props_mask & ~WINEBLUETOOTH_DEVICE_LE_PROPERTIES || event.invalid_props_mask)
            bluetooth_radio_report_radio_in_range_event( radio_obj, device_old_flags, &device_new_info );
        if (report_adv)
            bluetooth_radio_report_le_advertisement( radio_obj, &adv );
    }

    LeaveCriticalSection( &device_list_cs );
}

static void bluetooth_radio_report_auth_event( struct winebluetooth_auth_event event )
{
    TARGET_DEVICE_CUSTOM_NOTIFICATION *notification;
    struct winebth_authentication_request *request;
    struct bluetooth_radio *radio;
    SIZE_T notif_size;

    notif_size = offsetof( TARGET_DEVICE_CUSTOM_NOTIFICATION, CustomDataBuffer[sizeof( *request )] );
    notification = ExAllocatePool( PagedPool, notif_size );
    if (!notification)
        return;

    notification->Version = 1;
    notification->Size = notif_size;
    notification->Event = GUID_WINEBTH_AUTHENTICATION_REQUEST;
    notification->FileObject = NULL;
    notification->NameBufferOffset = -1;
    request = (struct winebth_authentication_request *)notification->CustomDataBuffer;
    memset( request, 0, sizeof( *request ) );
    request->auth_method = event.method;
    request->numeric_value_or_passkey = event.numeric_value_or_passkey;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            if (winebluetooth_device_equal( event.device, device->device ))
            {
                NTSTATUS ret;

                EnterCriticalSection( &device->props_cs );
                winebluetooth_device_properties_to_info( device->props_mask, &device->props, &request->device_info );
                LeaveCriticalSection( &device->props_cs );
                LeaveCriticalSection( &device_list_cs );

                ret = IoReportTargetDeviceChange( device_auth, notification );
                if (ret)
                    ERR( "IoReportTargetDeviceChange failed: %#lx\n", ret );

                ExFreePool( notification );
                return;
            }
        }
    }
    LeaveCriticalSection( &device_list_cs );

    ExFreePool( notification );
}

static void complete_irp( IRP *irp, NTSTATUS result )
{
    RemoveEntryList( &irp->Tail.Overlay.ListEntry );

    irp->IoStatus.Status = result;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
}

/* Enables the low energy interface for this device if it hasn't been already. Caller should hold device->props_cs. */
static void bluetooth_device_enable_le_iface( struct bluetooth_remote_device *device )
{
    /* The device hasn't been started by the PnP manager yet. Set le, and let remote_device_pdo_pnp enable the
     * interface. */
    if (!device->started)
        device->le = TRUE;
    else if (!device->le)
    {
        device->le = TRUE;
        if (!IoRegisterDeviceInterface( device->device_obj, &GUID_BLUETOOTHLE_DEVICE_INTERFACE, NULL,
            &device->bthle_symlink_name ))
        IoSetDeviceInterfaceState( &device->bthle_symlink_name, TRUE );
    }
}

static void bluetooth_device_add_gatt_service( struct winebluetooth_watcher_event_gatt_service_added event )
{
    struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            if (winebluetooth_device_equal( event.device, device->device ) && !device->removed)
            {
                struct bluetooth_gatt_service *service;
                struct bluetooth_pdo_ext *ext;
                DEVICE_OBJECT *device_obj;
                NTSTATUS status;

                EnterCriticalSection( &device->props_cs );
                LIST_FOR_EACH_ENTRY( service, &device->gatt_services, struct bluetooth_gatt_service, entry )
                {
                    if (winebluetooth_gatt_service_equal( service->service, event.service ))
                    {
                        WARN( "GATT service %#Ix already exists, skipping.\n", event.device.handle );
                        LeaveCriticalSection( &device->props_cs );
                        goto failed;
                    }
                }

                TRACE( "Adding GATT service %s for remote device %p\n", debugstr_guid( &event.uuid ),
                       (void *)event.device.handle );

                status = IoCreateDevice( driver_obj, sizeof( *ext ), NULL, FILE_DEVICE_BLUETOOTH,
                                         FILE_AUTOGENERATED_DEVICE_NAME, FALSE, &device_obj );
                if (status)
                {
                    ERR( "Failed to create GATT service PDO, status %#lx\n", status );
                    LeaveCriticalSection( &device->props_cs );
                    goto failed;
                }

                ext = device_obj->DeviceExtension;
                ext->type = BLUETOOTH_PDO_EXT_GATT_SERVICE;
                ext->gatt_service.device_obj = device_obj;
                ext->gatt_service.service = event.service;
                ext->gatt_service.uuid = event.uuid;
                ext->gatt_service.primary = !!event.is_primary;
                ext->gatt_service.handle = event.attr_handle;
                ext->gatt_service.remote_device = device;
                InitializeListHead( &ext->gatt_service.irp_list );

                list_init( &ext->gatt_service.characteristics );
                InitializeCriticalSectionEx( &ext->gatt_service.chars_cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
                ext->gatt_service.chars_cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": bluetooth_gatt_service.chars_cs");
                bluetooth_device_enable_le_iface( device );

                list_add_tail( &device->gatt_services, &ext->gatt_service.entry );
                if (device->started)
                    IoInvalidateDeviceRelations( device->device_obj, BusRelations );
                LeaveCriticalSection( &device->props_cs );

                LeaveCriticalSection( &device_list_cs );
                winebluetooth_device_free( event.device );
                return;
            }
        }
    }
failed:
    LeaveCriticalSection( &device_list_cs );

    winebluetooth_device_free( event.device );
    winebluetooth_gatt_service_free( event.service );
}

static void bluetooth_gatt_service_remove( winebluetooth_gatt_service_t service )
{
    struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            struct bluetooth_gatt_service *svc;

            EnterCriticalSection( &device->props_cs );
            if (!device->le)
            {
                LeaveCriticalSection( &device->props_cs );
                continue;
            }
            LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
            {
                if (winebluetooth_gatt_service_equal( svc->service, service ))
                {
                    list_remove( &svc->entry );
                    svc->removed = 1;
                    if (device->started)
                        IoInvalidateDeviceRelations( device->device_obj, BusRelations );
                    LeaveCriticalSection( &device->props_cs );
                    LeaveCriticalSection( &device_list_cs );
                    winebluetooth_gatt_service_free( service );
                    return;
                }
            }
            LeaveCriticalSection( &device->props_cs );
        }
    }
    LeaveCriticalSection( &device_list_cs );
    winebluetooth_gatt_service_free( service );
}

static void
bluetooth_gatt_service_add_characteristic( struct winebluetooth_watcher_event_gatt_characteristic_added characteristic )
{
    struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            struct bluetooth_gatt_service *svc;

            EnterCriticalSection( &device->props_cs );
            if (!device->le)
            {
                LeaveCriticalSection( &device->props_cs );
                continue;
            }
            LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
            {
                if (winebluetooth_gatt_service_equal( svc->service, characteristic.service ))
                {
                    struct bluetooth_gatt_characteristic *entry;

                    LIST_FOR_EACH_ENTRY( entry, &svc->characteristics, struct bluetooth_gatt_characteristic, entry )
                    {
                        if (winebluetooth_gatt_characteristic_equal( entry->characteristic, characteristic.characteristic ))
                        {
                            WARN( "GATT characteristic %#Ix already exists, skipping.\n",
                                  entry->characteristic.handle );
                            LeaveCriticalSection( &device->props_cs );
                            goto failed;
                        }
                    }

                    if (!(entry = calloc( 1, sizeof( *entry ) )))
                    {
                        LeaveCriticalSection( &device->props_cs );
                        goto failed;
                    }
                    if (characteristic.value.size)
                    {
                        entry->value = calloc( 1, offsetof( BTH_LE_GATT_CHARACTERISTIC_VALUE, Data[characteristic.value.size] ) );
                        if (!entry->value)
                        {
                            LeaveCriticalSection( &device->props_cs );
                            free( entry );
                            goto failed;
                        }
                        entry->value->DataSize = characteristic.value.size;
                        winebluetooth_gatt_characteristic_value_move( &characteristic.value, entry->value->Data );
                    }

                    TRACE( "Adding GATT characteristic %#x under service %s for device %p\n",
                           characteristic.props.AttributeHandle, debugstr_guid( &svc->uuid ),
                           (void *)device->device.handle );

                    entry->characteristic = characteristic.characteristic;
                    entry->props = characteristic.props;
                    list_add_tail( &svc->characteristics, &entry->entry );
                    LeaveCriticalSection( &device->props_cs );
                    LeaveCriticalSection( &device_list_cs );
                    winebluetooth_gatt_service_free( characteristic.service );
                    return;
                }
            }
            LeaveCriticalSection( &device->props_cs );
        }
    }
failed:
    LeaveCriticalSection( &device_list_cs );
    winebluetooth_gatt_characteristic_value_free( &characteristic.value );
    winebluetooth_gatt_characteristic_free( characteristic.characteristic );
    winebluetooth_gatt_service_free( characteristic.service );
}

static void bluetooth_gatt_characteristic_remove( winebluetooth_gatt_characteristic_t handle )
{
     struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            struct bluetooth_gatt_service *svc;

            EnterCriticalSection( &device->props_cs );
            if (!device->le)
            {
                LeaveCriticalSection( &device->props_cs );
                continue;
            }
            LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
            {
                struct bluetooth_gatt_characteristic *chrc;
                EnterCriticalSection( &svc->chars_cs );
                LIST_FOR_EACH_ENTRY( chrc, &svc->characteristics, struct bluetooth_gatt_characteristic, entry )
                {
                    if (winebluetooth_gatt_characteristic_equal( chrc->characteristic, handle ))
                    {
                        list_remove( &chrc->entry );
                        LeaveCriticalSection( &svc->chars_cs );
                        LeaveCriticalSection( &device->props_cs );
                        LeaveCriticalSection( &device_list_cs );

                        winebluetooth_gatt_characteristic_free( chrc->characteristic );
                        winebluetooth_gatt_characteristic_free( handle );
                        if (chrc->value)
                            free( chrc->value );
                        free( chrc );
                        return;
                    }
                }
                LeaveCriticalSection( &svc->chars_cs );
            }
            LeaveCriticalSection( &device->props_cs );
        }
    }
    LeaveCriticalSection( &device_list_cs );
    winebluetooth_gatt_characteristic_free( handle );
}

/* Caller must hold svc->chars_cs. */
static void bluetooth_gatt_service_report_value_changed( struct bluetooth_gatt_service *svc,
                                                         struct bluetooth_gatt_characteristic *chrc )
{
    TARGET_DEVICE_CUSTOM_NOTIFICATION *notification;
    struct winebth_gatt_value_changed *changed;
    SIZE_T data_size, notif_size;
    NTSTATUS ret;

    data_size = offsetof( struct winebth_gatt_value_changed, data[chrc->value->DataSize] );
    notif_size = offsetof( TARGET_DEVICE_CUSTOM_NOTIFICATION, CustomDataBuffer[data_size] );
    if (!(notification = ExAllocatePool( PagedPool, notif_size )))
        return;
    notification->Version = 1;
    notification->Size = notif_size;
    notification->Event = GUID_WINEBTH_GATT_VALUE_CHANGED;
    notification->FileObject = NULL;
    notification->NameBufferOffset = -1;
    changed = (struct winebth_gatt_value_changed *)notification->CustomDataBuffer;
    changed->uuid = chrc->props.CharacteristicUuid;
    changed->handle = chrc->props.AttributeHandle;
    changed->size = chrc->value->DataSize;
    memcpy( changed->data, chrc->value->Data, chrc->value->DataSize );

    ret = IoReportTargetDeviceChange( svc->device_obj, notification );
    if (ret)
        ERR( "IoReportTargetDeviceChange failed: %#lx\n", ret );
    ExFreePool( notification );
}

static void bluetooth_gatt_characteristic_value_update( struct winebluetooth_watcher_event_gatt_characteristic_value_changed event )
{
    struct bluetooth_radio *radio;
    BOOL free_chrc_val = TRUE;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            struct bluetooth_gatt_service *svc;

            EnterCriticalSection( &device->props_cs );
            if (!device->le)
            {
                LeaveCriticalSection( &device->props_cs );
                continue;
            }
            LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
            {
                struct bluetooth_gatt_characteristic *chrc;

                EnterCriticalSection( &svc->chars_cs );
                LIST_FOR_EACH_ENTRY( chrc, &svc->characteristics, struct bluetooth_gatt_characteristic, entry )
                {
                    if (winebluetooth_gatt_characteristic_equal( chrc->characteristic, event.characteristic ))
                    {
                        if (!chrc->value || chrc->value->DataSize < event.value.size)
                        {
                            void *tmp;

                            tmp = realloc( chrc->value, offsetof( BTH_LE_GATT_CHARACTERISTIC_VALUE, Data[event.value.size] ) );
                            if (!tmp)
                            {
                                LeaveCriticalSection( &svc->chars_cs );
                                LeaveCriticalSection( &device->props_cs );
                                goto done;
                            }
                            chrc->value = tmp;
                        }
                        chrc->value->DataSize = event.value.size;
                        winebluetooth_gatt_characteristic_value_move( &event.value, chrc->value->Data );
                        free_chrc_val = FALSE;
                        bluetooth_gatt_service_report_value_changed( svc, chrc );
                        LeaveCriticalSection( &svc->chars_cs );
                        LeaveCriticalSection( &device->props_cs );
                        goto done;
                    }
                }
                LeaveCriticalSection( &svc->chars_cs );
            }
            LeaveCriticalSection( &device->props_cs );
        }
    }
done:
    LeaveCriticalSection( &device_list_cs );
    if (free_chrc_val)
        winebluetooth_gatt_characteristic_value_free( &event.value );
    winebluetooth_gatt_characteristic_free( event.characteristic );
}

static void bluetooth_gatt_characteristic_value_read_complete_irp(
    struct winebluetooth_watcher_event_gatt_characteristic_value_read read )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( read.irp );
    struct bluetooth_pdo_ext *ext = stack->DeviceObject->DeviceExtension;
    NTSTATUS status;

    assert( ext->type == BLUETOOTH_PDO_EXT_GATT_SERVICE );

    if (!(status = read.result))
    {
        ULONG needed = offsetof( struct winebth_gatt_service_read_characterisitic_value_params, buf[read.value.size] );
        struct winebth_gatt_service_read_characterisitic_value_params *params = read.irp->AssociatedIrp.SystemBuffer;
        ULONG outsize = stack->Parameters.DeviceIoControl.OutputBufferLength;

        params->size = read.value.size;
        if (outsize >= needed)
        {
            read.irp->IoStatus.Information = needed;
            winebluetooth_gatt_characteristic_value_move( &read.value, params->buf );
        }
        else
        {
            status = STATUS_MORE_ENTRIES;
            read.irp->IoStatus.Information = sizeof( *params );
            winebluetooth_gatt_characteristic_value_free( &read.value );
        }
    }

    EnterCriticalSection( &ext->gatt_service.chars_cs );
    complete_irp( read.irp, status );
    LeaveCriticalSection( &ext->gatt_service.chars_cs );
}

static void bluetooth_gatt_operation_complete_irp( struct winebluetooth_watcher_event_gatt_operation_finished finished )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( finished.irp );
    struct bluetooth_pdo_ext *ext = stack->DeviceObject->DeviceExtension;

    assert( ext->type == BLUETOOTH_PDO_EXT_GATT_SERVICE );
    EnterCriticalSection( &ext->gatt_service.chars_cs );
    complete_irp( finished.irp, finished.result );
    LeaveCriticalSection( &ext->gatt_service.chars_cs );
}

/* Whether the link has no owner left: every client closed and no Connect is in flight. Caller must hold props_cs. */
static BOOL bluetooth_device_unowned( struct bluetooth_remote_device *device )
{
    return !device->open_handles && !device->connecting && device->props.connected;
}

static void bluetooth_device_connect_finished( struct winebluetooth_watcher_event_connect_finished event )
{
    struct bluetooth_radio *radio;
    BOOL drop = FALSE;

    TRACE( "device %p irp %p result %#lx\n", (void *)event.device.handle, event.irp, event.result );

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;
        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            if (!winebluetooth_device_equal( event.device, device->device )) continue;
            EnterCriticalSection( &device->props_cs );
            device->connecting = FALSE;
            /* BlueZ aborts LE connections to devices with long advertising intervals. A second attempt
             * usually lands while the device is still awake, so retry before failing the request. */
            if (event.result && !IsListEmpty( &device->gatt_irp_list ) && device->connect_attempts < 3)
            {
                IRP *irp = CONTAINING_RECORD( device->gatt_irp_list.Flink, IRP, Tail.Overlay.ListEntry );
                NTSTATUS status;

                TRACE( "Retrying connection to %p after %#lx\n", (void *)event.device.handle, event.result );
                winebluetooth_device_dup( device->device );
                status = winebluetooth_device_connect( device->device, irp );
                if (status == STATUS_PENDING)
                {
                    device->connecting = TRUE;
                    device->connect_attempts++;
                    LeaveCriticalSection( &device->props_cs );
                    goto done;
                }
                winebluetooth_device_free( device->device );
                event.result = status;
            }
            if (event.result)
                bluetooth_device_complete_gatt_irps( device, event.result );
            else if (device->props.connected && device->props.services_resolved)
                bluetooth_device_complete_gatt_irps( device, STATUS_SUCCESS );
            /* The application may have given up and closed the device while Connect was pending. */
            drop = bluetooth_device_unowned( device );
            LeaveCriticalSection( &device->props_cs );
            goto done;
        }
    }
    /* The device went away while connecting. Its request queue is drained by remote_device_destroy.
     * event.irp may also have completed on ServicesResolved already, so never complete it here. */
done:
    LeaveCriticalSection( &device_list_cs );
    if (drop)
    {
        TRACE( "No client holds %p, disconnecting\n", (void *)event.device.handle );
        winebluetooth_device_disconnect( event.device );
    }
    winebluetooth_device_free( event.device );
}

static DWORD CALLBACK bluetooth_event_loop_thread_proc( void *arg )
{
    NTSTATUS status;
    while (TRUE)
    {
        struct winebluetooth_event result = {0};

        status = winebluetooth_get_event( &result );
        if (status != STATUS_PENDING) break;

        switch (result.status)
        {
            case WINEBLUETOOTH_EVENT_WATCHER_EVENT:
            {
                struct winebluetooth_watcher_event *event = &result.data.watcher_event;
                switch (event->event_type)
                {
                    case BLUETOOTH_WATCHER_EVENT_TYPE_SERVICE_DOWN:
                        bluetooth_remove_all_radios();
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_RADIO_ADDED:
                        add_bluetooth_radio( event->event_data.radio_added );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_RADIO_REMOVED:
                        remove_bluetooth_radio( event->event_data.radio_removed );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_RADIO_PROPERTIES_CHANGED:
                        update_bluetooth_radio_properties( event->event_data.radio_props_changed );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_DEVICE_ADDED:
                        bluetooth_radio_add_remote_device( event->event_data.device_added );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_DEVICE_REMOVED:
                        bluetooth_radio_remove_remote_device( event->event_data.device_removed );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_DEVICE_PROPERTIES_CHANGED:
                        bluetooth_radio_update_device_props( event->event_data.device_props_changed);
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_PAIRING_FINISHED:
                        EnterCriticalSection( &device_list_cs );
                        complete_irp( event->event_data.pairing_finished.irp,
                                      event->event_data.pairing_finished.result );
                        LeaveCriticalSection( &device_list_cs );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_CONNECT_FINISHED:
                        bluetooth_device_connect_finished( event->event_data.connect_finished );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_GATT_OPERATION_FINISHED:
                        bluetooth_gatt_operation_complete_irp( event->event_data.gatt_operation_finished );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_DEVICE_GATT_SERVICE_ADDED:
                        bluetooth_device_add_gatt_service( event->event_data.gatt_service_added );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_DEVICE_GATT_SERVICE_REMOVED:
                        bluetooth_gatt_service_remove( event->event_data.gatt_service_removed );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_GATT_CHARACTERISTIC_ADDED:
                        bluetooth_gatt_service_add_characteristic( event->event_data.gatt_characteristic_added );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_GATT_CHARACTERISTIC_REMOVED:
                        bluetooth_gatt_characteristic_remove( event->event_data.gatt_characterisic_removed );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_GATT_CHARACTERISTIC_VALUE_CHANGED:
                        bluetooth_gatt_characteristic_value_update( event->event_data.gatt_characteristic_value_changed );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_GATT_CHARACTERISTIC_VALUE_READ:
                        bluetooth_gatt_characteristic_value_read_complete_irp(
                            event->event_data.gatt_characteristic_value_read );
                        break;
                    default:
                        FIXME( "Unknown bluetooth watcher event code: %#x\n", event->event_type );
                }
                break;
            }
            case WINEBLUETOOTH_EVENT_AUTH_EVENT:
                bluetooth_radio_report_auth_event( result.data.auth_event);
                winebluetooth_device_free( result.data.auth_event.device );
                break;
            default:
                FIXME( "Unknown bluetooth event loop status code: %#x\n", result.status );
        }
    }

    if (status != STATUS_SUCCESS)
        ERR( "Bluetooth event loop terminated with %#lx", status );
    else
        TRACE( "Exiting bluetooth event loop\n" );
    return 0;
}

static NTSTATUS WINAPI fdo_pnp( DEVICE_OBJECT *device_obj, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );

    TRACE( "irp %p, minor function %s.\n", irp, debugstr_minor_function_code( stack->MinorFunction ) );

    switch (stack->MinorFunction)
    {
        case IRP_MN_QUERY_DEVICE_RELATIONS:
        {
            struct bluetooth_radio *radio;
            DEVICE_RELATIONS *devices;
            SIZE_T i = 0;

            if (stack->Parameters.QueryDeviceRelations.Type != BusRelations)
            {
                FIXME( "Unhandled Device Relation %x\n",
                       stack->Parameters.QueryDeviceRelations.Type );
                break;
            }

            EnterCriticalSection( &device_list_cs );
            devices = ExAllocatePool(
                PagedPool, offsetof( DEVICE_RELATIONS, Objects[list_count( &device_list )] ) );
            if (devices == NULL)
            {
                LeaveCriticalSection( &device_list_cs );
                irp->IoStatus.Status = STATUS_NO_MEMORY;
                break;
            }

            LIST_FOR_EACH_ENTRY(radio, &device_list, struct bluetooth_radio, entry)
            {
                devices->Objects[i++] = radio->device_obj;
                call_fastcall_func1( ObfReferenceObject, radio->device_obj );
            }
            LeaveCriticalSection( &device_list_cs );

            devices->Count = i;
            irp->IoStatus.Information = (ULONG_PTR)devices;
            irp->IoStatus.Status = STATUS_SUCCESS;
            break;
        }
        case IRP_MN_START_DEVICE:
            event_loop_thread =
                CreateThread( NULL, 0, bluetooth_event_loop_thread_proc, NULL, 0, NULL );
            irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_SURPRISE_REMOVAL:
            irp->IoStatus.Status = STATUS_SUCCESS;
            break;
        case IRP_MN_REMOVE_DEVICE:
        {
            struct bluetooth_radio *device, *cur;
            NTSTATUS ret;

            winebluetooth_shutdown();
            WaitForSingleObject( event_loop_thread, INFINITE );
            CloseHandle( event_loop_thread );
            EnterCriticalSection( &device_list_cs );
            LIST_FOR_EACH_ENTRY_SAFE( device, cur, &device_list, struct bluetooth_radio, entry )
            {
                assert( !device->removed );
                winebluetooth_radio_free( device->radio );
                list_remove( &device->entry );
                IoDeleteDevice( device->device_obj );
            }
            LeaveCriticalSection( &device_list_cs );
            IoSkipCurrentIrpStackLocation( irp );
            ret = IoCallDriver( bus_pdo, irp );
            IoDetachDevice( bus_pdo );
            IoDeleteDevice( bus_fdo );
            return ret;
        }

        case IRP_MN_QUERY_ID:
            break;

        default:
            FIXME( "Unhandled minor function %s.\n", debugstr_minor_function_code( stack->MinorFunction ) );
    }

    IoSkipCurrentIrpStackLocation( irp );
    return IoCallDriver( bus_pdo, irp );
}

static NTSTATUS gatt_service_query_id( struct bluetooth_gatt_service *ext, IRP *irp, BUS_QUERY_ID_TYPE type )
{
    struct string_buffer buf = {0};

    TRACE("(%p, %p, %s)\n", ext, irp, debugstr_BUS_QUERY_ID_TYPE( type ) );
    switch (type)
    {
    case BusQueryDeviceID:
        append_id( &buf, L"WINEBTH\\GATTSVC" );
        break;
    case BusQueryInstanceID:
    {
        BLUETOOTH_ADDRESS addr;
        GUID uuid = ext->uuid;

        EnterCriticalSection( &ext->remote_device->props_cs );
        addr = ext->remote_device->props.address;
        LeaveCriticalSection( &ext->remote_device->props_cs );
        append_id( &buf, L"%s&%02X%02X%02X%02X%02X%02X&{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}&%04X",
                   ext->remote_device->radio->hw_name, addr.rgBytes[0], addr.rgBytes[1], addr.rgBytes[2],
                   addr.rgBytes[3], addr.rgBytes[4], addr.rgBytes[5], uuid.Data1, uuid.Data2, uuid.Data3, uuid.Data4[0],
                   uuid.Data4[1], uuid.Data4[2], uuid.Data4[3], uuid.Data4[4], uuid.Data4[5], uuid.Data4[6],
                   uuid.Data4[7], ext->handle );
        break;
    }
    case BusQueryHardwareIDs:
    case BusQueryCompatibleIDs:
        append_id( &buf, L"" );
        break;
    default:
        return irp->IoStatus.Status;
    }

    if (!buf.string)
        return STATUS_NO_MEMORY;

    irp->IoStatus.Information = (ULONG_PTR)buf.string;
    return STATUS_SUCCESS;
}

static NTSTATUS remote_device_query_id( struct bluetooth_remote_device *ext, IRP *irp, BUS_QUERY_ID_TYPE type )
{
    struct string_buffer buf = {0};

    TRACE( "(%p, %p, %s)\n", ext, irp, debugstr_BUS_QUERY_ID_TYPE( type ) );
    switch (type)
    {
    case BusQueryDeviceID:
        append_id( &buf, L"WINEBTH\\DEVICE" );
        break;
    case BusQueryInstanceID:
    {
        BLUETOOTH_ADDRESS addr;

        EnterCriticalSection( &ext->props_cs );
        addr = ext->props.address;
        LeaveCriticalSection( &ext->props_cs );

        append_id( &buf, L"%s&%02X%02X%02X%02X%02X%02X", ext->radio->hw_name, addr.rgBytes[0], addr.rgBytes[1],
                   addr.rgBytes[2], addr.rgBytes[3], addr.rgBytes[4], addr.rgBytes[5] );
        break;
    }
    case BusQueryHardwareIDs:
    case BusQueryCompatibleIDs:
        append_id( &buf, L"" );
        break;
    default:
        return irp->IoStatus.Status;
    }

    if (!buf.string)
        return STATUS_NO_MEMORY;

    irp->IoStatus.Information = (ULONG_PTR)buf.string;
    return STATUS_SUCCESS;
}

static NTSTATUS radio_query_id( const struct bluetooth_radio *ext, IRP *irp, BUS_QUERY_ID_TYPE type )
{
    struct string_buffer buf = {0};

    TRACE( "(%p, %p, %s)\n", ext, irp, debugstr_BUS_QUERY_ID_TYPE( type ) );
    switch (type)
    {
    case BusQueryDeviceID:
        append_id( &buf, L"WINEBTH\\RADIO" );
        break;
    case BusQueryInstanceID:
        append_id( &buf, L"%s", ext->hw_name );
        break;
    case BusQueryHardwareIDs:
    case BusQueryCompatibleIDs:
        append_id( &buf, L"" );
        break;
    default:
        return irp->IoStatus.Status;
    }

    if (!buf.string)
        return STATUS_NO_MEMORY;

    irp->IoStatus.Information = (ULONG_PTR)buf.string;
    return STATUS_SUCCESS;
}

/* Caller must hold props_cs */
static void bluetooth_radio_set_properties( DEVICE_OBJECT *obj,
                                            winebluetooth_radio_props_mask_t mask,
                                            struct winebluetooth_radio_properties *props )
{
    if (mask & WINEBLUETOOTH_RADIO_PROPERTY_ADDRESS)
    {
        BTH_ADDR addr = RtlUlonglongByteSwap( props->address.ullLong ) >> 16;
        IoSetDevicePropertyData( obj, &DEVPKEY_BluetoothRadio_Address, LOCALE_NEUTRAL, 0,
                                 DEVPROP_TYPE_UINT64, sizeof( addr ), &addr );
    }
    if (mask & WINEBLUETOOTH_RADIO_PROPERTY_MANUFACTURER)
    {
        UINT16 manufacturer = props->manufacturer;
        IoSetDevicePropertyData( obj, &DEVPKEY_BluetoothRadio_Manufacturer, LOCALE_NEUTRAL,
                                 0, DEVPROP_TYPE_UINT16, sizeof( manufacturer ), &manufacturer );
    }
    if (mask & WINEBLUETOOTH_RADIO_PROPERTY_NAME)
    {
        WCHAR buf[BLUETOOTH_MAX_NAME_SIZE * sizeof(WCHAR)];
        INT ret;

        if ((ret = MultiByteToWideChar( CP_ACP, 0, props->name, -1, buf, BLUETOOTH_MAX_NAME_SIZE)))
            IoSetDevicePropertyData( obj, &DEVPKEY_NAME, LOCALE_NEUTRAL, 0, DEVPROP_TYPE_STRING, ret, buf );
    }
    if (mask & WINEBLUETOOTH_RADIO_PROPERTY_VERSION)
        IoSetDevicePropertyData( obj, &DEVPKEY_BluetoothRadio_LMPVersion, LOCALE_NEUTRAL, 0, DEVPROP_TYPE_BYTE,
                                 sizeof( props->version ), &props->version );
}

static void remove_pending_irps( LIST_ENTRY *irp_list )
{
    LIST_ENTRY *entry;
    IRP *irp;

    while ((entry = RemoveHeadList( irp_list )) != irp_list)
    {
        irp = CONTAINING_RECORD( entry, IRP, Tail.Overlay.ListEntry );
        irp->IoStatus.Status = STATUS_DELETE_PENDING;
        irp->IoStatus.Information = 0;
        IoCompleteRequest( irp, IO_NO_INCREMENT );
    }
}

static NTSTATUS WINAPI gatt_service_pdo_pnp( DEVICE_OBJECT *device_obj, struct bluetooth_gatt_service *ext, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    NTSTATUS ret = irp->IoStatus.Status;

    TRACE( "device_obj=%p, ext=%p, irp=%p, minor function=%s\n", device_obj, ext, irp,
           debugstr_minor_function_code( stack->MinorFunction ) );

    switch (stack->MinorFunction)
    {
    case IRP_MN_QUERY_ID:
        ret = gatt_service_query_id( ext, irp, stack->Parameters.QueryId.IdType );
        break;
    case IRP_MN_QUERY_CAPABILITIES:
    {
        DEVICE_CAPABILITIES *caps = stack->Parameters.DeviceCapabilities.Capabilities;
        caps->Removable = TRUE;
        caps->SurpriseRemovalOK = TRUE;
        caps->RawDeviceOK = TRUE;
        ret = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_START_DEVICE:
    {
        WCHAR addr_str[13];
        BLUETOOTH_ADDRESS addr;

        EnterCriticalSection( &ext->remote_device->props_cs );
        addr = ext->remote_device->props.address;
        LeaveCriticalSection( &ext->remote_device->props_cs );
        if (!IoRegisterDeviceInterface( device_obj, &GUID_BLUETOOTH_GATT_SERVICE_DEVICE_INTERFACE, NULL,
                                        &ext->service_symlink_name ))
            IoSetDeviceInterfaceState( &ext->service_symlink_name, TRUE );
        swprintf( addr_str, ARRAY_SIZE( addr_str ), L"%02x%02x%02x%02x%02x%02x", addr.rgBytes[0], addr.rgBytes[1],
                  addr.rgBytes[2], addr.rgBytes[3], addr.rgBytes[4], addr.rgBytes[5] );
        IoSetDevicePropertyData( device_obj, &DEVPKEY_Bluetooth_DeviceAddress, LOCALE_NEUTRAL, 0, DEVPROP_TYPE_STRING,
                                 sizeof( addr_str ), addr_str );
        IoSetDevicePropertyData( device_obj, &DEVPKEY_Bluetooth_ServiceGUID, LOCALE_NEUTRAL, 0, DEVPROP_TYPE_GUID,
                                 sizeof( ext->uuid ), &ext->uuid );
        ret = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_REMOVE_DEVICE:
    {
        struct bluetooth_gatt_characteristic *chrc, *next;

        assert( ext->removed );
        remove_pending_irps( &ext->irp_list );
        if (ext->service_symlink_name.Buffer)
        {
            IoSetDeviceInterfaceState( &ext->service_symlink_name, FALSE );
            RtlFreeUnicodeString( &ext->service_symlink_name );
        }
        winebluetooth_gatt_service_free( ext->service );
        ext->chars_cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &ext->chars_cs );
        LIST_FOR_EACH_ENTRY_SAFE( chrc, next, &ext->characteristics, struct bluetooth_gatt_characteristic, entry )
        {
            winebluetooth_gatt_characteristic_free( chrc->characteristic );
            free( chrc );
        }
        IoDeleteDevice( ext->device_obj );
        break;
    }
    case IRP_MN_SURPRISE_REMOVAL:
    {
        remove_pending_irps( &ext->irp_list );
        EnterCriticalSection( &ext->remote_device->props_cs );
        if (!ext->removed)
        {
            ext->removed = 1;
            list_remove( &ext->entry );
        }
        LeaveCriticalSection( &ext->remote_device->props_cs );
        ret = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_DEVICE_TEXT:
        WARN("Unhandled IRP_MN_QUERY_DEVICE_TEXT text type %u.\n", stack->Parameters.QueryDeviceText.DeviceTextType);
        break;
    default:
        FIXME("Unhandled minor function %#x.\n", stack->MinorFunction );
    }

    irp->IoStatus.Status = ret;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return ret;
}

static void remote_device_destroy( struct bluetooth_remote_device *ext )
{
    EnterCriticalSection( &ext->props_cs );
    bluetooth_device_complete_gatt_irps( ext, STATUS_DEVICE_REMOVED );
    LeaveCriticalSection( &ext->props_cs );
    if (ext->bthle_symlink_name.Buffer)
    {
        IoSetDeviceInterfaceState( &ext->bthle_symlink_name, FALSE );
        RtlFreeUnicodeString( &ext->bthle_symlink_name );
    }
    ext->props_cs.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection( &ext->props_cs );
    winebluetooth_device_free( ext->device );
    IoDeleteDevice( ext->device_obj );
}

static NTSTATUS WINAPI remote_device_pdo_pnp( DEVICE_OBJECT *device_obj, struct bluetooth_remote_device *ext, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    NTSTATUS ret = irp->IoStatus.Status;

    TRACE( "device_obj=%p, ext=%p, irp=%p, minor function=%s\n", device_obj, ext, irp,
           debugstr_minor_function_code( stack->MinorFunction ) );

    switch (stack->MinorFunction)
    {
    case IRP_MN_QUERY_DEVICE_RELATIONS:
    {
        struct bluetooth_gatt_service *service;
        DEVICE_RELATIONS *devices;
        SIZE_T i = 0;

        if (stack->Parameters.QueryDeviceRelations.Type != BusRelations)
        {
            FIXME( "Unhandled Device Relation %x\n", stack->Parameters.QueryDeviceRelations.Type );
            break;
        }
        EnterCriticalSection( &ext->props_cs );
        devices = ExAllocatePool( PagedPool, offsetof( DEVICE_RELATIONS, Objects[list_count( &ext->gatt_services )] ) );
        if (!devices)
        {
            LeaveCriticalSection( &ext->props_cs );
            irp->IoStatus.Status = STATUS_NO_MEMORY;
            break;
        }
        LIST_FOR_EACH_ENTRY( service, &ext->gatt_services, struct bluetooth_gatt_service, entry )
        {
            devices->Objects[i++] = service->device_obj;
            call_fastcall_func1( ObfReferenceObject, service->device_obj );
        }
        LeaveCriticalSection( &ext->props_cs );
        devices->Count = i;
        irp->IoStatus.Information = (ULONG_PTR)devices;
        ret = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_ID:
        ret = remote_device_query_id( ext, irp, stack->Parameters.QueryId.IdType );
        break;
    case IRP_MN_QUERY_CAPABILITIES:
    {
        DEVICE_CAPABILITIES *caps = stack->Parameters.DeviceCapabilities.Capabilities;
        caps->Removable = TRUE;
        caps->SurpriseRemovalOK = TRUE;
        caps->RawDeviceOK = TRUE;
        ret = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_START_DEVICE:
    {
        BLUETOOTH_ADDRESS adapter_addr;
        BOOL needs_invalidate;

        EnterCriticalSection( &device_list_cs );
        adapter_addr = ext->radio->props.address;
        LeaveCriticalSection( &device_list_cs );

        EnterCriticalSection( &ext->props_cs );
        if (ext->le &&
            !IoRegisterDeviceInterface( device_obj, &GUID_BLUETOOTHLE_DEVICE_INTERFACE, NULL,
                                        &ext->bthle_symlink_name ))
            IoSetDeviceInterfaceState( &ext->bthle_symlink_name, TRUE );
        ext->started = TRUE;
        bluetooth_device_set_properties( ext, adapter_addr.rgBytes, &ext->props, ext->props_mask );
        needs_invalidate = !list_empty( &ext->gatt_services );
        LeaveCriticalSection( &ext->props_cs );
        if (needs_invalidate)
            IoInvalidateDeviceRelations( device_obj, BusRelations );
        ret = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_REMOVE_DEVICE:
    {
        assert( ext->removed );
        remote_device_destroy( ext );
        ret = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_SURPRISE_REMOVAL:
    {
        EnterCriticalSection( &device_list_cs);
        if (!ext->removed)
        {
            ext->removed = TRUE;
            list_remove( &ext->entry );
        }
        LeaveCriticalSection( &device_list_cs );
        ret = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_DEVICE_TEXT:
        WARN("Unhandled IRP_MN_QUERY_DEVICE_TEXT text type %u.\n", stack->Parameters.QueryDeviceText.DeviceTextType);
        break;

    default:
        FIXME( "Unhandled minor function %#x.\n", stack->MinorFunction );
    }

    irp->IoStatus.Status = ret;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return ret;
}

static NTSTATUS WINAPI radio_pdo_pnp( DEVICE_OBJECT *device_obj, struct bluetooth_radio *device, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS ret = irp->IoStatus.Status;

    TRACE( "device_obj %p, device %p, irp %p, minor function %s\n", device_obj, device, irp,
           debugstr_minor_function_code( stack->MinorFunction ) );
    switch (stack->MinorFunction)
    {
        case IRP_MN_QUERY_DEVICE_RELATIONS:
        {
            struct bluetooth_remote_device *remote_device;
            DEVICE_RELATIONS *devices;
            SIZE_T i = 0;

            if (stack->Parameters.QueryDeviceRelations.Type != BusRelations)
            {
                FIXME( "Unhandled Device Relation %x\n", stack->Parameters.QueryDeviceRelations.Type );
                break;
            }

            EnterCriticalSection( &device_list_cs );
            devices = ExAllocatePool( PagedPool,
                                      offsetof( DEVICE_RELATIONS, Objects[list_count( &device->remote_devices )] ) );
            if (!devices)
            {
                LeaveCriticalSection( &device_list_cs );
                irp->IoStatus.Status = STATUS_NO_MEMORY;
                break;
            }
            LIST_FOR_EACH_ENTRY( remote_device, &device->remote_devices, struct bluetooth_remote_device, entry )
            {
                devices->Objects[i++] = remote_device->device_obj;
                call_fastcall_func1( ObfReferenceObject, remote_device->device_obj );
            }
            LeaveCriticalSection( &device_list_cs );

            devices->Count = i;
            irp->IoStatus.Information = (ULONG_PTR)devices;
            ret = STATUS_SUCCESS;
            break;
        }
        case IRP_MN_QUERY_ID:
            ret = radio_query_id( device, irp, stack->Parameters.QueryId.IdType );
            break;
        case IRP_MN_QUERY_CAPABILITIES:
        {
            DEVICE_CAPABILITIES *caps = stack->Parameters.DeviceCapabilities.Capabilities;
            caps->Removable = TRUE;
            caps->SurpriseRemovalOK = TRUE;
            caps->RawDeviceOK = TRUE;
            ret = STATUS_SUCCESS;
            break;
        }
        case IRP_MN_START_DEVICE:
        {
            BOOL needs_invalidate;

            EnterCriticalSection( &device_list_cs );
            bluetooth_radio_set_properties( device_obj, device->props_mask, &device->props );
            device->started = TRUE;
            needs_invalidate = !list_empty( &device->remote_devices );
            LeaveCriticalSection( &device_list_cs );

            if (IoRegisterDeviceInterface( device_obj, &GUID_BTHPORT_DEVICE_INTERFACE, NULL,
                                          &device->bthport_symlink_name ) == STATUS_SUCCESS)
                IoSetDeviceInterfaceState( &device->bthport_symlink_name, TRUE );

            if (IoRegisterDeviceInterface( device_obj, &GUID_BLUETOOTH_RADIO_INTERFACE, NULL,
                                          &device->bthradio_symlink_name ) == STATUS_SUCCESS)
                IoSetDeviceInterfaceState( &device->bthradio_symlink_name, TRUE );
            if (needs_invalidate)
                IoInvalidateDeviceRelations( device_obj, BusRelations );
            ret = STATUS_SUCCESS;
            break;
        }
        case IRP_MN_REMOVE_DEVICE:
            assert( device->removed );
            EnterCriticalSection( &device_list_cs );
            remove_pending_irps( &device->irp_list );
            LeaveCriticalSection( &device_list_cs );

            if (device->bthport_symlink_name.Buffer)
            {
                IoSetDeviceInterfaceState(&device->bthport_symlink_name, FALSE);
                RtlFreeUnicodeString( &device->bthport_symlink_name );
            }
            if (device->bthradio_symlink_name.Buffer)
            {
                IoSetDeviceInterfaceState(&device->bthradio_symlink_name, FALSE);
                RtlFreeUnicodeString( &device->bthradio_symlink_name );
            }
            free( device->hw_name );
            winebluetooth_radio_free( device->radio );
            IoDeleteDevice( device->device_obj );
            ret = STATUS_SUCCESS;
            break;
        case IRP_MN_SURPRISE_REMOVAL:
            EnterCriticalSection( &device_list_cs );
            remove_pending_irps( &device->irp_list );
            if (!device->removed)
            {
                device->removed = TRUE;
                list_remove( &device->entry );
            }
            LeaveCriticalSection( &device_list_cs );
            ret = STATUS_SUCCESS;
            break;
        case IRP_MN_QUERY_DEVICE_TEXT:
            WARN("Unhandled IRP_MN_QUERY_DEVICE_TEXT text type %u.\n", stack->Parameters.QueryDeviceText.DeviceTextType);
            break;
        default:
            FIXME( "Unhandled minor function %s.\n", debugstr_minor_function_code( stack->MinorFunction ) );
            break;
    }

    irp->IoStatus.Status = ret;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return ret;
}

static NTSTATUS auth_pnp( DEVICE_OBJECT *device, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS ret = irp->IoStatus.Status;

    TRACE( "device_obj %p, irp %p, minor function %s\n", device, irp, debugstr_minor_function_code( stack->MinorFunction ) );
    switch (stack->MinorFunction)
    {
    case IRP_MN_QUERY_ID:
    case IRP_MN_START_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        ret = STATUS_SUCCESS;
        break;
    case IRP_MN_REMOVE_DEVICE:
        IoDeleteDevice( device );
        ret = STATUS_SUCCESS;
        break;
        ret = STATUS_SUCCESS;
    default:
        FIXME( "Unhandled minor function %s.\n", debugstr_minor_function_code( stack->MinorFunction ) );
        break;
    }

    irp->IoStatus.Status = ret;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return ret;
}

static NTSTATUS WINAPI bluetooth_pnp( DEVICE_OBJECT *device, IRP *irp )
{
    struct bluetooth_pdo_ext *ext;

    if (device == bus_fdo)
        return fdo_pnp( device, irp );
    else if (device == device_auth)
        return auth_pnp( device, irp );

    ext = device->DeviceExtension;
    switch (ext->type)
    {
    case BLUETOOTH_PDO_EXT_RADIO:
        return radio_pdo_pnp( device, &ext->radio, irp );
    case BLUETOOTH_PDO_EXT_REMOTE_DEVICE:
        return remote_device_pdo_pnp( device, &ext->remote_device, irp );
    case BLUETOOTH_PDO_EXT_GATT_SERVICE:
        return gatt_service_pdo_pnp( device, &ext->gatt_service, irp );
    DEFAULT_UNREACHABLE;
    }
}

/* The remote device a handle on this device object counts against, if any. */
static struct bluetooth_remote_device *handle_owner( DEVICE_OBJECT *device )
{
    struct bluetooth_pdo_ext *ext = device->DeviceExtension;

    if (device == device_auth || device == bus_fdo) return NULL;
    if (ext->type == BLUETOOTH_PDO_EXT_REMOTE_DEVICE) return &ext->remote_device;
    if (ext->type == BLUETOOTH_PDO_EXT_GATT_SERVICE) return ext->gatt_service.remote_device;
    return NULL;
}

static NTSTATUS WINAPI bluetooth_create( DEVICE_OBJECT *device, IRP *irp )
{
    struct bluetooth_remote_device *remote = handle_owner( device );

    if (remote)
    {
        EnterCriticalSection( &remote->props_cs );
        remote->open_handles++;
        LeaveCriticalSection( &remote->props_cs );
    }
    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return STATUS_SUCCESS;
}

/* Windows drops an LE link once the last handle on the device goes away. Without this a client that
 * abandons a device leaves it connected to nobody, and no other client can reach it. */
static NTSTATUS WINAPI bluetooth_close( DEVICE_OBJECT *device, IRP *irp )
{
    struct bluetooth_remote_device *remote = handle_owner( device );
    BOOL drop = FALSE;

    if (remote)
    {
        EnterCriticalSection( &remote->props_cs );
        if (remote->open_handles) remote->open_handles--;
        drop = bluetooth_device_unowned( remote );
        if (drop) winebluetooth_device_dup( remote->device );
        LeaveCriticalSection( &remote->props_cs );
        if (drop)
        {
            TRACE( "Last client of %p closed, disconnecting\n", (void *)remote->device.handle );
            winebluetooth_device_disconnect( remote->device );
            winebluetooth_device_free( remote->device );
        }
    }
    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI driver_add_device( DRIVER_OBJECT *driver, DEVICE_OBJECT *pdo )
{
    NTSTATUS ret;

    TRACE( "(%p, %p)\n", driver, pdo );
    ret = IoCreateDevice( driver, 0, NULL, FILE_DEVICE_BUS_EXTENDER, 0, FALSE, &bus_fdo );
    if (ret != STATUS_SUCCESS)
    {
        ERR( "failed to create FDO: %#lx\n", ret );
        return ret;
    }

    IoAttachDeviceToDeviceStack( bus_fdo, pdo );
    bus_pdo = pdo;
    bus_fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static void WINAPI driver_unload( DRIVER_OBJECT *driver ) {}

NTSTATUS WINAPI DriverEntry( DRIVER_OBJECT *driver, UNICODE_STRING *path )
{
    UNICODE_STRING device_winebth_auth = RTL_CONSTANT_STRING( L"\\Device\\WINEBTHAUTH" );
    UNICODE_STRING object_winebth_auth = RTL_CONSTANT_STRING( WINEBTH_AUTH_DEVICE_PATH );
    NTSTATUS status;

    TRACE( "(%p, %s)\n", driver, debugstr_w( path->Buffer ) );

    status = winebluetooth_init();
    if (status)
        return status;

    driver_obj = driver;

    driver->DriverExtension->AddDevice = driver_add_device;
    driver->DriverUnload = driver_unload;
    driver->MajorFunction[IRP_MJ_CREATE] = bluetooth_create;
    driver->MajorFunction[IRP_MJ_CLOSE] = bluetooth_close;
    driver->MajorFunction[IRP_MJ_PNP] = bluetooth_pnp;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch_bluetooth;

    status = IoCreateDevice( driver, 0, &device_winebth_auth, 0, 0, FALSE, &device_auth );
    if (!status)
    {
        status = IoCreateSymbolicLink( &object_winebth_auth, &device_winebth_auth );
        if (status)
            ERR( "IoCreateSymbolicLink failed: %#lx\n", status );
    }
    else
        ERR( "IoCreateDevice failed: %#lx\n", status );
    return STATUS_SUCCESS;
}
