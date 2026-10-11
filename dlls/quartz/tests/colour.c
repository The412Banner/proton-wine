/*
 * Color Space Converter tests
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

#define COBJMACROS
#include "dshow.h"
#include "wine/strmbase.h"
#include "wine/test.h"

struct testfilter
{
    struct strmbase_filter filter;
    struct strmbase_source source;
    struct strmbase_sink sink;
    unsigned int received, eos, segments;
};

static const BYTE colors[6][3] =
{
    {0, 0, 255}, {0, 255, 0}, {255, 0, 0},
    {255, 255, 255}, {0, 0, 0}, {0, 255, 255},
};

struct video_format
{
    VIDEOINFOHEADER video;
    DWORD masks[3];
};

static const GUID *const subtypes[] =
{
    &MEDIASUBTYPE_RGB24, &MEDIASUBTYPE_RGB32, &MEDIASUBTYPE_RGB565, &MEDIASUBTYPE_RGB555,
};

static void init_format(AM_MEDIA_TYPE *mt, struct video_format *format, unsigned int type, LONG height)
{
    BITMAPINFOHEADER *header = &format->video.bmiHeader;

    memset(format, 0, sizeof(*format));
    format->video.AvgTimePerFrame = 400000;
    header->biSize = sizeof(*header);
    header->biWidth = 3;
    header->biHeight = height;
    header->biPlanes = 1;
    header->biBitCount = type == 0 ? 24 : type == 1 ? 32 : 16;
    header->biSizeImage = ((3 * header->biBitCount + 31) / 32 * 4) * 2;
    if (type >= 2)
    {
        header->biCompression = BI_BITFIELDS;
        format->masks[0] = type == 2 ? 0xf800 : 0x7c00;
        format->masks[1] = type == 2 ? 0x07e0 : 0x03e0;
        format->masks[2] = 0x001f;
    }
    memset(mt, 0, sizeof(*mt));
    mt->majortype = MEDIATYPE_Video;
    mt->subtype = *subtypes[type];
    mt->formattype = FORMAT_VideoInfo;
    mt->bFixedSizeSamples = TRUE;
    mt->lSampleSize = header->biSizeImage;
    mt->cbFormat = type >= 2 ? sizeof(*format) : sizeof(VIDEOINFOHEADER);
    mt->pbFormat = (BYTE *)format;
}

static void encode_pixel(BYTE *dst, unsigned int type, const BYTE *bgr)
{
    if (type < 2) memcpy(dst, bgr, 3);
    else
    {
        WORD value = ((bgr[2] >> 3) << (type == 2 ? 11 : 10))
                | ((bgr[1] >> (type == 2 ? 2 : 3)) << 5) | (bgr[0] >> 3);
        memcpy(dst, &value, 2);
    }
}

static struct testfilter *impl_from_filter(struct strmbase_filter *filter)
{
    return CONTAINING_RECORD(filter, struct testfilter, filter);
}

static struct strmbase_pin *testfilter_get_pin(struct strmbase_filter *iface, unsigned int index)
{
    struct testfilter *filter = impl_from_filter(iface);
    if (!index) return &filter->sink.pin;
    if (index == 1) return &filter->source.pin;
    return NULL;
}

static void testfilter_destroy(struct strmbase_filter *iface)
{
    struct testfilter *filter = impl_from_filter(iface);
    strmbase_sink_cleanup(&filter->sink);
    strmbase_source_cleanup(&filter->source);
    strmbase_filter_cleanup(iface);
}

static const struct strmbase_filter_ops testfilter_ops =
{
    .filter_get_pin = testfilter_get_pin,
    .filter_destroy = testfilter_destroy,
};

static HRESULT WINAPI decide_allocator(struct strmbase_source *source, IMemInputPin *peer, IMemAllocator **allocator)
{
    *allocator = NULL;
    return S_OK;
}

static const struct strmbase_source_ops testsource_ops =
{
    .pfnAttemptConnection = BaseOutputPinImpl_AttemptConnection,
    .pfnDecideAllocator = decide_allocator,
};

static HRESULT testsink_query_interface(struct strmbase_pin *pin, REFIID iid, void **out)
{
    struct testfilter *filter = impl_from_filter(pin->filter);
    if (!IsEqualGUID(iid, &IID_IMemInputPin)) return E_NOINTERFACE;
    *out = &filter->sink.IMemInputPin_iface;
    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT WINAPI testsink_receive(struct strmbase_sink *pin, IMediaSample *sample)
{
    struct testfilter *filter = impl_from_filter(pin->pin.filter);
    const VIDEOINFOHEADER *format = (VIDEOINFOHEADER *)pin->pin.mt.pbFormat;
    unsigned int type, y, x, stride = format->bmiHeader.biSizeImage / 2;
    unsigned int bytes = format->bmiHeader.biBitCount / 8;
    REFERENCE_TIME start, end;
    BYTE *data, expected[4] = {0};
    HRESULT hr;

    ++filter->received;
    for (type = 0; type < ARRAY_SIZE(subtypes); ++type)
        if (IsEqualGUID(&pin->pin.mt.subtype, subtypes[type])) break;
    ok(type < ARRAY_SIZE(subtypes), "Unexpected subtype.\n");
    if (type == ARRAY_SIZE(subtypes)) return E_FAIL;
    hr = IMediaSample_GetPointer(sample, &data);
    ok(hr == S_OK, "GetPointer returned %#lx.\n", hr);
    if (FAILED(hr)) return hr;
    ok(IMediaSample_GetActualDataLength(sample) == stride * 2, "Unexpected output size.\n");
    for (y = 0; y < 2; ++y)
        for (x = 0; x < 3; ++x)
        {
            unsigned int row = format->bmiHeader.biHeight > 0 ? 1 - y : y;
            encode_pixel(expected, type, colors[y * 3 + x]);
            ok(!memcmp(data + row * stride + x * bytes, expected, bytes == 4 ? 3 : bytes),
                    "Wrong pixel at %u,%u, type %u, height %ld.\n", x, y, type, format->bmiHeader.biHeight);
        }
    hr = IMediaSample_GetTime(sample, &start, &end);
    ok(hr == S_OK && start == 1000000 && end == 1400000, "Lost timestamps, hr %#lx.\n", hr);
    hr = IMediaSample_GetMediaTime(sample, &start, &end);
    ok(hr == S_OK && start == 12 && end == 13, "Lost media time, hr %#lx.\n", hr);
    ok(IMediaSample_IsSyncPoint(sample) == S_OK, "Missing sync point.\n");
    ok(IMediaSample_IsDiscontinuity(sample) == S_OK, "Missing discontinuity.\n");
    ok(IMediaSample_IsPreroll(sample) == S_OK, "Missing preroll.\n");
    return S_OK;
}

static HRESULT testsink_eos(struct strmbase_sink *pin)
{
    ++impl_from_filter(pin->pin.filter)->eos;
    return S_OK;
}

static HRESULT testsink_segment(struct strmbase_sink *pin, REFERENCE_TIME start, REFERENCE_TIME stop, double rate)
{
    ++impl_from_filter(pin->pin.filter)->segments;
    ok(start == 100 && stop == 200 && rate == 1.5, "Wrong segment.\n");
    return S_OK;
}

static const struct strmbase_sink_ops testsink_ops =
{
    .base.pin_query_interface = testsink_query_interface,
    .pfnReceive = testsink_receive,
    .sink_eos = testsink_eos,
    .sink_new_segment = testsink_segment,
};

static void testfilter_init(struct testfilter *filter)
{
    memset(filter, 0, sizeof(*filter));
    strmbase_filter_init(&filter->filter, NULL, &GUID_NULL, &testfilter_ops);
    strmbase_sink_init(&filter->sink, &filter->filter, L"Input", &testsink_ops, NULL);
    strmbase_source_init(&filter->source, &filter->filter, L"Output", &testsource_ops);
}

static void test_conversion(unsigned int input_type, unsigned int output_type, LONG input_height, LONG output_height)
{
    struct testfilter upstream, downstream;
    struct video_format input_format, output_format;
    AM_MEDIA_TYPE input_mt, output_mt, bad_mt, *enumerated;
    ALLOCATOR_PROPERTIES props = {1, 24, 1, 0}, actual;
    IMemAllocator *allocator = NULL;
    IMediaSample *sample = NULL;
    IBaseFilter *filter = NULL;
    IMemInputPin *meminput = NULL;
    IEnumMediaTypes *types = NULL;
    IPin *input = NULL, *output = NULL;
    IUnknown *iface;
    REFERENCE_TIME start, end;
    unsigned int y, x, stride, bytes;
    BYTE *data;
    CLSID clsid;
    HRESULT hr;

    testfilter_init(&upstream);
    testfilter_init(&downstream);
    init_format(&input_mt, &input_format, input_type, input_height);
    init_format(&output_mt, &output_format, output_type, output_height);
    hr = CoCreateInstance(&CLSID_Colour, NULL, CLSCTX_INPROC_SERVER, &IID_IBaseFilter, (void **)&filter);
    ok(hr == S_OK, "CLSID_Colour creation failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IBaseFilter_GetClassID(filter, &clsid);
    ok(hr == S_OK && IsEqualGUID(&clsid, &CLSID_Colour), "Wrong class identity.\n");
    hr = IBaseFilter_FindPin(filter, L"Input", &input);
    ok(hr == S_OK, "Find input failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IBaseFilter_FindPin(filter, L"Output", &output);
    ok(hr == S_OK, "Find output failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IPin_QueryInterface(input, &IID_IMemInputPin, (void **)&meminput);
    ok(hr == S_OK, "Missing IMemInputPin, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IPin_QueryInterface(output, &IID_IMediaSeeking, (void **)&iface);
    ok(hr == S_OK, "Missing seeking interface, hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IUnknown_Release(iface);
    hr = IPin_QueryInterface(output, &IID_IMediaPosition, (void **)&iface);
    ok(hr == S_OK, "Missing position interface, hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IUnknown_Release(iface);
    hr = IPin_QueryInterface(input, &IID_IQualityControl, (void **)&iface);
    ok(hr == S_OK, "Missing quality control, hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IUnknown_Release(iface);

    bad_mt = input_mt;
    bad_mt.majortype = MEDIATYPE_Audio;
    ok(IPin_QueryAccept(input, &bad_mt) == S_FALSE, "Accepted audio.\n");
    bad_mt = input_mt;
    bad_mt.subtype = MEDIASUBTYPE_YUY2;
    ok(IPin_QueryAccept(input, &bad_mt) == S_FALSE, "Accepted YUV.\n");
    bad_mt = input_mt;
    bad_mt.cbFormat = sizeof(VIDEOINFOHEADER) - 1;
    ok(IPin_QueryAccept(input, &bad_mt) == S_FALSE, "Accepted truncated format.\n");

    hr = IPin_Connect(&upstream.source.pin.IPin_iface, input, &input_mt);
    ok(hr == S_OK, "Input connection failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IPin_EnumMediaTypes(output, &types);
    ok(hr == S_OK, "EnumMediaTypes failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        while (IEnumMediaTypes_Next(types, 1, &enumerated, NULL) == S_OK)
        {
            ok(IPin_QueryAccept(output, enumerated) == S_OK, "Rejected enumerated output.\n");
            DeleteMediaType(enumerated);
        }
        IEnumMediaTypes_Release(types);
    }
    output_format.video.bmiHeader.biWidth = 4;
    ok(IPin_QueryAccept(output, &output_mt) == S_FALSE, "Accepted resizing.\n");
    output_format.video.bmiHeader.biWidth = 3;
    hr = IPin_Connect(output, &downstream.sink.pin.IPin_iface, &output_mt);
    ok(hr == S_OK, "Output connection failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = CoCreateInstance(&CLSID_MemoryAllocator, NULL, CLSCTX_INPROC_SERVER, &IID_IMemAllocator, (void **)&allocator);
    ok(hr == S_OK, "Allocator creation failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMemAllocator_SetProperties(allocator, &props, &actual);
    ok(hr == S_OK, "SetProperties failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMemAllocator_Commit(allocator);
    ok(hr == S_OK, "Commit failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMemAllocator_GetBuffer(allocator, &sample, NULL, NULL, 0);
    ok(hr == S_OK, "GetBuffer failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMediaSample_GetPointer(sample, &data);
    if (FAILED(hr)) goto done;
    memset(data, 0xab, props.cbBuffer);
    stride = input_format.video.bmiHeader.biSizeImage / 2;
    bytes = input_format.video.bmiHeader.biBitCount / 8;
    for (y = 0; y < 2; ++y)
        for (x = 0; x < 3; ++x)
            encode_pixel(data + (input_height > 0 ? 1 - y : y) * stride + x * bytes, input_type, colors[y * 3 + x]);
    IMediaSample_SetActualDataLength(sample, stride * 2);
    start = 1000000;
    end = 1400000;
    IMediaSample_SetTime(sample, &start, &end);
    start = 12;
    end = 13;
    IMediaSample_SetMediaTime(sample, &start, &end);
    IMediaSample_SetSyncPoint(sample, TRUE);
    IMediaSample_SetDiscontinuity(sample, TRUE);
    IMediaSample_SetPreroll(sample, TRUE);
    ok(IMemInputPin_Receive(meminput, sample) == VFW_E_WRONG_STATE, "Accepted sample while stopped.\n");
    hr = IBaseFilter_Pause(filter);
    ok(hr == S_OK, "Pause failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMemInputPin_Receive(meminput, sample);
    ok(hr == S_OK && downstream.received == 1, "No converted sample, hr %#lx.\n", hr);
    IMediaSample_SetActualDataLength(sample, 1);
    hr = IMemInputPin_Receive(meminput, sample);
    ok(FAILED(hr) && downstream.received == 1, "Accepted truncated sample, hr %#lx.\n", hr);
    IMediaSample_SetActualDataLength(sample, stride * 2);
    IPin_NewSegment(input, 100, 200, 1.5);
    ok(downstream.segments == 1, "Missing NewSegment.\n");
    IPin_EndOfStream(input);
    ok(downstream.eos == 1, "Missing EndOfStream.\n");
    IPin_BeginFlush(input);
    ok(downstream.sink.flushing, "Missing BeginFlush.\n");
    ok(IMemInputPin_Receive(meminput, sample) == S_FALSE, "Accepted sample while flushing.\n");
    IPin_EndFlush(input);
    ok(!downstream.sink.flushing, "Missing EndFlush.\n");
    hr = IMemInputPin_Receive(meminput, sample);
    ok(hr == S_OK && downstream.received == 2, "Did not resume after flush, hr %#lx.\n", hr);

done:
    if (sample) IMediaSample_Release(sample);
    if (allocator)
    {
        IMemAllocator_Decommit(allocator);
        IMemAllocator_Release(allocator);
    }
    if (filter) IBaseFilter_Stop(filter);
    if (input) IPin_Disconnect(input);
    if (output) IPin_Disconnect(output);
    IPin_Disconnect(&upstream.source.pin.IPin_iface);
    IPin_Disconnect(&downstream.sink.pin.IPin_iface);
    if (meminput) IMemInputPin_Release(meminput);
    if (input) IPin_Release(input);
    if (output) IPin_Release(output);
    if (filter) ok(!IBaseFilter_Release(filter), "Filter leaked references.\n");
    ok(!IBaseFilter_Release(&upstream.filter.IBaseFilter_iface), "Upstream leaked.\n");
    ok(!IBaseFilter_Release(&downstream.filter.IBaseFilter_iface), "Downstream leaked.\n");
}

START_TEST(colour)
{
    unsigned int input, output, orientation;

    CoInitialize(NULL);
    for (input = 0; input < ARRAY_SIZE(subtypes); ++input)
        for (output = 0; output < ARRAY_SIZE(subtypes); ++output)
            for (orientation = 0; orientation < 4; ++orientation)
                test_conversion(input, output, (orientation & 1) ? -2 : 2, (orientation & 2) ? -2 : 2);
    CoUninitialize();
}
