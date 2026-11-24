/*
 * Implementation of the SmartTee filter
 *
 * Copyright 2015 Damjan Jovanovic
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

#include "qcap_private.h"

#include <limits.h>

WINE_DEFAULT_DEBUG_CHANNEL(quartz);

typedef struct {
    struct strmbase_filter filter;
    struct strmbase_sink sink;
    struct strmbase_source capture, preview;
} SmartTeeFilter;

static inline SmartTeeFilter *impl_from_strmbase_filter(struct strmbase_filter *filter)
{
    return CONTAINING_RECORD(filter, SmartTeeFilter, filter);
}

static inline SmartTeeFilter *impl_from_strmbase_pin(struct strmbase_pin *pin)
{
    return impl_from_strmbase_filter(pin->filter);
}

static struct strmbase_pin *smart_tee_get_pin(struct strmbase_filter *iface, unsigned int index)
{
    SmartTeeFilter *filter = impl_from_strmbase_filter(iface);

    if (index == 0)
        return &filter->sink.pin;
    else if (index == 1)
        return &filter->capture.pin;
    else if (index == 2)
        return &filter->preview.pin;
    return NULL;
}

static void smart_tee_destroy(struct strmbase_filter *iface)
{
    SmartTeeFilter *filter = impl_from_strmbase_filter(iface);

    strmbase_sink_cleanup(&filter->sink);
    strmbase_source_cleanup(&filter->capture);
    strmbase_source_cleanup(&filter->preview);
    strmbase_filter_cleanup(&filter->filter);
    free(filter);
}

static HRESULT smart_tee_wait_state(struct strmbase_filter *iface, DWORD timeout)
{
    return iface->state == State_Paused ? VFW_S_CANT_CUE : S_OK;
}

static const struct strmbase_filter_ops filter_ops =
{
    .filter_get_pin = smart_tee_get_pin,
    .filter_destroy = smart_tee_destroy,
    .filter_wait_state = smart_tee_wait_state,
};

static HRESULT sink_query_accept(struct strmbase_pin *base, const AM_MEDIA_TYPE *pmt)
{
    SmartTeeFilter *This = impl_from_strmbase_pin(base);
    TRACE("(%p, AM_MEDIA_TYPE(%p))\n", This, pmt);
    if (!pmt)
        return VFW_E_TYPE_NOT_ACCEPTED;
    /* We'll take any media type, but the output pins will later
     * struggle to connect downstream. */
    return S_OK;
}

static HRESULT sink_get_media_type(struct strmbase_pin *base,
        unsigned int iPosition, AM_MEDIA_TYPE *amt)
{
    SmartTeeFilter *This = impl_from_strmbase_pin(base);
    HRESULT hr;
    TRACE("(%p)->(%d, %p)\n", This, iPosition, amt);
    if (iPosition)
        return S_FALSE;
    EnterCriticalSection(&This->filter.filter_cs);
    if (This->sink.pin.peer)
    {
        CopyMediaType(amt, &This->sink.pin.mt);
        hr = S_OK;
    }
    else
        hr = S_FALSE;
    LeaveCriticalSection(&This->filter.filter_cs);
    return hr;
}

