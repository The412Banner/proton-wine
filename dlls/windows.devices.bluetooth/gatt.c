/* Windows.Devices.Bluetooth.GenericAttributeProfile Implementation
 *
 * Copyright 2025 Vibhav Pant
 * Copyright 2026 Evan Thomas
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
#include "bthledef.h"
#include "bluetoothleapis.h"
#include "winioctl.h"
#include "setupapi.h"
#include "cfgmgr32.h"
#include "devpkey.h"
#include "ddk/bthguid.h"
#include "roapi.h"
#include "wine/winebth.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL( bluetooth );

/* --- Shared helpers --- */

struct event_handler
{
    IUnknown *handler;
    INT64 token;
};

struct event_handlers
{
    CRITICAL_SECTION cs;
    struct event_handler *entries;
    UINT32 count;
    UINT32 capacity;
    INT64 next_token;
};

static void event_handlers_init( struct event_handlers *handlers )
{
    memset( handlers, 0, sizeof( *handlers ) );
    InitializeCriticalSectionEx( &handlers->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
}

static void event_handlers_free( struct event_handlers *handlers )
{
    UINT32 i;
    for (i = 0; i < handlers->count; i++) IUnknown_Release( handlers->entries[i].handler );
    free( handlers->entries );
    DeleteCriticalSection( &handlers->cs );
}

static HRESULT event_handlers_add( struct event_handlers *handlers, IUnknown *handler, EventRegistrationToken *token )
{
    struct event_handler *entry;

    if (!handler) return E_INVALIDARG;
    EnterCriticalSection( &handlers->cs );
    if (handlers->count == handlers->capacity)
    {
        UINT32 capacity = max( 4, handlers->capacity * 2 );
        if (!(entry = realloc( handlers->entries, capacity * sizeof( *entry ) )))
        {
            LeaveCriticalSection( &handlers->cs );
            return E_OUTOFMEMORY;
        }
        handlers->entries = entry;
        handlers->capacity = capacity;
    }
    entry = &handlers->entries[handlers->count++];
    entry->handler = handler;
    IUnknown_AddRef( handler );
    entry->token = token->value = ++handlers->next_token;
    LeaveCriticalSection( &handlers->cs );
    return S_OK;
}

static HRESULT event_handlers_remove( struct event_handlers *handlers, EventRegistrationToken token )
{
    IUnknown *handler = NULL;
    UINT32 i;

    EnterCriticalSection( &handlers->cs );
    for (i = 0; i < handlers->count; i++)
    {
        if (handlers->entries[i].token != token.value) continue;
        handler = handlers->entries[i].handler;
        memmove( &handlers->entries[i], &handlers->entries[i + 1], (handlers->count - i - 1) * sizeof( *handlers->entries ) );
        handlers->count--;
        break;
    }
    LeaveCriticalSection( &handlers->cs );
    if (handler) IUnknown_Release( handler );
    return S_OK;
}

static UINT32 event_handlers_snapshot( struct event_handlers *handlers, IUnknown ***out )
{
    UINT32 i, count;

    EnterCriticalSection( &handlers->cs );
    count = handlers->count;
    if (!count || !(*out = malloc( count * sizeof( **out ) )))
    {
        LeaveCriticalSection( &handlers->cs );
        return 0;
    }
    for (i = 0; i < count; i++)
    {
        (*out)[i] = handlers->entries[i].handler;
        IUnknown_AddRef( (*out)[i] );
    }
    LeaveCriticalSection( &handlers->cs );
    return count;
}

static void le_uuid_to_guid( const BTH_LE_UUID *le_uuid, GUID *uuid )
{
    if (le_uuid->IsShortUuid)
    {
        *uuid = BTH_LE_ATT_BLUETOOTH_BASE_GUID;
        uuid->Data1 = le_uuid->Value.ShortUuid;
    }
    else
        *uuid = le_uuid->Value.LongUuid;
}

static HRESULT box_guid( const GUID *uuid, IInspectable **out )
{
    static const WCHAR *class_name = RuntimeClass_Windows_Foundation_PropertyValue;
    IPropertyValueStatics *statics;
    HSTRING_HEADER hdr;
    HSTRING str;
    HRESULT hr;

    if (FAILED((hr = WindowsCreateStringReference( class_name, wcslen( class_name ), &hdr, &str )))) return hr;
    if (FAILED((hr = RoGetActivationFactory( str, &IID_IPropertyValueStatics, (void **)&statics )))) return hr;
    hr = IPropertyValueStatics_CreateGuid( statics, *uuid, out );
    IPropertyValueStatics_Release( statics );
    return hr;
}

static HRESULT box_uint32( UINT32 value, IInspectable **out )
{
    static const WCHAR *class_name = RuntimeClass_Windows_Foundation_PropertyValue;
    IPropertyValueStatics *statics;
    HSTRING_HEADER hdr;
    HSTRING str;
    HRESULT hr;

    if (FAILED((hr = WindowsCreateStringReference( class_name, wcslen( class_name ), &hdr, &str )))) return hr;
    if (FAILED((hr = RoGetActivationFactory( str, &IID_IPropertyValueStatics, (void **)&statics )))) return hr;
    hr = IPropertyValueStatics_CreateUInt32( statics, value, out );
    IPropertyValueStatics_Release( statics );
    return hr;
}

static HRESULT unbox_guid( IUnknown *param, GUID *uuid )
{
    IPropertyValue *value;
    HRESULT hr;

    if (FAILED((hr = IUnknown_QueryInterface( param, &IID_IPropertyValue, (void **)&value )))) return hr;
    hr = IPropertyValue_GetGuid( value, uuid );
    IPropertyValue_Release( value );
    return hr;
}

static HRESULT unbox_uint32( IUnknown *param, UINT32 *out )
{
    IPropertyValue *value;
    HRESULT hr;

    if (FAILED((hr = IUnknown_QueryInterface( param, &IID_IPropertyValue, (void **)&value )))) return hr;
    hr = IPropertyValue_GetUInt32( value, out );
    IPropertyValue_Release( value );
    return hr;
}

/* Result of a completed enum-valued operation, returned through the async callback. */
static HRESULT async_result_uint32( PROPVARIANT *result, UINT32 value )
{
    result->vt = VT_UI4;
    result->ulVal = value;
    return S_OK;
}

static HRESULT async_result_object( PROPVARIANT *result, IUnknown *object )
{
    result->vt = VT_UNKNOWN;
    result->punkVal = object;
    return S_OK;
}

static HRESULT async_constant_uint32( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    UINT32 value;
    HRESULT hr;

    if (FAILED((hr = unbox_uint32( param, &value )))) return hr;
    return async_result_uint32( result, value );
}

/* A simple object with only IInspectable, for results and event args. */
#define DEFINE_SIMPLE_INSPECTABLE( pfx, iface_type, impl_type, class_name_str, destroy_expr )                     \
    static inline impl_type *impl_from_##iface_type( iface_type *iface )                                          \
    {                                                                                                             \
        return CONTAINING_RECORD( iface, impl_type, iface_type##_iface );                                         \
    }                                                                                                             \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )                       \
    {                                                                                                             \
        impl_type *impl = impl_from_##iface_type( iface );                                                        \
        TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );                                              \
        if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||                        \
            IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_##iface_type ))                       \
        {                                                                                                         \
            iface_type##_AddRef(( *out = &impl->iface_type##_iface ));                                            \
            return S_OK;                                                                                          \
        }                                                                                                         \
        *out = NULL;                                                                                              \
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );                          \
        return E_NOINTERFACE;                                                                                     \
    }                                                                                                             \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                                         \
    {                                                                                                             \
        impl_type *impl = impl_from_##iface_type( iface );                                                        \
        return InterlockedIncrement( &impl->ref );                                                                \
    }                                                                                                             \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                                        \
    {                                                                                                             \
        impl_type *impl = impl_from_##iface_type( iface );                                                        \
        ULONG ref = InterlockedDecrement( &impl->ref );                                                           \
        if (!ref)                                                                                                 \
        {                                                                                                         \
            destroy_expr;                                                                                         \
            free( impl );                                                                                         \
        }                                                                                                         \
        return ref;                                                                                               \
    }                                                                                                             \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )                        \
    {                                                                                                             \
        FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );                                                 \
        return E_NOTIMPL;                                                                                         \
    }                                                                                                             \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )                     \
    {                                                                                                             \
        return class_name_string( class_name_str, class_name );                                                   \
    }                                                                                                             \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *level )                             \
    {                                                                                                             \
        *level = BaseTrust;                                                                                       \
        return S_OK;                                                                                              \
    }

/* --- GattReadResult --- */

struct read_result
{
    IGattReadResult IGattReadResult_iface;
    LONG ref;
    GattCommunicationStatus status;
    IBuffer *value;
};

DEFINE_SIMPLE_INSPECTABLE( read_result, IGattReadResult, struct read_result,
                           L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattReadResult",
                           if (impl->value) IBuffer_Release( impl->value ) )

static HRESULT WINAPI read_result_get_Status( IGattReadResult *iface, GattCommunicationStatus *value )
{
    struct read_result *impl = impl_from_IGattReadResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI read_result_get_Value( IGattReadResult *iface, IBuffer **value )
{
    struct read_result *impl = impl_from_IGattReadResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->value)) IBuffer_AddRef( *value );
    return S_OK;
}

static const IGattReadResultVtbl read_result_vtbl =
{
    read_result_QueryInterface,
    read_result_AddRef,
    read_result_Release,
    read_result_GetIids,
    read_result_GetRuntimeClassName,
    read_result_GetTrustLevel,
    read_result_get_Status,
    read_result_get_Value,
};

static HRESULT read_result_create( GattCommunicationStatus status, IBuffer *value, IGattReadResult **out )
{
    struct read_result *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattReadResult_iface.lpVtbl = &read_result_vtbl;
    impl->ref = 1;
    impl->status = status;
    if ((impl->value = value)) IBuffer_AddRef( value );
    *out = &impl->IGattReadResult_iface;
    return S_OK;
}

/* --- GattWriteResult --- */

struct write_result
{
    IGattWriteResult IGattWriteResult_iface;
    LONG ref;
    GattCommunicationStatus status;
};

