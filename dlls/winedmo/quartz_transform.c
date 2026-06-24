/*
 * DirectShow transform filters
 *
 * Copyright 2022 Anton Baskanov
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
#include "winedmo_guids.h"

#include "mferror.h"
#include "mpegtype.h"
#include "wine/winedmo.h"

WINE_DEFAULT_DEBUG_CHANNEL(quartz);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

#define AC3_SPEAKER_5POINT1_SURROUND 0x60f

struct transform
{
    struct strmbase_filter filter;
    IMpegAudioDecoder IMpegAudioDecoder_iface;

    struct strmbase_sink sink;
    struct strmbase_source source;
    struct strmbase_passthrough passthrough;

    IQualityControl sink_IQualityControl_iface;
    IQualityControl source_IQualityControl_iface;
    IQualityControl *qc_sink;

    struct winedmo_transform winedmo_transform;
    CRITICAL_SECTION receive_cs;

    const struct transform_ops *ops;
    bool synthesize_video_timestamps;
    bool have_next_output_time;
    bool have_video_input;
    bool have_decoded_output;
    REFERENCE_TIME next_output_time;
};

struct transform_ops
{
    HRESULT (*sink_query_accept)(struct transform *filter, const AM_MEDIA_TYPE *mt);
    HRESULT (*source_query_accept)(struct transform *filter, const AM_MEDIA_TYPE *mt);
    HRESULT (*source_get_media_type)(struct transform *filter, unsigned int index, AM_MEDIA_TYPE *mt);
    HRESULT (*source_decide_buffer_size)(struct transform *filter, IMemAllocator *allocator, ALLOCATOR_PROPERTIES *props);
    HRESULT (*source_qc_notify)(struct transform *filter, IBaseFilter *sender, Quality q);
};

static inline struct transform *impl_from_strmbase_filter(struct strmbase_filter *iface)
{
    return CONTAINING_RECORD(iface, struct transform, filter);
}

static struct strmbase_pin *transform_get_pin(struct strmbase_filter *iface, unsigned int index)
{
    struct transform *filter = impl_from_strmbase_filter(iface);
    if (index == 0)
        return &filter->sink.pin;
    if (index == 1)
        return &filter->source.pin;
    return NULL;
}

static BOOL is_rgb_video_subtype(const GUID *subtype)
{
    return IsEqualGUID(subtype, &MEDIASUBTYPE_RGB24)
            || IsEqualGUID(subtype, &MEDIASUBTYPE_RGB32)
            || IsEqualGUID(subtype, &MEDIASUBTYPE_RGB565)
            || IsEqualGUID(subtype, &MEDIASUBTYPE_RGB555);
}

static BOOL is_mpeg4_part2_video_subtype(const GUID *subtype)
{
    switch (subtype->Data1)
    {
        case MAKEFOURCC('D','I','V','X'):
        case MAKEFOURCC('D','X','5','0'):
        case MAKEFOURCC('F','M','P','4'):
        case MAKEFOURCC('M','4','S','2'):
        case MAKEFOURCC('M','P','4','S'):
        case MAKEFOURCC('M','P','4','V'):
        case MAKEFOURCC('X','V','I','D'):
        case MAKEFOURCC('m','p','4','v'):
            return subtype->Data2 == 0x0000
                    && subtype->Data3 == 0x0010
                    && subtype->Data4[0] == 0x80
                    && subtype->Data4[1] == 0x00
                    && subtype->Data4[2] == 0x00
                    && subtype->Data4[3] == 0xaa
                    && subtype->Data4[4] == 0x00
                    && subtype->Data4[5] == 0x38
                    && subtype->Data4[6] == 0x9b
                    && subtype->Data4[7] == 0x71;
    }

    return FALSE;
}

static BOOL is_mpeg2_video_subtype(const GUID *subtype)
{
    static const GUID MEDIASUBTYPE_mpg2 = {MAKEFOURCC('m','p','g','2'),0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};

    return IsEqualGUID(subtype, &MEDIASUBTYPE_MPEG2_VIDEO)
            || IsEqualGUID(subtype, &MEDIASUBTYPE_mpg2);
}

static BOOL is_windows_media_video_subtype(const GUID *subtype)
{
    if (subtype->Data2 != 0x0000 || subtype->Data3 != 0x0010
            || subtype->Data4[0] != 0x80 || subtype->Data4[1] != 0x00
            || subtype->Data4[2] != 0x00 || subtype->Data4[3] != 0xaa
            || subtype->Data4[4] != 0x00 || subtype->Data4[5] != 0x38
            || subtype->Data4[6] != 0x9b || subtype->Data4[7] != 0x71)
        return FALSE;

    switch (subtype->Data1)
    {
        case MAKEFOURCC('W','M','V','1'):
        case MAKEFOURCC('W','M','V','2'):
        case MAKEFOURCC('W','M','V','3'):
        case MAKEFOURCC('W','M','V','A'):
        case MAKEFOURCC('W','M','V','P'):
        case MAKEFOURCC('W','V','C','1'):
        case MAKEFOURCC('V','C','1','S'):
            return TRUE;
    }

    return FALSE;
}

static void transform_destroy(struct strmbase_filter *iface)
{
    struct transform *filter = impl_from_strmbase_filter(iface);

    filter->receive_cs.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&filter->receive_cs);

    strmbase_passthrough_cleanup(&filter->passthrough);
    strmbase_source_cleanup(&filter->source);
    strmbase_sink_cleanup(&filter->sink);
    strmbase_filter_cleanup(&filter->filter);

    free(filter);
}

static HRESULT transform_query_interface(struct strmbase_filter *iface, REFIID iid, void **out)
{
    struct transform *filter = impl_from_strmbase_filter(iface);

    if (IsEqualGUID(iid, &IID_IMpegAudioDecoder) && filter->IMpegAudioDecoder_iface.lpVtbl)
        *out = &filter->IMpegAudioDecoder_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT transform_init_stream(struct strmbase_filter *iface)
{
    struct transform *filter = impl_from_strmbase_filter(iface);
    const AM_MEDIA_TYPE *sink_mt = &filter->sink.pin.mt;
    const AM_MEDIA_TYPE *source_mt = &filter->source.pin.mt;
    NTSTATUS status;
    HRESULT hr;

    if (filter->source.pin.peer)
    {
        if (IsEqualGUID(&sink_mt->majortype, &MEDIATYPE_Video))
        {
            const VIDEOINFOHEADER *input_vih = (const VIDEOINFOHEADER *)sink_mt->pbFormat;
            const VIDEOINFOHEADER *vih = (const VIDEOINFOHEADER *)source_mt->pbFormat;
            MFVIDEOFORMAT input_fmt = {0}, output_fmt = {0};

            input_fmt.dwSize = sizeof(input_fmt);
            input_fmt.guidFormat = sink_mt->subtype;
            input_fmt.videoInfo.dwWidth = input_vih->bmiHeader.biWidth;
            input_fmt.videoInfo.dwHeight = abs(input_vih->bmiHeader.biHeight);
            if (input_vih->AvgTimePerFrame)
            {
                input_fmt.videoInfo.FramesPerSecond.Numerator = 10000000;
                input_fmt.videoInfo.FramesPerSecond.Denominator = input_vih->AvgTimePerFrame;
            }

            output_fmt.dwSize = sizeof(output_fmt);
            output_fmt.guidFormat = source_mt->subtype;
            output_fmt.videoInfo.dwWidth = vih->bmiHeader.biWidth;
            output_fmt.videoInfo.dwHeight = abs(vih->bmiHeader.biHeight);
            if (vih->bmiHeader.biHeight > 0 && is_rgb_video_subtype(&source_mt->subtype))
                output_fmt.videoInfo.VideoFlags |= MFVideoFlag_BottomUpLinearRep;
            if (vih->AvgTimePerFrame)
            {
                output_fmt.videoInfo.FramesPerSecond.Numerator = 10000000;
                output_fmt.videoInfo.FramesPerSecond.Denominator = vih->AvgTimePerFrame;
            }

            status = winedmo_transform_create(MFMediaType_Video,
                    (union winedmo_format *)&input_fmt, sizeof(input_fmt),
                    (union winedmo_format *)&output_fmt, sizeof(output_fmt),
                    &filter->winedmo_transform);
        }
        else
        {
            status = winedmo_transform_create(MFMediaType_Audio,
                    (union winedmo_format *)sink_mt->pbFormat, sink_mt->cbFormat,
                    (union winedmo_format *)source_mt->pbFormat, source_mt->cbFormat,
                    &filter->winedmo_transform);
        }

        if (status)
        {
            WARN("Failed to create winedmo transform, status %#lx.\n", status);
            return E_FAIL;
        }
        filter->have_decoded_output = false;
        filter->have_video_input = false;

        hr = IMemAllocator_Commit(filter->source.pAllocator);
        if (FAILED(hr))
            ERR("Failed to commit allocator, hr %#lx.\n", hr);
    }

    return S_OK;
}

static HRESULT transform_cleanup_stream(struct strmbase_filter *iface)
{
    struct transform *filter = impl_from_strmbase_filter(iface);

    if (filter->source.pin.peer)
    {
        IMemAllocator_Decommit(filter->source.pAllocator);

        EnterCriticalSection(&filter->receive_cs);
        EnterCriticalSection(&filter->filter.stream_cs);
        winedmo_transform_destroy(filter->winedmo_transform);
        filter->winedmo_transform.handle = 0;
        LeaveCriticalSection(&filter->filter.stream_cs);
        LeaveCriticalSection(&filter->receive_cs);
    }

    return S_OK;
}

static const struct strmbase_filter_ops filter_ops =
{
    .filter_get_pin = transform_get_pin,
    .filter_destroy = transform_destroy,
    .filter_query_interface = transform_query_interface,
    .filter_init_stream = transform_init_stream,
    .filter_cleanup_stream = transform_cleanup_stream,
};

static struct transform *impl_from_IMpegAudioDecoder(IMpegAudioDecoder *iface)
{
    return CONTAINING_RECORD(iface, struct transform, IMpegAudioDecoder_iface);
}

static HRESULT WINAPI mpeg_audio_decoder_QueryInterface(IMpegAudioDecoder *iface,
        REFIID iid, void **out)
{
    struct transform *filter = impl_from_IMpegAudioDecoder(iface);
    return IUnknown_QueryInterface(filter->filter.outer_unk, iid, out);
}

static ULONG WINAPI mpeg_audio_decoder_AddRef(IMpegAudioDecoder *iface)
{
    struct transform *filter = impl_from_IMpegAudioDecoder(iface);
    return IUnknown_AddRef(filter->filter.outer_unk);
}

static ULONG WINAPI mpeg_audio_decoder_Release(IMpegAudioDecoder *iface)
{
    struct transform *filter = impl_from_IMpegAudioDecoder(iface);
    return IUnknown_Release(filter->filter.outer_unk);
}

static HRESULT WINAPI mpeg_audio_decoder_get_FrequencyDivider(IMpegAudioDecoder *iface, ULONG *divider)
{
    FIXME("iface %p, divider %p, stub!\n", iface, divider);
    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg_audio_decoder_put_FrequencyDivider(IMpegAudioDecoder *iface, ULONG divider)
{
    FIXME("iface %p, divider %lu, stub!\n", iface, divider);
    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg_audio_decoder_get_DecoderAccuracy(IMpegAudioDecoder *iface, ULONG *accuracy)
{
    FIXME("iface %p, accuracy %p, stub!\n", iface, accuracy);
    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg_audio_decoder_put_DecoderAccuracy(IMpegAudioDecoder *iface, ULONG accuracy)
{
    FIXME("iface %p, accuracy %lu, stub!\n", iface, accuracy);
    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg_audio_decoder_get_Stereo(IMpegAudioDecoder *iface, ULONG *stereo)
{
    FIXME("iface %p, stereo %p, stub!\n", iface, stereo);
    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg_audio_decoder_put_Stereo(IMpegAudioDecoder *iface, ULONG stereo)
{
    FIXME("iface %p, stereo %lu, stub!\n", iface, stereo);
    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg_audio_decoder_get_DecoderWordSize(IMpegAudioDecoder *iface, ULONG *word_size)
{
    FIXME("iface %p, word_size %p, stub!\n", iface, word_size);
    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg_audio_decoder_put_DecoderWordSize(IMpegAudioDecoder *iface, ULONG word_size)
{
    FIXME("iface %p, word_size %lu, stub!\n", iface, word_size);
    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg_audio_decoder_get_IntegerDecode(IMpegAudioDecoder *iface, ULONG *integer_decode)
{
    FIXME("iface %p, integer_decode %p, stub!\n", iface, integer_decode);
    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg_audio_decoder_put_IntegerDecode(IMpegAudioDecoder *iface, ULONG integer_decode)
{
    FIXME("iface %p, integer_decode %lu, stub!\n", iface, integer_decode);
    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg_audio_decoder_get_DualMode(IMpegAudioDecoder *iface, ULONG *dual_mode)
{
    FIXME("iface %p, dual_mode %p, stub!\n", iface, dual_mode);
    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg_audio_decoder_put_DualMode(IMpegAudioDecoder *iface, ULONG dual_mode)
{
    FIXME("iface %p, dual_mode %lu, stub!\n", iface, dual_mode);
    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg_audio_decoder_get_AudioFormat(IMpegAudioDecoder *iface, MPEG1WAVEFORMAT *format)
{
    FIXME("iface %p, format %p, stub!\n", iface, format);
    return E_NOTIMPL;
}

static const IMpegAudioDecoderVtbl mpeg_audio_decoder_vtbl =
{
    mpeg_audio_decoder_QueryInterface,
    mpeg_audio_decoder_AddRef,
    mpeg_audio_decoder_Release,
    mpeg_audio_decoder_get_FrequencyDivider,
    mpeg_audio_decoder_put_FrequencyDivider,
    mpeg_audio_decoder_get_DecoderAccuracy,
    mpeg_audio_decoder_put_DecoderAccuracy,
    mpeg_audio_decoder_get_Stereo,
    mpeg_audio_decoder_put_Stereo,
    mpeg_audio_decoder_get_DecoderWordSize,
    mpeg_audio_decoder_put_DecoderWordSize,
    mpeg_audio_decoder_get_IntegerDecode,
    mpeg_audio_decoder_put_IntegerDecode,
    mpeg_audio_decoder_get_DualMode,
    mpeg_audio_decoder_put_DualMode,
    mpeg_audio_decoder_get_AudioFormat,
};

static HRESULT transform_sink_query_accept(struct strmbase_pin *pin, const AM_MEDIA_TYPE *mt)
{
    struct transform *filter = impl_from_strmbase_filter(pin->filter);

    return filter->ops->sink_query_accept(filter, mt);
}

static HRESULT transform_sink_query_interface(struct strmbase_pin *pin, REFIID iid, void **out)
{
    struct transform *filter = impl_from_strmbase_filter(pin->filter);

    if (IsEqualGUID(iid, &IID_IMemInputPin))
        *out = &filter->sink.IMemInputPin_iface;
    else if (IsEqualGUID(iid, &IID_IQualityControl))
        *out = &filter->sink_IQualityControl_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT transform_drain_output(struct transform *filter, BOOL *delivered, BOOL drain_all)
{
    DWORD out_flags;
    UINT32 out_size;
    NTSTATUS status;
    HRESULT hr;

    *delivered = FALSE;

    for (;;)
    {
        IMediaSample *output_sample;
        INT64 out_pts, out_dur;
        BYTE *out_data;

        hr = IMemAllocator_GetBuffer(filter->source.pAllocator, &output_sample, NULL, NULL, 0);
        if (FAILED(hr))
            return hr;

        if (FAILED(hr = IMediaSample_GetPointer(output_sample, &out_data)))
        {
            IMediaSample_Release(output_sample);
            return hr;
        }
        out_size = IMediaSample_GetSize(output_sample);

        EnterCriticalSection(&filter->filter.stream_cs);
        if (!filter->winedmo_transform.handle)
        {
            LeaveCriticalSection(&filter->filter.stream_cs);
            IMediaSample_Release(output_sample);
            break;
        }
        status = winedmo_transform_get_output(filter->winedmo_transform, out_data, &out_size, &out_pts, &out_dur, &out_flags);
        LeaveCriticalSection(&filter->filter.stream_cs);
        if (status == STATUS_MORE_PROCESSING_REQUIRED || status == STATUS_END_OF_FILE)
        {
            IMediaSample_Release(output_sample);
            break;
        }
        if (status)
        {
            IMediaSample_Release(output_sample);
            return E_FAIL;
        }

        if (filter->sink.flushing)
        {
            IMediaSample_Release(output_sample);
            break;
        }

        if (out_flags & WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED)
        {
            IMediaSample_Release(output_sample);
            continue;
        }

        IMediaSample_SetActualDataLength(output_sample, out_size);
        if (filter->synthesize_video_timestamps
                && IsEqualGUID(&filter->source.pin.mt.formattype, &FORMAT_VideoInfo)
                && filter->source.pin.mt.cbFormat >= sizeof(VIDEOINFOHEADER))
        {
            const VIDEOINFOHEADER *format = (const VIDEOINFOHEADER *)filter->source.pin.mt.pbFormat;
            bool first_sample = !filter->have_next_output_time;

            if (format->AvgTimePerFrame)
            {
                bool synthesize_time = out_pts == INT64_MIN;

                if (!synthesize_time && filter->have_next_output_time)
                {
                    REFERENCE_TIME delta = out_pts - filter->next_output_time;

                    if (delta < 0)
                        delta = -delta;
                    if (out_dur == INT64_MIN || out_dur <= 0
                            || out_dur > format->AvgTimePerFrame * 3 / 2
                            || delta > format->AvgTimePerFrame * 3)
                        synthesize_time = true;
                }

                if (synthesize_time)
                {
                    if (first_sample)
                    {
                        filter->next_output_time = 0;
                        filter->have_next_output_time = true;
                    }
                    out_pts = filter->next_output_time;
                    out_dur = format->AvgTimePerFrame;
                    filter->next_output_time += format->AvgTimePerFrame;
                    if (first_sample)
                        out_flags |= WINEDMO_SAMPLE_FLAG_DISCONTINUITY;
                }
                else
                {
                    if (out_dur == INT64_MIN || out_dur <= 0)
                        out_dur = format->AvgTimePerFrame;
                    filter->next_output_time = out_pts + out_dur;
                    filter->have_next_output_time = true;
                }
            }
        }

        IMediaSample_SetSyncPoint(output_sample, (out_flags & WINEDMO_SAMPLE_FLAG_SYNC_POINT)
                || IsEqualGUID(&filter->source.pin.mt.majortype, &MEDIATYPE_Video));
        IMediaSample_SetDiscontinuity(output_sample, !!(out_flags & WINEDMO_SAMPLE_FLAG_DISCONTINUITY));
        IMediaSample_SetPreroll(output_sample, FALSE);

        if (out_pts != INT64_MIN)
        {
            REFERENCE_TIME pts_rt, end_rt;

            if (out_dur <= 0
                    && IsEqualGUID(&filter->source.pin.mt.formattype, &FORMAT_VideoInfo)
                    && filter->source.pin.mt.cbFormat >= sizeof(VIDEOINFOHEADER))
            {
                const VIDEOINFOHEADER *format = (const VIDEOINFOHEADER *)filter->source.pin.mt.pbFormat;

                if (format->AvgTimePerFrame > 0)
                    out_dur = format->AvgTimePerFrame;
            }

            pts_rt = out_pts;
            end_rt = out_pts + out_dur;
            IMediaSample_SetTime(output_sample, &pts_rt, out_dur != INT64_MIN ? &end_rt : NULL);
        }

        if (filter->sink.flushing)
            hr = S_FALSE;
        else
            hr = IMemInputPin_Receive(filter->source.pMemInputPin, output_sample);
        IMediaSample_Release(output_sample);
        if (FAILED(hr))
            return hr;
        *delivered = TRUE;
        filter->have_decoded_output = true;
        if (!drain_all && IsEqualGUID(&filter->source.pin.mt.majortype, &MEDIATYPE_Video))
            break;
    }

    return S_OK;
}

static HRESULT WINAPI transform_sink_receive(struct strmbase_sink *pin, IMediaSample *sample)
{
    struct transform *filter = impl_from_strmbase_filter(pin->pin.filter);
    REFERENCE_TIME start_time = 0, end_time = 0;
    INT64 pts = INT64_MIN, duration = INT64_MIN;
    DWORD push_flags = 0;
    NTSTATUS status;
    BOOL delivered;
    BYTE *data;
    LONG size;
    HRESULT hr = S_OK;

    /* We do not expect pin connection state to change while the filter is
     * running. This guarantee is necessary, since otherwise we would have to
     * take the filter lock, and we can't take the filter lock from a streaming
     * thread. */
    if (!filter->source.pMemInputPin)
    {
        WARN("Source is not connected, returning VFW_E_NOT_CONNECTED.\n");
        return VFW_E_NOT_CONNECTED;
    }

    if (filter->filter.state == State_Stopped)
        return VFW_E_WRONG_STATE;

    if (filter->sink.flushing)
        return S_FALSE;

    EnterCriticalSection(&filter->receive_cs);
    if (filter->sink.flushing)
    {
        hr = S_FALSE;
        goto done;
    }

    if (FAILED(hr = IMediaSample_GetPointer(sample, &data)))
        goto done;
    size = IMediaSample_GetActualDataLength(sample);

    hr = IMediaSample_GetTime(sample, &start_time, &end_time);
    if (hr == S_OK)
    {
        pts = start_time;
        duration = end_time - start_time;
    }
    else if (hr == VFW_S_NO_STOP_TIME)
        pts = start_time;

    if (IMediaSample_IsSyncPoint(sample) == S_OK)
        push_flags |= WINEDMO_SAMPLE_FLAG_SYNC_POINT;
    if (IMediaSample_IsDiscontinuity(sample) == S_OK)
    {
        push_flags |= WINEDMO_SAMPLE_FLAG_DISCONTINUITY;
        filter->have_next_output_time = false;
    }

    if (IsEqualGUID(&filter->sink.pin.mt.majortype, &MEDIATYPE_Video)
            && filter->have_video_input && pts == 0 && (push_flags & WINEDMO_SAMPLE_FLAG_SYNC_POINT))
    {
        push_flags |= WINEDMO_SAMPLE_FLAG_DISCONTINUITY;
        filter->have_next_output_time = false;
    }

    if ((push_flags & WINEDMO_SAMPLE_FLAG_DISCONTINUITY)
            && is_windows_media_video_subtype(&filter->sink.pin.mt.subtype))
    {
        EnterCriticalSection(&filter->filter.stream_cs);
        if (filter->winedmo_transform.handle)
            status = winedmo_transform_flush(filter->winedmo_transform);
        else
            status = STATUS_SUCCESS;
        LeaveCriticalSection(&filter->filter.stream_cs);
        if (status)
        {
            hr = E_FAIL;
            goto done;
        }
    }

    for (;;)
    {
        EnterCriticalSection(&filter->filter.stream_cs);
        if (!filter->winedmo_transform.handle)
        {
            LeaveCriticalSection(&filter->filter.stream_cs);
            hr = S_FALSE;
            goto done;
        }
        status = winedmo_transform_push_input(filter->winedmo_transform, data, size,
                pts, INT64_MIN, duration, push_flags);
        LeaveCriticalSection(&filter->filter.stream_cs);
        if (status != STATUS_DEVICE_BUSY)
            break;
        if (!is_windows_media_video_subtype(&filter->sink.pin.mt.subtype))
        {
            hr = MF_E_NOTACCEPTING;
            goto done;
        }

        if (FAILED(hr = transform_drain_output(filter, &delivered, FALSE)))
            goto done;
        if (!delivered)
        {
            hr = MF_E_NOTACCEPTING;
            goto done;
        }
    }
    if (status)
    {
        hr = E_FAIL;
        goto done;
    }

    if (IsEqualGUID(&filter->sink.pin.mt.majortype, &MEDIATYPE_Video))
        filter->have_video_input = true;

    hr = transform_drain_output(filter, &delivered, FALSE);

