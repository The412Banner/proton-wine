/* Copyright 2022 Rémi Bernon for CodeWeavers
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winedmo_private.h"

#include "mfapi.h"
#include "mferror.h"
#include "mfobjects.h"
#include "mftransform.h"
#include "wmcodecdsp.h"

#include "wine/debug.h"
#include "wine/winedmo.h"

#include "initguid.h"
#include "d3d11.h"
#include "d3d11_4.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

extern GUID MFVideoFormat_ABGR32;

static const GUID MF_XVP_PLAYBACK_MODE = { 0x3c5d293f, 0xad67, 0x4e29, { 0xaf, 0x12, 0xcf, 0x3e, 0x23, 0x8a, 0xcc, 0xe9 } };

static const GUID *const input_types[] =
{
    &MFVideoFormat_IYUV,
    &MFVideoFormat_YV12,
    &MFVideoFormat_NV12,
    &MFVideoFormat_420O,
    &MFVideoFormat_UYVY,
    &MFVideoFormat_YUY2,
    &MEDIASUBTYPE_P208,
    &MFVideoFormat_NV11,
    &MFVideoFormat_AYUV,
    &MFVideoFormat_ARGB32,
    &MFVideoFormat_RGB32,
    &MFVideoFormat_RGB24,
    &MFVideoFormat_I420,
    &MFVideoFormat_YVYU,
    &MFVideoFormat_RGB555,
    &MFVideoFormat_RGB565,
    &MFVideoFormat_RGB8,
    &MFVideoFormat_Y216,
    &MFVideoFormat_v410,
    &MFVideoFormat_Y41P,
    &MFVideoFormat_Y41T,
    &MFVideoFormat_Y42T,
};
static const GUID *const output_types[] =
{
    &MFVideoFormat_YUY2,
    &MFVideoFormat_IYUV,
    &MFVideoFormat_I420,
    &MFVideoFormat_NV12,
    &MFVideoFormat_RGB24,
    &MFVideoFormat_ARGB32,
    &MFVideoFormat_RGB32,
    &MFVideoFormat_YV12,
    &MFVideoFormat_AYUV,
    &MFVideoFormat_RGB555,
    &MFVideoFormat_RGB565,
    &MFVideoFormat_ABGR32,
};

struct video_processor
{
    IMFTransform IMFTransform_iface;
    LONG refcount;

    IMFAttributes *attributes;
    IMFAttributes *output_attributes;

    IMFMediaType *input_type;
    MFT_INPUT_STREAM_INFO input_info;
    IMFMediaType *output_type;
    MFT_OUTPUT_STREAM_INFO output_info;
    BOOL flip_output;
    UINT32 flip_output_height;
    UINT32 flip_output_row_size;

    IMFSample *input_sample;
    struct winedmo_transform winedmo_transform;
    BYTE *cpu_input_buffer;
    DWORD cpu_input_buffer_size;
    BYTE *cpu_output_buffer;
    DWORD cpu_output_buffer_size;

    IUnknown *device_manager;
    IMFVideoSampleAllocatorEx *allocator;
    ID3D11Device *d3d11_device;
    ID3D11VideoDevice *d3d11_video_device;
    ID3D11VideoProcessorEnumerator *d3d11_enumerator;
    ID3D11VideoProcessor *d3d11_processor;
    GUID d3d11_input_subtype;
    GUID d3d11_output_subtype;
    UINT64 d3d11_input_size;
    UINT64 d3d11_output_size;
    UINT64 d3d11_input_rate;
    UINT64 d3d11_output_rate;

    IMFVideoProcessorControl IMFVideoProcessorControl_iface;
};

static void update_video_aperture(MFVideoInfo *input_info, MFVideoInfo *output_info)
{
    RECT input_rect, output_rect;

    get_mf_video_content_rect(input_info, &input_rect);
    get_mf_video_content_rect(output_info, &output_rect);

    if (!EqualRect(&input_rect, &output_rect))
    {
        FIXME("Mismatched content size %s vs %s\n", wine_dbgstr_rect(&input_rect),
                wine_dbgstr_rect(&output_rect));
    }

    input_info->MinimumDisplayAperture.OffsetX.value = input_rect.left;
    input_info->MinimumDisplayAperture.OffsetY.value = input_rect.top;
    input_info->MinimumDisplayAperture.Area.cx = input_rect.right - input_rect.left;
    input_info->MinimumDisplayAperture.Area.cy = input_rect.bottom - input_rect.top;
    output_info->MinimumDisplayAperture = input_info->MinimumDisplayAperture;
}

static HRESULT normalize_media_types(BOOL bottom_up, IMFMediaType **input_type, IMFMediaType **output_type)
{
    MFVIDEOFORMAT *input_format, *output_format;
    BOOL normalize_input, normalize_output;
    UINT32 size;
    HRESULT hr;

    normalize_input = FAILED(IMFMediaType_GetItem(*input_type, &MF_MT_DEFAULT_STRIDE, NULL));
    normalize_output = FAILED(IMFMediaType_GetItem(*output_type, &MF_MT_DEFAULT_STRIDE, NULL));

    if (FAILED(hr = MFCreateMFVideoFormatFromMFMediaType(*input_type, &input_format, &size)))
        return hr;
    if (FAILED(hr = MFCreateMFVideoFormatFromMFMediaType(*output_type, &output_format, &size)))
    {
        CoTaskMemFree(input_format);
        return hr;
    }

    if (bottom_up && normalize_input)
        input_format->videoInfo.VideoFlags |= MFVideoFlag_BottomUpLinearRep;
    if (bottom_up && normalize_output)
        output_format->videoInfo.VideoFlags |= MFVideoFlag_BottomUpLinearRep;

    update_video_aperture(&input_format->videoInfo, &output_format->videoInfo);

    if (FAILED(hr = MFCreateVideoMediaType(input_format, (IMFVideoMediaType **)input_type)))
        goto done;
    if (FAILED(hr = MFCreateVideoMediaType(output_format, (IMFVideoMediaType **)output_type)))
    {
        IMFMediaType_Release(*input_type);
        *input_type = NULL;
    }

done:
    CoTaskMemFree(input_format);
    CoTaskMemFree(output_format);
    return hr;
}

static void update_rgb32_output_flip(struct video_processor *impl, IMFMediaType *type)
{
    UINT32 stride_value, width, height, row_size;
    UINT64 frame_size, needed_size;
    INT32 stride;
    GUID subtype;

    impl->flip_output = FALSE;

    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype))
            || !IsEqualGUID(&subtype, &MFVideoFormat_RGB32)
            || FAILED(IMFMediaType_GetUINT32(type, &MF_MT_DEFAULT_STRIDE, &stride_value))
            || FAILED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &frame_size)))
        return;

    stride = (INT32)stride_value;
    if (stride >= 0)
        return;

    width = frame_size >> 32;
    height = (UINT32)frame_size;
    row_size = -stride;
    needed_size = (UINT64)row_size * height;

    if (row_size < width * 4 || needed_size > UINT32_MAX)
        return;

    impl->flip_output = TRUE;
    impl->flip_output_height = height;
    impl->flip_output_row_size = row_size;
}

static void flip_rgb32_output(struct video_processor *impl, BYTE *data, UINT32 data_size)
{
    UINT32 y, row_size = impl->flip_output_row_size, height = impl->flip_output_height;
    BYTE *tmp;

    if (!impl->flip_output || (UINT64)row_size * height > data_size)
        return;

    if (!(tmp = malloc(row_size)))
        return;

    for (y = 0; y < height / 2; ++y)
    {
        BYTE *top = data + y * row_size;
        BYTE *bottom = data + (height - y - 1) * row_size;

        memcpy(tmp, top, row_size);
        memcpy(top, bottom, row_size);
        memcpy(bottom, tmp, row_size);
    }

    free(tmp);
}

static HRESULT try_create_winedmo_transform(struct video_processor *impl)
{
    BOOL bottom_up = !impl->device_manager; /* when not D3D-enabled, the transform outputs bottom up RGB buffers */
    IMFMediaType *input_type = impl->input_type, *output_type = impl->output_type;
    MFVIDEOFORMAT *input_format, *output_format;
    UINT32 input_size, output_size;
    NTSTATUS status;
    HRESULT hr;

    if (impl->winedmo_transform.handle)
    {
        winedmo_transform_destroy(impl->winedmo_transform);
        impl->winedmo_transform.handle = 0;
    }
    impl->flip_output = FALSE;

    if (FAILED(hr = normalize_media_types(bottom_up, &input_type, &output_type)))
        return hr;

    if (FAILED(hr = MFCreateMFVideoFormatFromMFMediaType(input_type, &input_format, &input_size)))
    {
        IMFMediaType_Release(input_type);
        IMFMediaType_Release(output_type);
        return hr;
    }
    if (FAILED(hr = MFCreateMFVideoFormatFromMFMediaType(output_type, &output_format, &output_size)))
    {
        CoTaskMemFree(input_format);
        IMFMediaType_Release(input_type);
        IMFMediaType_Release(output_type);
        return hr;
    }
    update_rgb32_output_flip(impl, output_type);
    IMFMediaType_Release(input_type);
    IMFMediaType_Release(output_type);

    status = winedmo_transform_create(MFMediaType_Video,
            (const union winedmo_format *)input_format, input_size,
            (const union winedmo_format *)output_format, output_size,
            &impl->winedmo_transform);
    CoTaskMemFree(output_format);
    CoTaskMemFree(input_format);
    return status ? E_FAIL : S_OK;
}

