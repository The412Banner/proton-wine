/*
 * DirectShow parser filters backed by winedmo (ffmpeg) demuxer
 *
 * Copyright 2024 GloriousEggroll
 *
 * Based on quartz_parser.c:
 * Copyright 2010 Maarten Lankhorst for CodeWeavers
 * Copyright 2010 Aric Stewart for CodeWeavers
 * Copyright 2019-2020 Zebediah Figura
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
#include "wine/winedmo.h"
#include "wine/list.h"

#ifndef VFW_E_FLUSHING
#define VFW_E_FLUSHING ((HRESULT)0x80040228)
#endif

#include "amvideo.h"
#include "dvdmedia.h"
#include "mmreg.h"
#include "ks.h"
#include "ksmedia.h"

WINE_DEFAULT_DEBUG_CHANNEL(quartz);

/* ========================================================================
 * IMediaBuffer — lightweight wrapper for winedmo_demuxer_read
 * ======================================================================== */

struct simple_buffer
{
    IMediaBuffer IMediaBuffer_iface;
    LONG refcount;
    ULONG max_length;
    ULONG length;
    BYTE data[];
};

static inline struct simple_buffer *impl_from_IMediaBuffer(IMediaBuffer *iface)
{
    return CONTAINING_RECORD(iface, struct simple_buffer, IMediaBuffer_iface);
}

static HRESULT WINAPI simple_buffer_QueryInterface(IMediaBuffer *iface, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMediaBuffer))
    {
        *out = iface;
        IMediaBuffer_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI simple_buffer_AddRef(IMediaBuffer *iface)
{
    struct simple_buffer *buf = impl_from_IMediaBuffer(iface);
    return InterlockedIncrement(&buf->refcount);
}

static ULONG WINAPI simple_buffer_Release(IMediaBuffer *iface)
{
    struct simple_buffer *buf = impl_from_IMediaBuffer(iface);
    ULONG ref = InterlockedDecrement(&buf->refcount);
    if (!ref) free(buf);
    return ref;
}

static HRESULT WINAPI simple_buffer_SetLength(IMediaBuffer *iface, DWORD len)
{
    struct simple_buffer *buf = impl_from_IMediaBuffer(iface);
    if (len > buf->max_length) return E_INVALIDARG;
    buf->length = len;
    return S_OK;
}

static HRESULT WINAPI simple_buffer_GetMaxLength(IMediaBuffer *iface, DWORD *out)
{
    struct simple_buffer *buf = impl_from_IMediaBuffer(iface);
    *out = buf->max_length;
    return S_OK;
}

static HRESULT WINAPI simple_buffer_GetBufferAndLength(IMediaBuffer *iface, BYTE **data, DWORD *len)
{
    struct simple_buffer *buf = impl_from_IMediaBuffer(iface);
    if (data) *data = buf->data;
    if (len) *len = buf->length;
    return S_OK;
}

static const IMediaBufferVtbl simple_buffer_vtbl =
{
    simple_buffer_QueryInterface,
    simple_buffer_AddRef,
    simple_buffer_Release,
    simple_buffer_SetLength,
    simple_buffer_GetMaxLength,
    simple_buffer_GetBufferAndLength,
};

static struct simple_buffer *simple_buffer_create(ULONG size)
{
    struct simple_buffer *buf = malloc(offsetof(struct simple_buffer, data[size]));
    if (!buf) return NULL;
    buf->IMediaBuffer_iface.lpVtbl = &simple_buffer_vtbl;
    buf->refcount = 1;
    buf->max_length = size;
    buf->length = 0;
    return buf;
}

/* ========================================================================
 * Packet queue entries
 * ======================================================================== */

struct dmo_packet
{
    struct list entry;
    BYTE *data;
    DWORD size;
    unsigned int generation;
    bool has_pts, has_duration, sync_point;
    REFERENCE_TIME pts, duration;
};

static void dmo_packet_free(struct dmo_packet *pkt)
{
    free(pkt->data);
    free(pkt);
}

static void dmo_packet_queue_flush(struct list *queue)
{
    struct dmo_packet *pkt, *next;
    LIST_FOR_EACH_ENTRY_SAFE(pkt, next, queue, struct dmo_packet, entry)
    {
        list_remove(&pkt->entry);
        dmo_packet_free(pkt);
    }
}

/* ========================================================================
 * Structures
 * ======================================================================== */

struct dmo_parser;

#define DMO_PARSER_MAX_QUEUED_PACKETS 64

struct dmo_parser_source
{
    struct strmbase_source pin;
    IQualityControl IQualityControl_iface;

    UINT stream_index;
    AM_MEDIA_TYPE stream_mt;   /* format reported by winedmo for this stream */
    INT64 duration;            /* file duration in 100ns units */

    struct list packet_queue;
    unsigned int queued_packets;
    CRITICAL_SECTION queue_cs;
    CONDITION_VARIABLE queue_cv;
    CONDITION_VARIABLE queue_space_cv;

    /* held during IMemInputPin_Receive; seeking acquires to serialize */
    CRITICAL_SECTION flushing_cs;
    CONDITION_VARIABLE eos_cv;

    bool eos;       /* demuxer reached end for this stream */
    bool eos_sent;  /* EndOfStream sent downstream */
    bool selected;

    SourceSeeking seek;
    HANDLE thread;
    bool need_segment;
};

struct dmo_parser
{
    struct strmbase_filter filter;
    IAMStreamSelect IAMStreamSelect_iface;

    struct strmbase_sink sink;
    IAsyncReader *reader;

    struct dmo_parser_source **sources;
    unsigned int source_count;
    BOOL enum_sink_first;

    struct winedmo_demuxer winedmo_demuxer;
    struct
    {
        struct winedmo_stream stream;
        LONGLONG position;
        LONGLONG file_size;
    } winedmo_stream;

    /* Set TRUE to make the read callback return 0 bytes (interrupts demuxer). */
    BOOL read_flushing;
    unsigned int read_flush_generation;

    CRITICAL_SECTION streaming_cs;
    CONDITION_VARIABLE flushing_cv;
    bool streaming;
    bool sink_connected;
    bool flushing;
    bool downstream_flushing;
    unsigned int max_queued_packets;
    REFERENCE_TIME stream_start;

    HANDLE demux_thread;

    BOOL (*init_streams)(struct dmo_parser *filter, UINT stream_count, INT64 duration);
    BOOL (*check_mime_type)(const WCHAR *mime_type);
    HRESULT (*source_query_accept)(struct dmo_parser_source *pin, const AM_MEDIA_TYPE *mt);
    HRESULT (*source_get_media_type)(struct dmo_parser_source *pin, unsigned int index, AM_MEDIA_TYPE *mt);
};

static void dmo_parser_source_flush_queue(struct dmo_parser_source *pin)
{
    dmo_packet_queue_flush(&pin->packet_queue);
    pin->queued_packets = 0;
    WakeConditionVariable(&pin->queue_space_cv);
}

/* ========================================================================
 * impl_from helpers
 * ======================================================================== */

static inline struct dmo_parser *dmo_impl_from_strmbase_filter(struct strmbase_filter *iface)
{
    return CONTAINING_RECORD(iface, struct dmo_parser, filter);
}

static inline struct dmo_parser *dmo_impl_from_strmbase_sink(struct strmbase_sink *iface)
{
    return CONTAINING_RECORD(iface, struct dmo_parser, sink);
}

static inline struct dmo_parser_source *dmo_impl_from_strmbase_source(struct strmbase_pin *iface)
{
    return CONTAINING_RECORD(iface, struct dmo_parser_source, pin.pin);
}

static void dmo_parser_begin_downstream_flush(struct dmo_parser *filter);
static void dmo_parser_end_downstream_flush(struct dmo_parser *filter);

static inline struct dmo_parser *dmo_impl_from_IAMStreamSelect(IAMStreamSelect *iface)
{
    return CONTAINING_RECORD(iface, struct dmo_parser, IAMStreamSelect_iface);
}

static inline struct dmo_parser_source *dmo_impl_from_IQualityControl(IQualityControl *iface)
{
    return CONTAINING_RECORD(iface, struct dmo_parser_source, IQualityControl_iface);
}

static inline struct dmo_parser_source *dmo_impl_from_IMediaSeeking(IMediaSeeking *iface)
{
    return CONTAINING_RECORD(iface, struct dmo_parser_source, seek.IMediaSeeking_iface);
}

static void dmo_parser_source_wait_for_video_time(struct dmo_parser_source *pin, struct dmo_packet *pkt)
{
    struct dmo_parser *filter = dmo_impl_from_strmbase_filter(pin->pin.pin.filter);
    IReferenceClock *clock;
    REFERENCE_TIME sample_time, target;

    if (!pkt->has_pts || !IsEqualGUID(&pin->stream_mt.majortype, &MEDIATYPE_Video)
            || pin->stream_mt.bTemporalCompression)
        return;

    sample_time = pkt->pts - pin->seek.llCurrent;
    if (sample_time < 0)
        return;

    EnterCriticalSection(&filter->filter.filter_cs);
    clock = filter->filter.clock;
    if (clock)
        IReferenceClock_AddRef(clock);
    target = filter->stream_start + sample_time;
    LeaveCriticalSection(&filter->filter.filter_cs);

    if (!clock)
        return;

    for (;;)
    {
        REFERENCE_TIME now, remaining;
        DWORD sleep_ms;

        EnterCriticalSection(&filter->streaming_cs);
        if (!filter->streaming || filter->flushing)
        {
            LeaveCriticalSection(&filter->streaming_cs);
            break;
        }
        LeaveCriticalSection(&filter->streaming_cs);

        if (FAILED(IReferenceClock_GetTime(clock, &now)))
            break;
        remaining = target - now;
        if (remaining <= 0)
            break;

        sleep_ms = remaining / 10000;
        Sleep(min(max(sleep_ms, 1), 10));
    }

    IReferenceClock_Release(clock);
}

static BOOL dmo_parser_source_is_active(const struct dmo_parser_source *pin)
{
    return pin->selected && pin->pin.pin.peer;
}

static BOOL dmo_parser_has_selected_stream_type(const struct dmo_parser *filter, const GUID *major_type)
{
    unsigned int i;

    for (i = 0; i < filter->source_count; ++i)
    {
        if (filter->sources[i]->selected && IsEqualGUID(&filter->sources[i]->stream_mt.majortype, major_type))
            return TRUE;
    }

    return FALSE;
}

/* ========================================================================
 * winedmo stream callbacks (IAsyncReader bridge)
 * ======================================================================== */

static NTSTATUS CDECL dmo_stream_seek_cb(struct winedmo_stream *stream, UINT64 *pos)
{
    struct dmo_parser *filter = CONTAINING_RECORD(stream, struct dmo_parser, winedmo_stream.stream);
    filter->winedmo_stream.position = *pos;
    return STATUS_SUCCESS;
}

static NTSTATUS CDECL dmo_stream_read_cb(struct winedmo_stream *stream, BYTE *buffer, ULONG *size)
{
    struct dmo_parser *filter = CONTAINING_RECORD(stream, struct dmo_parser, winedmo_stream.stream);
    LONGLONG pos = filter->winedmo_stream.position;
    LONGLONG avail;
    HRESULT hr;

    if (filter->read_flushing)
    {
        *size = 0;
        return STATUS_SUCCESS;
    }

    avail = filter->winedmo_stream.file_size - pos;
    if (avail <= 0)
    {
        *size = 0;
        return STATUS_SUCCESS;
    }

    if ((LONGLONG)*size > avail)
        *size = (ULONG)avail;

    hr = IAsyncReader_SyncRead(filter->reader, pos, *size, buffer);
    if (FAILED(hr))
    {
        WARN("SyncRead at %I64d size %lu failed, hr %#lx.\n", pos, *size, hr);
        return STATUS_UNSUCCESSFUL;
    }

    filter->winedmo_stream.position += *size;
    return STATUS_SUCCESS;
}

/* ========================================================================
 * Format conversion: winedmo → AM_MEDIA_TYPE
 * ======================================================================== */

static HRESULT amt_from_winedmo_audio(AM_MEDIA_TYPE *mt, const WAVEFORMATEX *wfx, UINT32 format_size)
{
    WAVEFORMATEX *dst;
    GUID subtype;

    if (format_size < sizeof(WAVEFORMATEX))
        return E_INVALIDARG;

    if (!(dst = CoTaskMemAlloc(format_size)))
        return E_OUTOFMEMORY;
    memcpy(dst, wfx, format_size);

    memset(mt, 0, sizeof(*mt));
    mt->majortype = MEDIATYPE_Audio;
    mt->formattype = FORMAT_WaveFormatEx;
    mt->pbFormat = (BYTE *)dst;
    mt->cbFormat = format_size;

    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wfx;
        mt->subtype = ext->SubFormat;
    }
    else
    {
        subtype.Data1 = wfx->wFormatTag;
        subtype.Data2 = 0;
        subtype.Data3 = 0x0010;
        subtype.Data4[0] = 0x80; subtype.Data4[1] = 0x00;
        subtype.Data4[2] = 0x00; subtype.Data4[3] = 0xaa;
        subtype.Data4[4] = 0x00; subtype.Data4[5] = 0x38;
        subtype.Data4[6] = 0x9b; subtype.Data4[7] = 0x71;
        mt->subtype = subtype;
    }

    mt->bTemporalCompression = TRUE;
    mt->lSampleSize = wfx->nBlockAlign ? wfx->nBlockAlign : 1;
    return S_OK;
}