DEFINE_SIMPLE_INSPECTABLE( write_result, IGattWriteResult, struct write_result,
                           L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattWriteResult", (void)0 )

static HRESULT WINAPI write_result_get_Status( IGattWriteResult *iface, GattCommunicationStatus *value )
{
    struct write_result *impl = impl_from_IGattWriteResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI write_result_get_ProtocolError( IGattWriteResult *iface, IReference_BYTE **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = NULL;
    return S_OK;
}

static const IGattWriteResultVtbl write_result_vtbl =
{
    write_result_QueryInterface,
    write_result_AddRef,
    write_result_Release,
    write_result_GetIids,
    write_result_GetRuntimeClassName,
    write_result_GetTrustLevel,
    write_result_get_Status,
    write_result_get_ProtocolError,
};

static HRESULT write_result_create( GattCommunicationStatus status, IGattWriteResult **out )
{
    struct write_result *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattWriteResult_iface.lpVtbl = &write_result_vtbl;
    impl->ref = 1;
    impl->status = status;
    *out = &impl->IGattWriteResult_iface;
    return S_OK;
}

/* --- GattCharacteristicsResult --- */

struct characteristics_result
{
    IGattCharacteristicsResult IGattCharacteristicsResult_iface;
    LONG ref;
    GattCommunicationStatus status;
    IVectorView_GattCharacteristic *characteristics;
};

DEFINE_SIMPLE_INSPECTABLE( characteristics_result, IGattCharacteristicsResult, struct characteristics_result,
                           L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristicsResult",
                           if (impl->characteristics) IVectorView_GattCharacteristic_Release( impl->characteristics ) )

static HRESULT WINAPI characteristics_result_get_Status( IGattCharacteristicsResult *iface, GattCommunicationStatus *value )
{
    struct characteristics_result *impl = impl_from_IGattCharacteristicsResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI characteristics_result_get_ProtocolError( IGattCharacteristicsResult *iface, IReference_BYTE **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI characteristics_result_get_Characteristics( IGattCharacteristicsResult *iface,
                                                                  IVectorView_GattCharacteristic **value )
{
    struct characteristics_result *impl = impl_from_IGattCharacteristicsResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->characteristics)) IVectorView_GattCharacteristic_AddRef( *value );
    return S_OK;
}

static const IGattCharacteristicsResultVtbl characteristics_result_vtbl =
{
    characteristics_result_QueryInterface,
    characteristics_result_AddRef,
    characteristics_result_Release,
    characteristics_result_GetIids,
    characteristics_result_GetRuntimeClassName,
    characteristics_result_GetTrustLevel,
    characteristics_result_get_Status,
    characteristics_result_get_ProtocolError,
    characteristics_result_get_Characteristics,
};

static HRESULT characteristics_result_create( GattCommunicationStatus status, IVector_IInspectable *vector,
                                              IGattCharacteristicsResult **out )
{
    struct characteristics_result *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattCharacteristicsResult_iface.lpVtbl = &characteristics_result_vtbl;
    impl->ref = 1;
    impl->status = status;
    if (vector && FAILED((hr = IVector_IInspectable_GetView( vector, (IVectorView_IInspectable **)&impl->characteristics ))))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IGattCharacteristicsResult_iface;
    return S_OK;
}

/* --- GattDeviceServicesResult --- */

struct services_result
{
    IGattDeviceServicesResult IGattDeviceServicesResult_iface;
    LONG ref;
    GattCommunicationStatus status;
    IVectorView_GattDeviceService *services;
};

DEFINE_SIMPLE_INSPECTABLE( services_result, IGattDeviceServicesResult, struct services_result,
                           L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult",
                           if (impl->services) IVectorView_GattDeviceService_Release( impl->services ) )

static HRESULT WINAPI services_result_get_Status( IGattDeviceServicesResult *iface, GattCommunicationStatus *value )
{
    struct services_result *impl = impl_from_IGattDeviceServicesResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI services_result_get_ProtocolError( IGattDeviceServicesResult *iface, IReference_BYTE **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI services_result_get_Services( IGattDeviceServicesResult *iface, IVectorView_GattDeviceService **value )
{
    struct services_result *impl = impl_from_IGattDeviceServicesResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->services)) IVectorView_GattDeviceService_AddRef( *value );
    return S_OK;
}

static const IGattDeviceServicesResultVtbl services_result_vtbl =
{
    services_result_QueryInterface,
    services_result_AddRef,
    services_result_Release,
    services_result_GetIids,
    services_result_GetRuntimeClassName,
    services_result_GetTrustLevel,
    services_result_get_Status,
    services_result_get_ProtocolError,
    services_result_get_Services,
};

HRESULT gatt_device_services_result_create( IVector_IInspectable *services, IGattDeviceServicesResult **out )
{
    struct services_result *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattDeviceServicesResult_iface.lpVtbl = &services_result_vtbl;
    impl->ref = 1;
    impl->status = services ? GattCommunicationStatus_Success : GattCommunicationStatus_Unreachable;
    if (services && FAILED((hr = IVector_IInspectable_GetView( services, (IVectorView_IInspectable **)&impl->services ))))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IGattDeviceServicesResult_iface;
    return S_OK;
}

/* --- GattValueChangedEventArgs --- */

struct value_changed_args
{
    IGattValueChangedEventArgs IGattValueChangedEventArgs_iface;
    LONG ref;
    IBuffer *value;
    DateTime timestamp;
};

DEFINE_SIMPLE_INSPECTABLE( value_changed_args, IGattValueChangedEventArgs, struct value_changed_args,
                           L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattValueChangedEventArgs",
                           if (impl->value) IBuffer_Release( impl->value ) )

static HRESULT WINAPI value_changed_args_get_CharacteristicValue( IGattValueChangedEventArgs *iface, IBuffer **value )
{
    struct value_changed_args *impl = impl_from_IGattValueChangedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->value)) IBuffer_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI value_changed_args_get_Timestamp( IGattValueChangedEventArgs *iface, DateTime *value )
{
    struct value_changed_args *impl = impl_from_IGattValueChangedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->timestamp;
    return S_OK;
}

static const IGattValueChangedEventArgsVtbl value_changed_args_vtbl =
{
    value_changed_args_QueryInterface,
    value_changed_args_AddRef,
    value_changed_args_Release,
    value_changed_args_GetIids,
    value_changed_args_GetRuntimeClassName,
    value_changed_args_GetTrustLevel,
    value_changed_args_get_CharacteristicValue,
    value_changed_args_get_Timestamp,
};

static HRESULT value_changed_args_create( const BYTE *data, UINT32 size, IGattValueChangedEventArgs **out )
{
    struct value_changed_args *impl;
    FILETIME now;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattValueChangedEventArgs_iface.lpVtbl = &value_changed_args_vtbl;
    impl->ref = 1;
    GetSystemTimeAsFileTime( &now );
    impl->timestamp.UniversalTime = ((UINT64)now.dwHighDateTime << 32) | now.dwLowDateTime;
    if (FAILED((hr = buffer_create( data, size, &impl->value ))))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IGattValueChangedEventArgs_iface;
    return S_OK;
}

/* --- BluetoothDeviceId --- */

struct device_id
{
    IBluetoothDeviceId IBluetoothDeviceId_iface;
    LONG ref;
    HSTRING id;
};

DEFINE_SIMPLE_INSPECTABLE( device_id, IBluetoothDeviceId, struct device_id, L"Windows.Devices.Bluetooth.BluetoothDeviceId",
                           WindowsDeleteString( impl->id ) )

static HRESULT WINAPI device_id_get_Id( IBluetoothDeviceId *iface, HSTRING *value )
{
    struct device_id *impl = impl_from_IBluetoothDeviceId( iface );
    TRACE( "(%p, %p)\n", iface, value );
    return WindowsDuplicateString( impl->id, value );
}

static HRESULT WINAPI device_id_get_IsClassicDevice( IBluetoothDeviceId *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI device_id_get_IsLowEnergyDevice( IBluetoothDeviceId *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = TRUE;
    return S_OK;
}

static const IBluetoothDeviceIdVtbl device_id_vtbl =
{
    device_id_QueryInterface,
    device_id_AddRef,
    device_id_Release,
    device_id_GetIids,
    device_id_GetRuntimeClassName,
    device_id_GetTrustLevel,
    device_id_get_Id,
    device_id_get_IsClassicDevice,
    device_id_get_IsLowEnergyDevice,
};

HRESULT bluetoothdeviceid_create( HSTRING id, IBluetoothDeviceId **out )
{
    struct device_id *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothDeviceId_iface.lpVtbl = &device_id_vtbl;
    impl->ref = 1;
    if (FAILED((hr = WindowsDuplicateString( id, &impl->id ))))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IBluetoothDeviceId_iface;
    return S_OK;
}

struct device_id_statics
{
    IActivationFactory IActivationFactory_iface;
    IBluetoothDeviceIdStatics IBluetoothDeviceIdStatics_iface;
    LONG ref;
};

static inline struct device_id_statics *device_id_statics_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct device_id_statics, IActivationFactory_iface );
}

static HRESULT WINAPI device_id_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct device_id_statics *impl = device_id_statics_from_IActivationFactory( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothDeviceIdStatics ))
    {
        IActivationFactory_AddRef( iface );
        *out = &impl->IBluetoothDeviceIdStatics_iface;
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI device_id_factory_AddRef( IActivationFactory *iface )
{
    struct device_id_statics *impl = device_id_statics_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI device_id_factory_Release( IActivationFactory *iface )
{
    struct device_id_statics *impl = device_id_statics_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI device_id_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI device_id_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.BluetoothDeviceId", class_name );
}

static HRESULT WINAPI device_id_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI device_id_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "(%p, %p): stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl device_id_factory_vtbl =
{
    device_id_factory_QueryInterface,
    device_id_factory_AddRef,
    device_id_factory_Release,
    device_id_factory_GetIids,
    device_id_factory_GetRuntimeClassName,
    device_id_factory_GetTrustLevel,
    device_id_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( device_id_statics, IBluetoothDeviceIdStatics, struct device_id_statics, IActivationFactory_iface )

static HRESULT WINAPI device_id_statics_FromId( IBluetoothDeviceIdStatics *iface, HSTRING id, IBluetoothDeviceId **result )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_hstring( id ), result );
    return bluetoothdeviceid_create( id, result );
}

static const IBluetoothDeviceIdStaticsVtbl device_id_statics_vtbl =
{
    device_id_statics_QueryInterface,
    device_id_statics_AddRef,
    device_id_statics_Release,
    device_id_statics_GetIids,
    device_id_statics_GetRuntimeClassName,
    device_id_statics_GetTrustLevel,
    device_id_statics_FromId,
};

static struct device_id_statics device_id_statics =
{
    {&device_id_factory_vtbl},
    {&device_id_statics_vtbl},
    1
};

IActivationFactory *bluetoothdeviceid_statics_factory = &device_id_statics.IActivationFactory_iface;

/* --- BluetoothUuidHelper --- */

/* 00000000-0000-1000-8000-00805F9B34FB */
static const GUID bluetooth_base_uuid = {0x00000000, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}};

struct uuid_helper_statics
{
    IActivationFactory IActivationFactory_iface;
    IBluetoothUuidHelperStatics IBluetoothUuidHelperStatics_iface;
    LONG ref;
};

static inline struct uuid_helper_statics *uuid_helper_statics_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct uuid_helper_statics, IActivationFactory_iface );
}