static HRESULT video_processor_uninit_allocator(struct video_processor *processor)
{
    HRESULT hr;

    if (!processor->allocator)
        return S_OK;

    if (SUCCEEDED(hr = IMFVideoSampleAllocatorEx_UninitializeSampleAllocator(processor->allocator)))
        hr = IMFVideoSampleAllocatorEx_SetDirectXManager(processor->allocator, NULL);
    IMFVideoSampleAllocatorEx_Release(processor->allocator);
    processor->allocator = NULL;

    return hr;
}

static HRESULT video_processor_get_cpu_output_buffer(struct video_processor *processor, BYTE **data, UINT32 *size)
{
    BYTE *buffer;

    if (!processor->output_info.cbSize)
        return E_FAIL;

    if (processor->cpu_output_buffer_size < processor->output_info.cbSize)
    {
        if (!(buffer = realloc(processor->cpu_output_buffer, processor->output_info.cbSize)))
            return E_OUTOFMEMORY;
        processor->cpu_output_buffer = buffer;
        processor->cpu_output_buffer_size = processor->output_info.cbSize;
    }

    *data = processor->cpu_output_buffer;
    *size = processor->output_info.cbSize;
    return S_OK;
}

static HRESULT video_processor_get_cpu_input_buffer(struct video_processor *processor, BYTE **data, DWORD *size)
{
    BYTE *buffer;

    if (!processor->input_info.cbSize)
        return E_FAIL;

    if (processor->cpu_input_buffer_size < processor->input_info.cbSize)
    {
        if (!(buffer = realloc(processor->cpu_input_buffer, processor->input_info.cbSize)))
            return E_OUTOFMEMORY;
        processor->cpu_input_buffer = buffer;
        processor->cpu_input_buffer_size = processor->input_info.cbSize;
    }

    *data = processor->cpu_input_buffer;
    *size = processor->input_info.cbSize;
    return S_OK;
}

static HRESULT video_processor_init_allocator(struct video_processor *processor)
{
    IMFVideoSampleAllocatorEx *allocator;
    UINT32 count;
    HRESULT hr;

    if (processor->allocator)
        return S_OK;
    if (!processor->device_manager)
        return E_FAIL;

    if (FAILED(hr = MFCreateVideoSampleAllocatorEx(&IID_IMFVideoSampleAllocatorEx, (void **)&allocator)))
        return hr;
    if (FAILED(IMFAttributes_GetUINT32(processor->attributes, &MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT, &count)))
        count = 2;
    if (FAILED(hr = IMFVideoSampleAllocatorEx_SetDirectXManager(allocator, processor->device_manager))
            || FAILED(hr = IMFVideoSampleAllocatorEx_InitializeSampleAllocatorEx(allocator, count, max(count + 2, 10),
            processor->output_attributes, processor->output_type)))
    {
        IMFVideoSampleAllocatorEx_Release(allocator);
        return hr;
    }

    processor->allocator = allocator;
    return S_OK;
}

static HRESULT video_processor_get_d3d11_resource(IMFSample *sample, ID3D11Resource **resource)
{
    IMFMediaBuffer *buffer;
    DWORD count;
    HRESULT hr;

    if (FAILED(IMFSample_GetBufferCount(sample, &count)) || count > 1)
        return E_FAIL;

    if (SUCCEEDED(hr = IMFSample_GetBufferByIndex(sample, 0, &buffer)))
    {
        IMFDXGIBuffer *dxgi_buffer;

        if (SUCCEEDED(hr = IMFMediaBuffer_QueryInterface(buffer, &IID_IMFDXGIBuffer, (void **)&dxgi_buffer)))
        {
            hr = IMFDXGIBuffer_GetResource(dxgi_buffer, &IID_ID3D11Resource, (void **)resource);
            IMFDXGIBuffer_Release(dxgi_buffer);
        }

        IMFMediaBuffer_Release(buffer);
    }

    return hr;
}

static void enable_d3d11_multithread_protection(ID3D11DeviceContext *context)
{
    static const GUID d3d11_multithread_iid =
            { 0x9b7e4e00, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };
    ID3D11Multithread *multithread;

    if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(context, &d3d11_multithread_iid, (void **)&multithread)))
    {
        ID3D11Multithread_SetMultithreadProtected(multithread, TRUE);
        ID3D11Multithread_Release(multithread);
    }
}

static HRESULT get_d3d11_video_context(ID3D11Resource *resource, ID3D11VideoContext **video_context)
{
    ID3D11DeviceContext *context;
    ID3D11Device *device;
    HRESULT hr;

    ID3D11Resource_GetDevice(resource, &device);
    ID3D11Device_GetImmediateContext(device, &context);
    enable_d3d11_multithread_protection(context);
    hr = ID3D11DeviceContext_QueryInterface(context, &IID_ID3D11VideoContext, (void **)video_context);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    return hr;
}

static void video_processor_clear_d3d11_cache(struct video_processor *processor)
{
    if (processor->d3d11_processor)
    {
        ID3D11VideoProcessor_Release(processor->d3d11_processor);
        processor->d3d11_processor = NULL;
    }
    if (processor->d3d11_enumerator)
    {
        ID3D11VideoProcessorEnumerator_Release(processor->d3d11_enumerator);
        processor->d3d11_enumerator = NULL;
    }
    if (processor->d3d11_video_device)
    {
        ID3D11VideoDevice_Release(processor->d3d11_video_device);
        processor->d3d11_video_device = NULL;
    }
    if (processor->d3d11_device)
    {
        ID3D11Device_Release(processor->d3d11_device);
        processor->d3d11_device = NULL;
    }
}

struct resource_desc
{
    GUID subtype;
    UINT64 frame_size;
    UINT64 frame_rate;
};

static void set_d3d11_video_rate(DXGI_RATIONAL *rate, UINT64 media_type_rate)
{
    rate->Numerator = media_type_rate >> 32;
    rate->Denominator = (UINT32)media_type_rate;

    if (!rate->Numerator || !rate->Denominator)
    {
        rate->Numerator = 1;
        rate->Denominator = 1;
    }
}

static HRESULT create_video_processor_enumerator(ID3D11Device *device, const struct resource_desc *input_desc,
        const struct resource_desc *output_desc, ID3D11VideoDevice **video_device,
        ID3D11VideoProcessorEnumerator **enumerator)
{
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC enum_desc = {0};
    HRESULT hr;

    enum_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    set_d3d11_video_rate(&enum_desc.InputFrameRate, input_desc->frame_rate);
    enum_desc.InputWidth = input_desc->frame_size >> 32;
    enum_desc.InputHeight = (UINT32)input_desc->frame_size;
    set_d3d11_video_rate(&enum_desc.OutputFrameRate,
            output_desc->frame_rate ? output_desc->frame_rate : input_desc->frame_rate);
    enum_desc.OutputWidth = output_desc->frame_size >> 32;
    enum_desc.OutputHeight = (UINT32)output_desc->frame_size;
    enum_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(hr = ID3D11Device_QueryInterface(device, &IID_ID3D11VideoDevice, (void **)video_device)))
        return hr;
    if (FAILED(hr = ID3D11VideoDevice_CreateVideoProcessorEnumerator(*video_device, &enum_desc, enumerator)))
    {
        ID3D11VideoDevice_Release(*video_device);
        *video_device = NULL;
    }

    return hr;
}

