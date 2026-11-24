/* WinRT Windows.Devices.Radios Implementation
 *
 * Copyright (C) 2026 Mohamad Al-Jaf
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

#include "initguid.h"
#include "private.h"
#include "bthsdpdef.h"
#include "bluetoothapis.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(radios);

struct radio
{
    IRadio IRadio_iface;
    LONG ref;

    RadioKind kind;
    HSTRING name;
};

static inline struct radio *impl_from_IRadio( IRadio *iface )
{
    return CONTAINING_RECORD( iface, struct radio, IRadio_iface );
}

static HRESULT WINAPI radio_QueryInterface( IRadio *iface, REFIID iid, void **out )
{
    struct radio *impl = impl_from_IRadio( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IRadio ))
    {
        *out = &impl->IRadio_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI radio_AddRef( IRadio *iface )
{
    struct radio *impl = impl_from_IRadio( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI radio_Release( IRadio *iface )
{
    struct radio *impl = impl_from_IRadio( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        WindowsDeleteString( impl->name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI radio_GetIids( IRadio *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI radio_GetRuntimeClassName( IRadio *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return WindowsCreateString( RuntimeClass_Windows_Devices_Radios_Radio,
                                wcslen( RuntimeClass_Windows_Devices_Radios_Radio ), class_name );
}

static HRESULT WINAPI radio_GetTrustLevel( IRadio *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI radio_SetStateAsync( IRadio *iface, RadioState value, IAsyncOperation_RadioAccessStatus **operation )
{
    FIXME( "iface %p, value %d, operation %p stub!\n", iface, value, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI radio_add_StateChanged( IRadio *iface, ITypedEventHandler_Radio_IInspectable *handler, EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p semi-stub!\n", iface, handler, token );

    /* The state never changes, so the handler would never be invoked. */
    if (!handler || !token) return E_INVALIDARG;
    token->value = 0xdeadbeef;
    return S_OK;
}

static HRESULT WINAPI radio_remove_StateChanged( IRadio *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x semi-stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI radio_get_State( IRadio *iface, RadioState *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    /* Radios are only reported while their adapter is present. */
    *value = RadioState_On;
    return S_OK;
}

static HRESULT WINAPI radio_get_Name( IRadio *iface, HSTRING *value )
{
    struct radio *impl = impl_from_IRadio( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl->name, value );
}

static HRESULT WINAPI radio_get_Kind( IRadio *iface, RadioKind *value )
{
    struct radio *impl = impl_from_IRadio( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->kind;
    return S_OK;
}

static const struct IRadioVtbl radio_vtbl =
{
    /* IUnknown methods */
    radio_QueryInterface,
    radio_AddRef,
    radio_Release,
    /* IInspectable methods */
    radio_GetIids,
    radio_GetRuntimeClassName,
    radio_GetTrustLevel,
    /* IRadio methods */
    radio_SetStateAsync,
    radio_add_StateChanged,
    radio_remove_StateChanged,
    radio_get_State,
    radio_get_Name,
    radio_get_Kind,
};

static HRESULT radio_create( RadioKind kind, const WCHAR *name, IRadio **out )
{
    struct radio *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IRadio_iface.lpVtbl = &radio_vtbl;
    impl->ref = 1;
    impl->kind = kind;
    if (FAILED(hr = WindowsCreateString( name, wcslen( name ), &impl->name )))
    {
        free( impl );
        return hr;
    }

    *out = &impl->IRadio_iface;
    return S_OK;
}

static HRESULT append_bluetooth_radios( IVector_IInspectable *vector )
{
    BLUETOOTH_FIND_RADIO_PARAMS params = { .dwSize = sizeof( params ) };
    HBLUETOOTH_RADIO_FIND find;
    HANDLE handle;
    HRESULT hr = S_OK;

    if (!(find = BluetoothFindFirstRadio( &params, &handle )))
    {
        TRACE( "no Bluetooth radios found, error %lu.\n", GetLastError() );
        return S_OK;
    }

    do
    {
        BLUETOOTH_RADIO_INFO info = { .dwSize = sizeof( info ) };
        IRadio *radio;

        if (BluetoothGetRadioInfo( handle, &info ) != ERROR_SUCCESS || !info.szName[0])
            wcscpy( info.szName, L"Bluetooth" );
        CloseHandle( handle );

        TRACE( "found Bluetooth radio %s.\n", debugstr_w( info.szName ) );
        if (FAILED(hr = radio_create( RadioKind_Bluetooth, info.szName, &radio ))) break;
        hr = IVector_IInspectable_Append( vector, (IInspectable *)radio );
        IRadio_Release( radio );
        if (FAILED(hr)) break;
    } while (BluetoothFindNextRadio( find, &handle ));

    BluetoothFindRadioClose( find );
    return hr;
}

struct radio_statics
{
    IActivationFactory IActivationFactory_iface;
    IRadioStatics IRadioStatics_iface;
    LONG ref;
};

static inline struct radio_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct radio_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct radio_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IRadioStatics ))
    {
        *out = &impl->IRadioStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct radio_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct radio_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    /* IUnknown methods */
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( radio_statics, IRadioStatics, struct radio_statics, IActivationFactory_iface )

static HRESULT get_radios_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct vector_iids iids =
    {
        .iterable = &IID_IIterable_Radio,
        .iterator = &IID_IIterator_Radio,
        .vector = &IID_IVector_IInspectable,
        .view = &IID_IVectorView_Radio,
    };
    IVector_IInspectable *vector;
    IVectorView_Radio *view;
    HRESULT hr;

    TRACE( "invoker %p, param %p, result %p, called_async %d.\n", invoker, param, result, called_async );

    if (FAILED(hr = vector_create( &iids, (void **)&vector ))) return hr;

    if (FAILED(hr = append_bluetooth_radios( vector )))
    {
        IVector_IInspectable_Release( vector );
        return hr;
    }

    hr = IVector_IInspectable_GetView( vector, (IVectorView_IInspectable **)&view );
    IVector_IInspectable_Release( vector );
    if (FAILED(hr)) return hr;

    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)view;
    return S_OK;
}

