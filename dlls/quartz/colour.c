/*
 * DirectShow RGB color space converter
 *
 * Copyright 2026 GloriousEggroll
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

#include "quartz_private.h"
#include <limits.h>
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(quartz);

struct colour_quality
{
    IQualityControl IQualityControl_iface;
    struct strmbase_pin *pin;
    IQualityControl *sink;
};

struct colour
{
    struct strmbase_filter filter;
    struct strmbase_sink sink;
    struct strmbase_source source;
    struct strmbase_passthrough passthrough;
    struct colour_quality input_quality, output_quality;
};

static struct colour *impl_from_filter(struct strmbase_filter *filter)
{
    return CONTAINING_RECORD(filter, struct colour, filter);
}

static const struct
{
    const GUID *subtype;
    WORD bits;
    DWORD masks[3];
} rgb_formats[] =
{
    {&MEDIASUBTYPE_RGB32, 32},
    {&MEDIASUBTYPE_RGB24, 24},
    {&MEDIASUBTYPE_RGB565, 16, {0xf800, 0x07e0, 0x001f}},
    {&MEDIASUBTYPE_RGB555, 16, {0x7c00, 0x03e0, 0x001f}},
    {&MEDIASUBTYPE_RGB8, 8},
};

static DWORD image_size(const BITMAPINFOHEADER *header)
{
    return (((ULONGLONG)header->biWidth * header->biBitCount + 31) / 32 * 4)
            * abs(header->biHeight);
}

static BOOL valid_format(const AM_MEDIA_TYPE *mt)
{
    const BITMAPINFOHEADER *header;
    DWORD extra;
    unsigned int i;

    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Video)
            || !IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo)
            || !mt->pbFormat || mt->cbFormat < sizeof(VIDEOINFOHEADER))
        return FALSE;
    header = &((VIDEOINFOHEADER *)mt->pbFormat)->bmiHeader;
    if (header->biSize != sizeof(*header) || header->biPlanes != 1
            || header->biWidth <= 0 || !header->biHeight || header->biHeight == INT_MIN)
        return FALSE;
    for (i = 0; i < ARRAY_SIZE(rgb_formats); ++i)
        if (IsEqualGUID(&mt->subtype, rgb_formats[i].subtype)) break;
    if (i == ARRAY_SIZE(rgb_formats) || header->biBitCount != rgb_formats[i].bits)
        return FALSE;
    if (((ULONGLONG)header->biWidth * header->biBitCount + 31) / 32 * 4
            * abs(header->biHeight) > INT_MAX)
        return FALSE;
    if (header->biSizeImage && header->biSizeImage < image_size(header))
        return FALSE;

    if (header->biCompression == BI_BITFIELDS)
    {
        if (header->biBitCount != 16 && header->biBitCount != 32) return FALSE;
        if (mt->cbFormat < sizeof(VIDEOINFOHEADER) + 3 * sizeof(DWORD)) return FALSE;
        if (header->biBitCount == 16)
            return !memcmp(header + 1, rgb_formats[i].masks, 3 * sizeof(DWORD));
        return ((const DWORD *)(header + 1))[0] == 0xff0000
                && ((const DWORD *)(header + 1))[1] == 0x00ff00
                && ((const DWORD *)(header + 1))[2] == 0x0000ff;
    }
    if (header->biCompression != BI_RGB || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_RGB565))
        return FALSE;
    extra = header->biClrUsed;
    if (header->biBitCount == 8)
    {
        if (extra > 256) return FALSE;
        if (!extra) extra = 256;
    }
    return extra <= (mt->cbFormat - sizeof(VIDEOINFOHEADER)) / sizeof(RGBQUAD);
}

static BOOL same_size(const AM_MEDIA_TYPE *a, const AM_MEDIA_TYPE *b)
{
    const BITMAPINFOHEADER *ah = &((VIDEOINFOHEADER *)a->pbFormat)->bmiHeader;
    const BITMAPINFOHEADER *bh = &((VIDEOINFOHEADER *)b->pbFormat)->bmiHeader;

    return ah->biWidth == bh->biWidth && abs(ah->biHeight) == abs(bh->biHeight);
}

static HRESULT sink_query_accept(struct strmbase_pin *pin, const AM_MEDIA_TYPE *mt)
{
    struct colour *filter = impl_from_filter(pin->filter);

    if (!valid_format(mt)) return S_FALSE;
    if (filter->source.pin.peer && !same_size(mt, &filter->source.pin.mt)) return S_FALSE;
    return S_OK;
}

static HRESULT source_query_accept(struct strmbase_pin *pin, const AM_MEDIA_TYPE *mt)
{
    struct colour *filter = impl_from_filter(pin->filter);

    return filter->sink.pin.peer && valid_format(mt) && same_size(mt, &filter->sink.pin.mt) ? S_OK : S_FALSE;
}

static HRESULT source_get_media_type(struct strmbase_pin *pin, unsigned int index, AM_MEDIA_TYPE *mt)
{
    struct colour *filter = impl_from_filter(pin->filter);
    const VIDEOINFOHEADER *input = (VIDEOINFOHEADER *)filter->sink.pin.mt.pbFormat;
    VIDEOINFOHEADER *format;
    RGBQUAD *palette;
    unsigned int i;
    ULONG size;

    if (!filter->sink.pin.peer || index >= ARRAY_SIZE(rgb_formats)) return S_FALSE;
    size = sizeof(*format);
    if (rgb_formats[index].bits == 16) size += 3 * sizeof(DWORD);
    if (rgb_formats[index].bits == 8) size += 256 * sizeof(RGBQUAD);
    if (!(format = CoTaskMemAlloc(size))) return E_OUTOFMEMORY;
    memset(format, 0, size);
    format->AvgTimePerFrame = input->AvgTimePerFrame;
    format->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    format->bmiHeader.biWidth = input->bmiHeader.biWidth;
    format->bmiHeader.biHeight = input->bmiHeader.biHeight;
    format->bmiHeader.biPlanes = 1;
    format->bmiHeader.biBitCount = rgb_formats[index].bits;
    if (rgb_formats[index].bits == 16)
    {
        format->bmiHeader.biCompression = BI_BITFIELDS;
        memcpy(&format->bmiHeader + 1, rgb_formats[index].masks, 3 * sizeof(DWORD));
    }
    if (rgb_formats[index].bits == 8)
    {
        palette = (RGBQUAD *)(&format->bmiHeader + 1);
        format->bmiHeader.biClrUsed = 256;
        for (i = 0; i < 256; ++i)
        {
            palette[i].rgbRed = ((i >> 5) & 7) * 255 / 7;
            palette[i].rgbGreen = ((i >> 2) & 7) * 255 / 7;
            palette[i].rgbBlue = (i & 3) * 255 / 3;
        }
    }
    format->bmiHeader.biSizeImage = image_size(&format->bmiHeader);
    memset(mt, 0, sizeof(*mt));
    mt->majortype = MEDIATYPE_Video;
    mt->subtype = *rgb_formats[index].subtype;
    mt->formattype = FORMAT_VideoInfo;
    mt->bFixedSizeSamples = TRUE;
    mt->lSampleSize = format->bmiHeader.biSizeImage;
    mt->cbFormat = size;
    mt->pbFormat = (BYTE *)format;
    return S_OK;
}

static HRESULT WINAPI source_decide_buffer_size(struct strmbase_source *source, IMemAllocator *allocator,
        ALLOCATOR_PROPERTIES *props)
{
    const VIDEOINFOHEADER *format = (VIDEOINFOHEADER *)source->pin.mt.pbFormat;
    ALLOCATOR_PROPERTIES actual;
    HRESULT hr;

    props->cBuffers = max(props->cBuffers, 1);
    props->cbBuffer = max(props->cbBuffer, (LONG)image_size(&format->bmiHeader));
    props->cbAlign = max(props->cbAlign, 1);
    if (FAILED(hr = IMemAllocator_SetProperties(allocator, props, &actual))) return hr;
    return actual.cbBuffer < props->cbBuffer || actual.cBuffers < props->cBuffers ? E_FAIL : S_OK;
}

static HRESULT WINAPI sink_receive(struct strmbase_sink *pin, IMediaSample *sample)
{
    struct colour *filter = impl_from_filter(pin->pin.filter);
    const AM_MEDIA_TYPE *input_type = &pin->pin.mt;
    const VIDEOINFOHEADER *input, *output = (VIDEOINFOHEADER *)filter->source.pin.mt.pbFormat;
    AM_MEDIA_TYPE *dynamic_type = NULL;
    IMediaSample *converted = NULL;
    REFERENCE_TIME start, end;
    HBITMAP bitmap = NULL, old_bitmap = NULL;
    BYTE *src, *dst, *bits;
    HDC dc = NULL;
    DWORD size;
    HRESULT hr;

    if (filter->filter.state == State_Stopped) return VFW_E_WRONG_STATE;
    if (pin->flushing) return S_FALSE;
    if (!filter->source.pMemInputPin || !pin->pin.peer) return VFW_E_NOT_CONNECTED;
    if (IMediaSample_GetMediaType(sample, &dynamic_type) == S_OK)
    {
        if (sink_query_accept(&pin->pin, dynamic_type) != S_OK)
        {
            hr = VFW_E_TYPE_NOT_ACCEPTED;
            goto done;
        }
        FreeMediaType(&pin->pin.mt);
        pin->pin.mt = *dynamic_type;
        CoTaskMemFree(dynamic_type);
        dynamic_type = NULL;
    }
    input = (VIDEOINFOHEADER *)input_type->pbFormat;
    size = image_size(&output->bmiHeader);
    if (IMediaSample_GetActualDataLength(sample) < 0
            || (DWORD)IMediaSample_GetActualDataLength(sample) < image_size(&input->bmiHeader))
    {
        hr = E_INVALIDARG;
        goto done;
    }
    if (FAILED(hr = IMediaSample_GetPointer(sample, &src))) goto done;
    if (FAILED(hr = IMemAllocator_GetBuffer(filter->source.pAllocator, &converted, NULL, NULL, 0))) goto done;
    if (IMediaSample_GetSize(converted) < 0 || (DWORD)IMediaSample_GetSize(converted) < size)
    {
        hr = E_FAIL;
        goto done;
    }
    if (FAILED(hr = IMediaSample_GetPointer(converted, &dst))) goto done;

    /* Use a memory DIB only: no renderer window or display-driver resources.
     * GDI handles RGB palettes, bit masks, row padding and both orientations. */
    if (!(dc = CreateCompatibleDC(NULL))
            || !(bitmap = CreateDIBSection(dc, (BITMAPINFO *)&output->bmiHeader,
                    DIB_RGB_COLORS, (void **)&bits, NULL, 0)))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (!(old_bitmap = SelectObject(dc, bitmap)))
    {
        hr = E_FAIL;
        goto done;
    }
    memset(bits, 0, size);
    if (!SetDIBitsToDevice(dc, 0, 0, input->bmiHeader.biWidth, abs(input->bmiHeader.biHeight),
            0, 0, 0, abs(input->bmiHeader.biHeight), src, (BITMAPINFO *)&input->bmiHeader, DIB_RGB_COLORS))
    {
        hr = E_FAIL;
        goto done;
    }
    GdiFlush();
    memcpy(dst, bits, size);
    IMediaSample_SetActualDataLength(converted, size);
    IMediaSample_SetSyncPoint(converted, TRUE);
    IMediaSample_SetPreroll(converted, IMediaSample_IsPreroll(sample) == S_OK);
    IMediaSample_SetDiscontinuity(converted, IMediaSample_IsDiscontinuity(sample) == S_OK);
    hr = IMediaSample_GetTime(sample, &start, &end);
    if (hr == S_OK) IMediaSample_SetTime(converted, &start, &end);
    else if (hr == VFW_S_NO_STOP_TIME) IMediaSample_SetTime(converted, &start, NULL);
    else IMediaSample_SetTime(converted, NULL, NULL);
    if (IMediaSample_GetMediaTime(sample, &start, &end) == S_OK)
        IMediaSample_SetMediaTime(converted, &start, &end);
    else
        IMediaSample_SetMediaTime(converted, NULL, NULL);
    hr = pin->flushing ? S_FALSE : IMemInputPin_Receive(filter->source.pMemInputPin, converted);