done:
    LeaveCriticalSection(&filter->receive_cs);
    return hr;
}

static HRESULT transform_sink_begin_flush(struct strmbase_sink *pin)
{
    struct transform *filter = impl_from_strmbase_filter(pin->pin.filter);
    NTSTATUS status = STATUS_SUCCESS;
    HRESULT hr = S_OK;

    /* BeginFlush is used to abort in-flight delivery. Forward it downstream
     * before waiting for stream_cs; Receive() may be blocked there while
     * holding stream_cs, and the downstream flush is what wakes it up. */
    if (filter->source.pin.peer && FAILED(hr = IPin_BeginFlush(filter->source.pin.peer)))
        return hr;

    EnterCriticalSection(&filter->filter.stream_cs);
    filter->have_next_output_time = false;
    if (filter->winedmo_transform.handle)
        status = winedmo_transform_flush(filter->winedmo_transform);
    LeaveCriticalSection(&filter->filter.stream_cs);
    if (status)
        return E_FAIL;

    return hr;
}

static HRESULT transform_sink_end_flush(struct strmbase_sink *pin)
{
    struct transform *filter = impl_from_strmbase_filter(pin->pin.filter);

    TRACE("filter %p.\n", filter);

    if (filter->source.pin.peer)
        return IPin_EndFlush(filter->source.pin.peer);

    return S_OK;
}