static HRESULT video_processor_get_cached_d3d11_processor(struct video_processor *processor_impl,
        const struct resource_desc *input_desc, ID3D11Resource *input,
        const struct resource_desc *output_desc, ID3D11VideoProcessor **processor,
        ID3D11VideoDevice **device, ID3D11VideoProcessorEnumerator **enumerator)
{
    ID3D11Device *d3d11_device;
    HRESULT hr;

    ID3D11Resource_GetDevice(input, &d3d11_device);
    if (processor_impl->d3d11_processor && processor_impl->d3d11_device == d3d11_device
            && processor_impl->d3d11_input_size == input_desc->frame_size
            && processor_impl->d3d11_output_size == output_desc->frame_size
            && processor_impl->d3d11_input_rate == input_desc->frame_rate
            && processor_impl->d3d11_output_rate == output_desc->frame_rate
            && IsEqualGUID(&processor_impl->d3d11_input_subtype, &input_desc->subtype)
            && IsEqualGUID(&processor_impl->d3d11_output_subtype, &output_desc->subtype))
    {
        *device = processor_impl->d3d11_video_device;
        *enumerator = processor_impl->d3d11_enumerator;
        *processor = processor_impl->d3d11_processor;
        ID3D11VideoDevice_AddRef(*device);
        ID3D11VideoProcessorEnumerator_AddRef(*enumerator);
        ID3D11VideoProcessor_AddRef(*processor);
        ID3D11Device_Release(d3d11_device);
        return S_OK;
    }

    video_processor_clear_d3d11_cache(processor_impl);
    processor_impl->d3d11_device = d3d11_device;
    ID3D11Device_AddRef(processor_impl->d3d11_device);

    if (FAILED(hr = create_video_processor_enumerator(d3d11_device, input_desc, output_desc,
            &processor_impl->d3d11_video_device, &processor_impl->d3d11_enumerator)))
        goto failed;
    if (FAILED(hr = ID3D11VideoDevice_CreateVideoProcessor(processor_impl->d3d11_video_device,
            processor_impl->d3d11_enumerator, 0, &processor_impl->d3d11_processor)))
        goto failed;

    processor_impl->d3d11_input_subtype = input_desc->subtype;
    processor_impl->d3d11_output_subtype = output_desc->subtype;
    processor_impl->d3d11_input_size = input_desc->frame_size;
    processor_impl->d3d11_output_size = output_desc->frame_size;
    processor_impl->d3d11_input_rate = input_desc->frame_rate;
    processor_impl->d3d11_output_rate = output_desc->frame_rate;

    *device = processor_impl->d3d11_video_device;
    *enumerator = processor_impl->d3d11_enumerator;
    *processor = processor_impl->d3d11_processor;
    ID3D11VideoDevice_AddRef(*device);
    ID3D11VideoProcessorEnumerator_AddRef(*enumerator);
    ID3D11VideoProcessor_AddRef(*processor);
    ID3D11Device_Release(d3d11_device);
    return S_OK;

failed:
    ID3D11Device_Release(d3d11_device);
    video_processor_clear_d3d11_cache(processor_impl);
    return hr;
}

static HRESULT init_d3d11_video_processor(struct video_processor *processor_impl,
        const struct resource_desc *input_desc, ID3D11Resource *input,
        const struct resource_desc *output_desc, ID3D11Resource *output, ID3D11VideoProcessor **processor,
        ID3D11VideoProcessorInputView **input_view, ID3D11VideoProcessorOutputView **output_view)
{
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc = {0};
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc = {0};
    ID3D11VideoProcessorEnumerator *enumerator;
    ID3D11VideoDevice *device;
    HRESULT hr;

    *processor = NULL;
    *input_view = NULL;
    *output_view = NULL;

    input_view_desc.FourCC = input_desc->subtype.Data1;
    input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_view_desc.Texture2D.MipSlice = 0;
    input_view_desc.Texture2D.ArraySlice = 0;

    output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_view_desc.Texture2D.MipSlice = 0;

    if (FAILED(hr = video_processor_get_cached_d3d11_processor(processor_impl, input_desc, input,
            output_desc, processor, &device, &enumerator)))
        return hr;

    if (FAILED(hr = ID3D11VideoDevice_CreateVideoProcessorInputView(device, input, enumerator,
            &input_view_desc, input_view)))
        goto failed;
    if (FAILED(hr = ID3D11VideoDevice_CreateVideoProcessorOutputView(device, output, enumerator,
            &output_view_desc, output_view)))
    {
        ID3D11VideoProcessorInputView_Release(*input_view);
        *input_view = NULL;
        goto failed;
    }

failed:
    if (FAILED(hr) && *processor)
    {
        ID3D11VideoProcessor_Release(*processor);
        *processor = NULL;
    }
    ID3D11VideoProcessorEnumerator_Release(enumerator);
    ID3D11VideoDevice_Release(device);
    return hr;
}