done:
    if (old_bitmap) SelectObject(dc, old_bitmap);
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    if (converted) IMediaSample_Release(converted);
    if (dynamic_type) DeleteMediaType(dynamic_type);
    return hr;
}

static HRESULT sink_eos(struct strmbase_sink *pin)
{
    struct colour *filter = impl_from_filter(pin->pin.filter);
    return filter->source.pin.peer ? IPin_EndOfStream(filter->source.pin.peer) : S_OK;
}

static HRESULT sink_begin_flush(struct strmbase_sink *pin)
{
    struct colour *filter = impl_from_filter(pin->pin.filter);
    return filter->source.pin.peer ? IPin_BeginFlush(filter->source.pin.peer) : S_OK;
}

static HRESULT sink_end_flush(struct strmbase_sink *pin)
{
    struct colour *filter = impl_from_filter(pin->pin.filter);
    return filter->source.pin.peer ? IPin_EndFlush(filter->source.pin.peer) : S_OK;
}

static HRESULT sink_new_segment(struct strmbase_sink *pin, REFERENCE_TIME start, REFERENCE_TIME stop, double rate)
{
    struct colour *filter = impl_from_filter(pin->pin.filter);
    return filter->source.pin.peer ? IPin_NewSegment(filter->source.pin.peer, start, stop, rate) : S_OK;
}