static HRESULT transform_sink_eos(struct strmbase_sink *pin)
{
    struct transform *filter = impl_from_strmbase_filter(pin->pin.filter);
    BOOL delivered;
    NTSTATUS status = STATUS_SUCCESS;
    HRESULT hr;

    TRACE("filter %p.\n", filter);

    EnterCriticalSection(&filter->receive_cs);
    EnterCriticalSection(&filter->filter.stream_cs);
    if (filter->winedmo_transform.handle)
        status = winedmo_transform_drain(filter->winedmo_transform);
    LeaveCriticalSection(&filter->filter.stream_cs);

    if (status)
        hr = E_FAIL;
    else
        /* On EOS, drain all remaining decoded output so the renderer can
         * receive the complete stream before EndOfStream is forwarded. */
        hr = transform_drain_output(filter, &delivered, TRUE);
    LeaveCriticalSection(&filter->receive_cs);

    if (FAILED(hr))
        return hr;

    if (filter->source.pin.peer)
        return IPin_EndOfStream(filter->source.pin.peer);

    return S_OK;
}

static const struct strmbase_sink_ops sink_ops =
{
    .base.pin_query_accept = transform_sink_query_accept,
    .base.pin_query_interface = transform_sink_query_interface,
    .pfnReceive = transform_sink_receive,
    .sink_eos = transform_sink_eos,
    .sink_begin_flush = transform_sink_begin_flush,
    .sink_end_flush = transform_sink_end_flush,
};

