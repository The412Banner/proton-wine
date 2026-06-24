/* Audio Decoder Transform
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
#include "ks.h"
#include "ksmedia.h"

#include "wine/debug.h"
#include "wine/winedmo.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

#define CBSIZE(x)       (sizeof(x) - sizeof(WAVEFORMATEX))

#define NEXT_WAVEFORMATEXTENSIBLE(format) (WAVEFORMATEXTENSIBLE *)((BYTE *)(&(format)->Format + 1) + (format)->Format.cbSize)

static WAVEFORMATEXTENSIBLE const audio_decoder_output_types[] =
{
    {.Format = {.wFormatTag = WAVE_FORMAT_PCM, .wBitsPerSample = 16, .nSamplesPerSec = 48000, .nChannels = 2,
                .cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)}},
    {.Format = {.wFormatTag = WAVE_FORMAT_IEEE_FLOAT, .wBitsPerSample = 32, .nSamplesPerSec = 48000, .nChannels = 2,
                .cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)}},
};

static const UINT32 default_channel_mask[7] =
{
    0,
    SPEAKER_FRONT_CENTER,
    SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT,
    SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_CENTER,
    KSAUDIO_SPEAKER_QUAD,
    KSAUDIO_SPEAKER_QUAD | SPEAKER_FRONT_CENTER,
    KSAUDIO_SPEAKER_5POINT1,
};

static inline UINT32 wave_format_size(const WAVEFORMATEX *format)
{
    return sizeof(*format) + format->cbSize;
}

struct audio_decoder
{
    IMFTransform IMFTransform_iface;
    LONG refcount;

    UINT input_type_count;
    WAVEFORMATEXTENSIBLE *input_types;

    IMFMediaType *input_type;
    IMFMediaType *output_type;

    struct winedmo_transform winedmo_transform;
};

static struct audio_decoder *impl_from_IMFTransform(IMFTransform *iface)
{
    return CONTAINING_RECORD(iface, struct audio_decoder, IMFTransform_iface);
}

static HRESULT try_create_winedmo_transform(struct audio_decoder *decoder)
{
    WAVEFORMATEX *input_format, *output_format;
    UINT32 input_size, output_size;
    NTSTATUS status;
    HRESULT hr;

    if (decoder->winedmo_transform.handle)
    {
        winedmo_transform_destroy(decoder->winedmo_transform);
        decoder->winedmo_transform.handle = 0;
    }

    if (FAILED(hr = MFCreateWaveFormatExFromMFMediaType(decoder->input_type,
            &input_format, &input_size, MFWaveFormatExConvertFlag_ForceExtensible)))
        return hr;

    if (FAILED(hr = MFCreateWaveFormatExFromMFMediaType(decoder->output_type,
            &output_format, &output_size, MFWaveFormatExConvertFlag_ForceExtensible)))
    {
        CoTaskMemFree(input_format);
        return hr;
    }

    status = winedmo_transform_create(MFMediaType_Audio,
            (union winedmo_format *)input_format, input_size,
            (union winedmo_format *)output_format, output_size,
            &decoder->winedmo_transform);
    CoTaskMemFree(input_format);
    CoTaskMemFree(output_format);

    if (status) WARN("winedmo_transform_create failed, status %#lx\n", status);
    return status ? E_FAIL : S_OK;
}

static HRESULT WINAPI transform_QueryInterface(IMFTransform *iface, REFIID iid, void **out)
{
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMFTransform))
        *out = &decoder->IMFTransform_iface;
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
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);
    ULONG refcount = InterlockedIncrement(&decoder->refcount);
    TRACE("iface %p increasing refcount to %lu.\n", decoder, refcount);
    return refcount;
}

static ULONG WINAPI transform_Release(IMFTransform *iface)
{
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);
    ULONG refcount = InterlockedDecrement(&decoder->refcount);

    TRACE("iface %p decreasing refcount to %lu.\n", decoder, refcount);

    if (!refcount)
    {
        if (decoder->winedmo_transform.handle)
            winedmo_transform_destroy(decoder->winedmo_transform);
        if (decoder->input_type)
            IMFMediaType_Release(decoder->input_type);
        if (decoder->output_type)
            IMFMediaType_Release(decoder->output_type);
        free(decoder);
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
    TRACE("iface %p, input_size %lu, inputs %p, output_size %lu, outputs %p.\n", iface,
            input_size, inputs, output_size, outputs);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetInputStreamInfo(IMFTransform *iface, DWORD id, MFT_INPUT_STREAM_INFO *info)
{
    TRACE("iface %p, id %#lx, info %p.\n", iface, id, info);

    if (id)
        return MF_E_INVALIDSTREAMNUMBER;

    memset(info, 0, sizeof(*info));
    info->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES | MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER
            | MFT_INPUT_STREAM_FIXED_SAMPLE_SIZE | MFT_INPUT_STREAM_HOLDS_BUFFERS;

    return S_OK;
}

static HRESULT WINAPI transform_GetOutputStreamInfo(IMFTransform *iface, DWORD id, MFT_OUTPUT_STREAM_INFO *info)
{
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);
    UINT32 bytes_per_sec = 0;

    TRACE("iface %p, id %#lx, info %p.\n", iface, id, info);

    if (id)
        return MF_E_INVALIDSTREAMNUMBER;

    memset(info, 0, sizeof(*info));
    info->dwFlags = MFT_OUTPUT_STREAM_WHOLE_SAMPLES;

    if (decoder->output_type)
        IMFMediaType_GetUINT32(decoder->output_type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes_per_sec);

    /* Return ~100 ms worth of decoded PCM per buffer so the caller gets
     * a small stream of manageable chunks rather than one monolithic blob. */
    info->cbSize = bytes_per_sec ? max(bytes_per_sec / 10, 4096u) : 4096;

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
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);
    const WAVEFORMATEXTENSIBLE *format = decoder->input_types;
    UINT count = decoder->input_type_count;

    TRACE("iface %p, id %#lx, index %#lx, type %p.\n", iface, id, index, type);

    *type = NULL;
    if (id)
        return MF_E_INVALIDSTREAMNUMBER;
    for (format = decoder->input_types; index > 0 && count > 0; index--, count--)
        format = NEXT_WAVEFORMATEXTENSIBLE(format);
    return count ? MFCreateAudioMediaType(&format->Format, (IMFAudioMediaType **)type) : MF_E_NO_MORE_TYPES;
}

