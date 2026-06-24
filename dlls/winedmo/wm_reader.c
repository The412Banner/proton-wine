/*
 * Copyright 2012 Austin English
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
#include "wine/winedmo.h"
#include "wine/list.h"
#include "amvideo.h"
#include "dvdmedia.h"
#include "mmreg.h"
#include "mediaobj.h"
#include "initguid.h"
#include "wmsdk.h"

WINE_DEFAULT_DEBUG_CHANNEL(wmvcore);

struct wm_stream
{
    struct wm_reader *reader;
    WMT_STREAM_SELECTION selection;
    WORD index;
    bool eos;
    bool read_compressed;   /* what the caller requested */
    bool decode_stream;     /* true = run decoder; false = deliver compressed */

    /* Stream format from winedmo demuxer */
    GUID major_type;
    union winedmo_format *compressed_fmt;  /* dynamically allocated */
    UINT32 compressed_fmt_size;

    /* Decoded output format (for GetOutputFormat/Props) */
    struct winedmo_codec_format decoded_winedmo_format;

    /* Decoder transform (when decode_stream = true) */
    struct winedmo_transform decoder;
    CRITICAL_SECTION decoder_cs;

    /* Per-stream packet queue */
    struct list packet_queue;
    CRITICAL_SECTION queue_cs;
    CONDITION_VARIABLE queue_cv;
    UINT queued_packets;
    bool queue_eos;
    bool queue_flushing;

    /* Partial packet consumption (for audio chunking) */
    BYTE *current_data;
    UINT32 current_size;
    UINT32 current_offset;
    INT64 current_pts;
    INT64 current_duration;
    DWORD current_flags;
    INT64 last_decoded_pts;
    INT64 observed_decoded_duration;
    INT64 nominal_decoded_duration;
    bool next_decoded_cleanpoint;
    bool video_sample_durations;

    IWMReaderAllocatorEx *output_allocator;
    IWMReaderAllocatorEx *stream_allocator;
};

struct wm_reader
{
    IUnknown IUnknown_inner;
    IWMSyncReader2 IWMSyncReader2_iface;
    IWMHeaderInfo3 IWMHeaderInfo3_iface;
    IWMLanguageList IWMLanguageList_iface;
    IWMPacketSize2 IWMPacketSize2_iface;
    IWMProfile3 IWMProfile3_iface;
    IWMReaderPlaylistBurn IWMReaderPlaylistBurn_iface;
    IWMReaderTimecode IWMReaderTimecode_iface;
    IUnknown *outer;
    LONG refcount;

    CRITICAL_SECTION cs;
    QWORD start_time;
    QWORD file_size;

    WCHAR *filename;
    IStream *source_stream;
    HANDLE file;

    /* winedmo demuxer */
    struct winedmo_demuxer demuxer;
    struct {
        struct winedmo_stream stream;
        LONGLONG position;
    } demuxer_stream;
    INT64 duration;           /* in 100ns units, from winedmo_demuxer_create */
    HANDLE demux_thread;
    bool demux_shutdown;
    bool read_flushing;
    unsigned int read_flush_generation;
    bool flushing;
    CRITICAL_SECTION demuxer_cs;  /* serialises winedmo_demuxer_read vs winedmo_demuxer_seek */

    struct wm_stream *streams;
    WORD stream_count;
    UINT preferred_audio_stream;
};

static struct wm_stream *get_stream_by_output_number(struct wm_reader *reader, DWORD output)
{
    if (output < reader->stream_count)
        return &reader->streams[output];
    WARN("Invalid output number %lu.\n", output);
    return NULL;
}

/* ========================================================================
 * wm_packet: compressed packet from the demux thread
 * ======================================================================== */

struct wm_packet
{
    struct list entry;
    BYTE *data;
    UINT32 size;
    INT64 pts;      /* -1 if not available */
    INT64 duration; /* -1 if not available */
    DWORD flags;    /* WM_SF_* flags */
};

/* ========================================================================
 * IMediaBuffer — lightweight wrapper for winedmo_demuxer_read
 * ======================================================================== */

struct wm_simple_buffer
{
    IMediaBuffer IMediaBuffer_iface;
    LONG refcount;
    ULONG max_length;
    ULONG length;
    BYTE data[];
};

static inline struct wm_simple_buffer *wm_impl_from_IMediaBuffer(IMediaBuffer *iface)
{
    return CONTAINING_RECORD(iface, struct wm_simple_buffer, IMediaBuffer_iface);
}

static HRESULT WINAPI wm_sbuf_QueryInterface(IMediaBuffer *iface, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMediaBuffer))
    { *out = iface; IMediaBuffer_AddRef(iface); return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI wm_sbuf_AddRef(IMediaBuffer *iface)
{ return InterlockedIncrement(&wm_impl_from_IMediaBuffer(iface)->refcount); }
static ULONG WINAPI wm_sbuf_Release(IMediaBuffer *iface)
{
    struct wm_simple_buffer *buf = wm_impl_from_IMediaBuffer(iface);
    ULONG ref = InterlockedDecrement(&buf->refcount);
    if (!ref) free(buf);
    return ref;
}
static HRESULT WINAPI wm_sbuf_SetLength(IMediaBuffer *iface, DWORD len)
{
    struct wm_simple_buffer *buf = wm_impl_from_IMediaBuffer(iface);
    if (len > buf->max_length) return E_INVALIDARG;
    buf->length = len; return S_OK;
}
static HRESULT WINAPI wm_sbuf_GetMaxLength(IMediaBuffer *iface, DWORD *out)
{ *out = wm_impl_from_IMediaBuffer(iface)->max_length; return S_OK; }
static HRESULT WINAPI wm_sbuf_GetBufferAndLength(IMediaBuffer *iface, BYTE **data, DWORD *len)
{
    struct wm_simple_buffer *buf = wm_impl_from_IMediaBuffer(iface);
    if (data) *data = buf->data;
    if (len) *len = buf->length;
    return S_OK;
}
static const IMediaBufferVtbl wm_sbuf_vtbl =
{
    wm_sbuf_QueryInterface, wm_sbuf_AddRef, wm_sbuf_Release,
    wm_sbuf_SetLength, wm_sbuf_GetMaxLength, wm_sbuf_GetBufferAndLength,
};
static struct wm_simple_buffer *wm_simple_buffer_create(ULONG size)
{
    struct wm_simple_buffer *buf = malloc(offsetof(struct wm_simple_buffer, data[size]));
    if (!buf) return NULL;
    buf->IMediaBuffer_iface.lpVtbl = &wm_sbuf_vtbl;
    buf->refcount = 1; buf->max_length = size; buf->length = 0;
    return buf;
}

/* ========================================================================
 * Format helpers
 * ======================================================================== */

/* Derive decoded winedmo_format from winedmo compressed format */
static void winedmo_decoded_format_from_winedmo(const GUID *major, const union winedmo_format *fmt,
        struct winedmo_codec_format *wg)
{
    memset(wg, 0, sizeof(*wg));
    if (IsEqualGUID(major, &MFMediaType_Audio))
    {
        wg->major_type = WINEDMO_MAJOR_TYPE_AUDIO;
        wg->u.audio.format = WINEDMO_AUDIO_FORMAT_S16LE;
        wg->u.audio.channels = fmt->audio.nChannels;
        wg->u.audio.rate = fmt->audio.nSamplesPerSec;
    }
    else if (IsEqualGUID(major, &MFMediaType_Video))
    {
        wg->major_type = WINEDMO_MAJOR_TYPE_VIDEO;
        wg->u.video.format = WINEDMO_VIDEO_FORMAT_BGR;
        wg->u.video.width = fmt->video.videoInfo.dwWidth;
        wg->u.video.height = (int32_t)fmt->video.videoInfo.dwHeight;
        /* Match the old WM reader: RGB output exposed to WMSDK callers is
         * bottom-up, and the decoder output must be configured the same way
         * as the media type returned by GetOutputProps(). */
        if (wg->u.video.height > 0)
            wg->u.video.height = -wg->u.video.height;
        wg->u.video.fps_n = fmt->video.videoInfo.FramesPerSecond.Numerator;
        wg->u.video.fps_d = fmt->video.videoInfo.FramesPerSecond.Denominator
                ? fmt->video.videoInfo.FramesPerSecond.Denominator : 1;
    }
}

/* Map winedmo_video_format to MFVideoFormat GUID for decoder output */
static GUID mf_subtype_from_winedmo_video_format(enum winedmo_video_format fmt)
{
    switch (fmt)
    {
    case WINEDMO_VIDEO_FORMAT_NV12:  return MFVideoFormat_NV12;
    case WINEDMO_VIDEO_FORMAT_YV12:  return MFVideoFormat_YV12;
    case WINEDMO_VIDEO_FORMAT_YUY2:  return MFVideoFormat_YUY2;
    case WINEDMO_VIDEO_FORMAT_UYVY:  return MFVideoFormat_UYVY;
    case WINEDMO_VIDEO_FORMAT_YVYU:  return MFVideoFormat_YVYU;
    case WINEDMO_VIDEO_FORMAT_BGRA:  return MFVideoFormat_ARGB32;
    case WINEDMO_VIDEO_FORMAT_BGRx:  return MFVideoFormat_RGB32;
    case WINEDMO_VIDEO_FORMAT_BGR:   return MFVideoFormat_RGB24;
    case WINEDMO_VIDEO_FORMAT_RGB16: return MFVideoFormat_RGB565;
    case WINEDMO_VIDEO_FORMAT_RGB15: return MFVideoFormat_RGB555;
    default:                    return MFVideoFormat_NV12;
    }
}

/* Build decoder transform for a stream */
static void wm_stream_create_decoder(struct wm_stream *stream)
{
    union winedmo_format out_fmt;
    UINT32 out_size;

    if (!stream->compressed_fmt) return;

    memset(&out_fmt, 0, sizeof(out_fmt));
    if (IsEqualGUID(&stream->major_type, &MFMediaType_Audio))
    {
        out_fmt.audio.wFormatTag = WAVE_FORMAT_PCM;
        out_fmt.audio.nChannels = stream->decoded_winedmo_format.u.audio.channels;
        out_fmt.audio.nSamplesPerSec = stream->decoded_winedmo_format.u.audio.rate;
        out_fmt.audio.wBitsPerSample = 16;
        out_fmt.audio.nBlockAlign = out_fmt.audio.nChannels * 2;
        out_fmt.audio.nAvgBytesPerSec = out_fmt.audio.nSamplesPerSec * out_fmt.audio.nBlockAlign;
        out_fmt.audio.cbSize = 0;
        out_size = sizeof(WAVEFORMATEX);
    }
    else
    {
        out_fmt.video.dwSize = sizeof(MFVIDEOFORMAT);
        out_fmt.video.videoInfo = stream->compressed_fmt->video.videoInfo;
        out_fmt.video.videoInfo.dwWidth = stream->decoded_winedmo_format.u.video.width;
        out_fmt.video.videoInfo.dwHeight = abs(stream->decoded_winedmo_format.u.video.height);
        out_fmt.video.videoInfo.FramesPerSecond.Numerator = stream->decoded_winedmo_format.u.video.fps_n;
        out_fmt.video.videoInfo.FramesPerSecond.Denominator = stream->decoded_winedmo_format.u.video.fps_d;
        if (stream->decoded_winedmo_format.u.video.height < 0
                && winedmo_video_format_is_rgb(stream->decoded_winedmo_format.u.video.format))
            out_fmt.video.videoInfo.VideoFlags |= MFVideoFlag_BottomUpLinearRep;
        else
            out_fmt.video.videoInfo.VideoFlags &= ~MFVideoFlag_BottomUpLinearRep;
        out_fmt.video.guidFormat = mf_subtype_from_winedmo_video_format(stream->decoded_winedmo_format.u.video.format);
        out_size = sizeof(MFVIDEOFORMAT);
    }

    EnterCriticalSection(&stream->decoder_cs);
    {
        NTSTATUS status = winedmo_transform_create(stream->major_type,
                stream->compressed_fmt, stream->compressed_fmt_size,
                &out_fmt, out_size, &stream->decoder);
        if (status)
            ERR("Failed to create decoder transform for stream %u (status %#lx).\n", stream->index, status);
    }
    LeaveCriticalSection(&stream->decoder_cs);
}

static void wm_stream_destroy_decoder(struct wm_stream *stream)
{
    EnterCriticalSection(&stream->decoder_cs);
    if (stream->decoder.handle)
    {
        winedmo_transform_destroy(stream->decoder);
        stream->decoder.handle = 0;
    }
    LeaveCriticalSection(&stream->decoder_cs);
}

static void wm_stream_reset_decoded_timing(struct wm_stream *stream)
{
    stream->last_decoded_pts = INT64_MIN;
    stream->observed_decoded_duration = 0;
    stream->next_decoded_cleanpoint = false;
}

static INT64 wm_stream_get_video_duration(const struct wm_stream *stream)
{
    if (stream->nominal_decoded_duration > 0)
        return stream->nominal_decoded_duration;
    if (stream->observed_decoded_duration > 0)
        return stream->observed_decoded_duration;
    return 333333;
}

static BOOL wm_stream_update_decoded_video_timing(struct wm_stream *stream, INT64 *pts, INT64 *duration)
{
    INT64 delta;
    BOOL missing_duration;

    if (!IsEqualGUID(&stream->major_type, &MFMediaType_Video) || *pts == INT64_MIN)
        return TRUE;

    missing_duration = *duration <= 0;
    if (missing_duration)
        *duration = wm_stream_get_video_duration(stream);

    if (stream->last_decoded_pts != INT64_MIN)
    {
        delta = *pts - stream->last_decoded_pts;
        if (delta > 100000 && delta < 1000000 && delta > stream->observed_decoded_duration)
            stream->observed_decoded_duration = delta;
        else if (delta <= 0)
            return FALSE;
    }
    stream->last_decoded_pts = *pts;

    if (stream->observed_decoded_duration > 0 && missing_duration)
        *duration = stream->observed_decoded_duration;

    return TRUE;
}

/* ========================================================================
 * output_props
 * ======================================================================== */

struct output_props
{
    IWMOutputMediaProps IWMOutputMediaProps_iface;
    LONG refcount;

    AM_MEDIA_TYPE mt;
};

static inline struct output_props *impl_from_IWMOutputMediaProps(IWMOutputMediaProps *iface)
{
    return CONTAINING_RECORD(iface, struct output_props, IWMOutputMediaProps_iface);
}

static HRESULT WINAPI output_props_QueryInterface(IWMOutputMediaProps *iface, REFIID iid, void **out)
{
    struct output_props *props = impl_from_IWMOutputMediaProps(iface);

    TRACE("props %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IWMOutputMediaProps))
        *out = &props->IWMOutputMediaProps_iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI output_props_AddRef(IWMOutputMediaProps *iface)
{
    struct output_props *props = impl_from_IWMOutputMediaProps(iface);
    ULONG refcount = InterlockedIncrement(&props->refcount);

    TRACE("%p increasing refcount to %lu.\n", props, refcount);

    return refcount;
}

static ULONG WINAPI output_props_Release(IWMOutputMediaProps *iface)
{
    struct output_props *props = impl_from_IWMOutputMediaProps(iface);
    ULONG refcount = InterlockedDecrement(&props->refcount);

    TRACE("%p decreasing refcount to %lu.\n", props, refcount);

    if (!refcount)
        free(props);

    return refcount;
}

static HRESULT WINAPI output_props_GetType(IWMOutputMediaProps *iface, GUID *major_type)
{
    const struct output_props *props = impl_from_IWMOutputMediaProps(iface);

    TRACE("iface %p, major_type %p.\n", iface, major_type);

    *major_type = props->mt.majortype;
    return S_OK;
}

static HRESULT WINAPI output_props_GetMediaType(IWMOutputMediaProps *iface, WM_MEDIA_TYPE *mt, DWORD *size)
{
    const struct output_props *props = impl_from_IWMOutputMediaProps(iface);
    const DWORD req_size = *size;

    TRACE("iface %p, mt %p, size %p.\n", iface, mt, size);

    *size = sizeof(*mt) + props->mt.cbFormat;
    if (!mt)
        return S_OK;
    if (req_size < *size)
        return ASF_E_BUFFERTOOSMALL;

    strmbase_dump_media_type(&props->mt);

    memcpy(mt, &props->mt, sizeof(*mt));
    memcpy(mt + 1, props->mt.pbFormat, props->mt.cbFormat);
    mt->pbFormat = (BYTE *)(mt + 1);
    return S_OK;
}

static HRESULT WINAPI output_props_SetMediaType(IWMOutputMediaProps *iface, WM_MEDIA_TYPE *mt)
{
    const struct output_props *props = impl_from_IWMOutputMediaProps(iface);

    TRACE("iface %p, mt %p.\n", iface, mt);

    if (!mt)
        return E_POINTER;

    if (!IsEqualGUID(&props->mt.majortype, &mt->majortype))
        return E_FAIL;

    FreeMediaType((AM_MEDIA_TYPE *)&props->mt);
    return CopyMediaType((AM_MEDIA_TYPE *)&props->mt, (AM_MEDIA_TYPE *)mt);
}

static HRESULT WINAPI output_props_GetStreamGroupName(IWMOutputMediaProps *iface, WCHAR *name, WORD *len)
{
    FIXME("iface %p, name %p, len %p, stub!\n", iface, name, len);
    return E_NOTIMPL;
}

static HRESULT WINAPI output_props_GetConnectionName(IWMOutputMediaProps *iface, WCHAR *name, WORD *len)
{
    FIXME("iface %p, name %p, len %p, stub!\n", iface, name, len);
    return E_NOTIMPL;
}

static const struct IWMOutputMediaPropsVtbl output_props_vtbl =
{
    output_props_QueryInterface,
    output_props_AddRef,
    output_props_Release,
    output_props_GetType,
    output_props_GetMediaType,
    output_props_SetMediaType,
    output_props_GetStreamGroupName,
    output_props_GetConnectionName,
};

static struct output_props *unsafe_impl_from_IWMOutputMediaProps(IWMOutputMediaProps *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &output_props_vtbl);
    return impl_from_IWMOutputMediaProps(iface);
}

static IWMOutputMediaProps *output_props_create(const struct winedmo_codec_format *format)
{
    struct output_props *object;

    if (!(object = calloc(1, sizeof(*object))))
        return NULL;
    object->IWMOutputMediaProps_iface.lpVtbl = &output_props_vtbl;
    object->refcount = 1;

    if (!amt_from_winedmo_format(&object->mt, format, true))
    {
        free(object);
        return NULL;
    }

    TRACE("Created output properties %p.\n", object);
    return &object->IWMOutputMediaProps_iface;
}

struct buffer
{
    INSSBuffer INSSBuffer_iface;
    LONG refcount;

    DWORD size, capacity;
    BYTE data[1];
};

static struct buffer *impl_from_INSSBuffer(INSSBuffer *iface)
{
    return CONTAINING_RECORD(iface, struct buffer, INSSBuffer_iface);
}

static HRESULT WINAPI buffer_QueryInterface(INSSBuffer *iface, REFIID iid, void **out)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);

    TRACE("buffer %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_INSSBuffer))
        *out = &buffer->INSSBuffer_iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI buffer_AddRef(INSSBuffer *iface)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);
    ULONG refcount = InterlockedIncrement(&buffer->refcount);

    TRACE("%p increasing refcount to %lu.\n", buffer, refcount);

    return refcount;
}