static HRESULT amt_from_winedmo_video(AM_MEDIA_TYPE *mt, const MFVIDEOFORMAT *mfvf, UINT32 format_size)
{
    DWORD extradata_size = (format_size > sizeof(MFVIDEOFORMAT)) ? format_size - sizeof(MFVIDEOFORMAT) : 0;
    DWORD vih_size = sizeof(VIDEOINFOHEADER) + extradata_size;
    VIDEOINFOHEADER *vih;

    if (IsEqualGUID(&mfvf->guidFormat, &MEDIASUBTYPE_MPEG1Payload))
    {
        MPEG1VIDEOINFO *mpeg_vih;
        DWORD mpeg_vih_size = max(sizeof(*mpeg_vih), offsetof(MPEG1VIDEOINFO, bSequenceHeader) + extradata_size);

        if (!(mpeg_vih = CoTaskMemAlloc(mpeg_vih_size)))
            return E_OUTOFMEMORY;
        memset(mpeg_vih, 0, mpeg_vih_size);

        mpeg_vih->hdr.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        mpeg_vih->hdr.bmiHeader.biWidth = mfvf->videoInfo.dwWidth;
        mpeg_vih->hdr.bmiHeader.biHeight = mfvf->videoInfo.dwHeight;
        mpeg_vih->hdr.bmiHeader.biPlanes = 1;
        mpeg_vih->hdr.bmiHeader.biBitCount = 12;
        mpeg_vih->hdr.bmiHeader.biCompression = mfvf->guidFormat.Data1;
        mpeg_vih->hdr.bmiHeader.biSizeImage = mfvf->videoInfo.dwWidth * mfvf->videoInfo.dwHeight * 3 / 2;

        if (mfvf->videoInfo.FramesPerSecond.Numerator && mfvf->videoInfo.FramesPerSecond.Denominator)
            mpeg_vih->hdr.AvgTimePerFrame = (LONGLONG)10000000
                    * mfvf->videoInfo.FramesPerSecond.Denominator
                    / mfvf->videoInfo.FramesPerSecond.Numerator;

        if (extradata_size)
        {
            mpeg_vih->cbSequenceHeader = extradata_size;
            memcpy(mpeg_vih->bSequenceHeader, mfvf + 1, extradata_size);
        }

        memset(mt, 0, sizeof(*mt));
        mt->majortype = MEDIATYPE_Video;
        mt->subtype = mfvf->guidFormat;
        mt->formattype = FORMAT_MPEGVideo;
        mt->pbFormat = (BYTE *)mpeg_vih;
        mt->cbFormat = mpeg_vih_size;
        mt->bTemporalCompression = TRUE;
        mt->lSampleSize = 1;
        return S_OK;
    }

    if (!(vih = CoTaskMemAlloc(vih_size)))
        return E_OUTOFMEMORY;
    memset(vih, 0, vih_size);

    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER) + extradata_size;
    vih->bmiHeader.biWidth = mfvf->videoInfo.dwWidth;
    vih->bmiHeader.biHeight = mfvf->videoInfo.dwHeight;
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biCompression = mfvf->guidFormat.Data1; /* FOURCC */
    vih->bmiHeader.biBitCount = 24;

    if (mfvf->videoInfo.FramesPerSecond.Numerator && mfvf->videoInfo.FramesPerSecond.Denominator)
        vih->AvgTimePerFrame = (LONGLONG)10000000
                * mfvf->videoInfo.FramesPerSecond.Denominator
                / mfvf->videoInfo.FramesPerSecond.Numerator;

    if (extradata_size)
        memcpy(vih + 1, mfvf + 1, extradata_size);

    memset(mt, 0, sizeof(*mt));
    mt->majortype = MEDIATYPE_Video;
    mt->subtype = mfvf->guidFormat;
    mt->formattype = FORMAT_VideoInfo;
    mt->pbFormat = (BYTE *)vih;
    mt->cbFormat = vih_size;
    mt->bTemporalCompression = TRUE;
    return S_OK;
}

static HRESULT amt_from_winedmo_demuxer_format(AM_MEDIA_TYPE *mt, const GUID *major,
        const union winedmo_format *format, UINT32 format_size)
{
    if (IsEqualGUID(major, &MFMediaType_Audio))
        return amt_from_winedmo_audio(mt, &format->audio, format_size);
    if (IsEqualGUID(major, &MFMediaType_Video))
        return amt_from_winedmo_video(mt, &format->video, format_size);

    FIXME("Unsupported major type %s.\n", debugstr_guid(major));
    return E_NOTIMPL;
}

/* ========================================================================
 * Seeking — IMediaSeeking forwarded from output pins
 * ======================================================================== */

static HRESULT WINAPI dmo_seeking_QueryInterface(IMediaSeeking *iface, REFIID iid, void **out)
{
    struct dmo_parser_source *pin = dmo_impl_from_IMediaSeeking(iface);
    return IPin_QueryInterface(&pin->pin.pin.IPin_iface, iid, out);
}

static ULONG WINAPI dmo_seeking_AddRef(IMediaSeeking *iface)
{
    struct dmo_parser_source *pin = dmo_impl_from_IMediaSeeking(iface);
    return IPin_AddRef(&pin->pin.pin.IPin_iface);
}

static ULONG WINAPI dmo_seeking_Release(IMediaSeeking *iface)
{
    struct dmo_parser_source *pin = dmo_impl_from_IMediaSeeking(iface);
    return IPin_Release(&pin->pin.pin.IPin_iface);
}

static HRESULT WINAPI dmo_seeking_ChangeCurrent(IMediaSeeking *iface)
{
    /* Handled in SetPositions. */
    return S_OK;
}

static HRESULT WINAPI dmo_seeking_ChangeStop(IMediaSeeking *iface)
{
    return S_OK;
}

static HRESULT WINAPI dmo_seeking_ChangeRate(IMediaSeeking *iface)
{
    return S_OK;
}

static HRESULT WINAPI dmo_seeking_GetDuration(IMediaSeeking *iface, LONGLONG *duration)
{
    struct dmo_parser_source *pin = dmo_impl_from_IMediaSeeking(iface);

    if (pin->seek.llDuration < 0)
        return E_NOTIMPL;

    return SourceSeekingImpl_GetDuration(iface, duration);
}

static HRESULT WINAPI dmo_seeking_GetStopPosition(IMediaSeeking *iface, LONGLONG *stop)
{
    struct dmo_parser_source *pin = dmo_impl_from_IMediaSeeking(iface);

    if (pin->seek.llStop < 0)
        return E_NOTIMPL;

    return SourceSeekingImpl_GetStopPosition(iface, stop);
}