static HRESULT WINAPI transform_GetOutputAvailableType(IMFTransform *iface, DWORD id, DWORD index,
        IMFMediaType **type)
{
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);
    UINT32 input_channel_count, channel_count, sample_rate;
    BOOL stereo_fallback = FALSE;
    WAVEFORMATEXTENSIBLE wfx = {{0}};
    IMFMediaType *media_type;
    HRESULT hr;

    TRACE("iface %p, id %#lx, index %#lx, type %p.\n", iface, id, index, type);

    *type = NULL;

    if (id)
        return MF_E_INVALIDSTREAMNUMBER;
    if (!decoder->input_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = IMFMediaType_GetUINT32(decoder->input_type, &MF_MT_AUDIO_NUM_CHANNELS, &input_channel_count))
            || !input_channel_count)
        input_channel_count = 2;
    if (FAILED(hr = IMFMediaType_GetUINT32(decoder->input_type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate)))
        sample_rate = 48000;

    if (input_channel_count >= ARRAY_SIZE(default_channel_mask))
        return MF_E_INVALIDMEDIATYPE;

    channel_count = input_channel_count;

    if (input_channel_count > 2 && index >= ARRAY_SIZE(audio_decoder_output_types))
    {
        /* Advertise native multichannel formats first. Some games set a
         * partially specified output type and expect the reader to preserve
         * the source channel layout rather than downmixing immediately;
         * The Medium's C001_51.mp4 audio distorts if stereo fallback wins
         * before the native 5.1 AAC output type is offered. */
        index -= ARRAY_SIZE(audio_decoder_output_types);
        channel_count = 2;
        stereo_fallback = TRUE;
    }

    if (index >= ARRAY_SIZE(audio_decoder_output_types))
        return MF_E_NO_MORE_TYPES;

    wfx = audio_decoder_output_types[index % ARRAY_SIZE(audio_decoder_output_types)];
    wfx.Format.nChannels = channel_count;
    wfx.Format.nSamplesPerSec = sample_rate;
    wfx.Format.nBlockAlign = wfx.Format.wBitsPerSample * wfx.Format.nChannels / 8;
    wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;

    if (wfx.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT || wfx.Format.nChannels >= 3)
    {
        wfx.SubFormat = MFAudioFormat_Base;
        wfx.SubFormat.Data1 = wfx.Format.wFormatTag;
        wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfx.dwChannelMask = default_channel_mask[wfx.Format.nChannels];
    }
    else
        wfx.Format.cbSize = 0;

    if (FAILED(hr = MFCreateAudioMediaType(&wfx.Format, (IMFAudioMediaType **)&media_type)))
        return hr;
    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_FIXED_SIZE_SAMPLES, 1)))
        goto done;

    TRACE("Advertising %s output type index %#lx as %lu ch %lu Hz %s.\n",
            stereo_fallback ? "stereo fallback" : "native",
            index, (DWORD)wfx.Format.nChannels, (DWORD)wfx.Format.nSamplesPerSec,
            wfx.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(&wfx.SubFormat, &MFAudioFormat_Float)
                ? "float" : (wfx.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ? "float" : "pcm"));

