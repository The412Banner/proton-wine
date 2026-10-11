/*
 * Copyright 2024 Rémi Bernon for CodeWeavers
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

#include <stddef.h>
#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"

#include "d3d9.h"
#include "dmoreg.h"
#include "dshow.h"
#include "mfapi.h"
#include "rpcproxy.h"
#include "wmcodecdsp.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dmo);

#include "initguid.h"

DEFINE_MEDIATYPE_GUID(MFVideoFormat_ABGR32, D3DFMT_A8B8G8R8);
DEFINE_GUID(DMOVideoFormat_RGB32,D3DFMT_X8R8G8B8,0x524f,0x11ce,0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70);
DEFINE_GUID(DMOVideoFormat_RGB24,D3DFMT_R8G8B8,0x524f,0x11ce,0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70);
DEFINE_GUID(DMOVideoFormat_RGB565,D3DFMT_R5G6B5,0x524f,0x11ce,0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70);
DEFINE_GUID(DMOVideoFormat_RGB555,D3DFMT_X1R5G5B5,0x524f,0x11ce,0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70);
DEFINE_GUID(DMOVideoFormat_RGB8,D3DFMT_P8,0x524f,0x11ce,0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70);

static HRESULT WINAPI resizer_factory_CreateInstance(IClassFactory *iface, IUnknown *outer,
        REFIID riid, void **out)
{
    static const GUID CLSID_winedmo_video_processor = {0xd527607f,0x89cb,0x4e94,{0x95,0x71,0xbc,0xfe,0x62,0x17,0x56,0x13}};
    return CoCreateInstance(&CLSID_winedmo_video_processor, outer, CLSCTX_INPROC_SERVER, riid, out);
}

static HRESULT WINAPI class_factory_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
    *out = IsEqualGUID(riid, &IID_IClassFactory) || IsEqualGUID(riid, &IID_IUnknown) ? iface : NULL;
    return *out ? S_OK : E_NOINTERFACE;
}

static ULONG WINAPI class_factory_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI class_factory_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI class_factory_LockServer(IClassFactory *iface, BOOL dolock)
{
    return S_OK;
}

static const IClassFactoryVtbl resizer_factory_vtbl =
{
    class_factory_QueryInterface,
    class_factory_AddRef,
    class_factory_Release,
    resizer_factory_CreateInstance,
    class_factory_LockServer,
};

static IClassFactory resizer_factory = {&resizer_factory_vtbl};

/***********************************************************************
 *              DllGetClassObject (msvproc.@)
 */
HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
    if (IsEqualGUID(clsid, &CLSID_CResizerDMO))
        return IClassFactory_QueryInterface(&resizer_factory, riid, out);

    *out = NULL;
    FIXME("Unknown clsid %s.\n", debugstr_guid(clsid));
    return CLASS_E_CLASSNOTAVAILABLE;
}

/***********************************************************************
 *              DllRegisterServer (colorcnv.@)
 */