static HRESULT WINAPI dmo_seeking_SetPositions(IMediaSeeking *iface,
        LONGLONG *current, DWORD current_flags, LONGLONG *stop, DWORD stop_flags)
{
    struct dmo_parser_source *pin = dmo_impl_from_IMediaSeeking(iface);
    struct dmo_parser *filter = dmo_impl_from_strmbase_filter(pin->pin.pin.filter);
    DWORD current_positioning = current_flags & AM_SEEKING_PositioningBitsMask;
    bool current_unchanged;
    unsigned int i;

    TRACE("pin %p, current %s, current_flags %#lx, stop %s, stop_flags %#lx.\n",
            pin, current ? debugstr_time(*current) : "<null>", current_flags,
            stop ? debugstr_time(*stop) : "<null>", stop_flags);

    if (pin->pin.pin.filter->state == State_Stopped)
    {
        SourceSeekingImpl_SetPositions(iface, current, current_flags, stop, stop_flags);
        return S_OK;
    }

    current_unchanged = !current || current_positioning == AM_SEEKING_NoPositioning
            || (current_positioning == AM_SEEKING_AbsolutePositioning && *current == pin->seek.llCurrent)
            || (current_positioning == AM_SEEKING_RelativePositioning && !*current)
            || (current_positioning == AM_SEEKING_IncrementalPositioning && !*current);
    if (current_unchanged)
        return SourceSeekingImpl_SetPositions(iface, current, current_flags, stop, stop_flags);

    /* Tell the read callback to return 0 bytes so the demux thread unblocks. */
    EnterCriticalSection(&filter->streaming_cs);
    filter->flushing = true;
    filter->read_flushing = TRUE;
    ++filter->read_flush_generation;
    LeaveCriticalSection(&filter->streaming_cs);
    WakeAllConditionVariable(&filter->flushing_cv);

    /* Wake stream threads that may be sleeping on queue_cv. */
    for (i = 0; i < filter->source_count; ++i)
    {
        WakeConditionVariable(&filter->sources[i]->queue_cv);
        WakeConditionVariable(&filter->sources[i]->queue_space_cv);
    }

    /* Acquire per-pin flushing_cs to ensure no Receive is in progress. */
    for (i = 0; i < filter->source_count; ++i)
    {
        if (dmo_parser_source_is_active(filter->sources[i]))
            EnterCriticalSection(&filter->sources[i]->flushing_cs);
    }

    if (!(current_flags & AM_SEEKING_NoFlush))
        dmo_parser_begin_downstream_flush(filter);

    SourceSeekingImpl_SetPositions(iface, current, current_flags, stop, stop_flags);

    /* Perform the actual seek. */
    {
        LONGLONG seek_time = filter->sources[0]->seek.llCurrent;
        NTSTATUS status;
        WARN("DirectShow parser SetPositions seek to %s.\n", debugstr_time(seek_time));
        status = winedmo_demuxer_seek(filter->winedmo_demuxer, seek_time);
        if (status)
            WARN("winedmo_demuxer_seek failed, status %#lx.\n", status);
    }

    /* Clear all packet queues and reset state. */
    for (i = 0; i < filter->source_count; ++i)
    {
        struct dmo_parser_source *src = filter->sources[i];
        EnterCriticalSection(&src->queue_cs);
        dmo_parser_source_flush_queue(src);
        src->eos = false;
        src->eos_sent = false;
        src->need_segment = true;
        LeaveCriticalSection(&src->queue_cs);
    }

    /* Release per-pin locks in reverse order. */
    for (i = filter->source_count; i-- > 0;)
    {
        if (dmo_parser_source_is_active(filter->sources[i]))
        {
            WakeConditionVariable(&filter->sources[i]->eos_cv);
            WakeConditionVariable(&filter->sources[i]->queue_cv);
            WakeConditionVariable(&filter->sources[i]->queue_space_cv);
            LeaveCriticalSection(&filter->sources[i]->flushing_cs);
        }
    }

    if (!(current_flags & AM_SEEKING_NoFlush))
        dmo_parser_end_downstream_flush(filter);

    EnterCriticalSection(&filter->streaming_cs);
    filter->flushing = false;
    filter->read_flushing = FALSE;
    LeaveCriticalSection(&filter->streaming_cs);
    WakeAllConditionVariable(&filter->flushing_cv);

    return S_OK;
}

static const IMediaSeekingVtbl dmo_seeking_vtbl =
{
    dmo_seeking_QueryInterface,
    dmo_seeking_AddRef,
    dmo_seeking_Release,
    SourceSeekingImpl_GetCapabilities,
    SourceSeekingImpl_CheckCapabilities,
    SourceSeekingImpl_IsFormatSupported,
    SourceSeekingImpl_QueryPreferredFormat,
    SourceSeekingImpl_GetTimeFormat,
    SourceSeekingImpl_IsUsingTimeFormat,
    SourceSeekingImpl_SetTimeFormat,
    dmo_seeking_GetDuration,
    dmo_seeking_GetStopPosition,
    SourceSeekingImpl_GetCurrentPosition,
    SourceSeekingImpl_ConvertTimeFormat,
    dmo_seeking_SetPositions,
    SourceSeekingImpl_GetPositions,
    SourceSeekingImpl_GetAvailable,
    SourceSeekingImpl_SetRate,
    SourceSeekingImpl_GetRate,
    SourceSeekingImpl_GetPreroll,
};

/* ========================================================================
 * IQualityControl (stub)
 * ======================================================================== */

static HRESULT WINAPI dmo_qc_QueryInterface(IQualityControl *iface, REFIID iid, void **out)
{
    struct dmo_parser_source *pin = dmo_impl_from_IQualityControl(iface);
    return IPin_QueryInterface(&pin->pin.pin.IPin_iface, iid, out);
}

static ULONG WINAPI dmo_qc_AddRef(IQualityControl *iface)
{
    struct dmo_parser_source *pin = dmo_impl_from_IQualityControl(iface);
    return IPin_AddRef(&pin->pin.pin.IPin_iface);
}

static ULONG WINAPI dmo_qc_Release(IQualityControl *iface)
{
    struct dmo_parser_source *pin = dmo_impl_from_IQualityControl(iface);
    return IPin_Release(&pin->pin.pin.IPin_iface);
}

static HRESULT WINAPI dmo_qc_Notify(IQualityControl *iface, IBaseFilter *sender, Quality q)
{
    return S_OK;
}

static HRESULT WINAPI dmo_qc_SetSink(IQualityControl *iface, IQualityControl *sink)
{
    return S_OK;
}

static const IQualityControlVtbl dmo_qc_vtbl =
{
    dmo_qc_QueryInterface,
    dmo_qc_AddRef,
    dmo_qc_Release,
    dmo_qc_Notify,
    dmo_qc_SetSink,
};

/* ========================================================================
 * IAMStreamSelect
 * ======================================================================== */

static HRESULT WINAPI dmo_stream_select_QueryInterface(IAMStreamSelect *iface, REFIID iid, void **out)
{
    struct dmo_parser *filter = dmo_impl_from_IAMStreamSelect(iface);
    return IUnknown_QueryInterface(filter->filter.outer_unk, iid, out);
}

static ULONG WINAPI dmo_stream_select_AddRef(IAMStreamSelect *iface)
{
    struct dmo_parser *filter = dmo_impl_from_IAMStreamSelect(iface);
    return IUnknown_AddRef(filter->filter.outer_unk);
}

static ULONG WINAPI dmo_stream_select_Release(IAMStreamSelect *iface)
{
    struct dmo_parser *filter = dmo_impl_from_IAMStreamSelect(iface);
    return IUnknown_Release(filter->filter.outer_unk);
}

static HRESULT WINAPI dmo_stream_select_Count(IAMStreamSelect *iface, DWORD *count)
{
    struct dmo_parser *filter = dmo_impl_from_IAMStreamSelect(iface);
    *count = filter->source_count;
    return S_OK;
}

static HRESULT WINAPI dmo_stream_select_Info(IAMStreamSelect *iface, LONG index,
        AM_MEDIA_TYPE **ppmt, DWORD *flags, LCID *lcid, DWORD *group, WCHAR **name,
        IUnknown **object, IUnknown **unknown)
{
    struct dmo_parser *filter = dmo_impl_from_IAMStreamSelect(iface);
    struct dmo_parser_source *pin;

    if (index < 0 || (UINT)index >= filter->source_count)
        return S_FALSE;

    pin = filter->sources[index];

    if (ppmt)
    {
        if (!(*ppmt = CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE))))
            return E_OUTOFMEMORY;
        CopyMediaType(*ppmt, &pin->stream_mt);
    }
    if (flags)
        *flags = (pin->selected ? AMSTREAMSELECTINFO_ENABLED : 0) | AMSTREAMSELECTINFO_EXCLUSIVE;
    if (lcid)
        *lcid = 0;
    if (group)
        *group = IsEqualGUID(&pin->stream_mt.majortype, &MEDIATYPE_Video) ? 0 : 1;
    if (name)
    {
        const WCHAR *pin_name = pin->pin.pin.name;
        size_t len = (wcslen(pin_name) + 1) * sizeof(WCHAR);
        if (!(*name = CoTaskMemAlloc(len)))
            return E_OUTOFMEMORY;
        memcpy(*name, pin_name, len);
    }
    if (object)
    {
        *object = (IUnknown *)&pin->pin.pin.IPin_iface;
        IUnknown_AddRef(*object);
    }
    if (unknown)
        *unknown = NULL;
    return S_OK;
}

static HRESULT WINAPI dmo_stream_select_Enable(IAMStreamSelect *iface, LONG index, DWORD flags)
{
    struct dmo_parser *filter = dmo_impl_from_IAMStreamSelect(iface);
    struct dmo_parser_source *pin;
    unsigned int i;

    TRACE("iface %p, index %ld, flags %#lx.\n", iface, index, flags);

    if (index < 0 || (UINT)index >= filter->source_count)
        return S_FALSE;

    pin = filter->sources[index];
    if (!(flags & AMSTREAMSELECTENABLE_ENABLE))
    {
        pin->selected = false;
        return S_OK;
    }

    for (i = 0; i < filter->source_count; ++i)
    {
        struct dmo_parser_source *other = filter->sources[i];

        if (other == pin)
            continue;
        if (IsEqualGUID(&other->stream_mt.majortype, &pin->stream_mt.majortype))
            other->selected = false;
    }

    pin->selected = true;
    return S_OK;
}

static const IAMStreamSelectVtbl dmo_stream_select_vtbl =
{
    dmo_stream_select_QueryInterface,
    dmo_stream_select_AddRef,
    dmo_stream_select_Release,
    dmo_stream_select_Count,
    dmo_stream_select_Info,
    dmo_stream_select_Enable,
};