done:
    if (SUCCEEDED(hr))
        IMFMediaType_AddRef((*type = media_type));

    IMFMediaType_Release(media_type);
    return hr;
}

static BOOL matches_format(const WAVEFORMATEXTENSIBLE *a, const WAVEFORMATEXTENSIBLE *b)
{
    if (a->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && b->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        return IsEqualGUID(&a->SubFormat, &b->SubFormat);
    if (a->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        return a->SubFormat.Data1 == b->Format.wFormatTag;
    if (b->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        return b->SubFormat.Data1 == a->Format.wFormatTag;
    return a->Format.wFormatTag == b->Format.wFormatTag;
}

static HRESULT WINAPI transform_SetInputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);
    UINT32 size, count = decoder->input_type_count;
    WAVEFORMATEXTENSIBLE *format, wfx;
    HRESULT hr;

    TRACE("iface %p, id %#lx, type %p, flags %#lx.\n", iface, id, type, flags);

    if (id)
        return MF_E_INVALIDSTREAMNUMBER;

    if (!type)
    {
        if (decoder->input_type)
        {
            IMFMediaType_Release(decoder->input_type);
            decoder->input_type = NULL;
        }
        if (decoder->output_type)
        {
            IMFMediaType_Release(decoder->output_type);
            decoder->output_type = NULL;
        }
        if (decoder->winedmo_transform.handle)
        {
            winedmo_transform_destroy(decoder->winedmo_transform);
            decoder->winedmo_transform.handle = 0;
        }

        return S_OK;
    }

    if (FAILED(hr = MFCreateWaveFormatExFromMFMediaType(type, (WAVEFORMATEX **)&format, &size,
            MFWaveFormatExConvertFlag_ForceExtensible)))
        return hr;
    wfx = *format;
    CoTaskMemFree(format);

    for (format = decoder->input_types; count > 0 && !matches_format(format, &wfx); count--)
        format = NEXT_WAVEFORMATEXTENSIBLE(format);
    if (!count)
        return MF_E_INVALIDMEDIATYPE;

    if (wfx.Format.nChannels >= ARRAY_SIZE(default_channel_mask) || !wfx.Format.nSamplesPerSec
            /* 2 is the minimum size of AudioSpecificConfig() */
            || wfx.Format.cbSize < 2 + CBSIZE(WAVEFORMATEXTENSIBLE))
        return MF_E_INVALIDMEDIATYPE;
    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    if (!decoder->input_type && FAILED(hr = MFCreateMediaType(&decoder->input_type)))
        return hr;

    if (decoder->output_type)
    {
        IMFMediaType_Release(decoder->output_type);
        decoder->output_type = NULL;
    }

    return IMFMediaType_CopyAllItems(type, (IMFAttributes *)decoder->input_type);
}

static HRESULT WINAPI transform_SetOutputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);
    WAVEFORMATEXTENSIBLE *format, wfx;
    UINT32 size;
    HRESULT hr;
    ULONG i;

    TRACE("iface %p, id %#lx, type %p, flags %#lx.\n", iface, id, type, flags);

    if (id)
        return MF_E_INVALIDSTREAMNUMBER;

    if (!type)
    {
        if (decoder->output_type)
        {
            IMFMediaType_Release(decoder->output_type);
            decoder->output_type = NULL;
        }
        if (decoder->winedmo_transform.handle)
        {
            winedmo_transform_destroy(decoder->winedmo_transform);
            decoder->winedmo_transform.handle = 0;
        }

        return S_OK;
    }

    if (!decoder->input_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = MFCreateWaveFormatExFromMFMediaType(type, (WAVEFORMATEX **)&format, &size,
            MFWaveFormatExConvertFlag_ForceExtensible)))
        return hr;
    wfx = *format;
    CoTaskMemFree(format);

    for (i = 0; i < ARRAY_SIZE(audio_decoder_output_types); ++i)
        if (matches_format(&audio_decoder_output_types[i], &wfx))
            break;
    if (i == ARRAY_SIZE(audio_decoder_output_types))
        return MF_E_INVALIDMEDIATYPE;

    if (!wfx.Format.nChannels || !wfx.Format.nSamplesPerSec)
        return MF_E_INVALIDMEDIATYPE;
    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    {
        /* Start from the clean type template to avoid carrying over attributes from the
         * upstream compressed format (e.g. 4-bit ADPCM bits-per-sample, mono channel mask)
         * that update_media_type_from_upstream may have injected into the output type.
         * Use cbSize=0 to produce a plain WAVEFORMATEX without the EXTENSIBLE extension
         * bytes; games use the returned WFX to configure XAudio2/dsound and a spurious
         * 22-byte zero extension (from the WAVEFORMATEXTENSIBLE template) confuses them.
         * Float output is different: keep it extensible so the channel mask / subtype stay
         * explicit all the way into the Unix decoder and renderer path. */
        UINT32 channel_count = wfx.Format.nChannels;
        UINT32 sample_rate   = wfx.Format.nSamplesPerSec;
        wfx = audio_decoder_output_types[i];
        /* Preserve the requested channel count and sample rate from the output type.
         * Games that read PCM directly from the source reader use this format to configure
         * their audio engine and expect native format (e.g. mono 44100 Hz for ADPCM files).
         * The SAR requests its preferred mix format (typically 2ch/48000 Hz) explicitly via
         * SetOutputType, so no forced normalization is needed here. */
        wfx.Format.nChannels      = channel_count;
        wfx.Format.nSamplesPerSec = sample_rate;
        if (wfx.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT || channel_count >= 3)
        {
            wfx.SubFormat = MFAudioFormat_Base;
            wfx.SubFormat.Data1 = wfx.Format.wFormatTag;
            wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            wfx.Samples.wValidBitsPerSample = wfx.Format.wBitsPerSample;
            wfx.dwChannelMask = default_channel_mask[channel_count];
        }
        else
            wfx.Format.cbSize = 0;
    }

    wfx.Format.nBlockAlign = wfx.Format.wBitsPerSample * wfx.Format.nChannels / 8;
    wfx.Format.nAvgBytesPerSec = wfx.Format.nBlockAlign * wfx.Format.nSamplesPerSec;

    if (decoder->output_type)
    {
        IMFMediaType_Release(decoder->output_type);
        decoder->output_type = NULL;
    }
    if (FAILED(hr = MFCreateAudioMediaType(&wfx.Format, (IMFAudioMediaType **)&decoder->output_type)))
        return hr;

    if (decoder->winedmo_transform.handle)
    {
        winedmo_transform_set_output_format(decoder->winedmo_transform,
                (union winedmo_format *)&wfx.Format, wave_format_size(&wfx.Format));
        hr = S_OK;
    }
    else
        hr = try_create_winedmo_transform(decoder);

    if (FAILED(hr))
    {
        IMFMediaType_Release(decoder->output_type);
        decoder->output_type = NULL;
    }
    return hr;
}

