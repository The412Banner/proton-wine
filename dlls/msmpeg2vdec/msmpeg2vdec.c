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

#include "video_decoder.h"

#include "dmoreg.h"
#include "rpcproxy.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dmo);

static const GUID MFVideoFormat_mp4v = {MAKEFOURCC('m','p','4','v'), 0x0000, 0x0010,
        {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID CLSID_MPEG4Part2DecoderMFT = {0x2d709e52, 0x123f, 0x49b5,
        {0x9c, 0xbc, 0x9a, 0xf5, 0xcd, 0xe2, 0x8f, 0xb9}};

/***********************************************************************
 *              DllGetClassObject (msmpeg2vdec.@)
 */
HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
    if (IsEqualGUID(clsid, &CLSID_MSH264DecoderMFT) ||
            IsEqualGUID(clsid, &CLSID_MPEG4Part2DecoderMFT))
        return IClassFactory_QueryInterface(&h264_decoder_factory, riid, out);

    *out = NULL;
    FIXME("Unknown clsid %s.\n", debugstr_guid(clsid));
    return CLASS_E_CLASSNOTAVAILABLE;
}

/***********************************************************************
 *              DllRegisterServer (msmpeg2vdec.@)
 */
HRESULT WINAPI DllRegisterServer(void)
{
    MFT_REGISTER_TYPE_INFO h264_decoder_mft_inputs[] =
    {
        {MFMediaType_Video, MFVideoFormat_H264},
        {MFMediaType_Video, MFVideoFormat_H264_ES},
    };
    MFT_REGISTER_TYPE_INFO h264_decoder_mft_outputs[] =
    {
        {MFMediaType_Video, MFVideoFormat_NV12},
        {MFMediaType_Video, MFVideoFormat_YV12},
        {MFMediaType_Video, MFVideoFormat_IYUV},
        {MFMediaType_Video, MFVideoFormat_I420},
        {MFMediaType_Video, MFVideoFormat_YUY2},
    };
    DMO_PARTIAL_MEDIATYPE h264_decoder_dmo_inputs[] =
    {
        {.type = MEDIATYPE_Video, .subtype = MFVideoFormat_H264},
        {.type = MEDIATYPE_Video, .subtype = MFVideoFormat_H264_ES},
    };
    DMO_PARTIAL_MEDIATYPE h264_decoder_dmo_outputs[] =
    {
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_NV12},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_YV12},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_IYUV},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_I420},
        {.type = MEDIATYPE_Video, .subtype = MEDIASUBTYPE_YUY2},
    };
    MFT_REGISTER_TYPE_INFO mpeg4_decoder_mft_inputs[] =
    {
        {MFMediaType_Video, MFVideoFormat_M4S2},
        {MFMediaType_Video, MFVideoFormat_MP4S},
        {MFMediaType_Video, MFVideoFormat_MP4V},
        {MFMediaType_Video, MFVideoFormat_mp4v},
    };
    HRESULT hr;

    TRACE("\n");

    if (FAILED(hr = __wine_register_resources()))
        return hr;
    if (FAILED(hr = MFTRegister(CLSID_MSH264DecoderMFT, MFT_CATEGORY_VIDEO_DECODER,
            (WCHAR *)L"Microsoft H264 Video Decoder MFT", MFT_ENUM_FLAG_SYNCMFT,
            ARRAY_SIZE(h264_decoder_mft_inputs), h264_decoder_mft_inputs,
            ARRAY_SIZE(h264_decoder_mft_outputs), h264_decoder_mft_outputs, NULL)))
        return hr;
    if (FAILED(hr = DMORegister(L"Microsoft H264 Video Decoder DMO", &CLSID_MSH264DecoderMFT,
            &DMOCATEGORY_VIDEO_DECODER, 0, ARRAY_SIZE(h264_decoder_dmo_inputs), h264_decoder_dmo_inputs,
            ARRAY_SIZE(h264_decoder_dmo_outputs), h264_decoder_dmo_outputs)))
        return hr;
    if (FAILED(hr = MFTRegister(CLSID_MPEG4Part2DecoderMFT, MFT_CATEGORY_VIDEO_DECODER,
            (WCHAR *)L"Microsoft MPEG4 Part 2 Video Decoder MFT", MFT_ENUM_FLAG_SYNCMFT,
            ARRAY_SIZE(mpeg4_decoder_mft_inputs), mpeg4_decoder_mft_inputs,
            ARRAY_SIZE(h264_decoder_mft_outputs), h264_decoder_mft_outputs, NULL)))
        return hr;

    return S_OK;
}

/***********************************************************************
 *              DllUnregisterServer (msmpeg2vdec.@)
 */
HRESULT WINAPI DllUnregisterServer(void)
{
    HRESULT hr;

    TRACE("\n");

    if (FAILED(hr = __wine_unregister_resources()))
        return hr;
    if (FAILED(hr = MFTUnregister(CLSID_MSH264DecoderMFT)))
        return hr;
    if (FAILED(hr = DMOUnregister(&CLSID_MSH264DecoderMFT, &DMOCATEGORY_VIDEO_DECODER)))
        return hr;
    if (FAILED(hr = MFTUnregister(CLSID_MPEG4Part2DecoderMFT)))
        return hr;

    return S_OK;
}