/* ========================================================================
 * Source pin operations
 * ======================================================================== */

static HRESULT dmo_source_query_interface(struct strmbase_pin *iface, REFIID iid, void **out)
{
    struct dmo_parser_source *pin = dmo_impl_from_strmbase_source(iface);

    if (IsEqualGUID(iid, &IID_IMediaSeeking))
        *out = &pin->seek.IMediaSeeking_iface;
    else if (IsEqualGUID(iid, &IID_IQualityControl))
        *out = &pin->IQualityControl_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT dmo_source_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    struct dmo_parser_source *pin = dmo_impl_from_strmbase_source(iface);
    struct dmo_parser *filter = dmo_impl_from_strmbase_filter(iface->filter);
    return filter->source_query_accept(pin, mt);
}

static HRESULT dmo_source_get_media_type(struct strmbase_pin *iface, unsigned int index, AM_MEDIA_TYPE *mt)
{
    struct dmo_parser_source *pin = dmo_impl_from_strmbase_source(iface);
    struct dmo_parser *filter = dmo_impl_from_strmbase_filter(iface->filter);
    return filter->source_get_media_type(pin, index, mt);
}

static HRESULT WINAPI dmo_source_decide_buffer_size(struct strmbase_source *iface,
        IMemAllocator *allocator, ALLOCATOR_PROPERTIES *props)
{
    struct dmo_parser_source *pin = CONTAINING_RECORD(iface, struct dmo_parser_source, pin);
    ALLOCATOR_PROPERTIES ret_props;
    DWORD buffer_size = 0x10000;
    DWORD buffer_count = 1;

    if (IsEqualGUID(&pin->pin.pin.mt.formattype, &FORMAT_WaveFormatEx))
    {
        const WAVEFORMATEX *wfx = (const WAVEFORMATEX *)pin->pin.pin.mt.pbFormat;
        if (IsEqualGUID(&pin->pin.pin.mt.subtype, &MEDIASUBTYPE_PCM)
                || IsEqualGUID(&pin->pin.pin.mt.subtype, &MEDIASUBTYPE_IEEE_FLOAT))
            buffer_size = wfx->nAvgBytesPerSec;
        else
            buffer_size = max(wfx->nBlockAlign * 8, 0x10000);
        buffer_count = 4;
    }
    else if (IsEqualGUID(&pin->pin.pin.mt.formattype, &FORMAT_VideoInfo)
            || IsEqualGUID(&pin->pin.pin.mt.formattype, &FORMAT_MPEGVideo))
    {
        /* Compressed video: give generous buffer for one frame. */
        const VIDEOINFOHEADER *vih = (const VIDEOINFOHEADER *)pin->pin.pin.mt.pbFormat;
        LONG height = vih->bmiHeader.biHeight < 0 ? -vih->bmiHeader.biHeight : vih->bmiHeader.biHeight;
        buffer_size = max((DWORD)(vih->bmiHeader.biWidth * height * 3 / 2), 0x40000);
        buffer_count = 8;
    }

    props->cBuffers = max(props->cBuffers, (LONG)buffer_count);
    props->cbBuffer = max(props->cbBuffer, (LONG)buffer_size);
    props->cbAlign = max(props->cbAlign, 1);
    return IMemAllocator_SetProperties(allocator, props, &ret_props);
}

static const struct strmbase_source_ops dmo_source_ops =
{
    .base.pin_query_interface = dmo_source_query_interface,
    .base.pin_query_accept = dmo_source_query_accept,
    .base.pin_get_media_type = dmo_source_get_media_type,
    .pfnAttemptConnection = BaseOutputPinImpl_AttemptConnection,
    .pfnDecideAllocator = BaseOutputPinImpl_DecideAllocator,
    .pfnDecideBufferSize = dmo_source_decide_buffer_size,
};

/* ========================================================================
 * Pin creation / destruction
 * ======================================================================== */

static void dmo_free_source_pin(struct dmo_parser_source *pin)
{
    if (pin->pin.pin.peer)
    {
        if (SUCCEEDED(IMemAllocator_Decommit(pin->pin.pAllocator)))
            IPin_Disconnect(pin->pin.pin.peer);
        IPin_Disconnect(&pin->pin.pin.IPin_iface);
    }

    dmo_parser_source_flush_queue(pin);

    pin->queue_cs.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&pin->queue_cs);
    pin->flushing_cs.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&pin->flushing_cs);
    FreeMediaType(&pin->stream_mt);
    strmbase_seeking_cleanup(&pin->seek);
    strmbase_source_cleanup(&pin->pin);
    free(pin);
}

static struct dmo_parser_source *create_dmo_pin(struct dmo_parser *filter,
        UINT stream_index, const WCHAR *name, const AM_MEDIA_TYPE *mt, INT64 duration)
{
    struct dmo_parser_source *pin, **new_array;

    if (!(new_array = realloc(filter->sources,
            (filter->source_count + 1) * sizeof(*filter->sources))))
        return NULL;
    filter->sources = new_array;

    if (!(pin = calloc(1, sizeof(*pin))))
        return NULL;

    pin->stream_index = stream_index;
    pin->duration = duration;
    list_init(&pin->packet_queue);

    strmbase_source_init(&pin->pin, &filter->filter, name, &dmo_source_ops);
    pin->IQualityControl_iface.lpVtbl = &dmo_qc_vtbl;
    strmbase_seeking_init(&pin->seek, &dmo_seeking_vtbl,
            dmo_seeking_ChangeStop, dmo_seeking_ChangeCurrent, dmo_seeking_ChangeRate);
    pin->seek.llDuration = duration;
    pin->seek.llStop = duration;

    InitializeCriticalSectionEx(&pin->queue_cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    pin->queue_cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": pin.queue_cs");
    InitializeConditionVariable(&pin->queue_cv);
    InitializeConditionVariable(&pin->queue_space_cv);

    InitializeCriticalSectionEx(&pin->flushing_cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    pin->flushing_cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": pin.flushing_cs");
    InitializeConditionVariable(&pin->eos_cv);
    CopyMediaType(&pin->stream_mt, mt);
    pin->selected = IsEqualGUID(&mt->majortype, &MEDIATYPE_Video)
            || !dmo_parser_has_selected_stream_type(filter, &mt->majortype);
    BaseFilterImpl_IncrementPinVersion(&filter->filter);

    filter->sources[filter->source_count++] = pin;
    return pin;
}

/* ========================================================================
 * Demux thread — reads from winedmo and routes to per-stream queues
 * ======================================================================== */

static void dmo_parser_source_send_eos(struct dmo_parser_source *pin);

static void dmo_parser_wait_for_seek_or_stop(struct dmo_parser *filter)
{
    EnterCriticalSection(&filter->streaming_cs);
    while (filter->streaming && !filter->flushing)
        SleepConditionVariableCS(&filter->flushing_cv, &filter->streaming_cs, INFINITE);
    LeaveCriticalSection(&filter->streaming_cs);
}

static void dmo_parser_wait_for_queue_space(struct dmo_parser *filter)
{
    unsigned int i;

    for (i = 0; i < filter->source_count; ++i)
    {
        struct dmo_parser_source *pin = filter->sources[i];

        if (!dmo_parser_source_is_active(pin))
            continue;

        EnterCriticalSection(&pin->queue_cs);
        while (pin->queued_packets >= filter->max_queued_packets && !pin->eos)
        {
            bool wait;

            EnterCriticalSection(&filter->streaming_cs);
            wait = filter->streaming && !filter->flushing;
            LeaveCriticalSection(&filter->streaming_cs);
            if (!wait)
                break;

            SleepConditionVariableCS(&pin->queue_space_cv, &pin->queue_cs, INFINITE);
        }
        LeaveCriticalSection(&pin->queue_cs);
    }
}

static struct dmo_parser_source *dmo_parser_find_source_for_stream(struct dmo_parser *filter, UINT stream_index)
{
    unsigned int i;

    for (i = 0; i < filter->source_count; ++i)
    {
        if (filter->sources[i]->stream_index == stream_index)
            return filter->sources[i];
    }

    return NULL;
}