static ULONG WINAPI buffer_Release(INSSBuffer *iface)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);
    ULONG refcount = InterlockedDecrement(&buffer->refcount);

    TRACE("%p decreasing refcount to %lu.\n", buffer, refcount);

    if (!refcount)
        free(buffer);

    return refcount;
}

static HRESULT WINAPI buffer_GetLength(INSSBuffer *iface, DWORD *size)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);

    TRACE("buffer %p, size %p.\n", buffer, size);

    *size = buffer->size;
    return S_OK;
}

static HRESULT WINAPI buffer_SetLength(INSSBuffer *iface, DWORD size)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);

    TRACE("iface %p, size %lu.\n", buffer, size);

    if (size > buffer->capacity)
        return E_INVALIDARG;

    buffer->size = size;
    return S_OK;
}

static HRESULT WINAPI buffer_GetMaxLength(INSSBuffer *iface, DWORD *size)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);

    TRACE("buffer %p, size %p.\n", buffer, size);

    *size = buffer->capacity;
    return S_OK;
}

static HRESULT WINAPI buffer_GetBuffer(INSSBuffer *iface, BYTE **data)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);

    TRACE("buffer %p, data %p.\n", buffer, data);

    *data = buffer->data;
    return S_OK;
}

static HRESULT WINAPI buffer_GetBufferAndLength(INSSBuffer *iface, BYTE **data, DWORD *size)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);

    TRACE("buffer %p, data %p, size %p.\n", buffer, data, size);

    *size = buffer->size;
    *data = buffer->data;
    return S_OK;
}

static const INSSBufferVtbl buffer_vtbl =
{
    buffer_QueryInterface,
    buffer_AddRef,
    buffer_Release,
    buffer_GetLength,
    buffer_SetLength,
    buffer_GetMaxLength,
    buffer_GetBuffer,
    buffer_GetBufferAndLength,
};

struct stream_config
{
    IWMStreamConfig IWMStreamConfig_iface;
    IWMMediaProps IWMMediaProps_iface;
    LONG refcount;

    const struct wm_stream *stream;
};

static struct stream_config *impl_from_IWMStreamConfig(IWMStreamConfig *iface)
{
    return CONTAINING_RECORD(iface, struct stream_config, IWMStreamConfig_iface);
}

static HRESULT WINAPI stream_config_QueryInterface(IWMStreamConfig *iface, REFIID iid, void **out)
{
    struct stream_config *config = impl_from_IWMStreamConfig(iface);

    TRACE("config %p, iid %s, out %p.\n", config, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IWMStreamConfig))
        *out = &config->IWMStreamConfig_iface;
    else if (IsEqualGUID(iid, &IID_IWMMediaProps))
        *out = &config->IWMMediaProps_iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI stream_config_AddRef(IWMStreamConfig *iface)
{
    struct stream_config *config = impl_from_IWMStreamConfig(iface);
    ULONG refcount = InterlockedIncrement(&config->refcount);

    TRACE("%p increasing refcount to %lu.\n", config, refcount);

    return refcount;
}

static ULONG WINAPI stream_config_Release(IWMStreamConfig *iface)
{
    struct stream_config *config = impl_from_IWMStreamConfig(iface);
    ULONG refcount = InterlockedDecrement(&config->refcount);

    TRACE("%p decreasing refcount to %lu.\n", config, refcount);

    if (!refcount)
    {
        IWMProfile3_Release(&config->stream->reader->IWMProfile3_iface);
        free(config);
    }

    return refcount;
}

static HRESULT WINAPI stream_config_GetStreamType(IWMStreamConfig *iface, GUID *type)
{
    struct stream_config *config = impl_from_IWMStreamConfig(iface);
    struct wm_reader *reader = config->stream->reader;

    TRACE("config %p, type %p.\n", config, type);

    EnterCriticalSection(&reader->cs);
    /* MFMediaType_Audio == MEDIATYPE_Audio, same for Video */
    *type = config->stream->major_type;
    LeaveCriticalSection(&reader->cs);

    return S_OK;
}

static HRESULT WINAPI stream_config_GetStreamNumber(IWMStreamConfig *iface, WORD *number)
{
    struct stream_config *config = impl_from_IWMStreamConfig(iface);

    TRACE("config %p, number %p.\n", config, number);

    *number = config->stream->index + 1;
    return S_OK;
}

