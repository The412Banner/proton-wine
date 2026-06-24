/* WMA Decoder DMO / MF Transform
 *
 * Copyright 2022 Rémi Bernon for CodeWeavers
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
#include "mediaerr.h"
#include "dmort.h"

#include "wine/debug.h"
#include "wine/winedmo.h"

WINE_DEFAULT_DEBUG_CHANNEL(wmadec);

extern const GUID MFAudioFormat_XMAudio2;

static const GUID *const wma_decoder_input_types[] =
{
    &MEDIASUBTYPE_MSAUDIO1,
    &MFAudioFormat_WMAudioV8,
    &MFAudioFormat_WMAudioV9,
    &MFAudioFormat_WMAudio_Lossless,
    &MFAudioFormat_XMAudio2,
};
static const GUID *const wma_decoder_output_types[] =
{
    &MFAudioFormat_Float,
    &MFAudioFormat_PCM,
};

static BOOL is_wma_decoder_input_subtype(const GUID *subtype)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(wma_decoder_input_types); ++i)
    {
        if (IsEqualGUID(subtype, wma_decoder_input_types[i]))
            return TRUE;
    }

    return FALSE;
}

static BOOL is_valid_wma_decoder_input_type(const DMO_MEDIA_TYPE *type)
{
    const WAVEFORMATEX *wfx;

    if (!IsEqualGUID(&type->majortype, &MEDIATYPE_Audio)
            || !is_wma_decoder_input_subtype(&type->subtype))
        return FALSE;

    if (!IsEqualGUID(&type->formattype, &FORMAT_WaveFormatEx)
            || type->cbFormat < sizeof(*wfx) || !type->pbFormat)
        return FALSE;

    wfx = (const WAVEFORMATEX *)type->pbFormat;
    if (type->cbFormat < sizeof(*wfx) + wfx->cbSize)
        return FALSE;

    if (!wfx->nChannels || !wfx->nSamplesPerSec || !wfx->nBlockAlign)
        return FALSE;

    return TRUE;
}

struct wma_decoder
{
    IUnknown IUnknown_inner;
    IMFTransform IMFTransform_iface;
    IMediaObject IMediaObject_iface;
    IPropertyBag IPropertyBag_iface;
    IUnknown *outer;
    LONG refcount;

    DMO_MEDIA_TYPE input_type;
    DMO_MEDIA_TYPE output_type;

    DWORD input_buf_size;
    DWORD output_buf_size;

    struct winedmo_transform winedmo_transform;
};

static inline struct wma_decoder *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct wma_decoder, IUnknown_inner);
}

static HRESULT try_create_winedmo_transform(struct wma_decoder *decoder)
{
    const WAVEFORMATEX *in_wfx  = (WAVEFORMATEX *)decoder->input_type.pbFormat;
    const WAVEFORMATEX *out_wfx = (WAVEFORMATEX *)decoder->output_type.pbFormat;
    NTSTATUS status;

    if (decoder->winedmo_transform.handle)
    {
        winedmo_transform_destroy(decoder->winedmo_transform);
        decoder->winedmo_transform.handle = 0;
    }

    if (!in_wfx || !out_wfx)
        return E_FAIL;

    status = winedmo_transform_create(MFMediaType_Audio,
            (union winedmo_format *)in_wfx,  sizeof(*in_wfx)  + in_wfx->cbSize,
            (union winedmo_format *)out_wfx, sizeof(*out_wfx) + out_wfx->cbSize,
            &decoder->winedmo_transform);
    if (status) WARN("winedmo_transform_create failed, status %#lx\n", status);
    return status ? E_FAIL : S_OK;
}

static HRESULT WINAPI unknown_QueryInterface(IUnknown *iface, REFIID iid, void **out)
{
    struct wma_decoder *decoder = impl_from_IUnknown(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown))
        *out = &decoder->IUnknown_inner;
    else if (IsEqualGUID(iid, &IID_IMFTransform))
        *out = &decoder->IMFTransform_iface;
    else if (IsEqualGUID(iid, &IID_IMediaObject))
        *out = &decoder->IMediaObject_iface;
    else if (IsEqualIID(iid, &IID_IPropertyBag))
        *out = &decoder->IPropertyBag_iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI unknown_AddRef(IUnknown *iface)
{
    struct wma_decoder *decoder = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedIncrement(&decoder->refcount);

    TRACE("iface %p increasing refcount to %lu.\n", decoder, refcount);

    return refcount;
}

static ULONG WINAPI unknown_Release(IUnknown *iface)
{
    struct wma_decoder *decoder = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&decoder->refcount);

    TRACE("iface %p decreasing refcount to %lu.\n", decoder, refcount);

    if (!refcount)
    {
        if (decoder->winedmo_transform.handle)
            winedmo_transform_destroy(decoder->winedmo_transform);

        MoFreeMediaType(&decoder->input_type);
        MoFreeMediaType(&decoder->output_type);
        free(decoder);
    }

    return refcount;
}

static const IUnknownVtbl unknown_vtbl =
{
    unknown_QueryInterface,
    unknown_AddRef,
    unknown_Release,
};

static struct wma_decoder *impl_from_IMFTransform(IMFTransform *iface)
{
    return CONTAINING_RECORD(iface, struct wma_decoder, IMFTransform_iface);
}

static HRESULT WINAPI transform_QueryInterface(IMFTransform *iface, REFIID iid, void **out)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    return IUnknown_QueryInterface(decoder->outer, iid, out);
}

static ULONG WINAPI transform_AddRef(IMFTransform *iface)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    return IUnknown_AddRef(decoder->outer);
}

static ULONG WINAPI transform_Release(IMFTransform *iface)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    return IUnknown_Release(decoder->outer);
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
    TRACE("iface %p, input_size %lu, inputs %p, output_size %lu, outputs %p.\n", iface,
            input_size, inputs, output_size, outputs);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetInputStreamInfo(IMFTransform *iface, DWORD id, MFT_INPUT_STREAM_INFO *info)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %lu, info %p.\n", iface, id, info);

    if (IsEqualGUID(&decoder->input_type.majortype, &GUID_NULL)
            || IsEqualGUID(&decoder->output_type.majortype, &GUID_NULL))
    {
        memset(info, 0, sizeof(*info));
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    }

    info->hnsMaxLatency = 0;
    info->dwFlags = 0;
    info->cbSize = decoder->input_buf_size;
    info->cbMaxLookahead = 0;
    info->cbAlignment = 1;
    return S_OK;
}

static HRESULT WINAPI transform_GetOutputStreamInfo(IMFTransform *iface, DWORD id, MFT_OUTPUT_STREAM_INFO *info)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %lu, info %p.\n", iface, id, info);

    if (IsEqualGUID(&decoder->input_type.majortype, &GUID_NULL)
            || IsEqualGUID(&decoder->output_type.majortype, &GUID_NULL))
    {
        memset(info, 0, sizeof(*info));
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    }

    info->dwFlags = 0;
    info->cbSize = decoder->output_buf_size;
    info->cbAlignment = 1;
    return S_OK;
}

static HRESULT WINAPI transform_GetAttributes(IMFTransform *iface, IMFAttributes **attributes)
{
    TRACE("iface %p, attributes %p.\n", iface, attributes);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetInputStreamAttributes(IMFTransform *iface, DWORD id, IMFAttributes **attributes)
{
    TRACE("iface %p, id %#lx, attributes %p.\n", iface, id, attributes);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetOutputStreamAttributes(IMFTransform *iface, DWORD id, IMFAttributes **attributes)
{
    TRACE("iface %p, id %#lx, attributes %p.\n", iface, id, attributes);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_DeleteInputStream(IMFTransform *iface, DWORD id)
{
    TRACE("iface %p, id %#lx.\n", iface, id);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_AddInputStreams(IMFTransform *iface, DWORD streams, DWORD *ids)
{
    TRACE("iface %p, streams %lu, ids %p.\n", iface, streams, ids);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetInputAvailableType(IMFTransform *iface, DWORD id, DWORD index,
        IMFMediaType **type)
{
    FIXME("iface %p, id %lu, index %lu, type %p stub!\n", iface, id, index, type);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetOutputAvailableType(IMFTransform *iface, DWORD id, DWORD index,
        IMFMediaType **type)
{
    UINT32 sample_size, block_alignment, out_sample_rate, out_channels;
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    IMFMediaType *media_type;
    const GUID *output_type;
    WAVEFORMATEX *wfx;
    HRESULT hr;

    TRACE("iface %p, id %lu, index %lu, type %p.\n", iface, id, index, type);

    if (IsEqualGUID(&decoder->input_type.majortype, &GUID_NULL))
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    *type = NULL;

    if (index >= ARRAY_SIZE(wma_decoder_output_types))
        return MF_E_NO_MORE_TYPES;
    output_type = wma_decoder_output_types[index];

    if (FAILED(hr = MFCreateMediaType(&media_type)))
        return hr;

    if (FAILED(hr = IMFMediaType_SetGUID(media_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio)))
        goto done;
    if (FAILED(hr = IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, output_type)))
        goto done;

    if (IsEqualGUID(output_type, &MFAudioFormat_Float))
        sample_size = 32;
    else if (IsEqualGUID(output_type, &MFAudioFormat_PCM))
        sample_size = 16;
    else
    {
        FIXME("Subtype %s not implemented!\n", debugstr_guid(output_type));
        hr = E_NOTIMPL;
        goto done;
    }

    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_BITS_PER_SAMPLE,
            sample_size)))
        goto done;

    wfx = (WAVEFORMATEX *)decoder->input_type.pbFormat;
    out_channels = wfx->nChannels;
    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_NUM_CHANNELS, out_channels)))
        goto done;
    out_sample_rate = wfx->nSamplesPerSec;
    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, out_sample_rate)))
        goto done;

    block_alignment = sample_size * out_channels / 8;
    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, block_alignment)))
        goto done;
    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, out_sample_rate * block_alignment)))
        goto done;

    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_ALL_SAMPLES_INDEPENDENT, 1)))
        goto done;
    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_FIXED_SIZE_SAMPLES, 1)))
        goto done;
    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_PREFER_WAVEFORMATEX, 1)))
        goto done;

done:
    if (SUCCEEDED(hr))
        IMFMediaType_AddRef((*type = media_type));

    IMFMediaType_Release(media_type);
    return hr;
}

static HRESULT WINAPI transform_SetInputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    MF_ATTRIBUTE_TYPE item_type;
    UINT32 block_alignment;
    GUID major, subtype;
    HRESULT hr;
    ULONG i;

    TRACE("iface %p, id %lu, type %p, flags %#lx.\n", iface, id, type, flags);

    if (FAILED(hr = IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major)) ||
        FAILED(hr = IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return hr;

    if (!IsEqualGUID(&major, &MFMediaType_Audio))
        return MF_E_INVALIDMEDIATYPE;

    for (i = 0; i < ARRAY_SIZE(wma_decoder_input_types); ++i)
        if (IsEqualGUID(&subtype, wma_decoder_input_types[i]))
            break;
    if (i == ARRAY_SIZE(wma_decoder_input_types))
        return MF_E_INVALIDMEDIATYPE;

    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_USER_DATA, &item_type)) ||
        item_type != MF_ATTRIBUTE_BLOB)
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, &block_alignment)))
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &item_type)) ||
        item_type != MF_ATTRIBUTE_UINT32)
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_NUM_CHANNELS, &item_type)) ||
        item_type != MF_ATTRIBUTE_UINT32)
        return MF_E_INVALIDMEDIATYPE;
    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    MoFreeMediaType(&decoder->output_type);
    memset(&decoder->output_type, 0, sizeof(decoder->output_type));
    MoFreeMediaType(&decoder->input_type);
    memset(&decoder->input_type, 0, sizeof(decoder->input_type));

    if (SUCCEEDED(hr = MFInitAMMediaTypeFromMFMediaType(type, GUID_NULL, &decoder->input_type)))
        decoder->input_buf_size = block_alignment;

    return hr;
}

static HRESULT WINAPI transform_SetOutputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    UINT32 channel_count, block_alignment;
    MF_ATTRIBUTE_TYPE item_type;
    ULONG i, sample_size;
    GUID major, subtype;
    HRESULT hr;

    TRACE("iface %p, id %lu, type %p, flags %#lx.\n", iface, id, type, flags);

    if (IsEqualGUID(&decoder->input_type.majortype, &GUID_NULL))
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major)) ||
        FAILED(hr = IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return hr;

    if (!IsEqualGUID(&major, &MFMediaType_Audio))
        return MF_E_INVALIDMEDIATYPE;

    for (i = 0; i < ARRAY_SIZE(wma_decoder_output_types); ++i)
        if (IsEqualGUID(&subtype, wma_decoder_output_types[i]))
            break;
    if (i == ARRAY_SIZE(wma_decoder_output_types))
        return MF_E_INVALIDMEDIATYPE;

    if (IsEqualGUID(&subtype, &MFAudioFormat_Float))
        sample_size = 32;
    else if (IsEqualGUID(&subtype, &MFAudioFormat_PCM))
        sample_size = 16;
    else
    {
        FIXME("Subtype %s not implemented!\n", debugstr_guid(&subtype));
        hr = E_NOTIMPL;
        return hr;
    }

    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &item_type)) ||
        item_type != MF_ATTRIBUTE_UINT32)
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &item_type)) ||
        item_type != MF_ATTRIBUTE_UINT32)
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &channel_count)))
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &item_type)) ||
        item_type != MF_ATTRIBUTE_UINT32)
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, &item_type)))
        return MF_E_INVALIDMEDIATYPE;
    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    MoFreeMediaType(&decoder->output_type);
    memset(&decoder->output_type, 0, sizeof(decoder->output_type));

    if (SUCCEEDED(hr = MFInitAMMediaTypeFromMFMediaType(type, GUID_NULL, &decoder->output_type)))
    {
        WAVEFORMATEX *out_wfx = (WAVEFORMATEX *)decoder->output_type.pbFormat;

        out_wfx->wBitsPerSample = sample_size;
        block_alignment = sample_size / 8 * out_wfx->nChannels;
        out_wfx->nBlockAlign = block_alignment;
        out_wfx->nAvgBytesPerSec = block_alignment * out_wfx->nSamplesPerSec;
        decoder->output_buf_size = max(8192, block_alignment * 1024);
    }

    if (decoder->winedmo_transform.handle)
    {
        WAVEFORMATEX *out_wfx = (WAVEFORMATEX *)decoder->output_type.pbFormat;
        winedmo_transform_set_output_format(decoder->winedmo_transform,
                (union winedmo_format *)out_wfx, sizeof(*out_wfx) + out_wfx->cbSize);
        hr = S_OK;
    }
    else
        hr = try_create_winedmo_transform(decoder);

    if (FAILED(hr))
        goto failed;

    return S_OK;

failed:
    MoFreeMediaType(&decoder->output_type);
    memset(&decoder->output_type, 0, sizeof(decoder->output_type));
    return hr;
}

static HRESULT WINAPI transform_GetInputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %lu, type %p.\n", iface, id, type);

    if (id)
        return MF_E_INVALIDSTREAMNUMBER;
    if (IsEqualGUID(&decoder->input_type.majortype, &GUID_NULL))
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    if (!type)
        return E_POINTER;

    return MFCreateMediaTypeFromRepresentation(AM_MEDIA_TYPE_REPRESENTATION,
            &decoder->input_type, type);
}

static HRESULT WINAPI transform_GetOutputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %lu, type %p.\n", iface, id, type);

    if (id)
        return MF_E_INVALIDSTREAMNUMBER;
    if (IsEqualGUID(&decoder->output_type.majortype, &GUID_NULL))
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    if (!type)
        return E_POINTER;

    return MFCreateMediaTypeFromRepresentation(AM_MEDIA_TYPE_REPRESENTATION,
            &decoder->output_type, type);
}

static HRESULT WINAPI transform_GetInputStatus(IMFTransform *iface, DWORD id, DWORD *flags)
{
    FIXME("iface %p, id %lu, flags %p stub!\n", iface, id, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetOutputStatus(IMFTransform *iface, DWORD *flags)
{
    FIXME("iface %p, flags %p stub!\n", iface, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_SetOutputBounds(IMFTransform *iface, LONGLONG lower, LONGLONG upper)
{
    TRACE("iface %p, lower %I64d, upper %I64d.\n", iface, lower, upper);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_ProcessEvent(IMFTransform *iface, DWORD id, IMFMediaEvent *event)
{
    FIXME("iface %p, id %lu, event %p stub!\n", iface, id, event);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_ProcessMessage(IMFTransform *iface, MFT_MESSAGE_TYPE message, ULONG_PTR param)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, message %#x, param %p.\n", iface, message, (void *)param);

    if (!decoder->winedmo_transform.handle)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (message == MFT_MESSAGE_COMMAND_DRAIN)
        return winedmo_transform_drain(decoder->winedmo_transform) ? E_FAIL : S_OK;
    if (message == MFT_MESSAGE_COMMAND_FLUSH)
        return winedmo_transform_flush(decoder->winedmo_transform) ? E_FAIL : S_OK;

    FIXME("Ignoring message %#x.\n", message);

    return S_OK;
}

static HRESULT WINAPI transform_ProcessInput(IMFTransform *iface, DWORD id, IMFSample *sample, DWORD flags)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    IMFMediaBuffer *buffer;
    MFT_INPUT_STREAM_INFO info;
    LONGLONG time, duration;
    BYTE *data;
    DWORD size, total_length, push_flags = 0;
    UINT32 value;
    NTSTATUS status;
    HRESULT hr;

    TRACE("iface %p, id %lu, sample %p, flags %#lx.\n", iface, id, sample, flags);

    if (!decoder->winedmo_transform.handle)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = IMFTransform_GetInputStreamInfo(iface, 0, &info))
            || FAILED(hr = IMFSample_GetTotalLength(sample, &total_length)))
        return hr;

    /* WMA transform uses fixed size input samples and ignores samples with invalid sizes */
    if (total_length % info.cbSize)
        return S_OK;

    if (FAILED(hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer)))
        return hr;
    if (FAILED(hr = IMFMediaBuffer_Lock(buffer, &data, NULL, &size)))
    {
        IMFMediaBuffer_Release(buffer);
        return hr;
    }

    if (FAILED(IMFSample_GetSampleTime(sample, &time))) time = INT64_MIN;
    if (FAILED(IMFSample_GetSampleDuration(sample, &duration))) duration = INT64_MIN;
    if (SUCCEEDED(IMFSample_GetUINT32(sample, &MFSampleExtension_CleanPoint, &value)) && value)
        push_flags |= WINEDMO_SAMPLE_FLAG_SYNC_POINT;
    if (SUCCEEDED(IMFSample_GetUINT32(sample, &MFSampleExtension_Discontinuity, &value)) && value)
        push_flags |= WINEDMO_SAMPLE_FLAG_DISCONTINUITY;

    status = winedmo_transform_push_input(decoder->winedmo_transform, data, size,
            time, INT64_MIN, duration, push_flags);
    IMFMediaBuffer_Unlock(buffer);
    IMFMediaBuffer_Release(buffer);

    if (status == STATUS_DEVICE_BUSY) return MF_E_NOTACCEPTING;
    return status ? E_FAIL : S_OK;
}