static HRESULT transform_source_query_accept(struct strmbase_pin *pin, const AM_MEDIA_TYPE *mt)
{
    struct transform *filter = impl_from_strmbase_filter(pin->filter);

    return filter->ops->source_query_accept(filter, mt);
}

static HRESULT transform_source_get_media_type(struct strmbase_pin *pin, unsigned int index, AM_MEDIA_TYPE *mt)
{
    struct transform *filter = impl_from_strmbase_filter(pin->filter);

    return filter->ops->source_get_media_type(filter, index, mt);
}

static HRESULT transform_source_query_interface(struct strmbase_pin *pin, REFIID iid, void **out)
{
    struct transform *filter = impl_from_strmbase_filter(pin->filter);

    if (IsEqualGUID(iid, &IID_IMediaPosition))
        *out = &filter->passthrough.IMediaPosition_iface;
    else if (IsEqualGUID(iid, &IID_IMediaSeeking))
        *out = &filter->passthrough.IMediaSeeking_iface;
    else if (IsEqualGUID(iid, &IID_IQualityControl))
        *out = &filter->source_IQualityControl_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT WINAPI transform_source_DecideBufferSize(struct strmbase_source *pin, IMemAllocator *allocator, ALLOCATOR_PROPERTIES *props)
{
    struct transform *filter = impl_from_strmbase_filter(pin->pin.filter);

    return filter->ops->source_decide_buffer_size(filter, allocator, props);
}

static const struct strmbase_source_ops source_ops =
{
    .base.pin_query_accept = transform_source_query_accept,
    .base.pin_get_media_type = transform_source_get_media_type,
    .base.pin_query_interface = transform_source_query_interface,
    .pfnAttemptConnection = BaseOutputPinImpl_AttemptConnection,
    .pfnDecideAllocator = BaseOutputPinImpl_DecideAllocator,
    .pfnDecideBufferSize = transform_source_DecideBufferSize,
};

static struct transform *impl_from_sink_IQualityControl(IQualityControl *iface)
{
    return CONTAINING_RECORD(iface, struct transform, sink_IQualityControl_iface);
}

static HRESULT WINAPI sink_quality_control_QueryInterface(IQualityControl *iface, REFIID iid, void **out)
{
    struct transform *filter = impl_from_sink_IQualityControl(iface);
    return IPin_QueryInterface(&filter->source.pin.IPin_iface, iid, out);
}

static ULONG WINAPI sink_quality_control_AddRef(IQualityControl *iface)
{
    struct transform *filter = impl_from_sink_IQualityControl(iface);
    return IPin_AddRef(&filter->source.pin.IPin_iface);
}

static ULONG WINAPI sink_quality_control_Release(IQualityControl *iface)
{
    struct transform *filter = impl_from_sink_IQualityControl(iface);
    return IPin_Release(&filter->source.pin.IPin_iface);
}

static HRESULT WINAPI sink_quality_control_Notify(IQualityControl *iface, IBaseFilter *sender, Quality q)
{
    struct transform *filter = impl_from_sink_IQualityControl(iface);

    TRACE("filter %p, sender %p, type %#x, proportion %ld, late %s, timestamp %s.\n",
            filter, sender, q.Type, q.Proportion, debugstr_time(q.Late), debugstr_time(q.TimeStamp));

    return S_OK;
}

static HRESULT WINAPI sink_quality_control_SetSink(IQualityControl *iface, IQualityControl *sink)
{
    struct transform *filter = impl_from_sink_IQualityControl(iface);

    TRACE("filter %p, sink %p.\n", filter, sink);

    filter->qc_sink = sink;

    return S_OK;
}

static const IQualityControlVtbl sink_quality_control_vtbl =
{
    sink_quality_control_QueryInterface,
    sink_quality_control_AddRef,
    sink_quality_control_Release,
    sink_quality_control_Notify,
    sink_quality_control_SetSink,
};

static struct transform *impl_from_source_IQualityControl(IQualityControl *iface)
{
    return CONTAINING_RECORD(iface, struct transform, source_IQualityControl_iface);
}

static HRESULT WINAPI source_quality_control_QueryInterface(IQualityControl *iface, REFIID iid, void **out)
{
    struct transform *filter = impl_from_source_IQualityControl(iface);
    return IPin_QueryInterface(&filter->source.pin.IPin_iface, iid, out);
}

static ULONG WINAPI source_quality_control_AddRef(IQualityControl *iface)
{
    struct transform *filter = impl_from_source_IQualityControl(iface);
    return IPin_AddRef(&filter->source.pin.IPin_iface);
}

static ULONG WINAPI source_quality_control_Release(IQualityControl *iface)
{
    struct transform *filter = impl_from_source_IQualityControl(iface);
    return IPin_Release(&filter->source.pin.IPin_iface);
}

static HRESULT WINAPI source_quality_control_Notify(IQualityControl *iface, IBaseFilter *sender, Quality q)
{
    struct transform *filter = impl_from_source_IQualityControl(iface);

    return filter->ops->source_qc_notify(filter, sender, q);
}

static HRESULT WINAPI source_quality_control_SetSink(IQualityControl *iface, IQualityControl *sink)
{
    struct transform *filter = impl_from_source_IQualityControl(iface);

    TRACE("filter %p, sink %p.\n", filter, sink);

    return S_OK;
}

static const IQualityControlVtbl source_quality_control_vtbl =
{
    source_quality_control_QueryInterface,
    source_quality_control_AddRef,
    source_quality_control_Release,
    source_quality_control_Notify,
    source_quality_control_SetSink,
};

static HRESULT transform_create(IUnknown *outer, const CLSID *clsid, const struct transform_ops *ops, struct transform **out)
{
    struct transform *object;

    object = calloc(1, sizeof(*object));
    if (!object)
        return E_OUTOFMEMORY;

    strmbase_filter_init(&object->filter, outer, clsid, &filter_ops);
    InitializeCriticalSectionEx(&object->receive_cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    object->receive_cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": transform.receive_cs");

    strmbase_sink_init(&object->sink, &object->filter, L"In", &sink_ops, NULL);
    strmbase_source_init(&object->source, &object->filter, L"Out", &source_ops);

    strmbase_passthrough_init(&object->passthrough, (IUnknown *)&object->source.pin.IPin_iface);
    ISeekingPassThru_Init(&object->passthrough.ISeekingPassThru_iface, FALSE,
            &object->sink.pin.IPin_iface);

    object->sink_IQualityControl_iface.lpVtbl = &sink_quality_control_vtbl;
    object->source_IQualityControl_iface.lpVtbl = &source_quality_control_vtbl;

    object->ops = ops;

    *out = object;
    return S_OK;
}

static HRESULT passthrough_source_qc_notify(struct transform *filter, IBaseFilter *sender, Quality q)
{
    IQualityControl *peer;
    HRESULT hr = VFW_E_NOT_FOUND;

    TRACE("filter %p, sender %p, type %s, proportion %ld, late %s, timestamp %s.\n",
            filter, sender, q.Type == Famine ? "Famine" : "Flood", q.Proportion,
            debugstr_time(q.Late), debugstr_time(q.TimeStamp));

    if (filter->qc_sink)
        return IQualityControl_Notify(filter->qc_sink, &filter->filter.IBaseFilter_iface, q);

    if (filter->sink.pin.peer
            && SUCCEEDED(IPin_QueryInterface(filter->sink.pin.peer, &IID_IQualityControl, (void **)&peer)))
    {
        hr = IQualityControl_Notify(peer, &filter->filter.IBaseFilter_iface, q);
        IQualityControl_Release(peer);
    }

    return hr;
}

static BOOL is_aac_wave_format_tag(WORD tag)
{
    switch (tag)
    {
        case WAVE_FORMAT_RAW_AAC1:
        case WAVE_FORMAT_FRAUNHOFER_IIS_MPEG2_AAC:
        case WAVE_FORMAT_MPEG_ADTS_AAC:
        case WAVE_FORMAT_MPEG_RAW_AAC:
        case WAVE_FORMAT_NOKIA_MPEG_ADTS_AAC:
        case WAVE_FORMAT_NOKIA_MPEG_RAW_AAC:
        case WAVE_FORMAT_VODAFONE_MPEG_ADTS_AAC:
        case WAVE_FORMAT_VODAFONE_MPEG_RAW_AAC:
        case WAVE_FORMAT_MPEG_HEAAC:
        case WAVE_FORMAT_MPEG4_AAC:
            return TRUE;
        default:
            return FALSE;
    }
}

static BOOL is_wave_format_subtype(const GUID *subtype, WORD tag)
{
    static const BYTE tail[8] = {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

    return subtype->Data1 == tag && subtype->Data2 == 0x0000 && subtype->Data3 == 0x0010
            && !memcmp(subtype->Data4, tail, sizeof(tail));
}

static BOOL is_aac_audio_subtype(const GUID *subtype)
{
    if (IsEqualGUID(subtype, &GUID_NULL))
        return TRUE;

    return is_wave_format_subtype(subtype, WAVE_FORMAT_RAW_AAC1)
            || is_wave_format_subtype(subtype, WAVE_FORMAT_FRAUNHOFER_IIS_MPEG2_AAC)
            || is_wave_format_subtype(subtype, WAVE_FORMAT_MPEG_ADTS_AAC)
            || is_wave_format_subtype(subtype, WAVE_FORMAT_MPEG_RAW_AAC)
            || is_wave_format_subtype(subtype, WAVE_FORMAT_NOKIA_MPEG_ADTS_AAC)
            || is_wave_format_subtype(subtype, WAVE_FORMAT_NOKIA_MPEG_RAW_AAC)
            || is_wave_format_subtype(subtype, WAVE_FORMAT_VODAFONE_MPEG_ADTS_AAC)
            || is_wave_format_subtype(subtype, WAVE_FORMAT_VODAFONE_MPEG_RAW_AAC)
            || is_wave_format_subtype(subtype, WAVE_FORMAT_MPEG_HEAAC)
            || is_wave_format_subtype(subtype, WAVE_FORMAT_MPEG4_AAC);
}

static BOOL is_ac3_audio_subtype(const GUID *subtype)
{
    static const GUID dolby_ac3 = {0xe06d802c, 0xdb46, 0x11cf, {0xb4, 0xd1, 0x00, 0x80, 0x5f, 0x6c, 0xbb, 0xea}};

    return IsEqualGUID(subtype, &MFAudioFormat_Dolby_AC3)
            || IsEqualGUID(subtype, &dolby_ac3)
            || is_wave_format_subtype(subtype, WAVE_FORMAT_DOLBY_AC3_SPDIF);
}

static HRESULT aac_audio_decoder_sink_query_accept(struct transform *filter, const AM_MEDIA_TYPE *mt)
{
    const WAVEFORMATEX *format;

    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Audio)
            || !IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx)
            || mt->cbFormat < sizeof(WAVEFORMATEX) || !mt->pbFormat)
        return S_FALSE;

    format = (const WAVEFORMATEX *)mt->pbFormat;
    if (!is_aac_wave_format_tag(format->wFormatTag) && !is_aac_audio_subtype(&mt->subtype))
        return S_FALSE;

    return S_OK;
}