static HRESULT sink_query_interface(struct strmbase_pin *iface, REFIID iid, void **out)
{
    SmartTeeFilter *filter = impl_from_strmbase_pin(iface);

    if (IsEqualGUID(iid, &IID_IMemInputPin))
        *out = &filter->sink.IMemInputPin_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT copy_sample(IMediaSample *inputSample, IMemAllocator *allocator, IMediaSample **pOutputSample)
{
    REFERENCE_TIME startTime, endTime;
    BOOL haveStartTime = TRUE, haveEndTime = TRUE;
    IMediaSample *outputSample = NULL;
    BYTE *ptrIn, *ptrOut;
    AM_MEDIA_TYPE *mediaType = NULL;
    HRESULT hr;

    hr = IMediaSample_GetTime(inputSample, &startTime, &endTime);
    if (hr == S_OK)
        ;
    else if (hr == VFW_S_NO_STOP_TIME)
        haveEndTime = FALSE;
    else if (hr == VFW_E_SAMPLE_TIME_NOT_SET)
        haveStartTime = haveEndTime = FALSE;
    else
        goto end;

    hr = IMemAllocator_GetBuffer(allocator, &outputSample,
            haveStartTime ? &startTime : NULL, haveEndTime ? &endTime : NULL, 0);
    if (hr == VFW_E_NOT_COMMITTED)
    {
        HRESULT commit_hr;
        commit_hr = IMemAllocator_Commit(allocator);
        if (SUCCEEDED(commit_hr))
            hr = IMemAllocator_GetBuffer(allocator, &outputSample,
                    haveStartTime ? &startTime : NULL, haveEndTime ? &endTime : NULL, 0);
    }
    if (FAILED(hr)) goto end;
    if (IMediaSample_GetSize(outputSample) < IMediaSample_GetActualDataLength(inputSample)) {
        ERR("insufficient space in sample\n");
        hr = VFW_E_BUFFER_OVERFLOW;
        goto end;
    }

    hr = IMediaSample_SetTime(outputSample, haveStartTime ? &startTime : NULL, haveEndTime ? &endTime : NULL);
    if (FAILED(hr)) goto end;

    hr = IMediaSample_GetPointer(inputSample, &ptrIn);
    if (FAILED(hr)) goto end;
    hr = IMediaSample_GetPointer(outputSample, &ptrOut);
    if (FAILED(hr)) goto end;
    memcpy(ptrOut, ptrIn, IMediaSample_GetActualDataLength(inputSample));
    IMediaSample_SetActualDataLength(outputSample, IMediaSample_GetActualDataLength(inputSample));

    hr = IMediaSample_SetDiscontinuity(outputSample, IMediaSample_IsDiscontinuity(inputSample) == S_OK);
    if (FAILED(hr)) goto end;

    haveStartTime = haveEndTime = TRUE;
    hr = IMediaSample_GetMediaTime(inputSample, &startTime, &endTime);
    if (hr == S_OK)
        ;
    else if (hr == VFW_S_NO_STOP_TIME)
        haveEndTime = FALSE;
    else if (hr == VFW_E_MEDIA_TIME_NOT_SET)
        haveStartTime = haveEndTime = FALSE;
    else
        goto end;
    hr = IMediaSample_SetMediaTime(outputSample, haveStartTime ? &startTime : NULL, haveEndTime ? &endTime : NULL);
    if (FAILED(hr)) goto end;

    hr = IMediaSample_GetMediaType(inputSample, &mediaType);
    if (FAILED(hr)) goto end;
    if (hr == S_OK) {
        hr = IMediaSample_SetMediaType(outputSample, mediaType);
        if (FAILED(hr)) goto end;
    }

    hr = IMediaSample_SetPreroll(outputSample, IMediaSample_IsPreroll(inputSample) == S_OK);
    if (FAILED(hr)) goto end;

    hr = IMediaSample_SetSyncPoint(outputSample, IMediaSample_IsSyncPoint(inputSample) == S_OK);
    if (FAILED(hr)) goto end;

end:
    if (mediaType)
        DeleteMediaType(mediaType);
    if (FAILED(hr) && outputSample) {
        IMediaSample_Release(outputSample);
        outputSample = NULL;
    }
    *pOutputSample = outputSample;
    return hr;
}

static BOOL mt_is_rgb24_video(const AM_MEDIA_TYPE *mt)
{
    return IsEqualGUID(&mt->majortype, &MEDIATYPE_Video)
            && IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_RGB24)
            && IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo)
            && mt->cbFormat >= sizeof(VIDEOINFOHEADER)
            && mt->pbFormat;
}

static BOOL mt_is_rgb32_video(const AM_MEDIA_TYPE *mt)
{
    return IsEqualGUID(&mt->majortype, &MEDIATYPE_Video)
            && IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_RGB32)
            && IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo)
            && mt->cbFormat >= sizeof(VIDEOINFOHEADER)
            && mt->pbFormat;
}

