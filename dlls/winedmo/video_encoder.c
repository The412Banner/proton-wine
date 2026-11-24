/* Generic Video Encoder Transform
 *
 * Copyright 2024 Ziqing Hui for CodeWeavers
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
#include "mediaerr.h"
#include "wmcodecdsp.h"

#include "wine/debug.h"
#include "wine/winedmo.h"

#include "initguid.h"
#include "icodecapi.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

struct video_encoder
{
    IMFTransform IMFTransform_iface;
    ICodecAPI ICodecAPI_iface;
    LONG refcount;

    const GUID *const *input_types;
    UINT input_type_count;
    const GUID *const *output_types;
    UINT output_type_count;

    IMFMediaType *input_type;
    MFT_INPUT_STREAM_INFO input_info;
    IMFMediaType *output_type;
    MFT_OUTPUT_STREAM_INFO output_info;

    IMFAttributes *attributes;

    struct winedmo_transform winedmo_transform;
};

static inline struct video_encoder *impl_from_IMFTransform(IMFTransform *iface)
{
    return CONTAINING_RECORD(iface, struct video_encoder, IMFTransform_iface);
}

static inline struct video_encoder *impl_from_ICodecAPI(ICodecAPI *iface)
{
    return CONTAINING_RECORD(iface, struct video_encoder, ICodecAPI_iface);
}

static HRESULT video_encoder_create_input_type(struct video_encoder *encoder,
        const GUID *subtype, IMFMediaType **out)
{
    IMFVideoMediaType *input_type;
    UINT64 ratio;
    UINT32 value;
    HRESULT hr;

    if (FAILED(hr = MFCreateVideoMediaTypeFromSubtype(subtype, &input_type)))
        return hr;

    if (FAILED(hr = IMFMediaType_GetUINT64(encoder->output_type, &MF_MT_FRAME_SIZE, &ratio))
            || FAILED(hr = IMFVideoMediaType_SetUINT64(input_type, &MF_MT_FRAME_SIZE, ratio)))
        goto done;

    if (FAILED(hr = IMFMediaType_GetUINT64(encoder->output_type, &MF_MT_FRAME_RATE, &ratio))
            || FAILED(hr = IMFVideoMediaType_SetUINT64(input_type, &MF_MT_FRAME_RATE, ratio)))
        goto done;

    if (FAILED(hr = IMFMediaType_GetUINT32(encoder->output_type, &MF_MT_INTERLACE_MODE, &value))
            || FAILED(hr = IMFVideoMediaType_SetUINT32(input_type, &MF_MT_INTERLACE_MODE, value)))
        goto done;

    if (FAILED(IMFMediaType_GetUINT32(encoder->output_type, &MF_MT_VIDEO_NOMINAL_RANGE, &value)))
        value = MFNominalRange_Wide;
    if (FAILED(hr = IMFVideoMediaType_SetUINT32(input_type, &MF_MT_VIDEO_NOMINAL_RANGE, value)))
        goto done;

    if (FAILED(IMFMediaType_GetUINT64(encoder->output_type, &MF_MT_PIXEL_ASPECT_RATIO, &ratio)))
        ratio = (UINT64)1 << 32 | 1;
    if (FAILED(hr = IMFVideoMediaType_SetUINT64(input_type, &MF_MT_PIXEL_ASPECT_RATIO, ratio)))
        goto done;

    IMFMediaType_AddRef((*out = (IMFMediaType *)input_type));

done:
    IMFVideoMediaType_Release(input_type);
    return hr;
}

static HRESULT video_encoder_try_create_winedmo_transform(struct video_encoder *encoder)
{
    MFVIDEOFORMAT *input_format, *output_format;
    UINT32 input_size, output_size;
    NTSTATUS status;
    HRESULT hr;

    if (encoder->winedmo_transform.handle)
    {
        winedmo_transform_destroy(encoder->winedmo_transform);
        encoder->winedmo_transform.handle = 0;
    }

    if (FAILED(hr = MFCreateMFVideoFormatFromMFMediaType(encoder->input_type, &input_format, &input_size)))
        return hr;
    if (FAILED(hr = MFCreateMFVideoFormatFromMFMediaType(encoder->output_type, &output_format, &output_size)))
    {
        CoTaskMemFree(input_format);
        return hr;
    }

    status = winedmo_transform_create(MFMediaType_Video,
            (const union winedmo_format *)input_format, input_size,
            (const union winedmo_format *)output_format, output_size,
            &encoder->winedmo_transform);
    CoTaskMemFree(output_format);
    CoTaskMemFree(input_format);
    return status ? E_FAIL : S_OK;
}

static HRESULT WINAPI transform_QueryInterface(IMFTransform *iface, REFIID iid, void **out)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IMFTransform) || IsEqualGUID(iid, &IID_IUnknown))
        *out = &encoder->IMFTransform_iface;
    else if (IsEqualGUID(iid, &IID_ICodecAPI))
        *out = &encoder->ICodecAPI_iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI transform_AddRef(IMFTransform *iface)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);
    ULONG refcount = InterlockedIncrement(&encoder->refcount);

    TRACE("iface %p increasing refcount to %lu.\n", encoder, refcount);

    return refcount;
}

static ULONG WINAPI transform_Release(IMFTransform *iface)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);
    ULONG refcount = InterlockedDecrement(&encoder->refcount);

    TRACE("iface %p decreasing refcount to %lu.\n", encoder, refcount);

    if (!refcount)
    {
        if (encoder->input_type)
            IMFMediaType_Release(encoder->input_type);
        if (encoder->output_type)
            IMFMediaType_Release(encoder->output_type);
        IMFAttributes_Release(encoder->attributes);
        if (encoder->winedmo_transform.handle)
            winedmo_transform_destroy(encoder->winedmo_transform);
        free(encoder);
    }

    return refcount;
}

static HRESULT WINAPI transform_GetStreamLimits(IMFTransform *iface, DWORD *input_minimum,
        DWORD *input_maximum, DWORD *output_minimum, DWORD *output_maximum)
{
    TRACE("iface %p, input_minimum %p, input_maximum %p, output_minimum %p, output_maximum %p.\n",
            iface, input_minimum, input_maximum, output_minimum, output_maximum);
    *input_minimum = *input_maximum = *output_minimum = *output_maximum = 1;
    return S_OK;
}

static HRESULT WINAPI transform_GetStreamCount(IMFTransform *iface, DWORD *inputs, DWORD *outputs)
{
    TRACE("iface %p, inputs %p, outputs %p.\n", iface, inputs, outputs);
    *inputs = *outputs = 1;
    return S_OK;
}

static HRESULT WINAPI transform_GetStreamIDs(IMFTransform *iface, DWORD input_size, DWORD *inputs,
        DWORD output_size, DWORD *outputs)
{
    FIXME("iface %p, input_size %lu, inputs %p, output_size %lu, outputs %p.\n", iface,
            input_size, inputs, output_size, outputs);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetInputStreamInfo(IMFTransform *iface, DWORD id, MFT_INPUT_STREAM_INFO *info)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %#lx, info %p.\n", iface, id, info);

    *info = encoder->input_info;
    return S_OK;
}

static HRESULT WINAPI transform_GetOutputStreamInfo(IMFTransform *iface, DWORD id, MFT_OUTPUT_STREAM_INFO *info)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %#lx, info %p.\n", iface, id, info);

    *info = encoder->output_info;
    return S_OK;
}

static HRESULT WINAPI transform_GetAttributes(IMFTransform *iface, IMFAttributes **attributes)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, attributes %p.\n", iface, attributes);

    if (!attributes)
        return E_POINTER;

    IMFAttributes_AddRef((*attributes = encoder->attributes));
    return S_OK;
}

static HRESULT WINAPI transform_GetInputStreamAttributes(IMFTransform *iface, DWORD id, IMFAttributes **attributes)
{
    FIXME("iface %p, id %#lx, attributes %p.\n", iface, id, attributes);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetOutputStreamAttributes(IMFTransform *iface, DWORD id, IMFAttributes **attributes)
{
    FIXME("iface %p, id %#lx, attributes %p.\n", iface, id, attributes);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_DeleteInputStream(IMFTransform *iface, DWORD id)
{
    FIXME("iface %p, id %#lx.\n", iface, id);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_AddInputStreams(IMFTransform *iface, DWORD streams, DWORD *ids)
{
    FIXME("iface %p, streams %lu, ids %p.\n", iface, streams, ids);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetInputAvailableType(IMFTransform *iface, DWORD id, DWORD index,
        IMFMediaType **type)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %#lx, index %#lx, type %p.\n", iface, id, index, type);

    *type = NULL;

    if (!encoder->output_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    if (index >= encoder->input_type_count)
        return MF_E_NO_MORE_TYPES;

    return video_encoder_create_input_type(encoder, encoder->input_types[index], type);
}

static HRESULT WINAPI transform_GetOutputAvailableType(IMFTransform *iface, DWORD id,
        DWORD index, IMFMediaType **type)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %#lx, index %#lx, type %p.\n", iface, id, index, type);

    *type = NULL;
    if (index >= encoder->output_type_count)
        return MF_E_NO_MORE_TYPES;
    return MFCreateVideoMediaTypeFromSubtype(encoder->output_types[index], (IMFVideoMediaType **)type);
}

static HRESULT WINAPI transform_SetInputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);
    IMFMediaType *good_input_type;
    GUID major, subtype;
    UINT64 ratio;
    BOOL result;
    HRESULT hr;
    ULONG i;

    TRACE("iface %p, id %#lx, type %p, flags %#lx.\n", iface, id, type, flags);

    if (!type)
    {
        if (encoder->input_type)
        {
            IMFMediaType_Release(encoder->input_type);
            encoder->input_type = NULL;
        }
        if (encoder->winedmo_transform.handle)
        {
            winedmo_transform_destroy(encoder->winedmo_transform);
            encoder->winedmo_transform.handle = 0;
        }
        return S_OK;
    }

    if (!encoder->output_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major)) ||
            FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return E_INVALIDARG;

    if (!IsEqualGUID(&major, &MFMediaType_Video))
        return MF_E_INVALIDMEDIATYPE;

    for (i = 0; i < encoder->input_type_count; ++i)
        if (IsEqualGUID(&subtype, encoder->input_types[i]))
            break;
    if (i == encoder->input_type_count)
        return MF_E_INVALIDMEDIATYPE;

    if (FAILED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &ratio))
            || FAILED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_RATE, &ratio)))
        return MF_E_INVALIDMEDIATYPE;

    if (FAILED(hr = video_encoder_create_input_type(encoder, &subtype, &good_input_type)))
        return hr;
    hr = IMFMediaType_Compare(good_input_type, (IMFAttributes *)type,
            MF_ATTRIBUTES_MATCH_INTERSECTION, &result);
    IMFMediaType_Release(good_input_type);
    if (FAILED(hr) || !result)
        return MF_E_INVALIDMEDIATYPE;

    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    if (encoder->input_type)
        IMFMediaType_Release(encoder->input_type);
    IMFMediaType_AddRef((encoder->input_type = type));

    if (FAILED(hr = video_encoder_try_create_winedmo_transform(encoder)))
    {
        IMFMediaType_Release(encoder->input_type);
        encoder->input_type = NULL;
    }

    return hr;
}

static HRESULT WINAPI transform_SetOutputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);
    UINT32 uint32_value;
    UINT64 uint64_value;
    GUID major, subtype;
    ULONG i;

    TRACE("iface %p, id %#lx, type %p, flags %#lx.\n", iface, id, type, flags);

    if (!type)
    {
        if (encoder->input_type)
        {
            IMFMediaType_Release(encoder->input_type);
            encoder->input_type = NULL;
        }
        if (encoder->output_type)
        {
            IMFMediaType_Release(encoder->output_type);
            encoder->output_type = NULL;
            memset(&encoder->output_info, 0, sizeof(encoder->output_info));
        }
        if (encoder->winedmo_transform.handle)
        {
            winedmo_transform_destroy(encoder->winedmo_transform);
            encoder->winedmo_transform.handle = 0;
        }
        return S_OK;
    }

    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major))
            || FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return E_INVALIDARG;

    if (!IsEqualGUID(&major, &MFMediaType_Video))
        return MF_E_INVALIDMEDIATYPE;

    for (i = 0; i < encoder->output_type_count; ++i)
        if (IsEqualGUID(&subtype, encoder->output_types[i]))
            break;
    if (i == encoder->output_type_count)
        return MF_E_INVALIDMEDIATYPE;

    if (FAILED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &uint64_value)))
        return MF_E_INVALIDMEDIATYPE;

    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    if (FAILED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_RATE, &uint64_value))
            || FAILED(IMFMediaType_GetUINT32(type, &MF_MT_AVG_BITRATE, &uint32_value))
            || FAILED(IMFMediaType_GetUINT32(type, &MF_MT_INTERLACE_MODE, &uint32_value)))
        return MF_E_INVALIDMEDIATYPE;

    if (encoder->input_type)
    {
        IMFMediaType_Release(encoder->input_type);
        encoder->input_type = NULL;
    }

    if (encoder->output_type)
        IMFMediaType_Release(encoder->output_type);
    IMFMediaType_AddRef((encoder->output_type = type));

    /* FIXME: Add MF_MT_MPEG_SEQUENCE_HEADER attribute. */

    /* FIXME: Hardcode a size that native uses for 1920 * 1080.
     * And hope it's large enough to make things work for now.
     * The right way is to calculate it based on frame width and height. */
    encoder->output_info.cbSize = 0x3bc400;

    if (encoder->winedmo_transform.handle)
    {
        winedmo_transform_destroy(encoder->winedmo_transform);
        encoder->winedmo_transform.handle = 0;
    }

    return S_OK;
}