static HRESULT WINAPI transform_ProcessOutput(IMFTransform *iface, DWORD flags, DWORD count,
        MFT_OUTPUT_DATA_BUFFER *samples, DWORD *status)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    MFT_OUTPUT_STREAM_INFO info;
    IMFMediaBuffer *buffer;
    BYTE *data;
    UINT32 size;
    INT64 pts = INT64_MIN, duration = INT64_MIN;
    DWORD out_flags;
    NTSTATUS ntstatus;
    BOOL retried_format_change = FALSE;
    HRESULT hr;

    TRACE("iface %p, flags %#lx, count %lu, samples %p, status %p.\n", iface, flags, count, samples, status);

    if (count != 1)
        return E_INVALIDARG;

    if (!decoder->winedmo_transform.handle)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    *status = samples->dwStatus = 0;
    if (!samples->pSample)
    {
        samples[0].dwStatus = MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE;
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }

    if (FAILED(hr = IMFTransform_GetOutputStreamInfo(iface, 0, &info)))
        return hr;

    if (FAILED(hr = IMFSample_ConvertToContiguousBuffer(samples->pSample, &buffer)))
        return hr;
    if (FAILED(hr = IMFMediaBuffer_Lock(buffer, &data, NULL, NULL)))
    {
        IMFMediaBuffer_Release(buffer);
        return hr;
    }

    size = info.cbSize;

