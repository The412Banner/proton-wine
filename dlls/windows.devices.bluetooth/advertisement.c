/* windows.Devices.Bluetooth.Advertisement Implementation
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
#include "initguid.h"
#include "robuffer.h"
#include "roapi.h"
#include "winioctl.h"
#include "setupapi.h"
#include "cfgmgr32.h"
#include "wine/winebth.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL( bluetooth );

/* --- Helpers --- */

static IBufferFactory *buffer_factory;

static HRESULT get_buffer_factory( IBufferFactory **factory )
{
    static const WCHAR class_name[] = L"Windows.Storage.Streams.Buffer";
    IBufferFactory *created;
    HSTRING_HEADER hdr;
    HSTRING str;
    HRESULT hr;

    if ((*factory = buffer_factory)) return S_OK;
    if (FAILED((hr = WindowsCreateStringReference( class_name, ARRAY_SIZE( class_name ) - 1, &hdr, &str )))) return hr;
    if (FAILED((hr = RoGetActivationFactory( str, &IID_IBufferFactory, (void **)&created )))) return hr;
    if (InterlockedCompareExchangePointer( (void **)&buffer_factory, created, NULL ))
        IBufferFactory_Release( created );
    *factory = buffer_factory;
    return S_OK;
}

HRESULT buffer_create( const BYTE *data, UINT32 size, IBuffer **out )
{
    IBufferByteAccess *access;
    IBufferFactory *factory;
    HRESULT hr;
    BYTE *bytes;

    *out = NULL;
    if (FAILED((hr = get_buffer_factory( &factory )))) return hr;
    if (FAILED((hr = IBufferFactory_Create( factory, size, out )))) return hr;

    if (FAILED((hr = IBuffer_QueryInterface( *out, &IID_IBufferByteAccess, (void **)&access ))))
    {
        IBuffer_Release( *out );
        *out = NULL;
        return hr;
    }
    hr = IBufferByteAccess_Buffer( access, &bytes );
    IBufferByteAccess_Release( access );
    if (SUCCEEDED(hr))
    {
        memcpy( bytes, data, size );
        hr = IBuffer_put_Length( *out, size );
    }
    if (FAILED(hr))
    {
        IBuffer_Release( *out );
        *out = NULL;
    }
    return hr;
}

HRESULT class_name_string( const WCHAR *name, HSTRING *out )
{
    return WindowsCreateString( name, wcslen( name ), out );
}

HRESULT buffer_get_data( IBuffer *buffer, BYTE **data, UINT32 *size )
{
    IBufferByteAccess *access;
    HRESULT hr;

    if (FAILED((hr = IBuffer_get_Length( buffer, size )))) return hr;
    if (FAILED((hr = IBuffer_QueryInterface( buffer, &IID_IBufferByteAccess, (void **)&access )))) return hr;
    hr = IBufferByteAccess_Buffer( access, data );
    IBufferByteAccess_Release( access );
    return hr;
}

/* A UUID in the Bluetooth base range that fits in 16 or 32 bits. */
static BOOL uuid_short_value( const GUID *uuid, UINT32 *value )
{
    static const GUID base = { 0, 0, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };
    if (uuid->Data2 != base.Data2 || uuid->Data3 != base.Data3 || memcmp( uuid->Data4, base.Data4, sizeof( base.Data4 ) ))
        return FALSE;
    *value = uuid->Data1;
    return TRUE;
}

/* Bluetooth transmits 128-bit UUIDs least significant byte first. */
static void uuid_to_le_bytes( const GUID *uuid, BYTE out[16] )
{
    int i;
    for (i = 0; i < 8; i++) out[i] = uuid->Data4[7 - i];
    out[8] = uuid->Data3 & 0xff;
    out[9] = uuid->Data3 >> 8;
    out[10] = uuid->Data2 & 0xff;
    out[11] = uuid->Data2 >> 8;
    out[12] = uuid->Data1 & 0xff;
    out[13] = (uuid->Data1 >> 8) & 0xff;
    out[14] = (uuid->Data1 >> 16) & 0xff;
    out[15] = uuid->Data1 >> 24;
}

/* --- IVector<GUID> --- */

struct guid_vector
{
    IVector_GUID IVector_GUID_iface;
    IVectorView_GUID IVectorView_GUID_iface;
    IIterable_GUID IIterable_GUID_iface;
    LONG ref;
    BOOL view;
    GUID *items;
    UINT32 count;
    UINT32 capacity;
};

static HRESULT guid_vector_create( const GUID *items, UINT32 count, BOOL view, struct guid_vector **out );

static inline struct guid_vector *impl_from_IVector_GUID( IVector_GUID *iface )
{
    return CONTAINING_RECORD( iface, struct guid_vector, IVector_GUID_iface );
}