static HRESULT video_processor_process_output_d3d11(struct video_processor *processor,
        IMFSample *input_sample, IMFSample *output_sample)
{
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE color_space;
    D3D11_VIDEO_PROCESSOR_STREAM streams = {0};
    struct resource_desc input_desc, output_desc;
    ID3D11VideoProcessorOutputView *output_view;
    ID3D11VideoProcessorInputView *input_view;
    ID3D11VideoProcessor *video_processor;
    ID3D11VideoContext *video_context;
    ID3D11Resource *input, *output;
    MFVideoArea aperture;
    RECT rect = {0};
    LONGLONG time;
    UINT32 value;
    DWORD flags;
    HRESULT hr;

    if (FAILED(hr = IMFMediaType_GetUINT64(processor->input_type, &MF_MT_FRAME_SIZE, &input_desc.frame_size))
            || FAILED(hr = IMFMediaType_GetGUID(processor->input_type, &MF_MT_SUBTYPE, &input_desc.subtype))
            || FAILED(hr = IMFMediaType_GetUINT64(processor->output_type, &MF_MT_FRAME_SIZE, &output_desc.frame_size))
            || FAILED(hr = IMFMediaType_GetGUID(processor->output_type, &MF_MT_SUBTYPE, &output_desc.subtype)))
        return hr;
    if (FAILED(IMFMediaType_GetUINT64(processor->input_type, &MF_MT_FRAME_RATE, &input_desc.frame_rate)))
        input_desc.frame_rate = ((UINT64)1 << 32) | 1;
    if (FAILED(IMFMediaType_GetUINT64(processor->output_type, &MF_MT_FRAME_RATE, &output_desc.frame_rate)))
        output_desc.frame_rate = input_desc.frame_rate;

    if (FAILED(hr = video_processor_get_d3d11_resource(input_sample, &input)))
        return hr;
    if (FAILED(hr = video_processor_get_d3d11_resource(output_sample, &output)))
    {
        ID3D11Resource_Release(input);
        return hr;
    }

    if (FAILED(hr = get_d3d11_video_context(input, &video_context)))
        goto failed;
    if (FAILED(hr = init_d3d11_video_processor(processor, &input_desc, input, &output_desc, output,
            &video_processor, &input_view, &output_view)))
    {
        ID3D11VideoContext_Release(video_context);
        goto failed;
    }

    streams.Enable = TRUE;
    streams.OutputIndex = 0;
    streams.InputFrameOrField = 0;
    streams.PastFrames = 0;
    streams.FutureFrames = 0;
    streams.pInputSurface = input_view;

    if (SUCCEEDED(IMFMediaType_GetBlob(processor->input_type, &MF_MT_MINIMUM_DISPLAY_APERTURE, (BYTE *)&aperture, sizeof(aperture), NULL)))
        SetRect(&rect, aperture.OffsetX.value, aperture.OffsetY.value, aperture.OffsetX.value + aperture.Area.cx,
                aperture.OffsetY.value + aperture.Area.cy);
    else
        SetRect(&rect, 0, 0, input_desc.frame_size >> 32, (UINT32)input_desc.frame_size);
    ID3D11VideoContext_VideoProcessorSetStreamSourceRect(video_context, video_processor, 0, TRUE, &rect);

    if (SUCCEEDED(IMFMediaType_GetBlob(processor->output_type, &MF_MT_MINIMUM_DISPLAY_APERTURE, (BYTE *)&aperture, sizeof(aperture), NULL)))
        SetRect(&rect, aperture.OffsetX.value, aperture.OffsetY.value, aperture.OffsetX.value + aperture.Area.cx,
                aperture.OffsetY.value + aperture.Area.cy);
    else
        SetRect(&rect, 0, 0, output_desc.frame_size >> 32, (UINT32)output_desc.frame_size);
    ID3D11VideoContext_VideoProcessorSetStreamDestRect(video_context, video_processor, 0, TRUE, &rect);

    memset(&color_space, 0, sizeof(color_space));
    if (SUCCEEDED(IMFMediaType_GetUINT32(processor->input_type, &MF_MT_VIDEO_NOMINAL_RANGE, &value)))
        color_space.Nominal_Range = value == MFNominalRange_Wide ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235
                                                                 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    ID3D11VideoContext_VideoProcessorSetStreamColorSpace(video_context, video_processor, 0, &color_space);
    ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(video_context, video_processor, 0,
            D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(video_context, video_processor, 0, FALSE);

    memset(&color_space, 0, sizeof(color_space));
    if (SUCCEEDED(IMFMediaType_GetUINT32(processor->output_type, &MF_MT_VIDEO_NOMINAL_RANGE, &value)))
        color_space.Nominal_Range = value == MFNominalRange_Wide ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235
                                                                 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    ID3D11VideoContext_VideoProcessorSetOutputColorSpace(video_context, video_processor, &color_space);

    ID3D11VideoContext_VideoProcessorBlt(video_context, video_processor, output_view, 0, 1, &streams);

    IMFSample_CopyAllItems(input_sample, (IMFAttributes *)output_sample);
    if (SUCCEEDED(IMFSample_GetSampleDuration(input_sample, &time)))
        IMFSample_SetSampleDuration(output_sample, time);
    if (SUCCEEDED(IMFSample_GetSampleTime(input_sample, &time)))
        IMFSample_SetSampleTime(output_sample, time);
    if (SUCCEEDED(IMFSample_GetSampleFlags(input_sample, &flags)))
        IMFSample_SetSampleFlags(output_sample, flags);

    ID3D11VideoProcessorOutputView_Release(output_view);
    ID3D11VideoProcessorInputView_Release(input_view);
    ID3D11VideoProcessor_Release(video_processor);
    ID3D11VideoContext_Release(video_context);

failed:
    ID3D11Resource_Release(output);
    ID3D11Resource_Release(input);
    return hr;
}

static BOOL video_processor_can_use_d3d11(IMFSample *input_sample, IMFSample *output_sample)
{
    ID3D11Resource *input, *output;

    if (FAILED(video_processor_get_d3d11_resource(input_sample, &input)))
        return FALSE;
    ID3D11Resource_Release(input);

    if (FAILED(video_processor_get_d3d11_resource(output_sample, &output)))
        return FALSE;
    ID3D11Resource_Release(output);

    return TRUE;
}

static HRESULT video_processor_get_d3d11_texture(IMFSample *sample, ID3D11Texture2D **texture, UINT *subresource)
{
    IMFMediaBuffer *buffer;
    IMFDXGIBuffer *dxgi_buffer;
    HRESULT hr;

    *texture = NULL;
    *subresource = 0;

    if (FAILED(hr = IMFSample_GetBufferByIndex(sample, 0, &buffer)))
        return hr;

    if (SUCCEEDED(hr = IMFMediaBuffer_QueryInterface(buffer, &IID_IMFDXGIBuffer, (void **)&dxgi_buffer)))
    {
        hr = IMFDXGIBuffer_GetResource(dxgi_buffer, &IID_ID3D11Texture2D, (void **)texture);
        if (SUCCEEDED(hr))
        {
            hr = IMFDXGIBuffer_GetSubresourceIndex(dxgi_buffer, subresource);
            if (FAILED(hr))
            {
                ID3D11Texture2D_Release(*texture);
                *texture = NULL;
            }
        }
        IMFDXGIBuffer_Release(dxgi_buffer);
    }

    IMFMediaBuffer_Release(buffer);
    return hr;
}

static HRESULT video_processor_upload_d3d11_output(struct video_processor *processor, IMFSample *output_sample,
        const BYTE *data, DWORD data_size)
{
    ID3D11DeviceContext *context;
    ID3D11Texture2D *texture;
    IMFMediaBuffer *buffer;
    ID3D11Device *device;
    UINT64 frame_size;
    UINT subresource;
    LONG stride;
    GUID subtype;
    HRESULT hr;

    if (FAILED(hr = IMFMediaType_GetGUID(processor->output_type, &MF_MT_SUBTYPE, &subtype))
            || FAILED(hr = IMFMediaType_GetUINT64(processor->output_type, &MF_MT_FRAME_SIZE, &frame_size))
            || FAILED(hr = MFGetStrideForBitmapInfoHeader(subtype.Data1, frame_size >> 32, &stride)))
        return hr;

    if (stride < 0)
        stride = -stride;
    if ((UINT64)stride * (UINT32)frame_size > data_size)
        return E_INVALIDARG;

    if (FAILED(hr = video_processor_get_d3d11_texture(output_sample, &texture, &subresource)))
        return hr;

    ID3D11Texture2D_GetDevice(texture, &device);
    ID3D11Device_GetImmediateContext(device, &context);
    enable_d3d11_multithread_protection(context);
    ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)texture, subresource, NULL, data, stride, 0);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    ID3D11Texture2D_Release(texture);

    if (SUCCEEDED(IMFSample_GetBufferByIndex(output_sample, 0, &buffer)))
    {
        IMFMediaBuffer_SetCurrentLength(buffer, data_size);
        IMFMediaBuffer_Release(buffer);
    }

    return S_OK;
}

static HRESULT video_processor_allocate_cpu_sample(struct video_processor *processor, IMFSample **out)
{
    IMFMediaBuffer *buffer;
    IMFSample *sample;
    UINT64 frame_size;
    UINT32 stride;
    GUID subtype;
    HRESULT hr;

    *out = NULL;

    if ((FAILED(hr = IMFMediaType_GetGUID(processor->output_type, &MF_MT_SUBTYPE, &subtype))
            || FAILED(hr = IMFMediaType_GetUINT64(processor->output_type, &MF_MT_FRAME_SIZE, &frame_size))
            || FAILED(hr = MFCreate2DMediaBuffer(frame_size >> 32, (UINT32)frame_size, subtype.Data1,
                    SUCCEEDED(IMFMediaType_GetUINT32(processor->output_type, &MF_MT_DEFAULT_STRIDE, &stride))
                    && (INT32)stride < 0, &buffer)))
            && FAILED(hr = MFCreateMediaBufferFromMediaType(processor->output_type, 0, processor->output_info.cbSize, 0, &buffer))
            && FAILED(hr = MFCreateMemoryBuffer(processor->output_info.cbSize, &buffer)))
        return hr;

    if (SUCCEEDED(hr = MFCreateSample(&sample)))
    {
        if (SUCCEEDED(hr = IMFSample_AddBuffer(sample, buffer)))
            *out = sample;
        else
            IMFSample_Release(sample);
    }

    IMFMediaBuffer_Release(buffer);
    return hr;
}