static DWORD CALLBACK dmo_demux_thread(void *arg)
{
    struct dmo_parser *filter = arg;
    ULONG buffer_size = 0x10000;

    TRACE("Demux thread starting for filter %p.\n", filter);

    for (;;)
    {
        struct simple_buffer *sbuf;
        DMO_OUTPUT_DATA_BUFFER output;
        UINT stream_idx;
        UINT needed_size;
        NTSTATUS status;
        struct dmo_parser_source *pin;
        struct dmo_packet *pkt;
        BYTE *pkt_data;
        DWORD data_len;
        unsigned int flush_generation;

        /* Check flushing / stop. */
        EnterCriticalSection(&filter->streaming_cs);
        while (filter->flushing)
            SleepConditionVariableCS(&filter->flushing_cv, &filter->streaming_cs, INFINITE);
        if (!filter->streaming)
        {
            LeaveCriticalSection(&filter->streaming_cs);
            break;
        }
        flush_generation = filter->read_flush_generation;
        LeaveCriticalSection(&filter->streaming_cs);

        dmo_parser_wait_for_queue_space(filter);
        EnterCriticalSection(&filter->streaming_cs);
        if (!filter->streaming || filter->flushing)
        {
            LeaveCriticalSection(&filter->streaming_cs);
            continue;
        }
        LeaveCriticalSection(&filter->streaming_cs);

    retry:
        if (!(sbuf = simple_buffer_create(buffer_size)))
        {
            ERR("Out of memory allocating read buffer.\n");
            break;
        }

        memset(&output, 0, sizeof(output));
        output.pBuffer = &sbuf->IMediaBuffer_iface;
        stream_idx = 0;
        needed_size = buffer_size;

        status = winedmo_demuxer_read(filter->winedmo_demuxer, &stream_idx, &output, &needed_size);

        EnterCriticalSection(&filter->streaming_cs);
        if (!filter->streaming || filter->flushing || flush_generation != filter->read_flush_generation)
        {
            LeaveCriticalSection(&filter->streaming_cs);
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            continue;
        }
        LeaveCriticalSection(&filter->streaming_cs);

        if (status == STATUS_BUFFER_TOO_SMALL)
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            buffer_size = needed_size;
            goto retry;
        }

        if (status)
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);

            if (status == STATUS_END_OF_FILE)
            {
                /* Check if this is a fake EOF caused by flushing. */
                EnterCriticalSection(&filter->streaming_cs);
                if (filter->flushing || !filter->streaming)
                {
                    LeaveCriticalSection(&filter->streaming_cs);
                    continue;
                }
                LeaveCriticalSection(&filter->streaming_cs);

                /* Real EOF: signal all connected pins once their queued data drains. */
                WARN("Demuxer reached end of file for filter %p.\n", filter);
                for (UINT i = 0; i < filter->source_count; ++i)
                {
                    bool send_eos;
                    unsigned int queued_packets;

                    pin = filter->sources[i];
                    EnterCriticalSection(&pin->queue_cs);
                    pin->eos = true;
                    queued_packets = pin->queued_packets;
                    send_eos = list_empty(&pin->packet_queue);
                    LeaveCriticalSection(&pin->queue_cs);
                    WARN("Stream %u EOF, queued packets %u, send_eos %u, peer %p.\n",
                            i, queued_packets, send_eos, pin->pin.pin.peer);
                    WakeConditionVariable(&pin->queue_cv);

                    if (send_eos)
                        dmo_parser_source_send_eos(pin);
                }

                dmo_parser_wait_for_seek_or_stop(filter);
                continue;
            }
            else
            {
                WARN("winedmo_demuxer_read failed, status %#lx.\n", status);
            }

            /* Sleep briefly to avoid spinning on errors. */
            Sleep(1);
            continue;
        }

        /* Route by demuxer stream id; some containers expose unsupported streams
         * that do not get output pins, so source array indices may not match. */
        if (!(pin = dmo_parser_find_source_for_stream(filter, stream_idx)))
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            continue;
        }

        IMediaBuffer_SetLength(&sbuf->IMediaBuffer_iface, needed_size);

        /* Only queue if the pin is connected. */
        if (!dmo_parser_source_is_active(pin))
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            continue;
        }

        data_len = needed_size;

        if (!data_len)
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            continue;
        }

        if (!(pkt = malloc(sizeof(*pkt))))
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            continue;
        }

        if (!(pkt_data = malloc(data_len)))
        {
            free(pkt);
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            continue;
        }

        {
            BYTE *src;

            IMediaBuffer_GetBufferAndLength(&sbuf->IMediaBuffer_iface, &src, NULL);
            memcpy(pkt_data, src, data_len);
        }
        IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);

        pkt->data = pkt_data;
        pkt->size = data_len;
        pkt->generation = flush_generation;
        pkt->has_pts = !!(output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIME);
        pkt->has_duration = !!(output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH);
        pkt->sync_point = !!(output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT);
        pkt->pts = output.rtTimestamp;
        pkt->duration = output.rtTimelength;

        EnterCriticalSection(&pin->queue_cs);
        list_add_tail(&pin->packet_queue, &pkt->entry);
        ++pin->queued_packets;
        LeaveCriticalSection(&pin->queue_cs);
        WakeConditionVariable(&pin->queue_cv);
    }

    TRACE("Demux thread stopping for filter %p.\n", filter);
    return 0;
}

static void dmo_parser_source_send_eos(struct dmo_parser_source *pin)
{
    EnterCriticalSection(&pin->flushing_cs);
    if (!pin->eos_sent && pin->pin.pin.peer)
    {
        WARN("Pin %p sending EndOfStream to peer %p.\n", pin, pin->pin.pin.peer);
        IPin_EndOfStream(pin->pin.pin.peer);
        pin->eos_sent = true;
    }
    else
        WARN("Pin %p not sending EndOfStream, eos_sent %u, peer %p.\n",
                pin, pin->eos_sent, pin->pin.pin.peer);
    LeaveCriticalSection(&pin->flushing_cs);
}

static void dmo_parser_begin_downstream_flush(struct dmo_parser *filter)
{
    unsigned int i;

    if (filter->downstream_flushing)
        return;

    for (i = 0; i < filter->source_count; ++i)
    {
        struct dmo_parser_source *pin = filter->sources[i];

        if (dmo_parser_source_is_active(pin))
            IPin_BeginFlush(pin->pin.pin.peer);
    }
    if (filter->reader)
        IAsyncReader_BeginFlush(filter->reader);

    filter->downstream_flushing = true;
}

static void dmo_parser_end_downstream_flush(struct dmo_parser *filter)
{
    unsigned int i;

    if (!filter->downstream_flushing)
        return;

    for (i = 0; i < filter->source_count; ++i)
    {
        struct dmo_parser_source *pin = filter->sources[i];

        if (dmo_parser_source_is_active(pin))
            IPin_EndFlush(pin->pin.pin.peer);
    }
    if (filter->reader)
        IAsyncReader_EndFlush(filter->reader);

    filter->downstream_flushing = false;
}

/* ========================================================================
 * Stream threads — dequeue packets and send via IMemInputPin
 * ======================================================================== */

static DWORD CALLBACK dmo_stream_thread(void *arg)
{
    struct dmo_parser_source *pin = arg;
    struct dmo_parser *filter = dmo_impl_from_strmbase_filter(pin->pin.pin.filter);

    TRACE("Stream thread starting for pin %p.\n", pin);

    for (;;)
    {
        struct dmo_packet *pkt;
        bool eos_now;

        /* Wait for streaming to resume if flushing. */
        EnterCriticalSection(&filter->streaming_cs);
        while (filter->flushing)
            SleepConditionVariableCS(&filter->flushing_cv, &filter->streaming_cs, INFINITE);
        if (!filter->streaming)
        {
            LeaveCriticalSection(&filter->streaming_cs);
            break;
        }
        LeaveCriticalSection(&filter->streaming_cs);

        /* Wait for a packet or EOS. */
        EnterCriticalSection(&pin->queue_cs);
        while (list_empty(&pin->packet_queue) && !pin->eos)
        {
            SleepConditionVariableCS(&pin->queue_cv, &pin->queue_cs, INFINITE);

            /* Re-check streaming/flushing after wake. */
            EnterCriticalSection(&filter->streaming_cs);
            if (!filter->streaming || filter->flushing)
            {
                LeaveCriticalSection(&filter->streaming_cs);
                LeaveCriticalSection(&pin->queue_cs);
                goto next_iter;
            }
            LeaveCriticalSection(&filter->streaming_cs);
        }

        eos_now = pin->eos && list_empty(&pin->packet_queue);

        if (eos_now)
        {
            LeaveCriticalSection(&pin->queue_cs);

            WARN("Stream thread for pin %p reached drained EOS.\n", pin);
            dmo_parser_source_send_eos(pin);

            /* Sleep until a seek resets things. */
            EnterCriticalSection(&pin->flushing_cs);
            SleepConditionVariableCS(&pin->eos_cv, &pin->flushing_cs, INFINITE);
            LeaveCriticalSection(&pin->flushing_cs);
            continue;
        }

        pkt = LIST_ENTRY(list_head(&pin->packet_queue), struct dmo_packet, entry);
        list_remove(&pkt->entry);
        --pin->queued_packets;
        WakeConditionVariable(&pin->queue_space_cv);
        LeaveCriticalSection(&pin->queue_cs);

        dmo_parser_source_wait_for_video_time(pin, pkt);

        /* Send the packet under flushing_cs (seeking blocks here). */
        EnterCriticalSection(&pin->flushing_cs);
        EnterCriticalSection(&filter->streaming_cs);
        if (!filter->streaming || filter->flushing || pkt->generation != filter->read_flush_generation)
        {
            LeaveCriticalSection(&filter->streaming_cs);
            LeaveCriticalSection(&pin->flushing_cs);
            dmo_packet_free(pkt);
            continue;
        }
        LeaveCriticalSection(&filter->streaming_cs);

        if (pin->pin.pin.peer)
        {
            IMediaSample *sample;
            HRESULT hr;
            bool send_eos = false;
            bool discontinuity = false;

            if (pin->need_segment)
            {
                IPin_NewSegment(pin->pin.pin.peer,
                        pin->seek.llCurrent, pin->seek.llStop, pin->seek.dRate);
                pin->need_segment = false;
                discontinuity = true;
            }

            hr = IMemAllocator_GetBuffer(pin->pin.pAllocator, &sample, NULL, NULL, 0);
            if (SUCCEEDED(hr))
            {
                BYTE *ptr;
                DWORD max_size, copy_size;

                max_size = IMediaSample_GetSize(sample);
                if (pkt->size > max_size)
                {
                    WARN("Sample buffer too small for packet, size %lu, max %lu.\n", pkt->size, max_size);
                    IMediaSample_Release(sample);
                    EnterCriticalSection(&pin->queue_cs);
                    if (pin->eos)
                    {
                        dmo_parser_source_flush_queue(pin);
                        send_eos = true;
                    }
                    LeaveCriticalSection(&pin->queue_cs);
                    LeaveCriticalSection(&pin->flushing_cs);
                    if (send_eos)
                        dmo_parser_source_send_eos(pin);
                    dmo_packet_free(pkt);
                    continue;
                }
                copy_size = pkt->size;

                IMediaSample_GetPointer(sample, &ptr);
                memcpy(ptr, pkt->data, copy_size);
                IMediaSample_SetActualDataLength(sample, copy_size);

                if (pkt->has_pts)
                {
                    REFERENCE_TIME start = pkt->pts - pin->seek.llCurrent;
                    if (pkt->has_duration)
                    {
                        REFERENCE_TIME end = start + pkt->duration;
                        IMediaSample_SetTime(sample, &start, &end);
                    }
                    else
                        IMediaSample_SetTime(sample, &start, NULL);
                }
                else
                    IMediaSample_SetTime(sample, NULL, NULL);

                IMediaSample_SetSyncPoint(sample, pkt->sync_point);
                IMediaSample_SetDiscontinuity(sample, discontinuity);
                IMediaSample_SetPreroll(sample, FALSE);

                hr = IMemInputPin_Receive(pin->pin.pMemInputPin, sample);
                if (FAILED(hr) && hr != VFW_E_FLUSHING)
                {
                    WARN("Receive failed, hr %#lx.\n", hr);
                    EnterCriticalSection(&pin->queue_cs);
                    if (pin->eos)
                    {
                        dmo_parser_source_flush_queue(pin);
                        send_eos = true;
                    }
                    LeaveCriticalSection(&pin->queue_cs);
                }

                IMediaSample_Release(sample);
            }
            else
                ERR("Failed to get sample buffer, hr %#lx.\n", hr);

            if (send_eos)
            {
                LeaveCriticalSection(&pin->flushing_cs);
                dmo_parser_source_send_eos(pin);
                dmo_packet_free(pkt);
                continue;
            }
        }
        LeaveCriticalSection(&pin->flushing_cs);

        dmo_packet_free(pkt);

    next_iter:;
    }

    TRACE("Stream thread stopping for pin %p.\n", pin);
    return 0;
}