static HRESULT ac3_audio_decoder_sink_query_accept(struct transform *filter, const AM_MEDIA_TYPE *mt)
{
    const WAVEFORMATEX *format;

    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Audio)
            || !IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx)
            || mt->cbFormat < sizeof(WAVEFORMATEX) || !mt->pbFormat)
        return S_FALSE;

    format = (const WAVEFORMATEX *)mt->pbFormat;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)format;

        if (is_ac3_audio_subtype(&ext->SubFormat))
            return S_OK;
    }

    if (format->wFormatTag == WAVE_FORMAT_DOLBY_AC3_SPDIF || is_ac3_audio_subtype(&mt->subtype))
        return S_OK;

    return S_FALSE;
}

static HRESULT aac_audio_decoder_source_query_accept(struct transform *filter, const AM_MEDIA_TYPE *mt)
{
    const WAVEFORMATEX *input_format;
    const WAVEFORMATEX *output_format;

    if (!filter->sink.pin.peer)
        return S_FALSE;

    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Audio)
            || !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_PCM)
            || !IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx)
            || mt->cbFormat < sizeof(WAVEFORMATEX) || !mt->pbFormat)
        return S_FALSE;

    input_format = (const WAVEFORMATEX *)filter->sink.pin.mt.pbFormat;
    output_format = (const WAVEFORMATEX *)mt->pbFormat;

    if (output_format->wFormatTag != WAVE_FORMAT_PCM
            || input_format->nSamplesPerSec != output_format->nSamplesPerSec
            || input_format->nChannels != output_format->nChannels
            || output_format->wBitsPerSample != 16
            || output_format->nBlockAlign != output_format->nChannels * output_format->wBitsPerSample / 8
            || output_format->nAvgBytesPerSec != output_format->nBlockAlign * output_format->nSamplesPerSec)
        return S_FALSE;

    return S_OK;
}

