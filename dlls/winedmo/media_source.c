/* Media Foundation Media Source — winedmo demuxer backend
 *
 * Copyright 2020 Derek Lesho
 * Copyright 2020 Zebediah Figura for CodeWeavers
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

#include "mfapi.h"
#include "mferror.h"
#include "urlmon.h"

#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

/* ========================================================================
 * object_context — carries state through async BeginCreateObject
 * ======================================================================== */

struct object_context
{
    IUnknown IUnknown_iface;
    LONG refcount;

    IMFAsyncResult *result;
    IMFByteStream *stream;
    UINT64 file_size;
    WCHAR *url;
};

static struct object_context *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct object_context, IUnknown_iface);
}

static HRESULT WINAPI object_context_QueryInterface(IUnknown *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI object_context_AddRef(IUnknown *iface)
{
    struct object_context *context = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedIncrement(&context->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI object_context_Release(IUnknown *iface)
{
    struct object_context *context = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&context->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        IMFAsyncResult_Release(context->result);
        IMFByteStream_Release(context->stream);
        free(context->url);
        free(context);
    }

    return refcount;
}

static const IUnknownVtbl object_context_vtbl =
{
    object_context_QueryInterface,
    object_context_AddRef,
    object_context_Release,
};

static HRESULT object_context_create(DWORD flags, IMFByteStream *stream, const WCHAR *url,
        QWORD file_size, IMFAsyncResult *result, IUnknown **out)
{
    WCHAR *tmp_url = url ? wcsdup(url) : NULL;
    struct object_context *context;

    if (!(context = calloc(1, sizeof(*context))))
    {
        free(tmp_url);
        return E_OUTOFMEMORY;
    }

    context->IUnknown_iface.lpVtbl = &object_context_vtbl;
    context->refcount = 1;
    context->stream = stream;
    IMFByteStream_AddRef(context->stream);
    context->file_size = file_size;
    context->url = tmp_url;
    context->result = result;
    IMFAsyncResult_AddRef(context->result);

    *out = &context->IUnknown_iface;
    return S_OK;
}

/* ========================================================================
 * IMediaBuffer — lightweight wrapper used with winedmo_demuxer_read
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
    if (len)  *len  = buf->length;
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
 * Per-stream packet queue
 * ======================================================================== */

struct media_packet
{
    struct list entry;
    BYTE *data;
    UINT32 size;
    INT64 dts;      /* INT64_MIN if unavailable */
    INT64 pts;      /* INT64_MIN if unavailable */
    INT64 duration; /* INT64_MIN if unavailable */
};

static void media_packet_free(struct media_packet *pkt)
{
    free(pkt->data);
    free(pkt);
}

#define MEDIA_SOURCE_MAX_QUEUED_PACKETS 8
#define MEDIA_SOURCE_MAX_QUEUED_BYTES   (8 * 1024 * 1024)

struct media_stream;

static void media_packet_queue_flush(struct media_stream *stream);

/* ========================================================================
 * Core structures
 * ======================================================================== */

struct media_stream
{
    IMFMediaStream IMFMediaStream_iface;
    LONG ref;

    IMFMediaSource *media_source;
    IMFMediaEventQueue *event_queue;
    IMFStreamDescriptor *descriptor;

    UINT stream_index;
    BOOL is_video;
    struct list packet_queue;
    CRITICAL_SECTION queue_cs;
    CONDITION_VARIABLE queue_cv;

    IUnknown **token_queue;
    LONG token_queue_count;
    LONG token_queue_cap;

    DWORD stream_id;
    BOOL active;
    BOOL eos;
    UINT queued_packets;
    UINT64 queued_bytes;
};

static void media_packet_queue_flush(struct media_stream *stream)
{
    struct media_packet *pkt, *next;
    LIST_FOR_EACH_ENTRY_SAFE(pkt, next, &stream->packet_queue, struct media_packet, entry)
    {
        list_remove(&pkt->entry);
        media_packet_free(pkt);
    }
    stream->queued_packets = 0;
    stream->queued_bytes = 0;
}

enum source_async_op
{
    SOURCE_ASYNC_START,
    SOURCE_ASYNC_PAUSE,
    SOURCE_ASYNC_STOP,
    SOURCE_ASYNC_REQUEST_SAMPLE,
};

struct source_async_command
{
    IUnknown IUnknown_iface;
    LONG refcount;
    enum source_async_op op;
    union
    {
        struct
        {
            IMFPresentationDescriptor *descriptor;
            GUID format;
            PROPVARIANT position;
        } start;
        struct
        {
            struct media_stream *stream;
            IUnknown *token;
        } request_sample;
    } u;
};

struct media_source
{
    IMFMediaSource IMFMediaSource_iface;
    IMFGetService IMFGetService_iface;
    IMFRateSupport IMFRateSupport_iface;
    IMFRateControl IMFRateControl_iface;
    IMFMediaShutdownNotify IMFMediaShutdownNotify_iface;
    IMFAsyncCallback async_commands_callback;
    LONG ref;
    DWORD async_commands_queue;
    IMFMediaEventQueue *event_queue;
    IMFByteStream *byte_stream;

    IMFAsyncResult *shutdown_result;

    CRITICAL_SECTION cs;

    UINT64 file_size;
    UINT64 duration;
    bool has_duration;

    struct winedmo_demuxer demuxer;
    struct
    {
        struct winedmo_stream stream;
        LONGLONG position;
    } demuxer_stream;

    HANDLE demux_thread;
    bool demux_thread_shutdown;
    bool read_flushing;
    bool flushing;
    bool demux_eof;
    bool eop_queued;

    IMFStreamDescriptor **descriptors;
    struct media_stream **streams;
    ULONG stream_count;

    enum
    {
        SOURCE_OPENING,
        SOURCE_STOPPED,
        SOURCE_PAUSED,
        SOURCE_RUNNING,
        SOURCE_SHUTDOWN,
    } state;
    float rate;
};

/* ========================================================================
 * Forward declarations
 * ======================================================================== */

static inline struct media_stream *impl_from_IMFMediaStream(IMFMediaStream *iface)
{
    return CONTAINING_RECORD(iface, struct media_stream, IMFMediaStream_iface);
}

static inline struct media_source *impl_from_IMFMediaSource(IMFMediaSource *iface)
{
    return CONTAINING_RECORD(iface, struct media_source, IMFMediaSource_iface);
}

static inline struct media_source *impl_from_IMFGetService(IMFGetService *iface)
{
    return CONTAINING_RECORD(iface, struct media_source, IMFGetService_iface);
}

static inline struct media_source *impl_from_IMFRateSupport(IMFRateSupport *iface)
{
    return CONTAINING_RECORD(iface, struct media_source, IMFRateSupport_iface);
}

static inline struct media_source *impl_from_IMFRateControl(IMFRateControl *iface)
{
    return CONTAINING_RECORD(iface, struct media_source, IMFRateControl_iface);
}

static inline struct media_source *impl_from_IMFMediaShutdownNotify(IMFMediaShutdownNotify *iface)
{
    return CONTAINING_RECORD(iface, struct media_source, IMFMediaShutdownNotify_iface);
}

static inline struct media_source *impl_from_async_commands_callback_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct media_source, async_commands_callback);
}

static inline struct source_async_command *impl_from_async_command_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct source_async_command, IUnknown_iface);
}

static HRESULT media_stream_send_eos(struct media_source *source, struct media_stream *stream);

static BOOL media_source_stream_queues_full(struct media_source *source)
{
    UINT i;
    BOOL primed = TRUE;
    BOOL any_full = FALSE;

    for (i = 0; i < source->stream_count; ++i)
    {
        struct media_stream *stream = source->streams[i];
        BOOL full;
        BOOL empty;

        if (!stream->active || stream->eos)
            continue;

        EnterCriticalSection(&stream->queue_cs);
        empty = !stream->queued_packets;
        full = stream->queued_packets >= MEDIA_SOURCE_MAX_QUEUED_PACKETS
                || stream->queued_bytes >= MEDIA_SOURCE_MAX_QUEUED_BYTES;
        LeaveCriticalSection(&stream->queue_cs);

        if (empty)
            primed = FALSE;

        if (full)
            any_full = TRUE;
    }

    return primed && any_full;
}

/* ========================================================================
 * source_async_command COM boilerplate
 * ======================================================================== */

static HRESULT WINAPI source_async_command_QueryInterface(IUnknown *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI source_async_command_AddRef(IUnknown *iface)
{
    struct source_async_command *command = impl_from_async_command_IUnknown(iface);
    return InterlockedIncrement(&command->refcount);
}

static ULONG WINAPI source_async_command_Release(IUnknown *iface)
{
    struct source_async_command *command = impl_from_async_command_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&command->refcount);

    if (!refcount)
    {
        if (command->op == SOURCE_ASYNC_START)
        {
            IMFPresentationDescriptor_Release(command->u.start.descriptor);
            PropVariantClear(&command->u.start.position);
        }
        else if (command->op == SOURCE_ASYNC_REQUEST_SAMPLE)
        {
            if (command->u.request_sample.token)
                IUnknown_Release(command->u.request_sample.token);
        }
        free(command);
    }

    return refcount;
}

static const IUnknownVtbl source_async_command_vtbl =
{
    source_async_command_QueryInterface,
    source_async_command_AddRef,
    source_async_command_Release,
};

static HRESULT source_create_async_op(enum source_async_op op, IUnknown **out)
{
    struct source_async_command *command;

    if (!(command = calloc(1, sizeof(*command))))
        return E_OUTOFMEMORY;

    command->IUnknown_iface.lpVtbl = &source_async_command_vtbl;
    command->refcount = 1;
    command->op = op;

    *out = &command->IUnknown_iface;
    return S_OK;
}

/* ========================================================================
 * async_commands_callback boilerplate
 * ======================================================================== */

static HRESULT WINAPI callback_QueryInterface(IMFAsyncCallback *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IMFAsyncCallback) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFAsyncCallback_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI callback_GetParameters(IMFAsyncCallback *iface,
        DWORD *flags, DWORD *queue)
{
    return E_NOTIMPL;
}

static ULONG WINAPI source_async_commands_callback_AddRef(IMFAsyncCallback *iface)
{
    struct media_source *source = impl_from_async_commands_callback_IMFAsyncCallback(iface);
    return IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
}

static ULONG WINAPI source_async_commands_callback_Release(IMFAsyncCallback *iface)
{
    struct media_source *source = impl_from_async_commands_callback_IMFAsyncCallback(iface);
    return IMFMediaSource_Release(&source->IMFMediaSource_iface);
}