/* ========================================================================
 * Filter operations
 * ======================================================================== */

static struct strmbase_pin *dmo_parser_get_pin(struct strmbase_filter *base, unsigned int index)
{
    struct dmo_parser *filter = dmo_impl_from_strmbase_filter(base);

    if (filter->enum_sink_first)
    {
        if (!index)
            return &filter->sink.pin;
        else if (index <= filter->source_count)
            return &filter->sources[index - 1]->pin.pin;
    }
    else
    {
        if (index < filter->source_count)
            return &filter->sources[index]->pin.pin;
        else if (index == filter->source_count)
            return &filter->sink.pin;
    }
    return NULL;
}

static void dmo_parser_destroy(struct strmbase_filter *iface)
{
    struct dmo_parser *filter = dmo_impl_from_strmbase_filter(iface);

    if (filter->sink.pin.peer)
    {
        IPin_Disconnect(filter->sink.pin.peer);
        IPin_Disconnect(&filter->sink.pin.IPin_iface);
    }

    if (filter->reader)
    {
        IAsyncReader_Release(filter->reader);
        filter->reader = NULL;
    }

    filter->streaming_cs.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&filter->streaming_cs);

    strmbase_sink_cleanup(&filter->sink);
    strmbase_filter_cleanup(&filter->filter);
    free(filter);
}

static HRESULT dmo_parser_init_stream(struct strmbase_filter *iface)
{
    struct dmo_parser *filter = dmo_impl_from_strmbase_filter(iface);
    unsigned int i;

    if (!filter->sink_connected)
        return S_OK;

    /* Seek to the current position (retain seek state across Stop/Pause/Run). */
    {
        LONGLONG seek_time = filter->source_count ? filter->sources[0]->seek.llCurrent : 0;
        if (seek_time)
        {
            NTSTATUS status;
            WARN("DirectShow parser init seek to %s.\n", debugstr_time(seek_time));
            status = winedmo_demuxer_seek(filter->winedmo_demuxer, seek_time);
            if (status)
                WARN("Initial seek failed, status %#lx.\n", status);
        }
    }

    filter->streaming = true;
    filter->flushing = false;
    filter->read_flushing = FALSE;

    for (i = 0; i < filter->source_count; ++i)
    {
        struct dmo_parser_source *pin = filter->sources[i];
        if (!dmo_parser_source_is_active(pin))
            continue;
        if (FAILED(IMemAllocator_Commit(pin->pin.pAllocator)))
            ERR("Failed to commit allocator for pin %p.\n", pin);
        pin->need_segment = true;
        pin->eos = false;
        pin->eos_sent = false;
        EnterCriticalSection(&pin->queue_cs);
        dmo_parser_source_flush_queue(pin);
        LeaveCriticalSection(&pin->queue_cs);
        pin->thread = CreateThread(NULL, 0, dmo_stream_thread, pin, 0, NULL);
    }

    filter->demux_thread = CreateThread(NULL, 0, dmo_demux_thread, filter, 0, NULL);

    return S_OK;
}

static HRESULT dmo_parser_start_stream(struct strmbase_filter *iface, REFERENCE_TIME start)
{
    struct dmo_parser *filter = dmo_impl_from_strmbase_filter(iface);
    unsigned int i;

    TRACE("filter %p, start %s.\n", filter, debugstr_time(start));

    if (!filter->sink_connected)
        return S_OK;

    dmo_parser_end_downstream_flush(filter);

    EnterCriticalSection(&filter->streaming_cs);
    filter->flushing = false;
    filter->read_flushing = FALSE;
    filter->stream_start = start;
    LeaveCriticalSection(&filter->streaming_cs);

    for (i = 0; i < filter->source_count; ++i)
    {
        WakeConditionVariable(&filter->sources[i]->queue_cv);
        WakeConditionVariable(&filter->sources[i]->queue_space_cv);
        WakeConditionVariable(&filter->sources[i]->eos_cv);
    }
    WakeAllConditionVariable(&filter->flushing_cv);

    return S_OK;
}

static HRESULT dmo_parser_stop_stream(struct strmbase_filter *iface)
{
    struct dmo_parser *filter = dmo_impl_from_strmbase_filter(iface);
    unsigned int i;

    if (!filter->sink_connected)
        return S_OK;

    EnterCriticalSection(&filter->streaming_cs);
    filter->flushing = true;
    filter->read_flushing = TRUE;
    ++filter->read_flush_generation;
    LeaveCriticalSection(&filter->streaming_cs);

    dmo_parser_begin_downstream_flush(filter);
    WakeAllConditionVariable(&filter->flushing_cv);

    for (i = 0; i < filter->source_count; ++i)
    {
        WakeConditionVariable(&filter->sources[i]->queue_cv);
        WakeConditionVariable(&filter->sources[i]->queue_space_cv);
        WakeConditionVariable(&filter->sources[i]->eos_cv);
    }

    for (i = 0; i < filter->source_count; ++i)
    {
        struct dmo_parser_source *pin = filter->sources[i];

        if (!dmo_parser_source_is_active(pin))
            continue;
        EnterCriticalSection(&pin->flushing_cs);
        LeaveCriticalSection(&pin->flushing_cs);
    }

    return S_OK;
}

static HRESULT dmo_parser_cleanup_stream(struct strmbase_filter *iface)
{
    struct dmo_parser *filter = dmo_impl_from_strmbase_filter(iface);
    unsigned int i;

    if (!filter->sink_connected)
        return S_OK;

    dmo_parser_begin_downstream_flush(filter);

    /* Signal threads to stop. */
    EnterCriticalSection(&filter->streaming_cs);
    filter->streaming = false;
    filter->flushing = false;
    filter->read_flushing = TRUE;
    ++filter->read_flush_generation;
    LeaveCriticalSection(&filter->streaming_cs);
    WakeAllConditionVariable(&filter->flushing_cv);

    /* Wake stream threads. */
    for (i = 0; i < filter->source_count; ++i)
    {
        struct dmo_parser_source *pin = filter->sources[i];
        WakeConditionVariable(&pin->queue_cv);
        WakeConditionVariable(&pin->queue_space_cv);
        WakeConditionVariable(&pin->eos_cv);
    }

    /* Wait for demux thread. */
    if (filter->demux_thread)
    {
        WaitForSingleObject(filter->demux_thread, INFINITE);
        CloseHandle(filter->demux_thread);
        filter->demux_thread = NULL;
    }

    /* Wait for stream threads. */
    for (i = 0; i < filter->source_count; ++i)
    {
        struct dmo_parser_source *pin = filter->sources[i];
        if (!dmo_parser_source_is_active(pin))
            continue;
        IMemAllocator_Decommit(pin->pin.pAllocator);
        if (pin->thread)
        {
            WakeConditionVariable(&pin->eos_cv);
            WakeConditionVariable(&pin->queue_cv);
            WakeConditionVariable(&pin->queue_space_cv);
            WaitForSingleObject(pin->thread, INFINITE);
            CloseHandle(pin->thread);
            pin->thread = NULL;
        }
        /* Drain any remaining packets. */
        EnterCriticalSection(&pin->queue_cs);
        dmo_parser_source_flush_queue(pin);
        LeaveCriticalSection(&pin->queue_cs);
    }

    dmo_parser_end_downstream_flush(filter);

    filter->read_flushing = FALSE;
    return S_OK;
}

static void dmo_parser_remove_pins(struct dmo_parser *filter)
{
    unsigned int i;

    if (!filter->sink_connected)
        return;

    for (i = 0; i < filter->source_count; ++i)
    {
        if (filter->sources[i])
            dmo_free_source_pin(filter->sources[i]);
    }

    winedmo_demuxer_destroy(&filter->winedmo_demuxer);
    filter->sink_connected = false;
    filter->source_count = 0;
    free(filter->sources);
    filter->sources = NULL;

    BaseFilterImpl_IncrementPinVersion(&filter->filter);
}

/* ========================================================================
 * Sink pin — common connect / disconnect
 * ======================================================================== */