static HRESULT WINAPI transform_GetInputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **out)
{
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);
    IMFMediaType *type;
    HRESULT hr;

    TRACE("iface %p, id %#lx, out %p.\n", iface, id, out);

    if (id)
        return MF_E_INVALIDSTREAMNUMBER;
    if (!decoder->input_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = MFCreateMediaType(&type)))
        return hr;
    if (SUCCEEDED(hr = IMFMediaType_CopyAllItems(decoder->input_type, (IMFAttributes *)type)))
        IMFMediaType_AddRef(*out = type);
    IMFMediaType_Release(type);

    return hr;
}

static HRESULT WINAPI transform_GetOutputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **out)
{
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);
    IMFMediaType *type;
    HRESULT hr;

    TRACE("iface %p, id %#lx, out %p.\n", iface, id, out);

    if (id)
        return MF_E_INVALIDSTREAMNUMBER;
    if (!decoder->output_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = MFCreateMediaType(&type)))
        return hr;
    if (SUCCEEDED(hr = IMFMediaType_CopyAllItems(decoder->output_type, (IMFAttributes *)type)))
        IMFMediaType_AddRef(*out = type);
    IMFMediaType_Release(type);

    return hr;
}

static HRESULT WINAPI transform_GetInputStatus(IMFTransform *iface, DWORD id, DWORD *flags)
{
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %#lx, flags %p.\n", iface, id, flags);

    if (!decoder->winedmo_transform.handle)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    *flags = MFT_INPUT_STATUS_ACCEPT_DATA;
    return S_OK;
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
    FIXME("iface %p, id %#lx, event %p stub!\n", iface, id, event);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_ProcessMessage(IMFTransform *iface, MFT_MESSAGE_TYPE message, ULONG_PTR param)
{
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, message %#x, param %Ix.\n", iface, message, param);

    switch (message)
    {
    case MFT_MESSAGE_COMMAND_DRAIN:
        return winedmo_transform_drain(decoder->winedmo_transform) ? E_FAIL : S_OK;

    case MFT_MESSAGE_COMMAND_FLUSH:
        return winedmo_transform_flush(decoder->winedmo_transform) ? E_FAIL : S_OK;

    default:
        FIXME("Ignoring message %#x.\n", message);
        return S_OK;
    }
}

static HRESULT WINAPI transform_ProcessInput(IMFTransform *iface, DWORD id, IMFSample *sample, DWORD flags)
{
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);
    IMFMediaBuffer *buffer;
    LONGLONG time, duration;
    BYTE *data;
    DWORD size, push_flags = 0;
    UINT32 value;
    NTSTATUS status;
    HRESULT hr;

    TRACE("iface %p, id %#lx, sample %p, flags %#lx.\n", iface, id, sample, flags);

    if (!decoder->winedmo_transform.handle)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

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
    struct audio_decoder *decoder = impl_from_IMFTransform(iface);
    IMFMediaBuffer *buffer = NULL;
    BYTE *data = NULL;
    DWORD max_len = 0;
    UINT32 size = 0;
    INT64 pts, duration;
    DWORD out_flags;
    NTSTATUS ntstatus;
    HRESULT hr;

    TRACE("iface %p, flags %#lx, count %lu, samples %p, status %p.\n", iface, flags, count, samples, status);

    if (count != 1)
        return E_INVALIDARG;

    if (!decoder->winedmo_transform.handle)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    *status = samples->dwStatus = 0;

    /* Lock the caller's pre-allocated output buffer (non-PROVIDES_SAMPLES mode).
     * The caller (e.g. source_reader_pull_transform_samples) allocates the sample
     * based on GetOutputStreamInfo.cbSize and passes it here for us to fill. */
    if (samples->pSample)
    {
        if (FAILED(hr = IMFSample_ConvertToContiguousBuffer(samples->pSample, &buffer)))
            return hr;
        if (FAILED(hr = IMFMediaBuffer_Lock(buffer, &data, &max_len, NULL)))
        {
            IMFMediaBuffer_Release(buffer);
            return hr;
        }
        size = max_len;
    }

    ntstatus = winedmo_transform_get_output(decoder->winedmo_transform, data, &size,
            &pts, &duration, &out_flags);

    if (buffer)
        IMFMediaBuffer_Unlock(buffer);

    if (!ntstatus && !(out_flags & WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED))
    {
        if (buffer)
        {
            IMFMediaBuffer_SetCurrentLength(buffer, size);
            IMFMediaBuffer_Release(buffer);
        }
        if (pts != INT64_MIN) IMFSample_SetSampleTime(samples->pSample, pts);
        if (duration != INT64_MIN) IMFSample_SetSampleDuration(samples->pSample, duration);
        if (out_flags & WINEDMO_SAMPLE_FLAG_SYNC_POINT)
            IMFSample_SetUINT32(samples->pSample, &MFSampleExtension_CleanPoint, 1);
        if (out_flags & WINEDMO_SAMPLE_FLAG_DISCONTINUITY)
            IMFSample_SetUINT32(samples->pSample, &MFSampleExtension_Discontinuity, 1);
        if (out_flags & WINEDMO_SAMPLE_FLAG_INCOMPLETE)
            samples->dwStatus |= MFT_OUTPUT_DATA_BUFFER_INCOMPLETE;
        return S_OK;
    }

    if (buffer)
        IMFMediaBuffer_Release(buffer);

    if (!ntstatus && (out_flags & WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED))
    {
        samples->dwStatus = MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
        return MF_E_TRANSFORM_STREAM_CHANGE;
    }
    else if (ntstatus == STATUS_MORE_PROCESSING_REQUIRED)
    {
        samples->dwStatus = MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE;
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }
    else if (ntstatus == STATUS_END_OF_FILE)
    {
        samples->dwStatus = MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE;
        return MF_E_END_OF_STREAM;
    }
    else
    {
        samples->dwStatus = MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE;
        return E_FAIL;
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

static HEAACWAVEINFO aac_decoder_input_types[] =
{
#define MAKE_HEAACWAVEINFO(format, payload) \
    {.wfx = {.wFormatTag = format, .nChannels = 6, .nSamplesPerSec = 48000, .nAvgBytesPerSec = 1152000, \
             .nBlockAlign = 24, .wBitsPerSample = 32, .cbSize = sizeof(HEAACWAVEINFO) - sizeof(WAVEFORMATEX)}, \
     .wPayloadType = payload}

    MAKE_HEAACWAVEINFO(WAVE_FORMAT_MPEG_HEAAC, 0),
    MAKE_HEAACWAVEINFO(WAVE_FORMAT_RAW_AAC1, 0),
    MAKE_HEAACWAVEINFO(WAVE_FORMAT_MPEG_HEAAC, 1),
    MAKE_HEAACWAVEINFO(WAVE_FORMAT_MPEG_HEAAC, 3),
    MAKE_HEAACWAVEINFO(WAVE_FORMAT_MPEG_ADTS_AAC, 0),

#undef MAKE_HEAACWAVEINFO
};

HRESULT aac_decoder_create(REFIID riid, void **ret)
{
    struct audio_decoder *decoder;

    TRACE("riid %s, ret %p.\n", debugstr_guid(riid), ret);

    if (!(decoder = calloc(1, sizeof(*decoder))))
        return E_OUTOFMEMORY;
    decoder->input_types = (WAVEFORMATEXTENSIBLE *)aac_decoder_input_types;
    decoder->input_type_count = ARRAY_SIZE(aac_decoder_input_types);

    decoder->IMFTransform_iface.lpVtbl = &transform_vtbl;
    decoder->refcount = 1;

    *ret = &decoder->IMFTransform_iface;
    TRACE("Created decoder %p\n", *ret);
    return S_OK;
}

static WAVEFORMATEXTENSIBLE audio_decoder_input_types[] =
{
    {.Format = {.wFormatTag = WAVE_FORMAT_EXTENSIBLE, .nChannels = 6, .nSamplesPerSec = 48000, .nAvgBytesPerSec = 1152000, \
                .nBlockAlign = 24, .wBitsPerSample = 32, .cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)}, \
     .SubFormat = {0x8d2fd10b,0x5841,0x4a6b,{0x89,0x05,0x58,0x8f,0xec,0x1a,0xde,0xd9}}},
    {.Format = {.wFormatTag = WAVE_FORMAT_OPUS, .nChannels = 6, .nSamplesPerSec = 48000, .nAvgBytesPerSec = 1152000,
                .nBlockAlign = 24, .wBitsPerSample = 32, .cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)}},
    {.Format = {.wFormatTag = WAVE_FORMAT_ADPCM, .cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)}},
    {.Format = {.wFormatTag = 0x0160, .cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)}},  /* WMA v1 */
    {.Format = {.wFormatTag = 0x0161, .cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)}},  /* WMA v2/v8 */
    {.Format = {.wFormatTag = 0x0162, .cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)}},  /* WMA Pro/v9 */
    {.Format = {.wFormatTag = 0x0163, .cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)}},  /* WMA Lossless */
};

HRESULT audio_decoder_create(REFIID riid, void **ret)
{
    struct audio_decoder *decoder;

    TRACE("riid %s, ret %p.\n", debugstr_guid(riid), ret);

    if (!(decoder = calloc(1, sizeof(*decoder))))
        return E_OUTOFMEMORY;
    decoder->input_types = (WAVEFORMATEXTENSIBLE *)audio_decoder_input_types;
    decoder->input_type_count = ARRAY_SIZE(audio_decoder_input_types);

    decoder->IMFTransform_iface.lpVtbl = &transform_vtbl;
    decoder->refcount = 1;

    *ret = &decoder->IMFTransform_iface;
    TRACE("Created decoder %p\n", *ret);
    return S_OK;
}