static HRESULT convert_sample_rgb24_to_rgb32(IMediaSample *sample, const AM_MEDIA_TYPE *src_mt,
        const AM_MEDIA_TYPE *dst_mt)
{
    const VIDEOINFOHEADER *src_vih = (const VIDEOINFOHEADER *)src_mt->pbFormat;
    const VIDEOINFOHEADER *dst_vih = (const VIDEOINFOHEADER *)dst_mt->pbFormat;
    BYTE *data;
    LONG input_len, output_len, sample_size;
    LONG src_row, dst_row, width, height, row;
    LONG src_offset, dst_offset;
    HRESULT hr;

    if (!src_mt->pbFormat || src_mt->cbFormat < sizeof(*src_vih)
            || !dst_mt->pbFormat || dst_mt->cbFormat < sizeof(*dst_vih))
        return VFW_E_INVALIDMEDIATYPE;

    width = src_vih->bmiHeader.biWidth;
    if (width < 0)
        width = -width;
    height = src_vih->bmiHeader.biHeight;
    if (height < 0)
        height = -height;

    if (!width || !height)
        return VFW_E_INVALIDMEDIATYPE;

    if (width > LONG_MAX / 32)
        return VFW_E_INVALIDMEDIATYPE;

    src_row = ((width * 24 + 31) / 32) * 4;
    dst_row = ((width * 32 + 31) / 32) * 4;

    if (height > LONG_MAX / src_row || height > LONG_MAX / dst_row)
        return VFW_E_BUFFER_OVERFLOW;

    input_len = src_row * height;
    output_len = dst_row * height;

    sample_size = IMediaSample_GetActualDataLength(sample);
    if (sample_size < input_len)
        return VFW_E_TYPE_NOT_ACCEPTED;

    sample_size = IMediaSample_GetSize(sample);
    if (output_len > sample_size)
        return VFW_E_BUFFER_OVERFLOW;

    if (FAILED(hr = IMediaSample_GetPointer(sample, &data)))
        return hr;

    for (row = height - 1; row >= 0; --row)
    {
        LONG x;

        src_offset = row * src_row;
        dst_offset = row * dst_row;

        for (x = width - 1; x >= 0; --x)
        {
            BYTE b = data[src_offset + x * 3 + 0];
            BYTE g = data[src_offset + x * 3 + 1];
            BYTE r = data[src_offset + x * 3 + 2];

            data[dst_offset + x * 4 + 0] = b;
            data[dst_offset + x * 4 + 1] = g;
            data[dst_offset + x * 4 + 2] = r;
            data[dst_offset + x * 4 + 3] = 0xff;
        }
    }

    return IMediaSample_SetActualDataLength(sample, output_len);
}

static HRESULT WINAPI SmartTeeFilterInput_Receive(struct strmbase_sink *base, IMediaSample *inputSample)
{
    SmartTeeFilter *This = impl_from_strmbase_pin(&base->pin);
    IMediaSample *captureSample = NULL;
    IMediaSample *previewSample = NULL;
    HRESULT hrCapture = VFW_E_NOT_CONNECTED, hrPreview = VFW_E_NOT_CONNECTED;

    TRACE("(%p)->(%p)\n", This, inputSample);

    /* Modifying the image coming out of one pin doesn't modify the image
     * coming out of the other. MSDN claims the filter doesn't copy,
     * but unless it somehow uses copy-on-write, I just don't see how
     * that's possible. */

    /* FIXME: we should ideally do each of these in a separate thread */
    EnterCriticalSection(&This->filter.filter_cs);
    if (This->capture.pin.peer)
        hrCapture = copy_sample(inputSample, This->capture.pAllocator, &captureSample);
    LeaveCriticalSection(&This->filter.filter_cs);
    if (SUCCEEDED(hrCapture) && This->capture.pMemInputPin)
        hrCapture = IMemInputPin_Receive(This->capture.pMemInputPin, captureSample);
    if (captureSample)
        IMediaSample_Release(captureSample);

    EnterCriticalSection(&This->filter.filter_cs);
    if (This->preview.pin.peer)
        hrPreview = copy_sample(inputSample, This->preview.pAllocator, &previewSample);
    LeaveCriticalSection(&This->filter.filter_cs);

    if (SUCCEEDED(hrPreview) && mt_is_rgb24_video(&This->sink.pin.mt)
            && mt_is_rgb32_video(&This->preview.pin.mt))
        hrPreview = convert_sample_rgb24_to_rgb32(previewSample, &This->sink.pin.mt, &This->preview.pin.mt);

    /* No timestamps on preview stream: */
    if (SUCCEEDED(hrPreview))
        hrPreview = IMediaSample_SetTime(previewSample, NULL, NULL);
    if (SUCCEEDED(hrPreview) && This->preview.pMemInputPin)
        hrPreview = IMemInputPin_Receive(This->preview.pMemInputPin, previewSample);
    if (previewSample)
        IMediaSample_Release(previewSample);

    /* FIXME: how to merge the HRESULTs from the 2 pins? */
    if (SUCCEEDED(hrCapture))
        return hrCapture;
    else
        return hrPreview;
}