static HRESULT dmo_parser_sink_connect(struct strmbase_sink *iface, IPin *peer,
        const AM_MEDIA_TYPE *pmt)
{
    struct dmo_parser *filter = dmo_impl_from_strmbase_sink(iface);
    LONGLONG file_size, unused;
    INT64 duration = 0;
    UINT stream_count = 0;
    WCHAR mime_type[256] = {0};
    NTSTATUS status;

    if (filter->reader)
    {
        IAsyncReader_Release(filter->reader);
        filter->reader = NULL;
    }

    if (FAILED(IPin_QueryInterface(peer, &IID_IAsyncReader, (void **)&filter->reader)))
        return E_NOINTERFACE;

    IAsyncReader_Length(filter->reader, &file_size, &unused);
    filter->winedmo_stream.file_size = file_size;
    filter->winedmo_stream.position = 0;
    filter->winedmo_stream.stream.p_seek = dmo_stream_seek_cb;
    filter->winedmo_stream.stream.p_read = dmo_stream_read_cb;
    filter->read_flushing = FALSE;

    status = winedmo_demuxer_create(NULL, &filter->winedmo_stream.stream,
            (UINT64)file_size, &duration, &stream_count, mime_type,
            &filter->winedmo_demuxer);
    if (status)
    {
        WARN("Failed to create demuxer, status %#lx.\n", status);
        IAsyncReader_Release(filter->reader);
        filter->reader = NULL;
        return HRESULT_FROM_NT(status);
    }

    if (filter->check_mime_type && !filter->check_mime_type(mime_type))
    {
        TRACE("Rejecting demuxed MIME type %s.\n", debugstr_w(mime_type));
        winedmo_demuxer_destroy(&filter->winedmo_demuxer);
        IAsyncReader_Release(filter->reader);
        filter->reader = NULL;
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    if (!filter->init_streams(filter, stream_count, duration))
    {
        winedmo_demuxer_destroy(&filter->winedmo_demuxer);
        IAsyncReader_Release(filter->reader);
        filter->reader = NULL;
        return E_FAIL;
    }

    filter->sink_connected = true;
    return S_OK;
}

static void dmo_parser_sink_disconnect(struct strmbase_sink *iface)
{
    struct dmo_parser *filter = dmo_impl_from_strmbase_sink(iface);
    dmo_parser_remove_pins(filter);
    if (filter->reader)
    {
        IAsyncReader_Release(filter->reader);
        filter->reader = NULL;
    }
}

/* ========================================================================
 * Generic source_query_accept / source_get_media_type
 * ======================================================================== */

static HRESULT dmo_generic_source_query_accept(struct dmo_parser_source *pin,
        const AM_MEDIA_TYPE *mt)
{
    /* Accept only the exact type we offer. */
    if (IsEqualGUID(&mt->majortype, &pin->stream_mt.majortype)
            && IsEqualGUID(&mt->subtype, &pin->stream_mt.subtype))
        return S_OK;
    return S_FALSE;
}

static HRESULT dmo_generic_source_get_media_type(struct dmo_parser_source *pin,
        unsigned int index, AM_MEDIA_TYPE *mt)
{
    if (index > 0)
        return VFW_S_NO_MORE_ITEMS;
    CopyMediaType(mt, &pin->stream_mt);
    return S_OK;
}

/* ========================================================================
 * Filter creation helper
 * ======================================================================== */

static const struct strmbase_filter_ops dmo_filter_ops =
{
    .filter_get_pin = dmo_parser_get_pin,
    .filter_destroy = dmo_parser_destroy,
    .filter_init_stream = dmo_parser_init_stream,
    .filter_start_stream = dmo_parser_start_stream,
    .filter_stop_stream = dmo_parser_stop_stream,
    .filter_cleanup_stream = dmo_parser_cleanup_stream,
};

static HRESULT dmo_parser_alloc(struct dmo_parser **out)
{
    struct dmo_parser *filter;

    if (!(filter = calloc(1, sizeof(*filter))))
        return E_OUTOFMEMORY;

    filter->winedmo_stream.stream.p_seek = dmo_stream_seek_cb;
    filter->winedmo_stream.stream.p_read = dmo_stream_read_cb;
    filter->IAMStreamSelect_iface.lpVtbl = &dmo_stream_select_vtbl;
    filter->max_queued_packets = DMO_PARSER_MAX_QUEUED_PACKETS;

    InitializeCriticalSectionEx(&filter->streaming_cs, 0,
            RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    filter->streaming_cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": filter.streaming_cs");
    InitializeConditionVariable(&filter->flushing_cv);

    *out = filter;
    return S_OK;
}

/* ========================================================================
 * AVI splitter
 * ======================================================================== */

static HRESULT avi_sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    if (IsEqualGUID(&mt->majortype, &MEDIATYPE_Stream)
            && IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_Avi))
        return S_OK;
    return S_FALSE;
}

static const struct strmbase_sink_ops avi_sink_ops =
{
    .base.pin_query_accept = avi_sink_query_accept,
    .sink_connect = dmo_parser_sink_connect,
    .sink_disconnect = dmo_parser_sink_disconnect,
};

static BOOL avi_init_streams(struct dmo_parser *filter, UINT stream_count, INT64 duration)
{
    WCHAR name[20];
    UINT i;

    for (i = 0; i < stream_count; ++i)
    {
        union winedmo_format *fmt = NULL;
        GUID major;
        AM_MEDIA_TYPE mt;
        UINT32 fmt_size;
        NTSTATUS status;

        status = winedmo_demuxer_stream_type(filter->winedmo_demuxer, i, &major, &fmt);
        if (status || !fmt)
        {
            WARN("Failed to get format for stream %u, status %#lx.\n", i, status);
            continue;
        }

        fmt_size = IsEqualGUID(&major, &MFMediaType_Audio)
                ? sizeof(fmt->audio) + fmt->audio.cbSize
                : fmt->video.dwSize;

        swprintf(name, ARRAY_SIZE(name), L"Stream %02u", i);

        if (SUCCEEDED(amt_from_winedmo_demuxer_format(&mt, &major, fmt, fmt_size)))
        {
            create_dmo_pin(filter, i, name, &mt, duration);
            FreeMediaType(&mt);
        }
        free(fmt);
    }

    return filter->source_count > 0;
}

HRESULT avi_splitter_create(IUnknown *outer, IUnknown **out)
{
    struct dmo_parser *filter;
    HRESULT hr;

    if (FAILED(hr = dmo_parser_alloc(&filter)))
        return hr;

    strmbase_filter_init(&filter->filter, outer, &CLSID_AviSplitter, &dmo_filter_ops);
    strmbase_sink_init(&filter->sink, &filter->filter, L"input pin", &avi_sink_ops, NULL);
    filter->init_streams = avi_init_streams;
    filter->source_query_accept = dmo_generic_source_query_accept;
    filter->source_get_media_type = dmo_generic_source_get_media_type;

    TRACE("Created AVI splitter (winedmo) %p.\n", filter);
    *out = &filter->filter.IUnknown_inner;
    return S_OK;
}

/* ========================================================================
 * Wave parser
 * ======================================================================== */

static HRESULT wave_sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Stream))
        return S_FALSE;
    if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WAVE))
        return S_OK;
    if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_AU)
            || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_AIFF))
        FIXME("AU and AIFF files are not yet supported.\n");
    return S_FALSE;
}

static const struct strmbase_sink_ops wave_sink_ops =
{
    .base.pin_query_accept = wave_sink_query_accept,
    .sink_connect = dmo_parser_sink_connect,
    .sink_disconnect = dmo_parser_sink_disconnect,
};

static BOOL wave_init_streams(struct dmo_parser *filter, UINT stream_count, INT64 duration)
{
    union winedmo_format *fmt = NULL;
    GUID major;
    AM_MEDIA_TYPE mt;
    UINT32 fmt_size;
    NTSTATUS status;

    if (stream_count == 0)
        return FALSE;

    status = winedmo_demuxer_stream_type(filter->winedmo_demuxer, 0, &major, &fmt);
    if (status || !fmt)
    {
        WARN("Failed to get format for stream 0, status %#lx.\n", status);
        return FALSE;
    }

    fmt_size = IsEqualGUID(&major, &MFMediaType_Audio)
            ? sizeof(fmt->audio) + fmt->audio.cbSize
            : fmt->video.dwSize;

    if (SUCCEEDED(amt_from_winedmo_demuxer_format(&mt, &major, fmt, fmt_size)))
    {
        create_dmo_pin(filter, 0, L"output", &mt, duration);
        FreeMediaType(&mt);
    }
    free(fmt);

    return filter->source_count > 0;
}

HRESULT wave_parser_create(IUnknown *outer, IUnknown **out)
{
    struct dmo_parser *filter;
    HRESULT hr;

    if (FAILED(hr = dmo_parser_alloc(&filter)))
        return hr;

    strmbase_filter_init(&filter->filter, outer, &CLSID_WAVEParser, &dmo_filter_ops);
    strmbase_sink_init(&filter->sink, &filter->filter, L"input pin", &wave_sink_ops, NULL);
    filter->init_streams = wave_init_streams;
    filter->source_query_accept = dmo_generic_source_query_accept;
    filter->source_get_media_type = dmo_generic_source_get_media_type;

    TRACE("Created Wave parser (winedmo) %p.\n", filter);
    *out = &filter->filter.IUnknown_inner;
    return S_OK;
}

/* ========================================================================
 * MPEG-1 splitter
 * ======================================================================== */

static HRESULT mpeg_sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Stream))
        return S_FALSE;
    if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1Audio)
            || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1System)
            || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG2_PROGRAM))
        return S_OK;
    if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1Video)
            || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1VideoCD))
        FIXME("Unsupported subtype %s.\n", wine_dbgstr_guid(&mt->subtype));
    return S_FALSE;
}

static HRESULT mpeg_sink_get_media_type(struct strmbase_pin *pin,
        unsigned int index, AM_MEDIA_TYPE *mt)
{
    static const GUID * const subtypes[] =
    {
        &MEDIASUBTYPE_MPEG1System,
        &MEDIASUBTYPE_MPEG2_PROGRAM,
        &MEDIASUBTYPE_MPEG1VideoCD,
        &MEDIASUBTYPE_MPEG1Video,
        &MEDIASUBTYPE_MPEG1Audio,
    };
    if (index >= ARRAY_SIZE(subtypes))
        return S_FALSE;
    memset(mt, 0, sizeof(*mt));
    mt->majortype = MEDIATYPE_Stream;
    mt->subtype = *subtypes[index];
    mt->bFixedSizeSamples = TRUE;
    mt->bTemporalCompression = TRUE;
    mt->lSampleSize = 1;
    return S_OK;
}

static const struct strmbase_sink_ops mpeg_sink_ops =
{
    .base.pin_query_accept = mpeg_sink_query_accept,
    .base.pin_get_media_type = mpeg_sink_get_media_type,
    .sink_connect = dmo_parser_sink_connect,
    .sink_disconnect = dmo_parser_sink_disconnect,
};

static BOOL mpeg_check_mime_type(const WCHAR *mime_type)
{
    return !wcscmp(mime_type, L"video/mpeg") || !wcscmp(mime_type, L"audio/mp3");
}

static BOOL mpeg_init_streams(struct dmo_parser *filter, UINT stream_count, INT64 duration)
{
    UINT i;

    for (i = 0; i < stream_count; ++i)
    {
        union winedmo_format *fmt = NULL;
        GUID major;
        AM_MEDIA_TYPE mt;
        UINT32 fmt_size;
        const WCHAR *name;
        NTSTATUS status;

        status = winedmo_demuxer_stream_type(filter->winedmo_demuxer, i, &major, &fmt);
        if (status || !fmt)
        {
            WARN("Failed to get format for stream %u, status %#lx.\n", i, status);
            continue;
        }

        fmt_size = IsEqualGUID(&major, &MFMediaType_Audio)
                ? sizeof(fmt->audio) + fmt->audio.cbSize
                : fmt->video.dwSize;

        if (IsEqualGUID(&major, &MFMediaType_Video))
            name = L"Video";
        else if (IsEqualGUID(&major, &MFMediaType_Audio))
            name = L"Audio";
        else
            name = L"Stream";

        if (SUCCEEDED(amt_from_winedmo_demuxer_format(&mt, &major, fmt, fmt_size)))
        {
            create_dmo_pin(filter, i, name, &mt, duration);
            FreeMediaType(&mt);
        }
        free(fmt);
    }

    return filter->source_count > 0;
}