/* ========================================================================
 * Token queue helpers
 * ======================================================================== */

static BOOL enqueue_token(struct media_stream *stream, IUnknown *token)
{
    if (stream->token_queue_count == stream->token_queue_cap)
    {
        IUnknown **buf;
        stream->token_queue_cap = stream->token_queue_cap * 2 + 1;
        buf = realloc(stream->token_queue, stream->token_queue_cap * sizeof(*buf));
        if (buf)
            stream->token_queue = buf;
        else
        {
            stream->token_queue_cap = stream->token_queue_count;
            return FALSE;
        }
    }
    stream->token_queue[stream->token_queue_count++] = token;
    return TRUE;
}

static void flush_token_queue(struct media_stream *stream, BOOL send)
{
    struct media_source *source = impl_from_IMFMediaSource(stream->media_source);
    LONG i;

    for (i = 0; i < stream->token_queue_count; i++)
    {
        if (send)
        {
            IUnknown *op;
            HRESULT hr;

            if (SUCCEEDED(hr = source_create_async_op(SOURCE_ASYNC_REQUEST_SAMPLE, &op)))
            {
                struct source_async_command *command = impl_from_async_command_IUnknown(op);
                command->u.request_sample.stream = stream;
                command->u.request_sample.token = stream->token_queue[i];

                /* SOURCE_ASYNC_REQUEST_SAMPLE can block in wait_on_sample().
                 * Do not serialize those waits on the private command queue used
                 * for source start/stop/pause, or one stream's requests can starve
                 * another stream during clip transitions. */
                hr = MFPutWorkItem(MFASYNC_CALLBACK_QUEUE_STANDARD,
                        &source->async_commands_callback, op);
                IUnknown_Release(op);
            }
            if (FAILED(hr))
                WARN("Could not enqueue sample request, hr %#lx\n", hr);
        }
        else if (stream->token_queue[i])
            IUnknown_Release(stream->token_queue[i]);
    }
    free(stream->token_queue);
    stream->token_queue = NULL;
    stream->token_queue_count = 0;
    stream->token_queue_cap = 0;
}

/* ========================================================================
 * winedmo stream callbacks
 * ======================================================================== */

struct winedmo_bytestream_read_cb
{
    IMFAsyncCallback IMFAsyncCallback_iface;
    LONG refcount;
    HANDLE event;
    IMFAsyncResult *result;
};

static struct winedmo_bytestream_read_cb *winedmo_bytestream_read_cb_from_iface(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct winedmo_bytestream_read_cb, IMFAsyncCallback_iface);
}

static HRESULT WINAPI winedmo_bytestream_read_cb_QueryInterface(IMFAsyncCallback *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFAsyncCallback) || IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFAsyncCallback_AddRef(iface);
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI winedmo_bytestream_read_cb_AddRef(IMFAsyncCallback *iface)
{
    return InterlockedIncrement(&winedmo_bytestream_read_cb_from_iface(iface)->refcount);
}

static ULONG WINAPI winedmo_bytestream_read_cb_Release(IMFAsyncCallback *iface)
{
    struct winedmo_bytestream_read_cb *cb = winedmo_bytestream_read_cb_from_iface(iface);
    ULONG ref = InterlockedDecrement(&cb->refcount);
    if (!ref) { CloseHandle(cb->event); free(cb); }
    return ref;
}

static HRESULT WINAPI winedmo_bytestream_read_cb_GetParameters(IMFAsyncCallback *iface, DWORD *flags, DWORD *queue)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI winedmo_bytestream_read_cb_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct winedmo_bytestream_read_cb *cb = winedmo_bytestream_read_cb_from_iface(iface);
    cb->result = result;
    IMFAsyncResult_AddRef(result);
    SetEvent(cb->event);
    return S_OK;
}

static const IMFAsyncCallbackVtbl winedmo_bytestream_read_cb_vtbl =
{
    winedmo_bytestream_read_cb_QueryInterface,
    winedmo_bytestream_read_cb_AddRef,
    winedmo_bytestream_read_cb_Release,
    winedmo_bytestream_read_cb_GetParameters,
    winedmo_bytestream_read_cb_Invoke,
};

/* Some IMFByteStream implementations only support asynchronous reads and signal
 * this by returning pcbRead == 0xffffffff from the synchronous Read method.
 * Fall back to BeginRead/EndRead in that case. */
static HRESULT winedmo_bytestream_read(IMFByteStream *stream, BYTE *buffer, ULONG size, ULONG *read_size)
{
    struct winedmo_bytestream_read_cb *cb;
    IMFAsyncResult *result;
    HRESULT hr;

    if (FAILED(hr = IMFByteStream_Read(stream, buffer, size, read_size)))
        return hr;
    if (*read_size != 0xffffffff)
        return S_OK;

    if (!(cb = calloc(1, sizeof(*cb))))
        return E_OUTOFMEMORY;
    cb->IMFAsyncCallback_iface.lpVtbl = &winedmo_bytestream_read_cb_vtbl;
    cb->refcount = 1;
    cb->event = CreateEventA(NULL, FALSE, FALSE, NULL);

    if (FAILED(hr = IMFByteStream_BeginRead(stream, buffer, size, &cb->IMFAsyncCallback_iface, NULL)))
    {
        WARN("BeginRead failed, hr %#lx.\n", hr);
        IMFAsyncCallback_Release(&cb->IMFAsyncCallback_iface);
        return hr;
    }
    if (WaitForSingleObject(cb->event, 5000) != WAIT_OBJECT_0)
    {
        ERR("Timed out waiting for BeginRead.\n");
        IMFAsyncCallback_Release(&cb->IMFAsyncCallback_iface);
        return E_FAIL;
    }
    result = cb->result;
    cb->result = NULL;
    IMFAsyncCallback_Release(&cb->IMFAsyncCallback_iface);
    hr = IMFByteStream_EndRead(stream, result, read_size);
    IMFAsyncResult_Release(result);
    return hr;
}

static NTSTATUS CDECL source_stream_seek_cb(struct winedmo_stream *stream, UINT64 *pos)
{
    struct media_source *source = CONTAINING_RECORD(stream, struct media_source, demuxer_stream.stream);
    source->demuxer_stream.position = *pos;
    return STATUS_SUCCESS;
}

static NTSTATUS CDECL source_stream_read_cb(struct winedmo_stream *stream, BYTE *buffer, ULONG *size)
{
    struct media_source *source = CONTAINING_RECORD(stream, struct media_source, demuxer_stream.stream);
    UINT64 pos = source->demuxer_stream.position;
    ULONG ret_size = 0;
    bool known_size = source->file_size != (UINT64)-1;
    HRESULT hr;

    if (source->read_flushing || source->demux_thread_shutdown)
    {
        *size = 0;
        return STATUS_SUCCESS;
    }

    if (known_size)
    {
        if (pos >= source->file_size)
            *size = 0;
        else if (*size > source->file_size - pos)
            *size = (ULONG)(source->file_size - pos);
    }

    if (!*size)
        return STATUS_SUCCESS;

    if (!known_size || SUCCEEDED(hr = IMFByteStream_SetCurrentPosition(source->byte_stream, (QWORD)pos)))
        hr = winedmo_bytestream_read(source->byte_stream, buffer, *size, &ret_size);

    if (FAILED(hr))
    {
        /* Some game bytestreams fail BeginRead when the last partial chunk
         * would reach exactly EOF (remaining bytes < buffer capacity).
         * Only treat it as EOF when the read was genuinely at the file end;
         * failures in the middle of the file are real I/O errors. */
        if (known_size && pos + *size >= source->file_size)
        {
            WARN("Read at %I64u size %lu (at EOF boundary) failed, hr %#lx; treating as EOF.\n",
                 pos, *size, hr);
            *size = 0;
            return STATUS_SUCCESS;
        }
        return STATUS_UNSUCCESSFUL;
    }

    *size = ret_size;
    source->demuxer_stream.position += ret_size;
    return STATUS_SUCCESS;
}

/* ========================================================================
 * stream_descriptor_create — builds one MF compressed-format media type
 * ======================================================================== */

static HRESULT stream_descriptor_create(UINT32 id, const GUID *major,
        const union winedmo_format *fmt, UINT32 fmt_size, IMFStreamDescriptor **out)
{
    IMFStreamDescriptor *descriptor;
    IMFMediaTypeHandler *handler;
    IMFMediaType *type = NULL;
    HRESULT hr;

    if (IsEqualGUID(major, &MFMediaType_Audio))
    {
        if (FAILED(hr = MFCreateMediaType(&type)))
            return hr;
        if (FAILED(hr = MFInitMediaTypeFromWaveFormatEx(type, &fmt->audio,
                min(fmt_size, sizeof(WAVEFORMATEX) + fmt->audio.cbSize))))
        {
            IMFMediaType_Release(type);
            return hr;
        }
    }
    else if (IsEqualGUID(major, &MFMediaType_Video))
    {
        IMFVideoMediaType *vtype;
        if (FAILED(hr = MFCreateVideoMediaType(&fmt->video, &vtype)))
            return hr;
        type = (IMFMediaType *)vtype;
    }
    else
        return MF_E_INVALIDMEDIATYPE;

    hr = MFCreateStreamDescriptor(id, 1, &type, &descriptor);
    if (SUCCEEDED(hr))
    {
        hr = IMFStreamDescriptor_GetMediaTypeHandler(descriptor, &handler);
        if (SUCCEEDED(hr))
        {
            IMFMediaTypeHandler_SetCurrentMediaType(handler, type);
            IMFMediaTypeHandler_Release(handler);
        }
        if (FAILED(hr))
        {
            IMFStreamDescriptor_Release(descriptor);
            descriptor = NULL;
        }
    }
    IMFMediaType_Release(type);
    *out = descriptor;
    return hr;
}

/* ========================================================================
 * media_stream_start / media_source_start
 * ======================================================================== */

static HRESULT media_stream_start(struct media_stream *stream, BOOL active, BOOL seeking,
        const PROPVARIANT *position)
{
    struct media_source *source = impl_from_IMFMediaSource(stream->media_source);
    HRESULT hr;

    TRACE("source %p, stream %p\n", source, stream);

    if (FAILED(hr = IMFMediaEventQueue_QueueEventParamUnk(source->event_queue,
            active ? MEUpdatedStream : MENewStream, &GUID_NULL, S_OK,
            (IUnknown *)&stream->IMFMediaStream_iface)))
        WARN("Failed to send source stream event, hr %#lx\n", hr);

    return IMFMediaEventQueue_QueueEventParamVar(stream->event_queue,
            seeking ? MEStreamSeeked : MEStreamStarted, &GUID_NULL, S_OK, position);
}