static HRESULT pin_query_interface(struct strmbase_pin *pin, REFIID iid, void **out)
{
    struct colour *filter = impl_from_filter(pin->filter);

    if (pin->dir == PINDIR_INPUT && IsEqualGUID(iid, &IID_IMemInputPin))
        *out = &filter->sink.IMemInputPin_iface;
    else if (IsEqualGUID(iid, &IID_IQualityControl))
        *out = pin->dir == PINDIR_INPUT ? &filter->input_quality.IQualityControl_iface
                : &filter->output_quality.IQualityControl_iface;
    else if (pin->dir == PINDIR_OUTPUT && IsEqualGUID(iid, &IID_IMediaSeeking))
        *out = &filter->passthrough.IMediaSeeking_iface;
    else if (pin->dir == PINDIR_OUTPUT && IsEqualGUID(iid, &IID_IMediaPosition))
        *out = &filter->passthrough.IMediaPosition_iface;
    else
        return E_NOINTERFACE;
    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static const struct strmbase_sink_ops sink_ops =
{
    .base.pin_query_interface = pin_query_interface,
    .base.pin_query_accept = sink_query_accept,
    .pfnReceive = sink_receive,
    .sink_eos = sink_eos,
    .sink_begin_flush = sink_begin_flush,
    .sink_end_flush = sink_end_flush,
    .sink_new_segment = sink_new_segment,
};

static const struct strmbase_source_ops source_ops =
{
    .base.pin_query_interface = pin_query_interface,
    .base.pin_query_accept = source_query_accept,
    .base.pin_get_media_type = source_get_media_type,
    .pfnAttemptConnection = BaseOutputPinImpl_AttemptConnection,
    .pfnDecideAllocator = BaseOutputPinImpl_DecideAllocator,
    .pfnDecideBufferSize = source_decide_buffer_size,
};

static struct strmbase_pin *filter_get_pin(struct strmbase_filter *iface, unsigned int index)
{
    struct colour *filter = impl_from_filter(iface);
    if (!index) return &filter->sink.pin;
    if (index == 1) return &filter->source.pin;
    return NULL;
}

static HRESULT filter_init_stream(struct strmbase_filter *iface)
{
    struct colour *filter = impl_from_filter(iface);
    return filter->source.pin.peer ? IMemAllocator_Commit(filter->source.pAllocator) : S_OK;
}

static HRESULT filter_cleanup_stream(struct strmbase_filter *iface)
{
    struct colour *filter = impl_from_filter(iface);
    return filter->source.pin.peer ? IMemAllocator_Decommit(filter->source.pAllocator) : S_OK;
}

static void filter_destroy(struct strmbase_filter *iface)
{
    struct colour *filter = impl_from_filter(iface);
    strmbase_passthrough_cleanup(&filter->passthrough);
    strmbase_source_cleanup(&filter->source);
    strmbase_sink_cleanup(&filter->sink);
    strmbase_filter_cleanup(&filter->filter);
    free(filter);
}

static const struct strmbase_filter_ops filter_ops =
{
    .filter_get_pin = filter_get_pin,
    .filter_destroy = filter_destroy,
    .filter_init_stream = filter_init_stream,
    .filter_cleanup_stream = filter_cleanup_stream,
};

static struct colour_quality *impl_from_quality(IQualityControl *iface)
{
    return CONTAINING_RECORD(iface, struct colour_quality, IQualityControl_iface);
}

static HRESULT WINAPI quality_QueryInterface(IQualityControl *iface, REFIID iid, void **out)
{
    return IPin_QueryInterface(&impl_from_quality(iface)->pin->IPin_iface, iid, out);
}

static ULONG WINAPI quality_AddRef(IQualityControl *iface)
{
    return IPin_AddRef(&impl_from_quality(iface)->pin->IPin_iface);
}

static ULONG WINAPI quality_Release(IQualityControl *iface)
{
    return IPin_Release(&impl_from_quality(iface)->pin->IPin_iface);
}

static HRESULT WINAPI quality_Notify(IQualityControl *iface, IBaseFilter *sender, Quality quality)
{
    struct colour_quality *qc = impl_from_quality(iface);
    struct colour *filter = impl_from_filter(qc->pin->filter);
    IQualityControl *upstream;
    HRESULT hr;

    if (qc->sink) return IQualityControl_Notify(qc->sink, &filter->filter.IBaseFilter_iface, quality);
    if (!filter->sink.pin.peer) return VFW_E_NOT_FOUND;
    if (FAILED(hr = IPin_QueryInterface(filter->sink.pin.peer, &IID_IQualityControl, (void **)&upstream))) return hr;
    hr = IQualityControl_Notify(upstream, &filter->filter.IBaseFilter_iface, quality);
    IQualityControl_Release(upstream);
    return hr;
}

static HRESULT WINAPI quality_SetSink(IQualityControl *iface, IQualityControl *sink)
{
    impl_from_quality(iface)->sink = sink;
    return S_OK;
}

static const IQualityControlVtbl quality_vtbl =
{
    quality_QueryInterface,
    quality_AddRef,
    quality_Release,
    quality_Notify,
    quality_SetSink,
};

HRESULT colour_create(IUnknown *outer, IUnknown **out)
{
    struct colour *filter;

    TRACE("outer %p, out %p.\n", outer, out);

    if (!(filter = calloc(1, sizeof(*filter)))) return E_OUTOFMEMORY;
    strmbase_filter_init(&filter->filter, outer, &CLSID_Colour, &filter_ops);
    strmbase_sink_init(&filter->sink, &filter->filter, L"Input", &sink_ops, NULL);
    strmbase_source_init(&filter->source, &filter->filter, L"Output", &source_ops);
    strmbase_passthrough_init(&filter->passthrough, (IUnknown *)&filter->source.pin.IPin_iface);
    ISeekingPassThru_Init(&filter->passthrough.ISeekingPassThru_iface, FALSE, &filter->sink.pin.IPin_iface);
    filter->input_quality.IQualityControl_iface.lpVtbl = &quality_vtbl;
    filter->input_quality.pin = &filter->sink.pin;
    filter->output_quality.IQualityControl_iface.lpVtbl = &quality_vtbl;
    filter->output_quality.pin = &filter->source.pin;
    *out = &filter->filter.IUnknown_inner;
    return S_OK;
}