static HRESULT WINAPI guid_vector_QueryInterface( IVector_GUID *iface, REFIID iid, void **out )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        (!impl->view && IsEqualGUID( iid, &IID_IVector_GUID )))
    {
        IVector_GUID_AddRef(( *out = &impl->IVector_GUID_iface ));
        return S_OK;
    }
    if (impl->view && IsEqualGUID( iid, &IID_IVectorView_GUID ))
    {
        IVector_GUID_AddRef( iface );
        *out = &impl->IVectorView_GUID_iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IIterable_GUID ))
    {
        IVector_GUID_AddRef( iface );
        *out = &impl->IIterable_GUID_iface;
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI guid_vector_AddRef( IVector_GUID *iface )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI guid_vector_Release( IVector_GUID *iface )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        free( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI guid_vector_GetIids( IVector_GUID *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI guid_vector_GetRuntimeClassName( IVector_GUID *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Foundation.Collections.IVector`1<Guid>", class_name );
}

static HRESULT WINAPI guid_vector_GetTrustLevel( IVector_GUID *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI guid_vector_GetAt( IVector_GUID *iface, UINT32 index, GUID *value )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    if (index >= impl->count) return E_BOUNDS;
    *value = impl->items[index];
    return S_OK;
}

static HRESULT WINAPI guid_vector_get_Size( IVector_GUID *iface, UINT32 *value )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI guid_vector_GetView( IVector_GUID *iface, IVectorView_GUID **value )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface ), *view;
    HRESULT hr;

    TRACE( "(%p, %p)\n", iface, value );

    if (FAILED((hr = guid_vector_create( impl->items, impl->count, TRUE, &view )))) return hr;
    *value = &view->IVectorView_GUID_iface;
    return S_OK;
}

static HRESULT WINAPI guid_vector_IndexOf( IVector_GUID *iface, GUID element, UINT32 *index, BOOLEAN *found )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    UINT32 i;

    *found = FALSE;
    *index = 0;
    for (i = 0; i < impl->count; i++)
    {
        if (IsEqualGUID( &impl->items[i], &element ))
        {
            *index = i;
            *found = TRUE;
            break;
        }
    }
    return S_OK;
}

static HRESULT guid_vector_reserve( struct guid_vector *impl, UINT32 count )
{
    GUID *items;
    UINT32 capacity;

    if (count <= impl->capacity) return S_OK;
    capacity = max( count, impl->capacity * 2 );
    if (!(items = realloc( impl->items, capacity * sizeof( *items ) ))) return E_OUTOFMEMORY;
    impl->items = items;
    impl->capacity = capacity;
    return S_OK;
}

static HRESULT WINAPI guid_vector_SetAt( IVector_GUID *iface, UINT32 index, GUID value )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    if (index >= impl->count) return E_BOUNDS;
    impl->items[index] = value;
    return S_OK;
}

static HRESULT WINAPI guid_vector_InsertAt( IVector_GUID *iface, UINT32 index, GUID value )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    HRESULT hr;

    if (index > impl->count) return E_BOUNDS;
    if (FAILED((hr = guid_vector_reserve( impl, impl->count + 1 )))) return hr;
    memmove( &impl->items[index + 1], &impl->items[index], (impl->count - index) * sizeof( GUID ) );
    impl->items[index] = value;
    impl->count++;
    return S_OK;
}

static HRESULT WINAPI guid_vector_RemoveAt( IVector_GUID *iface, UINT32 index )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    if (index >= impl->count) return E_BOUNDS;
    memmove( &impl->items[index], &impl->items[index + 1], (impl->count - index - 1) * sizeof( GUID ) );
    impl->count--;
    return S_OK;
}

static HRESULT WINAPI guid_vector_Append( IVector_GUID *iface, GUID value )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    return guid_vector_InsertAt( iface, impl->count, value );
}

static HRESULT WINAPI guid_vector_RemoveAtEnd( IVector_GUID *iface )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    if (!impl->count) return E_BOUNDS;
    impl->count--;
    return S_OK;
}

static HRESULT WINAPI guid_vector_Clear( IVector_GUID *iface )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    impl->count = 0;
    return S_OK;
}

static HRESULT WINAPI guid_vector_GetMany( IVector_GUID *iface, UINT32 start_index, UINT32 items_size, GUID *items,
                                           UINT32 *count )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );

    if (start_index > impl->count) return E_BOUNDS;
    *count = min( items_size, impl->count - start_index );
    memcpy( items, &impl->items[start_index], *count * sizeof( GUID ) );
    return S_OK;
}

static HRESULT WINAPI guid_vector_ReplaceAll( IVector_GUID *iface, UINT32 count, GUID *items )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    HRESULT hr;

    if (FAILED((hr = guid_vector_reserve( impl, count )))) return hr;
    memcpy( impl->items, items, count * sizeof( GUID ) );
    impl->count = count;
    return S_OK;
}

static const IVector_GUIDVtbl guid_vector_vtbl =
{
    guid_vector_QueryInterface,
    guid_vector_AddRef,
    guid_vector_Release,
    guid_vector_GetIids,
    guid_vector_GetRuntimeClassName,
    guid_vector_GetTrustLevel,
    guid_vector_GetAt,
    guid_vector_get_Size,
    guid_vector_GetView,
    guid_vector_IndexOf,
    guid_vector_SetAt,
    guid_vector_InsertAt,
    guid_vector_RemoveAt,
    guid_vector_Append,
    guid_vector_RemoveAtEnd,
    guid_vector_Clear,
    guid_vector_GetMany,
    guid_vector_ReplaceAll,
};

DEFINE_IINSPECTABLE( guid_vector_view, IVectorView_GUID, struct guid_vector, IVector_GUID_iface )

static HRESULT WINAPI guid_vector_view_GetAt( IVectorView_GUID *iface, UINT32 index, GUID *value )
{
    struct guid_vector *impl = impl_from_IVectorView_GUID( iface );
    return guid_vector_GetAt( &impl->IVector_GUID_iface, index, value );
}

static HRESULT WINAPI guid_vector_view_get_Size( IVectorView_GUID *iface, UINT32 *value )
{
    struct guid_vector *impl = impl_from_IVectorView_GUID( iface );
    return guid_vector_get_Size( &impl->IVector_GUID_iface, value );
}

static HRESULT WINAPI guid_vector_view_IndexOf( IVectorView_GUID *iface, GUID element, UINT32 *index, BOOLEAN *found )
{
    struct guid_vector *impl = impl_from_IVectorView_GUID( iface );
    return guid_vector_IndexOf( &impl->IVector_GUID_iface, element, index, found );
}

static HRESULT WINAPI guid_vector_view_GetMany( IVectorView_GUID *iface, UINT32 start_index, UINT32 items_size,
                                                GUID *items, UINT32 *count )
{
    struct guid_vector *impl = impl_from_IVectorView_GUID( iface );
    return guid_vector_GetMany( &impl->IVector_GUID_iface, start_index, items_size, items, count );
}

static const IVectorView_GUIDVtbl guid_vector_view_vtbl =
{
    guid_vector_view_QueryInterface,
    guid_vector_view_AddRef,
    guid_vector_view_Release,
    guid_vector_view_GetIids,
    guid_vector_view_GetRuntimeClassName,
    guid_vector_view_GetTrustLevel,
    guid_vector_view_GetAt,
    guid_vector_view_get_Size,
    guid_vector_view_IndexOf,
    guid_vector_view_GetMany,
};

struct guid_iterator
{
    IIterator_GUID IIterator_GUID_iface;
    LONG ref;
    struct guid_vector *vector;
    UINT32 index;
};

static inline struct guid_iterator *impl_from_IIterator_GUID( IIterator_GUID *iface )
{
    return CONTAINING_RECORD( iface, struct guid_iterator, IIterator_GUID_iface );
}

static HRESULT WINAPI guid_iterator_QueryInterface( IIterator_GUID *iface, REFIID iid, void **out )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IIterator_GUID ))
    {
        IIterator_GUID_AddRef(( *out = &impl->IIterator_GUID_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI guid_iterator_AddRef( IIterator_GUID *iface )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI guid_iterator_Release( IIterator_GUID *iface )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        IVector_GUID_Release( &impl->vector->IVector_GUID_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI guid_iterator_GetIids( IIterator_GUID *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI guid_iterator_GetRuntimeClassName( IIterator_GUID *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Foundation.Collections.IIterator`1<Guid>", class_name );
}

static HRESULT WINAPI guid_iterator_GetTrustLevel( IIterator_GUID *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI guid_iterator_get_Current( IIterator_GUID *iface, GUID *value )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );
    return guid_vector_GetAt( &impl->vector->IVector_GUID_iface, impl->index, value );
}

static HRESULT WINAPI guid_iterator_get_HasCurrent( IIterator_GUID *iface, boolean *value )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );
    *value = impl->index < impl->vector->count;
    return S_OK;
}

static HRESULT WINAPI guid_iterator_MoveNext( IIterator_GUID *iface, boolean *value )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );
    if (impl->index < impl->vector->count) impl->index++;
    *value = impl->index < impl->vector->count;
    return S_OK;
}

static HRESULT WINAPI guid_iterator_GetMany( IIterator_GUID *iface, UINT32 items_size, GUID *items, UINT32 *count )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );
    HRESULT hr;

    hr = guid_vector_GetMany( &impl->vector->IVector_GUID_iface, impl->index, items_size, items, count );
    if (SUCCEEDED(hr)) impl->index += *count;
    return hr;
}

static const IIterator_GUIDVtbl guid_iterator_vtbl =
{
    guid_iterator_QueryInterface,
    guid_iterator_AddRef,
    guid_iterator_Release,
    guid_iterator_GetIids,
    guid_iterator_GetRuntimeClassName,
    guid_iterator_GetTrustLevel,
    guid_iterator_get_Current,
    guid_iterator_get_HasCurrent,
    guid_iterator_MoveNext,
    guid_iterator_GetMany,
};

DEFINE_IINSPECTABLE( guid_iterable, IIterable_GUID, struct guid_vector, IVector_GUID_iface )

static HRESULT WINAPI guid_iterable_First( IIterable_GUID *iface, IIterator_GUID **value )
{
    struct guid_vector *impl = impl_from_IIterable_GUID( iface );
    struct guid_iterator *iter;

    TRACE( "(%p, %p)\n", iface, value );

    if (!(iter = calloc( 1, sizeof( *iter ) ))) return E_OUTOFMEMORY;
    iter->IIterator_GUID_iface.lpVtbl = &guid_iterator_vtbl;
    iter->ref = 1;
    iter->vector = impl;
    IVector_GUID_AddRef( &impl->IVector_GUID_iface );
    *value = &iter->IIterator_GUID_iface;
    return S_OK;
}

static const IIterable_GUIDVtbl guid_iterable_vtbl =
{
    guid_iterable_QueryInterface,
    guid_iterable_AddRef,
    guid_iterable_Release,
    guid_iterable_GetIids,
    guid_iterable_GetRuntimeClassName,
    guid_iterable_GetTrustLevel,
    guid_iterable_First,
};

static HRESULT guid_vector_create( const GUID *items, UINT32 count, BOOL view, struct guid_vector **out )
{
    struct guid_vector *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IVector_GUID_iface.lpVtbl = &guid_vector_vtbl;
    impl->IVectorView_GUID_iface.lpVtbl = &guid_vector_view_vtbl;
    impl->IIterable_GUID_iface.lpVtbl = &guid_iterable_vtbl;
    impl->ref = 1;
    impl->view = view;
    if (count)
    {
        if (!(impl->items = malloc( count * sizeof( GUID ) )))
        {
            free( impl );
            return E_OUTOFMEMORY;
        }
        memcpy( impl->items, items, count * sizeof( GUID ) );
        impl->count = impl->capacity = count;
    }
    *out = impl;
    return S_OK;
}

/* --- BluetoothLEManufacturerData --- */

struct manufacturer_data
{
    IBluetoothLEManufacturerData IBluetoothLEManufacturerData_iface;
    LONG ref;
    UINT16 company_id;
    IBuffer *data;
};

static inline struct manufacturer_data *impl_from_IBluetoothLEManufacturerData( IBluetoothLEManufacturerData *iface )
{
    return CONTAINING_RECORD( iface, struct manufacturer_data, IBluetoothLEManufacturerData_iface );
}

static HRESULT WINAPI manufacturer_data_QueryInterface( IBluetoothLEManufacturerData *iface, REFIID iid, void **out )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEManufacturerData ))
    {
        IBluetoothLEManufacturerData_AddRef(( *out = &impl->IBluetoothLEManufacturerData_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI manufacturer_data_AddRef( IBluetoothLEManufacturerData *iface )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI manufacturer_data_Release( IBluetoothLEManufacturerData *iface )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        if (impl->data) IBuffer_Release( impl->data );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI manufacturer_data_GetIids( IBluetoothLEManufacturerData *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI manufacturer_data_GetRuntimeClassName( IBluetoothLEManufacturerData *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEManufacturerData", class_name );
}

static HRESULT WINAPI manufacturer_data_GetTrustLevel( IBluetoothLEManufacturerData *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI manufacturer_data_get_CompanyId( IBluetoothLEManufacturerData *iface, UINT16 *value )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->company_id;
    return S_OK;
}

static HRESULT WINAPI manufacturer_data_put_CompanyId( IBluetoothLEManufacturerData *iface, UINT16 value )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );
    TRACE( "(%p, %u)\n", iface, value );
    impl->company_id = value;
    return S_OK;
}

static HRESULT WINAPI manufacturer_data_get_Data( IBluetoothLEManufacturerData *iface, IBuffer **value )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->data)) IBuffer_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI manufacturer_data_put_Data( IBluetoothLEManufacturerData *iface, IBuffer *value )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (value) IBuffer_AddRef( value );
    if (impl->data) IBuffer_Release( impl->data );
    impl->data = value;
    return S_OK;
}