static HRESULT ac3_audio_decoder_source_query_accept(struct transform *filter, const AM_MEDIA_TYPE *mt)
{
    const WAVEFORMATEX *output_format;
    const WAVEFORMATEXTENSIBLE *ext;

    if (!filter->sink.pin.peer)
        return S_FALSE;

    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Audio)
            || !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_IEEE_FLOAT)
            || !IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx)
            || mt->cbFormat < sizeof(WAVEFORMATEXTENSIBLE) || !mt->pbFormat)
        return S_FALSE;

    output_format = (const WAVEFORMATEX *)mt->pbFormat;
    ext = (const WAVEFORMATEXTENSIBLE *)output_format;
    if (output_format->wFormatTag != WAVE_FORMAT_EXTENSIBLE
            || output_format->nSamplesPerSec != 48000
            || output_format->nChannels != 6
            || output_format->wBitsPerSample != 32
            || output_format->nBlockAlign != output_format->nChannels * output_format->wBitsPerSample / 8
            || output_format->nAvgBytesPerSec != output_format->nBlockAlign * output_format->nSamplesPerSec
            || output_format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            || ext->Samples.wValidBitsPerSample != output_format->wBitsPerSample
            || ext->dwChannelMask != AC3_SPEAKER_5POINT1_SURROUND
            || !IsEqualGUID(&ext->SubFormat, &MEDIASUBTYPE_IEEE_FLOAT))
        return S_FALSE;

    return S_OK;
}

static HRESULT aac_audio_decoder_source_get_media_type(struct transform *filter, unsigned int index, AM_MEDIA_TYPE *mt)
{
    const WAVEFORMATEX *input_format;
    WAVEFORMATEX *output_format;

    if (!filter->sink.pin.peer)
        return VFW_S_NO_MORE_ITEMS;

    if (index > 0)
        return VFW_S_NO_MORE_ITEMS;

    input_format = (const WAVEFORMATEX *)filter->sink.pin.mt.pbFormat;

    output_format = CoTaskMemAlloc(sizeof(*output_format));
    if (!output_format)
        return E_OUTOFMEMORY;

    memset(output_format, 0, sizeof(*output_format));
    output_format->wFormatTag = WAVE_FORMAT_PCM;
    output_format->nSamplesPerSec = input_format->nSamplesPerSec;
    output_format->nChannels = input_format->nChannels;
    output_format->wBitsPerSample = 16;
    output_format->nBlockAlign = output_format->nChannels * output_format->wBitsPerSample / 8;
    output_format->nAvgBytesPerSec = output_format->nBlockAlign * output_format->nSamplesPerSec;

    memset(mt, 0, sizeof(*mt));
    mt->majortype = MEDIATYPE_Audio;
    mt->subtype = MEDIASUBTYPE_PCM;
    mt->bFixedSizeSamples = TRUE;
    mt->lSampleSize = 1536 * output_format->nBlockAlign;
    mt->formattype = FORMAT_WaveFormatEx;
    mt->cbFormat = sizeof(*output_format);
    mt->pbFormat = (BYTE *)output_format;

    return S_OK;
}

static HRESULT ac3_audio_decoder_source_get_media_type(struct transform *filter, unsigned int index, AM_MEDIA_TYPE *mt)
{
    WAVEFORMATEXTENSIBLE *output_format;

    if (!filter->sink.pin.peer)
        return VFW_S_NO_MORE_ITEMS;

    if (index > 0)
        return VFW_S_NO_MORE_ITEMS;

    output_format = CoTaskMemAlloc(sizeof(*output_format));
    if (!output_format)
        return E_OUTOFMEMORY;

    memset(output_format, 0, sizeof(*output_format));
    output_format->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    output_format->Format.nSamplesPerSec = 48000;
    output_format->Format.nChannels = 6;
    output_format->Format.wBitsPerSample = 32;
    output_format->Format.nBlockAlign = output_format->Format.nChannels * output_format->Format.wBitsPerSample / 8;
    output_format->Format.nAvgBytesPerSec = output_format->Format.nBlockAlign * output_format->Format.nSamplesPerSec;
    output_format->Format.cbSize = sizeof(*output_format) - sizeof(output_format->Format);
    output_format->Samples.wValidBitsPerSample = output_format->Format.wBitsPerSample;
    output_format->dwChannelMask = AC3_SPEAKER_5POINT1_SURROUND;
    output_format->SubFormat = MEDIASUBTYPE_IEEE_FLOAT;

    memset(mt, 0, sizeof(*mt));
    mt->majortype = MEDIATYPE_Audio;
    mt->subtype = MEDIASUBTYPE_IEEE_FLOAT;
    mt->bFixedSizeSamples = TRUE;
    mt->lSampleSize = output_format->Format.nBlockAlign;
    mt->formattype = FORMAT_WaveFormatEx;
    mt->cbFormat = sizeof(*output_format);
    mt->pbFormat = (BYTE *)output_format;

    return S_OK;
}

static HRESULT ac3_audio_decoder_source_decide_buffer_size(struct transform *filter, IMemAllocator *allocator,
        ALLOCATOR_PROPERTIES *props)
{
    ALLOCATOR_PROPERTIES ret_props;

    props->cBuffers = max(props->cBuffers, 8);
    props->cbBuffer = max(props->cbBuffer, 65536);
    props->cbAlign = max(props->cbAlign, 1);

    return IMemAllocator_SetProperties(allocator, props, &ret_props);
}

static HRESULT aac_audio_decoder_source_decide_buffer_size(struct transform *filter, IMemAllocator *allocator,
        ALLOCATOR_PROPERTIES *props)
{
    ALLOCATOR_PROPERTIES ret_props;

    props->cBuffers = max(props->cBuffers, 8);
    props->cbBuffer = max(props->cbBuffer, filter->source.pin.mt.lSampleSize * 8);
    props->cbAlign = max(props->cbAlign, 1);

    return IMemAllocator_SetProperties(allocator, props, &ret_props);
}

static const struct transform_ops aac_audio_decoder_transform_ops =
{
    aac_audio_decoder_sink_query_accept,
    aac_audio_decoder_source_query_accept,
    aac_audio_decoder_source_get_media_type,
    aac_audio_decoder_source_decide_buffer_size,
    passthrough_source_qc_notify,
};

static const struct transform_ops ac3_audio_decoder_transform_ops =
{
    ac3_audio_decoder_sink_query_accept,
    ac3_audio_decoder_source_query_accept,
    ac3_audio_decoder_source_get_media_type,
    ac3_audio_decoder_source_decide_buffer_size,
    passthrough_source_qc_notify,
};

HRESULT aac_audio_decoder_create(IUnknown *outer, IUnknown **out)
{
    struct transform *object;
    HRESULT hr;

    hr = transform_create(outer, &CLSID_winedmo_aac_audio_decoder, &aac_audio_decoder_transform_ops, &object);
    if (FAILED(hr))
        return hr;

    wcscpy(object->sink.pin.name, L"XForm In");
    wcscpy(object->source.pin.name, L"XForm Out");

    TRACE("Created AAC audio decoder %p.\n", object);
    *out = &object->filter.IUnknown_inner;
    return hr;
}

HRESULT ac3_audio_decoder_create(IUnknown *outer, IUnknown **out)
{
    struct transform *object;
    HRESULT hr;

    hr = transform_create(outer, &CLSID_winedmo_ac3_audio_decoder, &ac3_audio_decoder_transform_ops, &object);
    if (FAILED(hr))
        return hr;

    wcscpy(object->sink.pin.name, L"XForm In");
    wcscpy(object->source.pin.name, L"XForm Out");

    TRACE("Created AC3 audio decoder %p.\n", object);
    *out = &object->filter.IUnknown_inner;
    return hr;
}