static HRESULT WINAPI stream_config_SetStreamNumber(IWMStreamConfig *iface, WORD number)
{
    FIXME("iface %p, number %u, stub!\n", iface, number);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_GetStreamName(IWMStreamConfig *iface, WCHAR *name, WORD *len)
{
    FIXME("iface %p, name %p, len %p, stub!\n", iface, name, len);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_SetStreamName(IWMStreamConfig *iface, const WCHAR *name)
{
    FIXME("iface %p, name %s, stub!\n", iface, debugstr_w(name));
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_GetConnectionName(IWMStreamConfig *iface, WCHAR *name, WORD *len)
{
    FIXME("iface %p, name %p, len %p, stub!\n", iface, name, len);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_SetConnectionName(IWMStreamConfig *iface, const WCHAR *name)
{
    FIXME("iface %p, name %s, stub!\n", iface, debugstr_w(name));
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_GetBitrate(IWMStreamConfig *iface, DWORD *bitrate)
{
    FIXME("iface %p, bitrate %p, stub!\n", iface, bitrate);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_SetBitrate(IWMStreamConfig *iface, DWORD bitrate)
{
    FIXME("iface %p, bitrate %lu, stub!\n", iface, bitrate);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_GetBufferWindow(IWMStreamConfig *iface, DWORD *window)
{
    FIXME("iface %p, window %p, stub!\n", iface, window);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_SetBufferWindow(IWMStreamConfig *iface, DWORD window)
{
    FIXME("iface %p, window %lu, stub!\n", iface, window);
    return E_NOTIMPL;
}

static const IWMStreamConfigVtbl stream_config_vtbl =
{
    stream_config_QueryInterface,
    stream_config_AddRef,
    stream_config_Release,
    stream_config_GetStreamType,
    stream_config_GetStreamNumber,
    stream_config_SetStreamNumber,
    stream_config_GetStreamName,
    stream_config_SetStreamName,
    stream_config_GetConnectionName,
    stream_config_SetConnectionName,
    stream_config_GetBitrate,
    stream_config_SetBitrate,
    stream_config_GetBufferWindow,
    stream_config_SetBufferWindow,
};

static struct stream_config *impl_from_IWMMediaProps(IWMMediaProps *iface)
{
    return CONTAINING_RECORD(iface, struct stream_config, IWMMediaProps_iface);
}

static HRESULT WINAPI stream_props_QueryInterface(IWMMediaProps *iface, REFIID iid, void **out)
{
    struct stream_config *config = impl_from_IWMMediaProps(iface);
    return IWMStreamConfig_QueryInterface(&config->IWMStreamConfig_iface, iid, out);
}

static ULONG WINAPI stream_props_AddRef(IWMMediaProps *iface)
{
    struct stream_config *config = impl_from_IWMMediaProps(iface);
    return IWMStreamConfig_AddRef(&config->IWMStreamConfig_iface);
}

static ULONG WINAPI stream_props_Release(IWMMediaProps *iface)
{
    struct stream_config *config = impl_from_IWMMediaProps(iface);
    return IWMStreamConfig_Release(&config->IWMStreamConfig_iface);
}

static HRESULT WINAPI stream_props_GetType(IWMMediaProps *iface, GUID *major_type)
{
    FIXME("iface %p, major_type %p, stub!\n", iface, major_type);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_props_GetMediaType(IWMMediaProps *iface, WM_MEDIA_TYPE *mt, DWORD *size)
{
    struct stream_config *config = impl_from_IWMMediaProps(iface);
    const struct wm_stream *stream = config->stream;
    const DWORD req_size = *size;
    AM_MEDIA_TYPE stream_mt;
    HRESULT hr;

    TRACE("iface %p, mt %p, size %p.\n", iface, mt, size);

    /* Always return the compressed stream format from the winedmo demuxer */
    if (!stream->compressed_fmt)
        return E_FAIL;

    memset(&stream_mt, 0, sizeof(stream_mt));
    if (IsEqualGUID(&stream->major_type, &MFMediaType_Audio))
    {
        const WAVEFORMATEX *wfx = &stream->compressed_fmt->audio;
        UINT32 fmt_size = sizeof(WAVEFORMATEX) + wfx->cbSize;
        WAVEFORMATEX *dst;

        if (!(dst = CoTaskMemAlloc(fmt_size)))
            return E_OUTOFMEMORY;
        memcpy(dst, wfx, fmt_size);

        stream_mt.majortype = MEDIATYPE_Audio;
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            stream_mt.subtype = ((const WAVEFORMATEXTENSIBLE *)wfx)->SubFormat;
        else
        {
            stream_mt.subtype.Data1 = wfx->wFormatTag;
            stream_mt.subtype.Data2 = 0; stream_mt.subtype.Data3 = 0x0010;
            stream_mt.subtype.Data4[0] = 0x80; stream_mt.subtype.Data4[1] = 0x00;
            stream_mt.subtype.Data4[2] = 0x00; stream_mt.subtype.Data4[3] = 0xaa;
            stream_mt.subtype.Data4[4] = 0x00; stream_mt.subtype.Data4[5] = 0x38;
            stream_mt.subtype.Data4[6] = 0x9b; stream_mt.subtype.Data4[7] = 0x71;
        }
        stream_mt.bTemporalCompression = TRUE;
        stream_mt.lSampleSize = wfx->nBlockAlign ? wfx->nBlockAlign : 1;
        stream_mt.formattype = FORMAT_WaveFormatEx;
        stream_mt.cbFormat = fmt_size;
        stream_mt.pbFormat = (BYTE *)dst;
        hr = S_OK;
    }
    else if (IsEqualGUID(&stream->major_type, &MFMediaType_Video))
    {
        const MFVIDEOFORMAT *vfmt = &stream->compressed_fmt->video;
        DWORD extra = (stream->compressed_fmt_size > sizeof(MFVIDEOFORMAT))
                ? stream->compressed_fmt_size - sizeof(MFVIDEOFORMAT) : 0;
        DWORD vih_size = sizeof(VIDEOINFOHEADER) + extra;
        VIDEOINFOHEADER *vih;

        if (!(vih = CoTaskMemAlloc(vih_size)))
            return E_OUTOFMEMORY;
        memset(vih, 0, vih_size);
        vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER) + extra;
        vih->bmiHeader.biWidth = vfmt->videoInfo.dwWidth;
        vih->bmiHeader.biHeight = vfmt->videoInfo.dwHeight;
        vih->bmiHeader.biPlanes = 1;
        vih->bmiHeader.biCompression = vfmt->guidFormat.Data1;
        vih->bmiHeader.biBitCount = 24;
        if (vfmt->videoInfo.FramesPerSecond.Numerator && vfmt->videoInfo.FramesPerSecond.Denominator)
            vih->AvgTimePerFrame = (LONGLONG)10000000
                    * vfmt->videoInfo.FramesPerSecond.Denominator
                    / vfmt->videoInfo.FramesPerSecond.Numerator;
        if (extra) memcpy(vih + 1, vfmt + 1, extra);

        stream_mt.majortype = MEDIATYPE_Video;
        stream_mt.subtype = vfmt->guidFormat;
        stream_mt.bTemporalCompression = TRUE;
        stream_mt.formattype = FORMAT_VideoInfo;
        stream_mt.cbFormat = vih_size;
        stream_mt.pbFormat = (BYTE *)vih;
        hr = S_OK;
    }
    else
    {
        return E_FAIL;
    }

    *size = sizeof(stream_mt) + stream_mt.cbFormat;
    if (mt && req_size >= *size)
    {
        strmbase_dump_media_type(&stream_mt);
        memcpy(mt, &stream_mt, sizeof(*mt));
        memcpy(mt + 1, stream_mt.pbFormat, stream_mt.cbFormat);
        mt->pbFormat = (BYTE *)(mt + 1);
    }
    FreeMediaType(&stream_mt);

    if (mt && req_size < *size)
        return ASF_E_BUFFERTOOSMALL;
    return hr;
}

static HRESULT WINAPI stream_props_SetMediaType(IWMMediaProps *iface, WM_MEDIA_TYPE *mt)
{
    FIXME("iface %p, mt %p, stub!\n", iface, mt);
    return E_NOTIMPL;
}

static const IWMMediaPropsVtbl stream_props_vtbl =
{
    stream_props_QueryInterface,
    stream_props_AddRef,
    stream_props_Release,
    stream_props_GetType,
    stream_props_GetMediaType,
    stream_props_SetMediaType,
};

/* ========================================================================
 * winedmo stream callbacks
 * ======================================================================== */

static NTSTATUS CDECL wm_stream_seek_cb(struct winedmo_stream *ws, UINT64 *pos)
{
    struct wm_reader *reader = CONTAINING_RECORD(ws, struct wm_reader, demuxer_stream.stream);
    reader->demuxer_stream.position = *pos;
    return STATUS_SUCCESS;
}

static NTSTATUS CDECL wm_stream_read_cb(struct winedmo_stream *ws, BYTE *buffer, ULONG *size)
{
    struct wm_reader *reader = CONTAINING_RECORD(ws, struct wm_reader, demuxer_stream.stream);
    ULONG ret_size = 0;
    HRESULT hr;

    if (reader->read_flushing || reader->demux_shutdown)
    {
        *size = 0;
        return STATUS_SUCCESS;
    }

    if (reader->file)
    {
        LARGE_INTEGER li;
        li.QuadPart = reader->demuxer_stream.position;
        if (!SetFilePointerEx(reader->file, li, NULL, FILE_BEGIN) ||
            !ReadFile(reader->file, buffer, *size, &ret_size, NULL))
        {
            *size = 0;
            return STATUS_SUCCESS;
        }
        *size = ret_size;
    }
    else if (reader->source_stream)
    {
        LARGE_INTEGER li;
        li.QuadPart = reader->demuxer_stream.position;
        hr = IStream_Seek(reader->source_stream, li, STREAM_SEEK_SET, NULL);
        if (FAILED(hr) || FAILED(hr = IStream_Read(reader->source_stream, buffer, *size, &ret_size)))
        {
            *size = 0;
            return STATUS_SUCCESS;
        }
        *size = ret_size;
    }
    else
    {
        *size = 0;
    }

    reader->demuxer_stream.position += *size;
    return STATUS_SUCCESS;
}

/* ========================================================================
 * Demux thread
 * ======================================================================== */

static void wm_flush_stream_queue(struct wm_stream *stream)
{
    struct wm_packet *pkt, *next;

    EnterCriticalSection(&stream->queue_cs);
    LIST_FOR_EACH_ENTRY_SAFE(pkt, next, &stream->packet_queue, struct wm_packet, entry)
    {
        list_remove(&pkt->entry);
        free(pkt->data);
        free(pkt);
    }
    stream->queued_packets = 0;
    stream->queue_eos = false;
    LeaveCriticalSection(&stream->queue_cs);
    WakeAllConditionVariable(&stream->queue_cv);

    free(stream->current_data);
    stream->current_data = NULL;
    stream->current_size = stream->current_offset = 0;
}

static BOOL wm_stream_wait_queue_space(struct wm_reader *reader, struct wm_stream *stream)
{
    if (!IsEqualGUID(&stream->major_type, &MFMediaType_Video))
        return TRUE;
    if (reader->stream_count <= 2)
        return TRUE;

    EnterCriticalSection(&stream->queue_cs);
    while (!reader->demux_shutdown && !stream->queue_flushing
            && stream->selection != WMT_OFF && stream->queued_packets >= 256)
        SleepConditionVariableCS(&stream->queue_cv, &stream->queue_cs, 10);

    if (reader->demux_shutdown || stream->queue_flushing || stream->selection == WMT_OFF)
    {
        LeaveCriticalSection(&stream->queue_cs);
        return FALSE;
    }

    LeaveCriticalSection(&stream->queue_cs);
    return TRUE;
}

static DWORD CALLBACK wm_demux_thread(void *arg)
{
    struct wm_reader *reader = arg;
    ULONG buffer_size = 0x10000;

    TRACE("Demux thread starting for reader %p.\n", reader);

    while (!reader->demux_shutdown)
    {
        struct wm_simple_buffer *sbuf;
        DMO_OUTPUT_DATA_BUFFER output;
        UINT stream_idx, needed_size;
        NTSTATUS status;
        BYTE *src;
        DWORD data_len;
        struct wm_stream *s;
        struct wm_packet *pkt;
        unsigned int read_generation;

        /* Don't call the demuxer while reinit_stream is seeking it. */
        if (reader->read_flushing)
        {
            Sleep(1);
            continue;
        }

        if (!(sbuf = wm_simple_buffer_create(buffer_size)))
        {
            Sleep(1);
            continue;
        }

        read_generation = reader->read_flush_generation;
        memset(&output, 0, sizeof(output));
        output.pBuffer = &sbuf->IMediaBuffer_iface;
        stream_idx = 0;
        needed_size = buffer_size;

        EnterCriticalSection(&reader->demuxer_cs);
        status = winedmo_demuxer_read(reader->demuxer, &stream_idx, &output, &needed_size);
        LeaveCriticalSection(&reader->demuxer_cs);

        if (status == STATUS_BUFFER_TOO_SMALL)
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            buffer_size = needed_size;
            continue;
        }

        if (status == STATUS_END_OF_FILE)
        {
            unsigned int flush_generation = reader->read_flush_generation;

            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            if (reader->read_flushing)
            {
                Sleep(1);
                continue; /* fake EOF from flushing, loop and wait */
            }
            /* Real EOF: drain decoders and signal all streams */
            TRACE("Demuxer reached end of file.\n");
            for (UINT i = 0; i < reader->stream_count; i++)
            {
                s = &reader->streams[i];
                if (s->selection == WMT_OFF)
                {
                    EnterCriticalSection(&s->queue_cs);
                    s->queue_eos = true;
                    LeaveCriticalSection(&s->queue_cs);
                    WakeAllConditionVariable(&s->queue_cv);
                    continue;
                }
                EnterCriticalSection(&s->decoder_cs);
                if (s->decode_stream && s->decoder.handle)
                {
                    UINT32 max_dec_size = winedmo_format_get_max_size(&s->decoded_winedmo_format);
                    NTSTATUS dec_status;

                    if (!max_dec_size) max_dec_size = 0x40000;
                    winedmo_transform_drain(s->decoder);

                    for (;;)
                    {
                        INT64 dec_pts, dec_dur;
                        DWORD dec_flags;
                        BYTE *dec_buf;
                        UINT32 dec_size = max_dec_size;

                        if (!wm_stream_wait_queue_space(reader, s) || !(dec_buf = malloc(dec_size)))
                            break;

                        dec_status = winedmo_transform_get_output(s->decoder, dec_buf, &dec_size,
                                                                  &dec_pts, &dec_dur, &dec_flags);
                        if (dec_status == STATUS_BUFFER_TOO_SMALL)
                        {
                            free(dec_buf);
                            continue;
                        }
                        if (dec_status != STATUS_SUCCESS)
                        {
                            free(dec_buf);
                            break;
                        }

                        if (dec_flags & WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED || !dec_size)
                        {
                            free(dec_buf);
                            continue;
                        }
                        if (!wm_stream_update_decoded_video_timing(s, &dec_pts, &dec_dur))
                        {
                            free(dec_buf);
                            continue;
                        }

                        if ((pkt = malloc(sizeof(*pkt))))
                        {
                            pkt->data = dec_buf;
                            pkt->size = dec_size;
                            pkt->pts = dec_pts;
                            pkt->duration = dec_dur;
                            pkt->flags = 0;
                            if (IsEqualGUID(&s->major_type, &MFMediaType_Video))
                                pkt->flags |= WM_SF_CLEANPOINT;
                            if (dec_flags & WINEDMO_SAMPLE_FLAG_SYNC_POINT) pkt->flags |= WM_SF_CLEANPOINT;
                            if (dec_flags & WINEDMO_SAMPLE_FLAG_DISCONTINUITY) pkt->flags |= WM_SF_DISCONTINUITY;
                            EnterCriticalSection(&s->queue_cs);
                            if (!s->queue_flushing)
                            {
                                list_add_tail(&s->packet_queue, &pkt->entry);
                                ++s->queued_packets;
                                LeaveCriticalSection(&s->queue_cs);
                                WakeConditionVariable(&s->queue_cv);
                            }
                            else
                            {
                                LeaveCriticalSection(&s->queue_cs);
                                free(pkt->data);
                                free(pkt);
                            }
                        }
                        else
                            free(dec_buf);
                    }
                }
                LeaveCriticalSection(&s->decoder_cs);
                EnterCriticalSection(&s->queue_cs);
                s->queue_eos = true;
                LeaveCriticalSection(&s->queue_cs);
                WakeAllConditionVariable(&s->queue_cv);
            }
            /* Wait for seek/shutdown */
            while (!reader->demux_shutdown && reader->read_flush_generation == flush_generation)
                Sleep(10);
            if (!reader->demux_shutdown)
            {
                /* Clear EOS flags so next seek restarts */
                for (UINT i = 0; i < reader->stream_count; i++)
                {
                    s = &reader->streams[i];
                    EnterCriticalSection(&s->queue_cs);
                    s->queue_eos = false;
                    LeaveCriticalSection(&s->queue_cs);
                }
            }
            continue;
        }

        if (status)
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            Sleep(1);
            continue;
        }

        if (read_generation != reader->read_flush_generation || reader->read_flushing)
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            continue;
        }

        IMediaBuffer_SetLength(&sbuf->IMediaBuffer_iface, needed_size);
        data_len = needed_size;
        IMediaBuffer_GetBufferAndLength(&sbuf->IMediaBuffer_iface, &src, NULL);
        /* Do NOT release sbuf here — src points into its buffer and must stay valid. */

        if (stream_idx >= reader->stream_count || !data_len)
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            continue;
        }

        s = &reader->streams[stream_idx];
        if (s->selection == WMT_OFF)
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            continue;
        }

        EnterCriticalSection(&s->decoder_cs);
        if (read_generation != reader->read_flush_generation || reader->read_flushing)
        {
            LeaveCriticalSection(&s->decoder_cs);
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            continue;
        }

        if (s->decode_stream && s->decoder.handle)
        {
            INT64 in_pts = (output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIME) ? output.rtTimestamp : INT64_MIN;
            INT64 in_dur = (output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH) ? output.rtTimelength : 0;
            DWORD in_flags = (output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT ? WINEDMO_SAMPLE_FLAG_SYNC_POINT : 0)
                           | (output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_DISCONTINUITY ? WINEDMO_SAMPLE_FLAG_DISCONTINUITY : 0);
            UINT32 max_dec_size = winedmo_format_get_max_size(&s->decoded_winedmo_format);
            NTSTATUS dec_status;

            if (!max_dec_size) max_dec_size = 0x40000;

            if ((dec_status = winedmo_transform_push_input(s->decoder, src, data_len,
                    in_pts, INT64_MIN, in_dur, in_flags))
                    && dec_status != STATUS_DEVICE_BUSY)
            {
                WARN("push_input failed for stream %u: %#lx\n", stream_idx, dec_status);
                IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
                LeaveCriticalSection(&s->decoder_cs);
                continue;
            }
            /* src is no longer needed after push_input copies the data */
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);

            for (;;)
            {
                INT64 dec_pts, dec_dur;
                DWORD dec_flags;
                BYTE *dec_buf;
                UINT32 dec_size = max_dec_size;

                if (!wm_stream_wait_queue_space(reader, s) || !(dec_buf = malloc(dec_size)))
                    break;

                dec_status = winedmo_transform_get_output(s->decoder, dec_buf, &dec_size,
                                                          &dec_pts, &dec_dur, &dec_flags);
                if (dec_status == STATUS_BUFFER_TOO_SMALL)
                {
                    /* dec_size updated by transform; retry with that allocation */
                    free(dec_buf);
                    continue;
                }
                if (dec_status != STATUS_SUCCESS)
                {
                    free(dec_buf);
                    break;
                }

                if (dec_flags & WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED || !dec_size)
                {
                    /* format change: frame_pending stays true, re-read on next iteration */
                    free(dec_buf);
                    continue;
                }
                if (!wm_stream_update_decoded_video_timing(s, &dec_pts, &dec_dur))
                {
                    free(dec_buf);
                    continue;
                }

                if ((pkt = malloc(sizeof(*pkt))))
                {
                    pkt->data = dec_buf;
                    pkt->size = dec_size;
                    pkt->pts = dec_pts;
                    pkt->duration = dec_dur;
                    pkt->flags = 0;
                    if (IsEqualGUID(&s->major_type, &MFMediaType_Video))
                        pkt->flags |= WM_SF_CLEANPOINT;
                    if (dec_flags & WINEDMO_SAMPLE_FLAG_SYNC_POINT) pkt->flags |= WM_SF_CLEANPOINT;
                    if (dec_flags & WINEDMO_SAMPLE_FLAG_DISCONTINUITY) pkt->flags |= WM_SF_DISCONTINUITY;
                    EnterCriticalSection(&s->queue_cs);
                    if (!s->queue_flushing)
                    {
                        list_add_tail(&s->packet_queue, &pkt->entry);
                        ++s->queued_packets;
                        LeaveCriticalSection(&s->queue_cs);
                        WakeConditionVariable(&s->queue_cv);
                    }
                    else
                    {
                        LeaveCriticalSection(&s->queue_cs);
                        free(pkt->data);
                        free(pkt);
                    }
                }
                else
                    free(dec_buf);
            }
            LeaveCriticalSection(&s->decoder_cs);
        }
        else
        {
            LeaveCriticalSection(&s->decoder_cs);
            if (!(pkt = malloc(sizeof(*pkt))))
            {
                IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
                continue;
            }
            if (!(pkt->data = malloc(data_len)))
            {
                free(pkt);
                IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
                continue;
            }
            memcpy(pkt->data, src, data_len);
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            pkt->size = data_len;
            pkt->pts = (output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIME) ? output.rtTimestamp : -1;
            pkt->duration = (output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH) ? output.rtTimelength : -1;
            pkt->flags = 0;
            if (IsEqualGUID(&s->major_type, &MFMediaType_Video))
                pkt->flags |= WM_SF_CLEANPOINT;
            if (output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT) pkt->flags |= WM_SF_CLEANPOINT;
            if (output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_DISCONTINUITY) pkt->flags |= WM_SF_DISCONTINUITY;
            EnterCriticalSection(&s->queue_cs);
            if (!s->queue_flushing)
            {
                list_add_tail(&s->packet_queue, &pkt->entry);
                ++s->queued_packets;
                LeaveCriticalSection(&s->queue_cs);
                WakeConditionVariable(&s->queue_cv);
            }
            else
            {
                LeaveCriticalSection(&s->queue_cs);
                free(pkt->data);
                free(pkt);
            }
        }
    }

    TRACE("Demux thread stopping for reader %p.\n", reader);
    return 0;
}

static struct wm_reader *impl_from_IWMProfile3(IWMProfile3 *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMProfile3_iface);
}

static HRESULT WINAPI profile_QueryInterface(IWMProfile3 *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMProfile3(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI profile_AddRef(IWMProfile3 *iface)
{
    struct wm_reader *reader = impl_from_IWMProfile3(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI profile_Release(IWMProfile3 *iface)
{
    struct wm_reader *reader = impl_from_IWMProfile3(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI profile_GetVersion(IWMProfile3 *iface, WMT_VERSION *version)
{
    FIXME("iface %p, version %p, stub!\n", iface, version);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetName(IWMProfile3 *iface, WCHAR *name, DWORD *length)
{
    FIXME("iface %p, name %p, length %p, stub!\n", iface, name, length);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_SetName(IWMProfile3 *iface, const WCHAR *name)
{
    FIXME("iface %p, name %s, stub!\n", iface, debugstr_w(name));
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetDescription(IWMProfile3 *iface, WCHAR *description, DWORD *length)
{
    FIXME("iface %p, description %p, length %p, stub!\n", iface, description, length);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_SetDescription(IWMProfile3 *iface, const WCHAR *description)
{
    FIXME("iface %p, description %s, stub!\n", iface, debugstr_w(description));
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetStreamCount(IWMProfile3 *iface, DWORD *count)
{
    struct wm_reader *reader = impl_from_IWMProfile3(iface);

    TRACE("reader %p, count %p.\n", reader, count);

    if (!count)
        return E_INVALIDARG;

    EnterCriticalSection(&reader->cs);
    *count = reader->stream_count;
    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI profile_GetStream(IWMProfile3 *iface, DWORD index, IWMStreamConfig **config)
{
    struct wm_reader *reader = impl_from_IWMProfile3(iface);
    struct stream_config *object;

    TRACE("reader %p, index %lu, config %p.\n", reader, index, config);

    EnterCriticalSection(&reader->cs);

    if (index >= reader->stream_count)
    {
        LeaveCriticalSection(&reader->cs);
        WARN("Index %lu exceeds stream count %u; returning E_INVALIDARG.\n", index, reader->stream_count);
        return E_INVALIDARG;
    }

    if (!(object = calloc(1, sizeof(*object))))
    {
        LeaveCriticalSection(&reader->cs);
        return E_OUTOFMEMORY;
    }

    object->IWMStreamConfig_iface.lpVtbl = &stream_config_vtbl;
    object->IWMMediaProps_iface.lpVtbl = &stream_props_vtbl;
    object->refcount = 1;
    object->stream = &reader->streams[index];
    IWMProfile3_AddRef(&reader->IWMProfile3_iface);

    LeaveCriticalSection(&reader->cs);

    TRACE("Created stream config %p.\n", object);
    *config = &object->IWMStreamConfig_iface;
    return S_OK;
}

static HRESULT WINAPI profile_GetStreamByNumber(IWMProfile3 *iface, WORD stream_number, IWMStreamConfig **config)
{
    HRESULT hr;

    TRACE("iface %p, stream_number %u, config %p.\n", iface, stream_number, config);

    if (!stream_number)
        return NS_E_NO_STREAM;

    hr = profile_GetStream(iface, stream_number - 1, config);
    if (hr == E_INVALIDARG)
        hr = NS_E_NO_STREAM;

    return hr;
}

static HRESULT WINAPI profile_RemoveStream(IWMProfile3 *iface, IWMStreamConfig *config)
{
    FIXME("iface %p, config %p, stub!\n", iface, config);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_RemoveStreamByNumber(IWMProfile3 *iface, WORD stream_number)
{
    FIXME("iface %p, stream_number %u, stub!\n", iface, stream_number);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_AddStream(IWMProfile3 *iface, IWMStreamConfig *config)
{
    FIXME("iface %p, config %p, stub!\n", iface, config);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_ReconfigStream(IWMProfile3 *iface, IWMStreamConfig *config)
{
    FIXME("iface %p, config %p, stub!\n", iface, config);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_CreateNewStream(IWMProfile3 *iface, REFGUID type, IWMStreamConfig **config)
{
    FIXME("iface %p, type %s, config %p, stub!\n", iface, debugstr_guid(type), config);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetMutualExclusionCount(IWMProfile3 *iface, DWORD *count)
{
    FIXME("iface %p, count %p, stub!\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetMutualExclusion(IWMProfile3 *iface, DWORD index, IWMMutualExclusion **excl)
{
    FIXME("iface %p, index %lu, excl %p, stub!\n", iface, index, excl);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_RemoveMutualExclusion(IWMProfile3 *iface, IWMMutualExclusion *excl)
{
    FIXME("iface %p, excl %p, stub!\n", iface, excl);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_AddMutualExclusion(IWMProfile3 *iface, IWMMutualExclusion *excl)
{
    FIXME("iface %p, excl %p, stub!\n", iface, excl);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_CreateNewMutualExclusion(IWMProfile3 *iface, IWMMutualExclusion **excl)
{
    FIXME("iface %p, excl %p, stub!\n", iface, excl);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetProfileID(IWMProfile3 *iface, GUID *id)
{
    FIXME("iface %p, id %p, stub!\n", iface, id);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetStorageFormat(IWMProfile3 *iface, WMT_STORAGE_FORMAT *format)
{
    FIXME("iface %p, format %p, stub!\n", iface, format);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_SetStorageFormat(IWMProfile3 *iface, WMT_STORAGE_FORMAT format)
{
    FIXME("iface %p, format %#x, stub!\n", iface, format);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetBandwidthSharingCount(IWMProfile3 *iface, DWORD *count)
{
    FIXME("iface %p, count %p, stub!\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetBandwidthSharing(IWMProfile3 *iface, DWORD index, IWMBandwidthSharing **sharing)
{
    FIXME("iface %p, index %lu, sharing %p, stub!\n", iface, index, sharing);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_RemoveBandwidthSharing( IWMProfile3 *iface, IWMBandwidthSharing *sharing)
{
    FIXME("iface %p, sharing %p, stub!\n", iface, sharing);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_AddBandwidthSharing(IWMProfile3 *iface, IWMBandwidthSharing *sharing)
{
    FIXME("iface %p, sharing %p, stub!\n", iface, sharing);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_CreateNewBandwidthSharing( IWMProfile3 *iface, IWMBandwidthSharing **sharing)
{
    FIXME("iface %p, sharing %p, stub!\n", iface, sharing);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetStreamPrioritization(IWMProfile3 *iface, IWMStreamPrioritization **stream)
{
    FIXME("iface %p, stream %p, stub!\n", iface, stream);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_SetStreamPrioritization(IWMProfile3 *iface, IWMStreamPrioritization *stream)
{
    FIXME("iface %p, stream %p, stub!\n", iface, stream);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_RemoveStreamPrioritization(IWMProfile3 *iface)
{
    FIXME("iface %p, stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_CreateNewStreamPrioritization(IWMProfile3 *iface, IWMStreamPrioritization **stream)
{
    FIXME("iface %p, stream %p, stub!\n", iface, stream);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetExpectedPacketCount(IWMProfile3 *iface, QWORD duration, QWORD *count)
{
    FIXME("iface %p, duration %s, count %p, stub!\n", iface, debugstr_time(duration), count);
    return E_NOTIMPL;
}

static const IWMProfile3Vtbl profile_vtbl =
{
    profile_QueryInterface,
    profile_AddRef,
    profile_Release,
    profile_GetVersion,
    profile_GetName,
    profile_SetName,
    profile_GetDescription,
    profile_SetDescription,
    profile_GetStreamCount,
    profile_GetStream,
    profile_GetStreamByNumber,
    profile_RemoveStream,
    profile_RemoveStreamByNumber,
    profile_AddStream,
    profile_ReconfigStream,
    profile_CreateNewStream,
    profile_GetMutualExclusionCount,
    profile_GetMutualExclusion,
    profile_RemoveMutualExclusion,
    profile_AddMutualExclusion,
    profile_CreateNewMutualExclusion,
    profile_GetProfileID,
    profile_GetStorageFormat,
    profile_SetStorageFormat,
    profile_GetBandwidthSharingCount,
    profile_GetBandwidthSharing,
    profile_RemoveBandwidthSharing,
    profile_AddBandwidthSharing,
    profile_CreateNewBandwidthSharing,
    profile_GetStreamPrioritization,
    profile_SetStreamPrioritization,
    profile_RemoveStreamPrioritization,
    profile_CreateNewStreamPrioritization,
    profile_GetExpectedPacketCount,
};

static struct wm_reader *impl_from_IWMHeaderInfo3(IWMHeaderInfo3 *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMHeaderInfo3_iface);
}

static HRESULT WINAPI header_info_QueryInterface(IWMHeaderInfo3 *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMHeaderInfo3(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI header_info_AddRef(IWMHeaderInfo3 *iface)
{
    struct wm_reader *reader = impl_from_IWMHeaderInfo3(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI header_info_Release(IWMHeaderInfo3 *iface)
{
    struct wm_reader *reader = impl_from_IWMHeaderInfo3(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI header_info_GetAttributeCount(IWMHeaderInfo3 *iface, WORD stream_number, WORD *count)
{
    FIXME("iface %p, stream_number %u, count %p, stub!\n", iface, stream_number, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetAttributeByIndex(IWMHeaderInfo3 *iface, WORD index, WORD *stream_number,
        WCHAR *name, WORD *name_len, WMT_ATTR_DATATYPE *type, BYTE *value, WORD *size)
{
    FIXME("iface %p, index %u, stream_number %p, name %p, name_len %p, type %p, value %p, size %p, stub!\n",
            iface, index, stream_number, name, name_len, type, value, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetAttributeByName(IWMHeaderInfo3 *iface, WORD *stream_number,
        const WCHAR *name, WMT_ATTR_DATATYPE *type, BYTE *value, WORD *size)
{
    struct wm_reader *reader = impl_from_IWMHeaderInfo3(iface);
    const WORD req_size = *size;

    TRACE("reader %p, stream_number %p, name %s, type %p, value %p, size %u.\n",
            reader, stream_number, debugstr_w(name), type, value, *size);

    if (!stream_number)
        return E_INVALIDARG;

    if (!wcscmp(name, L"Duration"))
    {
        QWORD duration;

        if (*stream_number)
        {
            WARN("Requesting duration for stream %u, returning ASF_E_NOTFOUND.\n", *stream_number);
            return ASF_E_NOTFOUND;
        }

        *size = sizeof(QWORD);
        if (!value)
        {
            *type = WMT_TYPE_QWORD;
            return S_OK;
        }
        if (req_size < *size)
            return ASF_E_BUFFERTOOSMALL;

        *type = WMT_TYPE_QWORD;
        EnterCriticalSection(&reader->cs);
        duration = (QWORD)reader->duration;
        LeaveCriticalSection(&reader->cs);
        TRACE("Returning duration %s.\n", debugstr_time(duration));
        memcpy(value, &duration, sizeof(QWORD));
        return S_OK;
    }
    else if (!wcscmp(name, L"Seekable"))
    {
        if (*stream_number)
        {
            WARN("Requesting duration for stream %u, returning ASF_E_NOTFOUND.\n", *stream_number);
            return ASF_E_NOTFOUND;
        }

        *size = sizeof(BOOL);
        if (!value)
        {
            *type = WMT_TYPE_BOOL;
            return S_OK;
        }
        if (req_size < *size)
            return ASF_E_BUFFERTOOSMALL;

        *type = WMT_TYPE_BOOL;
        *(BOOL *)value = TRUE;
        return S_OK;
    }
    else
    {
        FIXME("Unknown attribute %s.\n", debugstr_w(name));
        return ASF_E_NOTFOUND;
    }
}

static HRESULT WINAPI header_info_SetAttribute(IWMHeaderInfo3 *iface, WORD stream_number,
        const WCHAR *name, WMT_ATTR_DATATYPE type, const BYTE *value, WORD size)
{
    FIXME("iface %p, stream_number %u, name %s, type %#x, value %p, size %u, stub!\n",
            iface, stream_number, debugstr_w(name), type, value, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetMarkerCount(IWMHeaderInfo3 *iface, WORD *count)
{
    FIXME("iface %p, count %p, stub!\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetMarker(IWMHeaderInfo3 *iface,
        WORD index, WCHAR *name, WORD *len, QWORD *time)
{
    FIXME("iface %p, index %u, name %p, len %p, time %p, stub!\n", iface, index, name, len, time);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_AddMarker(IWMHeaderInfo3 *iface, const WCHAR *name, QWORD time)
{
    FIXME("iface %p, name %s, time %s, stub!\n", iface, debugstr_w(name), debugstr_time(time));
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_RemoveMarker(IWMHeaderInfo3 *iface, WORD index)
{
    FIXME("iface %p, index %u, stub!\n", iface, index);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetScriptCount(IWMHeaderInfo3 *iface, WORD *count)
{
    FIXME("iface %p, count %p, stub!\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetScript(IWMHeaderInfo3 *iface, WORD index, WCHAR *type,
        WORD *type_len, WCHAR *command, WORD *command_len, QWORD *time)
{
    FIXME("iface %p, index %u, type %p, type_len %p, command %p, command_len %p, time %p, stub!\n",
            iface, index, type, type_len, command, command_len, time);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_AddScript(IWMHeaderInfo3 *iface,
        const WCHAR *type, const WCHAR *command, QWORD time)
{
    FIXME("iface %p, type %s, command %s, time %s, stub!\n",
            iface, debugstr_w(type), debugstr_w(command), debugstr_time(time));
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_RemoveScript(IWMHeaderInfo3 *iface, WORD index)
{
    FIXME("iface %p, index %u, stub!\n", iface, index);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetCodecInfoCount(IWMHeaderInfo3 *iface, DWORD *count)
{
    FIXME("iface %p, count %p, stub!\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetCodecInfo(IWMHeaderInfo3 *iface, DWORD index, WORD *name_len,
        WCHAR *name, WORD *desc_len, WCHAR *desc, WMT_CODEC_INFO_TYPE *type, WORD *size, BYTE *info)
{
    FIXME("iface %p, index %lu, name_len %p, name %p, desc_len %p, desc %p, type %p, size %p, info %p, stub!\n",
            iface, index, name_len, name, desc_len, desc, type, size, info);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetAttributeCountEx(IWMHeaderInfo3 *iface, WORD stream_number, WORD *count)
{
    FIXME("iface %p, stream_number %u, count %p, stub!\n", iface, stream_number, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetAttributeIndices(IWMHeaderInfo3 *iface, WORD stream_number,
        const WCHAR *name, WORD *lang_index, WORD *indices, WORD *count)
{
    FIXME("iface %p, stream_number %u, name %s, lang_index %p, indices %p, count %p, stub!\n",
            iface, stream_number, debugstr_w(name), lang_index, indices, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetAttributeByIndexEx(IWMHeaderInfo3 *iface,
        WORD stream_number, WORD index, WCHAR *name, WORD *name_len,
        WMT_ATTR_DATATYPE *type, WORD *lang_index, BYTE *value, DWORD *size)
{
    FIXME("iface %p, stream_number %u, index %u, name %p, name_len %p,"
            " type %p, lang_index %p, value %p, size %p, stub!\n",
            iface, stream_number, index, name, name_len, type, lang_index, value, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_ModifyAttribute(IWMHeaderInfo3 *iface, WORD stream_number,
        WORD index, WMT_ATTR_DATATYPE type, WORD lang_index, const BYTE *value, DWORD size)
{
    FIXME("iface %p, stream_number %u, index %u, type %#x, lang_index %u, value %p, size %lu, stub!\n",
            iface, stream_number, index, type, lang_index, value, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_AddAttribute(IWMHeaderInfo3 *iface,
        WORD stream_number, const WCHAR *name, WORD *index,
        WMT_ATTR_DATATYPE type, WORD lang_index, const BYTE *value, DWORD size)
{
    FIXME("iface %p, stream_number %u, name %s, index %p, type %#x, lang_index %u, value %p, size %lu, stub!\n",
            iface, stream_number, debugstr_w(name), index, type, lang_index, value, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_DeleteAttribute(IWMHeaderInfo3 *iface, WORD stream_number, WORD index)
{
    FIXME("iface %p, stream_number %u, index %u, stub!\n", iface, stream_number, index);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_AddCodecInfo(IWMHeaderInfo3 *iface, const WCHAR *name,
        const WCHAR *desc, WMT_CODEC_INFO_TYPE type, WORD size, BYTE *info)
{
    FIXME("iface %p, name %s, desc %s, type %#x, size %u, info %p, stub!\n",
            info, debugstr_w(name), debugstr_w(desc), type, size, info);
    return E_NOTIMPL;
}

static const IWMHeaderInfo3Vtbl header_info_vtbl =
{
    header_info_QueryInterface,
    header_info_AddRef,
    header_info_Release,
    header_info_GetAttributeCount,
    header_info_GetAttributeByIndex,
    header_info_GetAttributeByName,
    header_info_SetAttribute,
    header_info_GetMarkerCount,
    header_info_GetMarker,
    header_info_AddMarker,
    header_info_RemoveMarker,
    header_info_GetScriptCount,
    header_info_GetScript,
    header_info_AddScript,
    header_info_RemoveScript,
    header_info_GetCodecInfoCount,
    header_info_GetCodecInfo,
    header_info_GetAttributeCountEx,
    header_info_GetAttributeIndices,
    header_info_GetAttributeByIndexEx,
    header_info_ModifyAttribute,
    header_info_AddAttribute,
    header_info_DeleteAttribute,
    header_info_AddCodecInfo,
};

static struct wm_reader *impl_from_IWMLanguageList(IWMLanguageList *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMLanguageList_iface);
}

static HRESULT WINAPI language_list_QueryInterface(IWMLanguageList *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMLanguageList(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI language_list_AddRef(IWMLanguageList *iface)
{
    struct wm_reader *reader = impl_from_IWMLanguageList(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI language_list_Release(IWMLanguageList *iface)
{
    struct wm_reader *reader = impl_from_IWMLanguageList(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI language_list_GetLanguageCount(IWMLanguageList *iface, WORD *count)
{
    FIXME("iface %p, count %p, stub!\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI language_list_GetLanguageDetails(IWMLanguageList *iface,
        WORD index, WCHAR *lang, WORD *len)
{
    FIXME("iface %p, index %u, lang %p, len %p, stub!\n", iface, index, lang, len);
    return E_NOTIMPL;
}

static HRESULT WINAPI language_list_AddLanguageByRFC1766String(IWMLanguageList *iface,
        const WCHAR *lang, WORD *index)
{
    FIXME("iface %p, lang %s, index %p, stub!\n", iface, debugstr_w(lang), index);
    return E_NOTIMPL;
}

static const IWMLanguageListVtbl language_list_vtbl =
{
    language_list_QueryInterface,
    language_list_AddRef,
    language_list_Release,
    language_list_GetLanguageCount,
    language_list_GetLanguageDetails,
    language_list_AddLanguageByRFC1766String,
};

static struct wm_reader *impl_from_IWMPacketSize2(IWMPacketSize2 *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMPacketSize2_iface);
}

static HRESULT WINAPI packet_size_QueryInterface(IWMPacketSize2 *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMPacketSize2(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI packet_size_AddRef(IWMPacketSize2 *iface)
{
    struct wm_reader *reader = impl_from_IWMPacketSize2(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI packet_size_Release(IWMPacketSize2 *iface)
{
    struct wm_reader *reader = impl_from_IWMPacketSize2(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI packet_size_GetMaxPacketSize(IWMPacketSize2 *iface, DWORD *size)
{
    FIXME("iface %p, size %p, stub!\n", iface, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI packet_size_SetMaxPacketSize(IWMPacketSize2 *iface, DWORD size)
{
    FIXME("iface %p, size %lu, stub!\n", iface, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI packet_size_GetMinPacketSize(IWMPacketSize2 *iface, DWORD *size)
{
    FIXME("iface %p, size %p, stub!\n", iface, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI packet_size_SetMinPacketSize(IWMPacketSize2 *iface, DWORD size)
{
    FIXME("iface %p, size %lu, stub!\n", iface, size);
    return E_NOTIMPL;
}

static const IWMPacketSize2Vtbl packet_size_vtbl =
{
    packet_size_QueryInterface,
    packet_size_AddRef,
    packet_size_Release,
    packet_size_GetMaxPacketSize,
    packet_size_SetMaxPacketSize,
    packet_size_GetMinPacketSize,
    packet_size_SetMinPacketSize,
};

static struct wm_reader *impl_from_IWMReaderPlaylistBurn(IWMReaderPlaylistBurn *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMReaderPlaylistBurn_iface);
}

static HRESULT WINAPI playlist_QueryInterface(IWMReaderPlaylistBurn *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMReaderPlaylistBurn(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI playlist_AddRef(IWMReaderPlaylistBurn *iface)
{
    struct wm_reader *reader = impl_from_IWMReaderPlaylistBurn(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI playlist_Release(IWMReaderPlaylistBurn *iface)
{
    struct wm_reader *reader = impl_from_IWMReaderPlaylistBurn(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI playlist_InitPlaylistBurn(IWMReaderPlaylistBurn *iface, DWORD count,
        const WCHAR **filenames, IWMStatusCallback *callback, void *context)
{
    FIXME("iface %p, count %lu, filenames %p, callback %p, context %p, stub!\n",
            iface, count, filenames, callback, context);
    return E_NOTIMPL;
}

static HRESULT WINAPI playlist_GetInitResults(IWMReaderPlaylistBurn *iface, DWORD count, HRESULT *hrs)
{
    FIXME("iface %p, count %lu, hrs %p, stub!\n", iface, count, hrs);
    return E_NOTIMPL;
}

static HRESULT WINAPI playlist_Cancel(IWMReaderPlaylistBurn *iface)
{
    FIXME("iface %p, stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI playlist_EndPlaylistBurn(IWMReaderPlaylistBurn *iface, HRESULT hr)
{
    FIXME("iface %p, hr %#lx, stub!\n", iface, hr);
    return E_NOTIMPL;
}

static const IWMReaderPlaylistBurnVtbl playlist_vtbl =
{
    playlist_QueryInterface,
    playlist_AddRef,
    playlist_Release,
    playlist_InitPlaylistBurn,
    playlist_GetInitResults,
    playlist_Cancel,
    playlist_EndPlaylistBurn,
};

static struct wm_reader *impl_from_IWMReaderTimecode(IWMReaderTimecode *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMReaderTimecode_iface);
}

static HRESULT WINAPI timecode_QueryInterface(IWMReaderTimecode *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMReaderTimecode(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI timecode_AddRef(IWMReaderTimecode *iface)
{
    struct wm_reader *reader = impl_from_IWMReaderTimecode(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI timecode_Release(IWMReaderTimecode *iface)
{
    struct wm_reader *reader = impl_from_IWMReaderTimecode(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI timecode_GetTimecodeRangeCount(IWMReaderTimecode *iface,
        WORD stream_number, WORD *count)
{
    FIXME("iface %p, stream_number %u, count %p, stub!\n", iface, stream_number, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI timecode_GetTimecodeRangeBounds(IWMReaderTimecode *iface,
        WORD stream_number, WORD index, DWORD *start, DWORD *end)
{
    FIXME("iface %p, stream_number %u, index %u, start %p, end %p, stub!\n",
            iface, stream_number, index, start, end);
    return E_NOTIMPL;
}

static const IWMReaderTimecodeVtbl timecode_vtbl =
{
    timecode_QueryInterface,
    timecode_AddRef,
    timecode_Release,
    timecode_GetTimecodeRangeCount,
    timecode_GetTimecodeRangeBounds,
};

static void free_stream_buffers(struct wm_reader *reader)
{
    unsigned int i;

    for (i = 0; i < reader->stream_count; ++i)
        wm_flush_stream_queue(&reader->streams[i]);
}

static HRESULT init_stream(struct wm_reader *reader)
{
    NTSTATUS status;
    UINT stream_count;
    WORD i;

    reader->demuxer_stream.stream.p_seek = wm_stream_seek_cb;
    reader->demuxer_stream.stream.p_read = wm_stream_read_cb;
    reader->demuxer_stream.position = 0;

    if ((status = winedmo_demuxer_create(reader->filename, &reader->demuxer_stream.stream,
            reader->file_size, &reader->duration, &stream_count, NULL, &reader->demuxer)))
    {
        ERR("Failed to create demuxer, status %#lx.\n", status);
        return E_FAIL;
    }

    reader->stream_count = stream_count;

    if (!(reader->streams = calloc(reader->stream_count, sizeof(*reader->streams))))
    {
        winedmo_demuxer_destroy(&reader->demuxer);
        return E_OUTOFMEMORY;
    }

    for (i = 0; i < reader->stream_count; ++i)
    {
        struct wm_stream *stream = &reader->streams[i];
        union winedmo_format *fmt = NULL;
        UINT32 fmt_size;

        stream->reader = reader;
        stream->index = i;
        stream->selection = WMT_ON;
        list_init(&stream->packet_queue);
        InitializeCriticalSection(&stream->queue_cs);
        InitializeConditionVariable(&stream->queue_cv);
        InitializeCriticalSection(&stream->decoder_cs);
        wm_stream_reset_decoded_timing(stream);

        if ((status = winedmo_demuxer_stream_type(reader->demuxer, i, &stream->major_type, &fmt)))
        {
            WARN("Failed to get stream type for stream %u, status %#lx.\n", i, status);
            continue;
        }
        if (IsEqualGUID(&stream->major_type, &MFMediaType_Audio))
            fmt_size = sizeof(WAVEFORMATEX) + fmt->audio.cbSize;
        else
            fmt_size = fmt->video.dwSize ? fmt->video.dwSize : sizeof(MFVIDEOFORMAT);

        if ((stream->compressed_fmt = malloc(fmt_size)))
        {
            memcpy(stream->compressed_fmt, fmt, fmt_size);
            stream->compressed_fmt_size = fmt_size;
            if (IsEqualGUID(&stream->major_type, &MFMediaType_Video)
                    && stream->compressed_fmt->video.videoInfo.FramesPerSecond.Numerator
                    && stream->compressed_fmt->video.videoInfo.FramesPerSecond.Denominator)
                stream->nominal_decoded_duration = (INT64)10000000
                        * stream->compressed_fmt->video.videoInfo.FramesPerSecond.Denominator
                        / stream->compressed_fmt->video.videoInfo.FramesPerSecond.Numerator;
        }

        winedmo_decoded_format_from_winedmo(&stream->major_type, fmt, &stream->decoded_winedmo_format);
        free(fmt);
        stream->decode_stream = true;
        wm_stream_create_decoder(stream);
    }

    reader->demux_shutdown = false;
    reader->read_flushing = false;
    if (!(reader->demux_thread = CreateThread(NULL, 0, wm_demux_thread, reader, 0, NULL)))
    {
        ERR("Failed to create demux thread.\n");
        free_stream_buffers(reader);
        for (i = 0; i < reader->stream_count; ++i)
        {
            free(reader->streams[i].compressed_fmt);
            DeleteCriticalSection(&reader->streams[i].decoder_cs);
            DeleteCriticalSection(&reader->streams[i].queue_cs);
        }
        free(reader->streams);
        reader->streams = NULL;
        winedmo_demuxer_destroy(&reader->demuxer);
        return E_FAIL;
    }

    return S_OK;
}

static HRESULT reinit_stream(struct wm_reader *reader)
{
    WORD i;

    /* Signal flushing so the demux thread's read callback returns empty */
    reader->read_flushing = true;
    reader->read_flush_generation++;
    for (i = 0; i < reader->stream_count; ++i)
    {
        struct wm_stream *stream = &reader->streams[i];
        EnterCriticalSection(&stream->queue_cs);
        stream->queue_flushing = true;
        LeaveCriticalSection(&stream->queue_cs);
        WakeAllConditionVariable(&stream->queue_cv);
    }

    /* Flush all queued packets */
    for (i = 0; i < reader->stream_count; ++i)
        wm_flush_stream_queue(&reader->streams[i]);

    /* Seek demuxer back to start_time.  Hold demuxer_cs so the demux thread
     * cannot race us with a concurrent winedmo_demuxer_read call. */
    EnterCriticalSection(&reader->demuxer_cs);
    winedmo_demuxer_seek(reader->demuxer, reader->start_time);
    LeaveCriticalSection(&reader->demuxer_cs);

    /* Recreate decoders and reset EOS flags */
    for (i = 0; i < reader->stream_count; ++i)
    {
        struct wm_stream *stream = &reader->streams[i];

        wm_stream_destroy_decoder(stream);
        wm_stream_reset_decoded_timing(stream);
        if (stream->decode_stream)
            wm_stream_create_decoder(stream);

        stream->eos = false;
        EnterCriticalSection(&stream->queue_cs);
        stream->queue_eos = false;
        stream->queue_flushing = false;
        LeaveCriticalSection(&stream->queue_cs);
    }

    /* Clear read_flushing last so the demux thread only resumes once
     * decoders and queue state are fully reset. */
    reader->read_flushing = false;

    return S_OK;
}

static struct wm_stream *wm_reader_get_stream_by_stream_number(struct wm_reader *reader, WORD stream_number)
{
    if (stream_number && stream_number <= reader->stream_count)
        return &reader->streams[stream_number - 1];
    WARN("Invalid stream number %u.\n", stream_number);
    return NULL;
}

static HRESULT wm_stream_allocate_sample(struct wm_stream *stream, DWORD size, INSSBuffer **sample);

/* Dequeue one sample from a stream's packet queue, blocking until available.
 * Returns NS_E_NO_MORE_SAMPLES on EOS, S_FALSE if flushing (retry), S_OK on success. */
static HRESULT wm_stream_dequeue_sample(struct wm_stream *stream, BYTE **data, UINT32 *size,
        INT64 *pts, INT64 *duration, DWORD *flags)
{
    struct wm_packet *pkt;

    EnterCriticalSection(&stream->queue_cs);
    while (list_empty(&stream->packet_queue) && !stream->queue_eos && !stream->queue_flushing)
        SleepConditionVariableCS(&stream->queue_cv, &stream->queue_cs, INFINITE);

    if (stream->queue_flushing)
    {
        LeaveCriticalSection(&stream->queue_cs);
        return S_FALSE;
    }
    if (list_empty(&stream->packet_queue))
    {
        /* EOS */
        LeaveCriticalSection(&stream->queue_cs);
        return NS_E_NO_MORE_SAMPLES;
    }

    pkt = LIST_ENTRY(list_head(&stream->packet_queue), struct wm_packet, entry);
    list_remove(&pkt->entry);
    LeaveCriticalSection(&stream->queue_cs);

    *data = pkt->data;
    *size = pkt->size;
    *pts = pkt->pts;
    *duration = pkt->duration;
    *flags = pkt->flags;
    free(pkt); /* data ownership transferred to caller */
    return S_OK;
}

static HRESULT wm_reader_read_stream_sample(struct wm_reader *reader, struct wm_stream *stream,
        INSSBuffer **sample, QWORD *pts, QWORD *duration, DWORD *flags)
{
    BYTE *raw_data = NULL;
    UINT32 raw_size = 0;
    INT64 raw_pts, raw_dur;
    DWORD raw_flags;
    HRESULT hr;
    BYTE *buf_data;
    DWORD buf_size;
    DWORD capacity;

    /* If we have a partially consumed packet already, keep using it */
    if (!stream->current_data)
    {
        hr = wm_stream_dequeue_sample(stream, &raw_data, &raw_size, &raw_pts, &raw_dur, &raw_flags);
        if (hr != S_OK) return hr;

        stream->current_data = raw_data;
        stream->current_size = raw_size;
        stream->current_offset = 0;
        stream->current_pts = raw_pts;
        stream->current_duration = raw_dur;
        stream->current_flags = raw_flags;
    }

    capacity = stream->current_size - stream->current_offset;
    if (IsEqualGUID(&stream->major_type, &MFMediaType_Audio) && stream->decode_stream)
        capacity = min(capacity, 16384);

    if (FAILED(hr = wm_stream_allocate_sample(stream, capacity, sample)))
    {
        ERR("Failed to allocate sample of %lu bytes, hr %#lx.\n", capacity, hr);
        free(stream->current_data);
        stream->current_data = NULL;
        return hr;
    }

    if (FAILED(hr = INSSBuffer_GetBufferAndLength(*sample, &buf_data, &buf_size)))
        ERR("Failed to get data pointer, hr %#lx.\n", hr);

    memcpy(buf_data, stream->current_data + stream->current_offset, capacity);
    INSSBuffer_SetLength(*sample, capacity);

    if (stream->current_pts >= 0)
        *pts = (QWORD)(stream->current_pts + (INT64)stream->current_duration * stream->current_offset / stream->current_size);
    else
        *pts = 0;
    if (stream->current_duration >= 0)
        *duration = (QWORD)((INT64)stream->current_duration * capacity / stream->current_size);
    else
        *duration = 0;

    *flags = stream->current_flags;
    /* Only first chunk gets discontinuity/cleanpoint */
    stream->current_flags &= ~(WM_SF_DISCONTINUITY | WM_SF_CLEANPOINT);

    stream->current_offset += capacity;
    if (stream->current_offset >= stream->current_size)
    {
        free(stream->current_data);
        stream->current_data = NULL;
        stream->current_size = 0;
        stream->current_offset = 0;
    }

    return S_OK;
}

static BOOL wm_stream_peek_pending_sample(struct wm_stream *stream, INT64 *pts)
{
    BOOL has_sample = FALSE;

    EnterCriticalSection(&stream->queue_cs);

    if (stream->current_data)
    {
        if (pts)
        {
            if (stream->current_pts >= 0)
                *pts = stream->current_pts
                        + (INT64)stream->current_duration * stream->current_offset / stream->current_size;
            else
                *pts = INT64_MIN;
        }
        has_sample = TRUE;
    }
    else if (!list_empty(&stream->packet_queue))
    {
        struct wm_packet *pkt = LIST_ENTRY(list_head(&stream->packet_queue), struct wm_packet, entry);
        if (pts) *pts = pkt->pts;
        has_sample = TRUE;
    }

    LeaveCriticalSection(&stream->queue_cs);
    return has_sample;
}

static const enum winedmo_video_format video_formats[] =
{
    /* Try to prefer YUV formats over RGB ones. Most decoders output in the
     * YUV color space, and it's generally much less expensive for
     * videoconvert to do YUV -> YUV transformations. */
    WINEDMO_VIDEO_FORMAT_NV12,
    WINEDMO_VIDEO_FORMAT_YV12,
    WINEDMO_VIDEO_FORMAT_YUY2,
    WINEDMO_VIDEO_FORMAT_UYVY,
    WINEDMO_VIDEO_FORMAT_YVYU,
    WINEDMO_VIDEO_FORMAT_BGRA,
    WINEDMO_VIDEO_FORMAT_BGRx,
    WINEDMO_VIDEO_FORMAT_BGR,
    WINEDMO_VIDEO_FORMAT_RGB16,
    WINEDMO_VIDEO_FORMAT_RGB15,
};


static HRESULT wm_stream_allocate_sample(struct wm_stream *stream, DWORD size, INSSBuffer **sample)
{
    struct buffer *buffer;

    if (!stream->read_compressed && stream->output_allocator)
        return IWMReaderAllocatorEx_AllocateForOutputEx(stream->output_allocator, stream->index,
                size, sample, 0, 0, 0, NULL);

    if (stream->read_compressed && stream->stream_allocator)
        return IWMReaderAllocatorEx_AllocateForStreamEx(stream->stream_allocator, stream->index + 1,
                size, sample, 0, 0, 0, NULL);

    /* FIXME: Should these be pooled? */
    if (!(buffer = calloc(1, offsetof(struct buffer, data[size]))))
        return E_OUTOFMEMORY;
    buffer->INSSBuffer_iface.lpVtbl = &buffer_vtbl;
    buffer->refcount = 1;
    buffer->capacity = size;

    TRACE("Created buffer %p.\n", buffer);
    *sample = &buffer->INSSBuffer_iface;
    return S_OK;
}

static struct wm_reader *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IUnknown_inner);
}

static HRESULT WINAPI unknown_inner_QueryInterface(IUnknown *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IUnknown(iface);

    TRACE("reader %p, iid %s, out %p.\n", reader, debugstr_guid(iid), out);

    if (IsEqualIID(iid, &IID_IUnknown)
            || IsEqualIID(iid, &IID_IWMSyncReader)
            || IsEqualIID(iid, &IID_IWMSyncReader2))
        *out = &reader->IWMSyncReader2_iface;
    else if (IsEqualIID(iid, &IID_IWMHeaderInfo)
            || IsEqualIID(iid, &IID_IWMHeaderInfo2)
            || IsEqualIID(iid, &IID_IWMHeaderInfo3))
        *out = &reader->IWMHeaderInfo3_iface;
    else if (IsEqualIID(iid, &IID_IWMLanguageList))
        *out = &reader->IWMLanguageList_iface;
    else if (IsEqualIID(iid, &IID_IWMPacketSize)
            || IsEqualIID(iid, &IID_IWMPacketSize2))
        *out = &reader->IWMPacketSize2_iface;
    else if (IsEqualIID(iid, &IID_IWMProfile)
            || IsEqualIID(iid, &IID_IWMProfile2)
            || IsEqualIID(iid, &IID_IWMProfile3))
        *out = &reader->IWMProfile3_iface;
    else if (IsEqualIID(iid, &IID_IWMReaderPlaylistBurn))
        *out = &reader->IWMReaderPlaylistBurn_iface;
    else if (IsEqualIID(iid, &IID_IWMReaderTimecode))
        *out = &reader->IWMReaderTimecode_iface;
    else
    {
        FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI unknown_inner_AddRef(IUnknown *iface)
{
    struct wm_reader *reader = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedIncrement(&reader->refcount);
    TRACE("%p increasing refcount to %lu.\n", reader, refcount);
    return refcount;
}

static ULONG WINAPI unknown_inner_Release(IUnknown *iface)
{
    struct wm_reader *reader = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&reader->refcount);

    TRACE("%p decreasing refcount to %lu.\n", reader, refcount);

    if (!refcount)
    {
        IWMSyncReader2_Close(&reader->IWMSyncReader2_iface);

        reader->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&reader->cs);
        reader->shutdown_cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&reader->shutdown_cs);

        free(reader);
    }

    return refcount;
}

static const IUnknownVtbl unknown_inner_vtbl =
{
    unknown_inner_QueryInterface,
    unknown_inner_AddRef,
    unknown_inner_Release,
};

static struct wm_reader *impl_from_IWMSyncReader2(IWMSyncReader2 *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMSyncReader2_iface);
}

static HRESULT WINAPI reader_QueryInterface(IWMSyncReader2 *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI reader_AddRef(IWMSyncReader2 *iface)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI reader_Release(IWMSyncReader2 *iface)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI reader_Close(IWMSyncReader2 *iface)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    WORD i;

    TRACE("reader %p.\n", reader);

    EnterCriticalSection(&reader->cs);

    if (!reader->demuxer.handle)
    {
        LeaveCriticalSection(&reader->cs);
        return NS_E_INVALID_REQUEST;
    }

    /* Stop the demux thread */
    reader->demux_shutdown = true;
    /* Wake any waiting consumers so they can exit */
    for (i = 0; i < reader->stream_count; ++i)
    {
        EnterCriticalSection(&reader->streams[i].queue_cs);
        reader->streams[i].queue_flushing = true;
        LeaveCriticalSection(&reader->streams[i].queue_cs);
        WakeAllConditionVariable(&reader->streams[i].queue_cv);
    }
    WaitForSingleObject(reader->demux_thread, INFINITE);
    CloseHandle(reader->demux_thread);
    reader->demux_thread = NULL;

    /* Destroy decoders and flush queues */
    for (i = 0; i < reader->stream_count; ++i)
    {
        struct wm_stream *stream = &reader->streams[i];
        wm_stream_destroy_decoder(stream);
        wm_flush_stream_queue(stream);
        DeleteCriticalSection(&stream->decoder_cs);
        DeleteCriticalSection(&stream->queue_cs);
        free(stream->compressed_fmt);
        stream->compressed_fmt = NULL;
        if (stream->output_allocator)
        {
            IWMReaderAllocatorEx_Release(stream->output_allocator);
            stream->output_allocator = NULL;
        }
        if (stream->stream_allocator)
        {
            IWMReaderAllocatorEx_Release(stream->stream_allocator);
            stream->stream_allocator = NULL;
        }
    }
    free(reader->streams);
    reader->streams = NULL;
    reader->stream_count = 0;

    winedmo_demuxer_destroy(&reader->demuxer);

    if (reader->source_stream)
        IStream_Release(reader->source_stream);
    reader->source_stream = NULL;
    if (reader->file)
        CloseHandle(reader->file);
    reader->file = NULL;
    free(reader->filename);
    reader->filename = NULL;

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetMaxOutputSampleSize(IWMSyncReader2 *iface, DWORD output, DWORD *max)
{
    struct wm_reader *This = impl_from_IWMSyncReader2(iface);
    FIXME("(%p)->(%lu %p): stub!\n", This, output, max);
    return E_NOTIMPL;
}

static HRESULT WINAPI reader_GetMaxStreamSampleSize(IWMSyncReader2 *iface, WORD stream_number, DWORD *size)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, stream_number %u, size %p.\n", reader, stream_number, size);

    EnterCriticalSection(&reader->cs);

    if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    *size = winedmo_format_get_max_size(&stream->decoded_winedmo_format);

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetNextSample(IWMSyncReader2 *iface,
        WORD stream_number, INSSBuffer **sample, QWORD *pts, QWORD *duration,
        DWORD *flags, DWORD *output_number, WORD *ret_stream_number)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;
    HRESULT hr;

    TRACE("reader %p, stream_number %u, sample %p, pts %p, duration %p,"
            " flags %p, output_number %p, ret_stream_number %p.\n",
            reader, stream_number, sample, pts, duration, flags, output_number, ret_stream_number);

    if (!stream_number && !output_number && !ret_stream_number)
        return E_INVALIDARG;

    if (reader->outer == &reader->IUnknown_inner)
        EnterCriticalSection(&reader->cs);

    if (!stream_number)
    {
        /* Any-stream read: pick the pending sample with the earliest timestamp
         * across all selected streams instead of starving later streams by
         * always returning the first stream in index order. */
        struct wm_stream *best_stream = NULL;
        WORD i, first_active = reader->stream_count;
        INT64 best_pts = INT64_MAX;
        BOOL best_has_pts = FALSE;

        for (i = 0; i < reader->stream_count; ++i)
        {
            INT64 candidate_pts = INT64_MIN;
            BOOL has_sample;

            stream = &reader->streams[i];
            if (stream->selection == WMT_OFF || stream->eos) continue;

            if (first_active == reader->stream_count)
                first_active = i;

            has_sample = wm_stream_peek_pending_sample(stream, &candidate_pts);
            if (!has_sample)
                continue;

            if (!best_stream)
            {
                best_stream = stream;
                best_pts = candidate_pts;
                best_has_pts = (candidate_pts >= 0);
                continue;
            }

            if (candidate_pts >= 0)
            {
                if (!best_has_pts || candidate_pts < best_pts)
                {
                    best_stream = stream;
                    best_pts = candidate_pts;
                    best_has_pts = TRUE;
                }
            }
        }

        if (best_stream)
        {
            hr = wm_reader_read_stream_sample(reader, best_stream, sample, pts, duration, flags);
            if (hr == S_OK)
            {
                stream_number = best_stream->index + 1;
                goto done;
            }
            if (hr == NS_E_NO_MORE_SAMPLES)
                best_stream->eos = true;
        }
        else if (first_active < reader->stream_count)
        {
            stream = &reader->streams[first_active];
            hr = wm_reader_read_stream_sample(reader, stream, sample, pts, duration, flags);
            if (hr == S_OK)
            {
                stream_number = stream->index + 1;
                goto done;
            }
            if (hr == NS_E_NO_MORE_SAMPLES)
                stream->eos = true;
        }
        else
            hr = NS_E_NO_MORE_SAMPLES;

        goto done;
    }

    if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
    {
        hr = E_INVALIDARG;
        goto done;
    }
    if (stream->selection == WMT_OFF)
    {
        hr = NS_E_INVALID_REQUEST;
        goto done;
    }
    if (stream->eos)
    {
        hr = NS_E_NO_MORE_SAMPLES;
        goto done;
    }

    hr = wm_reader_read_stream_sample(reader, stream, sample, pts, duration, flags);
    if (hr == NS_E_NO_MORE_SAMPLES)
        stream->eos = true;

done:
    if (output_number && hr == S_OK)
        *output_number = stream_number - 1;
    if (ret_stream_number && (hr == S_OK || stream_number))
        *ret_stream_number = stream_number;

    if (reader->outer == &reader->IUnknown_inner)
        LeaveCriticalSection(&reader->cs);
    return hr;
}

static HRESULT WINAPI reader_GetOutputCount(IWMSyncReader2 *iface, DWORD *count)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);

    TRACE("reader %p, count %p.\n", reader, count);

    EnterCriticalSection(&reader->cs);
    *count = reader->stream_count;
    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetOutputFormat(IWMSyncReader2 *iface,
        DWORD output, DWORD index, IWMOutputMediaProps **props)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;
    struct winedmo_codec_format format;

    TRACE("reader %p, output %lu, index %lu, props %p.\n", reader, output, index, props);

    EnterCriticalSection(&reader->cs);

    if (!(stream = get_stream_by_output_number(reader, output)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    format = stream->decoded_winedmo_format;

    switch (format.major_type)
    {
        case WINEDMO_MAJOR_TYPE_VIDEO:
            if (index >= ARRAY_SIZE(video_formats))
            {
                LeaveCriticalSection(&reader->cs);
                return NS_E_INVALID_OUTPUT_FORMAT;
            }
            format.u.video.format = video_formats[index];
            /* API consumers expect RGB video to be bottom-up. YUV/FOURCC
             * formats do not use the DIB bottom-up convention. */
            if (format.u.video.height > 0 && winedmo_video_format_is_rgb(format.u.video.format))
                format.u.video.height = -format.u.video.height;
            else if (format.u.video.height < 0 && !winedmo_video_format_is_rgb(format.u.video.format))
                format.u.video.height = -format.u.video.height;
            break;

        case WINEDMO_MAJOR_TYPE_AUDIO:
            if (index)
            {
                LeaveCriticalSection(&reader->cs);
                return NS_E_INVALID_OUTPUT_FORMAT;
            }
            format.u.audio.format = WINEDMO_AUDIO_FORMAT_S16LE;
            break;

        default:
            LeaveCriticalSection(&reader->cs);
            return NS_E_INVALID_OUTPUT_FORMAT;
    }

    LeaveCriticalSection(&reader->cs);

    *props = output_props_create(&format);
    return *props ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI reader_GetOutputFormatCount(IWMSyncReader2 *iface, DWORD output, DWORD *count)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, output %lu, count %p.\n", reader, output, count);

    EnterCriticalSection(&reader->cs);

    if (!(stream = get_stream_by_output_number(reader, output)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    if (stream->decoded_winedmo_format.major_type == WINEDMO_MAJOR_TYPE_VIDEO)
        *count = ARRAY_SIZE(video_formats);
    else
        *count = 1;

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetOutputNumberForStream(IWMSyncReader2 *iface,
        WORD stream_number, DWORD *output)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);

    TRACE("reader %p, stream_number %u, output %p.\n", reader, stream_number, output);

    *output = stream_number - 1;
    return S_OK;
}

static HRESULT WINAPI reader_GetOutputProps(IWMSyncReader2 *iface,
        DWORD output, IWMOutputMediaProps **props)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, output %lu, props %p.\n", reader, output, props);

    EnterCriticalSection(&reader->cs);

    if (!(stream = get_stream_by_output_number(reader, output)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    *props = output_props_create(&stream->decoded_winedmo_format);
    LeaveCriticalSection(&reader->cs);
    return *props ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI reader_GetOutputSetting(IWMSyncReader2 *iface, DWORD output_num, const WCHAR *name,
        WMT_ATTR_DATATYPE *type, BYTE *value, WORD *length)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;
    BOOL ret;

    TRACE("reader %p, output %lu, name %s, type %p, value %p, length %p.\n",
            reader, output_num, debugstr_w(name), type, value, length);

    if (!wcscmp(name, L"VideoSampleDurations"))
    {
        EnterCriticalSection(&reader->cs);

        if (!(stream = get_stream_by_output_number(reader, output_num))
                || !IsEqualGUID(&stream->major_type, &MFMediaType_Video))
        {
            LeaveCriticalSection(&reader->cs);
            return E_INVALIDARG;
        }

        ret = stream->video_sample_durations;
        LeaveCriticalSection(&reader->cs);

        if (type)
            *type = WMT_TYPE_BOOL;
        if (!length)
            return E_INVALIDARG;
        if (!value || *length < sizeof(ret))
        {
            *length = sizeof(ret);
            return NS_E_INVALID_REQUEST;
        }

        memcpy(value, &ret, sizeof(ret));
        *length = sizeof(ret);
        return S_OK;
    }

    FIXME("Unknown setting %s; returning E_NOTIMPL.\n", debugstr_w(name));
    return E_NOTIMPL;
}

static HRESULT WINAPI reader_GetReadStreamSamples(IWMSyncReader2 *iface, WORD stream_number, BOOL *compressed)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, stream_number %u, compressed %p.\n", reader, stream_number, compressed);

    EnterCriticalSection(&reader->cs);

    if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    *compressed = stream->read_compressed;

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetStreamNumberForOutput(IWMSyncReader2 *iface,
        DWORD output, WORD *stream_number)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);

    TRACE("reader %p, output %lu, stream_number %p.\n", reader, output, stream_number);

    *stream_number = output + 1;
    return S_OK;
}

static HRESULT WINAPI reader_GetStreamSelected(IWMSyncReader2 *iface,
        WORD stream_number, WMT_STREAM_SELECTION *selection)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, stream_number %u, selection %p.\n", reader, stream_number, selection);

    EnterCriticalSection(&reader->cs);

    if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    *selection = stream->selection;

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_Open(IWMSyncReader2 *iface, const WCHAR *filename)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    LARGE_INTEGER size;
    HANDLE file;
    HRESULT hr;

    TRACE("reader %p, filename %s.\n", reader, debugstr_w(filename));

    if ((file = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, 0, NULL)) == INVALID_HANDLE_VALUE)
    {
        ERR("Failed to open %s, error %lu.\n", debugstr_w(filename), GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (!GetFileSizeEx(file, &size))
    {
        ERR("Failed to get the size of %s, error %lu.\n", debugstr_w(filename), GetLastError());
        CloseHandle(file);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    EnterCriticalSection(&reader->cs);

    if (reader->demuxer.handle)
    {
        LeaveCriticalSection(&reader->cs);
        WARN("Stream is already open; returning E_UNEXPECTED.\n");
        CloseHandle(file);
        return E_UNEXPECTED;
    }

    reader->filename = wcsdup(filename);
    reader->file = file;
    reader->file_size = size.QuadPart;

    if (FAILED(hr = init_stream(reader)))
    {
        CloseHandle(reader->file);
        reader->file = NULL;
        free(reader->filename);
        reader->filename = NULL;
    }

    LeaveCriticalSection(&reader->cs);
    return hr;
}

static HRESULT WINAPI reader_OpenStream(IWMSyncReader2 *iface, IStream *stream)
{
    static const ULONG64 canary_size = 0xdeadbeeffeedcafe;
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    STATSTG stat;
    HRESULT hr;

    TRACE("reader %p, stream %p.\n", reader, stream);

    stat.cbSize.QuadPart = canary_size;
    if (FAILED(hr = IStream_Stat(stream, &stat, STATFLAG_NONAME)))
    {
        ERR("Failed to stat stream, hr %#lx.\n", hr);
        return hr;
    }

    if (stat.cbSize.QuadPart == canary_size)
    {
        /* Call of Juarez: Gunslinger implements IStream_Stat as an empty function returning S_OK, leaving
         * the output stat unchanged. Windows doesn't call IStream_Seek(_SEEK_END) and probably validates
         * the size against WMV file headers so the bigger cbSize doesn't change anything.
         * Such streams work as soon as the uninitialized cbSize is big enough which is usually the case
         * (if that is not the case Windows will favour shorter cbSize). */
        static const LARGE_INTEGER zero = { 0 };
        ULARGE_INTEGER pos = { .QuadPart = canary_size };

        if (SUCCEEDED(hr = IStream_Seek(stream, zero, STREAM_SEEK_END, &pos)))
            IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
        stat.cbSize.QuadPart = pos.QuadPart == canary_size ? 0 : pos.QuadPart;
        ERR("IStream_Stat did not fill the stream size, size from _Seek %I64u.\n", stat.cbSize.QuadPart);
    }

    EnterCriticalSection(&reader->cs);

    if (reader->demuxer.handle)
    {
        LeaveCriticalSection(&reader->cs);
        WARN("Stream is already open; returning E_UNEXPECTED.\n");
        return E_UNEXPECTED;
    }

    IStream_AddRef(reader->source_stream = stream);
    reader->file_size = stat.cbSize.QuadPart;

    if (FAILED(hr = init_stream(reader)))
    {
        IStream_Release(stream);
        reader->source_stream = NULL;
    }

    LeaveCriticalSection(&reader->cs);
    return hr;
}

static HRESULT WINAPI reader_SetOutputProps(IWMSyncReader2 *iface, DWORD output, IWMOutputMediaProps *props_iface)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct output_props *props = unsafe_impl_from_IWMOutputMediaProps(props_iface);
    struct winedmo_codec_format format;
    struct wm_stream *stream;
    HRESULT hr = S_OK;
    int i;

    TRACE("reader %p, output %lu, props_iface %p.\n", reader, output, props_iface);

    strmbase_dump_media_type(&props->mt);

    if (!amt_to_winedmo_format(&props->mt, &format))
    {
        ERR("Failed to convert media type to winedmo_format.\n");
        return E_FAIL;
    }

    EnterCriticalSection(&reader->cs);

    if (!(stream = get_stream_by_output_number(reader, output)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    if (stream->decoded_winedmo_format.major_type != format.major_type)
    {
        /* R.U.S.E sets the type of the wrong stream, apparently by accident. */
        hr = NS_E_INCOMPATIBLE_FORMAT;
    }
    else switch (format.major_type)
    {
        case WINEDMO_MAJOR_TYPE_AUDIO:
            if (format.u.audio.format == WINEDMO_AUDIO_FORMAT_UNKNOWN)
                hr = NS_E_AUDIO_CODEC_NOT_INSTALLED;
            else if (format.u.audio.channels > stream->decoded_winedmo_format.u.audio.channels)
                hr = NS_E_AUDIO_CODEC_NOT_INSTALLED;
            break;

        case WINEDMO_MAJOR_TYPE_VIDEO:
            for (i = 0; i < ARRAY_SIZE(video_formats); ++i)
                if (format.u.video.format == video_formats[i])
                    break;
            if (i == ARRAY_SIZE(video_formats))
                hr = NS_E_INVALID_OUTPUT_FORMAT;
            else if (stream->decoded_winedmo_format.u.video.width != format.u.video.width)
                hr = NS_E_INVALID_OUTPUT_FORMAT;
            else if (abs(stream->decoded_winedmo_format.u.video.height) != abs(format.u.video.height))
                hr = NS_E_INVALID_OUTPUT_FORMAT;
            break;

        default:
            hr = NS_E_INCOMPATIBLE_FORMAT;
            break;
    }

    if (FAILED(hr))
    {
        WARN("Unsupported media type, returning %#lx.\n", hr);
        LeaveCriticalSection(&reader->cs);
        return hr;
    }

    if (!memcmp(&stream->decoded_winedmo_format, &format, sizeof(format)))
    {
        TRACE("Output format unchanged, keeping current stream state.\n");
        LeaveCriticalSection(&reader->cs);
        return S_OK;
    }

    stream->decoded_winedmo_format = format;

    reinit_stream(reader);

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_SetOutputSetting(IWMSyncReader2 *iface, DWORD output,
        const WCHAR *name, WMT_ATTR_DATATYPE type, const BYTE *value, WORD size)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);

    TRACE("reader %p, output %lu, name %s, type %#x, value %p, size %u.\n",
            reader, output, debugstr_w(name), type, value, size);

    if (!wcscmp(name, L"VideoSampleDurations"))
    {
        struct wm_stream *stream;

        if (type != WMT_TYPE_BOOL || size < sizeof(BOOL))
            return E_INVALIDARG;

        EnterCriticalSection(&reader->cs);

        if (!(stream = get_stream_by_output_number(reader, output))
                || !IsEqualGUID(&stream->major_type, &MFMediaType_Video))
        {
            LeaveCriticalSection(&reader->cs);
            return E_INVALIDARG;
        }

        stream->video_sample_durations = !!*(const BOOL *)value;
        LeaveCriticalSection(&reader->cs);
        return S_OK;
    }
    if (!wcscmp(name, L"EnableDiscreteOutput"))
    {
        FIXME("Ignoring EnableDiscreteOutput setting.\n");
        return S_OK;
    }
    if (!wcscmp(name, L"SpeakerConfig"))
    {
        FIXME("Ignoring SpeakerConfig setting.\n");
        return S_OK;
    }
    else
    {
        FIXME("Unknown setting %s; returning E_NOTIMPL.\n", debugstr_w(name));
        return E_NOTIMPL;
    }
}

static HRESULT WINAPI reader_SetRange(IWMSyncReader2 *iface, QWORD start, LONGLONG duration)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);

    TRACE("reader %p, start %I64u, duration %I64d.\n", reader, start, duration);

    EnterCriticalSection(&reader->cs);

    reader->start_time = start;
    reinit_stream(reader);

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_SetRangeByFrame(IWMSyncReader2 *iface, WORD stream_num, QWORD frame_num,
        LONGLONG frames)
{
    struct wm_reader *This = impl_from_IWMSyncReader2(iface);
    FIXME("(%p)->(%d %s %s): stub!\n", This, stream_num, wine_dbgstr_longlong(frame_num), wine_dbgstr_longlong(frames));
    return E_NOTIMPL;
}

static HRESULT WINAPI reader_SetReadStreamSamples(IWMSyncReader2 *iface, WORD stream_number, BOOL compressed)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;
    bool decode;

    TRACE("reader %p, stream_index %u, compressed %d.\n", reader, stream_number, compressed);

    EnterCriticalSection(&reader->cs);

    if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    stream->read_compressed = compressed;

    decode = !compressed;

    if (stream->decode_stream != decode)
    {
        stream->decode_stream = decode;
        reinit_stream(reader);
    }

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_SetStreamsSelected(IWMSyncReader2 *iface,
        WORD count, WORD *stream_numbers, WMT_STREAM_SELECTION *selections)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;
    BOOL prefer_single_audio = FALSE;
    UINT selected_audio_stream = UINT_MAX;
    WORD enabled_audio_count = 0;
    WORD i;

    TRACE("reader %p, count %u, stream_numbers %p, selections %p.\n",
            reader, count, stream_numbers, selections);

    if (!count)
        return E_INVALIDARG;

    EnterCriticalSection(&reader->cs);

    for (i = 0; i < count; ++i)
    {
        if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_numbers[i])))
        {
            LeaveCriticalSection(&reader->cs);
            WARN("Invalid stream number %u; returning NS_E_INVALID_REQUEST.\n", stream_numbers[i]);
            return NS_E_INVALID_REQUEST;
        }
        if (IsEqualGUID(&stream->major_type, &MFMediaType_Audio)
                && selections[i] != WMT_OFF)
        {
            ++enabled_audio_count;
            selected_audio_stream = stream->index;
        }
    }

    prefer_single_audio = reader->preferred_audio_stream != UINT_MAX && enabled_audio_count
            && (enabled_audio_count > 1 || selected_audio_stream != reader->preferred_audio_stream);

    for (i = 0; i < count; ++i)
    {
        stream = wm_reader_get_stream_by_stream_number(reader, stream_numbers[i]);
        if (selections[i] != WMT_ON && selections[i] != WMT_OFF)
            FIXME("Ignoring selection %#x for stream %u; treating as enabled.\n",
                    selections[i], stream_numbers[i]);
        if (prefer_single_audio && IsEqualGUID(&stream->major_type, &MFMediaType_Audio))
            stream->selection = stream->index == reader->preferred_audio_stream ? WMT_ON : WMT_OFF;
        else
            stream->selection = selections[i];
    }

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_SetRangeByTimecode(IWMSyncReader2 *iface, WORD stream_num,
        WMT_TIMECODE_EXTENSION_DATA *start, WMT_TIMECODE_EXTENSION_DATA *end)
{
    struct wm_reader *This = impl_from_IWMSyncReader2(iface);
    FIXME("(%p)->(%u %p %p): stub!\n", This, stream_num, start, end);
    return E_NOTIMPL;
}

static HRESULT WINAPI reader_SetRangeByFrameEx(IWMSyncReader2 *iface, WORD stream_num, QWORD frame_num,
        LONGLONG frames_to_read, QWORD *starttime)
{
    struct wm_reader *This = impl_from_IWMSyncReader2(iface);
    FIXME("(%p)->(%u %s %s %p): stub!\n", This, stream_num, wine_dbgstr_longlong(frame_num),
          wine_dbgstr_longlong(frames_to_read), starttime);
    return E_NOTIMPL;
}

static HRESULT WINAPI reader_SetAllocateForOutput(IWMSyncReader2 *iface, DWORD output, IWMReaderAllocatorEx *allocator)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, output %lu, allocator %p.\n", reader, output, allocator);

    EnterCriticalSection(&reader->cs);

    if (!(stream = get_stream_by_output_number(reader, output)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    if (stream->output_allocator)
        IWMReaderAllocatorEx_Release(stream->output_allocator);
    if ((stream->output_allocator = allocator))
        IWMReaderAllocatorEx_AddRef(stream->output_allocator);

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetAllocateForOutput(IWMSyncReader2 *iface, DWORD output, IWMReaderAllocatorEx **allocator)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, output %lu, allocator %p.\n", reader, output, allocator);

    if (!allocator)
        return E_INVALIDARG;

    EnterCriticalSection(&reader->cs);

    if (!(stream = get_stream_by_output_number(reader, output)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    stream = reader->streams + output;
    if ((*allocator = stream->output_allocator))
        IWMReaderAllocatorEx_AddRef(*allocator);

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_SetAllocateForStream(IWMSyncReader2 *iface, DWORD stream_number, IWMReaderAllocatorEx *allocator)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, stream_number %lu, allocator %p.\n", reader, stream_number, allocator);

    EnterCriticalSection(&reader->cs);

    if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    if (stream->stream_allocator)
        IWMReaderAllocatorEx_Release(stream->stream_allocator);
    if ((stream->stream_allocator = allocator))
        IWMReaderAllocatorEx_AddRef(stream->stream_allocator);

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetAllocateForStream(IWMSyncReader2 *iface, DWORD stream_number, IWMReaderAllocatorEx **allocator)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, stream_number %lu, allocator %p.\n", reader, stream_number, allocator);

    if (!allocator)
        return E_INVALIDARG;

    EnterCriticalSection(&reader->cs);

    if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    if ((*allocator = stream->stream_allocator))
        IWMReaderAllocatorEx_AddRef(*allocator);

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static const IWMSyncReader2Vtbl reader_vtbl =
{
    reader_QueryInterface,
    reader_AddRef,
    reader_Release,
    reader_Open,
    reader_Close,
    reader_SetRange,
    reader_SetRangeByFrame,
    reader_GetNextSample,
    reader_SetStreamsSelected,
    reader_GetStreamSelected,
    reader_SetReadStreamSamples,
    reader_GetReadStreamSamples,
    reader_GetOutputSetting,
    reader_SetOutputSetting,
    reader_GetOutputCount,
    reader_GetOutputProps,
    reader_SetOutputProps,
    reader_GetOutputFormatCount,
    reader_GetOutputFormat,
    reader_GetOutputNumberForStream,
    reader_GetStreamNumberForOutput,
    reader_GetMaxOutputSampleSize,
    reader_GetMaxStreamSampleSize,
    reader_OpenStream,
    reader_SetRangeByTimecode,
    reader_SetRangeByFrameEx,
    reader_SetAllocateForOutput,
    reader_GetAllocateForOutput,
    reader_SetAllocateForStream,
    reader_GetAllocateForStream
};

HRESULT WINAPI winedmo_create_wm_sync_reader(IUnknown *outer, void **out)
{
    struct wm_reader *object;

    TRACE("out %p.\n", out);

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IUnknown_inner.lpVtbl = &unknown_inner_vtbl;
    object->IWMSyncReader2_iface.lpVtbl = &reader_vtbl;
    object->IWMHeaderInfo3_iface.lpVtbl = &header_info_vtbl;
    object->IWMLanguageList_iface.lpVtbl = &language_list_vtbl;
    object->IWMPacketSize2_iface.lpVtbl = &packet_size_vtbl;
    object->IWMProfile3_iface.lpVtbl = &profile_vtbl;
    object->IWMReaderPlaylistBurn_iface.lpVtbl = &playlist_vtbl;
    object->IWMReaderTimecode_iface.lpVtbl = &timecode_vtbl;
    object->outer = outer ? outer : &object->IUnknown_inner;
    object->refcount = 1;

    InitializeCriticalSectionEx(&object->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    object->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": reader.cs");
    InitializeCriticalSection(&object->demuxer_cs);

    TRACE("Created reader %p.\n", object);
    *out = outer ? (void *)&object->IUnknown_inner : (void *)&object->IWMSyncReader2_iface;
    return S_OK;
}