static const IBluetoothLEManufacturerDataVtbl manufacturer_data_vtbl =
{
    manufacturer_data_QueryInterface,
    manufacturer_data_AddRef,
    manufacturer_data_Release,
    manufacturer_data_GetIids,
    manufacturer_data_GetRuntimeClassName,
    manufacturer_data_GetTrustLevel,
    manufacturer_data_get_CompanyId,
    manufacturer_data_put_CompanyId,
    manufacturer_data_get_Data,
    manufacturer_data_put_Data,
};

static HRESULT manufacturer_data_create( UINT16 company_id, const BYTE *data, UINT32 size,
                                         IBluetoothLEManufacturerData **out )
{
    struct manufacturer_data *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEManufacturerData_iface.lpVtbl = &manufacturer_data_vtbl;
    impl->ref = 1;
    impl->company_id = company_id;
    if (FAILED((hr = buffer_create( data, size, &impl->data ))))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IBluetoothLEManufacturerData_iface;
    return S_OK;
}

/* --- BluetoothLEAdvertisementDataSection --- */

struct data_section
{
    IBluetoothLEAdvertisementDataSection IBluetoothLEAdvertisementDataSection_iface;
    LONG ref;
    BYTE type;
    IBuffer *data;
};

static inline struct data_section *impl_from_IBluetoothLEAdvertisementDataSection( IBluetoothLEAdvertisementDataSection *iface )
{
    return CONTAINING_RECORD( iface, struct data_section, IBluetoothLEAdvertisementDataSection_iface );
}

static HRESULT WINAPI data_section_QueryInterface( IBluetoothLEAdvertisementDataSection *iface, REFIID iid, void **out )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEAdvertisementDataSection ))
    {
        IBluetoothLEAdvertisementDataSection_AddRef(( *out = &impl->IBluetoothLEAdvertisementDataSection_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI data_section_AddRef( IBluetoothLEAdvertisementDataSection *iface )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI data_section_Release( IBluetoothLEAdvertisementDataSection *iface )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        if (impl->data) IBuffer_Release( impl->data );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI data_section_GetIids( IBluetoothLEAdvertisementDataSection *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_section_GetRuntimeClassName( IBluetoothLEAdvertisementDataSection *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementDataSection", class_name );
}

static HRESULT WINAPI data_section_GetTrustLevel( IBluetoothLEAdvertisementDataSection *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI data_section_get_DataType( IBluetoothLEAdvertisementDataSection *iface, BYTE *value )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->type;
    return S_OK;
}

static HRESULT WINAPI data_section_put_DataType( IBluetoothLEAdvertisementDataSection *iface, BYTE value )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );
    TRACE( "(%p, %u)\n", iface, value );
    impl->type = value;
    return S_OK;
}

static HRESULT WINAPI data_section_get_Data( IBluetoothLEAdvertisementDataSection *iface, IBuffer **value )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->data)) IBuffer_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI data_section_put_Data( IBluetoothLEAdvertisementDataSection *iface, IBuffer *value )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (value) IBuffer_AddRef( value );
    if (impl->data) IBuffer_Release( impl->data );
    impl->data = value;
    return S_OK;
}

static const IBluetoothLEAdvertisementDataSectionVtbl data_section_vtbl =
{
    data_section_QueryInterface,
    data_section_AddRef,
    data_section_Release,
    data_section_GetIids,
    data_section_GetRuntimeClassName,
    data_section_GetTrustLevel,
    data_section_get_DataType,
    data_section_put_DataType,
    data_section_get_Data,
    data_section_put_Data,
};

static HRESULT data_section_create( BYTE type, const BYTE *data, UINT32 size, IBluetoothLEAdvertisementDataSection **out )
{
    struct data_section *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEAdvertisementDataSection_iface.lpVtbl = &data_section_vtbl;
    impl->ref = 1;
    impl->type = type;
    if (FAILED((hr = buffer_create( data, size, &impl->data ))))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IBluetoothLEAdvertisementDataSection_iface;
    return S_OK;
}

/* --- BluetoothLEAdvertisement --- */

static const struct vector_iids manufacturer_data_vector_iids =
{
    .iterable = &IID_IIterable_BluetoothLEManufacturerData,
    .iterator = &IID_IIterator_BluetoothLEManufacturerData,
    .vector = &IID_IVector_BluetoothLEManufacturerData,
    .view = &IID_IVectorView_BluetoothLEManufacturerData,
};

static const struct vector_iids data_section_vector_iids =
{
    .iterable = &IID_IIterable_BluetoothLEAdvertisementDataSection,
    .iterator = &IID_IIterator_BluetoothLEAdvertisementDataSection,
    .vector = &IID_IVector_BluetoothLEAdvertisementDataSection,
    .view = &IID_IVectorView_BluetoothLEAdvertisementDataSection,
};

struct advertisement
{
    IBluetoothLEAdvertisement IBluetoothLEAdvertisement_iface;
    LONG ref;
    IReference_BluetoothLEAdvertisementFlags *flags;
    HSTRING local_name;
    struct guid_vector *uuids;
    IVector_IInspectable *manufacturer_data;
    IVector_IInspectable *sections;
};

static inline struct advertisement *impl_from_IBluetoothLEAdvertisement( IBluetoothLEAdvertisement *iface )
{
    return CONTAINING_RECORD( iface, struct advertisement, IBluetoothLEAdvertisement_iface );
}

static HRESULT WINAPI advertisement_QueryInterface( IBluetoothLEAdvertisement *iface, REFIID iid, void **out )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEAdvertisement ))
    {
        IBluetoothLEAdvertisement_AddRef(( *out = &impl->IBluetoothLEAdvertisement_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI advertisement_AddRef( IBluetoothLEAdvertisement *iface )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI advertisement_Release( IBluetoothLEAdvertisement *iface )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        if (impl->flags) IReference_BluetoothLEAdvertisementFlags_Release( impl->flags );
        WindowsDeleteString( impl->local_name );
        if (impl->uuids) IVector_GUID_Release( &impl->uuids->IVector_GUID_iface );
        if (impl->manufacturer_data) IVector_IInspectable_Release( impl->manufacturer_data );
        if (impl->sections) IVector_IInspectable_Release( impl->sections );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI advertisement_GetIids( IBluetoothLEAdvertisement *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI advertisement_GetRuntimeClassName( IBluetoothLEAdvertisement *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisement", class_name );
}

static HRESULT WINAPI advertisement_GetTrustLevel( IBluetoothLEAdvertisement *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI advertisement_get_Flags( IBluetoothLEAdvertisement *iface, IReference_BluetoothLEAdvertisementFlags **value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->flags)) IReference_BluetoothLEAdvertisementFlags_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI advertisement_put_Flags( IBluetoothLEAdvertisement *iface, IReference_BluetoothLEAdvertisementFlags *value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (value) IReference_BluetoothLEAdvertisementFlags_AddRef( value );
    if (impl->flags) IReference_BluetoothLEAdvertisementFlags_Release( impl->flags );
    impl->flags = value;
    return S_OK;
}

static HRESULT WINAPI advertisement_get_LocalName( IBluetoothLEAdvertisement *iface, HSTRING *value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %p)\n", iface, value );
    return WindowsDuplicateString( impl->local_name, value );
}

static HRESULT WINAPI advertisement_put_LocalName( IBluetoothLEAdvertisement *iface, HSTRING value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "(%p, %s)\n", iface, debugstr_hstring( value ) );

    if (FAILED((hr = WindowsDuplicateString( value, &copy )))) return hr;
    WindowsDeleteString( impl->local_name );
    impl->local_name = copy;
    return S_OK;
}

static HRESULT WINAPI advertisement_get_ServiceUuids( IBluetoothLEAdvertisement *iface, IVector_GUID **value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %p)\n", iface, value );
    IVector_GUID_AddRef(( *value = &impl->uuids->IVector_GUID_iface ));
    return S_OK;
}

static HRESULT WINAPI advertisement_get_ManufacturerData( IBluetoothLEAdvertisement *iface,
                                                          IVector_BluetoothLEManufacturerData **value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %p)\n", iface, value );
    IVector_IInspectable_AddRef( impl->manufacturer_data );
    *value = (IVector_BluetoothLEManufacturerData *)impl->manufacturer_data;
    return S_OK;
}