static HRESULT mpeg_audio_codec_sink_query_accept(struct transform *filter, const AM_MEDIA_TYPE *mt)
{
    const MPEG1WAVEFORMAT *format;

    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Audio))
        return S_FALSE;

    if (!IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1Packet)
            && !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1Payload)
            && !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1AudioPayload)
            && !IsEqualGUID(&mt->subtype, &GUID_NULL))
        return S_FALSE;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx)
            || mt->cbFormat < sizeof(MPEG1WAVEFORMAT))
        return S_FALSE;

    format = (const MPEG1WAVEFORMAT *)mt->pbFormat;

    if (format->wfx.wFormatTag != WAVE_FORMAT_MPEG
            || format->fwHeadLayer == ACM_MPEG_LAYER3)
        return S_FALSE;

    return S_OK;
}

static HRESULT mpeg_audio_codec_source_query_accept(struct transform *filter, const AM_MEDIA_TYPE *mt)
{
    const MPEG1WAVEFORMAT *input_format;
    const WAVEFORMATEX *output_format;
    DWORD expected_avg_bytes_per_sec;
    WORD expected_block_align;

    if (!filter->sink.pin.peer)
        return S_FALSE;

    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Audio)
            || !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_PCM)
            || !IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx)
            || mt->cbFormat < sizeof(WAVEFORMATEX))
        return S_FALSE;

    input_format = (const MPEG1WAVEFORMAT *)filter->sink.pin.mt.pbFormat;
    output_format = (const WAVEFORMATEX *)mt->pbFormat;

    if (output_format->wFormatTag != WAVE_FORMAT_PCM
            || input_format->wfx.nSamplesPerSec != output_format->nSamplesPerSec
            || input_format->wfx.nChannels != output_format->nChannels
            || (output_format->wBitsPerSample != 8 && output_format->wBitsPerSample != 16))
        return S_FALSE;

    expected_block_align = output_format->nChannels * output_format->wBitsPerSample / 8;
    expected_avg_bytes_per_sec = expected_block_align * output_format->nSamplesPerSec;

    if (output_format->nBlockAlign != expected_block_align
            || output_format->nAvgBytesPerSec != expected_avg_bytes_per_sec)
        return S_FALSE;

    return S_OK;
}

static HRESULT mpeg_audio_codec_source_get_media_type(struct transform *filter, unsigned int index, AM_MEDIA_TYPE *mt)
{
    const MPEG1WAVEFORMAT *input_format;
    WAVEFORMATEX *output_format;

    if (!filter->sink.pin.peer)
        return VFW_S_NO_MORE_ITEMS;

    if (index > 1)
        return VFW_S_NO_MORE_ITEMS;

    input_format = (const MPEG1WAVEFORMAT *)filter->sink.pin.mt.pbFormat;

    output_format = CoTaskMemAlloc(sizeof(*output_format));
    if (!output_format)
        return E_OUTOFMEMORY;

    memset(output_format, 0, sizeof(*output_format));
    output_format->wFormatTag = WAVE_FORMAT_PCM;
    output_format->nSamplesPerSec = input_format->wfx.nSamplesPerSec;
    output_format->nChannels = input_format->wfx.nChannels;
    output_format->wBitsPerSample = index ? 8 : 16;
    output_format->nBlockAlign = output_format->nChannels * output_format->wBitsPerSample / 8;
    output_format->nAvgBytesPerSec = output_format->nBlockAlign * output_format->nSamplesPerSec;

    memset(mt, 0, sizeof(*mt));
    mt->majortype = MEDIATYPE_Audio;
    mt->subtype = MEDIASUBTYPE_PCM;
    mt->bFixedSizeSamples = TRUE;
    mt->lSampleSize = output_format->nBlockAlign;
    mt->formattype = FORMAT_WaveFormatEx;
    mt->cbFormat = sizeof(*output_format);
    mt->pbFormat = (BYTE *)output_format;

    return S_OK;
}

static HRESULT mpeg_audio_codec_source_decide_buffer_size(struct transform *filter, IMemAllocator *allocator, ALLOCATOR_PROPERTIES *props)
{
    MPEG1WAVEFORMAT *input_format = (MPEG1WAVEFORMAT *)filter->sink.pin.mt.pbFormat;
    WAVEFORMATEX *output_format = (WAVEFORMATEX *)filter->source.pin.mt.pbFormat;
    LONG frame_samples = (input_format->fwHeadLayer & ACM_MPEG_LAYER2) ? 1152 : 384;
    LONG frame_size = frame_samples * output_format->nBlockAlign;
    ALLOCATOR_PROPERTIES ret_props;

    props->cBuffers = max(props->cBuffers, 8);
    props->cbBuffer = max(props->cbBuffer, frame_size * 4);
    props->cbAlign = max(props->cbAlign, 1);

    return IMemAllocator_SetProperties(allocator, props, &ret_props);
}

static const struct transform_ops mpeg_audio_codec_transform_ops =
{
    mpeg_audio_codec_sink_query_accept,
    mpeg_audio_codec_source_query_accept,
    mpeg_audio_codec_source_get_media_type,
    mpeg_audio_codec_source_decide_buffer_size,
    passthrough_source_qc_notify,
};

HRESULT mpeg_audio_codec_create(IUnknown *outer, IUnknown **out)
{
    struct transform *object;
    HRESULT hr;

    hr = transform_create(outer, &CLSID_CMpegAudioCodec, &mpeg_audio_codec_transform_ops, &object);
    if (FAILED(hr))
        return hr;

    wcscpy(object->sink.pin.name, L"XForm In");
    wcscpy(object->source.pin.name, L"XForm Out");

    object->IMpegAudioDecoder_iface.lpVtbl = &mpeg_audio_decoder_vtbl;

    TRACE("Created MPEG audio decoder %p.\n", object);
    *out = &object->filter.IUnknown_inner;
    return hr;
}

static HRESULT mpeg_video_codec_sink_query_accept(struct transform *filter, const AM_MEDIA_TYPE *mt)
{
    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Video)
            || (!IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1Payload)
                    && !is_mpeg2_video_subtype(&mt->subtype)
                    && !is_mpeg4_part2_video_subtype(&mt->subtype))
            || !mt->pbFormat)
        return S_FALSE;

    if (IsEqualGUID(&mt->formattype, &FORMAT_MPEGVideo) && mt->cbFormat >= sizeof(MPEG1VIDEOINFO))
        return S_OK;

    if (IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo) && mt->cbFormat >= sizeof(VIDEOINFOHEADER))
        return S_OK;

    return S_FALSE;
}

static HRESULT mpeg_video_codec_source_query_accept(struct transform *filter, const AM_MEDIA_TYPE *mt)
{
    if (!filter->sink.pin.peer)
        return S_FALSE;

    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Video)
            || !IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo)
            || mt->cbFormat < sizeof(VIDEOINFOHEADER))
        return S_FALSE;

    if (!IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_YV12)
            /* missing: MEDIASUBTYPE_Y41P, not supported by winedmo */
            && !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_YUY2)
            && !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_UYVY)
            && !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_RGB24)
            && !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_RGB32)
            && !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_RGB565)
            && !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_RGB555)
            /* missing: MEDIASUBTYPE_RGB8, not supported by winedmo */)
        return S_FALSE;

    return S_OK;
}

