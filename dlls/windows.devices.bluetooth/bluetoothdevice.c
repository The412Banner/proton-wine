/* BluetoothDevice, BluetoothLEDevice Implementation
 *
 * Copyright 2025 Vibhav Pant
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

#include "private.h"
#include "roapi.h"
#include "setupapi.h"
#include "cfgmgr32.h"
#include "bthdef.h"
#include "winioctl.h"
#include "wine/winebth.h"
#include "initguid.h"
#include "devpkey.h"
#include "propkey.h"
#include "bthledef.h"
#include "ddk/bthguid.h"
#include "bluetoothleapis.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL( bluetooth );

struct bluetoothdevice_statics
{
    IActivationFactory IActivationFactory_iface;
    IBluetoothDeviceStatics IBluetoothDeviceStatics_iface;
    LONG ref;
};

static inline struct bluetoothdevice_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct bluetoothdevice_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct bluetoothdevice_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "(%p, %s, %p) %ld\n", iface, debugstr_guid( iid ), out, impl->ref );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothDeviceStatics))
    {
        IBluetoothDeviceStatics_AddRef(( *out = &impl->IBluetoothDeviceStatics_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct bluetoothdevice_statics *impl = impl_from_IActivationFactory( iface );
    TRACE( "(%p)\n", iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct bluetoothdevice_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p)\n", iface );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    FIXME( "(%p, %p): stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "(%p, %p): stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory */
    factory_ActivateInstance
};

DEFINE_IINSPECTABLE( bluetoothdevice_statics, IBluetoothDeviceStatics, struct bluetoothdevice_statics, IActivationFactory_iface );

static HRESULT WINAPI bluetoothdevice_statics_FromIdAsync( IBluetoothDeviceStatics *iface, HSTRING id, IAsyncOperation_BluetoothDevice **async_op )
{
    FIXME( "(%p, %s, %p): stub!\n", iface, debugstr_hstring( id ), async_op );
    return E_NOTIMPL;
}

static HRESULT WINAPI bluetoothdevice_statics_FromHostNameAsync( IBluetoothDeviceStatics *iface, IHostName *name, IAsyncOperation_BluetoothDevice **async_op )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, name, async_op );
    return E_NOTIMPL;
}

static HRESULT WINAPI bluetoothdevice_statics_FromBluetoothAddressAsync( IBluetoothDeviceStatics *iface,
                                                                         UINT64 address,
                                                                         IAsyncOperation_BluetoothDevice **async_op )
{
    FIXME( "(%p, %#I64x, %p): stub!\n", iface, address, async_op );
    return E_NOTIMPL;
}