static HRESULT mpeg_splitter_query_interface(struct strmbase_filter *iface, REFIID iid, void **out)
{
    struct dmo_parser *filter = dmo_impl_from_strmbase_filter(iface);

    if (IsEqualGUID(iid, &IID_IAMStreamSelect))
    {
        *out = &filter->IAMStreamSelect_iface;
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static const struct strmbase_filter_ops dmo_mpeg_filter_ops =
{
    .filter_query_interface = mpeg_splitter_query_interface,
    .filter_get_pin = dmo_parser_get_pin,
    .filter_destroy = dmo_parser_destroy,
    .filter_init_stream = dmo_parser_init_stream,
    .filter_start_stream = dmo_parser_start_stream,
    .filter_stop_stream = dmo_parser_stop_stream,
    .filter_cleanup_stream = dmo_parser_cleanup_stream,
};

HRESULT mpeg_splitter_create(IUnknown *outer, IUnknown **out)
{
    struct dmo_parser *filter;
    HRESULT hr;

    if (FAILED(hr = dmo_parser_alloc(&filter)))
        return hr;

    strmbase_filter_init(&filter->filter, outer, &CLSID_MPEG1Splitter, &dmo_mpeg_filter_ops);
    strmbase_sink_init(&filter->sink, &filter->filter, L"Input", &mpeg_sink_ops, NULL);
    filter->init_streams = mpeg_init_streams;
    filter->check_mime_type = mpeg_check_mime_type;
    filter->source_query_accept = dmo_generic_source_query_accept;
    filter->source_get_media_type = dmo_generic_source_get_media_type;
    filter->enum_sink_first = TRUE;
    filter->max_queued_packets = 256;

    TRACE("Created MPEG-1 splitter (winedmo) %p.\n", filter);
    *out = &filter->filter.IUnknown_inner;
    return S_OK;
}

/* ========================================================================
 * ASF splitter (new, winedmo-backed)
 * ======================================================================== */

static HRESULT asf_sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Stream))
        return S_FALSE;
    /* Accept generic streams; quartz will pass MEDIASUBTYPE_Asf or GUID_NULL. */
    return S_OK;
}

static const struct strmbase_sink_ops asf_sink_ops =
{
    .base.pin_query_accept = asf_sink_query_accept,
    .sink_connect = dmo_parser_sink_connect,
    .sink_disconnect = dmo_parser_sink_disconnect,
};

static BOOL asf_init_streams(struct dmo_parser *filter, UINT stream_count, INT64 duration)
{
    UINT preferred_audio_stream = UINT_MAX;
    UINT first_audio_stream = UINT_MAX;
    UINT i;

    for (i = 0; i < stream_count; ++i)
    {
        union winedmo_format *fmt = NULL;
        GUID major;
        NTSTATUS status;

        status = winedmo_demuxer_stream_type(filter->winedmo_demuxer, i, &major, &fmt);
        if (status || !fmt)
        {
            WARN("Failed to get format for stream %u, status %#lx.\n", i, status);
            continue;
        }

        if (IsEqualGUID(&major, &MFMediaType_Audio))
        {
            WCHAR lang[32];

            if (first_audio_stream == UINT_MAX)
                first_audio_stream = i;

            if (preferred_audio_stream == UINT_MAX
                    && !winedmo_demuxer_stream_lang(filter->winedmo_demuxer, i, lang, ARRAY_SIZE(lang))
                    && (!wcsicmp(lang, L"eng") || !wcsicmp(lang, L"en")))
                preferred_audio_stream = i;
        }

        free(fmt);
    }

    if (preferred_audio_stream == UINT_MAX)
        preferred_audio_stream = first_audio_stream;

    for (i = 0; i < stream_count; ++i)
    {
        union winedmo_format *fmt = NULL;
        GUID major;
        AM_MEDIA_TYPE mt;
        UINT32 fmt_size;
        const WCHAR *name;
        NTSTATUS status;

        status = winedmo_demuxer_stream_type(filter->winedmo_demuxer, i, &major, &fmt);
        if (status || !fmt)
        {
            WARN("Failed to get format for stream %u, status %#lx.\n", i, status);
            continue;
        }

        fmt_size = IsEqualGUID(&major, &MFMediaType_Audio)
                ? sizeof(fmt->audio) + fmt->audio.cbSize
                : fmt->video.dwSize;

        /* DirectShow's graph builder renders every exposed output pin.  For
         * multilingual ASF files, exposing all language tracks builds multiple
         * decoder chains and can exhaust 32-bit address space before playback
         * completes.  Offer the default audio stream plus video pins only. */
        if (IsEqualGUID(&major, &MFMediaType_Audio) && i != preferred_audio_stream)
        {
            free(fmt);
            continue;
        }

        if (IsEqualGUID(&major, &MFMediaType_Video))
            name = L"Video";
        else if (IsEqualGUID(&major, &MFMediaType_Audio))
            name = L"Audio";
        else
            name = L"Stream";

        if (SUCCEEDED(amt_from_winedmo_demuxer_format(&mt, &major, fmt, fmt_size)))
        {
            struct dmo_parser_source *pin = create_dmo_pin(filter, i, name, &mt, duration);

            if (pin && IsEqualGUID(&major, &MFMediaType_Audio))
                pin->selected = i == preferred_audio_stream;
            FreeMediaType(&mt);
        }
        free(fmt);
    }

    return filter->source_count > 0;
}

static HRESULT asf_splitter_query_interface(struct strmbase_filter *iface, REFIID iid, void **out)
{
    struct dmo_parser *filter = dmo_impl_from_strmbase_filter(iface);

    if (IsEqualGUID(iid, &IID_IAMStreamSelect))
    {
        *out = &filter->IAMStreamSelect_iface;
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static const struct strmbase_filter_ops dmo_asf_filter_ops =
{
    .filter_query_interface = asf_splitter_query_interface,
    .filter_get_pin = dmo_parser_get_pin,
    .filter_destroy = dmo_parser_destroy,
    .filter_init_stream = dmo_parser_init_stream,
    .filter_start_stream = dmo_parser_start_stream,
    .filter_stop_stream = dmo_parser_stop_stream,
    .filter_cleanup_stream = dmo_parser_cleanup_stream,
};

HRESULT asf_splitter_create(IUnknown *outer, IUnknown **out)
{
    static const GUID CLSID_winedmo_asf_splitter =
            {0x6d3cd6e1, 0x5862, 0x4d7e, {0xb4,0xb7,0x25,0x27,0x6b,0x59,0x76,0x22}};
    struct dmo_parser *filter;
    HRESULT hr;

    if (FAILED(hr = dmo_parser_alloc(&filter)))
        return hr;

    strmbase_filter_init(&filter->filter, outer, &CLSID_winedmo_asf_splitter, &dmo_asf_filter_ops);
    strmbase_sink_init(&filter->sink, &filter->filter, L"Input", &asf_sink_ops, NULL);
    filter->init_streams = asf_init_streams;
    filter->source_query_accept = dmo_generic_source_query_accept;
    filter->source_get_media_type = dmo_generic_source_get_media_type;
    filter->enum_sink_first = TRUE;

    TRACE("Created ASF splitter (winedmo) %p.\n", filter);
    *out = &filter->filter.IUnknown_inner;
    return S_OK;
}

/* ========================================================================
 * Decodebin parser — generic splitter accepting any stream format
 * ======================================================================== */

static HRESULT decodebin_sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    /* Accept any stream type; winedmo will handle format detection. */
    if (IsEqualGUID(&mt->majortype, &MEDIATYPE_Stream))
        return S_OK;
    return S_FALSE;
}

static const struct strmbase_sink_ops decodebin_sink_ops =
{
    .base.pin_query_accept = decodebin_sink_query_accept,
    .sink_connect = dmo_parser_sink_connect,
    .sink_disconnect = dmo_parser_sink_disconnect,
};

static BOOL decodebin_init_streams(struct dmo_parser *filter, UINT stream_count, INT64 duration)
{
    WCHAR name[20];
    UINT i;

    for (i = 0; i < stream_count; ++i)
    {
        union winedmo_format *fmt = NULL;
        GUID major;
        AM_MEDIA_TYPE mt;
        UINT32 fmt_size;
        NTSTATUS status;

        status = winedmo_demuxer_stream_type(filter->winedmo_demuxer, i, &major, &fmt);
        if (status || !fmt)
        {
            WARN("Failed to get format for stream %u, status %#lx.\n", i, status);
            continue;
        }

        fmt_size = IsEqualGUID(&major, &MFMediaType_Audio)
                ? sizeof(fmt->audio) + fmt->audio.cbSize
                : fmt->video.dwSize;

        swprintf(name, ARRAY_SIZE(name), L"Stream %02u", i);

        if (SUCCEEDED(amt_from_winedmo_demuxer_format(&mt, &major, fmt, fmt_size)))
        {
            create_dmo_pin(filter, i, name, &mt, duration);
            FreeMediaType(&mt);
        }
        free(fmt);
    }

    return filter->source_count > 0;
}

HRESULT decodebin_parser_create(IUnknown *outer, IUnknown **out)
{
    struct dmo_parser *filter;
    HRESULT hr;

    if (FAILED(hr = dmo_parser_alloc(&filter)))
        return hr;

    strmbase_filter_init(&filter->filter, outer, &CLSID_decodebin_parser, &dmo_filter_ops);
    strmbase_sink_init(&filter->sink, &filter->filter, L"input pin", &decodebin_sink_ops, NULL);
    filter->init_streams = decodebin_init_streams;
    filter->source_query_accept = dmo_generic_source_query_accept;
    filter->source_get_media_type = dmo_generic_source_get_media_type;

    TRACE("Created decodebin parser (winedmo) %p.\n", filter);
    *out = &filter->filter.IUnknown_inner;
    return S_OK;
}