static BOOL video_processor_types_match_for_copy(struct video_processor *processor)
{
    UINT32 input_stride, output_stride;
    UINT64 input_size, output_size;
    GUID input_subtype, output_subtype;

    if (FAILED(IMFMediaType_GetGUID(processor->input_type, &MF_MT_SUBTYPE, &input_subtype))
            || FAILED(IMFMediaType_GetGUID(processor->output_type, &MF_MT_SUBTYPE, &output_subtype))
            || !IsEqualGUID(&input_subtype, &output_subtype))
        return FALSE;

    if (FAILED(IMFMediaType_GetUINT64(processor->input_type, &MF_MT_FRAME_SIZE, &input_size))
            || FAILED(IMFMediaType_GetUINT64(processor->output_type, &MF_MT_FRAME_SIZE, &output_size))
            || input_size != output_size)
        return FALSE;

    if (SUCCEEDED(IMFMediaType_GetUINT32(processor->input_type, &MF_MT_DEFAULT_STRIDE, &input_stride))
            && SUCCEEDED(IMFMediaType_GetUINT32(processor->output_type, &MF_MT_DEFAULT_STRIDE, &output_stride))
            && input_stride != output_stride)
        return FALSE;

    return TRUE;
}

static HRESULT video_processor_copy_sample(IMFSample *input_sample, IMFSample *output_sample)
{
    IMFMediaBuffer *input_buffer, *output_buffer;
    IMF2DBuffer2 *input_2d, *output_2d;
    LONGLONG time;
    DWORD flags;
    HRESULT hr;

    if (FAILED(hr = IMFSample_GetBufferByIndex(input_sample, 0, &input_buffer)))
        return hr;
    if (FAILED(hr = IMFSample_GetBufferByIndex(output_sample, 0, &output_buffer)))
    {
        IMFMediaBuffer_Release(input_buffer);
        return hr;
    }

    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(input_buffer, &IID_IMF2DBuffer2, (void **)&input_2d)))
    {
        if (SUCCEEDED(IMFMediaBuffer_QueryInterface(output_buffer, &IID_IMF2DBuffer2, (void **)&output_2d)))
        {
            hr = IMF2DBuffer2_Copy2DTo(input_2d, output_2d);
            IMF2DBuffer2_Release(output_2d);
        }
        else
            hr = E_NOINTERFACE;
        IMF2DBuffer2_Release(input_2d);
    }
    else
        hr = E_NOINTERFACE;

    if (FAILED(hr))
    {
        IMFMediaBuffer *input_contiguous, *output_contiguous;
        BYTE *input_data, *output_data;
        DWORD input_length, output_length;

        if (SUCCEEDED(hr = IMFSample_ConvertToContiguousBuffer(input_sample, &input_contiguous)))
        {
            if (SUCCEEDED(hr = IMFSample_ConvertToContiguousBuffer(output_sample, &output_contiguous)))
            {
                if (SUCCEEDED(hr = IMFMediaBuffer_Lock(input_contiguous, &input_data, NULL, &input_length)))
                {
                    if (SUCCEEDED(hr = IMFMediaBuffer_Lock(output_contiguous, &output_data, &output_length, NULL)))
                    {
                        if (output_length < input_length)
                            hr = E_INVALIDARG;
                        else
                        {
                            memcpy(output_data, input_data, input_length);
                            IMFMediaBuffer_SetCurrentLength(output_contiguous, input_length);
                        }
                        IMFMediaBuffer_Unlock(output_contiguous);
                    }
                    IMFMediaBuffer_Unlock(input_contiguous);
                }
                IMFMediaBuffer_Release(output_contiguous);
            }
            IMFMediaBuffer_Release(input_contiguous);
        }
    }

    if (SUCCEEDED(hr))
    {
        IMFSample_CopyAllItems(input_sample, (IMFAttributes *)output_sample);
        if (SUCCEEDED(IMFSample_GetSampleTime(input_sample, &time)))
            IMFSample_SetSampleTime(output_sample, time);
        if (SUCCEEDED(IMFSample_GetSampleDuration(input_sample, &time)))
            IMFSample_SetSampleDuration(output_sample, time);
        if (SUCCEEDED(IMFSample_GetSampleFlags(input_sample, &flags)))
            IMFSample_SetSampleFlags(output_sample, flags);
    }

    IMFMediaBuffer_Release(output_buffer);
    IMFMediaBuffer_Release(input_buffer);
    return hr;
}

static HRESULT video_processor_process_output_cpu(struct video_processor *processor, IMFSample *input_sample,
        IMFSample *output_sample, MFT_OUTPUT_DATA_BUFFER *sample)
{
    IMFMediaBuffer *buffer;
    ID3D11Resource *resource;
    NTSTATUS nt_status;
    BYTE *data;
    DWORD current_size, max_size;
    UINT32 out_size;
    INT64 in_pts, in_dur, out_pts, out_dur;
    DWORD out_flags;
    HRESULT hr;

    if (video_processor_types_match_for_copy(processor))
        return video_processor_copy_sample(input_sample, output_sample);

    if (SUCCEEDED(hr = IMFSample_GetBufferByIndex(input_sample, 0, &buffer)))
    {
        IMF2DBuffer2 *buffer_2d;

        if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buffer, &IID_IMF2DBuffer2, (void **)&buffer_2d)))
        {
            if (SUCCEEDED(hr = video_processor_get_cpu_input_buffer(processor, &data, &current_size)))
                hr = IMF2DBuffer2_ContiguousCopyTo(buffer_2d, data, current_size);
            IMF2DBuffer2_Release(buffer_2d);
        }
        else
            hr = E_NOINTERFACE;
        IMFMediaBuffer_Release(buffer);
    }
    if (FAILED(hr))
    {
        if (FAILED(hr = IMFSample_ConvertToContiguousBuffer(input_sample, &buffer)))
            return hr;
        if (FAILED(hr = IMFMediaBuffer_Lock(buffer, &data, NULL, &current_size)))
        {
            IMFMediaBuffer_Release(buffer);
            return hr;
        }
        if (FAILED(IMFSample_GetSampleTime(input_sample, &in_pts))) in_pts = INT64_MIN;
        if (FAILED(IMFSample_GetSampleDuration(input_sample, &in_dur))) in_dur = INT64_MIN;
        nt_status = winedmo_transform_push_input(processor->winedmo_transform, data, current_size,
                in_pts, INT64_MIN, in_dur, 0);
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_Release(buffer);
    }
    else
    {
        if (FAILED(IMFSample_GetSampleTime(input_sample, &in_pts))) in_pts = INT64_MIN;
        if (FAILED(IMFSample_GetSampleDuration(input_sample, &in_dur))) in_dur = INT64_MIN;
        nt_status = winedmo_transform_push_input(processor->winedmo_transform, data, current_size,
                in_pts, INT64_MIN, in_dur, 0);
    }
    if (nt_status == STATUS_DEVICE_BUSY) return MF_E_NOTACCEPTING;
    if (nt_status) return E_FAIL;

    if (processor->output_info.cbSize && SUCCEEDED(video_processor_get_d3d11_resource(output_sample, &resource)))
    {
        ID3D11Resource_Release(resource);

        if (SUCCEEDED(hr = video_processor_get_cpu_output_buffer(processor, &data, &out_size)))
        {
            nt_status = winedmo_transform_get_output(processor->winedmo_transform, data, &out_size,
                    &out_pts, &out_dur, &out_flags);
            if (!nt_status)
            {
                if (out_flags & WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED)
                    return MF_E_TRANSFORM_STREAM_CHANGE;
                flip_rgb32_output(processor, data, out_size);
                if (SUCCEEDED(hr = video_processor_upload_d3d11_output(processor, output_sample, data, out_size)))
                {
                    if (out_pts != INT64_MIN)
                    {
                        IMFSample_SetSampleTime(output_sample, out_pts);
                        if (out_dur != INT64_MIN)
                            IMFSample_SetSampleDuration(output_sample, out_dur);
                    }
                    if (out_flags & WINEDMO_SAMPLE_FLAG_INCOMPLETE)
                        sample->dwStatus |= MFT_OUTPUT_DATA_BUFFER_INCOMPLETE;
                }
                return hr;
            }
            if (nt_status == STATUS_MORE_PROCESSING_REQUIRED) return MF_E_TRANSFORM_NEED_MORE_INPUT;
            if (nt_status == STATUS_END_OF_FILE) return MF_E_END_OF_STREAM;
            if (nt_status) return E_FAIL;
        }
    }

    if (FAILED(hr = IMFSample_ConvertToContiguousBuffer(output_sample, &buffer)))
        return hr;
    if (FAILED(hr = IMFMediaBuffer_Lock(buffer, &data, &max_size, NULL)))
    {
        IMFMediaBuffer_Release(buffer);
        return hr;
    }
    out_size = max_size;
    nt_status = winedmo_transform_get_output(processor->winedmo_transform, data, &out_size, &out_pts, &out_dur, &out_flags);
    if (!nt_status)
    {
        if (out_flags & WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED)
        {
            IMFMediaBuffer_Unlock(buffer);
            IMFMediaBuffer_Release(buffer);
            return MF_E_TRANSFORM_STREAM_CHANGE;
        }
        IMFMediaBuffer_SetCurrentLength(buffer, out_size);
        flip_rgb32_output(processor, data, out_size);
        if (out_pts != INT64_MIN)
        {
            IMFSample_SetSampleTime(output_sample, out_pts);
            if (out_dur != INT64_MIN)
                IMFSample_SetSampleDuration(output_sample, out_dur);
        }
        if (out_flags & WINEDMO_SAMPLE_FLAG_INCOMPLETE)
            sample->dwStatus |= MFT_OUTPUT_DATA_BUFFER_INCOMPLETE;
    }
    IMFMediaBuffer_Unlock(buffer);
    IMFMediaBuffer_Release(buffer);
    if (nt_status == STATUS_MORE_PROCESSING_REQUIRED) return MF_E_TRANSFORM_NEED_MORE_INPUT;
    if (nt_status == STATUS_END_OF_FILE) return MF_E_END_OF_STREAM;
    if (nt_status) return E_FAIL;
    return S_OK;
}