static const struct strmbase_sink_ops sink_ops =
{
    .base.pin_query_accept = sink_query_accept,
    .base.pin_get_media_type = sink_get_media_type,
    .base.pin_query_interface = sink_query_interface,
    .pfnReceive = SmartTeeFilterInput_Receive,
};

static HRESULT capture_query_accept(struct strmbase_pin *base, const AM_MEDIA_TYPE *amt)
{
    FIXME("(%p) stub\n", base);
    return S_OK;
}

static HRESULT source_get_media_type(struct strmbase_pin *iface,
        unsigned int index, AM_MEDIA_TYPE *mt)
{
    SmartTeeFilter *filter = impl_from_strmbase_pin(iface);
    HRESULT hr = S_OK;
    BOOL is_preview = iface == &filter->preview.pin;
    BOOL rgb24_video;

    EnterCriticalSection(&filter->filter.filter_cs);

    if (!filter->sink.pin.peer)
        hr = VFW_E_NOT_CONNECTED;
    else if (is_preview)
    {
        rgb24_video = mt_is_rgb24_video(&filter->sink.pin.mt);

        if (!index)
        {
            CopyMediaType(mt, &filter->sink.pin.mt);
            if (rgb24_video)
            {
                VIDEOINFOHEADER *vih = (VIDEOINFOHEADER *)mt->pbFormat;
                LONG width = vih->bmiHeader.biWidth;
                LONG height = vih->bmiHeader.biHeight < 0 ? -vih->bmiHeader.biHeight : vih->bmiHeader.biHeight;
                LONG row_size;

                if (width < 0)
                    width = -width;

                if (!width || !height || width > LONG_MAX / 4 || height > LONG_MAX / (width * 4))
                    hr = VFW_E_INVALIDMEDIATYPE;
                else
                {
                    row_size = ((width * 32 + 31) / 32) * 4;

                    mt->subtype = MEDIASUBTYPE_RGB32;
                    mt->lSampleSize = row_size * height;
                    vih->bmiHeader.biBitCount = 32;
                    vih->bmiHeader.biCompression = BI_RGB;
                    vih->bmiHeader.biSizeImage = mt->lSampleSize;
                }
            }
        }
        else if (index == 1 && rgb24_video)
            CopyMediaType(mt, &filter->sink.pin.mt);
        else
            hr = VFW_S_NO_MORE_ITEMS;
    }
    else if (!index)
        CopyMediaType(mt, &filter->sink.pin.mt);
    else
        hr = VFW_S_NO_MORE_ITEMS;

    LeaveCriticalSection(&filter->filter.filter_cs);
    return hr;
}

static HRESULT WINAPI SmartTeeFilterCapture_DecideAllocator(struct strmbase_source *base,
        IMemInputPin *pPin, IMemAllocator **pAlloc)
{
    SmartTeeFilter *This = impl_from_strmbase_pin(&base->pin);
    TRACE("(%p, %p, %p)\n", This, pPin, pAlloc);
    *pAlloc = This->sink.pAllocator;
    IMemAllocator_AddRef(This->sink.pAllocator);
    return IMemInputPin_NotifyAllocator(pPin, This->sink.pAllocator, TRUE);
}

static const struct strmbase_source_ops capture_ops =
{
    .base.pin_query_accept = capture_query_accept,
    .base.pin_get_media_type = source_get_media_type,
    .pfnAttemptConnection = BaseOutputPinImpl_AttemptConnection,
    .pfnDecideAllocator = SmartTeeFilterCapture_DecideAllocator,
};