static HRESULT media_source_start(struct media_source *source, IMFPresentationDescriptor *descriptor,
        GUID *format, PROPVARIANT *position)
{
    BOOL starting = source->state == SOURCE_STOPPED, seek_message = !starting && position->vt != VT_EMPTY;
    IMFStreamDescriptor **descriptors;
    DWORD i, count;
    HRESULT hr;

    TRACE("source %p, descriptor %p, format %s, position %s\n", source, descriptor,
            debugstr_guid(format), wine_dbgstr_variant((VARIANT *)position));

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    /* seek to beginning on stop->play */
    if (source->state == SOURCE_STOPPED && position->vt == VT_EMPTY)
    {
        position->vt = VT_I8;
        position->hVal.QuadPart = 0;
    }

    if (!(descriptors = calloc(source->stream_count, sizeof(*descriptors))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = IMFPresentationDescriptor_GetStreamDescriptorCount(descriptor, &count)))
        WARN("Failed to get presentation descriptor stream count, hr %#lx\n", hr);

    for (i = 0; i < count; i++)
    {
        IMFStreamDescriptor *stream_descriptor;
        BOOL selected;
        DWORD id;

        if (FAILED(hr = IMFPresentationDescriptor_GetStreamDescriptorByIndex(descriptor, i,
                &selected, &stream_descriptor)))
            WARN("Failed to get presentation stream descriptor, hr %#lx\n", hr);
        else
        {
            if (FAILED(hr = IMFStreamDescriptor_GetStreamIdentifier(stream_descriptor, &id)))
                WARN("Failed to get stream descriptor id, hr %#lx\n", hr);
            else if (id >= source->stream_count)
                WARN("Invalid stream descriptor id %lu, hr %#lx\n", id, hr);
            else if (selected)
                IMFStreamDescriptor_AddRef((descriptors[id] = stream_descriptor));

            IMFStreamDescriptor_Release(stream_descriptor);
        }
    }

    if (position->vt == VT_I8)
    {
        /* Flush: signal demux thread to stop producing, seek, then re-enable. */
        source->read_flushing = TRUE;
        source->flushing = TRUE;
        source->demux_eof = FALSE;
        source->eop_queued = false;

        for (i = 0; i < source->stream_count; i++)
        {
            struct media_stream *stream = source->streams[i];
            EnterCriticalSection(&stream->queue_cs);
            media_packet_queue_flush(stream);
            stream->eos = FALSE;
            LeaveCriticalSection(&stream->queue_cs);
            WakeConditionVariable(&stream->queue_cv);
        }

        WARN("MF media source seek to %s.\n", debugstr_time(position->hVal.QuadPart));
        winedmo_demuxer_seek(source->demuxer, position->hVal.QuadPart);

        source->flushing = FALSE;
        source->read_flushing = FALSE;
    }

    for (i = 0; i < source->stream_count; i++)
    {
        struct media_stream *stream = source->streams[i];
        BOOL was_active = !starting && stream->active;

        if (position->vt != VT_EMPTY)
            stream->eos = FALSE;

        if (!(stream->active = !!descriptors[i]))
        {
            /* stream deselected -- nothing special needed, demux thread skips inactive streams */
        }
        else
        {
            if (FAILED(hr = media_stream_start(stream, was_active, seek_message, position)))
                WARN("Failed to start media stream, hr %#lx\n", hr);
            IMFStreamDescriptor_Release(descriptors[i]);
        }
    }

    free(descriptors);

    if (position->vt != VT_EMPTY)
    {
        source->demux_eof = FALSE;
        source->eop_queued = false;
    }

    source->state = SOURCE_RUNNING;

    for (i = 0; i < source->stream_count; i++)
        flush_token_queue(source->streams[i], position->vt == VT_EMPTY);

    return IMFMediaEventQueue_QueueEventParamVar(source->event_queue,
            seek_message ? MESourceSeeked : MESourceStarted, &GUID_NULL, S_OK, position);
}

/* ========================================================================
 * media_source_pause / media_source_stop
 * ======================================================================== */

static HRESULT media_source_pause(struct media_source *source)
{
    unsigned int i;
    HRESULT hr;

    TRACE("source %p\n", source);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    for (i = 0; i < source->stream_count; i++)
    {
        struct media_stream *stream = source->streams[i];
        if (stream->active && FAILED(hr = IMFMediaEventQueue_QueueEventParamVar(stream->event_queue,
                    MEStreamPaused, &GUID_NULL, S_OK, NULL)))
            WARN("Failed to queue MEStreamPaused event, hr %#lx\n", hr);
    }

    source->state = SOURCE_PAUSED;
    return IMFMediaEventQueue_QueueEventParamVar(source->event_queue, MESourcePaused, &GUID_NULL, S_OK, NULL);
}

static HRESULT media_source_stop(struct media_source *source)
{
    unsigned int i;
    HRESULT hr;

    TRACE("source %p\n", source);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    for (i = 0; i < source->stream_count; i++)
    {
        struct media_stream *stream = source->streams[i];
        if (stream->active && FAILED(hr = IMFMediaEventQueue_QueueEventParamVar(stream->event_queue,
                    MEStreamStopped, &GUID_NULL, S_OK, NULL)))
            WARN("Failed to queue MEStreamStopped event, hr %#lx\n", hr);
    }

    source->state = SOURCE_STOPPED;
    for (i = 0; i < source->stream_count; i++)
        flush_token_queue(source->streams[i], FALSE);

    return IMFMediaEventQueue_QueueEventParamVar(source->event_queue, MESourceStopped, &GUID_NULL, S_OK, NULL);
}

/* ========================================================================
 * media_stream_send_eos / media_stream_send_sample / wait_on_sample
 * ======================================================================== */

static void media_source_queue_end_of_presentation(struct media_source *source)
{
    PROPVARIANT empty = {.vt = VT_EMPTY};
    HRESULT hr;

    if (source->eop_queued)
        return;
    source->eop_queued = true;

    if (FAILED(hr = IMFMediaEventQueue_QueueEventParamVar(source->event_queue,
            MEEndOfPresentation, &GUID_NULL, S_OK, &empty)))
        WARN("Failed to queue MEEndOfPresentation event, hr %#lx\n", hr);
}

static HRESULT media_stream_send_eos(struct media_source *source, struct media_stream *stream)
{
    PROPVARIANT empty = {.vt = VT_EMPTY};
    HRESULT hr;
    UINT i;

    TRACE("source %p, stream %p\n", source, stream);

    stream->eos = TRUE;
    if (FAILED(hr = IMFMediaEventQueue_QueueEventParamVar(stream->event_queue,
            MEEndOfStream, &GUID_NULL, S_OK, &empty)))
        WARN("Failed to queue MEEndOfStream event, hr %#lx\n", hr);

    for (i = 0; i < source->stream_count; i++)
    {
        struct media_stream *s = source->streams[i];
        if (s->active && !s->eos)
            return S_OK;
    }

    media_source_queue_end_of_presentation(source);
    return S_OK;
}

static HRESULT media_stream_send_sample(struct media_stream *stream, struct media_packet *pkt,
        IUnknown *token)
{
    IMFMediaBuffer *buffer;
    IMFSample *sample = NULL;
    BYTE *data;
    HRESULT hr;

    if (FAILED(hr = MFCreateMemoryBuffer(pkt->size, &buffer)))
        return hr;
    if (FAILED(hr = IMFMediaBuffer_SetCurrentLength(buffer, pkt->size)))
        goto out;
    if (FAILED(hr = IMFMediaBuffer_Lock(buffer, &data, NULL, NULL)))
        goto out;
    memcpy(data, pkt->data, pkt->size);
    IMFMediaBuffer_Unlock(buffer);

    if (FAILED(hr = MFCreateSample(&sample)))
        goto out;
    if (FAILED(hr = IMFSample_AddBuffer(sample, buffer)))
        goto out;
    if (stream->is_video && pkt->dts != INT64_MIN)
        IMFSample_SetUINT64(sample, &MFSampleExtension_DecodeTimestamp, pkt->dts);
    if (pkt->pts != INT64_MIN)
        IMFSample_SetSampleTime(sample, pkt->pts);
    if (pkt->duration != INT64_MIN)
        IMFSample_SetSampleDuration(sample, pkt->duration);
    if (token)
        IMFSample_SetUnknown(sample, &MFSampleExtension_Token, token);

    hr = IMFMediaEventQueue_QueueEventParamUnk(stream->event_queue, MEMediaSample,
            &GUID_NULL, S_OK, (IUnknown *)sample);

out:
    if (sample)
        IMFSample_Release(sample);
    IMFMediaBuffer_Release(buffer);
    return hr;
}

static struct media_packet *stream_dequeue_packet(struct media_stream *stream)
{
    struct media_source *source = impl_from_IMFMediaSource(stream->media_source);
    struct media_packet *pkt = NULL;

    EnterCriticalSection(&stream->queue_cs);
    while (list_empty(&stream->packet_queue) && !stream->eos && !source->demux_thread_shutdown)
        SleepConditionVariableCS(&stream->queue_cv, &stream->queue_cs, INFINITE);
    if (!list_empty(&stream->packet_queue))
    {
        pkt = LIST_ENTRY(list_head(&stream->packet_queue), struct media_packet, entry);
        list_remove(&pkt->entry);
        --stream->queued_packets;
        stream->queued_bytes -= pkt->size;
    }
    LeaveCriticalSection(&stream->queue_cs);
    return pkt;
}

static HRESULT wait_on_sample(struct media_stream *stream, IUnknown *token)
{
    struct media_source *source = impl_from_IMFMediaSource(stream->media_source);
    struct media_packet *pkt;
    HRESULT hr;

    TRACE("%p, %p\n", stream, token);

    LeaveCriticalSection(&source->cs);
    pkt = stream_dequeue_packet(stream);
    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
        return S_OK;

    if (!pkt)
        return media_stream_send_eos(source, stream);

    hr = media_stream_send_sample(stream, pkt, token);
    media_packet_free(pkt);
    return hr;
}

/* ========================================================================
 * Demux thread
 * ======================================================================== */