static struct video_processor *impl_from_IMFTransform(IMFTransform *iface)
{
    return CONTAINING_RECORD(iface, struct video_processor, IMFTransform_iface);
}

static HRESULT WINAPI video_processor_QueryInterface(IMFTransform *iface, REFIID iid, void **out)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IMFTransform))
        *out = &impl->IMFTransform_iface;
    else if (IsEqualGUID(iid, &IID_IMFVideoProcessorControl))
        *out = &impl->IMFVideoProcessorControl_iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI video_processor_AddRef(IMFTransform *iface)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);
    ULONG refcount = InterlockedIncrement(&impl->refcount);

    TRACE("iface %p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI video_processor_Release(IMFTransform *iface)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);
    ULONG refcount = InterlockedDecrement(&impl->refcount);

    TRACE("iface %p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        video_processor_clear_d3d11_cache(impl);
        video_processor_uninit_allocator(impl);
        if (impl->device_manager)
            IUnknown_Release(impl->device_manager);
        free(impl->cpu_input_buffer);
        free(impl->cpu_output_buffer);
        if (impl->winedmo_transform.handle)
            winedmo_transform_destroy(impl->winedmo_transform);
        if (impl->input_type)
            IMFMediaType_Release(impl->input_type);
        if (impl->output_type)
            IMFMediaType_Release(impl->output_type);
        if (impl->attributes)
            IMFAttributes_Release(impl->attributes);
        if (impl->output_attributes)
            IMFAttributes_Release(impl->output_attributes);

        free(impl);
    }

    return refcount;
}

static HRESULT WINAPI video_processor_GetStreamLimits(IMFTransform *iface, DWORD *input_minimum,
        DWORD *input_maximum, DWORD *output_minimum, DWORD *output_maximum)
{
    TRACE("iface %p, input_minimum %p, input_maximum %p, output_minimum %p, output_maximum %p.\n",
            iface, input_minimum, input_maximum, output_minimum, output_maximum);
    *input_minimum = *input_maximum = *output_minimum = *output_maximum = 1;
    return S_OK;
}

static HRESULT WINAPI video_processor_GetStreamCount(IMFTransform *iface, DWORD *inputs, DWORD *outputs)
{
    TRACE("iface %p, inputs %p, outputs %p.\n", iface, inputs, outputs);
    *inputs = *outputs = 1;
    return S_OK;
}

static HRESULT WINAPI video_processor_GetStreamIDs(IMFTransform *iface, DWORD input_size, DWORD *inputs,
        DWORD output_size, DWORD *outputs)
{
    TRACE("iface %p, input_size %lu, inputs %p, output_size %lu, outputs %p.\n", iface,
            input_size, inputs, output_size, outputs);
    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_GetInputStreamInfo(IMFTransform *iface, DWORD id, MFT_INPUT_STREAM_INFO *info)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %#lx, info %p.\n", iface, id, info);

    if (id)
        return MF_E_INVALIDSTREAMNUMBER;

    *info = impl->input_info;
    return S_OK;
}

static HRESULT WINAPI video_processor_GetOutputStreamInfo(IMFTransform *iface, DWORD id, MFT_OUTPUT_STREAM_INFO *info)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %#lx, info %p.\n", iface, id, info);

    if (id)
        return MF_E_INVALIDSTREAMNUMBER;

    *info = impl->output_info;
    return S_OK;
}

static HRESULT WINAPI video_processor_GetAttributes(IMFTransform *iface, IMFAttributes **attributes)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);

    TRACE("iface %p, attributes %p\n", iface, attributes);

    if (!attributes)
        return E_POINTER;

    IMFAttributes_AddRef((*attributes = impl->attributes));
    return S_OK;
}

static HRESULT WINAPI video_processor_GetInputStreamAttributes(IMFTransform *iface, DWORD id, IMFAttributes **attributes)
{
    TRACE("iface %p, id %#lx, attributes %p.\n", iface, id, attributes);
    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_GetOutputStreamAttributes(IMFTransform *iface, DWORD id, IMFAttributes **attributes)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %#lx, attributes %p\n", iface, id, attributes);

    if (!attributes)
        return E_POINTER;
    if (id)
        return MF_E_INVALIDSTREAMNUMBER;

    IMFAttributes_AddRef((*attributes = impl->output_attributes));
    return S_OK;
}

static HRESULT WINAPI video_processor_DeleteInputStream(IMFTransform *iface, DWORD id)
{
    TRACE("iface %p, id %#lx.\n", iface, id);
    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_AddInputStreams(IMFTransform *iface, DWORD streams, DWORD *ids)
{
    TRACE("iface %p, streams %lu, ids %p.\n", iface, streams, ids);
    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_GetInputAvailableType(IMFTransform *iface, DWORD id, DWORD index,
        IMFMediaType **type)
{
    IMFMediaType *media_type;
    const GUID *subtype;
    HRESULT hr;

    TRACE("iface %p, id %#lx, index %#lx, type %p.\n", iface, id, index, type);

    *type = NULL;

    if (index >= ARRAY_SIZE(input_types))
        return MF_E_NO_MORE_TYPES;
    subtype = input_types[index];

    if (FAILED(hr = MFCreateMediaType(&media_type)))
        return hr;

    if (FAILED(hr = IMFMediaType_SetGUID(media_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video)))
        goto done;
    if (FAILED(hr = IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, subtype)))
        goto done;

    IMFMediaType_AddRef((*type = media_type));

done:
    IMFMediaType_Release(media_type);
    return hr;
}

static HRESULT WINAPI video_processor_GetOutputAvailableType(IMFTransform *iface, DWORD id, DWORD index,
        IMFMediaType **type)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);
    IMFMediaType *media_type;
    UINT64 frame_size;
    GUID subtype;
    HRESULT hr;

    TRACE("iface %p, id %#lx, index %#lx, type %p.\n", iface, id, index, type);

    *type = NULL;

    if (!impl->input_type)
        return MF_E_NO_MORE_TYPES;

    if (FAILED(hr = IMFMediaType_GetGUID(impl->input_type, &MF_MT_SUBTYPE, &subtype))
            || FAILED(hr = IMFMediaType_GetUINT64(impl->input_type, &MF_MT_FRAME_SIZE, &frame_size)))
        return hr;

    if (index > ARRAY_SIZE(output_types))
        return MF_E_NO_MORE_TYPES;
    if (index > 0)
        subtype = *output_types[index - 1];

    if (FAILED(hr = MFCreateMediaType(&media_type)))
        return hr;

    if (FAILED(hr = IMFMediaType_SetGUID(media_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video)))
        goto done;
    if (FAILED(hr = IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &subtype)))
        goto done;
    if (FAILED(hr = IMFMediaType_SetUINT64(media_type, &MF_MT_FRAME_SIZE, frame_size)))
        goto done;

    IMFMediaType_AddRef((*type = media_type));