retry:
    ntstatus = winedmo_transform_get_output(decoder->winedmo_transform, data, &size,
            &pts, &duration, &out_flags);

    if (!ntstatus && (out_flags & WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED) && !retried_format_change)
    {
        union winedmo_format *new_format;
        GUID major;

        retried_format_change = TRUE;
        /* The Unix decoder reports its native output format on the first decoded frame.
         * Keep that discovery internal and immediately retry with the already-selected
         * consumer-visible output type instead of surfacing a spurious stream change. */
        if (!winedmo_transform_get_output_format(decoder->winedmo_transform, &major, &new_format))
            free(new_format);
        size = info.cbSize;
        pts = duration = INT64_MIN;
        out_flags = 0;
        goto retry;
    }

    IMFMediaBuffer_Unlock(buffer);

    if (!ntstatus && !(out_flags & WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED))
    {
        IMFMediaBuffer_SetCurrentLength(buffer, size);
        if (pts != INT64_MIN) IMFSample_SetSampleTime(samples->pSample, pts);
        if (duration != INT64_MIN) IMFSample_SetSampleDuration(samples->pSample, duration);
        /* PCM output is always independently decodable */
        IMFSample_SetUINT32(samples->pSample, &MFSampleExtension_CleanPoint, 1);
        if (out_flags & WINEDMO_SAMPLE_FLAG_INCOMPLETE)
            samples->dwStatus |= MFT_OUTPUT_DATA_BUFFER_INCOMPLETE;
        hr = S_OK;
    }
    else if (!ntstatus && (out_flags & WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED))
    {
        samples->dwStatus = MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
        hr = MF_E_TRANSFORM_STREAM_CHANGE;
    }
    else if (ntstatus == STATUS_MORE_PROCESSING_REQUIRED)
    {
        samples->dwStatus = MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE;
        hr = MF_E_TRANSFORM_NEED_MORE_INPUT;
    }
    else if (ntstatus == STATUS_END_OF_FILE)
    {
        samples->dwStatus = MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE;
        hr = MF_E_END_OF_STREAM;
    }
    else
    {
        samples->dwStatus = MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE;
        hr = E_FAIL;
    }

    IMFMediaBuffer_Release(buffer);
    return hr;
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