static HRESULT WINAPI uuid_helper_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct uuid_helper_statics *impl = uuid_helper_statics_from_IActivationFactory( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothUuidHelperStatics ))
    {
        IActivationFactory_AddRef( iface );
        *out = &impl->IBluetoothUuidHelperStatics_iface;
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI uuid_helper_factory_AddRef( IActivationFactory *iface )
{
    struct uuid_helper_statics *impl = uuid_helper_statics_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI uuid_helper_factory_Release( IActivationFactory *iface )
{
    struct uuid_helper_statics *impl = uuid_helper_statics_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI uuid_helper_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI uuid_helper_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.BluetoothUuidHelper", class_name );
}

static HRESULT WINAPI uuid_helper_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI uuid_helper_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "(%p, %p): stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl uuid_helper_factory_vtbl =
{
    uuid_helper_factory_QueryInterface,
    uuid_helper_factory_AddRef,
    uuid_helper_factory_Release,
    uuid_helper_factory_GetIids,
    uuid_helper_factory_GetRuntimeClassName,
    uuid_helper_factory_GetTrustLevel,
    uuid_helper_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( uuid_helper_statics, IBluetoothUuidHelperStatics, struct uuid_helper_statics, IActivationFactory_iface )

static HRESULT WINAPI uuid_helper_statics_FromShortId( IBluetoothUuidHelperStatics *iface, UINT32 short_id, GUID *result )
{
    TRACE( "(%p, %#x, %p)\n", iface, short_id, result );

    if (!result) return E_POINTER;
    *result = bluetooth_base_uuid;
    result->Data1 = short_id;
    return S_OK;
}

static HRESULT WINAPI uuid_helper_statics_TryGetShortId( IBluetoothUuidHelperStatics *iface, GUID uuid, IReference_UINT32 **result )
{
    IInspectable *boxed;
    HRESULT hr;

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( &uuid ), result );

    if (!result) return E_POINTER;
    *result = NULL;

    /* Only UUIDs derived from the Bluetooth base UUID have a short form. */
    if (uuid.Data2 != bluetooth_base_uuid.Data2 || uuid.Data3 != bluetooth_base_uuid.Data3 ||
        memcmp( uuid.Data4, bluetooth_base_uuid.Data4, sizeof( uuid.Data4 ) ))
        return S_OK;

    if (FAILED(hr = box_uint32( uuid.Data1, &boxed ))) return hr;
    hr = IInspectable_QueryInterface( boxed, &IID_IReference_UINT32, (void **)result );
    IInspectable_Release( boxed );
    return hr;
}

static const IBluetoothUuidHelperStaticsVtbl uuid_helper_statics_vtbl =
{
    uuid_helper_statics_QueryInterface,
    uuid_helper_statics_AddRef,
    uuid_helper_statics_Release,
    uuid_helper_statics_GetIids,
    uuid_helper_statics_GetRuntimeClassName,
    uuid_helper_statics_GetTrustLevel,
    uuid_helper_statics_FromShortId,
    uuid_helper_statics_TryGetShortId,
};

static struct uuid_helper_statics uuid_helper_statics =
{
    {&uuid_helper_factory_vtbl},
    {&uuid_helper_statics_vtbl},
    1
};

IActivationFactory *bluetoothuuidhelper_statics_factory = &uuid_helper_statics.IActivationFactory_iface;

/* --- GattSession --- */

/* winebth only connects an LE device when its GATT services are requested. Windows connects as soon as
 * MaintainConnection is set, and apps (like Zwift) wait for ConnectionStatusChanged before asking for
 * services. A gatt_link keeps a device handle open with a services request, which makes winebth connect
 * and keeps the link up, and re-issues the request periodically so a dropped link is re-established. */
struct gatt_link
{
    LONG ref;
    HANDLE stop;
    HANDLE device;
    WCHAR path[1];
};

static void gatt_link_release( struct gatt_link *link )
{
    if (InterlockedDecrement( &link->ref )) return;
    if (link->device != INVALID_HANDLE_VALUE) CloseHandle( link->device );
    if (link->stop) CloseHandle( link->stop );
    free( link );
}

static DWORD CALLBACK gatt_link_thread( void *arg )
{
    struct gatt_link *link = arg;
    struct
    {
        struct winebth_le_device_get_gatt_services_params params;
        BTH_LE_GATT_SERVICE services[1];
    } buffer;
    DWORD bytes;
    BOOL connected = FALSE;

    SetThreadDescription( GetCurrentThread(), L"wine_gatt_link" );

    do
    {
        memset( &buffer, 0, sizeof( buffer ) );
        if (!DeviceIoControl( link->device, IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES, NULL, 0,
                              &buffer, sizeof( buffer ), &bytes, NULL ) && GetLastError() != ERROR_MORE_DATA)
        {
            WARN( "connecting %s failed: %lu\n", debugstr_w( link->path ), GetLastError() );
            connected = FALSE;
        }
        else
        {
            TRACE( "%s connected, %lu services\n", debugstr_w( link->path ), buffer.params.count );
            connected = TRUE;
        }
        /* Retry quickly while not yet connected (the first attempt commonly races ahead of
         * winebth/BlueZ), but fall back to the original slow keep-alive cadence once connected
         * so we don't needlessly hammer an already-live link. */
    } while (WaitForSingleObject( link->stop, connected ? 5000 : 300 ) == WAIT_TIMEOUT);

    gatt_link_release( link );
    return 0;
}

static struct gatt_link *gatt_link_start( IBluetoothDeviceId *id )
{
    struct gatt_link *link;
    const WCHAR *path;
    HANDLE thread;
    HSTRING str;
    UINT32 len;

    if (FAILED(IBluetoothDeviceId_get_Id( id, &str ))) return NULL;
    path = WindowsGetStringRawBuffer( str, &len );
    if (!(link = calloc( 1, offsetof( struct gatt_link, path[len + 1] ) )))
    {
        WindowsDeleteString( str );
        return NULL;
    }
    memcpy( link->path, path, len * sizeof(WCHAR) );
    WindowsDeleteString( str );

    link->ref = 1;
    link->device = INVALID_HANDLE_VALUE;
    if (!(link->stop = CreateEventW( NULL, TRUE, FALSE, NULL )) ||
        (link->device = CreateFileW( link->path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, OPEN_EXISTING, 0, NULL )) == INVALID_HANDLE_VALUE)
    {
        WARN( "failed to open %s: %lu\n", debugstr_w( link->path ), GetLastError() );
        gatt_link_release( link );
        return NULL;
    }

    link->ref++; /* owned by the thread */
    if (!(thread = CreateThread( NULL, 0, gatt_link_thread, link, 0, NULL )))
    {
        link->ref--;
        gatt_link_release( link );
        return NULL;
    }
    CloseHandle( thread );
    TRACE( "maintaining connection to %s\n", debugstr_w( link->path ) );
    return link;
}

static void gatt_link_stop( struct gatt_link *link )
{
    if (!link) return;
    SetEvent( link->stop );
    CancelIoEx( link->device, NULL );
    gatt_link_release( link );
}

struct gatt_session
{
    IGattSession IGattSession_iface;
    IClosable IClosable_iface;
    LONG ref;
    IBluetoothDeviceId *id;
    boolean maintain_connection;
    struct gatt_link *link;
    struct event_handlers pdu_changed;
    struct event_handlers status_changed;
};

static inline struct gatt_session *impl_from_IGattSession( IGattSession *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_session, IGattSession_iface );
}

static HRESULT WINAPI gatt_session_QueryInterface( IGattSession *iface, REFIID iid, void **out )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IGattSession ))
    {
        IGattSession_AddRef(( *out = &impl->IGattSession_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        IGattSession_AddRef( iface );
        *out = &impl->IClosable_iface;
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_session_AddRef( IGattSession *iface )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI gatt_session_Release( IGattSession *iface )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        gatt_link_stop( InterlockedExchangePointer( (void **)&impl->link, NULL ) );
        IBluetoothDeviceId_Release( impl->id );
        event_handlers_free( &impl->pdu_changed );
        event_handlers_free( &impl->status_changed );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_session_GetIids( IGattSession *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_session_GetRuntimeClassName( IGattSession *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession", class_name );
}

static HRESULT WINAPI gatt_session_GetTrustLevel( IGattSession *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_session_get_DeviceId( IGattSession *iface, IBluetoothDeviceId **value )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    TRACE( "(%p, %p)\n", iface, value );
    IBluetoothDeviceId_AddRef(( *value = impl->id ));
    return S_OK;
}

static HRESULT WINAPI gatt_session_get_CanMaintainConnection( IGattSession *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI gatt_session_put_MaintainConnection( IGattSession *iface, boolean value )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    struct gatt_link *link;

    TRACE( "(%p, %d)\n", iface, value );
    impl->maintain_connection = value;
    if (value && !impl->link)
    {
        if ((link = gatt_link_start( impl->id )) && InterlockedCompareExchangePointer( (void **)&impl->link, link, NULL ))
            gatt_link_stop( link );
    }
    else if (!value)
        gatt_link_stop( InterlockedExchangePointer( (void **)&impl->link, NULL ) );
    return S_OK;
}

static HRESULT WINAPI gatt_session_get_MaintainConnection( IGattSession *iface, boolean *value )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->maintain_connection;
    return S_OK;
}

static HRESULT WINAPI gatt_session_get_MaxPduSize( IGattSession *iface, UINT16 *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = 23;
    return S_OK;
}

static HRESULT WINAPI gatt_session_get_SessionStatus( IGattSession *iface, GattSessionStatus *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = GattSessionStatus_Active;
    return S_OK;
}

static HRESULT WINAPI gatt_session_add_MaxPduSizeChanged( IGattSession *iface,
                                                          ITypedEventHandler_GattSession_IInspectable *handler,
                                                          EventRegistrationToken *token )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    TRACE( "(%p, %p, %p)\n", iface, handler, token );
    return event_handlers_add( &impl->pdu_changed, (IUnknown *)handler, token );
}

static HRESULT WINAPI gatt_session_remove_MaxPduSizeChanged( IGattSession *iface, EventRegistrationToken token )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    TRACE( "(%p, %I64x)\n", iface, token.value );
    return event_handlers_remove( &impl->pdu_changed, token );
}

static HRESULT WINAPI gatt_session_add_SessionStatusChanged( IGattSession *iface,
                                                             ITypedEventHandler_GattSession_GattSessionStatusChangedEventArgs *handler,
                                                             EventRegistrationToken *token )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    TRACE( "(%p, %p, %p)\n", iface, handler, token );
    return event_handlers_add( &impl->status_changed, (IUnknown *)handler, token );
}

static HRESULT WINAPI gatt_session_remove_SessionStatusChanged( IGattSession *iface, EventRegistrationToken token )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    TRACE( "(%p, %I64x)\n", iface, token.value );
    return event_handlers_remove( &impl->status_changed, token );
}

static const IGattSessionVtbl gatt_session_vtbl =
{
    gatt_session_QueryInterface,
    gatt_session_AddRef,
    gatt_session_Release,
    gatt_session_GetIids,
    gatt_session_GetRuntimeClassName,
    gatt_session_GetTrustLevel,
    gatt_session_get_DeviceId,
    gatt_session_get_CanMaintainConnection,
    gatt_session_put_MaintainConnection,
    gatt_session_get_MaintainConnection,
    gatt_session_get_MaxPduSize,
    gatt_session_get_SessionStatus,
    gatt_session_add_MaxPduSizeChanged,
    gatt_session_remove_MaxPduSizeChanged,
    gatt_session_add_SessionStatusChanged,
    gatt_session_remove_SessionStatusChanged,
};

DEFINE_IINSPECTABLE_( gatt_session_closable, IClosable, struct gatt_session, gatt_session_from_IClosable, IClosable_iface, &impl->IGattSession_iface )

static HRESULT WINAPI gatt_session_closable_Close( IClosable *iface )
{
    struct gatt_session *impl = CONTAINING_RECORD( iface, struct gatt_session, IClosable_iface );

    TRACE( "(%p)\n", iface );
    gatt_link_stop( InterlockedExchangePointer( (void **)&impl->link, NULL ) );
    impl->maintain_connection = FALSE;
    return S_OK;
}

static const IClosableVtbl gatt_session_closable_vtbl =
{
    gatt_session_closable_QueryInterface,
    gatt_session_closable_AddRef,
    gatt_session_closable_Release,
    gatt_session_closable_GetIids,
    gatt_session_closable_GetRuntimeClassName,
    gatt_session_closable_GetTrustLevel,
    gatt_session_closable_Close,
};

HRESULT gatt_session_create( IBluetoothDeviceId *id, IGattSession **session )
{
    struct gatt_session *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattSession_iface.lpVtbl = &gatt_session_vtbl;
    impl->IClosable_iface.lpVtbl = &gatt_session_closable_vtbl;
    impl->ref = 1;
    IBluetoothDeviceId_AddRef(( impl->id = id ));
    event_handlers_init( &impl->pdu_changed );
    event_handlers_init( &impl->status_changed );
    *session = &impl->IGattSession_iface;
    return S_OK;
}

static HRESULT gatt_session_from_id_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    IBluetoothDeviceId *id;
    IGattSession *session;
    HRESULT hr;

    if (FAILED((hr = IUnknown_QueryInterface( param, &IID_IBluetoothDeviceId, (void **)&id )))) return hr;
    hr = gatt_session_create( id, &session );
    IBluetoothDeviceId_Release( id );
    if (FAILED(hr)) return hr;
    return async_result_object( result, (IUnknown *)session );
}

struct gatt_session_statics
{
    IActivationFactory IActivationFactory_iface;
    IGattSessionStatics IGattSessionStatics_iface;
    LONG ref;
};

static inline struct gatt_session_statics *gatt_session_statics_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_session_statics, IActivationFactory_iface );
}