static HRESULT WINAPI radio_statics_GetRadiosAsync( IRadioStatics *iface, IAsyncOperation_IVectorView_Radio **value )
{
    TRACE( "iface %p, value %p\n", iface, value );

    if (!value) return E_POINTER;
    return async_operation_inspectable_create( &IID_IAsyncOperation_IVectorView_Radio, NULL, NULL, get_radios_async, (IAsyncOperation_IInspectable **)value );
}

static HRESULT WINAPI radio_statics_GetDeviceSelector( IRadioStatics *iface, HSTRING *selector )
{
    FIXME( "iface %p, selector %p stub!\n", iface, selector );
    return E_NOTIMPL;
}

static HRESULT WINAPI radio_statics_FromIdAsync( IRadioStatics *iface, HSTRING id, IAsyncOperation_Radio **value )
{
    FIXME( "iface %p, id %s, value %p stub!\n", iface, debugstr_hstring( id ), value );
    return E_NOTIMPL;
}

static HRESULT WINAPI radio_statics_RequestAccessAsync( IRadioStatics *iface, IAsyncOperation_RadioAccessStatus **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static const struct IRadioStaticsVtbl radio_statics_vtbl =
{
    /* IUnknown methods */
    radio_statics_QueryInterface,
    radio_statics_AddRef,
    radio_statics_Release,
    /* IInspectable methods */
    radio_statics_GetIids,
    radio_statics_GetRuntimeClassName,
    radio_statics_GetTrustLevel,
    /* IRadioStatics methods */
    radio_statics_GetRadiosAsync,
    radio_statics_GetDeviceSelector,
    radio_statics_FromIdAsync,
    radio_statics_RequestAccessAsync,
};

static struct radio_statics radio_statics =
{
    {&factory_vtbl},
    {&radio_statics_vtbl},
    1,
};

static IActivationFactory *radio_factory = &radio_statics.IActivationFactory_iface;

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *buffer = WindowsGetStringRawBuffer( classid, NULL );

    TRACE( "class %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;

    if (!wcscmp( buffer, RuntimeClass_Windows_Devices_Radios_Radio ))
        IActivationFactory_QueryInterface( radio_factory, &IID_IActivationFactory, (void **)factory );

    if (*factory) return S_OK;
    return CLASS_E_CLASSNOTAVAILABLE;
}