static inline struct wma_decoder *impl_from_IMediaObject(IMediaObject *iface)
{
    return CONTAINING_RECORD(iface, struct wma_decoder, IMediaObject_iface);
}

static HRESULT WINAPI media_object_QueryInterface(IMediaObject *iface, REFIID iid, void **obj)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);
    return IUnknown_QueryInterface(decoder->outer, iid, obj);
}

static ULONG WINAPI media_object_AddRef(IMediaObject *iface)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);
    return IUnknown_AddRef(decoder->outer);
}

static ULONG WINAPI media_object_Release(IMediaObject *iface)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);
    return IUnknown_Release(decoder->outer);
}

static HRESULT WINAPI media_object_GetStreamCount(IMediaObject *iface, DWORD *input, DWORD *output)
{
    FIXME("iface %p, input %p, output %p semi-stub!\n", iface, input, output);
    *input = *output = 1;
    return S_OK;
}

static HRESULT WINAPI media_object_GetInputStreamInfo(IMediaObject *iface, DWORD index, DWORD *flags)
{
    FIXME("iface %p, index %lu, flags %p stub!\n", iface, index, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_GetOutputStreamInfo(IMediaObject *iface, DWORD index, DWORD *flags)
{
    FIXME("iface %p, index %lu, flags %p stub!\n", iface, index, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_GetInputType(IMediaObject *iface, DWORD index, DWORD type_index,
        DMO_MEDIA_TYPE *type)
{
    TRACE("iface %p, index %lu, type_index %lu, type %p.\n", iface, index, type_index, type);

    if (index > 0)
        return DMO_E_INVALIDSTREAMINDEX;
    if (type_index >= ARRAY_SIZE(wma_decoder_input_types))
        return DMO_E_NO_MORE_ITEMS;
    if (!type)
        return S_OK;

    memset(type, 0, sizeof(*type));
    type->majortype = MFMediaType_Audio;
    type->subtype = *wma_decoder_input_types[type_index];
    type->bFixedSizeSamples = FALSE;
    type->bTemporalCompression = TRUE;
    type->lSampleSize = 0;

    return S_OK;
}

static HRESULT WINAPI media_object_GetOutputType(IMediaObject *iface, DWORD index, DWORD type_index,
        DMO_MEDIA_TYPE *type)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);
    UINT32 depth, channels, rate;
    IMFMediaType *media_type;
    HRESULT hr;

    TRACE("iface %p, index %lu, type_index %lu, type %p\n", iface, index, type_index, type);

    if (index > 0)
        return DMO_E_INVALIDSTREAMINDEX;
    if (type_index >= 1)
        return DMO_E_NO_MORE_ITEMS;
    if (IsEqualGUID(&decoder->input_type.majortype, &GUID_NULL))
        return DMO_E_TYPE_NOT_SET;
    if (!type)
        return S_OK;

    if (FAILED(hr = MFCreateMediaTypeFromRepresentation(AM_MEDIA_TYPE_REPRESENTATION,
            &decoder->input_type, &media_type)))
        return hr;

    if (SUCCEEDED(IMFMediaType_GetUINT32(media_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &depth))
            && depth == 32)
    {
        hr = IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFAudioFormat_Float);
    }
    else
    {
        /* Compressed WMA reports the source depth in WAVEFORMATEX, but that is
         * not a requirement for the decoded PCM output. Prefer the standard
         * 16-bit PCM output type unless the caller explicitly requests another
         * supported type with SetOutputType(). */
        depth = 16;
        hr = IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    }

    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, depth);

    if (SUCCEEDED(hr))
        hr = IMFMediaType_GetUINT32(media_type, &MF_MT_AUDIO_NUM_CHANNELS, &channels);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, depth * channels / 8);

    if (SUCCEEDED(hr))
        hr = IMFMediaType_GetUINT32(media_type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, depth * channels / 8 * rate);

    if (SUCCEEDED(hr))
        hr = IMFMediaType_DeleteItem(media_type, &MF_MT_USER_DATA);
    if (SUCCEEDED(hr))
        hr = MFInitAMMediaTypeFromMFMediaType(media_type, GUID_NULL, type);

    IMFMediaType_Release(media_type);
    return hr;
}