static HRESULT WINAPI gatt_session_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct gatt_session_statics *impl = gatt_session_statics_from_IActivationFactory( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IGattSessionStatics ))
    {
        IActivationFactory_AddRef( iface );
        *out = &impl->IGattSessionStatics_iface;
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_session_factory_AddRef( IActivationFactory *iface )
{
    struct gatt_session_statics *impl = gatt_session_statics_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI gatt_session_factory_Release( IActivationFactory *iface )
{
    struct gatt_session_statics *impl = gatt_session_statics_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI gatt_session_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_session_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession", class_name );
}

static HRESULT WINAPI gatt_session_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_session_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "(%p, %p): stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl gatt_session_factory_vtbl =
{
    gatt_session_factory_QueryInterface,
    gatt_session_factory_AddRef,
    gatt_session_factory_Release,
    gatt_session_factory_GetIids,
    gatt_session_factory_GetRuntimeClassName,
    gatt_session_factory_GetTrustLevel,
    gatt_session_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( gatt_session_statics, IGattSessionStatics, struct gatt_session_statics, IActivationFactory_iface )

static HRESULT WINAPI gatt_session_statics_FromDeviceIdAsync( IGattSessionStatics *iface, IBluetoothDeviceId *id,
                                                              IAsyncOperation_GattSession **async )
{
    TRACE( "(%p, %p, %p)\n", iface, id, async );
    return async_operation_inspectable_create( &IID_IAsyncOperation_GattSession, (IUnknown *)iface, (IUnknown *)id,
                                               gatt_session_from_id_async, (IAsyncOperation_IInspectable **)async );
}

static const IGattSessionStaticsVtbl gatt_session_statics_vtbl =
{
    gatt_session_statics_QueryInterface,
    gatt_session_statics_AddRef,
    gatt_session_statics_Release,
    gatt_session_statics_GetIids,
    gatt_session_statics_GetRuntimeClassName,
    gatt_session_statics_GetTrustLevel,
    gatt_session_statics_FromDeviceIdAsync,
};

static struct gatt_session_statics gatt_session_statics =
{
    {&gatt_session_factory_vtbl},
    {&gatt_session_statics_vtbl},
    1
};

IActivationFactory *gattsession_statics_factory = &gatt_session_statics.IActivationFactory_iface;

/* --- GattDeviceService --- */

struct gatt_service
{
    IGattDeviceService IGattDeviceService_iface;
    IGattDeviceService2 IGattDeviceService2_iface;
    IGattDeviceService3 IGattDeviceService3_iface;
    IClosable IClosable_iface;
    LONG ref;
    BTH_LE_GATT_SERVICE service;
    HANDLE device;
    UINT64 addr;
    IBluetoothLEDevice *ble_device;
    CRITICAL_SECTION cs;
    HANDLE service_handle; /* Guarded by cs */
};

static const struct vector_iids characteristic_vector_iids =
{
    .vector = &IID_IVector_IInspectable,
    .view = &IID_IVectorView_GattCharacteristic,
    .iterable = &IID_IIterable_GattCharacteristic,
    .iterator = &IID_IIterator_GattCharacteristic,
};

static const struct vector_iids service_vector_iids =
{
    .vector = &IID_IVector_IInspectable,
    .view = &IID_IVectorView_GattDeviceService,
    .iterable = &IID_IIterable_GattDeviceService,
    .iterator = &IID_IIterator_GattDeviceService,
};

static HRESULT gatt_characteristic_create( struct gatt_service *service, const BTH_LE_GATT_CHARACTERISTIC *chrc,
                                           IGattCharacteristic **out );

static inline struct gatt_service *impl_from_IGattDeviceService( IGattDeviceService *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_service, IGattDeviceService_iface );
}

/* Find the driver's device node for this service. Caller must hold impl->cs. */
static HANDLE gatt_service_find( struct gatt_service *impl )
{
    char buffer[sizeof( SP_DEVICE_INTERFACE_DETAIL_DATA_W ) + MAX_PATH * sizeof( WCHAR )];
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buffer;
    SP_DEVICE_INTERFACE_DATA iface_data = { .cbSize = sizeof( iface_data ) };
    WCHAR wanted_addr[13];
    HDEVINFO devinfo;
    DWORD idx = 0;
    GUID uuid;

    if (impl->service_handle != INVALID_HANDLE_VALUE) return impl->service_handle;

    le_uuid_to_guid( &impl->service.ServiceUuid, &uuid );
    swprintf( wanted_addr, ARRAY_SIZE( wanted_addr ), L"%012I64x", impl->addr );
    devinfo = SetupDiGetClassDevsW( &GUID_BLUETOOTH_GATT_SERVICE_DEVICE_INTERFACE, NULL, NULL,
                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE );
    if (devinfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    detail->cbSize = sizeof( *detail );
    while (SetupDiEnumDeviceInterfaces( devinfo, NULL, &GUID_BLUETOOTH_GATT_SERVICE_DEVICE_INTERFACE, idx++, &iface_data ))
    {
        SP_DEVINFO_DATA devinfo_data = { .cbSize = sizeof( devinfo_data ) };
        WCHAR addr_str[13];
        DEVPROPTYPE type;
        GUID svc_uuid;

        if (!SetupDiGetDeviceInterfaceDetailW( devinfo, &iface_data, detail, sizeof( buffer ), NULL, &devinfo_data ))
            continue;
        if (!SetupDiGetDevicePropertyW( devinfo, &devinfo_data, &DEVPKEY_Bluetooth_DeviceAddress, &type, (BYTE *)addr_str,
                                        sizeof( addr_str ), NULL, 0 ) || wcsicmp( addr_str, wanted_addr ))
            continue;
        if (!SetupDiGetDevicePropertyW( devinfo, &devinfo_data, &DEVPKEY_Bluetooth_ServiceGUID, &type, (BYTE *)&svc_uuid,
                                        sizeof( svc_uuid ), NULL, 0 ) || !IsEqualGUID( &svc_uuid, &uuid ))
            continue;
        impl->service_handle = CreateFileW( detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                            FILE_FLAG_OVERLAPPED, NULL );
        if (impl->service_handle == INVALID_HANDLE_VALUE)
            WARN( "Failed to open %s: %lu\n", debugstr_w( detail->DevicePath ), GetLastError() );
        break;
    }
    SetupDiDestroyDeviceInfoList( devinfo );
    return impl->service_handle;
}

/* winebth creates the service device nodes asynchronously once BlueZ has resolved the services, so an app
 * that asks for characteristics right after connecting can get ahead of them. Wait a little for them to
 * appear. Caller must hold impl->cs. */
static HANDLE gatt_service_open( struct gatt_service *impl )
{
    GUID uuid;
    int i;

    for (i = 0; i < 50; i++)
    {
        if (gatt_service_find( impl ) != INVALID_HANDLE_VALUE) return impl->service_handle;
        Sleep( 100 );
    }
    le_uuid_to_guid( &impl->service.ServiceUuid, &uuid );
    WARN( "No device node for service %s\n", debugstr_guid( &uuid ) );
    return INVALID_HANDLE_VALUE;
}

static HANDLE gatt_service_get_handle( struct gatt_service *impl )
{
    HANDLE handle;

    EnterCriticalSection( &impl->cs );
    handle = gatt_service_open( impl );
    LeaveCriticalSection( &impl->cs );
    return handle;
}

/* Enumerate the characteristics of the service, optionally filtered by UUID. */
static HRESULT gatt_service_get_characteristics_vector( struct gatt_service *impl, const GUID *filter,
                                                        IVector_IInspectable **out )
{
    BTH_LE_GATT_CHARACTERISTIC *buf = NULL;
    IVector_IInspectable *vector;
    USHORT actual = 0, i;
    HRESULT hr;

    *out = NULL;
    if (FAILED((hr = vector_create( &characteristic_vector_iids, (void **)&vector )))) return hr;

    hr = BluetoothGATTGetCharacteristics( impl->device, &impl->service, 0, NULL, &actual, 0 );
    if (SUCCEEDED(hr)) goto done;
    if (hr != HRESULT_FROM_WIN32( ERROR_MORE_DATA )) goto done;
    if (!(buf = calloc( actual, sizeof( *buf ) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (FAILED((hr = BluetoothGATTGetCharacteristics( impl->device, &impl->service, actual, buf, &actual, 0 )))) goto done;
    for (i = 0; i < actual; i++)
    {
        IGattCharacteristic *chrc;
        GUID uuid;

        if (filter)
        {
            le_uuid_to_guid( &buf[i].CharacteristicUuid, &uuid );
            if (!IsEqualGUID( filter, &uuid )) continue;
        }
        if (FAILED((hr = gatt_characteristic_create( impl, &buf[i], &chrc )))) goto done;
        hr = IVector_IInspectable_Append( vector, (IInspectable *)chrc );
        IGattCharacteristic_Release( chrc );
        if (FAILED(hr)) goto done;
    }
done:
    free( buf );
    if (FAILED(hr))
    {
        WARN( "Failed to enumerate characteristics: %#lx\n", hr );
        IVector_IInspectable_Release( vector );
        return hr;
    }
    *out = vector;
    return S_OK;
}

static HRESULT WINAPI gatt_service_QueryInterface( IGattDeviceService *iface, REFIID iid, void **out )
{
    struct gatt_service *impl = impl_from_IGattDeviceService( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IGattDeviceService ))
    {
        IGattDeviceService_AddRef(( *out = &impl->IGattDeviceService_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IGattDeviceService2 ))
    {
        IGattDeviceService_AddRef( iface );
        *out = &impl->IGattDeviceService2_iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IGattDeviceService3 ))
    {
        IGattDeviceService_AddRef( iface );
        *out = &impl->IGattDeviceService3_iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        IGattDeviceService_AddRef( iface );
        *out = &impl->IClosable_iface;
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_service_AddRef( IGattDeviceService *iface )
{
    struct gatt_service *impl = impl_from_IGattDeviceService( iface );
    TRACE( "(%p)\n", iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI gatt_service_Release( IGattDeviceService *iface )
{
    struct gatt_service *impl = impl_from_IGattDeviceService( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "(%p)\n", iface );

    if (!ref)
    {
        if (impl->service_handle != INVALID_HANDLE_VALUE) CloseHandle( impl->service_handle );
        CloseHandle( impl->device );
        if (impl->ble_device) IBluetoothLEDevice_Release( impl->ble_device );
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_service_GetIids( IGattDeviceService *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service_GetRuntimeClassName( IGattDeviceService *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceService", class_name );
}

static HRESULT WINAPI gatt_service_GetTrustLevel( IGattDeviceService *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_service_GetCharacteristics( IGattDeviceService *iface, GUID uuid, IVectorView_GattCharacteristic **chars )
{
    struct gatt_service *impl = impl_from_IGattDeviceService( iface );
    IVector_IInspectable *vector;
    HRESULT hr;

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( &uuid ), chars );

    if (FAILED((hr = gatt_service_get_characteristics_vector( impl, &uuid, &vector )))) return hr;
    hr = IVector_IInspectable_GetView( vector, (IVectorView_IInspectable **)chars );
    IVector_IInspectable_Release( vector );
    return hr;
}

static HRESULT WINAPI gatt_service_GetIncludedServices( IGattDeviceService *iface, GUID uuid, IVectorView_GattDeviceService **services )
{
    IVector_IInspectable *vector;
    HRESULT hr;

    FIXME( "(%p, %s, %p): semi-stub!\n", iface, debugstr_guid( &uuid ), services );

    if (FAILED((hr = vector_create( &service_vector_iids, (void **)&vector )))) return hr;
    hr = IVector_IInspectable_GetView( vector, (IVectorView_IInspectable **)services );
    IVector_IInspectable_Release( vector );
    return hr;
}

static HRESULT WINAPI gatt_service_get_DeviceId( IGattDeviceService *iface, HSTRING *value )
{
    struct gatt_service *impl = impl_from_IGattDeviceService( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!impl->ble_device)
    {
        *value = NULL;
        return E_NOTIMPL;
    }
    return IBluetoothLEDevice_get_DeviceId( impl->ble_device, value );
}

static HRESULT WINAPI gatt_service_get_Uuid( IGattDeviceService *iface, GUID *value )
{
    struct gatt_service *impl = impl_from_IGattDeviceService( iface );
    TRACE( "(%p, %p)\n", iface, value );
    le_uuid_to_guid( &impl->service.ServiceUuid, value );
    return S_OK;
}

static HRESULT WINAPI gatt_service_get_AttributeHandle( IGattDeviceService *iface, UINT16 *value )
{
    struct gatt_service *impl = impl_from_IGattDeviceService( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->service.AttributeHandle;
    return S_OK;
}

static const IGattDeviceServiceVtbl gatt_service_vtbl =
{
    /* IUnknown */
    gatt_service_QueryInterface,
    gatt_service_AddRef,
    gatt_service_Release,
    /* IInspectable */
    gatt_service_GetIids,
    gatt_service_GetRuntimeClassName,
    gatt_service_GetTrustLevel,
    /* IGattDeviceService */
    gatt_service_GetCharacteristics,
    gatt_service_GetIncludedServices,
    gatt_service_get_DeviceId,
    gatt_service_get_Uuid,
    gatt_service_get_AttributeHandle
};

DEFINE_IINSPECTABLE( gatt_service2, IGattDeviceService2, struct gatt_service, IGattDeviceService_iface )

static HRESULT WINAPI gatt_service2_get_Device( IGattDeviceService2 *iface, IBluetoothLEDevice **value )
{
    struct gatt_service *impl = impl_from_IGattDeviceService2( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->ble_device)) IBluetoothLEDevice_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI gatt_service2_get_ParentServices( IGattDeviceService2 *iface, IVectorView_GattDeviceService **value )
{
    IVector_IInspectable *vector;
    HRESULT hr;

    TRACE( "(%p, %p)\n", iface, value );

    if (FAILED((hr = vector_create( &service_vector_iids, (void **)&vector )))) return hr;
    hr = IVector_IInspectable_GetView( vector, (IVectorView_IInspectable **)value );
    IVector_IInspectable_Release( vector );
    return hr;
}

static HRESULT WINAPI gatt_service2_GetAllCharacteristics( IGattDeviceService2 *iface, IVectorView_GattCharacteristic **value )
{
    struct gatt_service *impl = impl_from_IGattDeviceService2( iface );
    IVector_IInspectable *vector;
    HRESULT hr;

    TRACE( "(%p, %p)\n", iface, value );

    if (FAILED((hr = gatt_service_get_characteristics_vector( impl, NULL, &vector )))) return hr;
    hr = IVector_IInspectable_GetView( vector, (IVectorView_IInspectable **)value );
    IVector_IInspectable_Release( vector );
    return hr;
}

static HRESULT WINAPI gatt_service2_GetAllIncludedServices( IGattDeviceService2 *iface, IVectorView_GattDeviceService **value )
{
    return gatt_service2_get_ParentServices( iface, value );
}

static const IGattDeviceService2Vtbl gatt_service2_vtbl =
{
    gatt_service2_QueryInterface,
    gatt_service2_AddRef,
    gatt_service2_Release,
    gatt_service2_GetIids,
    gatt_service2_GetRuntimeClassName,
    gatt_service2_GetTrustLevel,
    gatt_service2_get_Device,
    gatt_service2_get_ParentServices,
    gatt_service2_GetAllCharacteristics,
    gatt_service2_GetAllIncludedServices,
};

DEFINE_IINSPECTABLE( gatt_service3, IGattDeviceService3, struct gatt_service, IGattDeviceService_iface )

static HRESULT WINAPI gatt_service3_get_DeviceAccessInformation( IGattDeviceService3 *iface, IDeviceAccessInformation **value )
{
    FIXME( "(%p, %p): stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service3_get_Session( IGattDeviceService3 *iface, IGattSession **value )
{
    struct gatt_service *impl = impl_from_IGattDeviceService3( iface );
    IBluetoothDeviceId *id;
    HSTRING id_str;
    HRESULT hr;

    TRACE( "(%p, %p)\n", iface, value );

    *value = NULL;
    if (!impl->ble_device) return E_NOTIMPL;
    if (FAILED((hr = IBluetoothLEDevice_get_DeviceId( impl->ble_device, &id_str )))) return hr;
    hr = bluetoothdeviceid_create( id_str, &id );
    WindowsDeleteString( id_str );
    if (FAILED(hr)) return hr;
    hr = gatt_session_create( id, value );
    IBluetoothDeviceId_Release( id );
    return hr;
}

static HRESULT WINAPI gatt_service3_get_SharingMode( IGattDeviceService3 *iface, GattSharingMode *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = GattSharingMode_SharedReadAndWrite;
    return S_OK;
}

static HRESULT WINAPI gatt_service3_RequestAccessAsync( IGattDeviceService3 *iface, IAsyncOperation_DeviceAccessStatus **async )
{
    IInspectable *param;
    HRESULT hr;

    TRACE( "(%p, %p)\n", iface, async );

    if (FAILED((hr = box_uint32( DeviceAccessStatus_Allowed, &param )))) return hr;
    hr = async_operation_uint32_create( &IID_IAsyncOperation_DeviceAccessStatus, (IUnknown *)iface, (IUnknown *)param,
                                        async_constant_uint32, (IAsyncOperation_IInspectable **)async );
    IInspectable_Release( param );
    return hr;
}

static HRESULT WINAPI gatt_service3_OpenAsync( IGattDeviceService3 *iface, GattSharingMode mode,
                                               IAsyncOperation_GattOpenStatus **async )
{
    IInspectable *param;
    HRESULT hr;

    TRACE( "(%p, %d, %p)\n", iface, mode, async );

    if (FAILED((hr = box_uint32( GattOpenStatus_Success, &param )))) return hr;
    hr = async_operation_uint32_create( &IID_IAsyncOperation_GattOpenStatus, (IUnknown *)iface, (IUnknown *)param,
                                        async_constant_uint32, (IAsyncOperation_IInspectable **)async );
    IInspectable_Release( param );
    return hr;
}

static HRESULT gatt_service_get_characteristics_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct gatt_service *impl = impl_from_IGattDeviceService3( (IGattDeviceService3 *)invoker );
    IGattCharacteristicsResult *chars_result;
    IVector_IInspectable *vector = NULL;
    GUID uuid, *filter = NULL;
    HRESULT hr;

    if (param)
    {
        if (FAILED((hr = unbox_guid( param, &uuid )))) return hr;
        filter = &uuid;
    }
    hr = gatt_service_get_characteristics_vector( impl, filter, &vector );
    hr = characteristics_result_create( SUCCEEDED(hr) ? GattCommunicationStatus_Success : GattCommunicationStatus_Unreachable,
                                        vector, &chars_result );
    if (vector) IVector_IInspectable_Release( vector );
    if (FAILED(hr)) return hr;
    return async_result_object( result, (IUnknown *)chars_result );
}

static HRESULT gatt_service_start_characteristics_async( IGattDeviceService3 *iface, const GUID *uuid,
                                                         IAsyncOperation_GattCharacteristicsResult **async )
{
    IInspectable *param = NULL;
    HRESULT hr;

    if (uuid && FAILED((hr = box_guid( uuid, &param )))) return hr;
    hr = async_operation_inspectable_create( &IID_IAsyncOperation_GattCharacteristicsResult, (IUnknown *)iface,
                                             (IUnknown *)param, gatt_service_get_characteristics_async,
                                             (IAsyncOperation_IInspectable **)async );
    if (param) IInspectable_Release( param );
    return hr;
}

static HRESULT WINAPI gatt_service3_GetCharacteristicsAsync( IGattDeviceService3 *iface,
                                                             IAsyncOperation_GattCharacteristicsResult **async )
{
    TRACE( "(%p, %p)\n", iface, async );
    return gatt_service_start_characteristics_async( iface, NULL, async );
}

static HRESULT WINAPI gatt_service3_GetCharacteristicsWithCacheModeAsync( IGattDeviceService3 *iface, BluetoothCacheMode mode,
                                                                          IAsyncOperation_GattCharacteristicsResult **async )
{
    TRACE( "(%p, %d, %p)\n", iface, mode, async );
    return gatt_service_start_characteristics_async( iface, NULL, async );
}

static HRESULT WINAPI gatt_service3_GetCharacteristicsForUuidAsync( IGattDeviceService3 *iface, GUID uuid,
                                                                    IAsyncOperation_GattCharacteristicsResult **async )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( &uuid ), async );
    return gatt_service_start_characteristics_async( iface, &uuid, async );
}

static HRESULT WINAPI gatt_service3_GetCharacteristicsForUuidWithCacheModeAsync( IGattDeviceService3 *iface, GUID uuid,
                                                                                 BluetoothCacheMode mode,
                                                                                 IAsyncOperation_GattCharacteristicsResult **async )
{
    TRACE( "(%p, %s, %d, %p)\n", iface, debugstr_guid( &uuid ), mode, async );
    return gatt_service_start_characteristics_async( iface, &uuid, async );
}

static HRESULT gatt_service_empty_services_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    IGattDeviceServicesResult *services_result;
    IVector_IInspectable *vector;
    HRESULT hr;

    if (FAILED((hr = vector_create( &service_vector_iids, (void **)&vector )))) return hr;
    hr = gatt_device_services_result_create( vector, &services_result );
    IVector_IInspectable_Release( vector );
    if (FAILED(hr)) return hr;
    return async_result_object( result, (IUnknown *)services_result );
}

static HRESULT WINAPI gatt_service3_GetIncludedServicesAsync( IGattDeviceService3 *iface,
                                                              IAsyncOperation_GattDeviceServicesResult **async )
{
    FIXME( "(%p, %p): semi-stub!\n", iface, async );
    return async_operation_inspectable_create( &IID_IAsyncOperation_GattDeviceServicesResult, (IUnknown *)iface, NULL,
                                               gatt_service_empty_services_async, (IAsyncOperation_IInspectable **)async );
}

static HRESULT WINAPI gatt_service3_GetIncludedServicesWithCacheModeAsync( IGattDeviceService3 *iface, BluetoothCacheMode mode,
                                                                           IAsyncOperation_GattDeviceServicesResult **async )
{
    return gatt_service3_GetIncludedServicesAsync( iface, async );
}

static HRESULT WINAPI gatt_service3_GetIncludedServicesForUuidAsync( IGattDeviceService3 *iface, GUID uuid,
                                                                     IAsyncOperation_GattDeviceServicesResult **async )
{
    return gatt_service3_GetIncludedServicesAsync( iface, async );
}

static HRESULT WINAPI gatt_service3_GetIncludedServicesForUuidWithCacheModeAsync( IGattDeviceService3 *iface, GUID uuid,
                                                                                  BluetoothCacheMode mode,
                                                                                  IAsyncOperation_GattDeviceServicesResult **async )
{
    return gatt_service3_GetIncludedServicesAsync( iface, async );
}

static const IGattDeviceService3Vtbl gatt_service3_vtbl =
{
    gatt_service3_QueryInterface,
    gatt_service3_AddRef,
    gatt_service3_Release,
    gatt_service3_GetIids,
    gatt_service3_GetRuntimeClassName,
    gatt_service3_GetTrustLevel,
    gatt_service3_get_DeviceAccessInformation,
    gatt_service3_get_Session,
    gatt_service3_get_SharingMode,
    gatt_service3_RequestAccessAsync,
    gatt_service3_OpenAsync,
    gatt_service3_GetCharacteristicsAsync,
    gatt_service3_GetCharacteristicsWithCacheModeAsync,
    gatt_service3_GetCharacteristicsForUuidAsync,
    gatt_service3_GetCharacteristicsForUuidWithCacheModeAsync,
    gatt_service3_GetIncludedServicesAsync,
    gatt_service3_GetIncludedServicesWithCacheModeAsync,
    gatt_service3_GetIncludedServicesForUuidAsync,
    gatt_service3_GetIncludedServicesForUuidWithCacheModeAsync,
};

DEFINE_IINSPECTABLE_( gatt_service_closable, IClosable, struct gatt_service, gatt_service_from_IClosable, IClosable_iface, &impl->IGattDeviceService_iface )

static HRESULT WINAPI gatt_service_closable_Close( IClosable *iface )
{
    TRACE( "(%p)\n", iface );
    return S_OK;
}

static const IClosableVtbl gatt_service_closable_vtbl =
{
    gatt_service_closable_QueryInterface,
    gatt_service_closable_AddRef,
    gatt_service_closable_Release,
    gatt_service_closable_GetIids,
    gatt_service_closable_GetRuntimeClassName,
    gatt_service_closable_GetTrustLevel,
    gatt_service_closable_Close,
};

HRESULT gatt_service_create( const BTH_LE_GATT_SERVICE *svc, HANDLE device, UINT64 addr, IBluetoothLEDevice *ble_device,
                             IGattDeviceService **service )
{
    struct gatt_service *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) )))
        return E_OUTOFMEMORY;
    if (!DuplicateHandle( GetCurrentProcess(), device, GetCurrentProcess(), &impl->device, 0, FALSE, DUPLICATE_SAME_ACCESS ))
    {
        free( impl );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    impl->IGattDeviceService_iface.lpVtbl = &gatt_service_vtbl;
    impl->IGattDeviceService2_iface.lpVtbl = &gatt_service2_vtbl;
    impl->IGattDeviceService3_iface.lpVtbl = &gatt_service3_vtbl;
    impl->IClosable_iface.lpVtbl = &gatt_service_closable_vtbl;
    impl->ref = 1;
    impl->service = *svc;
    impl->addr = addr;
    impl->service_handle = INVALID_HANDLE_VALUE;
    if ((impl->ble_device = ble_device)) IBluetoothLEDevice_AddRef( ble_device );
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );

    *service = &impl->IGattDeviceService_iface;
    return S_OK;
}

/* --- GattCharacteristic --- */

struct gatt_characteristic
{
    IGattCharacteristic IGattCharacteristic_iface;
    IGattCharacteristic2 IGattCharacteristic2_iface;
    IGattCharacteristic3 IGattCharacteristic3_iface;
    IClosable IClosable_iface;
    LONG ref;
    struct gatt_service *service;
    BTH_LE_GATT_CHARACTERISTIC chrc;
    GattProtectionLevel protection_level;
    struct event_handlers value_changed;
    HCMNOTIFICATION notification; /* Guarded by value_changed.cs */
};

/* Parameters for an asynchronous write or notification change. */
struct gatt_write_request
{
    IUnknown IUnknown_iface;
    LONG ref;
    IBuffer *value;
    BOOL without_response;
    BOOL with_result;
    BOOL notify_request;
    UINT32 notify_value;
};

static inline struct gatt_write_request *impl_from_write_request( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_write_request, IUnknown_iface );
}

static HRESULT WINAPI write_request_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ))
    {
        IUnknown_AddRef( iface );
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI write_request_AddRef( IUnknown *iface )
{
    struct gatt_write_request *impl = impl_from_write_request( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI write_request_Release( IUnknown *iface )
{
    struct gatt_write_request *impl = impl_from_write_request( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        if (impl->value) IBuffer_Release( impl->value );
        free( impl );
    }
    return ref;
}

static const IUnknownVtbl write_request_vtbl = { write_request_QueryInterface, write_request_AddRef, write_request_Release };

static HRESULT write_request_create( IBuffer *value, BOOL without_response, BOOL with_result, struct gatt_write_request **out )
{
    struct gatt_write_request *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IUnknown_iface.lpVtbl = &write_request_vtbl;
    impl->ref = 1;
    if ((impl->value = value)) IBuffer_AddRef( value );
    impl->without_response = without_response;
    impl->with_result = with_result;
    *out = impl;
    return S_OK;
}

static inline struct gatt_characteristic *impl_from_IGattCharacteristic( IGattCharacteristic *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_characteristic, IGattCharacteristic_iface );
}

static HRESULT WINAPI gatt_characteristic_QueryInterface( IGattCharacteristic *iface, REFIID iid, void **out )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IGattCharacteristic ))
    {
        IGattCharacteristic_AddRef(( *out = &impl->IGattCharacteristic_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IGattCharacteristic2 ))
    {
        IGattCharacteristic_AddRef( iface );
        *out = &impl->IGattCharacteristic2_iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IGattCharacteristic3 ))
    {
        IGattCharacteristic_AddRef( iface );
        *out = &impl->IGattCharacteristic3_iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        IGattCharacteristic_AddRef( iface );
        *out = &impl->IClosable_iface;
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_characteristic_AddRef( IGattCharacteristic *iface )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI gatt_characteristic_Release( IGattCharacteristic *iface )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "(%p)\n", iface );

    if (!ref)
    {
        if (impl->notification) CM_Unregister_Notification( impl->notification );
        event_handlers_free( &impl->value_changed );
        IGattDeviceService_Release( &impl->service->IGattDeviceService_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_characteristic_GetIids( IGattCharacteristic *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_characteristic_GetRuntimeClassName( IGattCharacteristic *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristic", class_name );
}

static HRESULT WINAPI gatt_characteristic_GetTrustLevel( IGattCharacteristic *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

/* --- GattDescriptor ---
 *
 * The driver does not expose descriptors, so the Client Characteristic Configuration descriptor is synthesised for
 * characteristics that support notifications. Writes to it map onto the driver's notification IOCTL. */

static const GUID cccd_uuid = { 0x2902, 0, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };

static const struct vector_iids descriptor_vector_iids =
{
    .vector = &IID_IVector_IInspectable,
    .view = &IID_IVectorView_GattDescriptor,
    .iterable = &IID_IIterable_GattDescriptor,
    .iterator = &IID_IIterator_GattDescriptor,
};

struct gatt_descriptor
{
    IGattDescriptor IGattDescriptor_iface;
    IGattDescriptor2 IGattDescriptor2_iface;
    IClosable IClosable_iface;
    LONG ref;
    struct gatt_characteristic *characteristic;
    GUID uuid;
    UINT16 handle;
    GattProtectionLevel protection_level;
};

static HRESULT gatt_characteristic_start_notify( IGattCharacteristic *iface,
                                                 GattClientCharacteristicConfigurationDescriptorValue value,
                                                 BOOL with_result, void **async );

static inline struct gatt_descriptor *impl_from_IGattDescriptor( IGattDescriptor *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_descriptor, IGattDescriptor_iface );
}

static HRESULT WINAPI gatt_descriptor_QueryInterface( IGattDescriptor *iface, REFIID iid, void **out )
{
    struct gatt_descriptor *impl = impl_from_IGattDescriptor( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IGattDescriptor ))
    {
        IGattDescriptor_AddRef(( *out = &impl->IGattDescriptor_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IGattDescriptor2 ))
    {
        IGattDescriptor_AddRef( iface );
        *out = &impl->IGattDescriptor2_iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        IGattDescriptor_AddRef( iface );
        *out = &impl->IClosable_iface;
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_descriptor_AddRef( IGattDescriptor *iface )
{
    struct gatt_descriptor *impl = impl_from_IGattDescriptor( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI gatt_descriptor_Release( IGattDescriptor *iface )
{
    struct gatt_descriptor *impl = impl_from_IGattDescriptor( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        IGattCharacteristic_Release( &impl->characteristic->IGattCharacteristic_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_descriptor_GetIids( IGattDescriptor *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_descriptor_GetRuntimeClassName( IGattDescriptor *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattDescriptor", class_name );
}

static HRESULT WINAPI gatt_descriptor_GetTrustLevel( IGattDescriptor *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_descriptor_get_ProtectionLevel( IGattDescriptor *iface, GattProtectionLevel *value )
{
    struct gatt_descriptor *impl = impl_from_IGattDescriptor( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->protection_level;
    return S_OK;
}

static HRESULT WINAPI gatt_descriptor_put_ProtectionLevel( IGattDescriptor *iface, GattProtectionLevel value )
{
    struct gatt_descriptor *impl = impl_from_IGattDescriptor( iface );
    TRACE( "(%p, %d)\n", iface, value );
    impl->protection_level = value;
    return S_OK;
}

static HRESULT WINAPI gatt_descriptor_get_Uuid( IGattDescriptor *iface, GUID *value )
{
    struct gatt_descriptor *impl = impl_from_IGattDescriptor( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->uuid;
    return S_OK;
}

static HRESULT WINAPI gatt_descriptor_get_AttributeHandle( IGattDescriptor *iface, UINT16 *value )
{
    struct gatt_descriptor *impl = impl_from_IGattDescriptor( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->handle;
    return S_OK;
}

static HRESULT gatt_descriptor_read_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    static const BYTE cccd_value[2];
    IGattReadResult *read_result;
    IBuffer *buffer;
    HRESULT hr;

    if (FAILED((hr = buffer_create( cccd_value, sizeof( cccd_value ), &buffer )))) return hr;
    hr = read_result_create( GattCommunicationStatus_Success, buffer, &read_result );
    IBuffer_Release( buffer );
    if (FAILED(hr)) return hr;
    return async_result_object( result, (IUnknown *)read_result );
}

static HRESULT WINAPI gatt_descriptor_ReadValueAsync( IGattDescriptor *iface, IAsyncOperation_GattReadResult **async )
{
    TRACE( "(%p, %p)\n", iface, async );
    return async_operation_inspectable_create( &IID_IAsyncOperation_GattReadResult, (IUnknown *)iface, NULL,
                                               gatt_descriptor_read_async, (IAsyncOperation_IInspectable **)async );
}

static HRESULT WINAPI gatt_descriptor_ReadValueWithCacheModeAsync( IGattDescriptor *iface, BluetoothCacheMode mode,
                                                                   IAsyncOperation_GattReadResult **async )
{
    TRACE( "(%p, %d, %p)\n", iface, mode, async );
    return gatt_descriptor_ReadValueAsync( iface, async );
}

static HRESULT gatt_descriptor_write( struct gatt_descriptor *impl, IBuffer *value, BOOL with_result, void **async )
{
    BYTE *data;
    UINT32 size;
    HRESULT hr;

    if (!value) return E_INVALIDARG;
    if (!IsEqualGUID( &impl->uuid, &cccd_uuid ))
    {
        FIXME( "Writes to descriptor %s are not supported\n", debugstr_guid( &impl->uuid ) );
        return E_NOTIMPL;
    }
    if (FAILED((hr = buffer_get_data( value, &data, &size )))) return hr;
    return gatt_characteristic_start_notify( &impl->characteristic->IGattCharacteristic_iface,
                                             size ? data[0] & 3 : 0, with_result, async );
}

static HRESULT WINAPI gatt_descriptor_WriteValueAsync( IGattDescriptor *iface, IBuffer *value,
                                                       IAsyncOperation_GattCommunicationStatus **async )
{
    struct gatt_descriptor *impl = impl_from_IGattDescriptor( iface );
    TRACE( "(%p, %p, %p)\n", iface, value, async );
    return gatt_descriptor_write( impl, value, FALSE, (void **)async );
}

static const IGattDescriptorVtbl gatt_descriptor_vtbl =
{
    gatt_descriptor_QueryInterface,
    gatt_descriptor_AddRef,
    gatt_descriptor_Release,
    gatt_descriptor_GetIids,
    gatt_descriptor_GetRuntimeClassName,
    gatt_descriptor_GetTrustLevel,
    gatt_descriptor_get_ProtectionLevel,
    gatt_descriptor_put_ProtectionLevel,
    gatt_descriptor_get_Uuid,
    gatt_descriptor_get_AttributeHandle,
    gatt_descriptor_ReadValueAsync,
    gatt_descriptor_ReadValueWithCacheModeAsync,
    gatt_descriptor_WriteValueAsync,
};

DEFINE_IINSPECTABLE( gatt_descriptor2, IGattDescriptor2, struct gatt_descriptor, IGattDescriptor_iface )

static HRESULT WINAPI gatt_descriptor2_WriteValueWithResultAsync( IGattDescriptor2 *iface, IBuffer *value,
                                                                  IAsyncOperation_GattWriteResult **async )
{
    struct gatt_descriptor *impl = impl_from_IGattDescriptor2( iface );
    TRACE( "(%p, %p, %p)\n", iface, value, async );
    return gatt_descriptor_write( impl, value, TRUE, (void **)async );
}

static const IGattDescriptor2Vtbl gatt_descriptor2_vtbl =
{
    gatt_descriptor2_QueryInterface,
    gatt_descriptor2_AddRef,
    gatt_descriptor2_Release,
    gatt_descriptor2_GetIids,
    gatt_descriptor2_GetRuntimeClassName,
    gatt_descriptor2_GetTrustLevel,
    gatt_descriptor2_WriteValueWithResultAsync,
};

DEFINE_IINSPECTABLE_( gatt_descriptor_closable, IClosable, struct gatt_descriptor, gatt_descriptor_from_IClosable,
                      IClosable_iface, &impl->IGattDescriptor_iface )

static HRESULT WINAPI gatt_descriptor_closable_Close( IClosable *iface )
{
    TRACE( "(%p)\n", iface );
    return S_OK;
}

static const IClosableVtbl gatt_descriptor_closable_vtbl =
{
    gatt_descriptor_closable_QueryInterface,
    gatt_descriptor_closable_AddRef,
    gatt_descriptor_closable_Release,
    gatt_descriptor_closable_GetIids,
    gatt_descriptor_closable_GetRuntimeClassName,
    gatt_descriptor_closable_GetTrustLevel,
    gatt_descriptor_closable_Close,
};

static HRESULT gatt_descriptor_create( struct gatt_characteristic *chrc, const GUID *uuid, UINT16 handle, IGattDescriptor **out )
{
    struct gatt_descriptor *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattDescriptor_iface.lpVtbl = &gatt_descriptor_vtbl;
    impl->IGattDescriptor2_iface.lpVtbl = &gatt_descriptor2_vtbl;
    impl->IClosable_iface.lpVtbl = &gatt_descriptor_closable_vtbl;
    impl->ref = 1;
    impl->characteristic = chrc;
    IGattCharacteristic_AddRef( &chrc->IGattCharacteristic_iface );
    impl->uuid = *uuid;
    impl->handle = handle;
    *out = &impl->IGattDescriptor_iface;
    return S_OK;
}

/* Build the descriptor list for a characteristic, optionally filtered by UUID. */
static HRESULT gatt_characteristic_get_descriptors_vector( struct gatt_characteristic *impl, const GUID *filter,
                                                           IVector_IInspectable **out )
{
    IVector_IInspectable *vector;
    HRESULT hr;

    if (FAILED((hr = vector_create( &descriptor_vector_iids, (void **)&vector )))) return hr;
    if ((impl->chrc.IsNotifiable || impl->chrc.IsIndicatable) && (!filter || IsEqualGUID( filter, &cccd_uuid )))
    {
        IGattDescriptor *descriptor;

        hr = gatt_descriptor_create( impl, &cccd_uuid, impl->chrc.CharacteristicValueHandle + 1, &descriptor );
        if (SUCCEEDED(hr))
        {
            hr = IVector_IInspectable_Append( vector, (IInspectable *)descriptor );
            IGattDescriptor_Release( descriptor );
        }
        if (FAILED(hr))
        {
            IVector_IInspectable_Release( vector );
            return hr;
        }
    }
    *out = vector;
    return S_OK;
}

/* --- GattDescriptorsResult --- */

struct descriptors_result
{
    IGattDescriptorsResult IGattDescriptorsResult_iface;
    LONG ref;
    GattCommunicationStatus status;
    IVectorView_GattDescriptor *descriptors;
};

DEFINE_SIMPLE_INSPECTABLE( descriptors_result, IGattDescriptorsResult, struct descriptors_result,
                           L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattDescriptorsResult",
                           if (impl->descriptors) IVectorView_GattDescriptor_Release( impl->descriptors ) )

static HRESULT WINAPI descriptors_result_get_Status( IGattDescriptorsResult *iface, GattCommunicationStatus *value )
{
    struct descriptors_result *impl = impl_from_IGattDescriptorsResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI descriptors_result_get_ProtocolError( IGattDescriptorsResult *iface, IReference_BYTE **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI descriptors_result_get_Descriptors( IGattDescriptorsResult *iface, IVectorView_GattDescriptor **value )
{
    struct descriptors_result *impl = impl_from_IGattDescriptorsResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->descriptors)) IVectorView_GattDescriptor_AddRef( *value );
    return S_OK;
}

static const IGattDescriptorsResultVtbl descriptors_result_vtbl =
{
    descriptors_result_QueryInterface,
    descriptors_result_AddRef,
    descriptors_result_Release,
    descriptors_result_GetIids,
    descriptors_result_GetRuntimeClassName,
    descriptors_result_GetTrustLevel,
    descriptors_result_get_Status,
    descriptors_result_get_ProtocolError,
    descriptors_result_get_Descriptors,
};

static HRESULT gatt_characteristic_get_descriptors_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( (IGattCharacteristic *)invoker );
    struct descriptors_result *descriptors_result;
    IVector_IInspectable *vector;
    GUID uuid, *filter = NULL;
    HRESULT hr;

    if (param)
    {
        if (FAILED((hr = unbox_guid( param, &uuid )))) return hr;
        filter = &uuid;
    }
    if (FAILED((hr = gatt_characteristic_get_descriptors_vector( impl, filter, &vector )))) return hr;
    if (!(descriptors_result = calloc( 1, sizeof( *descriptors_result ) )))
    {
        IVector_IInspectable_Release( vector );
        return E_OUTOFMEMORY;
    }
    descriptors_result->IGattDescriptorsResult_iface.lpVtbl = &descriptors_result_vtbl;
    descriptors_result->ref = 1;
    descriptors_result->status = GattCommunicationStatus_Success;
    hr = IVector_IInspectable_GetView( vector, (IVectorView_IInspectable **)&descriptors_result->descriptors );
    IVector_IInspectable_Release( vector );
    if (FAILED(hr))
    {
        free( descriptors_result );
        return hr;
    }
    return async_result_object( result, (IUnknown *)&descriptors_result->IGattDescriptorsResult_iface );
}

static HRESULT gatt_characteristic_start_descriptors_async( IGattCharacteristic *iface, const GUID *uuid,
                                                            IAsyncOperation_GattDescriptorsResult **async )
{
    IInspectable *param = NULL;
    HRESULT hr;

    if (uuid && FAILED((hr = box_guid( uuid, &param )))) return hr;
    hr = async_operation_inspectable_create( &IID_IAsyncOperation_GattDescriptorsResult, (IUnknown *)iface,
                                             (IUnknown *)param, gatt_characteristic_get_descriptors_async,
                                             (IAsyncOperation_IInspectable **)async );
    if (param) IInspectable_Release( param );
    return hr;
}

static HRESULT WINAPI gatt_characteristic_GetDescriptors( IGattCharacteristic *iface, GUID uuid, IVectorView_GattDescriptor **value )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    IVector_IInspectable *vector;
    HRESULT hr;

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( &uuid ), value );

    if (FAILED((hr = gatt_characteristic_get_descriptors_vector( impl, &uuid, &vector )))) return hr;
    hr = IVector_IInspectable_GetView( vector, (IVectorView_IInspectable **)value );
    IVector_IInspectable_Release( vector );
    return hr;
}

static HRESULT WINAPI gatt_characteristic_get_CharacteristicProperties( IGattCharacteristic *iface, GattCharacteristicProperties *value )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    GattCharacteristicProperties props = GattCharacteristicProperties_None;

    TRACE( "(%p, %p)\n", iface, value );

    if (impl->chrc.IsBroadcastable) props |= GattCharacteristicProperties_Broadcast;
    if (impl->chrc.IsReadable) props |= GattCharacteristicProperties_Read;
    if (impl->chrc.IsWritableWithoutResponse) props |= GattCharacteristicProperties_WriteWithoutResponse;
    if (impl->chrc.IsWritable) props |= GattCharacteristicProperties_Write;
    if (impl->chrc.IsNotifiable) props |= GattCharacteristicProperties_Notify;
    if (impl->chrc.IsIndicatable) props |= GattCharacteristicProperties_Indicate;
    if (impl->chrc.IsSignedWritable) props |= GattCharacteristicProperties_AuthenticatedSignedWrites;
    if (impl->chrc.HasExtendedProperties) props |= GattCharacteristicProperties_ExtendedProperties;
    *value = props;
    return S_OK;
}

static HRESULT WINAPI gatt_characteristic_get_ProtectionLevel( IGattCharacteristic *iface, GattProtectionLevel *value )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->protection_level;
    return S_OK;
}

static HRESULT WINAPI gatt_characteristic_put_ProtectionLevel( IGattCharacteristic *iface, GattProtectionLevel value )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    TRACE( "(%p, %d)\n", iface, value );
    impl->protection_level = value;
    return S_OK;
}

static HRESULT WINAPI gatt_characteristic_get_UserDescription( IGattCharacteristic *iface, HSTRING *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    return WindowsCreateString( NULL, 0, value );
}

static HRESULT WINAPI gatt_characteristic_get_Uuid( IGattCharacteristic *iface, GUID *value )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    TRACE( "(%p, %p)\n", iface, value );
    le_uuid_to_guid( &impl->chrc.CharacteristicUuid, value );
    return S_OK;
}

static HRESULT WINAPI gatt_characteristic_get_AttributeHandle( IGattCharacteristic *iface, UINT16 *value )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->chrc.AttributeHandle;
    return S_OK;
}

static HRESULT WINAPI gatt_characteristic_get_PresentationFormats( IGattCharacteristic *iface, IVectorView_GattPresentationFormat **value )
{
    FIXME( "(%p, %p): stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

/* Read the characteristic through the driver, returning a fresh buffer. */
static HRESULT gatt_characteristic_read( struct gatt_characteristic *impl, ULONG flags, IBuffer **buffer )
{
    BTH_LE_GATT_CHARACTERISTIC_VALUE *value;
    USHORT actual = 0;
    HANDLE service;
    HRESULT hr;

    *buffer = NULL;
    if ((service = gatt_service_get_handle( impl->service )) == INVALID_HANDLE_VALUE) return E_FAIL;
    hr = BluetoothGATTGetCharacteristicValue( service, &impl->chrc, 0, NULL, &actual, flags );
    if (hr != HRESULT_FROM_WIN32( ERROR_MORE_DATA ) && FAILED(hr)) return hr;
    if (!(value = calloc( 1, max( actual, sizeof( *value ) ) ))) return E_OUTOFMEMORY;
    hr = BluetoothGATTGetCharacteristicValue( service, &impl->chrc, actual, value, &actual, flags );
    if (SUCCEEDED(hr)) hr = buffer_create( value->Data, value->DataSize, buffer );
    free( value );
    return hr;
}

static HRESULT gatt_characteristic_read_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( (IGattCharacteristic *)invoker );
    IGattReadResult *read_result;
    IBuffer *buffer;
    UINT32 mode = BluetoothCacheMode_Uncached;
    HRESULT hr;

    if (param) unbox_uint32( param, &mode );
    hr = gatt_characteristic_read( impl, mode == BluetoothCacheMode_Cached ? BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_CACHE
                                                                           : BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE, &buffer );
    if (FAILED(hr)) WARN( "Read failed: %#lx\n", hr );
    hr = read_result_create( SUCCEEDED(hr) ? GattCommunicationStatus_Success : GattCommunicationStatus_Unreachable, buffer,
                             &read_result );
    if (buffer) IBuffer_Release( buffer );
    if (FAILED(hr)) return hr;
    return async_result_object( result, (IUnknown *)read_result );
}

static HRESULT WINAPI gatt_characteristic_ReadValueAsync( IGattCharacteristic *iface, IAsyncOperation_GattReadResult **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    return async_operation_inspectable_create( &IID_IAsyncOperation_GattReadResult, (IUnknown *)iface, NULL,
                                               gatt_characteristic_read_async, (IAsyncOperation_IInspectable **)value );
}

static HRESULT WINAPI gatt_characteristic_ReadValueWithCacheModeAsync( IGattCharacteristic *iface, BluetoothCacheMode mode,
                                                                       IAsyncOperation_GattReadResult **value )
{
    IInspectable *param;
    HRESULT hr;

    TRACE( "(%p, %d, %p)\n", iface, mode, value );

    if (FAILED((hr = box_uint32( mode, &param )))) return hr;
    hr = async_operation_inspectable_create( &IID_IAsyncOperation_GattReadResult, (IUnknown *)iface, (IUnknown *)param,
                                             gatt_characteristic_read_async, (IAsyncOperation_IInspectable **)value );
    IInspectable_Release( param );
    return hr;
}

/* Perform a write or notification change synchronously through the driver. */
static GattCommunicationStatus gatt_characteristic_perform_write( struct gatt_characteristic *impl,
                                                                  struct gatt_write_request *request )
{
    struct winebth_gatt_service_write_characteristic_value_params *params = NULL;
    struct winebth_gatt_service_set_characteristic_notify_params notify_params = {0};
    OVERLAPPED ovl = {0};
    DWORD bytes, err = ERROR_SUCCESS, size;
    void *in;
    DWORD code;
    HANDLE service;
    BOOL ret;

    if ((service = gatt_service_get_handle( impl->service )) == INVALID_HANDLE_VALUE)
        return GattCommunicationStatus_Unreachable;

    if (request->notify_request)
    {
        notify_params.uuid = impl->chrc.CharacteristicUuid;
        notify_params.handle = impl->chrc.AttributeHandle;
        notify_params.enable = request->notify_value != GattClientCharacteristicConfigurationDescriptorValue_None;
        code = IOCTL_WINEBTH_GATT_SERVICE_SET_CHARACTERISTIC_NOTIFY;
        in = &notify_params;
        size = sizeof( notify_params );
    }
    else
    {
        BYTE *data;
        UINT32 len;

        if (FAILED(buffer_get_data( request->value, &data, &len ))) return GattCommunicationStatus_Unreachable;
        size = offsetof( struct winebth_gatt_service_write_characteristic_value_params, buf[len] );
        if (!(params = calloc( 1, max( size, sizeof( *params ) ) ))) return GattCommunicationStatus_Unreachable;
        params->uuid = impl->chrc.CharacteristicUuid;
        params->handle = impl->chrc.AttributeHandle;
        params->without_response = !!request->without_response;
        params->size = len;
        memcpy( params->buf, data, len );
        code = IOCTL_WINEBTH_GATT_SERVICE_WRITE_CHARACTERISTIC_VALUE;
        in = params;
        size = max( size, sizeof( *params ) );
    }

    ovl.hEvent = CreateEventW( NULL, TRUE, FALSE, NULL );
    ret = DeviceIoControl( service, code, in, size, NULL, 0, &bytes, &ovl );
    if (!ret)
    {
        err = GetLastError();
        if (err == ERROR_IO_PENDING)
        {
            err = ERROR_SUCCESS;
            if (!GetOverlappedResult( service, &ovl, &bytes, TRUE )) err = GetLastError();
        }
    }
    CloseHandle( ovl.hEvent );
    free( params );

    if (err)
    {
        WARN( "GATT operation %#lx failed: %lu\n", code, err );
        return err == ERROR_PRIVILEGE_NOT_HELD || err == ERROR_ACCESS_DENIED ? GattCommunicationStatus_AccessDenied
                                                                              : GattCommunicationStatus_Unreachable;
    }
    return GattCommunicationStatus_Success;
}

static HRESULT gatt_characteristic_write_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( (IGattCharacteristic *)invoker );
    struct gatt_write_request *request = impl_from_write_request( param );
    GattCommunicationStatus status;

    status = gatt_characteristic_perform_write( impl, request );
    if (request->with_result)
    {
        IGattWriteResult *write_result;
        HRESULT hr;

        if (FAILED((hr = write_result_create( status, &write_result )))) return hr;
        return async_result_object( result, (IUnknown *)write_result );
    }
    return async_result_uint32( result, status );
}

static HRESULT gatt_characteristic_start_write( IGattCharacteristic *iface, IBuffer *value, BOOL without_response,
                                                BOOL with_result, void **async )
{
    struct gatt_write_request *request;
    HRESULT hr;

    if (!value) return E_INVALIDARG;
    if (FAILED((hr = write_request_create( value, without_response, with_result, &request )))) return hr;
    if (with_result)
        hr = async_operation_inspectable_create( &IID_IAsyncOperation_GattWriteResult, (IUnknown *)iface,
                                                 &request->IUnknown_iface, gatt_characteristic_write_async,
                                                 (IAsyncOperation_IInspectable **)async );
    else
        hr = async_operation_uint32_create( &IID_IAsyncOperation_GattCommunicationStatus, (IUnknown *)iface,
                                            &request->IUnknown_iface, gatt_characteristic_write_async,
                                            (IAsyncOperation_IInspectable **)async );
    IUnknown_Release( &request->IUnknown_iface );
    return hr;
}

static HRESULT gatt_characteristic_start_notify( IGattCharacteristic *iface,
                                                 GattClientCharacteristicConfigurationDescriptorValue value,
                                                 BOOL with_result, void **async )
{
    struct gatt_write_request *request;
    HRESULT hr;

    if (FAILED((hr = write_request_create( NULL, FALSE, with_result, &request )))) return hr;
    request->notify_request = TRUE;
    request->notify_value = value;
    if (with_result)
        hr = async_operation_inspectable_create( &IID_IAsyncOperation_GattWriteResult, (IUnknown *)iface,
                                                 &request->IUnknown_iface, gatt_characteristic_write_async,
                                                 (IAsyncOperation_IInspectable **)async );
    else
        hr = async_operation_uint32_create( &IID_IAsyncOperation_GattCommunicationStatus, (IUnknown *)iface,
                                            &request->IUnknown_iface, gatt_characteristic_write_async,
                                            (IAsyncOperation_IInspectable **)async );
    IUnknown_Release( &request->IUnknown_iface );
    return hr;
}

static HRESULT WINAPI gatt_characteristic_WriteValueAsync( IGattCharacteristic *iface, IBuffer *value,
                                                           IAsyncOperation_GattCommunicationStatus **async )
{
    TRACE( "(%p, %p, %p)\n", iface, value, async );
    return gatt_characteristic_start_write( iface, value, FALSE, FALSE, (void **)async );
}

static HRESULT WINAPI gatt_characteristic_WriteValueWithOptionAsync( IGattCharacteristic *iface, IBuffer *value, GattWriteOption option,
                                                                     IAsyncOperation_GattCommunicationStatus **async )
{
    TRACE( "(%p, %p, %d, %p)\n", iface, value, option, async );
    return gatt_characteristic_start_write( iface, value, option == GattWriteOption_WriteWithoutResponse, FALSE, (void **)async );
}

static HRESULT WINAPI gatt_characteristic_ReadClientCharacteristicConfigurationDescriptorAsync(
    IGattCharacteristic *iface, IAsyncOperation_GattReadClientCharacteristicConfigurationDescriptorResult **async )
{
    FIXME( "(%p, %p): stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_characteristic_WriteClientCharacteristicConfigurationDescriptorAsync(
    IGattCharacteristic *iface, GattClientCharacteristicConfigurationDescriptorValue value,
    IAsyncOperation_GattCommunicationStatus **async )
{
    TRACE( "(%p, %d, %p)\n", iface, value, async );
    return gatt_characteristic_start_notify( iface, value, FALSE, (void **)async );
}

static void gatt_characteristic_dispatch_value_changed( struct gatt_characteristic *impl, const BYTE *data, UINT32 size )
{
    IGattValueChangedEventArgs *args;
    IUnknown **handlers;
    UINT32 i, count;

    if (!(count = event_handlers_snapshot( &impl->value_changed, &handlers ))) return;
    if (SUCCEEDED(value_changed_args_create( data, size, &args )))
    {
        for (i = 0; i < count; i++)
            ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Invoke(
                (ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs *)handlers[i],
                &impl->IGattCharacteristic_iface, args );
        IGattValueChangedEventArgs_Release( args );
    }
    for (i = 0; i < count; i++) IUnknown_Release( handlers[i] );
    free( handlers );
}

static DWORD CALLBACK gatt_characteristic_notify_callback( HCMNOTIFICATION notify, void *ctx, CM_NOTIFY_ACTION action,
                                                           CM_NOTIFY_EVENT_DATA *event_data, DWORD size )
{
    struct gatt_characteristic *impl = ctx;
    const struct winebth_gatt_value_changed *changed;

    if (action != CM_NOTIFY_ACTION_DEVICECUSTOMEVENT) return ERROR_SUCCESS;
    if (!IsEqualGUID( &event_data->u.DeviceHandle.EventGuid, &GUID_WINEBTH_GATT_VALUE_CHANGED )) return ERROR_SUCCESS;
    changed = (const struct winebth_gatt_value_changed *)event_data->u.DeviceHandle.Data;
    if (event_data->u.DeviceHandle.DataSize < offsetof( struct winebth_gatt_value_changed, data[changed->size] ))
        return ERROR_SUCCESS;
    if (changed->handle != impl->chrc.AttributeHandle) return ERROR_SUCCESS;

    TRACE( "characteristic %p handle %#x size %lu\n", impl, changed->handle, changed->size );
    gatt_characteristic_dispatch_value_changed( impl, changed->data, changed->size );
    return ERROR_SUCCESS;
}

static HRESULT WINAPI gatt_characteristic_add_ValueChanged( IGattCharacteristic *iface,
                                                            ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs *handler,
                                                            EventRegistrationToken *token )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    CM_NOTIFY_FILTER filter = { .cbSize = sizeof( filter ), .FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE };
    HRESULT hr;

    TRACE( "(%p, %p, %p)\n", iface, handler, token );

    if (FAILED((hr = event_handlers_add( &impl->value_changed, (IUnknown *)handler, token )))) return hr;

    EnterCriticalSection( &impl->value_changed.cs );
    if (!impl->notification)
    {
        HANDLE service = gatt_service_get_handle( impl->service );
        CONFIGRET ret;

        if (service == INVALID_HANDLE_VALUE)
            WARN( "No service handle, value changes will not be delivered\n" );
        else
        {
            filter.u.DeviceHandle.hTarget = service;
            if ((ret = CM_Register_Notification( &filter, impl, gatt_characteristic_notify_callback, &impl->notification )))
                ERR( "CM_Register_Notification failed: %#lx\n", ret );
        }
    }
    LeaveCriticalSection( &impl->value_changed.cs );
    return S_OK;
}

static HRESULT WINAPI gatt_characteristic_remove_ValueChanged( IGattCharacteristic *iface, EventRegistrationToken token )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    TRACE( "(%p, %I64x)\n", iface, token.value );
    return event_handlers_remove( &impl->value_changed, token );
}

static const IGattCharacteristicVtbl gatt_characteristic_vtbl =
{
    gatt_characteristic_QueryInterface,
    gatt_characteristic_AddRef,
    gatt_characteristic_Release,
    gatt_characteristic_GetIids,
    gatt_characteristic_GetRuntimeClassName,
    gatt_characteristic_GetTrustLevel,
    gatt_characteristic_GetDescriptors,
    gatt_characteristic_get_CharacteristicProperties,
    gatt_characteristic_get_ProtectionLevel,
    gatt_characteristic_put_ProtectionLevel,
    gatt_characteristic_get_UserDescription,
    gatt_characteristic_get_Uuid,
    gatt_characteristic_get_AttributeHandle,
    gatt_characteristic_get_PresentationFormats,
    gatt_characteristic_ReadValueAsync,
    gatt_characteristic_ReadValueWithCacheModeAsync,
    gatt_characteristic_WriteValueAsync,
    gatt_characteristic_WriteValueWithOptionAsync,
    gatt_characteristic_ReadClientCharacteristicConfigurationDescriptorAsync,
    gatt_characteristic_WriteClientCharacteristicConfigurationDescriptorAsync,
    gatt_characteristic_add_ValueChanged,
    gatt_characteristic_remove_ValueChanged,
};

DEFINE_IINSPECTABLE( gatt_characteristic2, IGattCharacteristic2, struct gatt_characteristic, IGattCharacteristic_iface )

static HRESULT WINAPI gatt_characteristic2_get_Service( IGattCharacteristic2 *iface, IGattDeviceService **value )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic2( iface );
    TRACE( "(%p, %p)\n", iface, value );
    IGattDeviceService_AddRef(( *value = &impl->service->IGattDeviceService_iface ));
    return S_OK;
}

static HRESULT WINAPI gatt_characteristic2_GetAllDescriptors( IGattCharacteristic2 *iface, IVectorView_GattDescriptor **value )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic2( iface );
    IVector_IInspectable *vector;
    HRESULT hr;

    TRACE( "(%p, %p)\n", iface, value );

    if (FAILED((hr = gatt_characteristic_get_descriptors_vector( impl, NULL, &vector )))) return hr;
    hr = IVector_IInspectable_GetView( vector, (IVectorView_IInspectable **)value );
    IVector_IInspectable_Release( vector );
    return hr;
}

static const IGattCharacteristic2Vtbl gatt_characteristic2_vtbl =
{
    gatt_characteristic2_QueryInterface,
    gatt_characteristic2_AddRef,
    gatt_characteristic2_Release,
    gatt_characteristic2_GetIids,
    gatt_characteristic2_GetRuntimeClassName,
    gatt_characteristic2_GetTrustLevel,
    gatt_characteristic2_get_Service,
    gatt_characteristic2_GetAllDescriptors,
};

DEFINE_IINSPECTABLE( gatt_characteristic3, IGattCharacteristic3, struct gatt_characteristic, IGattCharacteristic_iface )

static HRESULT WINAPI gatt_characteristic3_GetDescriptorsAsync( IGattCharacteristic3 *iface,
                                                                IAsyncOperation_GattDescriptorsResult **async )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic3( iface );
    TRACE( "(%p, %p)\n", iface, async );
    return gatt_characteristic_start_descriptors_async( &impl->IGattCharacteristic_iface, NULL, async );
}

static HRESULT WINAPI gatt_characteristic3_GetDescriptorsWithCacheModeAsync( IGattCharacteristic3 *iface, BluetoothCacheMode mode,
                                                                             IAsyncOperation_GattDescriptorsResult **async )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic3( iface );
    TRACE( "(%p, %d, %p)\n", iface, mode, async );
    return gatt_characteristic_start_descriptors_async( &impl->IGattCharacteristic_iface, NULL, async );
}

static HRESULT WINAPI gatt_characteristic3_GetDescriptorsForUuidAsync( IGattCharacteristic3 *iface, GUID uuid,
                                                                       IAsyncOperation_GattDescriptorsResult **async )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic3( iface );
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( &uuid ), async );
    return gatt_characteristic_start_descriptors_async( &impl->IGattCharacteristic_iface, &uuid, async );
}

static HRESULT WINAPI gatt_characteristic3_GetDescriptorsForUuidWithCacheModeAsync( IGattCharacteristic3 *iface, GUID uuid,
                                                                                    BluetoothCacheMode mode,
                                                                                    IAsyncOperation_GattDescriptorsResult **async )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic3( iface );
    TRACE( "(%p, %s, %d, %p)\n", iface, debugstr_guid( &uuid ), mode, async );
    return gatt_characteristic_start_descriptors_async( &impl->IGattCharacteristic_iface, &uuid, async );
}

static HRESULT WINAPI gatt_characteristic3_WriteValueWithResultAsync( IGattCharacteristic3 *iface, IBuffer *value,
                                                                      IAsyncOperation_GattWriteResult **async )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic3( iface );
    TRACE( "(%p, %p, %p)\n", iface, value, async );
    return gatt_characteristic_start_write( &impl->IGattCharacteristic_iface, value, FALSE, TRUE, (void **)async );
}

static HRESULT WINAPI gatt_characteristic3_WriteValueWithResultAndOptionAsync( IGattCharacteristic3 *iface, IBuffer *value,
                                                                               GattWriteOption option,
                                                                               IAsyncOperation_GattWriteResult **async )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic3( iface );
    TRACE( "(%p, %p, %d, %p)\n", iface, value, option, async );
    return gatt_characteristic_start_write( &impl->IGattCharacteristic_iface, value,
                                            option == GattWriteOption_WriteWithoutResponse, TRUE, (void **)async );
}