static DWORD CALLBACK source_demux_thread(void *arg)
{
    struct media_source *source = arg;
    ULONG buffer_size = 0x40000;

    TRACE("Demux thread starting for source %p.\n", source);

    while (!source->demux_thread_shutdown)
    {
        struct simple_buffer *sbuf;
        DMO_OUTPUT_DATA_BUFFER output = {0};
        UINT stream_idx = 0, needed_size;
        NTSTATUS status;
        struct media_stream *stream;
        struct media_packet *pkt;
        BYTE *src_data;
        DWORD data_len;

        if (source->flushing)
        {
            Sleep(1);
            continue;
        }

        if (source->demux_eof)
        {
            Sleep(1);
            continue;
        }

        if (media_source_stream_queues_full(source))
        {
            Sleep(1);
            continue;
        }

    retry:
        if (!(sbuf = simple_buffer_create(buffer_size)))
            break;

        output.pBuffer = &sbuf->IMediaBuffer_iface;
        output.dwStatus = 0;
        stream_idx = 0;
        needed_size = buffer_size;

        status = winedmo_demuxer_read(source->demuxer, &stream_idx, &output, &needed_size);

        if (status == STATUS_BUFFER_TOO_SMALL)
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            buffer_size = needed_size;
            goto retry;
        }

        if (status == STATUS_END_OF_FILE)
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);

            if (source->flushing || source->demux_thread_shutdown)
                continue;

            /* Real EOF: signal EOS to all active streams. */
            TRACE("Demuxer reached end of file.\n");
            source->demux_eof = TRUE;
            for (UINT i = 0; i < source->stream_count; ++i)
            {
                struct media_stream *s = source->streams[i];
                EnterCriticalSection(&s->queue_cs);
                s->eos = TRUE;
                LeaveCriticalSection(&s->queue_cs);
                WakeConditionVariable(&s->queue_cv);
            }
            continue;
        }

        if (status)
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            Sleep(1);
            continue;
        }

        if (stream_idx >= source->stream_count)
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            continue;
        }

        stream = source->streams[stream_idx];

        IMediaBuffer_SetLength(&sbuf->IMediaBuffer_iface, needed_size);
        data_len = needed_size;
        IMediaBuffer_GetBufferAndLength(&sbuf->IMediaBuffer_iface, &src_data, NULL);

        if (!data_len || !stream->active)
        {
            IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
            continue;
        }

        if ((pkt = malloc(sizeof(*pkt))) && (pkt->data = malloc(data_len)))
        {
            memcpy(pkt->data, src_data, data_len);
            pkt->size = data_len;
            pkt->dts      = (output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIME)
                            ? output.rtTimestamp : INT64_MIN;
            pkt->pts      = (output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIME)
                            ? output.rtTimestamp : INT64_MIN;
            pkt->duration = (output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH)
                            ? output.rtTimelength : INT64_MIN;
            list_init(&pkt->entry);

            EnterCriticalSection(&stream->queue_cs);
            list_add_tail(&stream->packet_queue, &pkt->entry);
            ++stream->queued_packets;
            stream->queued_bytes += pkt->size;
            LeaveCriticalSection(&stream->queue_cs);
            WakeConditionVariable(&stream->queue_cv);
        }
        else
        {
            if (pkt)
            {
                free(pkt->data);
                free(pkt);
            }
        }

        IMediaBuffer_Release(&sbuf->IMediaBuffer_iface);
    }

    TRACE("Demux thread stopping for source %p.\n", source);
    return 0;
}

/* ========================================================================
 * source_async_commands_Invoke
 * ======================================================================== */

static HRESULT WINAPI source_async_commands_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct media_source *source = impl_from_async_commands_callback_IMFAsyncCallback(iface);
    struct source_async_command *command;
    IUnknown *state;
    HRESULT hr;

    if (FAILED(hr = IMFAsyncResult_GetState(result, &state)))
        return hr;

    EnterCriticalSection(&source->cs);

    command = impl_from_async_command_IUnknown(state);
    switch (command->op)
    {
        case SOURCE_ASYNC_START:
        {
            IMFPresentationDescriptor *descriptor = command->u.start.descriptor;
            GUID format = command->u.start.format;
            PROPVARIANT position = command->u.start.position;

            if (FAILED(hr = media_source_start(source, descriptor, &format, &position)))
                WARN("Failed to start source %p, hr %#lx\n", source, hr);
            break;
        }
        case SOURCE_ASYNC_PAUSE:
            if (FAILED(hr = media_source_pause(source)))
                WARN("Failed to pause source %p, hr %#lx\n", source, hr);
            break;
        case SOURCE_ASYNC_STOP:
            if (FAILED(hr = media_source_stop(source)))
                WARN("Failed to stop source %p, hr %#lx\n", source, hr);
            break;
        case SOURCE_ASYNC_REQUEST_SAMPLE:
            if (source->state == SOURCE_PAUSED)
                enqueue_token(command->u.request_sample.stream, command->u.request_sample.token);
            else if (source->state == SOURCE_RUNNING)
            {
                if (FAILED(hr = wait_on_sample(command->u.request_sample.stream,
                        command->u.request_sample.token)))
                    WARN("Failed to request sample, hr %#lx\n", hr);
            }
            break;
    }

    LeaveCriticalSection(&source->cs);

    IUnknown_Release(state);

    return S_OK;
}

static const IMFAsyncCallbackVtbl source_async_commands_callback_vtbl =
{
    callback_QueryInterface,
    source_async_commands_callback_AddRef,
    source_async_commands_callback_Release,
    callback_GetParameters,
    source_async_commands_Invoke,
};

/* ========================================================================
 * IMFMediaStream implementation
 * ======================================================================== */

static HRESULT WINAPI media_stream_QueryInterface(IMFMediaStream *iface, REFIID riid, void **out)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IMFMediaStream) ||
        IsEqualIID(riid, &IID_IMFMediaEventGenerator) ||
        IsEqualIID(riid, &IID_IUnknown))
    {
        *out = &stream->IMFMediaStream_iface;
    }
    else
    {
        FIXME("(%s, %p)\n", debugstr_guid(riid), out);
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*out);
    return S_OK;
}

static ULONG WINAPI media_stream_AddRef(IMFMediaStream *iface)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);
    ULONG ref = InterlockedIncrement(&stream->ref);

    TRACE("%p, refcount %lu.\n", iface, ref);

    return ref;
}

static ULONG WINAPI media_stream_Release(IMFMediaStream *iface)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);
    ULONG ref = InterlockedDecrement(&stream->ref);

    TRACE("%p, refcount %lu.\n", iface, ref);

    if (!ref)
    {
        IMFMediaSource_Release(stream->media_source);
        IMFStreamDescriptor_Release(stream->descriptor);
        IMFMediaEventQueue_Release(stream->event_queue);
        flush_token_queue(stream, FALSE);
        media_packet_queue_flush(stream);
        stream->queue_cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&stream->queue_cs);
        free(stream);
    }

    return ref;
}

static HRESULT WINAPI media_stream_GetEvent(IMFMediaStream *iface, DWORD flags, IMFMediaEvent **event)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    TRACE("%p, %#lx, %p.\n", iface, flags, event);

    return IMFMediaEventQueue_GetEvent(stream->event_queue, flags, event);
}

static HRESULT WINAPI media_stream_BeginGetEvent(IMFMediaStream *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    TRACE("%p, %p, %p.\n", iface, callback, state);

    return IMFMediaEventQueue_BeginGetEvent(stream->event_queue, callback, state);
}

static HRESULT WINAPI media_stream_EndGetEvent(IMFMediaStream *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    TRACE("%p, %p, %p.\n", stream, result, event);

    return IMFMediaEventQueue_EndGetEvent(stream->event_queue, result, event);
}

static HRESULT WINAPI media_stream_QueueEvent(IMFMediaStream *iface, MediaEventType event_type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    TRACE("%p, %lu, %s, %#lx, %p.\n", iface, event_type, debugstr_guid(ext_type), hr, value);

    return IMFMediaEventQueue_QueueEventParamVar(stream->event_queue, event_type, ext_type, hr, value);
}

static HRESULT WINAPI media_stream_GetMediaSource(IMFMediaStream *iface, IMFMediaSource **out)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);
    struct media_source *source = impl_from_IMFMediaSource(stream->media_source);
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, out);

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else
    {
        IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
        *out = &source->IMFMediaSource_iface;
    }

    LeaveCriticalSection(&source->cs);

    return hr;
}

static HRESULT WINAPI media_stream_GetStreamDescriptor(IMFMediaStream* iface, IMFStreamDescriptor **descriptor)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);
    struct media_source *source = impl_from_IMFMediaSource(stream->media_source);
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, descriptor);

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else
    {
        IMFStreamDescriptor_AddRef(stream->descriptor);
        *descriptor = stream->descriptor;
    }

    LeaveCriticalSection(&source->cs);

    return hr;
}

static HRESULT WINAPI media_stream_RequestSample(IMFMediaStream *iface, IUnknown *token)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);
    struct media_source *source = impl_from_IMFMediaSource(stream->media_source);
    IUnknown *op;
    HRESULT hr;

    TRACE("%p, %p.\n", iface, token);

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else if (!stream->active)
        hr = MF_E_MEDIA_SOURCE_WRONGSTATE;
    else
    {
        if (SUCCEEDED(hr = source_create_async_op(SOURCE_ASYNC_REQUEST_SAMPLE, &op)))
        {
            struct source_async_command *command = impl_from_async_command_IUnknown(op);
            command->u.request_sample.stream = stream;
            if (token)
                IUnknown_AddRef(token);
            command->u.request_sample.token = token;

            /* SOURCE_ASYNC_REQUEST_SAMPLE may block waiting for demuxed packets.
             * Use the standard MF queue so per-stream sample waits are not
             * serialized behind each other on the source's private command queue. */
            hr = MFPutWorkItem(MFASYNC_CALLBACK_QUEUE_STANDARD,
                    &source->async_commands_callback, op);
            IUnknown_Release(op);
        }
    }

    LeaveCriticalSection(&source->cs);

    return hr;
}

static const IMFMediaStreamVtbl media_stream_vtbl =
{
    media_stream_QueryInterface,
    media_stream_AddRef,
    media_stream_Release,
    media_stream_GetEvent,
    media_stream_BeginGetEvent,
    media_stream_EndGetEvent,
    media_stream_QueueEvent,
    media_stream_GetMediaSource,
    media_stream_GetStreamDescriptor,
    media_stream_RequestSample
};