static HRESULT WINAPI media_object_SetInputType(IMediaObject *iface, DWORD index,
        const DMO_MEDIA_TYPE *type, DWORD flags)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);

    TRACE("iface %p, index %lu, type %p, flags %#lx.\n", iface, index, type, flags);

    if (index > 0)
        return DMO_E_INVALIDSTREAMINDEX;

    if (flags & DMO_SET_TYPEF_CLEAR)
    {
        if (flags != DMO_SET_TYPEF_CLEAR)
            return E_INVALIDARG;
        MoFreeMediaType(&decoder->input_type);
        memset(&decoder->input_type, 0, sizeof(decoder->input_type));
        if (decoder->winedmo_transform.handle)
        {
            winedmo_transform_destroy(decoder->winedmo_transform);
            decoder->winedmo_transform.handle = 0;
        }
        return S_OK;
    }
    if (!type)
        return E_POINTER;
    if (flags & ~DMO_SET_TYPEF_TEST_ONLY)
        return E_INVALIDARG;

    if (!is_valid_wma_decoder_input_type(type))
        return DMO_E_TYPE_NOT_ACCEPTED;

    if (flags & DMO_SET_TYPEF_TEST_ONLY)
        return S_OK;

    MoFreeMediaType(&decoder->input_type);
    memset(&decoder->input_type, 0, sizeof(decoder->input_type));
    MoCopyMediaType(&decoder->input_type, type);

    if (decoder->winedmo_transform.handle)
    {
        winedmo_transform_destroy(decoder->winedmo_transform);
        decoder->winedmo_transform.handle = 0;
    }

    return S_OK;
}