done:
    IMFMediaType_Release(media_type);
    return hr;
}

static HRESULT WINAPI video_processor_SetInputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);
    GUID major, subtype;
    UINT64 frame_size;
    HRESULT hr;
    ULONG i;

    TRACE("iface %p, id %#lx, type %p, flags %#lx.\n", iface, id, type, flags);

    if (!type)
    {
        video_processor_clear_d3d11_cache(impl);
        if (impl->input_type)
        {
            IMFMediaType_Release(impl->input_type);
            impl->input_type = NULL;
        }
        if (impl->winedmo_transform.handle)
        {
            winedmo_transform_destroy(impl->winedmo_transform);
            impl->winedmo_transform.handle = 0;
        }

        return S_OK;
    }

    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major))
            || !IsEqualGUID(&major, &MFMediaType_Video))
        return E_INVALIDARG;
    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(hr = IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &frame_size)))
        return hr;

    for (i = 0; i < ARRAY_SIZE(input_types); ++i)
        if (IsEqualGUID(&subtype, input_types[i]))
            break;
    if (i == ARRAY_SIZE(input_types))
        return MF_E_INVALIDMEDIATYPE;
    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    video_processor_clear_d3d11_cache(impl);

    if (impl->input_type)
        IMFMediaType_Release(impl->input_type);
    IMFMediaType_AddRef((impl->input_type = type));

    if (impl->output_type && FAILED(hr = try_create_winedmo_transform(impl)))
    {
        IMFMediaType_Release(impl->input_type);
        impl->input_type = NULL;
    }

    if (FAILED(hr) || FAILED(MFCalculateImageSize(&subtype, frame_size >> 32, (UINT32)frame_size,
            (UINT32 *)&impl->input_info.cbSize)))
        impl->input_info.cbSize = 0;

    return hr;
}

static HRESULT WINAPI video_processor_SetOutputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);
    GUID major, subtype;
    UINT64 frame_size;
    HRESULT hr;
    ULONG i;

    TRACE("iface %p, id %#lx, type %p, flags %#lx.\n", iface, id, type, flags);

    if (!type)
    {
        video_processor_clear_d3d11_cache(impl);
        if (impl->output_type)
        {
            IMFMediaType_Release(impl->output_type);
            impl->output_type = NULL;
        }
        if (impl->winedmo_transform.handle)
        {
            winedmo_transform_destroy(impl->winedmo_transform);
            impl->winedmo_transform.handle = 0;
        }

        return S_OK;
    }

    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major))
            || !IsEqualGUID(&major, &MFMediaType_Video))
        return E_INVALIDARG;
    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(hr = IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &frame_size)))
        return hr;

    for (i = 0; i < ARRAY_SIZE(output_types); ++i)
        if (IsEqualGUID(&subtype, output_types[i]))
            break;
    if (i == ARRAY_SIZE(output_types))
        return MF_E_INVALIDMEDIATYPE;
    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    video_processor_clear_d3d11_cache(impl);

    if (FAILED(hr = video_processor_uninit_allocator(impl)))
        return hr;

    if (impl->output_type)
        IMFMediaType_Release(impl->output_type);
    IMFMediaType_AddRef((impl->output_type = type));

    if (impl->input_type && FAILED(hr = try_create_winedmo_transform(impl)))
    {
        IMFMediaType_Release(impl->output_type);
        impl->output_type = NULL;
    }

    if (FAILED(hr) || FAILED(MFCalculateImageSize(&subtype, frame_size >> 32, (UINT32)frame_size,
            (UINT32 *)&impl->output_info.cbSize)))
        impl->output_info.cbSize = 0;

    return hr;
}

static HRESULT WINAPI video_processor_GetInputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);
    HRESULT hr;

    TRACE("iface %p, id %#lx, type %p.\n", iface, id, type);

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;

    if (!impl->input_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = MFCreateMediaType(type)))
        return hr;

    if (FAILED(hr = IMFMediaType_CopyAllItems(impl->input_type, (IMFAttributes *)*type)))
        IMFMediaType_Release(*type);

    return hr;
}

static HRESULT WINAPI video_processor_GetOutputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);
    HRESULT hr;

    TRACE("iface %p, id %#lx, type %p.\n", iface, id, type);

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;

    if (!impl->output_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = MFCreateMediaType(type)))
        return hr;

    if (FAILED(hr = IMFMediaType_CopyAllItems(impl->output_type, (IMFAttributes *)*type)))
        IMFMediaType_Release(*type);

    return hr;
}

static HRESULT WINAPI video_processor_GetInputStatus(IMFTransform *iface, DWORD id, DWORD *flags)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);

    FIXME("iface %p, id %#lx, flags %p stub!\n", iface, id, flags);

    if (!impl->input_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    *flags = MFT_INPUT_STATUS_ACCEPT_DATA;
    return S_OK;
}

static HRESULT WINAPI video_processor_GetOutputStatus(IMFTransform *iface, DWORD *flags)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);

    FIXME("iface %p, flags %p stub!\n", iface, flags);

    if (!impl->output_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    *flags = MFT_OUTPUT_STATUS_SAMPLE_READY;
    return S_OK;
}