static HRESULT media_stream_create(IMFMediaSource *source, IMFStreamDescriptor *descriptor,
        UINT stream_index, BOOL is_video, struct media_stream **out)
{
    struct media_stream *object;
    HRESULT hr;

    TRACE("source %p, descriptor %p, stream_index %u.\n", source, descriptor, stream_index);

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IMFMediaStream_iface.lpVtbl = &media_stream_vtbl;
    object->ref = 1;

    if (FAILED(hr = MFCreateEventQueue(&object->event_queue)))
    {
        free(object);
        return hr;
    }

    IMFMediaSource_AddRef(source);
    object->media_source = source;
    IMFStreamDescriptor_AddRef(descriptor);
    object->descriptor = descriptor;

    object->active = TRUE;
    object->eos = FALSE;
    object->stream_index = stream_index;
    object->is_video = is_video;

    list_init(&object->packet_queue);
    InitializeCriticalSectionEx(&object->queue_cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    object->queue_cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": stream.queue_cs");
    InitializeConditionVariable(&object->queue_cv);

    TRACE("Created stream object %p.\n", object);

    *out = object;
    return S_OK;
}

/* ========================================================================
 * IMFGetService
 * ======================================================================== */

static HRESULT WINAPI media_source_get_service_QueryInterface(IMFGetService *iface, REFIID riid, void **obj)
{
    struct media_source *source = impl_from_IMFGetService(iface);
    return IMFMediaSource_QueryInterface(&source->IMFMediaSource_iface, riid, obj);
}

static ULONG WINAPI media_source_get_service_AddRef(IMFGetService *iface)
{
    struct media_source *source = impl_from_IMFGetService(iface);
    return IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
}

static ULONG WINAPI media_source_get_service_Release(IMFGetService *iface)
{
    struct media_source *source = impl_from_IMFGetService(iface);
    return IMFMediaSource_Release(&source->IMFMediaSource_iface);
}

static HRESULT WINAPI media_source_get_service_GetService(IMFGetService *iface, REFGUID service, REFIID riid, void **obj)
{
    struct media_source *source = impl_from_IMFGetService(iface);

    TRACE("%p, %s, %s, %p.\n", iface, debugstr_guid(service), debugstr_guid(riid), obj);

    *obj = NULL;

    if (IsEqualGUID(service, &MF_RATE_CONTROL_SERVICE))
    {
        if (IsEqualIID(riid, &IID_IMFRateSupport))
        {
            *obj = &source->IMFRateSupport_iface;
        }
        else if (IsEqualIID(riid, &IID_IMFRateControl))
        {
            *obj = &source->IMFRateControl_iface;
        }
    }
    else
        FIXME("Unsupported service %s.\n", debugstr_guid(service));

    if (*obj)
        IUnknown_AddRef((IUnknown *)*obj);

    return *obj ? S_OK : E_NOINTERFACE;
}

static const IMFGetServiceVtbl media_source_get_service_vtbl =
{
    media_source_get_service_QueryInterface,
    media_source_get_service_AddRef,
    media_source_get_service_Release,
    media_source_get_service_GetService,
};

/* ========================================================================
 * IMFRateSupport
 * ======================================================================== */

static HRESULT WINAPI media_source_rate_support_QueryInterface(IMFRateSupport *iface, REFIID riid, void **obj)
{
    struct media_source *source = impl_from_IMFRateSupport(iface);
    return IMFMediaSource_QueryInterface(&source->IMFMediaSource_iface, riid, obj);
}

static ULONG WINAPI media_source_rate_support_AddRef(IMFRateSupport *iface)
{
    struct media_source *source = impl_from_IMFRateSupport(iface);
    return IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
}

static ULONG WINAPI media_source_rate_support_Release(IMFRateSupport *iface)
{
    struct media_source *source = impl_from_IMFRateSupport(iface);
    return IMFMediaSource_Release(&source->IMFMediaSource_iface);
}

static HRESULT WINAPI media_source_rate_support_GetSlowestRate(IMFRateSupport *iface, MFRATE_DIRECTION direction, BOOL thin, float *rate)
{
    TRACE("%p, %d, %d, %p.\n", iface, direction, thin, rate);

    *rate = 0.0f;

    return S_OK;
}

static HRESULT WINAPI media_source_rate_support_GetFastestRate(IMFRateSupport *iface, MFRATE_DIRECTION direction, BOOL thin, float *rate)
{
    TRACE("%p, %d, %d, %p.\n", iface, direction, thin, rate);

    *rate = direction == MFRATE_FORWARD ? 1e6f : -1e6f;

    return S_OK;
}

static HRESULT WINAPI media_source_rate_support_IsRateSupported(IMFRateSupport *iface, BOOL thin, float rate,
        float *nearest_rate)
{
    TRACE("%p, %d, %f, %p.\n", iface, thin, rate, nearest_rate);

    if (nearest_rate)
        *nearest_rate = rate;

    return rate >= -1e6f && rate <= 1e6f ? S_OK : MF_E_UNSUPPORTED_RATE;
}

static const IMFRateSupportVtbl media_source_rate_support_vtbl =
{
    media_source_rate_support_QueryInterface,
    media_source_rate_support_AddRef,
    media_source_rate_support_Release,
    media_source_rate_support_GetSlowestRate,
    media_source_rate_support_GetFastestRate,
    media_source_rate_support_IsRateSupported,
};

/* ========================================================================
 * IMFRateControl
 * ======================================================================== */

static HRESULT WINAPI media_source_rate_control_QueryInterface(IMFRateControl *iface, REFIID riid, void **obj)
{
    struct media_source *source = impl_from_IMFRateControl(iface);
    return IMFMediaSource_QueryInterface(&source->IMFMediaSource_iface, riid, obj);
}

static ULONG WINAPI media_source_rate_control_AddRef(IMFRateControl *iface)
{
    struct media_source *source = impl_from_IMFRateControl(iface);
    return IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
}

static ULONG WINAPI media_source_rate_control_Release(IMFRateControl *iface)
{
    struct media_source *source = impl_from_IMFRateControl(iface);
    return IMFMediaSource_Release(&source->IMFMediaSource_iface);
}

static HRESULT WINAPI media_source_rate_control_SetRate(IMFRateControl *iface, BOOL thin, float rate)
{
    struct media_source *source = impl_from_IMFRateControl(iface);
    HRESULT hr;

    FIXME("%p, %d, %f.\n", iface, thin, rate);

    if (rate < 0.0f)
        return MF_E_REVERSE_UNSUPPORTED;

    if (thin)
        return MF_E_THINNING_UNSUPPORTED;

    if (FAILED(hr = IMFRateSupport_IsRateSupported(&source->IMFRateSupport_iface, thin, rate, NULL)))
        return hr;

    EnterCriticalSection(&source->cs);
    source->rate = rate;
    LeaveCriticalSection(&source->cs);

    return IMFMediaEventQueue_QueueEventParamVar(source->event_queue, MESourceRateChanged, &GUID_NULL, S_OK, NULL);
}

static HRESULT WINAPI media_source_rate_control_GetRate(IMFRateControl *iface, BOOL *thin, float *rate)
{
    struct media_source *source = impl_from_IMFRateControl(iface);

    TRACE("%p, %p, %p.\n", iface, thin, rate);

    if (thin)
        *thin = FALSE;

    EnterCriticalSection(&source->cs);
    *rate = source->rate;
    LeaveCriticalSection(&source->cs);

    return S_OK;
}

static const IMFRateControlVtbl media_source_rate_control_vtbl =
{
    media_source_rate_control_QueryInterface,
    media_source_rate_control_AddRef,
    media_source_rate_control_Release,
    media_source_rate_control_SetRate,
    media_source_rate_control_GetRate,
};

/* ========================================================================
 * IMFMediaShutdownNotify
 * ======================================================================== */

static void media_source_release_shutdown_callback(struct media_source *source)
{
    if (source->shutdown_result)
        IMFAsyncResult_Release(source->shutdown_result);
    source->shutdown_result = NULL;
}

static HRESULT WINAPI media_source_shutdown_notify_QueryInterface(IMFMediaShutdownNotify *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFMediaShutdownNotify) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI media_source_shutdown_notify_AddRef(IMFMediaShutdownNotify *iface)
{
    struct media_source *source = impl_from_IMFMediaShutdownNotify(iface);
    return IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
}

static ULONG WINAPI media_source_shutdown_notify_Release(IMFMediaShutdownNotify *iface)
{
    struct media_source *source = impl_from_IMFMediaShutdownNotify(iface);
    return IMFMediaSource_Release(&source->IMFMediaSource_iface);
}

static HRESULT WINAPI media_source_shutdown_notify_set_notification_callback(IMFMediaShutdownNotify *iface,
        IMFAsyncCallback *callback, IUnknown *state)
{
    struct media_source *source = impl_from_IMFMediaShutdownNotify(iface);
    IMFAsyncResult *result = NULL;
    HRESULT hr = S_OK;

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else
    {
        if (callback && FAILED(hr = MFCreateAsyncResult(NULL, callback, state, &result)))
        {
            LeaveCriticalSection(&source->cs);
            return hr;
        }

        media_source_release_shutdown_callback(source);
        source->shutdown_result = result;
    }

    LeaveCriticalSection(&source->cs);

    return hr;
}

static const IMFMediaShutdownNotifyVtbl media_source_shutdown_notify_vtbl =
{
    media_source_shutdown_notify_QueryInterface,
    media_source_shutdown_notify_AddRef,
    media_source_shutdown_notify_Release,
    media_source_shutdown_notify_set_notification_callback,
};

/* ========================================================================
 * IMFMediaSource
 * ======================================================================== */

static HRESULT WINAPI media_source_QueryInterface(IMFMediaSource *iface, REFIID riid, void **out)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IMFMediaSource) ||
        IsEqualIID(riid, &IID_IMFMediaEventGenerator) ||
        IsEqualIID(riid, &IID_IUnknown))
    {
        *out = &source->IMFMediaSource_iface;
    }
    else if (IsEqualIID(riid, &IID_IMFGetService))
    {
        *out = &source->IMFGetService_iface;
    }
    else if (IsEqualIID(riid, &IID_IMFMediaShutdownNotify))
    {
        *out = &source->IMFMediaShutdownNotify_iface;
    }
    else
    {
        FIXME("%s, %p.\n", debugstr_guid(riid), out);
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*out);
    return S_OK;
}

static ULONG WINAPI media_source_AddRef(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);
    ULONG ref = InterlockedIncrement(&source->ref);

    TRACE("%p, refcount %lu.\n", iface, ref);

    return ref;
}

static ULONG WINAPI media_source_Release(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);
    ULONG ref = InterlockedDecrement(&source->ref);

    TRACE("%p, refcount %lu.\n", iface, ref);

    if (!ref)
    {
        media_source_release_shutdown_callback(source);
        IMFMediaSource_Shutdown(iface);
        IMFMediaEventQueue_Release(source->event_queue);
        IMFByteStream_Release(source->byte_stream);
        source->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&source->cs);
        free(source);
    }

    return ref;
}