static HRESULT WINAPI media_object_SetOutputType(IMediaObject *iface, DWORD index,
        const DMO_MEDIA_TYPE *type, DWORD flags)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);
    unsigned int i;
    HRESULT hr;

    TRACE("iface %p, index %lu, type %p, flags %#lx,\n", iface, index, type, flags);

    if (index > 0)
        return DMO_E_INVALIDSTREAMINDEX;

    if (flags & DMO_SET_TYPEF_CLEAR)
    {
        if (flags != DMO_SET_TYPEF_CLEAR)
            return E_INVALIDARG;
        MoFreeMediaType(&decoder->output_type);
        memset(&decoder->output_type, 0, sizeof(decoder->output_type));
        if (decoder->winedmo_transform.handle)
        {
            winedmo_transform_destroy(decoder->winedmo_transform);
            decoder->winedmo_transform.handle = 0;
        }
        return S_OK;
    }
    if (!type)
        return E_POINTER;
    if (flags & ~DMO_SET_TYPEF_TEST_ONLY)
        return E_INVALIDARG;

    if (!IsEqualGUID(&type->majortype, &MEDIATYPE_Audio))
        return DMO_E_TYPE_NOT_ACCEPTED;

    for (i = 0; i < ARRAY_SIZE(wma_decoder_output_types); ++i)
        if (IsEqualGUID(&type->subtype, wma_decoder_output_types[i]))
            break;
    if (i == ARRAY_SIZE(wma_decoder_output_types))
        return DMO_E_TYPE_NOT_ACCEPTED;

    if (IsEqualGUID(&decoder->input_type.majortype, &GUID_NULL))
        return DMO_E_TYPE_NOT_SET;
    if (flags & DMO_SET_TYPEF_TEST_ONLY)
        return S_OK;

    MoFreeMediaType(&decoder->output_type);
    memset(&decoder->output_type, 0, sizeof(decoder->output_type));
    MoCopyMediaType(&decoder->output_type, type);

    if (FAILED(hr = try_create_winedmo_transform(decoder)))
        return hr;

    return S_OK;
}

static HRESULT WINAPI media_object_GetInputCurrentType(IMediaObject *iface, DWORD index, DMO_MEDIA_TYPE *type)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);

    TRACE("iface %p, index %lu, type %p\n", iface, index, type);

    if (index)
        return DMO_E_INVALIDSTREAMINDEX;
    if (IsEqualGUID(&decoder->input_type.majortype, &GUID_NULL))
        return DMO_E_TYPE_NOT_SET;
    if (!type)
        return E_POINTER;
    return MoCopyMediaType(type, &decoder->input_type);
}