static HRESULT WINAPI video_processor_SetOutputBounds(IMFTransform *iface, LONGLONG lower, LONGLONG upper)
{
    TRACE("iface %p, lower %I64d, upper %I64d.\n", iface, lower, upper);
    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_ProcessEvent(IMFTransform *iface, DWORD id, IMFMediaEvent *event)
{
    FIXME("iface %p, id %#lx, event %p stub!\n", iface, id, event);
    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_ProcessMessage(IMFTransform *iface, MFT_MESSAGE_TYPE message, ULONG_PTR param)
{
    struct video_processor *processor = impl_from_IMFTransform(iface);
    HRESULT hr;

    TRACE("iface %p, message %#x, param %Ix.\n", iface, message, param);

    switch (message)
    {
    case MFT_MESSAGE_SET_D3D_MANAGER:
        video_processor_clear_d3d11_cache(processor);
        if (FAILED(hr = video_processor_uninit_allocator(processor)))
            return hr;

        if (processor->device_manager)
        {
            processor->output_info.dwFlags &= ~MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
            IUnknown_Release(processor->device_manager);
        }
        if ((processor->device_manager = (IUnknown *)param))
        {
            IUnknown_AddRef(processor->device_manager);
            processor->output_info.dwFlags |= MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
        }
        return S_OK;

    case MFT_MESSAGE_COMMAND_FLUSH:
    case MFT_MESSAGE_COMMAND_DRAIN:
    case MFT_MESSAGE_NOTIFY_BEGIN_STREAMING:
    case MFT_MESSAGE_NOTIFY_END_STREAMING:
    case MFT_MESSAGE_NOTIFY_START_OF_STREAM:
    case MFT_MESSAGE_NOTIFY_END_OF_STREAM:
        video_processor_clear_d3d11_cache(processor);
        return S_OK;

    default:
        FIXME("Ignoring message %#x.\n", message);
        return S_OK;
    }
}

static HRESULT WINAPI video_processor_ProcessInput(IMFTransform *iface, DWORD id, IMFSample *sample, DWORD flags)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %#lx, sample %p, flags %#lx.\n", iface, id, sample, flags);

    if (!impl->winedmo_transform.handle)
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    if (impl->input_sample)
        return MF_E_NOTACCEPTING;

    impl->input_sample = sample;
    IMFSample_AddRef(impl->input_sample);
    return S_OK;
}

static HRESULT WINAPI video_processor_ProcessOutput(IMFTransform *iface, DWORD flags, DWORD count,
        MFT_OUTPUT_DATA_BUFFER *samples, DWORD *status)
{
    struct video_processor *impl = impl_from_IMFTransform(iface);
    IMFSample *input_sample, *output_sample;
    HRESULT hr;
    BOOL playback_mode, provide_samples, use_d3d11_processor;

    TRACE("iface %p, flags %#lx, count %lu, samples %p, status %p.\n", iface, flags, count, samples, status);

    if (count != 1)
        return E_INVALIDARG;

    if (!impl->winedmo_transform.handle)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    samples->dwStatus = 0;

    if (!(input_sample = impl->input_sample))
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    impl->input_sample = NULL;

    if (FAILED(IMFAttributes_GetUINT32(impl->attributes, &MF_XVP_PLAYBACK_MODE, (UINT32 *) &playback_mode)))
        playback_mode = FALSE;

    provide_samples = (impl->output_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) && !playback_mode;

    if (provide_samples)
    {
        if (impl->device_manager
                && SUCCEEDED(video_processor_init_allocator(impl))
                && SUCCEEDED(IMFVideoSampleAllocatorEx_AllocateSample(impl->allocator, &output_sample)))
            use_d3d11_processor = TRUE;
        else if (FAILED(hr = video_processor_allocate_cpu_sample(impl, &output_sample)))
        {
            IMFSample_Release(input_sample);
            return hr;
        }
        else
            use_d3d11_processor = FALSE;
    }
    else
    {
        if (!(output_sample = samples->pSample))
        {
            IMFSample_Release(input_sample);
            return E_INVALIDARG;
        }
        IMFSample_AddRef(output_sample);
        use_d3d11_processor = video_processor_can_use_d3d11(input_sample, output_sample);
    }

    if (use_d3d11_processor)
        use_d3d11_processor = FALSE;

    if (use_d3d11_processor)
        hr = video_processor_process_output_d3d11(impl, input_sample, output_sample);
    else
        hr = video_processor_process_output_cpu(impl, input_sample, output_sample, samples);
    if (FAILED(hr) && use_d3d11_processor && provide_samples)
    {
        IMFSample_Release(output_sample);
        if (FAILED(hr = video_processor_allocate_cpu_sample(impl, &output_sample)))
            goto done;
        hr = video_processor_process_output_cpu(impl, input_sample, output_sample, samples);
    }
    if (FAILED(hr))
        goto done;

    if (provide_samples)
    {
        samples->pSample = output_sample;
        IMFSample_AddRef(output_sample);
    }

done:
    IMFSample_Release(output_sample);
    IMFSample_Release(input_sample);
    return hr;
}

static const IMFTransformVtbl video_processor_vtbl =
{
    video_processor_QueryInterface,
    video_processor_AddRef,
    video_processor_Release,
    video_processor_GetStreamLimits,
    video_processor_GetStreamCount,
    video_processor_GetStreamIDs,
    video_processor_GetInputStreamInfo,
    video_processor_GetOutputStreamInfo,
    video_processor_GetAttributes,
    video_processor_GetInputStreamAttributes,
    video_processor_GetOutputStreamAttributes,
    video_processor_DeleteInputStream,
    video_processor_AddInputStreams,
    video_processor_GetInputAvailableType,
    video_processor_GetOutputAvailableType,
    video_processor_SetInputType,
    video_processor_SetOutputType,
    video_processor_GetInputCurrentType,
    video_processor_GetOutputCurrentType,
    video_processor_GetInputStatus,
    video_processor_GetOutputStatus,
    video_processor_SetOutputBounds,
    video_processor_ProcessEvent,
    video_processor_ProcessMessage,
    video_processor_ProcessInput,
    video_processor_ProcessOutput,
};

static struct video_processor *impl_from_IMFVideoProcessorControl(IMFVideoProcessorControl *iface)
{
    return CONTAINING_RECORD(iface, struct video_processor, IMFVideoProcessorControl_iface);
}

static HRESULT WINAPI video_processor_control_QueryInterface(IMFVideoProcessorControl *iface, REFIID iid, void **out)
{
    return video_processor_QueryInterface(&impl_from_IMFVideoProcessorControl(iface)->IMFTransform_iface, iid, out);
}

static ULONG WINAPI video_processor_control_AddRef(IMFVideoProcessorControl *iface)
{
    return video_processor_AddRef(&impl_from_IMFVideoProcessorControl(iface)->IMFTransform_iface);
}

static ULONG WINAPI video_processor_control_Release(IMFVideoProcessorControl *iface)
{
    return video_processor_Release(&impl_from_IMFVideoProcessorControl(iface)->IMFTransform_iface);
}

static HRESULT WINAPI video_processor_control_SetBorderColor(IMFVideoProcessorControl *iface, MFARGB *color)
{
    FIXME("iface %p, color %p: stub.\n", iface, color);
    //return E_NOTIMPL;
    return S_OK;
}

static HRESULT WINAPI video_processor_control_SetSourceRectangle(IMFVideoProcessorControl *iface, RECT *rect)
{
    FIXME("iface %p, rect %p: stub.\n", iface, rect);
    //return E_NOTIMPL;
    return S_OK;
}

static HRESULT WINAPI video_processor_control_SetDestinationRectangle(IMFVideoProcessorControl *iface, RECT *rect)
{
    FIXME("iface %p, rect %p: stub.\n", iface, rect);
    //return E_NOTIMPL;
    return S_OK;
}

static HRESULT WINAPI video_processor_control_SetMirror(IMFVideoProcessorControl *iface, MF_VIDEO_PROCESSOR_MIRROR mirror)
{
    FIXME("iface %p, mirror %d: stub.\n", iface, mirror);
    //return E_NOTIMPL;
    return S_OK;
}

static HRESULT WINAPI video_processor_control_SetRotation(IMFVideoProcessorControl *iface, MF_VIDEO_PROCESSOR_ROTATION rotation)
{
    FIXME("iface %p, rotation %d: stub.\n", iface, rotation);
    //return E_NOTIMPL;
    return S_OK;
}

static HRESULT WINAPI video_processor_control_SetConstrictionSize(IMFVideoProcessorControl *iface, SIZE *size)
{
    FIXME("iface %p, size %p: stub.\n", iface, size);
    //return E_NOTIMPL;
    return S_OK;
}

static const IMFVideoProcessorControlVtbl video_processor_control_vtbl =
{
    video_processor_control_QueryInterface,
    video_processor_control_AddRef,
    video_processor_control_Release,
    video_processor_control_SetBorderColor,
    video_processor_control_SetSourceRectangle,
    video_processor_control_SetDestinationRectangle,
    video_processor_control_SetMirror,
    video_processor_control_SetRotation,
    video_processor_control_SetConstrictionSize,
};

HRESULT video_processor_create(REFIID riid, void **ret)
{
    struct video_processor *impl;
    HRESULT hr;

    TRACE("riid %s, ret %p.\n", debugstr_guid(riid), ret);

    if (!(impl = calloc(1, sizeof(*impl))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = MFCreateAttributes(&impl->attributes, 0)))
        goto failed;
    if (FAILED(hr = IMFAttributes_SetUINT32(impl->attributes, &MF_SA_D3D11_AWARE, TRUE)))
        goto failed;
    /* native only has MF_SA_D3D_AWARE on Win7, but it is useful to have in mfreadwrite */
    if (FAILED(hr = IMFAttributes_SetUINT32(impl->attributes, &MF_SA_D3D_AWARE, TRUE)))
        goto failed;
    if (FAILED(hr = MFCreateAttributes(&impl->output_attributes, 0)))
        goto failed;

    impl->IMFTransform_iface.lpVtbl = &video_processor_vtbl;
    impl->IMFVideoProcessorControl_iface.lpVtbl = &video_processor_control_vtbl;
    impl->refcount = 1;

    *ret = &impl->IMFTransform_iface;
    TRACE("Created %p\n", *ret);
    return S_OK;

failed:
    if (impl->output_attributes)
        IMFAttributes_Release(impl->output_attributes);
    if (impl->attributes)
        IMFAttributes_Release(impl->attributes);
    free(impl);
    return hr;
}