static HRESULT WINAPI media_source_GetEvent(IMFMediaSource *iface, DWORD flags, IMFMediaEvent **event)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("%p, %#lx, %p.\n", iface, flags, event);

    return IMFMediaEventQueue_GetEvent(source->event_queue, flags, event);
}

static HRESULT WINAPI media_source_BeginGetEvent(IMFMediaSource *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("%p, %p, %p.\n", iface, callback, state);

    return IMFMediaEventQueue_BeginGetEvent(source->event_queue, callback, state);
}

static HRESULT WINAPI media_source_EndGetEvent(IMFMediaSource *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("%p, %p, %p.\n", iface, result, event);

    return IMFMediaEventQueue_EndGetEvent(source->event_queue, result, event);
}

static HRESULT WINAPI media_source_QueueEvent(IMFMediaSource *iface, MediaEventType event_type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("%p, %lu, %s, %#lx, %p.\n", iface, event_type, debugstr_guid(ext_type), hr, value);

    return IMFMediaEventQueue_QueueEventParamVar(source->event_queue, event_type, ext_type, hr, value);
}

static HRESULT WINAPI media_source_GetCharacteristics(IMFMediaSource *iface, DWORD *characteristics)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, characteristics);

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else
        *characteristics = MFMEDIASOURCE_CAN_SEEK | MFMEDIASOURCE_CAN_PAUSE;

    LeaveCriticalSection(&source->cs);

    return hr;
}

static HRESULT WINAPI media_source_CreatePresentationDescriptor(IMFMediaSource *iface, IMFPresentationDescriptor **descriptor)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);
    HRESULT hr;
    UINT i;

    TRACE("%p, %p.\n", iface, descriptor);

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else if (SUCCEEDED(hr = MFCreatePresentationDescriptor(source->stream_count, source->descriptors, descriptor)))
    {
        if (source->has_duration
                && FAILED(hr = IMFPresentationDescriptor_SetUINT64(*descriptor, &MF_PD_DURATION, source->duration)))
            WARN("Failed to set presentation descriptor MF_PD_DURATION, hr %#lx\n", hr);

        for (i = 0; i < source->stream_count; ++i)
        {
            if (FAILED(hr = IMFPresentationDescriptor_SelectStream(*descriptor, i)))
                WARN("Failed to select stream %u, hr %#lx\n", i, hr);
        }

        hr = S_OK;
    }

    LeaveCriticalSection(&source->cs);

    return hr;
}

static HRESULT WINAPI media_source_Start(IMFMediaSource *iface, IMFPresentationDescriptor *descriptor,
                                     const GUID *time_format, const PROPVARIANT *position)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);
    IUnknown *op;
    HRESULT hr;

    TRACE("%p, %p, %p, %p.\n", iface, descriptor, time_format, position);

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else if (!(IsEqualIID(time_format, &GUID_NULL)))
        hr = MF_E_UNSUPPORTED_TIME_FORMAT;
    else if (SUCCEEDED(hr = source_create_async_op(SOURCE_ASYNC_START, &op)))
    {
        struct source_async_command *command = impl_from_async_command_IUnknown(op);
        command->u.start.descriptor = descriptor;
        IMFPresentationDescriptor_AddRef(descriptor);
        command->u.start.format = *time_format;
        PropVariantCopy(&command->u.start.position, position);

        hr = MFPutWorkItem(source->async_commands_queue, &source->async_commands_callback, op);
        IUnknown_Release(op);
    }

    LeaveCriticalSection(&source->cs);

    return hr;
}

static HRESULT WINAPI media_source_Stop(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);
    IUnknown *op;
    HRESULT hr;

    TRACE("%p.\n", iface);

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else if (SUCCEEDED(hr = source_create_async_op(SOURCE_ASYNC_STOP, &op)))
    {
        hr = MFPutWorkItem(source->async_commands_queue, &source->async_commands_callback, op);
        IUnknown_Release(op);
    }

    LeaveCriticalSection(&source->cs);

    return hr;
}

static HRESULT WINAPI media_source_Pause(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);
    IUnknown *op;
    HRESULT hr;

    TRACE("%p.\n", iface);

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else if (source->state != SOURCE_RUNNING)
        hr = MF_E_INVALID_STATE_TRANSITION;
    else if (SUCCEEDED(hr = source_create_async_op(SOURCE_ASYNC_PAUSE, &op)))
    {
        hr = MFPutWorkItem(source->async_commands_queue, &source->async_commands_callback, op);
        IUnknown_Release(op);
    }

    LeaveCriticalSection(&source->cs);

    return hr;
}

static HRESULT WINAPI media_source_Shutdown(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);
    HRESULT hr;

    TRACE("%p.\n", iface);

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
    {
        LeaveCriticalSection(&source->cs);
        return MF_E_SHUTDOWN;
    }

    source->state = SOURCE_SHUTDOWN;

    /* Signal demux thread and all waiting stream queues to unblock. */
    source->demux_thread_shutdown = true;
    source->read_flushing = TRUE;

    for (unsigned int i = 0; i < source->stream_count; i++)
    {
        struct media_stream *stream = source->streams[i];
        EnterCriticalSection(&stream->queue_cs);
        stream->eos = TRUE;
        LeaveCriticalSection(&stream->queue_cs);
        WakeConditionVariable(&stream->queue_cv);
    }

    if (source->demux_thread)
    {
        WaitForSingleObject(source->demux_thread, INFINITE);
        CloseHandle(source->demux_thread);
        source->demux_thread = NULL;
    }

    winedmo_demuxer_destroy(&source->demuxer);

    IMFMediaEventQueue_Shutdown(source->event_queue);
    IMFByteStream_Close(source->byte_stream);

    while (source->stream_count--)
    {
        struct media_stream *stream = source->streams[source->stream_count];
        IMFStreamDescriptor_Release(source->descriptors[source->stream_count]);
        IMFMediaEventQueue_Shutdown(stream->event_queue);
        IMFMediaStream_Release(&stream->IMFMediaStream_iface);
    }
    free(source->descriptors);
    free(source->streams);

    if (source->shutdown_result)
    {
        if (FAILED(hr = MFPutWorkItemEx(MFASYNC_CALLBACK_QUEUE_STANDARD, source->shutdown_result)))
            WARN("Failed to put shutdown notification, hr %#lx.\n", hr);
        media_source_release_shutdown_callback(source);
    }

    LeaveCriticalSection(&source->cs);

    MFUnlockWorkQueue(source->async_commands_queue);

    return S_OK;
}

static const IMFMediaSourceVtbl IMFMediaSource_vtbl =
{
    media_source_QueryInterface,
    media_source_AddRef,
    media_source_Release,
    media_source_GetEvent,
    media_source_BeginGetEvent,
    media_source_EndGetEvent,
    media_source_QueueEvent,
    media_source_GetCharacteristics,
    media_source_CreatePresentationDescriptor,
    media_source_Start,
    media_source_Stop,
    media_source_Pause,
    media_source_Shutdown,
};

/* ========================================================================
 * media_source_init_descriptors — set language / name attributes
 * ======================================================================== */

static void media_source_init_descriptors(struct media_source *source)
{
    UINT i;

    for (i = 0; i < source->stream_count; i++)
    {
        struct media_stream *stream = source->streams[i];
        WCHAR buf[256];

        if (!winedmo_demuxer_stream_lang(source->demuxer, stream->stream_index, buf, ARRAY_SIZE(buf)))
            IMFStreamDescriptor_SetString(stream->descriptor, &MF_SD_LANGUAGE, buf);
        if (!winedmo_demuxer_stream_name(source->demuxer, stream->stream_index, buf, ARRAY_SIZE(buf)))
            IMFStreamDescriptor_SetString(stream->descriptor, &MF_SD_STREAM_NAME, buf);
    }
}

/* ========================================================================
 * media_source_create
 * ======================================================================== */

static HRESULT media_source_create(struct object_context *context, IMFMediaSource **out)
{
    struct media_source *object;
    UINT stream_count = 0;
    INT64 duration = 0;
    WCHAR mime_type[256] = {0};
    NTSTATUS status;
    unsigned int i;
    HRESULT hr;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IMFMediaSource_iface.lpVtbl = &IMFMediaSource_vtbl;
    object->IMFGetService_iface.lpVtbl = &media_source_get_service_vtbl;
    object->IMFRateSupport_iface.lpVtbl = &media_source_rate_support_vtbl;
    object->IMFRateControl_iface.lpVtbl = &media_source_rate_control_vtbl;
    object->IMFMediaShutdownNotify_iface.lpVtbl = &media_source_shutdown_notify_vtbl;
    object->async_commands_callback.lpVtbl = &source_async_commands_callback_vtbl;
    object->ref = 1;
    object->byte_stream = context->stream;
    IMFByteStream_AddRef(context->stream);
    object->file_size = context->file_size;
    object->rate = 1.0f;
    InitializeCriticalSectionEx(&object->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    object->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": cs");

    if (FAILED(hr = MFCreateEventQueue(&object->event_queue)))
        goto fail;

    if (FAILED(hr = MFAllocateWorkQueue(&object->async_commands_queue)))
        goto fail;

    /* Set up winedmo stream callbacks. */
    object->demuxer_stream.stream.p_seek = source_stream_seek_cb;
    object->demuxer_stream.stream.p_read = source_stream_read_cb;
    object->demuxer_stream.position = 0;

    object->state = SOURCE_OPENING;

    status = winedmo_demuxer_create(context->url, &object->demuxer_stream.stream,
            object->file_size, &duration, &stream_count, mime_type, &object->demuxer);
    if (status)
    {
        hr = HRESULT_FROM_NT(status);
        goto fail;
    }

    if (duration > 0)
    {
        object->duration = (UINT64)duration;
        object->has_duration = true;
    }

    if (!(object->descriptors = calloc(stream_count, sizeof(*object->descriptors)))
            || !(object->streams = calloc(stream_count, sizeof(*object->streams))))
    {
        hr = E_OUTOFMEMORY;
        goto fail;
    }

    for (i = 0; i < stream_count; i++)
    {
        union winedmo_format *fmt = NULL;
        GUID major = GUID_NULL;
        IMFStreamDescriptor *descriptor;
        struct media_stream *stream;
        UINT32 fmt_size;

        if (winedmo_demuxer_stream_type(object->demuxer, i, &major, &fmt) || !fmt)
        {
            WARN("Failed to get format for stream %u.\n", i);
            free(fmt);
            continue;
        }

        fmt_size = IsEqualGUID(&major, &MFMediaType_Audio)
                ? sizeof(WAVEFORMATEX) + fmt->audio.cbSize
                : fmt->video.dwSize;

        if (FAILED(hr = stream_descriptor_create(i, &major, fmt, fmt_size, &descriptor)))
        {
            WARN("Failed to create stream descriptor %u, hr %#lx.\n", i, hr);
            free(fmt);
            continue;
        }
        free(fmt);

        if (FAILED(hr = media_stream_create(&object->IMFMediaSource_iface, descriptor, i,
                IsEqualGUID(&major, &MFMediaType_Video), &stream)))
        {
            IMFStreamDescriptor_Release(descriptor);
            goto fail;
        }

        IMFStreamDescriptor_AddRef(descriptor);
        object->descriptors[object->stream_count] = descriptor;
        object->streams[object->stream_count] = stream;
        object->stream_count++;
        IMFStreamDescriptor_Release(descriptor);

    }

    if (!object->stream_count)
    {
        hr = E_FAIL;
        goto fail;
    }

    media_source_init_descriptors(object);
    object->state = SOURCE_STOPPED;

    /* Start the background demux thread. */
    object->demux_thread = CreateThread(NULL, 0, source_demux_thread, object, 0, NULL);
    if (!object->demux_thread)
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto fail;
    }

    *out = &object->IMFMediaSource_iface;
    TRACE("Created IMFMediaSource %p\n", *out);
    return S_OK;