static HRESULT WINAPI gatt_characteristic3_WriteClientCharacteristicConfigurationDescriptorWithResultAsync(
    IGattCharacteristic3 *iface, GattClientCharacteristicConfigurationDescriptorValue value,
    IAsyncOperation_GattWriteResult **async )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic3( iface );
    TRACE( "(%p, %d, %p)\n", iface, value, async );
    return gatt_characteristic_start_notify( &impl->IGattCharacteristic_iface, value, TRUE, (void **)async );
}

static const IGattCharacteristic3Vtbl gatt_characteristic3_vtbl =
{
    gatt_characteristic3_QueryInterface,
    gatt_characteristic3_AddRef,
    gatt_characteristic3_Release,
    gatt_characteristic3_GetIids,
    gatt_characteristic3_GetRuntimeClassName,
    gatt_characteristic3_GetTrustLevel,
    gatt_characteristic3_GetDescriptorsAsync,
    gatt_characteristic3_GetDescriptorsWithCacheModeAsync,
    gatt_characteristic3_GetDescriptorsForUuidAsync,
    gatt_characteristic3_GetDescriptorsForUuidWithCacheModeAsync,
    gatt_characteristic3_WriteValueWithResultAsync,
    gatt_characteristic3_WriteValueWithResultAndOptionAsync,
    gatt_characteristic3_WriteClientCharacteristicConfigurationDescriptorWithResultAsync,
};