static HRESULT preview_query_accept(struct strmbase_pin *base, const AM_MEDIA_TYPE *amt)
{
    FIXME("(%p) stub\n", base);
    return S_OK;
}

static HRESULT WINAPI SmartTeeFilterPreview_DecideAllocator(struct strmbase_source *base,
        IMemInputPin *pPin, IMemAllocator **pAlloc)
{
    SmartTeeFilter *This = impl_from_strmbase_pin(&base->pin);
    TRACE("(%p, %p, %p)\n", This, pPin, pAlloc);
    return BaseOutputPinImpl_DecideAllocator(base, pPin, pAlloc);
}

static HRESULT WINAPI SmartTeeFilterPreview_DecideBufferSize(struct strmbase_source *base,
        IMemAllocator *allocator, ALLOCATOR_PROPERTIES *props)
{
    SmartTeeFilter *filter = impl_from_strmbase_pin(&base->pin);
    ALLOCATOR_PROPERTIES input_props = {0}, ret_props;
    BOOL have_input_props = FALSE;
    HRESULT hr;

    if (!props)
        return E_POINTER;

    if (filter->sink.pAllocator
            && SUCCEEDED(IMemAllocator_GetProperties(filter->sink.pAllocator, &input_props)))
        have_input_props = TRUE;

    if (!props->cBuffers)
        props->cBuffers = have_input_props && input_props.cBuffers ? input_props.cBuffers : 3;

    if (!props->cbBuffer)
    {
        if (base->pin.mt.lSampleSize)
        {
            props->cbBuffer = base->pin.mt.lSampleSize;
        }
        else if (IsEqualGUID(&base->pin.mt.formattype, &FORMAT_VideoInfo)
                 && base->pin.mt.cbFormat >= sizeof(VIDEOINFOHEADER)
                 && base->pin.mt.pbFormat)
        {
            VIDEOINFOHEADER *format = (VIDEOINFOHEADER *)base->pin.mt.pbFormat;
            props->cbBuffer = format->bmiHeader.biSizeImage;
        }
        else if (have_input_props && input_props.cbBuffer)
        {
            props->cbBuffer = input_props.cbBuffer;
        }

        if (!props->cbBuffer)
            props->cbBuffer = 65536;
    }

    if (!props->cbAlign)
        props->cbAlign = have_input_props && input_props.cbAlign ? input_props.cbAlign : 1;

    if (!props->cbPrefix && have_input_props && input_props.cbPrefix)
        props->cbPrefix = input_props.cbPrefix;

    hr = IMemAllocator_SetProperties(allocator, props, &ret_props);
    return SUCCEEDED(hr) ? S_OK : hr;
}

static const struct strmbase_source_ops preview_ops =
{
    .base.pin_query_accept = preview_query_accept,
    .base.pin_get_media_type = source_get_media_type,
    .pfnAttemptConnection = BaseOutputPinImpl_AttemptConnection,
    .pfnDecideAllocator = SmartTeeFilterPreview_DecideAllocator,
    .pfnDecideBufferSize = SmartTeeFilterPreview_DecideBufferSize,
};

HRESULT smart_tee_create(IUnknown *outer, IUnknown **out)
{
    SmartTeeFilter *object;
    HRESULT hr;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    strmbase_filter_init(&object->filter, outer, &CLSID_SmartTee, &filter_ops);
    strmbase_sink_init(&object->sink, &object->filter, L"Input", &sink_ops, NULL);
    hr = CoCreateInstance(&CLSID_MemoryAllocator, NULL, CLSCTX_INPROC_SERVER,
            &IID_IMemAllocator, (void **)&object->sink.pAllocator);
    if (FAILED(hr))
    {
        strmbase_filter_cleanup(&object->filter);
        free(object);
        return hr;
    }

    strmbase_source_init(&object->capture, &object->filter, L"Capture", &capture_ops);
    strmbase_source_init(&object->preview, &object->filter, L"Preview", &preview_ops);

    TRACE("Created smart tee %p.\n", object);
    *out = &object->filter.IUnknown_inner;
    return S_OK;
}