fail:
    WARN("Failed to construct MFMediaSource, hr %#lx.\n", hr);

    while (object->streams && object->stream_count--)
    {
        struct media_stream *stream = object->streams[object->stream_count];
        IMFStreamDescriptor_Release(object->descriptors[object->stream_count]);
        IMFMediaStream_Release(&stream->IMFMediaStream_iface);
    }
    free(object->descriptors);
    free(object->streams);

    if (object->demuxer.handle)
    {
        object->demux_thread_shutdown = true;
        winedmo_demuxer_destroy(&object->demuxer);
    }
    if (object->demux_thread)
    {
        object->demux_thread_shutdown = true;
        /* Wake any blocked stream queues before joining. */
        WaitForSingleObject(object->demux_thread, INFINITE);
        CloseHandle(object->demux_thread);
    }
    if (object->async_commands_queue)
        MFUnlockWorkQueue(object->async_commands_queue);
    if (object->event_queue)
        IMFMediaEventQueue_Release(object->event_queue);
    IMFByteStream_Release(object->byte_stream);
    object->cs.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&object->cs);
    free(object);
    return hr;
}

/* ========================================================================
 * result_entry helpers shared by byte-stream and scheme handlers
 * ======================================================================== */

struct result_entry
{
    struct list entry;
    IMFAsyncResult *result;
    MF_OBJECT_TYPE type;
    IUnknown *object;
};

static HRESULT result_entry_create(IMFAsyncResult *result, MF_OBJECT_TYPE type,
        IUnknown *object, struct result_entry **out)
{
    struct result_entry *entry;

    if (!(entry = malloc(sizeof(*entry))))
        return E_OUTOFMEMORY;

    entry->result = result;
    IMFAsyncResult_AddRef(entry->result);
    entry->object = object;
    IUnknown_AddRef(entry->object);
    entry->type = type;

    *out = entry;
    return S_OK;
}

static void result_entry_destroy(struct result_entry *entry)
{
    IMFAsyncResult_Release(entry->result);
    IUnknown_Release(entry->object);
    free(entry);
}

/* ========================================================================
 * IMFByteStreamHandler
 * ======================================================================== */

struct stream_handler
{
    IMFByteStreamHandler IMFByteStreamHandler_iface;
    IMFAsyncCallback IMFAsyncCallback_iface;
    LONG refcount;
    struct list results;
    CRITICAL_SECTION cs;
};

static struct result_entry *handler_find_result_entry(struct stream_handler *handler, IMFAsyncResult *result)
{
    struct result_entry *entry;

    EnterCriticalSection(&handler->cs);
    LIST_FOR_EACH_ENTRY(entry, &handler->results, struct result_entry, entry)
    {
        if (result == entry->result)
        {
            list_remove(&entry->entry);
            LeaveCriticalSection(&handler->cs);
            return entry;
        }
    }
    LeaveCriticalSection(&handler->cs);

    return NULL;
}

static struct stream_handler *impl_from_IMFByteStreamHandler(IMFByteStreamHandler *iface)
{
    return CONTAINING_RECORD(iface, struct stream_handler, IMFByteStreamHandler_iface);
}

static struct stream_handler *impl_from_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct stream_handler, IMFAsyncCallback_iface);
}

static HRESULT WINAPI stream_handler_QueryInterface(IMFByteStreamHandler *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IMFByteStreamHandler) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFByteStreamHandler_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI stream_handler_AddRef(IMFByteStreamHandler *iface)
{
    struct stream_handler *handler = impl_from_IMFByteStreamHandler(iface);
    ULONG refcount = InterlockedIncrement(&handler->refcount);

    TRACE("%p, refcount %lu.\n", handler, refcount);

    return refcount;
}

static ULONG WINAPI stream_handler_Release(IMFByteStreamHandler *iface)
{
    struct stream_handler *handler = impl_from_IMFByteStreamHandler(iface);
    ULONG refcount = InterlockedDecrement(&handler->refcount);
    struct result_entry *result, *next;

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        LIST_FOR_EACH_ENTRY_SAFE(result, next, &handler->results, struct result_entry, entry)
            result_entry_destroy(result);
        DeleteCriticalSection(&handler->cs);
        free(handler);
    }

    return refcount;
}

static HRESULT WINAPI stream_handler_BeginCreateObject(IMFByteStreamHandler *iface, IMFByteStream *stream, const WCHAR *url, DWORD flags,
        IPropertyStore *props, IUnknown **cancel_cookie, IMFAsyncCallback *callback, IUnknown *state)
{
    struct stream_handler *handler = impl_from_IMFByteStreamHandler(iface);
    IMFAsyncResult *result;
    IUnknown *context;
    QWORD file_size;
    HRESULT hr;
    DWORD caps;

    TRACE("%p, %s, %#lx, %p, %p, %p, %p.\n", iface, debugstr_w(url), flags, props, cancel_cookie, callback, state);

    if (cancel_cookie)
        *cancel_cookie = NULL;

    if (!stream)
        return E_INVALIDARG;
    if (flags != MF_RESOLUTION_MEDIASOURCE)
        FIXME("Unimplemented flags %#lx\n", flags);

    if (FAILED(hr = IMFByteStream_GetCapabilities(stream, &caps)))
        return hr;
    if (!(caps & MFBYTESTREAM_IS_SEEKABLE))
    {
        FIXME("Non-seekable bytestreams not supported.\n");
        return MF_E_BYTESTREAM_NOT_SEEKABLE;
    }
    if (FAILED(hr = IMFByteStream_GetLength(stream, &file_size)))
    {
        FIXME("Failed to get byte stream length, hr %#lx.\n", hr);
        return hr;
    }

    if (FAILED(hr = MFCreateAsyncResult(NULL, callback, state, &result)))
        return hr;
    if (FAILED(hr = object_context_create(flags, stream, url, file_size, result, &context)))
    {
        IMFAsyncResult_Release(result);
        return hr;
    }

    hr = MFPutWorkItem(MFASYNC_CALLBACK_QUEUE_IO, &handler->IMFAsyncCallback_iface, context);
    IUnknown_Release(context);

    if (SUCCEEDED(hr) && cancel_cookie)
    {
        *cancel_cookie = (IUnknown *)result;
        IUnknown_AddRef(*cancel_cookie);
    }

    IMFAsyncResult_Release(result);

    return hr;
}

static HRESULT WINAPI stream_handler_EndCreateObject(IMFByteStreamHandler *iface, IMFAsyncResult *result,
        MF_OBJECT_TYPE *type, IUnknown **object)
{
    struct stream_handler *handler = impl_from_IMFByteStreamHandler(iface);
    struct result_entry *entry;
    HRESULT hr;

    TRACE("%p, %p, %p, %p.\n", iface, result, type, object);

    if (!(entry = handler_find_result_entry(handler, result)))
    {
        *type = MF_OBJECT_INVALID;
        *object = NULL;
        return MF_E_UNEXPECTED;
    }

    hr = IMFAsyncResult_GetStatus(entry->result);
    *type = entry->type;
    *object = entry->object;
    IUnknown_AddRef(*object);
    result_entry_destroy(entry);
    return hr;
}

static HRESULT WINAPI stream_handler_CancelObjectCreation(IMFByteStreamHandler *iface, IUnknown *cookie)
{
    struct stream_handler *handler = impl_from_IMFByteStreamHandler(iface);
    IMFAsyncResult *result = (IMFAsyncResult *)cookie;
    struct result_entry *entry;

    TRACE("%p, %p.\n", iface, cookie);

    if (!(entry = handler_find_result_entry(handler, result)))
        return MF_E_UNEXPECTED;

    result_entry_destroy(entry);
    return S_OK;
}

static HRESULT WINAPI stream_handler_GetMaxNumberOfBytesRequiredForResolution(IMFByteStreamHandler *iface, QWORD *bytes)
{
    FIXME("stub (%p %p)\n", iface, bytes);
    return E_NOTIMPL;
}

static const IMFByteStreamHandlerVtbl stream_handler_vtbl =
{
    stream_handler_QueryInterface,
    stream_handler_AddRef,
    stream_handler_Release,
    stream_handler_BeginCreateObject,
    stream_handler_EndCreateObject,
    stream_handler_CancelObjectCreation,
    stream_handler_GetMaxNumberOfBytesRequiredForResolution,
};

static HRESULT WINAPI stream_handler_callback_QueryInterface(IMFAsyncCallback *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFAsyncCallback) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFAsyncCallback_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI stream_handler_callback_AddRef(IMFAsyncCallback *iface)
{
    struct stream_handler *handler = impl_from_IMFAsyncCallback(iface);
    return IMFByteStreamHandler_AddRef(&handler->IMFByteStreamHandler_iface);
}

static ULONG WINAPI stream_handler_callback_Release(IMFAsyncCallback *iface)
{
    struct stream_handler *handler = impl_from_IMFAsyncCallback(iface);
    return IMFByteStreamHandler_Release(&handler->IMFByteStreamHandler_iface);
}