DEFINE_IINSPECTABLE_( gatt_characteristic_closable, IClosable, struct gatt_characteristic, gatt_characteristic_from_IClosable,
                      IClosable_iface, &impl->IGattCharacteristic_iface )

static HRESULT WINAPI gatt_characteristic_closable_Close( IClosable *iface )
{
    TRACE( "(%p)\n", iface );
    return S_OK;
}

static const IClosableVtbl gatt_characteristic_closable_vtbl =
{
    gatt_characteristic_closable_QueryInterface,
    gatt_characteristic_closable_AddRef,
    gatt_characteristic_closable_Release,
    gatt_characteristic_closable_GetIids,
    gatt_characteristic_closable_GetRuntimeClassName,
    gatt_characteristic_closable_GetTrustLevel,
    gatt_characteristic_closable_Close,
};

static HRESULT gatt_characteristic_create( struct gatt_service *service, const BTH_LE_GATT_CHARACTERISTIC *chrc,
                                           IGattCharacteristic **out )
{
    struct gatt_characteristic *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattCharacteristic_iface.lpVtbl = &gatt_characteristic_vtbl;
    impl->IGattCharacteristic2_iface.lpVtbl = &gatt_characteristic2_vtbl;
    impl->IGattCharacteristic3_iface.lpVtbl = &gatt_characteristic3_vtbl;
    impl->IClosable_iface.lpVtbl = &gatt_characteristic_closable_vtbl;
    impl->ref = 1;
    impl->chrc = *chrc;
    impl->service = service;
    IGattDeviceService_AddRef( &service->IGattDeviceService_iface );
    event_handlers_init( &impl->value_changed );
    *out = &impl->IGattCharacteristic_iface;
    return S_OK;
}