static HRESULT WINAPI media_object_GetOutputCurrentType(IMediaObject *iface, DWORD index, DMO_MEDIA_TYPE *type)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);

    TRACE("iface %p, index %lu, type %p\n", iface, index, type);

    if (index)
        return DMO_E_INVALIDSTREAMINDEX;
    if (IsEqualGUID(&decoder->output_type.majortype, &GUID_NULL))
        return DMO_E_TYPE_NOT_SET;
    if (!type)
        return E_POINTER;
    return MoCopyMediaType(type, &decoder->output_type);
}

static HRESULT WINAPI media_object_GetInputSizeInfo(IMediaObject *iface, DWORD index, DWORD *size,
        DWORD *lookahead, DWORD *alignment)
{
    FIXME("iface %p, index %lu, size %p, lookahead %p, alignment %p stub!\n", iface, index, size,
            lookahead, alignment);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_GetOutputSizeInfo(IMediaObject *iface, DWORD index, DWORD *size, DWORD *alignment)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);

    TRACE("iface %p, index %lu, size %p, alignment %p.\n", iface, index, size, alignment);

    if (!size || !alignment)
        return E_POINTER;
    if (index > 0)
        return DMO_E_INVALIDSTREAMINDEX;
    if (IsEqualGUID(&decoder->output_type.majortype, &GUID_NULL))
        return DMO_E_TYPE_NOT_SET;

    *size = 8192;
    *alignment = 1;

    return S_OK;
}

static HRESULT WINAPI media_object_GetInputMaxLatency(IMediaObject *iface, DWORD index, REFERENCE_TIME *latency)
{
    FIXME("iface %p, index %lu, latency %p stub!\n", iface, index, latency);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_SetInputMaxLatency(IMediaObject *iface, DWORD index, REFERENCE_TIME latency)
{
    FIXME("iface %p, index %lu, latency %s stub!\n", iface, index, wine_dbgstr_longlong(latency));
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_Flush(IMediaObject *iface)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);

    TRACE("iface %p.\n", iface);

    if (!decoder->winedmo_transform.handle)
        return DMO_E_TYPE_NOT_SET;

    if (winedmo_transform_flush(decoder->winedmo_transform))
        return E_FAIL;

    return S_OK;
}

static HRESULT WINAPI media_object_Discontinuity(IMediaObject *iface, DWORD index)
{
    TRACE("iface %p, index %lu.\n", iface, index);

    if (index > 0)
        return DMO_E_INVALIDSTREAMINDEX;

    return S_OK;
}

static HRESULT WINAPI media_object_AllocateStreamingResources(IMediaObject *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_FreeStreamingResources(IMediaObject *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_GetInputStatus(IMediaObject *iface, DWORD index, DWORD *flags)
{
    FIXME("iface %p, index %lu, flags %p stub!\n", iface, index, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_ProcessInput(IMediaObject *iface, DWORD index,
        IMediaBuffer *buffer, DWORD flags, REFERENCE_TIME timestamp, REFERENCE_TIME timelength)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);
    BYTE *data;
    DWORD size;
    DWORD push_flags = 0;
    NTSTATUS status;
    HRESULT hr;

    TRACE("iface %p, index %lu, buffer %p, flags %#lx, timestamp %s, timelength %s.\n", iface,
             index, buffer, flags, wine_dbgstr_longlong(timestamp), wine_dbgstr_longlong(timelength));

    if (!decoder->winedmo_transform.handle)
        return DMO_E_TYPE_NOT_SET;

    if (FAILED(hr = IMediaBuffer_GetBufferAndLength(buffer, &data, &size)))
        return hr;

    if (flags & DMO_INPUT_DATA_BUFFERF_SYNCPOINT) push_flags |= WINEDMO_SAMPLE_FLAG_SYNC_POINT;
    if (flags & DMO_INPUT_DATA_BUFFERF_DISCONTINUITY) push_flags |= WINEDMO_SAMPLE_FLAG_DISCONTINUITY;

    status = winedmo_transform_push_input(decoder->winedmo_transform, data, size,
            (flags & DMO_INPUT_DATA_BUFFERF_TIME) ? timestamp : INT64_MIN,
            INT64_MIN,
            (flags & DMO_INPUT_DATA_BUFFERF_TIMELENGTH) ? timelength : INT64_MIN,
            push_flags);
    if (status == STATUS_DEVICE_BUSY) return S_FALSE;
    return status ? E_FAIL : S_OK;
}

static HRESULT WINAPI media_object_ProcessOutput(IMediaObject *iface, DWORD flags, DWORD count,
        DMO_OUTPUT_DATA_BUFFER *buffers, DWORD *status)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);
    IMediaBuffer *ibuffer = buffers[0].pBuffer;
    BYTE *data;
    DWORD max_size;
    UINT32 out_size;
    INT64 pts, duration;
    DWORD out_flags;
    NTSTATUS ntstatus;
    BOOL retried_format_change = FALSE;
    HRESULT hr;

    TRACE("iface %p, flags %#lx, count %lu, buffers %p, status %p.\n", iface, flags, count, buffers, status);

    if (!decoder->winedmo_transform.handle)
        return DMO_E_TYPE_NOT_SET;

    *status = buffers[0].dwStatus = 0;

    if (FAILED(hr = IMediaBuffer_GetMaxLength(ibuffer, &max_size)))
        return hr;
    if (FAILED(hr = IMediaBuffer_GetBufferAndLength(ibuffer, &data, NULL)))
        return hr;

    out_size = max_size;

retry:
    ntstatus = winedmo_transform_get_output(decoder->winedmo_transform, data, &out_size,
            &pts, &duration, &out_flags);

    if (!ntstatus && (out_flags & WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED) && !retried_format_change)
    {
        union winedmo_format *new_format;
        GUID major;

        retried_format_change = TRUE;
        if (!winedmo_transform_get_output_format(decoder->winedmo_transform, &major, &new_format))
            free(new_format);
        out_size = max_size;
        pts = duration = INT64_MIN;
        out_flags = 0;
        goto retry;
    }

    if (!ntstatus && !(out_flags & WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED))
    {
        IMediaBuffer_SetLength(ibuffer, out_size);
        if (out_flags & WINEDMO_SAMPLE_FLAG_INCOMPLETE)
            buffers[0].dwStatus = DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE;
        if (pts != INT64_MIN)
        {
            buffers[0].rtTimestamp = pts;
            buffers[0].dwStatus |= DMO_OUTPUT_DATA_BUFFERF_TIME;
        }
        if (duration != INT64_MIN)
        {
            buffers[0].rtTimelength = duration;
            buffers[0].dwStatus |= DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH;
        }
        return S_OK;
    }
    else if (ntstatus == STATUS_MORE_PROCESSING_REQUIRED)
        return S_FALSE;

    return E_FAIL;
}