static HRESULT WINAPI bluetoothdevice_statics_GetDeviceSelector( IBluetoothDeviceStatics *iface, HSTRING *value )
{
    FIXME( "(%p, %p): stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static const IBluetoothDeviceStaticsVtbl bluetoothdevice_statics_vtbl =
{
    bluetoothdevice_statics_QueryInterface,
    bluetoothdevice_statics_AddRef,
    bluetoothdevice_statics_Release,
    /* IInspectable */
    bluetoothdevice_statics_GetIids,
    bluetoothdevice_statics_GetRuntimeClassName,
    bluetoothdevice_statics_GetTrustLevel,
    /* IBluetoothDeviceStatics */
    bluetoothdevice_statics_FromIdAsync,
    bluetoothdevice_statics_FromHostNameAsync,
    bluetoothdevice_statics_FromBluetoothAddressAsync,
    bluetoothdevice_statics_GetDeviceSelector,
};

struct ble_device
{
    IBluetoothLEDevice IBluetoothLEDevice_iface;
    IBluetoothLEDevice2 IBluetoothLEDevice2_iface;
    IBluetoothLEDevice3 IBluetoothLEDevice3_iface;
    IBluetoothLEDevice4 IBluetoothLEDevice4_iface;
    IBluetoothLEDevice5 IBluetoothLEDevice5_iface;
    IClosable IClosable_iface;
    HSTRING id;
    UINT64 addr;
    HANDLE device;
    LONG ref;

    CRITICAL_SECTION cs;
    HANDLE radio; /* Guarded by cs */
    HCMNOTIFICATION notification; /* Guarded by cs */
    struct
    {
        ITypedEventHandler_BluetoothLEDevice_IInspectable *handler;
        INT64 token;
    } *status_handlers; /* Guarded by cs */
    UINT32 status_handler_count;
    INT64 next_token;
};

/* Caller must hold impl->cs. */
static HANDLE ble_device_get_radio( struct ble_device *impl )
{
    BLUETOOTH_FIND_RADIO_PARAMS params = { .dwSize = sizeof( params ) };
    HBLUETOOTH_RADIO_FIND find;

    if (impl->radio) return impl->radio;
    if (!(find = BluetoothFindFirstRadio( &params, &impl->radio ))) return NULL;
    BluetoothFindRadioClose( find );
    return impl->radio;
}

static BOOL ble_device_is_connected( struct ble_device *impl )
{
    BLUETOOTH_DEVICE_INFO info = { .dwSize = sizeof( info ) };
    HANDLE radio;
    DWORD ret;

    EnterCriticalSection( &impl->cs );
    radio = ble_device_get_radio( impl );
    LeaveCriticalSection( &impl->cs );
    if (!radio) return FALSE;
    info.Address.ullLong = impl->addr;
    if ((ret = BluetoothGetDeviceInfo( radio, &info )))
    {
        WARN( "BluetoothGetDeviceInfo failed: %lu\n", ret );
        return FALSE;
    }
    return info.fConnected;
}

static void ble_device_dispatch_status_changed( struct ble_device *impl )
{
    ITypedEventHandler_BluetoothLEDevice_IInspectable **handlers;
    UINT32 i, count;

    EnterCriticalSection( &impl->cs );
    count = impl->status_handler_count;
    if (!count || !(handlers = malloc( count * sizeof( *handlers ) )))
    {
        LeaveCriticalSection( &impl->cs );
        return;
    }
    for (i = 0; i < count; i++)
        ITypedEventHandler_BluetoothLEDevice_IInspectable_AddRef(( handlers[i] = impl->status_handlers[i].handler ));
    LeaveCriticalSection( &impl->cs );

    for (i = 0; i < count; i++)
    {
        ITypedEventHandler_BluetoothLEDevice_IInspectable_Invoke( handlers[i], &impl->IBluetoothLEDevice_iface, NULL );
        ITypedEventHandler_BluetoothLEDevice_IInspectable_Release( handlers[i] );
    }
    free( handlers );
}

static DWORD CALLBACK ble_device_notify_callback( HCMNOTIFICATION notify, void *ctx, CM_NOTIFY_ACTION action,
                                                  CM_NOTIFY_EVENT_DATA *event_data, DWORD size )
{
    struct ble_device *impl = ctx;
    const BTH_RADIO_IN_RANGE *in_range;

    if (action != CM_NOTIFY_ACTION_DEVICECUSTOMEVENT) return ERROR_SUCCESS;
    if (!IsEqualGUID( &event_data->u.DeviceHandle.EventGuid, &GUID_BLUETOOTH_RADIO_IN_RANGE )) return ERROR_SUCCESS;
    if (event_data->u.DeviceHandle.DataSize < sizeof( *in_range )) return ERROR_SUCCESS;
    in_range = (const BTH_RADIO_IN_RANGE *)event_data->u.DeviceHandle.Data;
    if (in_range->deviceInfo.address != impl->addr) return ERROR_SUCCESS;
    if (!((in_range->deviceInfo.flags ^ in_range->previousDeviceFlags) & BDIF_CONNECTED)) return ERROR_SUCCESS;

    TRACE( "device %p connected %d\n", impl, !!(in_range->deviceInfo.flags & BDIF_CONNECTED) );
    ble_device_dispatch_status_changed( impl );
    return ERROR_SUCCESS;
}

static inline struct ble_device *impl_from_IBluetoothLEDevice( IBluetoothLEDevice *iface )
{
    return CONTAINING_RECORD( iface, struct ble_device, IBluetoothLEDevice_iface );
}

static HRESULT WINAPI ble_device_QueryInterface( IBluetoothLEDevice *iface, REFIID iid, void **out )
{
    struct ble_device *impl = impl_from_IBluetoothLEDevice( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEDevice ))
    {
        IBluetoothLEDevice_AddRef( (*out = &impl->IBluetoothLEDevice_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothLEDevice2 ))
    {
        IBluetoothLEDevice_AddRef( iface );
        *out = &impl->IBluetoothLEDevice2_iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothLEDevice3 ))
    {
        IBluetoothLEDevice_AddRef( iface );
        *out = &impl->IBluetoothLEDevice3_iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothLEDevice4 ))
    {
        IBluetoothLEDevice_AddRef( iface );
        *out = &impl->IBluetoothLEDevice4_iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothLEDevice5 ))
    {
        IBluetoothLEDevice_AddRef( iface );
        *out = &impl->IBluetoothLEDevice5_iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        IBluetoothLEDevice_AddRef( iface );
        *out = &impl->IClosable_iface;
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI ble_device_AddRef( IBluetoothLEDevice *iface )
{
     struct ble_device *impl = impl_from_IBluetoothLEDevice( iface );
     TRACE( "(%p)\n", iface );
     return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI ble_device_Release( IBluetoothLEDevice *iface )
{
    struct ble_device *impl = impl_from_IBluetoothLEDevice( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    TRACE( "(%p)\n", iface );

    if (!ref)
    {
        if (impl->notification) CM_Unregister_Notification( impl->notification );
        for (i = 0; i < impl->status_handler_count; i++)
            ITypedEventHandler_BluetoothLEDevice_IInspectable_Release( impl->status_handlers[i].handler );
        free( impl->status_handlers );
        if (impl->radio) CloseHandle( impl->radio );
        CloseHandle( impl->device );
        WindowsDeleteString( impl->id );
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI ble_device_GetIids( IBluetoothLEDevice *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI ble_device_GetRuntimeClassName( IBluetoothLEDevice *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI ble_device_GetTrustLevel( IBluetoothLEDevice *iface, TrustLevel *level )
{
    FIXME( "(%p, %p): stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI ble_device_get_DeviceId( IBluetoothLEDevice *iface, HSTRING *value )
{
    struct ble_device *impl = impl_from_IBluetoothLEDevice( iface );
    TRACE( "(%p, %p)\n", iface, value );
    return WindowsDuplicateString( impl->id, value );
}

static HRESULT WINAPI ble_device_get_Name( IBluetoothLEDevice *iface, HSTRING *value )
{
    struct ble_device *impl = impl_from_IBluetoothLEDevice( iface );
    BLUETOOTH_DEVICE_INFO info = { .dwSize = sizeof( info ) };
    HANDLE radio;

    TRACE( "(%p, %p)\n", iface, value );

    EnterCriticalSection( &impl->cs );
    radio = ble_device_get_radio( impl );
    LeaveCriticalSection( &impl->cs );
    info.Address.ullLong = impl->addr;
    if (!radio || BluetoothGetDeviceInfo( radio, &info ))
        return WindowsCreateString( NULL, 0, value );
    return WindowsCreateString( info.szName, wcslen( info.szName ), value );
}

HRESULT ble_device_get_services_vector( IBluetoothLEDevice *device, const GUID *uuid, IVector_IInspectable **out )
{
    static const struct vector_iids iids = {
        .vector = &IID_IVector_IInspectable,
        .view = &IID_IVectorView_GattDeviceService,
        .iterable = &IID_IIterable_GattDeviceService,
        .iterator = &IID_IIterator_GattDeviceService,
    };
    struct ble_device *impl = impl_from_IBluetoothLEDevice( device );
    BTH_LE_GATT_SERVICE *buf = NULL;
    IVector_IInspectable *vector;
    USHORT actual = 0, i;
    HRESULT hr;

    *out = NULL;
    if (FAILED(hr = vector_create( &iids, (void **)&vector ))) return hr;

    hr = BluetoothGATTGetServices( impl->device, 0, NULL, &actual, 0 );
    if (SUCCEEDED( hr ) || hr != HRESULT_FROM_WIN32( ERROR_MORE_DATA )) goto done;
    for (;;)
    {
        UINT32 size = actual;
        void *tmp;

        if (!(tmp = realloc( buf, sizeof( *buf ) * size )))
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        buf = tmp;
        if (SUCCEEDED(hr = BluetoothGATTGetServices( impl->device, size, buf, &actual, 0 ))) break;
        if (hr != HRESULT_FROM_WIN32( ERROR_INVALID_USER_BUFFER )) goto done;
    }
    for (i = 0; i < actual; i++)
    {
        IGattDeviceService *service;
        GUID svc_uuid;

        if (uuid)
        {
            if (buf[i].ServiceUuid.IsShortUuid)
            {
                svc_uuid = BTH_LE_ATT_BLUETOOTH_BASE_GUID;
                svc_uuid.Data1 = buf[i].ServiceUuid.Value.ShortUuid;
            }
            else
                svc_uuid = buf[i].ServiceUuid.Value.LongUuid;
            if (!IsEqualGUID( uuid, &svc_uuid )) continue;
        }
        if (FAILED(hr = gatt_service_create( &buf[i], impl->device, impl->addr, device, &service ))) goto done;
        hr = IVector_IInspectable_Append( vector, (IInspectable *)service );
        IGattDeviceService_Release( service );
        if (FAILED( hr )) goto done;
    }
done:
    free( buf );
    if (FAILED( hr ))
    {
        WARN( "Failed to enumerate services: %#lx\n", hr );
        IVector_IInspectable_Release( vector );
        return hr;
    }
    *out = vector;
    return S_OK;
}

static HRESULT WINAPI ble_device_get_GattServices( IBluetoothLEDevice *iface, IVectorView_GattDeviceService **services )
{
    IVector_IInspectable *vector;
    HRESULT hr;

    TRACE( "(%p, %p)\n", iface, services );

    *services = NULL;
    if (FAILED(hr = ble_device_get_services_vector( iface, NULL, &vector ))) return hr;
    hr = IVector_IInspectable_GetView( vector, (IVectorView_IInspectable **)services );
    IVector_IInspectable_Release( vector );
    return hr;
}

static HRESULT WINAPI ble_device_get_ConnectionStatus( IBluetoothLEDevice *iface, BluetoothConnectionStatus *value )
{
    struct ble_device *impl = impl_from_IBluetoothLEDevice( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = ble_device_is_connected( impl ) ? BluetoothConnectionStatus_Connected : BluetoothConnectionStatus_Disconnected;
    return S_OK;
}

static HRESULT WINAPI ble_device_get_BluetoothAddress( IBluetoothLEDevice *iface, UINT64 *value )
{
    struct ble_device *impl = impl_from_IBluetoothLEDevice( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->addr;
    return S_OK;
}

static HRESULT WINAPI ble_device_GetGattService( IBluetoothLEDevice *iface, GUID uuid, IGattDeviceService **service )
{
    FIXME( "(%p, %s, %p): stub!\n", iface, debugstr_guid( &uuid ), service );
    return E_NOTIMPL;
}

static HRESULT WINAPI ble_device_add_NameChanged( IBluetoothLEDevice *iface, ITypedEventHandler_BluetoothLEDevice_IInspectable *handler,
                                                  EventRegistrationToken *token )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, handler, token );
    return E_NOTIMPL;
}

static HRESULT WINAPI ble_device_remove_NameChanged( IBluetoothLEDevice *iface, EventRegistrationToken token )
{
    FIXME( "(%p, %I64d): stub!\n", iface, token.value );
    return E_NOTIMPL;
}

static HRESULT WINAPI ble_device_add_GattServicesChanged( IBluetoothLEDevice *iface, ITypedEventHandler_BluetoothLEDevice_IInspectable *handler,
                                                          EventRegistrationToken *token )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, handler, token );
    return E_NOTIMPL;
}

static HRESULT WINAPI ble_device_remove_GattServicesChanged( IBluetoothLEDevice *iface, EventRegistrationToken token )
{
    FIXME( "(%p, %I64d): stub!\n", iface, token.value );
    return E_NOTIMPL;
}

static HRESULT WINAPI ble_device_add_ConnectionStatusChanged( IBluetoothLEDevice *iface, ITypedEventHandler_BluetoothLEDevice_IInspectable *handler,
                                                              EventRegistrationToken *token )
{
    struct ble_device *impl = impl_from_IBluetoothLEDevice( iface );
    CM_NOTIFY_FILTER filter = { .cbSize = sizeof( filter ), .FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE };
    void *tmp;

    TRACE( "(%p, %p, %p)\n", iface, handler, token );

    if (!handler) return E_INVALIDARG;
    EnterCriticalSection( &impl->cs );
    if (!(tmp = realloc( impl->status_handlers, (impl->status_handler_count + 1) * sizeof( *impl->status_handlers ) )))
    {
        LeaveCriticalSection( &impl->cs );
        return E_OUTOFMEMORY;
    }
    impl->status_handlers = tmp;
    ITypedEventHandler_BluetoothLEDevice_IInspectable_AddRef( handler );
    impl->status_handlers[impl->status_handler_count].handler = handler;
    impl->status_handlers[impl->status_handler_count].token = token->value = ++impl->next_token;
    impl->status_handler_count++;
    if (!impl->notification && ble_device_get_radio( impl ))
    {
        CONFIGRET ret;
        filter.u.DeviceHandle.hTarget = impl->radio;
        if ((ret = CM_Register_Notification( &filter, impl, ble_device_notify_callback, &impl->notification )))
            ERR( "CM_Register_Notification failed: %#lx\n", ret );
    }
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI ble_device_remove_ConnectionStatusChanged( IBluetoothLEDevice *iface, EventRegistrationToken token )
{
    struct ble_device *impl = impl_from_IBluetoothLEDevice( iface );
    ITypedEventHandler_BluetoothLEDevice_IInspectable *handler = NULL;
    UINT32 i;

    TRACE( "(%p, %I64x)\n", iface, token.value );

    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->status_handler_count; i++)
    {
        if (impl->status_handlers[i].token != token.value) continue;
        handler = impl->status_handlers[i].handler;
        memmove( &impl->status_handlers[i], &impl->status_handlers[i + 1],
                 (impl->status_handler_count - i - 1) * sizeof( *impl->status_handlers ) );
        impl->status_handler_count--;
        break;
    }
    LeaveCriticalSection( &impl->cs );
    if (handler) ITypedEventHandler_BluetoothLEDevice_IInspectable_Release( handler );
    return S_OK;
}

static const IBluetoothLEDeviceVtbl ble_device_vtbl = {
    /* IUnknown */
    ble_device_QueryInterface,
    ble_device_AddRef,
    ble_device_Release,
    /* IInspectable */
    ble_device_GetIids,
    ble_device_GetRuntimeClassName,
    ble_device_GetTrustLevel,
    /* IBluetoothLEDevice */
    ble_device_get_DeviceId,
    ble_device_get_Name,
    ble_device_get_GattServices,
    ble_device_get_ConnectionStatus,
    ble_device_get_BluetoothAddress,
    ble_device_GetGattService,
    ble_device_add_NameChanged,
    ble_device_remove_NameChanged,
    ble_device_add_GattServicesChanged,
    ble_device_remove_GattServicesChanged,
    ble_device_add_ConnectionStatusChanged,
    ble_device_remove_ConnectionStatusChanged,
};

DEFINE_IINSPECTABLE_( ble_device_closable, IClosable, struct ble_device, ble_device_from_IClosable, IClosable_iface,
                      &impl->IBluetoothLEDevice_iface )

static HRESULT WINAPI ble_device_closable_Close( IClosable *iface )
{
    TRACE( "(%p)\n", iface );
    return S_OK;
}

static const IClosableVtbl ble_device_closable_vtbl =
{
    ble_device_closable_QueryInterface,
    ble_device_closable_AddRef,
    ble_device_closable_Release,
    ble_device_closable_GetIids,
    ble_device_closable_GetRuntimeClassName,
    ble_device_closable_GetTrustLevel,
    ble_device_closable_Close,
};

DEFINE_IINSPECTABLE( ble_device2, IBluetoothLEDevice2, struct ble_device, IBluetoothLEDevice_iface )

/* Minimal DeviceInformation for the LE device, since windows.devices.enumeration cannot create one from an id yet. */
struct device_information
{
    IDeviceInformation IDeviceInformation_iface;
    LONG ref;
    HSTRING id;
    HSTRING name;
};

static inline struct device_information *impl_from_IDeviceInformation( IDeviceInformation *iface )
{
    return CONTAINING_RECORD( iface, struct device_information, IDeviceInformation_iface );
}

static HRESULT WINAPI device_information_QueryInterface( IDeviceInformation *iface, REFIID iid, void **out )
{
    struct device_information *impl = impl_from_IDeviceInformation( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDeviceInformation ))
    {
        IDeviceInformation_AddRef(( *out = &impl->IDeviceInformation_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI device_information_AddRef( IDeviceInformation *iface )
{
    struct device_information *impl = impl_from_IDeviceInformation( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI device_information_Release( IDeviceInformation *iface )
{
    struct device_information *impl = impl_from_IDeviceInformation( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        WindowsDeleteString( impl->id );
        WindowsDeleteString( impl->name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI device_information_GetIids( IDeviceInformation *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI device_information_GetRuntimeClassName( IDeviceInformation *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Enumeration.DeviceInformation", class_name );
}

static HRESULT WINAPI device_information_GetTrustLevel( IDeviceInformation *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI device_information_get_Id( IDeviceInformation *iface, HSTRING *value )
{
    struct device_information *impl = impl_from_IDeviceInformation( iface );
    TRACE( "(%p, %p)\n", iface, value );
    return WindowsDuplicateString( impl->id, value );
}

static HRESULT WINAPI device_information_get_Name( IDeviceInformation *iface, HSTRING *value )
{
    struct device_information *impl = impl_from_IDeviceInformation( iface );
    TRACE( "(%p, %p)\n", iface, value );
    return WindowsDuplicateString( impl->name, value );
}

static HRESULT WINAPI device_information_get_IsEnabled( IDeviceInformation *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI device_information_get_IsDefault( IDeviceInformation *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI device_information_get_EnclosureLocation( IDeviceInformation *iface, IEnclosureLocation **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI device_information_get_Properties( IDeviceInformation *iface, IMapView_HSTRING_IInspectable **value )
{
    FIXME( "(%p, %p): stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI device_information_Update( IDeviceInformation *iface, IDeviceInformationUpdate *update )
{
    FIXME( "(%p, %p): stub!\n", iface, update );
    return E_NOTIMPL;
}

static HRESULT WINAPI device_information_GetThumbnailAsync( IDeviceInformation *iface, IAsyncOperation_DeviceThumbnail **op )
{
    FIXME( "(%p, %p): stub!\n", iface, op );
    return E_NOTIMPL;
}

static HRESULT WINAPI device_information_GetGlyphThumbnailAsync( IDeviceInformation *iface, IAsyncOperation_DeviceThumbnail **op )
{
    FIXME( "(%p, %p): stub!\n", iface, op );
    return E_NOTIMPL;
}

static const IDeviceInformationVtbl device_information_vtbl =
{
    device_information_QueryInterface,
    device_information_AddRef,
    device_information_Release,
    device_information_GetIids,
    device_information_GetRuntimeClassName,
    device_information_GetTrustLevel,
    device_information_get_Id,
    device_information_get_Name,
    device_information_get_IsEnabled,
    device_information_get_IsDefault,
    device_information_get_EnclosureLocation,
    device_information_get_Properties,
    device_information_Update,
    device_information_GetThumbnailAsync,
    device_information_GetGlyphThumbnailAsync,
};

static HRESULT WINAPI ble_device2_get_DeviceInformation( IBluetoothLEDevice2 *iface, IDeviceInformation **value )
{
    struct ble_device *impl = impl_from_IBluetoothLEDevice2( iface );
    struct device_information *info;
    HRESULT hr;

    TRACE( "(%p, %p)\n", iface, value );

    if (!(info = calloc( 1, sizeof( *info ) ))) return E_OUTOFMEMORY;
    info->IDeviceInformation_iface.lpVtbl = &device_information_vtbl;
    info->ref = 1;
    if (FAILED((hr = WindowsDuplicateString( impl->id, &info->id ))) ||
        FAILED((hr = IBluetoothLEDevice_get_Name( &impl->IBluetoothLEDevice_iface, &info->name ))))
    {
        IDeviceInformation_Release( &info->IDeviceInformation_iface );
        return hr;
    }
    *value = &info->IDeviceInformation_iface;
    return S_OK;
}

static HRESULT WINAPI ble_device2_get_Appearance( IBluetoothLEDevice2 *iface, IBluetoothLEAppearance **value )
{
    FIXME( "(%p, %p): semi-stub!\n", iface, value );
    *value = NULL;
    return S_OK;
}

/* The driver records the address type BlueZ reported on the device node. */
static HRESULT WINAPI ble_device2_get_BluetoothAddressType( IBluetoothLEDevice2 *iface, BluetoothAddressType *value )
{
    struct ble_device *impl = impl_from_IBluetoothLEDevice2( iface );
    SP_DEVICE_INTERFACE_DATA iface_data = { .cbSize = sizeof( iface_data ) };
    SP_DEVINFO_DATA devinfo_data = { .cbSize = sizeof( devinfo_data ) };
    BYTE addr_type = 0;
    DEVPROPTYPE type;
    HDEVINFO devinfo;

    TRACE( "(%p, %p)\n", iface, value );

    *value = BluetoothAddressType_Public;
    devinfo = SetupDiCreateDeviceInfoList( NULL, NULL );
    if (devinfo == INVALID_HANDLE_VALUE) return S_OK;
    if (SetupDiOpenDeviceInterfaceW( devinfo, WindowsGetStringRawBuffer( impl->id, NULL ), 0, &iface_data ) &&
        (SetupDiGetDeviceInterfaceDetailW( devinfo, &iface_data, NULL, 0, NULL, &devinfo_data ) ||
         GetLastError() == ERROR_INSUFFICIENT_BUFFER) &&
        SetupDiGetDevicePropertyW( devinfo, &devinfo_data, (DEVPROPKEY *)&PKEY_Devices_Aep_Bluetooth_Le_AddressType,
                                   &type, &addr_type, sizeof( addr_type ), NULL, 0 ) &&
        type == DEVPROP_TYPE_BYTE && addr_type == 1)
        *value = BluetoothAddressType_Random;
    else
        WARN( "no address type recorded for %s, assuming public\n", debugstr_hstring( impl->id ) );
    SetupDiDestroyDeviceInfoList( devinfo );
    return S_OK;
}

static const IBluetoothLEDevice2Vtbl ble_device2_vtbl =
{
    ble_device2_QueryInterface,
    ble_device2_AddRef,
    ble_device2_Release,
    ble_device2_GetIids,
    ble_device2_GetRuntimeClassName,
    ble_device2_GetTrustLevel,
    ble_device2_get_DeviceInformation,
    ble_device2_get_Appearance,
    ble_device2_get_BluetoothAddressType,
};

DEFINE_IINSPECTABLE( ble_device3, IBluetoothLEDevice3, struct ble_device, IBluetoothLEDevice_iface )

static HRESULT WINAPI ble_device3_get_DeviceAccessInformation( IBluetoothLEDevice3 *iface, IDeviceAccessInformation **value )
{
    FIXME( "(%p, %p): stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT ble_device_access_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_UI4;
    result->ulVal = DeviceAccessStatus_Allowed;
    return S_OK;
}

static HRESULT WINAPI ble_device3_RequestAccessAsync( IBluetoothLEDevice3 *iface, IAsyncOperation_DeviceAccessStatus **async )
{
    TRACE( "(%p, %p)\n", iface, async );
    return async_operation_uint32_create( &IID_IAsyncOperation_DeviceAccessStatus, (IUnknown *)iface, NULL,
                                          ble_device_access_async, (IAsyncOperation_IInspectable **)async );
}

static HRESULT ble_device_get_services_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct ble_device *impl = impl_from_IBluetoothLEDevice3( (IBluetoothLEDevice3 *)invoker );
    IGattDeviceServicesResult *services_result;
    IVector_IInspectable *vector = NULL;
    GUID uuid, *filter = NULL;
    HRESULT hr;

    if (param)
    {
        IPropertyValue *value;
        if (FAILED((hr = IUnknown_QueryInterface( param, &IID_IPropertyValue, (void **)&value )))) return hr;
        hr = IPropertyValue_GetGuid( value, &uuid );
        IPropertyValue_Release( value );
        if (FAILED(hr)) return hr;
        filter = &uuid;
    }
    ble_device_get_services_vector( &impl->IBluetoothLEDevice_iface, filter, &vector );
    hr = gatt_device_services_result_create( vector, &services_result );
    if (vector) IVector_IInspectable_Release( vector );
    if (FAILED(hr)) return hr;
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)services_result;
    return S_OK;
}

static HRESULT ble_device_start_services_async( IBluetoothLEDevice3 *iface, const GUID *uuid,
                                                IAsyncOperation_GattDeviceServicesResult **async )
{
    static const WCHAR *class_name = RuntimeClass_Windows_Foundation_PropertyValue;
    IInspectable *param = NULL;
    HRESULT hr;

    if (uuid)
    {
        IPropertyValueStatics *statics;
        HSTRING_HEADER hdr;
        HSTRING str;

        if (FAILED(hr = WindowsCreateStringReference( class_name, wcslen( class_name ), &hdr, &str ))) return hr;
        if (FAILED(hr = RoGetActivationFactory( str, &IID_IPropertyValueStatics, (void **)&statics ))) return hr;
        hr = IPropertyValueStatics_CreateGuid( statics, *uuid, &param );
        IPropertyValueStatics_Release( statics );
        if (FAILED(hr)) return hr;
    }
    hr = async_operation_inspectable_create( &IID_IAsyncOperation_GattDeviceServicesResult, (IUnknown *)iface,
                                             (IUnknown *)param, ble_device_get_services_async,
                                             (IAsyncOperation_IInspectable **)async );
    if (param) IInspectable_Release( param );
    return hr;
}

static HRESULT WINAPI ble_device3_GetGattServicesAsync( IBluetoothLEDevice3 *iface, IAsyncOperation_GattDeviceServicesResult **async )
{
    TRACE( "(%p, %p)\n", iface, async );
    return ble_device_start_services_async( iface, NULL, async );
}

static HRESULT WINAPI ble_device3_GetGattServicesWithCacheModeAsync( IBluetoothLEDevice3 *iface, BluetoothCacheMode mode,
                                                                     IAsyncOperation_GattDeviceServicesResult **async )
{
    TRACE( "(%p, %d, %p)\n", iface, mode, async );
    return ble_device_start_services_async( iface, NULL, async );
}

static HRESULT WINAPI ble_device3_GetGattServicesForUuidAsync( IBluetoothLEDevice3 *iface, GUID uuid,
                                                               IAsyncOperation_GattDeviceServicesResult **async )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( &uuid ), async );
    return ble_device_start_services_async( iface, &uuid, async );
}

static HRESULT WINAPI ble_device3_GetGattServicesForUuidWithCacheModeAsync( IBluetoothLEDevice3 *iface, GUID uuid,
                                                                            BluetoothCacheMode mode,
                                                                            IAsyncOperation_GattDeviceServicesResult **async )
{
    TRACE( "(%p, %s, %d, %p)\n", iface, debugstr_guid( &uuid ), mode, async );
    return ble_device_start_services_async( iface, &uuid, async );
}

static const IBluetoothLEDevice3Vtbl ble_device3_vtbl =
{
    ble_device3_QueryInterface,
    ble_device3_AddRef,
    ble_device3_Release,
    ble_device3_GetIids,
    ble_device3_GetRuntimeClassName,
    ble_device3_GetTrustLevel,
    ble_device3_get_DeviceAccessInformation,
    ble_device3_RequestAccessAsync,
    ble_device3_GetGattServicesAsync,
    ble_device3_GetGattServicesWithCacheModeAsync,
    ble_device3_GetGattServicesForUuidAsync,
    ble_device3_GetGattServicesForUuidWithCacheModeAsync,
};

DEFINE_IINSPECTABLE( ble_device4, IBluetoothLEDevice4, struct ble_device, IBluetoothLEDevice_iface )

static HRESULT WINAPI ble_device4_get_BluetoothDeviceId( IBluetoothLEDevice4 *iface, IBluetoothDeviceId **value )
{
    struct ble_device *impl = impl_from_IBluetoothLEDevice4( iface );
    TRACE( "(%p, %p)\n", iface, value );
    return bluetoothdeviceid_create( impl->id, value );
}

static const IBluetoothLEDevice4Vtbl ble_device4_vtbl =
{
    ble_device4_QueryInterface,
    ble_device4_AddRef,
    ble_device4_Release,
    ble_device4_GetIids,
    ble_device4_GetRuntimeClassName,
    ble_device4_GetTrustLevel,
    ble_device4_get_BluetoothDeviceId,
};

DEFINE_IINSPECTABLE( ble_device5, IBluetoothLEDevice5, struct ble_device, IBluetoothLEDevice_iface )

static HRESULT WINAPI ble_device5_get_WasSecureConnectionUsedForPairing( IBluetoothLEDevice5 *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = FALSE;
    return S_OK;
}

static const IBluetoothLEDevice5Vtbl ble_device5_vtbl =
{
    ble_device5_QueryInterface,
    ble_device5_AddRef,
    ble_device5_Release,
    ble_device5_GetIids,
    ble_device5_GetRuntimeClassName,
    ble_device5_GetTrustLevel,
    ble_device5_get_WasSecureConnectionUsedForPairing,
};

static HRESULT ble_device_create( IBluetoothLEDevice **device, const WCHAR *id, UINT64 addr )
{
    struct ble_device *impl;
    HRESULT hr;

    TRACE( "(%p, %s, %I64x)\n", device, debugstr_w( id ), addr );

    if (!(impl = calloc( 1, sizeof( *impl ))))
        return E_OUTOFMEMORY;
    if (FAILED(hr = WindowsCreateString( id, wcslen( id ), &impl->id )))
    {
        free( impl );
        return hr;
    }
    impl->device = CreateFileW( id, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    if (impl->device == INVALID_HANDLE_VALUE)
    {
        WindowsDeleteString( impl->id );
        free( impl );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    impl->ref = 1;
    impl->addr = addr;
    impl->IBluetoothLEDevice_iface.lpVtbl = &ble_device_vtbl;
    impl->IBluetoothLEDevice2_iface.lpVtbl = &ble_device2_vtbl;
    impl->IBluetoothLEDevice3_iface.lpVtbl = &ble_device3_vtbl;
    impl->IBluetoothLEDevice4_iface.lpVtbl = &ble_device4_vtbl;
    impl->IBluetoothLEDevice5_iface.lpVtbl = &ble_device5_vtbl;
    impl->IClosable_iface.lpVtbl = &ble_device_closable_vtbl;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    *device = &impl->IBluetoothLEDevice_iface;
    return S_OK;
}

/* Look up the driver's device node for an LE address. */
static BOOL find_le_device( UINT64 addr, WCHAR *path, DWORD path_size )
{
    char buffer[sizeof( SP_DEVICE_INTERFACE_DETAIL_DATA_W ) + MAX_PATH * sizeof( WCHAR )];
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *iface_detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buffer;
    SP_DEVICE_INTERFACE_DATA iface_data = { .cbSize = sizeof( iface_data ) };
    BOOL found = FALSE;
    HDEVINFO devinfo;
    DWORD idx = 0;

    devinfo = SetupDiGetClassDevsW( &GUID_BLUETOOTHLE_DEVICE_INTERFACE, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE );
    if (devinfo == INVALID_HANDLE_VALUE) return FALSE;

    iface_detail->cbSize = sizeof( *iface_detail );
    while (SetupDiEnumDeviceInterfaces( devinfo, NULL, &GUID_BLUETOOTHLE_DEVICE_INTERFACE, idx++, &iface_data ))
    {
        SP_DEVINFO_DATA devinfo_data = { .cbSize = sizeof( devinfo_data ) };
        WCHAR addr_str[13];
        DEVPROPTYPE type;
        UINT64 addr2 = 0;

        if (!SetupDiGetDeviceInterfaceDetailW( devinfo, &iface_data, iface_detail, sizeof( buffer ), NULL, &devinfo_data ))
            continue;
        if (!SetupDiGetDevicePropertyW( devinfo, &devinfo_data, &DEVPKEY_Bluetooth_DeviceAddress, &type, (BYTE *)addr_str, sizeof( addr_str ), NULL, 0 ))
            continue;
        if (swscanf( addr_str, L"%I64x", &addr2 ) != 1)
            continue;
        if (addr == addr2)
        {
            found = TRUE;
            lstrcpynW( path, iface_detail->DevicePath, path_size );
            break;
        }
    }
    SetupDiDestroyDeviceInfoList( devinfo );
    return found;
}

static HRESULT bluetoothledevice_get_device_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct winebth_radio_start_discovery_params discovery = { .le = 1 };
    BLUETOOTH_FIND_RADIO_PARAMS radio_params = { .dwSize = sizeof( radio_params ) };
    HBLUETOOTH_RADIO_FIND find;
    HANDLE radio = NULL;
    WCHAR path[MAX_PATH];
    IBluetoothLEDevice *device;
    BOOL found, scanning = FALSE;
    UINT64 addr;
    HRESULT hr;
    DWORD bytes;
    int i;

    if (!called_async) return STATUS_PENDING;
    if (FAILED(hr = IPropertyValue_GetUInt64( (IPropertyValue *)param, &addr )))
        return hr;

    result->vt = VT_NULL;

    /* BlueZ forgets unpaired devices soon after scanning stops. Windows scans on demand until the device is
     * heard again, so do the same for a while. */
    for (i = 0; !(found = find_le_device( addr, path, ARRAY_SIZE( path ) )) && i < 40; i++)
    {
        if (!scanning)
        {
            if ((find = BluetoothFindFirstRadio( &radio_params, &radio )))
            {
                BluetoothFindRadioClose( find );
                scanning = DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_START_DISCOVERY, &discovery, sizeof( discovery ),
                                            NULL, 0, &bytes, NULL );
                if (!scanning) WARN( "Failed to start discovery: %lu\n", GetLastError() );
            }
            if (!scanning) break;
            TRACE( "Device %#I64x not present, scanning for it\n", addr );
        }
        Sleep( 500 );
    }
    if (scanning)
        DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_STOP_DISCOVERY, NULL, 0, NULL, 0, &bytes, NULL );
    if (radio) CloseHandle( radio );

    if (!found)
    {
        WARN( "Device %#I64x not found\n", addr );
        return S_OK;
    }
    if (FAILED(hr = ble_device_create( &device, path, addr )))
        return hr;
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)device;
    return S_OK;
}

struct bluetoothledevice_statics
{
    IActivationFactory IActivationFactory_iface;
    IBluetoothLEDeviceStatics IBluetoothLEDeviceStatics_iface;
    LONG ref;
};

static inline struct bluetoothledevice_statics *ble_device_impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct bluetoothledevice_statics, IActivationFactory_iface );
}

static HRESULT WINAPI ble_device_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct bluetoothledevice_statics *impl = ble_device_impl_from_IActivationFactory( iface );

    TRACE( "(%p, %s, %p) %ld\n", iface, debugstr_guid( iid ), out, impl->ref );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothLEDeviceStatics ))
    {
        IBluetoothLEDeviceStatics_AddRef(( *out = &impl->IBluetoothLEDeviceStatics_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI ble_device_factory_AddRef( IActivationFactory *iface )
{
    struct bluetoothledevice_statics *impl = ble_device_impl_from_IActivationFactory( iface );
    TRACE( "(%p)\n", iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI ble_device_factory_Release( IActivationFactory *iface )
{
    struct bluetoothledevice_statics *impl = ble_device_impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p)\n", iface );
    return ref;
}

static HRESULT WINAPI ble_device_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI ble_device_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI ble_device_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    FIXME( "(%p, %p): stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI ble_device_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "(%p, %p): stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl ble_device_factory_vtbl =
{
    ble_device_factory_QueryInterface,
    ble_device_factory_AddRef,
    ble_device_factory_Release,
    /* IInspectable */
    ble_device_factory_GetIids,
    ble_device_factory_GetRuntimeClassName,
    ble_device_factory_GetTrustLevel,
    /* IActivationFactory */
    ble_device_factory_ActivateInstance
};

DEFINE_IINSPECTABLE( bluetoothledevice_statics, IBluetoothLEDeviceStatics, struct bluetoothledevice_statics, IActivationFactory_iface );

static HRESULT WINAPI bluetoothledevice_statics_FromIdAsync( IBluetoothLEDeviceStatics *iface, HSTRING id, IAsyncOperation_BluetoothLEDevice **async_op )
{
    FIXME( "(%p, %s, %p): stub!\n", iface, debugstr_hstring( id ), async_op );
    return E_NOTIMPL;
}

static HRESULT WINAPI bluetoothledevice_statics_FromBluetoothAddressAsync( IBluetoothLEDeviceStatics *iface,
                                                                           UINT64 addr,
                                                                           IAsyncOperation_BluetoothLEDevice **async_op )
{
    static const WCHAR *class_name = RuntimeClass_Windows_Foundation_PropertyValue;
    IPropertyValueStatics *statics;
    IPropertyValue *addr_val;
    HSTRING_HEADER hdr;
    HSTRING str;
    HRESULT hr;

    TRACE( "(%p, %#I64x, %p)\n", iface, addr, async_op );

    if (FAILED(hr = WindowsCreateStringReference( class_name, wcslen( class_name ), &hdr, &str ))) return hr;
    if (FAILED(hr = RoGetActivationFactory( str, &IID_IPropertyValueStatics, (void **)&statics ))) return hr;
    if (FAILED(hr = IPropertyValueStatics_CreateUInt64( statics, addr, (IInspectable **)&addr_val )))
    {
        IPropertyValueStatics_Release( statics );
        return hr;
    }
    IPropertyValueStatics_Release( statics );

    return async_operation_inspectable_create( &IID_IAsyncOperation_BluetoothLEDevice, (IUnknown *)iface, (IUnknown *)addr_val, bluetoothledevice_get_device_async,
                                               (IAsyncOperation_IInspectable **)async_op );
}

static HRESULT WINAPI bluetoothledevice_statics_GetDeviceSelector( IBluetoothLEDeviceStatics *iface, HSTRING *result )
{
    FIXME( "(%p, %p): stub!\n", iface, result );
    return E_NOTIMPL;
}

static const IBluetoothLEDeviceStaticsVtbl bluetoothledevice_statics_vtbl =
{
    /* IUnknown */
    bluetoothledevice_statics_QueryInterface,
    bluetoothledevice_statics_AddRef,
    bluetoothledevice_statics_Release,
    /* IInspectable */
    bluetoothledevice_statics_GetIids,
    bluetoothledevice_statics_GetRuntimeClassName,
    bluetoothledevice_statics_GetTrustLevel,
    /* IBluetoothLEDeviceStatics */
    bluetoothledevice_statics_FromIdAsync,
    bluetoothledevice_statics_FromBluetoothAddressAsync,
    bluetoothledevice_statics_GetDeviceSelector
};


static struct bluetoothdevice_statics bluetoothdevice_statics =
{
    {&factory_vtbl},
    {&bluetoothdevice_statics_vtbl},
    1
};

IActivationFactory *bluetoothdevice_statics_factory = &bluetoothdevice_statics.IActivationFactory_iface;

static struct bluetoothledevice_statics bluetoothledevice_statics =
{
    {&ble_device_factory_vtbl},
    {&bluetoothledevice_statics_vtbl},
    1
};

IActivationFactory *bluetoothledevice_statics_factory = &bluetoothledevice_statics.IActivationFactory_iface;