static HRESULT mpeg_video_codec_source_get_media_type(struct transform *filter, unsigned int index, AM_MEDIA_TYPE *mt)
{
    static const struct
    {
        const GUID *subtype;
        DWORD compression;
        WORD bit_count;
    } formats[] =
    {
        {&MEDIASUBTYPE_YV12,  MAKEFOURCC('Y','V','1','2'), 12},
        {&MEDIASUBTYPE_YUY2,  MAKEFOURCC('Y','U','Y','2'), 16},
        {&MEDIASUBTYPE_UYVY,  MAKEFOURCC('U','Y','V','Y'), 16},
        {&MEDIASUBTYPE_RGB24, BI_RGB, 24},
        {&MEDIASUBTYPE_RGB32, BI_RGB, 32},
        {&MEDIASUBTYPE_RGB565, BI_RGB, 16},
        {&MEDIASUBTYPE_RGB555, BI_RGB, 16},
    };

    const VIDEOINFOHEADER *input_format = (const VIDEOINFOHEADER *)filter->sink.pin.mt.pbFormat;
    VIDEOINFOHEADER *video_format;
    LONG width, height, stride, image_size;

    if (!filter->sink.pin.peer)
        return VFW_S_NO_MORE_ITEMS;

    if (index >= ARRAY_SIZE(formats))
        return VFW_S_NO_MORE_ITEMS;

    width = input_format->bmiHeader.biWidth;
    height = abs(input_format->bmiHeader.biHeight);

    if (formats[index].compression == MAKEFOURCC('Y','V','1','2'))
        image_size = width * height * 3 / 2;
    else
    {
        stride = (width * formats[index].bit_count / 8 + 3) & ~3;
        image_size = stride * height;
    }

    video_format = CoTaskMemAlloc(sizeof(*video_format));
    if (!video_format)
        return E_OUTOFMEMORY;

    memset(video_format, 0, sizeof(*video_format));
    video_format->AvgTimePerFrame = input_format->AvgTimePerFrame;
    video_format->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    video_format->bmiHeader.biWidth = width;
    video_format->bmiHeader.biHeight = height;
    video_format->bmiHeader.biPlanes = 1;
    video_format->bmiHeader.biBitCount = formats[index].bit_count;
    video_format->bmiHeader.biCompression = formats[index].compression;
    video_format->bmiHeader.biSizeImage = image_size;
    video_format->bmiHeader.biXPelsPerMeter = 2000;
    video_format->bmiHeader.biYPelsPerMeter = 2000;
    if (video_format->AvgTimePerFrame)
        video_format->dwBitRate = MulDiv(image_size * 8, 10000000, video_format->AvgTimePerFrame);
    SetRect(&video_format->rcSource, 0, 0, width, height);
    SetRect(&video_format->rcTarget, 0, 0, width, height);

    memset(mt, 0, sizeof(*mt));
    mt->majortype = MEDIATYPE_Video;
    mt->subtype = *formats[index].subtype;
    mt->bFixedSizeSamples = TRUE;
    mt->bTemporalCompression = FALSE;
    mt->lSampleSize = image_size;
    mt->formattype = FORMAT_VideoInfo;
    mt->cbFormat = sizeof(*video_format);
    mt->pbFormat = (BYTE *)video_format;

    return S_OK;
}

static HRESULT mpeg_video_codec_source_decide_buffer_size(struct transform *filter, IMemAllocator *allocator, ALLOCATOR_PROPERTIES *props)
{
    VIDEOINFOHEADER *output_format = (VIDEOINFOHEADER *)filter->source.pin.mt.pbFormat;
    ALLOCATOR_PROPERTIES ret_props;
    LONG frame_size = output_format->bmiHeader.biSizeImage;

    props->cBuffers = max(props->cBuffers, 1);
    if (frame_size >= 1024 * 1024 && props->cBuffers > 4)
    {
        TRACE("Capping large video allocator buffers from %ld to 4, frame size %ld.\n",
                props->cBuffers, frame_size);
        props->cBuffers = 4;
    }
    props->cbBuffer = max(props->cbBuffer, frame_size);
    props->cbAlign = max(props->cbAlign, 1);

    return IMemAllocator_SetProperties(allocator, props, &ret_props);
}

static const struct transform_ops mpeg_video_codec_transform_ops =
{
    mpeg_video_codec_sink_query_accept,
    mpeg_video_codec_source_query_accept,
    mpeg_video_codec_source_get_media_type,
    mpeg_video_codec_source_decide_buffer_size,
    passthrough_source_qc_notify,
};

HRESULT mpeg_video_codec_create(IUnknown *outer, IUnknown **out)
{
    struct transform *object;
    HRESULT hr;

    hr = transform_create(outer, &CLSID_CMpegVideoCodec, &mpeg_video_codec_transform_ops, &object);
    if (FAILED(hr))
        return hr;

    wcscpy(object->sink.pin.name, L"Input");
    wcscpy(object->source.pin.name, L"Output");
    object->synthesize_video_timestamps = true;

    TRACE("Created MPEG video decoder %p.\n", object);
    *out = &object->filter.IUnknown_inner;
    return hr;
}

static HRESULT mpeg_layer3_decoder_sink_query_accept(struct transform *filter, const AM_MEDIA_TYPE *mt)
{
    const MPEGLAYER3WAVEFORMAT *format;

    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Audio)
            || !IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx)
            || mt->cbFormat < sizeof(MPEGLAYER3WAVEFORMAT))
        return S_FALSE;

    format = (const MPEGLAYER3WAVEFORMAT *)mt->pbFormat;

    if (format->wfx.wFormatTag != WAVE_FORMAT_MPEGLAYER3)
        return S_FALSE;

    return S_OK;
}

static HRESULT mpeg_layer3_decoder_source_query_accept(struct transform *filter, const AM_MEDIA_TYPE *mt)
{
    if (!filter->sink.pin.peer)
        return S_FALSE;

    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Audio)
            || !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_PCM))
        return S_FALSE;

    return S_OK;
}

static HRESULT mpeg_layer3_decoder_source_get_media_type(struct transform *filter, unsigned int index, AM_MEDIA_TYPE *mt)
{
    const MPEGLAYER3WAVEFORMAT *input_format;
    WAVEFORMATEX *output_format;

    if (!filter->sink.pin.peer)
        return VFW_S_NO_MORE_ITEMS;

    if (index > 0)
        return VFW_S_NO_MORE_ITEMS;

    input_format = (const MPEGLAYER3WAVEFORMAT *)filter->sink.pin.mt.pbFormat;

    output_format = CoTaskMemAlloc(sizeof(*output_format));
    if (!output_format)
        return E_OUTOFMEMORY;

    memset(output_format, 0, sizeof(*output_format));
    output_format->wFormatTag = WAVE_FORMAT_PCM;
    output_format->nSamplesPerSec = input_format->wfx.nSamplesPerSec;
    output_format->nChannels = input_format->wfx.nChannels;
    output_format->wBitsPerSample = 16;
    output_format->nBlockAlign = output_format->nChannels * output_format->wBitsPerSample / 8;
    output_format->nAvgBytesPerSec = output_format->nBlockAlign * output_format->nSamplesPerSec;

    memset(mt, 0, sizeof(*mt));
    mt->majortype = MEDIATYPE_Audio;
    mt->subtype = MEDIASUBTYPE_PCM;
    mt->bFixedSizeSamples = TRUE;
    mt->lSampleSize = 1152 * output_format->nBlockAlign;
    mt->formattype = FORMAT_WaveFormatEx;
    mt->cbFormat = sizeof(*output_format);
    mt->pbFormat = (BYTE *)output_format;

    return S_OK;
}

static HRESULT mpeg_layer3_decoder_source_decide_buffer_size(struct transform *filter, IMemAllocator *allocator, ALLOCATOR_PROPERTIES *props)
{
    ALLOCATOR_PROPERTIES ret_props;

    props->cBuffers = max(props->cBuffers, 8);
    props->cbBuffer = max(props->cbBuffer, filter->source.pin.mt.lSampleSize * 4);
    props->cbAlign = max(props->cbAlign, 1);

    return IMemAllocator_SetProperties(allocator, props, &ret_props);
}

static const struct transform_ops mpeg_layer3_decoder_transform_ops =
{
    mpeg_layer3_decoder_sink_query_accept,
    mpeg_layer3_decoder_source_query_accept,
    mpeg_layer3_decoder_source_get_media_type,
    mpeg_layer3_decoder_source_decide_buffer_size,
    passthrough_source_qc_notify,
};

HRESULT mpeg_layer3_decoder_create(IUnknown *outer, IUnknown **out)
{
    struct transform *object;
    HRESULT hr;

    hr = transform_create(outer, &CLSID_mpeg_layer3_decoder, &mpeg_layer3_decoder_transform_ops, &object);
    if (FAILED(hr))
        return hr;

    wcscpy(object->sink.pin.name, L"XForm In");
    wcscpy(object->source.pin.name, L"XForm Out");

    TRACE("Created MPEG layer-3 decoder %p.\n", object);
    *out = &object->filter.IUnknown_inner;
    return hr;
}