static HRESULT WINAPI advertisement_get_DataSections( IBluetoothLEAdvertisement *iface,
                                                      IVector_BluetoothLEAdvertisementDataSection **value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %p)\n", iface, value );
    IVector_IInspectable_AddRef( impl->sections );
    *value = (IVector_BluetoothLEAdvertisementDataSection *)impl->sections;
    return S_OK;
}

static HRESULT WINAPI advertisement_GetManufacturerDataByCompanyId( IBluetoothLEAdvertisement *iface, UINT16 id,
                                                                    IVectorView_BluetoothLEManufacturerData **value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    IVector_IInspectable *matches;
    UINT32 i, size;
    HRESULT hr;

    TRACE( "(%p, %u, %p)\n", iface, id, value );

    if (FAILED((hr = vector_create( &manufacturer_data_vector_iids, (void **)&matches )))) return hr;
    IVector_IInspectable_get_Size( impl->manufacturer_data, &size );
    for (i = 0; i < size && SUCCEEDED(hr); i++)
    {
        IBluetoothLEManufacturerData *data;
        UINT16 company_id;

        if (FAILED(IVector_IInspectable_GetAt( impl->manufacturer_data, i, (IInspectable **)&data ))) continue;
        if (SUCCEEDED(IBluetoothLEManufacturerData_get_CompanyId( data, &company_id )) && company_id == id)
            hr = IVector_IInspectable_Append( matches, (IInspectable *)data );
        IBluetoothLEManufacturerData_Release( data );
    }
    if (SUCCEEDED(hr)) hr = IVector_IInspectable_GetView( matches, (IVectorView_IInspectable **)value );
    IVector_IInspectable_Release( matches );
    return hr;
}

static HRESULT WINAPI advertisement_GetSectionsByType( IBluetoothLEAdvertisement *iface, BYTE type,
                                                       IVectorView_BluetoothLEAdvertisementDataSection **value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    IVector_IInspectable *matches;
    UINT32 i, size;
    HRESULT hr;

    TRACE( "(%p, %#x, %p)\n", iface, type, value );

    if (FAILED((hr = vector_create( &data_section_vector_iids, (void **)&matches )))) return hr;
    IVector_IInspectable_get_Size( impl->sections, &size );
    for (i = 0; i < size && SUCCEEDED(hr); i++)
    {
        IBluetoothLEAdvertisementDataSection *section;
        BYTE section_type;

        if (FAILED(IVector_IInspectable_GetAt( impl->sections, i, (IInspectable **)&section ))) continue;
        if (SUCCEEDED(IBluetoothLEAdvertisementDataSection_get_DataType( section, &section_type )) && section_type == type)
            hr = IVector_IInspectable_Append( matches, (IInspectable *)section );
        IBluetoothLEAdvertisementDataSection_Release( section );
    }
    if (SUCCEEDED(hr)) hr = IVector_IInspectable_GetView( matches, (IVectorView_IInspectable **)value );
    IVector_IInspectable_Release( matches );
    return hr;
}

static const IBluetoothLEAdvertisementVtbl advertisement_vtbl =
{
    advertisement_QueryInterface,
    advertisement_AddRef,
    advertisement_Release,
    advertisement_GetIids,
    advertisement_GetRuntimeClassName,
    advertisement_GetTrustLevel,
    advertisement_get_Flags,
    advertisement_put_Flags,
    advertisement_get_LocalName,
    advertisement_put_LocalName,
    advertisement_get_ServiceUuids,
    advertisement_get_ManufacturerData,
    advertisement_get_DataSections,
    advertisement_GetManufacturerDataByCompanyId,
    advertisement_GetSectionsByType,
};

static HRESULT advertisement_add_section( struct advertisement *impl, BYTE type, const BYTE *data, UINT32 size )
{
    IBluetoothLEAdvertisementDataSection *section;
    HRESULT hr;

    if (FAILED((hr = data_section_create( type, data, size, &section )))) return hr;
    hr = IVector_IInspectable_Append( impl->sections, (IInspectable *)section );
    IBluetoothLEAdvertisementDataSection_Release( section );
    return hr;
}

/* Advertising data types from the Bluetooth Core Specification Supplement. */
#define AD_TYPE_UUID16_COMPLETE  0x03
#define AD_TYPE_UUID32_COMPLETE  0x05
#define AD_TYPE_UUID128_COMPLETE 0x07
#define AD_TYPE_LOCAL_NAME       0x09
#define AD_TYPE_TX_POWER         0x0a
#define AD_TYPE_SERVICE_DATA16   0x16
#define AD_TYPE_APPEARANCE       0x19
#define AD_TYPE_SERVICE_DATA32   0x20
#define AD_TYPE_SERVICE_DATA128  0x21
#define AD_TYPE_MANUFACTURER     0xff

static HRESULT advertisement_add_uuid_sections( struct advertisement *impl, const struct winebth_le_advertisement *adv )
{
    BYTE uuid16[WINEBTH_LE_ADV_MAX_UUIDS * 2], uuid32[WINEBTH_LE_ADV_MAX_UUIDS * 4], uuid128[WINEBTH_LE_ADV_MAX_UUIDS * 16];
    UINT32 n16 = 0, n32 = 0, n128 = 0, i, value;
    HRESULT hr = S_OK;

    for (i = 0; i < adv->uuid_count; i++)
    {
        if (uuid_short_value( &adv->uuids[i], &value ))
        {
            if (value <= 0xffff)
            {
                uuid16[n16 * 2] = value & 0xff;
                uuid16[n16 * 2 + 1] = value >> 8;
                n16++;
            }
            else
            {
                memcpy( &uuid32[n32 * 4], &value, 4 );
                n32++;
            }
        }
        else
            uuid_to_le_bytes( &adv->uuids[i], &uuid128[n128++ * 16] );
    }
    if (n16 && SUCCEEDED(hr)) hr = advertisement_add_section( impl, AD_TYPE_UUID16_COMPLETE, uuid16, n16 * 2 );
    if (n32 && SUCCEEDED(hr)) hr = advertisement_add_section( impl, AD_TYPE_UUID32_COMPLETE, uuid32, n32 * 4 );
    if (n128 && SUCCEEDED(hr)) hr = advertisement_add_section( impl, AD_TYPE_UUID128_COMPLETE, uuid128, n128 * 16 );
    return hr;
}

static HRESULT advertisement_create( const struct winebth_le_advertisement *adv, IBluetoothLEAdvertisement **out )
{
    struct advertisement *impl;
    HRESULT hr;
    UINT32 i;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEAdvertisement_iface.lpVtbl = &advertisement_vtbl;
    impl->ref = 1;

    if (FAILED((hr = guid_vector_create( adv->uuids, adv->uuid_count, FALSE, &impl->uuids )))) goto failed;
    if (FAILED((hr = vector_create( &manufacturer_data_vector_iids, (void **)&impl->manufacturer_data )))) goto failed;
    if (FAILED((hr = vector_create( &data_section_vector_iids, (void **)&impl->sections )))) goto failed;

    if (adv->flags & WINEBTH_LE_ADV_FLAG_NAME && adv->name[0])
    {
        WCHAR name[BLUETOOTH_MAX_NAME_SIZE];
        int len = MultiByteToWideChar( CP_UTF8, 0, adv->name, -1, name, ARRAY_SIZE( name ) );
        if (len > 0 && FAILED((hr = WindowsCreateString( name, len - 1, &impl->local_name )))) goto failed;
        if (FAILED((hr = advertisement_add_section( impl, AD_TYPE_LOCAL_NAME, (const BYTE *)adv->name, strlen( adv->name ) ))))
            goto failed;
    }
    if (FAILED((hr = advertisement_add_uuid_sections( impl, adv )))) goto failed;
    for (i = 0; i < adv->manufacturer_data_count; i++)
    {
        const struct winebth_le_manufacturer_data *data = &adv->manufacturer_data[i];
        IBluetoothLEManufacturerData *entry;
        BYTE section[2 + WINEBTH_LE_ADV_MAX_DATA];

        if (FAILED((hr = manufacturer_data_create( data->company_id, data->data, data->size, &entry )))) goto failed;
        hr = IVector_IInspectable_Append( impl->manufacturer_data, (IInspectable *)entry );
        IBluetoothLEManufacturerData_Release( entry );
        if (FAILED(hr)) goto failed;

        section[0] = data->company_id & 0xff;
        section[1] = data->company_id >> 8;
        memcpy( &section[2], data->data, data->size );
        if (FAILED((hr = advertisement_add_section( impl, AD_TYPE_MANUFACTURER, section, 2 + data->size )))) goto failed;
    }
    for (i = 0; i < adv->service_data_count; i++)
    {
        const struct winebth_le_service_data *data = &adv->service_data[i];
        BYTE section[16 + WINEBTH_LE_ADV_MAX_DATA], type;
        UINT32 value, uuid_len;

        if (uuid_short_value( &data->uuid, &value ))
        {
            uuid_len = value <= 0xffff ? 2 : 4;
            memcpy( section, &value, uuid_len );
            type = uuid_len == 2 ? AD_TYPE_SERVICE_DATA16 : AD_TYPE_SERVICE_DATA32;
        }
        else
        {
            uuid_len = 16;
            uuid_to_le_bytes( &data->uuid, section );
            type = AD_TYPE_SERVICE_DATA128;
        }
        memcpy( &section[uuid_len], data->data, data->size );
        if (FAILED((hr = advertisement_add_section( impl, type, section, uuid_len + data->size )))) goto failed;
    }
    if (adv->flags & WINEBTH_LE_ADV_FLAG_TX_POWER)
    {
        BYTE power = (BYTE)adv->tx_power;
        if (FAILED((hr = advertisement_add_section( impl, AD_TYPE_TX_POWER, &power, 1 )))) goto failed;
    }
    if (adv->flags & WINEBTH_LE_ADV_FLAG_APPEARANCE)
    {
        BYTE appearance[2] = { adv->appearance & 0xff, adv->appearance >> 8 };
        if (FAILED((hr = advertisement_add_section( impl, AD_TYPE_APPEARANCE, appearance, 2 )))) goto failed;
    }

    *out = &impl->IBluetoothLEAdvertisement_iface;
    return S_OK;

failed:
    IBluetoothLEAdvertisement_Release( &impl->IBluetoothLEAdvertisement_iface );
    return hr;
}