static HRESULT WINAPI stream_handler_callback_GetParameters(IMFAsyncCallback *iface, DWORD *flags, DWORD *queue)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_handler_callback_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct stream_handler *handler = impl_from_IMFAsyncCallback(iface);
    IUnknown *object, *state = IMFAsyncResult_GetStateNoAddRef(result);
    struct object_context *context;
    struct result_entry *entry;
    HRESULT hr;

    if (!state || !(context = impl_from_IUnknown(state)))
        return E_INVALIDARG;

    if (FAILED(hr = media_source_create(context, (IMFMediaSource **)&object)))
        WARN("Failed to create media source, hr %#lx\n", hr);
    else
    {
        if (FAILED(hr = result_entry_create(context->result, MF_OBJECT_MEDIASOURCE, object, &entry)))
            WARN("Failed to create handler result, hr %#lx\n", hr);
        else
        {
            EnterCriticalSection(&handler->cs);
            list_add_tail(&handler->results, &entry->entry);
            LeaveCriticalSection(&handler->cs);
        }

        IUnknown_Release(object);
    }

    IMFAsyncResult_SetStatus(context->result, hr);
    MFInvokeCallback(context->result);

    return S_OK;
}

static const IMFAsyncCallbackVtbl stream_handler_callback_vtbl =
{
    stream_handler_callback_QueryInterface,
    stream_handler_callback_AddRef,
    stream_handler_callback_Release,
    stream_handler_callback_GetParameters,
    stream_handler_callback_Invoke,
};

HRESULT winedmo_byte_stream_handler_create(REFIID riid, void **obj)
{
    struct stream_handler *handler;
    HRESULT hr;

    TRACE("%s, %p.\n", debugstr_guid(riid), obj);

    if (!(handler = calloc(1, sizeof(*handler))))
        return E_OUTOFMEMORY;

    list_init(&handler->results);
    InitializeCriticalSection(&handler->cs);

    handler->IMFByteStreamHandler_iface.lpVtbl = &stream_handler_vtbl;
    handler->IMFAsyncCallback_iface.lpVtbl = &stream_handler_callback_vtbl;
    handler->refcount = 1;

    hr = IMFByteStreamHandler_QueryInterface(&handler->IMFByteStreamHandler_iface, riid, obj);
    IMFByteStreamHandler_Release(&handler->IMFByteStreamHandler_iface);

    return hr;
}

/* ========================================================================
 * IMFSchemeHandler
 * ======================================================================== */

struct scheme_handler
{
    IMFSchemeHandler IMFSchemeHandler_iface;
    IMFAsyncCallback IMFAsyncCallback_iface;
    LONG refcount;
    struct list results;
    CRITICAL_SECTION cs;
};

static struct scheme_handler *impl_from_IMFSchemeHandler(IMFSchemeHandler *iface)
{
    return CONTAINING_RECORD(iface, struct scheme_handler, IMFSchemeHandler_iface);
}

static struct scheme_handler *scheme_handler_from_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct scheme_handler, IMFAsyncCallback_iface);
}

static struct result_entry *scheme_handler_find_result_entry(struct scheme_handler *handler, IMFAsyncResult *result)
{
    struct result_entry *entry;

    EnterCriticalSection(&handler->cs);
    LIST_FOR_EACH_ENTRY(entry, &handler->results, struct result_entry, entry)
    {
        if (result == entry->result)
        {
            list_remove(&entry->entry);
            LeaveCriticalSection(&handler->cs);
            return entry;
        }
    }
    LeaveCriticalSection(&handler->cs);

    return NULL;
}


static HRESULT WINAPI scheme_handler_QueryIntace(IMFSchemeHandler *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IMFSchemeHandler) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFSchemeHandler_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI scheme_handler_AddRef(IMFSchemeHandler *iface)
{
    struct scheme_handler *handler = impl_from_IMFSchemeHandler(iface);
    ULONG refcount = InterlockedIncrement(&handler->refcount);

    TRACE("%p, refcount %lu.\n", handler, refcount);

    return refcount;
}

static ULONG WINAPI scheme_handler_Release(IMFSchemeHandler *iface)
{
    struct scheme_handler *handler = impl_from_IMFSchemeHandler(iface);
    ULONG refcount = InterlockedDecrement(&handler->refcount);
    struct result_entry *result, *next;

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        LIST_FOR_EACH_ENTRY_SAFE(result, next, &handler->results, struct result_entry, entry)
            result_entry_destroy(result);
        DeleteCriticalSection(&handler->cs);
        free(handler);
    }

    return refcount;
}

static HRESULT WINAPI scheme_handler_BeginCreateObject(IMFSchemeHandler *iface, const WCHAR *url, DWORD flags,
        IPropertyStore *props, IUnknown **cancel_cookie, IMFAsyncCallback *callback, IUnknown *state)
{
    struct scheme_handler *handler = impl_from_IMFSchemeHandler(iface);
    IMFByteStream *bytestream;
    IMFAsyncResult *result;
    IUnknown *context;
    IStream *stream;
    HRESULT hr;

    TRACE("%p, %s, %#lx, %p, %p, %p, %p.\n", iface, debugstr_w(url), flags, props, cancel_cookie, callback, state);

    if (cancel_cookie)
        *cancel_cookie = NULL;

    if (!wcsnicmp(url, L"http://", 7) || !wcsnicmp(url, L"https://", 8))
    {
        if (FAILED(hr = URLOpenBlockingStreamW(NULL, url, &stream, 0, NULL)))
        {
            WARN("Failed to open url %s, hr %#lx\n", debugstr_w(url), hr);
            return hr;
        }
    }
    else if (FAILED(hr = CreateStreamOnHGlobal(0, TRUE, &stream)))
        return hr;

    hr = MFCreateMFByteStreamOnStream(stream, &bytestream);
    IStream_Release(stream);
    if (FAILED(hr))
        return hr;

    if (FAILED(hr = MFCreateAsyncResult(NULL, callback, state, &result)))
        return hr;
    if (FAILED(hr = object_context_create(flags, bytestream, url, -1, result, &context)))
    {
        IMFAsyncResult_Release(result);
        return hr;
    }

    hr = MFPutWorkItem(MFASYNC_CALLBACK_QUEUE_IO, &handler->IMFAsyncCallback_iface, context);
    IUnknown_Release(context);
    IMFAsyncResult_Release(result);

    return hr;
}

static HRESULT WINAPI scheme_handler_EndCreateObject(IMFSchemeHandler *iface, IMFAsyncResult *result,
        MF_OBJECT_TYPE *obj_type, IUnknown **object)
{
    struct scheme_handler *handler = impl_from_IMFSchemeHandler(iface);
    struct result_entry *entry;
    HRESULT hr;

    TRACE("%p, %p, %p, %p.\n", iface, result, obj_type, object);

    if (!(entry = scheme_handler_find_result_entry(handler, result)))
    {
        *obj_type = MF_OBJECT_INVALID;
        *object = NULL;
        return MF_E_UNEXPECTED;
    }

    hr = IMFAsyncResult_GetStatus(entry->result);
    *obj_type = MF_OBJECT_MEDIASOURCE;
    *object = entry->object;
    IUnknown_AddRef(*object);
    result_entry_destroy(entry);

    return hr;
}

static HRESULT WINAPI scheme_handler_CancelObjectCreation(IMFSchemeHandler *iface, IUnknown *cancel_cookie)
{
    /* Cancellation is not supported. */
    TRACE("%p, %p.\n", iface, cancel_cookie);
    return MF_E_UNEXPECTED;
}

static const IMFSchemeHandlerVtbl scheme_handler_vtbl =
{
    scheme_handler_QueryIntace,
    scheme_handler_AddRef,
    scheme_handler_Release,
    scheme_handler_BeginCreateObject,
    scheme_handler_EndCreateObject,
    scheme_handler_CancelObjectCreation,
};

static HRESULT WINAPI scheme_handler_callback_QueryInterface(IMFAsyncCallback *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFAsyncCallback) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFAsyncCallback_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI scheme_handler_callback_AddRef(IMFAsyncCallback *iface)
{
    struct scheme_handler *handler = scheme_handler_from_IMFAsyncCallback(iface);
    return IMFSchemeHandler_AddRef(&handler->IMFSchemeHandler_iface);
}

static ULONG WINAPI scheme_handler_callback_Release(IMFAsyncCallback *iface)
{
    struct scheme_handler *handler = scheme_handler_from_IMFAsyncCallback(iface);
    return IMFSchemeHandler_Release(&handler->IMFSchemeHandler_iface);
}

static HRESULT WINAPI scheme_handler_callback_GetParameters(IMFAsyncCallback *iface, DWORD *flags, DWORD *queue)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI scheme_handler_callback_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct scheme_handler *handler = scheme_handler_from_IMFAsyncCallback(iface);
    IUnknown *object, *state = IMFAsyncResult_GetStateNoAddRef(result);
    struct object_context *context;
    struct result_entry *entry;
    HRESULT hr;

    if (!state || !(context = impl_from_IUnknown(state)))
        return E_INVALIDARG;

    if (FAILED(hr = media_source_create(context, (IMFMediaSource **)&object)))
        WARN("Failed to create media source, hr %#lx\n", hr);
    else
    {
        if (FAILED(hr = result_entry_create(context->result, MF_OBJECT_MEDIASOURCE, object, &entry)))
            WARN("Failed to create handler result, hr %#lx\n", hr);
        else
        {
            EnterCriticalSection(&handler->cs);
            list_add_tail(&handler->results, &entry->entry);
            LeaveCriticalSection(&handler->cs);
        }

        IUnknown_Release(object);
    }

    IMFAsyncResult_SetStatus(context->result, hr);
    MFInvokeCallback(context->result);

    return S_OK;
}

static const IMFAsyncCallbackVtbl scheme_handler_callback_vtbl =
{
    scheme_handler_callback_QueryInterface,
    scheme_handler_callback_AddRef,
    scheme_handler_callback_Release,
    scheme_handler_callback_GetParameters,
    scheme_handler_callback_Invoke,
};

HRESULT winedmo_scheme_handler_create(REFIID riid, void **obj)
{
    struct scheme_handler *handler;
    HRESULT hr;

    TRACE("%s, %p.\n", debugstr_guid(riid), obj);

    if (!(handler = calloc(1, sizeof(*handler))))
        return E_OUTOFMEMORY;

    list_init(&handler->results);
    InitializeCriticalSection(&handler->cs);

    handler->IMFSchemeHandler_iface.lpVtbl = &scheme_handler_vtbl;
    handler->IMFAsyncCallback_iface.lpVtbl = &scheme_handler_callback_vtbl;
    handler->refcount = 1;

    hr = IMFSchemeHandler_QueryInterface(&handler->IMFSchemeHandler_iface, riid, obj);
    IMFSchemeHandler_Release(&handler->IMFSchemeHandler_iface);

    return hr;
}