HRESULT WINAPI DllRegisterServer(void)
{
    MFT_REGISTER_TYPE_INFO color_converter_mft_inputs[] =
    {
        {MFMediaType_Video, MFVideoFormat_YV12},
        {MFMediaType_Video, MFVideoFormat_YUY2},
        {MFMediaType_Video, MFVideoFormat_UYVY},
        {MFMediaType_Video, MFVideoFormat_AYUV},
        {MFMediaType_Video, MFVideoFormat_NV12},
        {MFMediaType_Video, MFVideoFormat_ARGB32},
        {MFMediaType_Video, DMOVideoFormat_RGB32},
        {MFMediaType_Video, DMOVideoFormat_RGB565},
        {MFMediaType_Video, MFVideoFormat_I420},
        {MFMediaType_Video, MFVideoFormat_IYUV},
        {MFMediaType_Video, MFVideoFormat_YVYU},
        {MFMediaType_Video, DMOVideoFormat_RGB24},
        {MFMediaType_Video, DMOVideoFormat_RGB555},
        {MFMediaType_Video, DMOVideoFormat_RGB8},
        {MFMediaType_Video, MEDIASUBTYPE_V216},
        {MFMediaType_Video, MEDIASUBTYPE_V410},
        {MFMediaType_Video, MFVideoFormat_NV11},
        {MFMediaType_Video, MFVideoFormat_Y41P},
        {MFMediaType_Video, MFVideoFormat_Y41T},
        {MFMediaType_Video, MFVideoFormat_Y42T},
        {MFMediaType_Video, MFVideoFormat_YVU9},
    };
    MFT_REGISTER_TYPE_INFO color_converter_mft_outputs[] =
    {
        {MFMediaType_Video, MFVideoFormat_YV12},
        {MFMediaType_Video, MFVideoFormat_YUY2},
        {MFMediaType_Video, MFVideoFormat_UYVY},
        {MFMediaType_Video, MFVideoFormat_AYUV},
        {MFMediaType_Video, MFVideoFormat_NV12},
        {MFMediaType_Video, MFVideoFormat_ARGB32},
        {MFMediaType_Video, DMOVideoFormat_RGB32},
        {MFMediaType_Video, DMOVideoFormat_RGB565},
        {MFMediaType_Video, MFVideoFormat_I420},
        {MFMediaType_Video, MFVideoFormat_IYUV},
        {MFMediaType_Video, MFVideoFormat_YVYU},
        {MFMediaType_Video, DMOVideoFormat_RGB24},
        {MFMediaType_Video, DMOVideoFormat_RGB555},
        {MFMediaType_Video, DMOVideoFormat_RGB8},
        {MFMediaType_Video, MEDIASUBTYPE_V216},
        {MFMediaType_Video, MEDIASUBTYPE_V410},
        {MFMediaType_Video, MFVideoFormat_NV11},
    };
    DMO_PARTIAL_MEDIATYPE color_converter_dmo_inputs[] =
    {
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_YV12},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_YUY2},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_UYVY},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_AYUV},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_NV12},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_RGB32},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_RGB565},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_I420},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_IYUV},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_YVYU},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_RGB24},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_RGB555},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_RGB8},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_V216},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_V410},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_NV11},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_Y41P},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_Y41T},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_Y42T},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_YVU9},
    };
    DMO_PARTIAL_MEDIATYPE color_converter_dmo_outputs[] =
    {
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_YV12},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_YUY2},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_UYVY},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_AYUV},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_NV12},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_RGB32},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_RGB565},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_I420},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_IYUV},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_YVYU},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_RGB24},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_RGB555},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_RGB8},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_V216},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_V410},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_NV11},
    };
    HRESULT hr;

    TRACE("\n");

    if (FAILED(hr = __wine_register_resources()))
        return hr;
    if (FAILED(hr = MFTRegister(CLSID_CResizerDMO, MFT_CATEGORY_VIDEO_EFFECT,
            (WCHAR *)L"Resizer MFT", MFT_ENUM_FLAG_SYNCMFT,
            ARRAY_SIZE(color_converter_mft_inputs), color_converter_mft_inputs,
            ARRAY_SIZE(color_converter_mft_outputs), color_converter_mft_outputs, NULL)))
        return hr;
    if (FAILED(hr = DMORegister(L"Resizer DMO", &CLSID_CResizerDMO, &DMOCATEGORY_VIDEO_EFFECT, 0,
            ARRAY_SIZE(color_converter_dmo_inputs), color_converter_dmo_inputs,
            ARRAY_SIZE(color_converter_dmo_outputs), color_converter_dmo_outputs)))
        return hr;

    return S_OK;
}

/***********************************************************************
 *              DllUnregisterServer (colorcnv.@)
 */
HRESULT WINAPI DllUnregisterServer(void)
{
    HRESULT hr;

    TRACE("\n");

    if (FAILED(hr = __wine_unregister_resources()))
        return hr;
    if (FAILED(hr = MFTUnregister(CLSID_CResizerDMO)))
        return hr;
    if (FAILED(hr = DMOUnregister(&CLSID_CResizerDMO, &DMOCATEGORY_VIDEO_EFFECT)))
        return hr;

    return S_OK;
}