/* --- BluetoothLEAdvertisementReceivedEventArgs --- */

struct received_args
{
    IBluetoothLEAdvertisementReceivedEventArgs IBluetoothLEAdvertisementReceivedEventArgs_iface;
    IBluetoothLEAdvertisementReceivedEventArgs2 IBluetoothLEAdvertisementReceivedEventArgs2_iface;
    LONG ref;
    UINT64 address;
    BluetoothAddressType address_type;
    INT16 rssi;
    BOOL has_tx_power;
    INT16 tx_power;
    DateTime timestamp;
    IBluetoothLEAdvertisement *advertisement;
};

static inline struct received_args *impl_from_IBluetoothLEAdvertisementReceivedEventArgs( IBluetoothLEAdvertisementReceivedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct received_args, IBluetoothLEAdvertisementReceivedEventArgs_iface );
}

static HRESULT WINAPI received_args_QueryInterface( IBluetoothLEAdvertisementReceivedEventArgs *iface, REFIID iid, void **out )
{
    struct received_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEAdvertisementReceivedEventArgs ))
    {
        IBluetoothLEAdvertisementReceivedEventArgs_AddRef(( *out = &impl->IBluetoothLEAdvertisementReceivedEventArgs_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothLEAdvertisementReceivedEventArgs2 ))
    {
        IBluetoothLEAdvertisementReceivedEventArgs_AddRef( iface );
        *out = &impl->IBluetoothLEAdvertisementReceivedEventArgs2_iface;
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI received_args_AddRef( IBluetoothLEAdvertisementReceivedEventArgs *iface )
{
    struct received_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI received_args_Release( IBluetoothLEAdvertisementReceivedEventArgs *iface )
{
    struct received_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        IBluetoothLEAdvertisement_Release( impl->advertisement );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI received_args_GetIids( IBluetoothLEAdvertisementReceivedEventArgs *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI received_args_GetRuntimeClassName( IBluetoothLEAdvertisementReceivedEventArgs *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementReceivedEventArgs", class_name );
}

static HRESULT WINAPI received_args_GetTrustLevel( IBluetoothLEAdvertisementReceivedEventArgs *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI received_args_get_RawSignalStrengthInDBm( IBluetoothLEAdvertisementReceivedEventArgs *iface, INT16 *value )
{
    struct received_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->rssi;
    return S_OK;
}

static HRESULT WINAPI received_args_get_BluetoothAddress( IBluetoothLEAdvertisementReceivedEventArgs *iface, UINT64 *value )
{
    struct received_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->address;
    return S_OK;
}

static HRESULT WINAPI received_args_get_AdvertisementType( IBluetoothLEAdvertisementReceivedEventArgs *iface,
                                                           BluetoothLEAdvertisementType *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = BluetoothLEAdvertisementType_ConnectableUndirected;
    return S_OK;
}

static HRESULT WINAPI received_args_get_Timestamp( IBluetoothLEAdvertisementReceivedEventArgs *iface, DateTime *value )
{
    struct received_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->timestamp;
    return S_OK;
}

static HRESULT WINAPI received_args_get_Advertisement( IBluetoothLEAdvertisementReceivedEventArgs *iface,
                                                       IBluetoothLEAdvertisement **value )
{
    struct received_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    IBluetoothLEAdvertisement_AddRef(( *value = impl->advertisement ));
    return S_OK;
}

static const IBluetoothLEAdvertisementReceivedEventArgsVtbl received_args_vtbl =
{
    received_args_QueryInterface,
    received_args_AddRef,
    received_args_Release,
    received_args_GetIids,
    received_args_GetRuntimeClassName,
    received_args_GetTrustLevel,
    received_args_get_RawSignalStrengthInDBm,
    received_args_get_BluetoothAddress,
    received_args_get_AdvertisementType,
    received_args_get_Timestamp,
    received_args_get_Advertisement,
};

DEFINE_IINSPECTABLE( received_args2, IBluetoothLEAdvertisementReceivedEventArgs2, struct received_args,
                     IBluetoothLEAdvertisementReceivedEventArgs_iface )

static HRESULT WINAPI received_args2_get_BluetoothAddressType( IBluetoothLEAdvertisementReceivedEventArgs2 *iface,
                                                               BluetoothAddressType *value )
{
    struct received_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs2( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->address_type;
    return S_OK;
}

static HRESULT WINAPI received_args2_get_TransmitPowerLevelInDBm( IBluetoothLEAdvertisementReceivedEventArgs2 *iface,
                                                                  IReference_INT16 **value )
{
    struct received_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs2( iface );
    static const WCHAR class_name[] = L"Windows.Foundation.PropertyValue";
    IPropertyValueStatics *statics;
    IInspectable *boxed;
    HSTRING_HEADER hdr;
    HSTRING str;
    HRESULT hr;

    TRACE( "(%p, %p)\n", iface, value );

    *value = NULL;
    if (!impl->has_tx_power) return S_OK;
    if (FAILED((hr = WindowsCreateStringReference( class_name, ARRAY_SIZE( class_name ) - 1, &hdr, &str )))) return hr;
    if (FAILED((hr = RoGetActivationFactory( str, &IID_IPropertyValueStatics, (void **)&statics )))) return hr;
    hr = IPropertyValueStatics_CreateInt16( statics, impl->tx_power, &boxed );
    IPropertyValueStatics_Release( statics );
    if (FAILED(hr)) return hr;
    hr = IInspectable_QueryInterface( boxed, &IID_IReference_INT16, (void **)value );
    IInspectable_Release( boxed );
    return hr;
}

static HRESULT WINAPI received_args2_get_IsAnonymous( IBluetoothLEAdvertisementReceivedEventArgs2 *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI received_args2_get_IsConnectable( IBluetoothLEAdvertisementReceivedEventArgs2 *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI received_args2_get_IsScannable( IBluetoothLEAdvertisementReceivedEventArgs2 *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI received_args2_get_IsDirected( IBluetoothLEAdvertisementReceivedEventArgs2 *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI received_args2_get_IsScanResponse( IBluetoothLEAdvertisementReceivedEventArgs2 *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = FALSE;
    return S_OK;
}

static const IBluetoothLEAdvertisementReceivedEventArgs2Vtbl received_args2_vtbl =
{
    received_args2_QueryInterface,
    received_args2_AddRef,
    received_args2_Release,
    received_args2_GetIids,
    received_args2_GetRuntimeClassName,
    received_args2_GetTrustLevel,
    received_args2_get_BluetoothAddressType,
    received_args2_get_TransmitPowerLevelInDBm,
    received_args2_get_IsAnonymous,
    received_args2_get_IsConnectable,
    received_args2_get_IsScannable,
    received_args2_get_IsDirected,
    received_args2_get_IsScanResponse,
};

static HRESULT received_args_create( const struct winebth_le_advertisement *adv,
                                     IBluetoothLEAdvertisementReceivedEventArgs **out )
{
    struct received_args *impl;
    FILETIME now;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEAdvertisementReceivedEventArgs_iface.lpVtbl = &received_args_vtbl;
    impl->IBluetoothLEAdvertisementReceivedEventArgs2_iface.lpVtbl = &received_args2_vtbl;
    impl->ref = 1;
    impl->address = adv->address;
    impl->address_type = adv->flags & WINEBTH_LE_ADV_FLAG_RANDOM_ADDRESS ? BluetoothAddressType_Random
                                                                        : BluetoothAddressType_Public;
    impl->rssi = adv->flags & WINEBTH_LE_ADV_FLAG_RSSI ? adv->rssi : -127;
    impl->has_tx_power = !!(adv->flags & WINEBTH_LE_ADV_FLAG_TX_POWER);
    impl->tx_power = adv->tx_power;
    GetSystemTimeAsFileTime( &now );
    impl->timestamp.UniversalTime = ((UINT64)now.dwHighDateTime << 32) | now.dwLowDateTime;
    if (FAILED((hr = advertisement_create( adv, &impl->advertisement ))))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IBluetoothLEAdvertisementReceivedEventArgs_iface;
    return S_OK;
}

/* --- BluetoothLEAdvertisementWatcherStoppedEventArgs --- */

struct stopped_args
{
    IBluetoothLEAdvertisementWatcherStoppedEventArgs IBluetoothLEAdvertisementWatcherStoppedEventArgs_iface;
    LONG ref;
    BluetoothError error;
};

static inline struct stopped_args *impl_from_IBluetoothLEAdvertisementWatcherStoppedEventArgs( IBluetoothLEAdvertisementWatcherStoppedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct stopped_args, IBluetoothLEAdvertisementWatcherStoppedEventArgs_iface );
}

static HRESULT WINAPI stopped_args_QueryInterface( IBluetoothLEAdvertisementWatcherStoppedEventArgs *iface, REFIID iid, void **out )
{
    struct stopped_args *impl = impl_from_IBluetoothLEAdvertisementWatcherStoppedEventArgs( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEAdvertisementWatcherStoppedEventArgs ))
    {
        IBluetoothLEAdvertisementWatcherStoppedEventArgs_AddRef(( *out = &impl->IBluetoothLEAdvertisementWatcherStoppedEventArgs_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI stopped_args_AddRef( IBluetoothLEAdvertisementWatcherStoppedEventArgs *iface )
{
    struct stopped_args *impl = impl_from_IBluetoothLEAdvertisementWatcherStoppedEventArgs( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI stopped_args_Release( IBluetoothLEAdvertisementWatcherStoppedEventArgs *iface )
{
    struct stopped_args *impl = impl_from_IBluetoothLEAdvertisementWatcherStoppedEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI stopped_args_GetIids( IBluetoothLEAdvertisementWatcherStoppedEventArgs *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI stopped_args_GetRuntimeClassName( IBluetoothLEAdvertisementWatcherStoppedEventArgs *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcherStoppedEventArgs", class_name );
}

static HRESULT WINAPI stopped_args_GetTrustLevel( IBluetoothLEAdvertisementWatcherStoppedEventArgs *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI stopped_args_get_Error( IBluetoothLEAdvertisementWatcherStoppedEventArgs *iface, BluetoothError *value )
{
    struct stopped_args *impl = impl_from_IBluetoothLEAdvertisementWatcherStoppedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->error;
    return S_OK;
}

static const IBluetoothLEAdvertisementWatcherStoppedEventArgsVtbl stopped_args_vtbl =
{
    stopped_args_QueryInterface,
    stopped_args_AddRef,
    stopped_args_Release,
    stopped_args_GetIids,
    stopped_args_GetRuntimeClassName,
    stopped_args_GetTrustLevel,
    stopped_args_get_Error,
};

static HRESULT stopped_args_create( BluetoothError error, IBluetoothLEAdvertisementWatcherStoppedEventArgs **out )
{
    struct stopped_args *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEAdvertisementWatcherStoppedEventArgs_iface.lpVtbl = &stopped_args_vtbl;
    impl->ref = 1;
    impl->error = error;
    *out = &impl->IBluetoothLEAdvertisementWatcherStoppedEventArgs_iface;
    return S_OK;
}

/* --- BluetoothLEAdvertisementWatcher --- */

typedef ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementReceivedEventArgs received_handler;
typedef ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementWatcherStoppedEventArgs stopped_handler;

struct event_handler
{
    IUnknown *handler;
    INT64 token;
};

struct event_handlers
{
    struct event_handler *entries;
    UINT32 count;
    UINT32 capacity;
};

struct adv_watcher
{
    IBluetoothLEAdvertisementWatcher IBluetoothLEAdvertisementWatcher_iface;
    IBluetoothLEAdvertisementWatcher2 IBluetoothLEAdvertisementWatcher2_iface;
    LONG ref;

    CRITICAL_SECTION cs;
    BluetoothLEAdvertisementWatcherStatus status;
    BluetoothLEScanningMode scanning_mode;
    boolean allow_extended;
    HANDLE radio;
    HCMNOTIFICATION notification;
    INT64 next_token;
    struct event_handlers received;
    struct event_handlers stopped;
};

static inline struct adv_watcher *impl_from_IBluetoothLEAdvertisementWatcher( IBluetoothLEAdvertisementWatcher *iface )
{
    return CONTAINING_RECORD( iface, struct adv_watcher, IBluetoothLEAdvertisementWatcher_iface );
}

static HRESULT event_handlers_add( struct adv_watcher *impl, struct event_handlers *handlers, IUnknown *handler,
                                   EventRegistrationToken *token )
{
    struct event_handler *entry;

    if (!handler) return E_INVALIDARG;
    EnterCriticalSection( &impl->cs );
    if (handlers->count == handlers->capacity)
    {
        UINT32 capacity = max( 4, handlers->capacity * 2 );
        if (!(entry = realloc( handlers->entries, capacity * sizeof( *entry ) )))
        {
            LeaveCriticalSection( &impl->cs );
            return E_OUTOFMEMORY;
        }
        handlers->entries = entry;
        handlers->capacity = capacity;
    }
    entry = &handlers->entries[handlers->count++];
    entry->handler = handler;
    IUnknown_AddRef( handler );
    entry->token = token->value = ++impl->next_token;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT event_handlers_remove( struct adv_watcher *impl, struct event_handlers *handlers, EventRegistrationToken token )
{
    IUnknown *handler = NULL;
    UINT32 i;

    EnterCriticalSection( &impl->cs );
    for (i = 0; i < handlers->count; i++)
    {
        if (handlers->entries[i].token != token.value) continue;
        handler = handlers->entries[i].handler;
        memmove( &handlers->entries[i], &handlers->entries[i + 1], (handlers->count - i - 1) * sizeof( *handlers->entries ) );
        handlers->count--;
        break;
    }
    LeaveCriticalSection( &impl->cs );
    if (handler) IUnknown_Release( handler );
    return S_OK;
}

/* Take a snapshot of the handlers so that they can be invoked without holding the lock. */
static UINT32 event_handlers_snapshot( struct adv_watcher *impl, struct event_handlers *handlers, IUnknown ***out )
{
    UINT32 i, count;

    EnterCriticalSection( &impl->cs );
    count = handlers->count;
    if (!count || !(*out = malloc( count * sizeof( **out ) )))
    {
        LeaveCriticalSection( &impl->cs );
        return 0;
    }
    for (i = 0; i < count; i++)
    {
        (*out)[i] = handlers->entries[i].handler;
        IUnknown_AddRef( (*out)[i] );
    }
    LeaveCriticalSection( &impl->cs );
    return count;
}

static void event_handlers_free( struct event_handlers *handlers )
{
    UINT32 i;
    for (i = 0; i < handlers->count; i++) IUnknown_Release( handlers->entries[i].handler );
    free( handlers->entries );
}

static void adv_watcher_dispatch_received( struct adv_watcher *impl, const struct winebth_le_advertisement *adv )
{
    IBluetoothLEAdvertisementReceivedEventArgs *args;
    IUnknown **handlers;
    UINT32 i, count;
    HRESULT hr;

    TRACE( "address %#I64x rssi %d name %s uuids %u manufacturer %u service data %u\n", adv->address, adv->rssi,
           debugstr_a( adv->name ), adv->uuid_count, adv->manufacturer_data_count, adv->service_data_count );

    if (!(count = event_handlers_snapshot( impl, &impl->received, &handlers ))) return;
    if (SUCCEEDED((hr = received_args_create( adv, &args ))))
    {
        for (i = 0; i < count; i++)
        {
            hr = ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementReceivedEventArgs_Invoke(
                (received_handler *)handlers[i], &impl->IBluetoothLEAdvertisementWatcher_iface, args );
            if (FAILED(hr)) WARN( "Received handler %p returned %#lx\n", handlers[i], hr );
        }
        IBluetoothLEAdvertisementReceivedEventArgs_Release( args );
    }
    else
        ERR( "Failed to create event args, hr %#lx\n", hr );
    for (i = 0; i < count; i++) IUnknown_Release( handlers[i] );
    free( handlers );
}

static void adv_watcher_dispatch_stopped( struct adv_watcher *impl, BluetoothError error )
{
    IBluetoothLEAdvertisementWatcherStoppedEventArgs *args;
    IUnknown **handlers;
    UINT32 i, count;

    if (!(count = event_handlers_snapshot( impl, &impl->stopped, &handlers ))) return;
    if (SUCCEEDED(stopped_args_create( error, &args )))
    {
        for (i = 0; i < count; i++)
            ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementWatcherStoppedEventArgs_Invoke(
                (stopped_handler *)handlers[i], &impl->IBluetoothLEAdvertisementWatcher_iface, args );
        IBluetoothLEAdvertisementWatcherStoppedEventArgs_Release( args );
    }
    for (i = 0; i < count; i++) IUnknown_Release( handlers[i] );
    free( handlers );
}

static DWORD CALLBACK adv_watcher_notify_callback( HCMNOTIFICATION notify, void *ctx, CM_NOTIFY_ACTION action,
                                                   CM_NOTIFY_EVENT_DATA *event_data, DWORD size )
{
    struct adv_watcher *impl = ctx;
    const struct winebth_le_advertisement *adv;

    TRACE( "(%p, %p, %d, %p, %lu)\n", notify, ctx, action, event_data, size );

    if (action != CM_NOTIFY_ACTION_DEVICECUSTOMEVENT) return ERROR_SUCCESS;
    if (!IsEqualGUID( &event_data->u.DeviceHandle.EventGuid, &GUID_WINEBTH_LE_ADVERTISEMENT )) return ERROR_SUCCESS;
    if (event_data->u.DeviceHandle.DataSize < sizeof( *adv ))
    {
        WARN( "Unexpected event data size %lu\n", event_data->u.DeviceHandle.DataSize );
        return ERROR_SUCCESS;
    }
    adv = (const struct winebth_le_advertisement *)event_data->u.DeviceHandle.Data;

    EnterCriticalSection( &impl->cs );
    if (impl->status != BluetoothLEAdvertisementWatcherStatus_Started)
    {
        LeaveCriticalSection( &impl->cs );
        return ERROR_SUCCESS;
    }
    LeaveCriticalSection( &impl->cs );
    adv_watcher_dispatch_received( impl, adv );
    return ERROR_SUCCESS;
}

/* Replay the advertisements the driver already knows about so that devices show up immediately. */
static void CALLBACK adv_watcher_replay( TP_CALLBACK_INSTANCE *instance, void *ctx )
{
    struct adv_watcher *impl = ctx;
    struct winebth_radio_get_le_advertisements_params *params = NULL;
    DWORD capacity = 32, size, bytes, i;
    HANDLE radio;

    EnterCriticalSection( &impl->cs );
    radio = impl->status == BluetoothLEAdvertisementWatcherStatus_Started ? impl->radio : NULL;
    LeaveCriticalSection( &impl->cs );
    if (!radio) goto done;

    for (;;)
    {
        void *tmp;

        size = offsetof( struct winebth_radio_get_le_advertisements_params, advertisements[capacity] );
        if (!(tmp = realloc( params, size ))) goto done;
        params = tmp;
        if (DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_GET_LE_ADVERTISEMENTS, NULL, 0, params, size, &bytes, NULL )) break;
        if (GetLastError() != ERROR_MORE_DATA || params->count <= capacity)
        {
            WARN( "IOCTL_WINEBTH_RADIO_GET_LE_ADVERTISEMENTS failed: %lu\n", GetLastError() );
            goto done;
        }
        capacity = params->count + 8;
    }
    for (i = 0; i < min( params->count, capacity ); i++)
        adv_watcher_dispatch_received( impl, &params->advertisements[i] );

done:
    free( params );
    IBluetoothLEAdvertisementWatcher_Release( &impl->IBluetoothLEAdvertisementWatcher_iface );
}

static HANDLE open_default_radio( void )
{
    char buffer[sizeof( SP_DEVICE_INTERFACE_DETAIL_DATA_W ) + MAX_PATH * sizeof( WCHAR )];
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buffer;
    SP_DEVICE_INTERFACE_DATA iface_data = { .cbSize = sizeof( iface_data ) };
    HANDLE radio = INVALID_HANDLE_VALUE;
    HDEVINFO devinfo;

    devinfo = SetupDiGetClassDevsW( &GUID_BLUETOOTH_RADIO_INTERFACE, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE );
    if (devinfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    detail->cbSize = sizeof( *detail );
    if (SetupDiEnumDeviceInterfaces( devinfo, NULL, &GUID_BLUETOOTH_RADIO_INTERFACE, 0, &iface_data ) &&
        SetupDiGetDeviceInterfaceDetailW( devinfo, &iface_data, detail, sizeof( buffer ), NULL, NULL ))
    {
        radio = CreateFileW( detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, 0, NULL );
        if (radio == INVALID_HANDLE_VALUE) WARN( "Failed to open %s: %lu\n", debugstr_w( detail->DevicePath ), GetLastError() );
    }
    SetupDiDestroyDeviceInfoList( devinfo );
    return radio;
}

static HRESULT WINAPI adv_watcher_QueryInterface( IBluetoothLEAdvertisementWatcher *iface, REFIID iid, void **out )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEAdvertisementWatcher ))
    {
        IBluetoothLEAdvertisementWatcher_AddRef(( *out = &impl->IBluetoothLEAdvertisementWatcher_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothLEAdvertisementWatcher2 ))
    {
        IBluetoothLEAdvertisementWatcher_AddRef( iface );
        *out = &impl->IBluetoothLEAdvertisementWatcher2_iface;
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI adv_watcher_AddRef( IBluetoothLEAdvertisementWatcher *iface )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    TRACE( "(%p)\n", iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI adv_watcher_Release( IBluetoothLEAdvertisementWatcher *iface )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "(%p)\n", iface );

    if (!ref)
    {
        if (impl->notification) CM_Unregister_Notification( impl->notification );
        if (impl->radio != INVALID_HANDLE_VALUE)
        {
            DeviceIoControl( impl->radio, IOCTL_WINEBTH_RADIO_STOP_DISCOVERY, NULL, 0, NULL, 0, NULL, NULL );
            CloseHandle( impl->radio );
        }
        event_handlers_free( &impl->received );
        event_handlers_free( &impl->stopped );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI adv_watcher_GetIids( IBluetoothLEAdvertisementWatcher *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_GetRuntimeClassName( IBluetoothLEAdvertisementWatcher *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher", class_name );
}

static HRESULT WINAPI adv_watcher_GetTrustLevel( IBluetoothLEAdvertisementWatcher *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_MinSamplingInternal( IBluetoothLEAdvertisementWatcher *iface, TimeSpan *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    value->Duration = 1000000;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_MaxSamplingInternal( IBluetoothLEAdvertisementWatcher *iface, TimeSpan *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    value->Duration = 255000000;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_MinOutOfRangeTimeout( IBluetoothLEAdvertisementWatcher *iface, TimeSpan *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    value->Duration = 10000000;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_MaxOutOfRangeTimeout( IBluetoothLEAdvertisementWatcher *iface, TimeSpan *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    value->Duration = 600000000;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_Status( IBluetoothLEAdvertisementWatcher *iface, BluetoothLEAdvertisementWatcherStatus *status )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );

    TRACE( "(%p, %p)\n", iface, status );

    EnterCriticalSection( &impl->cs );
    *status = impl->status;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_ScanningMode( IBluetoothLEAdvertisementWatcher *iface, BluetoothLEScanningMode *value )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->scanning_mode;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_put_ScanningMode( IBluetoothLEAdvertisementWatcher *iface, BluetoothLEScanningMode value )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    TRACE( "(%p, %d)\n", iface, value );
    impl->scanning_mode = value;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_SignalStrengthFilter( IBluetoothLEAdvertisementWatcher *iface, IBluetoothSignalStrengthFilter **filter )
{
    FIXME( "(%p, %p): stub!\n", iface, filter );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_put_SignalStrengthFilter( IBluetoothLEAdvertisementWatcher *iface, IBluetoothSignalStrengthFilter *filter )
{
    FIXME( "(%p, %p): stub!\n", iface, filter );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_get_AdvertisementFilter( IBluetoothLEAdvertisementWatcher *iface, IBluetoothLEAdvertisementFilter **filter )
{
    FIXME( "(%p, %p): stub!\n", iface, filter );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_put_AdvertisementFilter( IBluetoothLEAdvertisementWatcher *iface, IBluetoothLEAdvertisementFilter *filter )
{
    FIXME( "(%p, %p): stub!\n", iface, filter );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_Start( IBluetoothLEAdvertisementWatcher *iface )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    struct winebth_radio_start_discovery_params params = { .le = 1 };
    CM_NOTIFY_FILTER filter = { .cbSize = sizeof( filter ), .FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE };
    HCMNOTIFICATION notification;
    CONFIGRET ret;
    HANDLE radio;
    DWORD bytes;

    TRACE( "(%p)\n", iface );

    EnterCriticalSection( &impl->cs );
    if (impl->status == BluetoothLEAdvertisementWatcherStatus_Started)
    {
        LeaveCriticalSection( &impl->cs );
        return S_OK;
    }
    if (impl->status == BluetoothLEAdvertisementWatcherStatus_Stopping)
    {
        LeaveCriticalSection( &impl->cs );
        return E_ILLEGAL_METHOD_CALL;
    }

    if ((radio = open_default_radio()) == INVALID_HANDLE_VALUE)
    {
        impl->status = BluetoothLEAdvertisementWatcherStatus_Aborted;
        LeaveCriticalSection( &impl->cs );
        return HRESULT_FROM_WIN32( ERROR_DEVICE_NOT_AVAILABLE );
    }
    filter.u.DeviceHandle.hTarget = radio;
    if ((ret = CM_Register_Notification( &filter, impl, adv_watcher_notify_callback, &notification )))
    {
        ERR( "CM_Register_Notification failed: %#lx\n", ret );
        CloseHandle( radio );
        impl->status = BluetoothLEAdvertisementWatcherStatus_Aborted;
        LeaveCriticalSection( &impl->cs );
        return E_FAIL;
    }
    if (!DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_START_DISCOVERY, &params, sizeof( params ), NULL, 0, &bytes, NULL ))
        WARN( "IOCTL_WINEBTH_RADIO_START_DISCOVERY failed: %lu\n", GetLastError() );

    impl->radio = radio;
    impl->notification = notification;
    impl->status = BluetoothLEAdvertisementWatcherStatus_Started;
    LeaveCriticalSection( &impl->cs );

    IBluetoothLEAdvertisementWatcher_AddRef( iface );
    if (!TrySubmitThreadpoolCallback( adv_watcher_replay, impl, NULL ))
        IBluetoothLEAdvertisementWatcher_Release( iface );
    return S_OK;
}

struct stop_context
{
    struct adv_watcher *impl;
    HCMNOTIFICATION notification;
    HANDLE radio;
};

/* Unregistering waits for in-flight callbacks, so it cannot run on a thread that may be inside one. */
static void CALLBACK adv_watcher_finish_stop( TP_CALLBACK_INSTANCE *instance, void *ctx )
{
    struct stop_context *stop = ctx;
    struct adv_watcher *impl = stop->impl;

    CM_Unregister_Notification( stop->notification );
    DeviceIoControl( stop->radio, IOCTL_WINEBTH_RADIO_STOP_DISCOVERY, NULL, 0, NULL, 0, NULL, NULL );
    CloseHandle( stop->radio );

    EnterCriticalSection( &impl->cs );
    impl->status = BluetoothLEAdvertisementWatcherStatus_Stopped;
    LeaveCriticalSection( &impl->cs );

    adv_watcher_dispatch_stopped( impl, BluetoothError_Success );
    IBluetoothLEAdvertisementWatcher_Release( &impl->IBluetoothLEAdvertisementWatcher_iface );
    free( stop );
}

static HRESULT WINAPI adv_watcher_Stop( IBluetoothLEAdvertisementWatcher *iface )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    struct stop_context *stop;

    TRACE( "(%p)\n", iface );

    if (!(stop = malloc( sizeof( *stop ) ))) return E_OUTOFMEMORY;

    EnterCriticalSection( &impl->cs );
    if (impl->status != BluetoothLEAdvertisementWatcherStatus_Started)
    {
        LeaveCriticalSection( &impl->cs );
        free( stop );
        return S_OK;
    }
    impl->status = BluetoothLEAdvertisementWatcherStatus_Stopping;
    stop->impl = impl;
    stop->notification = impl->notification;
    stop->radio = impl->radio;
    impl->notification = NULL;
    impl->radio = INVALID_HANDLE_VALUE;
    LeaveCriticalSection( &impl->cs );

    IBluetoothLEAdvertisementWatcher_AddRef( iface );
    if (!TrySubmitThreadpoolCallback( adv_watcher_finish_stop, stop, NULL ))
        adv_watcher_finish_stop( NULL, stop );
    return S_OK;
}

static HRESULT WINAPI adv_watcher_add_Received( IBluetoothLEAdvertisementWatcher *iface, received_handler *handler,
                                                EventRegistrationToken *token )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    TRACE( "(%p, %p, %p)\n", iface, handler, token );
    return event_handlers_add( impl, &impl->received, (IUnknown *)handler, token );
}

static HRESULT WINAPI adv_watcher_remove_Received( IBluetoothLEAdvertisementWatcher *iface, EventRegistrationToken token )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    TRACE( "(%p, %I64x)\n", iface, token.value );
    return event_handlers_remove( impl, &impl->received, token );
}

static HRESULT WINAPI adv_watcher_add_Stopped( IBluetoothLEAdvertisementWatcher *iface, stopped_handler *handler,
                                               EventRegistrationToken *token )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    TRACE( "(%p, %p, %p)\n", iface, handler, token );
    return event_handlers_add( impl, &impl->stopped, (IUnknown *)handler, token );
}

static HRESULT WINAPI adv_watcher_remove_Stopped( IBluetoothLEAdvertisementWatcher *iface, EventRegistrationToken token )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    TRACE( "(%p, %I64x)\n", iface, token.value );
    return event_handlers_remove( impl, &impl->stopped, token );
}

static const IBluetoothLEAdvertisementWatcherVtbl adv_watcher_vtbl =
{
    /* IUnknown */
    adv_watcher_QueryInterface,
    adv_watcher_AddRef,
    adv_watcher_Release,
    /* IInspectable */
    adv_watcher_GetIids,
    adv_watcher_GetRuntimeClassName,
    adv_watcher_GetTrustLevel,
    /* IBluetoothLEAdvertisementWatcher */
    adv_watcher_get_MinSamplingInternal,
    adv_watcher_get_MaxSamplingInternal,
    adv_watcher_get_MinOutOfRangeTimeout,
    adv_watcher_get_MaxOutOfRangeTimeout,
    adv_watcher_get_Status,
    adv_watcher_get_ScanningMode,
    adv_watcher_put_ScanningMode,
    adv_watcher_get_SignalStrengthFilter,
    adv_watcher_put_SignalStrengthFilter,
    adv_watcher_get_AdvertisementFilter,
    adv_watcher_put_AdvertisementFilter,
    adv_watcher_Start,
    adv_watcher_Stop,
    adv_watcher_add_Received,
    adv_watcher_remove_Received,
    adv_watcher_add_Stopped,
    adv_watcher_remove_Stopped,
};

DEFINE_IINSPECTABLE( adv_watcher2, IBluetoothLEAdvertisementWatcher2, struct adv_watcher, IBluetoothLEAdvertisementWatcher_iface )

static HRESULT WINAPI adv_watcher2_get_AllowExtendedAdvertisements( IBluetoothLEAdvertisementWatcher2 *iface, boolean *value )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher2( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->allow_extended;
    return S_OK;
}

static HRESULT WINAPI adv_watcher2_put_AllowExtendedAdvertisements( IBluetoothLEAdvertisementWatcher2 *iface, boolean value )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher2( iface );
    TRACE( "(%p, %d)\n", iface, value );
    impl->allow_extended = value;
    return S_OK;
}

static const IBluetoothLEAdvertisementWatcher2Vtbl adv_watcher2_vtbl =
{
    adv_watcher2_QueryInterface,
    adv_watcher2_AddRef,
    adv_watcher2_Release,
    adv_watcher2_GetIids,
    adv_watcher2_GetRuntimeClassName,
    adv_watcher2_GetTrustLevel,
    adv_watcher2_get_AllowExtendedAdvertisements,
    adv_watcher2_put_AllowExtendedAdvertisements,
};

static HRESULT adv_watcher_create( IBluetoothLEAdvertisementWatcher **watcher )
{
    struct adv_watcher *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEAdvertisementWatcher_iface.lpVtbl = &adv_watcher_vtbl;
    impl->IBluetoothLEAdvertisementWatcher2_iface.lpVtbl = &adv_watcher2_vtbl;
    impl->ref = 1;
    impl->status = BluetoothLEAdvertisementWatcherStatus_Created;
    impl->scanning_mode = BluetoothLEScanningMode_Passive;
    impl->radio = INVALID_HANDLE_VALUE;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": adv_watcher.cs");

    *watcher = &impl->IBluetoothLEAdvertisementWatcher_iface;
    return S_OK;
}

struct adv_watcher_factory
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct adv_watcher_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct adv_watcher_factory, IActivationFactory_iface );
}

static HRESULT WINAPI adv_watcher_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct adv_watcher_factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI adv_watcher_factory_AddRef( IActivationFactory *iface )
{
    struct adv_watcher_factory *impl = impl_from_IActivationFactory( iface );
    TRACE( "(%p)\n", iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI adv_watcher_factory_Release( IActivationFactory *iface )
{
    struct adv_watcher_factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p)\n", iface );
    return ref;
}

static HRESULT WINAPI adv_watcher_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    FIXME( "(%p, %p): stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "(%p, %p)\n", iface, instance );
    return adv_watcher_create( (IBluetoothLEAdvertisementWatcher **)instance );
}

static const struct IActivationFactoryVtbl adv_watcher_factory_vtbl =
{
    adv_watcher_factory_QueryInterface,
    adv_watcher_factory_AddRef,
    adv_watcher_factory_Release,
    /* IInspectable */
    adv_watcher_factory_GetIids,
    adv_watcher_factory_GetRuntimeClassName,
    adv_watcher_factory_GetTrustLevel,
    /* IActivationFactory */
    adv_watcher_factory_ActivateInstance,
};

static struct adv_watcher_factory adv_watcher_factory =
{
    {&adv_watcher_factory_vtbl},
    1
};

IActivationFactory *advertisement_watcher_factory = &adv_watcher_factory.IActivationFactory_iface;