static HRESULT WINAPI transform_GetInputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);
    HRESULT hr;

    TRACE("iface %p, id %#lx, type %p\n", iface, id, type);

    if (!encoder->input_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = MFCreateMediaType(type)))
        return hr;

    return IMFMediaType_CopyAllItems(encoder->input_type, (IMFAttributes *)*type);

}

static HRESULT WINAPI transform_GetOutputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);
    HRESULT hr;

    TRACE("iface %p, id %#lx, type %p\n", iface, id, type);

    if (!encoder->output_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = MFCreateMediaType(type)))
        return hr;

    return IMFMediaType_CopyAllItems(encoder->output_type, (IMFAttributes *)*type);
}

static HRESULT WINAPI transform_GetInputStatus(IMFTransform *iface, DWORD id, DWORD *flags)
{
    FIXME("iface %p, id %#lx, flags %p.\n", iface, id, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetOutputStatus(IMFTransform *iface, DWORD *flags)
{
    FIXME("iface %p, flags %p stub!\n", iface, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_SetOutputBounds(IMFTransform *iface, LONGLONG lower, LONGLONG upper)
{
    FIXME("iface %p, lower %I64d, upper %I64d.\n", iface, lower, upper);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_ProcessEvent(IMFTransform *iface, DWORD id, IMFMediaEvent *event)
{
    FIXME("iface %p, id %#lx, event %p stub!\n", iface, id, event);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_ProcessMessage(IMFTransform *iface, MFT_MESSAGE_TYPE message, ULONG_PTR param)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, message %#x, param %Ix.\n", iface, message, param);

    switch (message)
    {
        case MFT_MESSAGE_COMMAND_DRAIN:
            return winedmo_transform_drain(encoder->winedmo_transform) ? E_FAIL : S_OK;

        case MFT_MESSAGE_COMMAND_FLUSH:
            return winedmo_transform_flush(encoder->winedmo_transform) ? E_FAIL : S_OK;

        default:
            FIXME("Ignoring message %#x.\n", message);
            return S_OK;
    }
}

static HRESULT WINAPI transform_ProcessInput(IMFTransform *iface, DWORD id, IMFSample *sample, DWORD flags)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %#lx, sample %p, flags %#lx.\n", iface, id, sample, flags);

    if (!encoder->winedmo_transform.handle)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    {
        IMFMediaBuffer *buffer;
        NTSTATUS nt_status;
        BYTE *data;
        DWORD size;
        INT64 pts, dur;
        HRESULT hr;

        if (FAILED(hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer)))
            return hr;
        if (FAILED(hr = IMFMediaBuffer_Lock(buffer, &data, NULL, &size)))
        {
            IMFMediaBuffer_Release(buffer);
            return hr;
        }
        if (FAILED(IMFSample_GetSampleTime(sample, &pts))) pts = INT64_MIN;
        if (FAILED(IMFSample_GetSampleDuration(sample, &dur))) dur = INT64_MIN;
        nt_status = winedmo_transform_push_input(encoder->winedmo_transform, data, size,
                pts, INT64_MIN, dur, 0);
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_Release(buffer);
        if (nt_status == STATUS_DEVICE_BUSY) return MF_E_NOTACCEPTING;
        return nt_status ? E_FAIL : S_OK;
    }
}

static HRESULT WINAPI transform_ProcessOutput(IMFTransform *iface, DWORD flags, DWORD count,
        MFT_OUTPUT_DATA_BUFFER *samples, DWORD *status)
{
    struct video_encoder *encoder = impl_from_IMFTransform(iface);
    HRESULT hr;

    TRACE("iface %p, flags %#lx, count %lu, samples %p, status %p.\n", iface, flags, count, samples, status);

    if (count != 1)
        return E_INVALIDARG;
    if (!encoder->winedmo_transform.handle)
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    if (!samples->pSample)
        return E_INVALIDARG;

    {
        IMFMediaBuffer *buffer;
        NTSTATUS nt_status;
        BYTE *data;
        DWORD max_size;
        UINT32 out_size;
        INT64 out_pts, out_dur;
        DWORD out_flags;

        if (FAILED(hr = IMFSample_ConvertToContiguousBuffer(samples->pSample, &buffer)))
            return hr;
        if (FAILED(hr = IMFMediaBuffer_Lock(buffer, &data, &max_size, NULL)))
        {
            IMFMediaBuffer_Release(buffer);
            return hr;
        }
        out_size = max_size;
        nt_status = winedmo_transform_get_output(encoder->winedmo_transform, data, &out_size, &out_pts, &out_dur, &out_flags);
        if (!nt_status)
        {
            if (out_flags & WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED)
            {
                IMFMediaBuffer_Unlock(buffer);
                IMFMediaBuffer_Release(buffer);
                return MF_E_TRANSFORM_STREAM_CHANGE;
            }
            IMFMediaBuffer_SetCurrentLength(buffer, out_size);
            if (out_pts != INT64_MIN)
            {
                IMFSample_SetSampleTime(samples->pSample, out_pts);
                if (out_dur != INT64_MIN)
                    IMFSample_SetSampleDuration(samples->pSample, out_dur);
            }
            if (out_flags & WINEDMO_SAMPLE_FLAG_INCOMPLETE)
                samples->dwStatus |= MFT_OUTPUT_DATA_BUFFER_INCOMPLETE;
        }
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_Release(buffer);
        if (nt_status == STATUS_MORE_PROCESSING_REQUIRED) return MF_E_TRANSFORM_NEED_MORE_INPUT;
        if (nt_status == STATUS_END_OF_FILE) return MF_E_END_OF_STREAM;
        return nt_status ? E_FAIL : S_OK;
    }
}

static const IMFTransformVtbl transform_vtbl =
{
    transform_QueryInterface,
    transform_AddRef,
    transform_Release,
    transform_GetStreamLimits,
    transform_GetStreamCount,
    transform_GetStreamIDs,
    transform_GetInputStreamInfo,
    transform_GetOutputStreamInfo,
    transform_GetAttributes,
    transform_GetInputStreamAttributes,
    transform_GetOutputStreamAttributes,
    transform_DeleteInputStream,
    transform_AddInputStreams,
    transform_GetInputAvailableType,
    transform_GetOutputAvailableType,
    transform_SetInputType,
    transform_SetOutputType,
    transform_GetInputCurrentType,
    transform_GetOutputCurrentType,
    transform_GetInputStatus,
    transform_GetOutputStatus,
    transform_SetOutputBounds,
    transform_ProcessEvent,
    transform_ProcessMessage,
    transform_ProcessInput,
    transform_ProcessOutput,
};

static HRESULT WINAPI codec_api_QueryInterface(ICodecAPI *iface, REFIID riid, void **out)
{
    struct video_encoder *encoder = impl_from_ICodecAPI(iface);
    return IMFTransform_QueryInterface(&encoder->IMFTransform_iface, riid, out);
}

static ULONG WINAPI codec_api_AddRef(ICodecAPI *iface)
{
    struct video_encoder *encoder = impl_from_ICodecAPI(iface);
    return IMFTransform_AddRef(&encoder->IMFTransform_iface);
}

static ULONG WINAPI codec_api_Release(ICodecAPI *iface)
{
    struct video_encoder *encoder = impl_from_ICodecAPI(iface);
    return IMFTransform_Release(&encoder->IMFTransform_iface);
}

static HRESULT WINAPI codec_api_IsSupported(ICodecAPI *iface, const GUID *api)
{
    FIXME("iface %p, api %s.\n", iface, debugstr_guid(api));
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_IsModifiable(ICodecAPI *iface, const GUID *api)
{
    FIXME("iface %p, api %s.\n", iface, debugstr_guid(api));
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_GetParameterRange(ICodecAPI *iface,
        const GUID *api, VARIANT *min, VARIANT *max, VARIANT *delta)
{
    FIXME("iface %p, api %s, min %p, max %p, delta %p.\n",
            iface, debugstr_guid(api), min, max, delta);
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_GetParameterValues(ICodecAPI *iface, const GUID *api, VARIANT **values, ULONG *count)
{
    FIXME("iface %p, api %s, values %p, count %p.\n", iface, debugstr_guid(api), values, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_GetDefaultValue(ICodecAPI *iface, const GUID *api, VARIANT *value)
{
    FIXME("iface %p, api %s, value %p.\n", iface, debugstr_guid(api), value);
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_GetValue(ICodecAPI *iface, const GUID *api, VARIANT *value)
{
    FIXME("iface %p, api %s, value %p.\n", iface, debugstr_guid(api), value);
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_SetValue(ICodecAPI *iface, const GUID *api, VARIANT *value)
{
    FIXME("iface %p, api %s, value %p.\n", iface, debugstr_guid(api), value);
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_RegisterForEvent(ICodecAPI *iface, const GUID *api, LONG_PTR userData)
{
    FIXME("iface %p, api %s, value %p.\n", iface, debugstr_guid(api), LongToPtr(userData));
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_UnregisterForEvent(ICodecAPI *iface, const GUID *api)
{
    FIXME("iface %p, api %s.\n", iface, debugstr_guid(api));
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_SetAllDefaults(ICodecAPI *iface)
{
    FIXME("iface %p.\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_SetValueWithNotify(ICodecAPI *iface,
        const GUID *api, VARIANT *value, GUID **param, ULONG *count)
{
    FIXME("iface %p, api %s, param %p, count %p.\n", iface, debugstr_guid(api), param, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_SetAllDefaultsWithNotify(ICodecAPI *iface, GUID **param, ULONG *count)
{
    FIXME("iface %p, param %p, count %p.\n", iface, param, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_GetAllSettings(ICodecAPI *iface, IStream *stream)
{
    FIXME("iface %p, stream %p.\n", iface, stream);
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_SetAllSettings(ICodecAPI *iface, IStream *stream)
{
    FIXME("iface %p, stream %p.\n", iface, stream);
    return E_NOTIMPL;
}

static HRESULT WINAPI codec_api_SetAllSettingsWithNotify(ICodecAPI *iface, IStream *stream, GUID **param, ULONG *count)
{
    FIXME("iface %p, stream %p, param %p, count %p.\n", iface, stream, param, count);
    return E_NOTIMPL;
}

static const ICodecAPIVtbl codec_api_vtbl =
{
    codec_api_QueryInterface,
    codec_api_AddRef,
    codec_api_Release,
    codec_api_IsSupported,
    codec_api_IsModifiable,
    codec_api_GetParameterRange,
    codec_api_GetParameterValues,
    codec_api_GetDefaultValue,
    codec_api_GetValue,
    codec_api_SetValue,
    codec_api_RegisterForEvent,
    codec_api_UnregisterForEvent,
    codec_api_SetAllDefaults,
    codec_api_SetValueWithNotify,
    codec_api_SetAllDefaultsWithNotify,
    codec_api_GetAllSettings,
    codec_api_SetAllSettings,
    codec_api_SetAllSettingsWithNotify,
};

static HRESULT video_encoder_create(const GUID *const *input_types, UINT input_type_count,
        const GUID *const *output_types, UINT output_type_count, struct video_encoder **out)
{
    struct video_encoder *encoder;
    HRESULT hr;

    if (!(encoder = calloc(1, sizeof(*encoder))))
        return E_OUTOFMEMORY;

    encoder->IMFTransform_iface.lpVtbl = &transform_vtbl;
    encoder->ICodecAPI_iface.lpVtbl = &codec_api_vtbl;
    encoder->refcount = 1;

    encoder->input_types = input_types;
    encoder->input_type_count = input_type_count;
    encoder->output_types = output_types;
    encoder->output_type_count = output_type_count;

    if (FAILED(hr = MFCreateAttributes(&encoder->attributes, 16)))
        goto failed;
    if (FAILED(hr = IMFAttributes_SetUINT32(encoder->attributes, &MFT_ENCODER_SUPPORTS_CONFIG_EVENT, TRUE)))
        goto failed;

    *out = encoder;
    TRACE("Created video encoder %p\n", encoder);
    return S_OK;

failed:
    if (encoder->attributes)
        IMFAttributes_Release(encoder->attributes);
    free(encoder);
    return hr;
}

static const GUID *const h264_encoder_input_types[] =
{
    &MFVideoFormat_IYUV,
    &MFVideoFormat_YV12,
    &MFVideoFormat_NV12,
    &MFVideoFormat_YUY2,
};

static const GUID *const h264_encoder_output_types[] =
{
    &MFVideoFormat_H264,
};

HRESULT h264_encoder_create(REFIID riid, void **out)
{
    struct video_encoder *encoder;
    HRESULT hr;

    TRACE("riid %s, out %p.\n", debugstr_guid(riid), out);

    if (FAILED(hr = video_encoder_create(h264_encoder_input_types, ARRAY_SIZE(h264_encoder_input_types),
            h264_encoder_output_types, ARRAY_SIZE(h264_encoder_output_types), &encoder)))
        return hr;

    TRACE("Created h264 encoder transform %p.\n", &encoder->IMFTransform_iface);

    hr = IMFTransform_QueryInterface(&encoder->IMFTransform_iface, riid, out);
    IMFTransform_Release(&encoder->IMFTransform_iface);
    return hr;
}