static HRESULT WINAPI media_object_Lock(IMediaObject *iface, LONG lock)
{
    FIXME("iface %p, lock %ld stub!\n", iface, lock);
    return E_NOTIMPL;
}

static const IMediaObjectVtbl media_object_vtbl =
{
    media_object_QueryInterface,
    media_object_AddRef,
    media_object_Release,
    media_object_GetStreamCount,
    media_object_GetInputStreamInfo,
    media_object_GetOutputStreamInfo,
    media_object_GetInputType,
    media_object_GetOutputType,
    media_object_SetInputType,
    media_object_SetOutputType,
    media_object_GetInputCurrentType,
    media_object_GetOutputCurrentType,
    media_object_GetInputSizeInfo,
    media_object_GetOutputSizeInfo,
    media_object_GetInputMaxLatency,
    media_object_SetInputMaxLatency,
    media_object_Flush,
    media_object_Discontinuity,
    media_object_AllocateStreamingResources,
    media_object_FreeStreamingResources,
    media_object_GetInputStatus,
    media_object_ProcessInput,
    media_object_ProcessOutput,
    media_object_Lock,
};

static inline struct wma_decoder *impl_from_IPropertyBag(IPropertyBag *iface)
{
    return CONTAINING_RECORD(iface, struct wma_decoder, IPropertyBag_iface);
}

static HRESULT WINAPI property_bag_QueryInterface(IPropertyBag *iface, REFIID iid, void **out)
{
    struct wma_decoder *filter = impl_from_IPropertyBag(iface);
    return IUnknown_QueryInterface(filter->outer, iid, out);
}

static ULONG WINAPI property_bag_AddRef(IPropertyBag *iface)
{
    struct wma_decoder *filter = impl_from_IPropertyBag(iface);
    return IUnknown_AddRef(filter->outer);
}

static ULONG WINAPI property_bag_Release(IPropertyBag *iface)
{
    struct wma_decoder *filter = impl_from_IPropertyBag(iface);
    return IUnknown_Release(filter->outer);
}

static HRESULT WINAPI property_bag_Read(IPropertyBag *iface, const WCHAR *prop_name, VARIANT *value,
        IErrorLog *error_log)
{
    FIXME("iface %p, prop_name %s, value %p, error_log %p stub!\n", iface, debugstr_w(prop_name), value, error_log);
    return E_NOTIMPL;
}

static HRESULT WINAPI property_bag_Write(IPropertyBag *iface, const WCHAR *prop_name, VARIANT *value)
{
    FIXME("iface %p, prop_name %s, value %p stub!\n", iface, debugstr_w(prop_name), value);
    return S_OK;
}

static const IPropertyBagVtbl property_bag_vtbl =
{
    property_bag_QueryInterface,
    property_bag_AddRef,
    property_bag_Release,
    property_bag_Read,
    property_bag_Write,
};

HRESULT wma_decoder_create(IUnknown *outer, IUnknown **out)
{
    struct wma_decoder *decoder;

    TRACE("outer %p, out %p.\n", outer, out);

    if (!(decoder = calloc(1, sizeof(*decoder))))
        return E_OUTOFMEMORY;

    decoder->IUnknown_inner.lpVtbl = &unknown_vtbl;
    decoder->IMFTransform_iface.lpVtbl = &transform_vtbl;
    decoder->IMediaObject_iface.lpVtbl = &media_object_vtbl;
    decoder->IPropertyBag_iface.lpVtbl = &property_bag_vtbl;
    decoder->refcount = 1;
    decoder->outer = outer ? outer : &decoder->IUnknown_inner;

    *out = &decoder->IUnknown_inner;
    TRACE("Created decoder %p\n", *out);
    return S_OK;
}
