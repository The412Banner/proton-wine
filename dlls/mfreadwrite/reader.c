/*
 *
 * Copyright 2014 Austin English
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

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "ole2.h"
#include "rpcproxy.h"

#undef INITGUID
#include <guiddef.h>
#include "mfapi.h"
#include "mfidl.h"
#include "mfreadwrite.h"
#include "d3d9.h"
#include "initguid.h"
#include "dxva2api.h"

#include "wine/debug.h"
#include "wine/list.h"

#include "mf_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

DEFINE_MEDIATYPE_GUID(MFVideoFormat_ABGR32, D3DFMT_A8B8G8R8);

struct stream_response
{
    struct list entry;
    HRESULT status;
    DWORD stream_index;
    DWORD stream_flags;
    LONGLONG timestamp;
    IMFSample *sample;
};

enum media_stream_state
{
    STREAM_STATE_READY = 0,
    STREAM_STATE_EOS,
};

enum media_source_state
{
    SOURCE_STATE_STOPPED = 0,
    SOURCE_STATE_STARTED,
};

enum media_stream_flags
{
    STREAM_FLAG_SAMPLE_REQUESTED = 0x1, /* Protects from making multiple sample requests. */
    STREAM_FLAG_SELECTED = 0x2,         /* Mirrors descriptor, used to simplify tests when starting the source. */
    STREAM_FLAG_PRESENTED = 0x4,        /* Set if stream was selected last time Start() was called. */
    STREAM_FLAG_STOPPED = 0x8,          /* Received MEStreamStopped */
    STREAM_FLAG_EOS_REPORTED = 0x10,    /* Per-stream EOS already surfaced in ANY_STREAM mode. */
};

struct transform_entry
{
    struct list entry;
    IMFTransform *transform;
    unsigned int min_buffer_size;
    UINT32 pending_flags;
    GUID category;
    BOOL hidden;
    BOOL attributes_initialized;
    IMFVideoSampleAllocator *allocator;
    IMFMediaType *allocator_type;
};

struct retained_audio_sample
{
    struct list entry;
    IMFSample *sample;
};

struct media_stream
{
    IMFMediaStream *stream;
    IMFMediaType *current;
    struct list transforms;
    IMFTransform *transform_service;
    DWORD id;
    unsigned int index;
    enum media_stream_state state;
    unsigned int flags;
    unsigned int requests;
    unsigned int responses;
    LONGLONG last_sample_ts;
    LONGLONG delivered_sample_ts;
    LONGLONG queued_sample_ts;
    LONGLONG last_sample_duration;
    LONGLONG timestamp_adjust;
    BOOL have_sample_ts;
    BOOL is_new_stream;
    BOOL unthrottled_audio_output;
    struct list retained_audio_samples;
    struct source_reader *reader;
};

enum source_reader_async_op
{
    SOURCE_READER_ASYNC_READ,
    SOURCE_READER_ASYNC_SEEK,
    SOURCE_READER_ASYNC_FLUSH,
    SOURCE_READER_ASYNC_SAMPLE_READY,
};

struct source_reader_async_command
{
    IUnknown IUnknown_iface;
    LONG refcount;
    enum source_reader_async_op op;
    union
    {
        struct
        {
            unsigned int flags;
            unsigned int stream_index;
        } read;
        struct
        {
            GUID format;
            PROPVARIANT position;
        } seek;
        struct
        {
            unsigned int stream_index;
        } flush;
        struct
        {
            unsigned int stream_index;
        } sample;
        struct
        {
            unsigned int stream_index;
        } sa;
    } u;
};

enum source_reader_flags
{
    SOURCE_READER_FLUSHING = 0x1,
    SOURCE_READER_SEEKING = 0x2,
    SOURCE_READER_SHUTDOWN_ON_RELEASE = 0x4,
    SOURCE_READER_D3D9_DEVICE_MANAGER = 0x8,
    SOURCE_READER_DXGI_DEVICE_MANAGER = 0x10,
    SOURCE_READER_PRESERVE_SOURCE_TIMESTAMPS = 0x20,
    SOURCE_READER_ASYNC_SEEK_QUEUED = 0x40,
    SOURCE_READER_HAS_DEVICE_MANAGER = SOURCE_READER_D3D9_DEVICE_MANAGER | SOURCE_READER_DXGI_DEVICE_MANAGER,
};

struct source_reader
{
    IMFSourceReaderEx IMFSourceReaderEx_iface;
    IMFAsyncCallback source_events_callback;
    IMFAsyncCallback stream_events_callback;
    IMFAsyncCallback async_commands_callback;
    LONG refcount;
    LONG public_refcount;
    IMFMediaSource *source;
    IMFPresentationDescriptor *descriptor;
    IMFSourceReaderCallback *async_callback;
    IMFAttributes *attributes;
    IUnknown *device_manager;
    unsigned int first_audio_stream_index;
    unsigned int first_video_stream_index;
    DWORD stream_count;
    unsigned int flags;
    BOOL unthrottled_audio_output;
    unsigned int seek_serial;
    unsigned int completed_seek_serial;
    DWORD queue;
    enum media_source_state source_state;
    struct media_stream *streams;
    struct list responses;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE sample_event;
    CONDITION_VARIABLE state_event;
    CONDITION_VARIABLE stop_event;

    BOOL flag_eos_for_all_streams;
    BOOL presentation_ended;
    DWORD next_stream_eos_index;
};

static inline struct source_reader *impl_from_IMFSourceReaderEx(IMFSourceReaderEx *iface)
{
    return CONTAINING_RECORD(iface, struct source_reader, IMFSourceReaderEx_iface);
}

static struct source_reader *impl_from_source_callback_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct source_reader, source_events_callback);
}

static struct source_reader *impl_from_stream_callback_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct source_reader, stream_events_callback);
}

static struct source_reader *impl_from_async_commands_callback_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct source_reader, async_commands_callback);
}

static struct source_reader_async_command *impl_from_async_command_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct source_reader_async_command, IUnknown_iface);
}

static void source_reader_release_responses(struct source_reader *reader, struct media_stream *stream);
static HRESULT source_reader_get_stream_selection(const struct source_reader *reader, DWORD index, BOOL *selected);
static HRESULT source_reader_drain_transform_samples(struct source_reader *reader, struct media_stream *stream,
        struct transform_entry *entry);
static HRESULT source_reader_flush_transform_samples(struct source_reader *reader, struct media_stream *stream,
        struct transform_entry *entry);

static ULONG source_reader_addref(struct source_reader *reader)
{
    return InterlockedIncrement(&reader->refcount);
}

static void transform_entry_destroy(struct transform_entry *entry)
{
    if (entry->allocator_type)
        IMFMediaType_Release(entry->allocator_type);
    if (entry->allocator)
        IMFVideoSampleAllocator_Release(entry->allocator);
    IMFTransform_Release(entry->transform);
    free(entry);
}

static void media_stream_release_retained_audio_samples(struct media_stream *stream)
{
    struct retained_audio_sample *sample, *next;

    LIST_FOR_EACH_ENTRY_SAFE(sample, next, &stream->retained_audio_samples, struct retained_audio_sample, entry)
    {
        list_remove(&sample->entry);
        IMFSample_Release(sample->sample);
        free(sample);
    }
}

static void media_stream_destroy(struct media_stream *stream)
{
    struct transform_entry *entry, *next;

    media_stream_release_retained_audio_samples(stream);

    LIST_FOR_EACH_ENTRY_SAFE(entry, next, &stream->transforms, struct transform_entry, entry)
    {
        list_remove(&entry->entry);
        transform_entry_destroy(entry);
    }

    if (stream->transform_service)
        IMFTransform_Release(stream->transform_service);
    if (stream->stream)
        IMFMediaStream_Release(stream->stream);
    if (stream->current)
        IMFMediaType_Release(stream->current);
}

static ULONG source_reader_release(struct source_reader *reader)
{
    ULONG refcount = InterlockedDecrement(&reader->refcount);
    unsigned int i;

    if (!refcount)
    {
        if (reader->device_manager)
            IUnknown_Release(reader->device_manager);
        if (reader->async_callback)
            IMFSourceReaderCallback_Release(reader->async_callback);
        if (reader->descriptor)
            IMFPresentationDescriptor_Release(reader->descriptor);
        if (reader->attributes)
            IMFAttributes_Release(reader->attributes);
        if (reader->queue)
            MFUnlockWorkQueue(reader->queue);
        IMFMediaSource_Release(reader->source);

        for (i = 0; i < reader->stream_count; ++i)
        {
            struct media_stream *stream = &reader->streams[i];
            media_stream_destroy(stream);
        }
        source_reader_release_responses(reader, NULL);
        free(reader->streams);
        DeleteCriticalSection(&reader->cs);
        free(reader);
    }

    return refcount;
}

static HRESULT WINAPI source_reader_async_command_QueryInterface(IUnknown *iface, REFIID riid, void **obj)
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

static ULONG WINAPI source_reader_async_command_AddRef(IUnknown *iface)
{
    struct source_reader_async_command *command = impl_from_async_command_IUnknown(iface);
    return InterlockedIncrement(&command->refcount);
}

static ULONG WINAPI source_reader_async_command_Release(IUnknown *iface)
{
    struct source_reader_async_command *command = impl_from_async_command_IUnknown(iface);
    ULONG refcount = InterlockedIncrement(&command->refcount);

    if (!refcount)
    {
        if (command->op == SOURCE_READER_ASYNC_SEEK)
            PropVariantClear(&command->u.seek.position);
        free(command);
    }

    return refcount;
}

static const IUnknownVtbl source_reader_async_command_vtbl =
{
    source_reader_async_command_QueryInterface,
    source_reader_async_command_AddRef,
    source_reader_async_command_Release,
};

static HRESULT source_reader_create_async_op(enum source_reader_async_op op, struct source_reader_async_command **ret)
{
    struct source_reader_async_command *command;

    if (!(command = calloc(1, sizeof(*command))))
        return E_OUTOFMEMORY;

    command->IUnknown_iface.lpVtbl = &source_reader_async_command_vtbl;
    command->op = op;

    *ret = command;

    return S_OK;
}

static HRESULT media_event_get_object(IMFMediaEvent *event, REFIID riid, void **obj)
{
    PROPVARIANT value;
    HRESULT hr;

    PropVariantInit(&value);
    if (FAILED(hr = IMFMediaEvent_GetValue(event, &value)))
    {
        WARN("Failed to get event value, hr %#lx.\n", hr);
        return hr;
    }

    if (value.vt != VT_UNKNOWN || !value.punkVal)
    {
        WARN("Unexpected value type %d.\n", value.vt);
        PropVariantClear(&value);
        return E_UNEXPECTED;
    }

    hr = IUnknown_QueryInterface(value.punkVal, riid, obj);
    PropVariantClear(&value);
    if (FAILED(hr))
    {
        WARN("Unexpected object type.\n");
        return hr;
    }

    return hr;
}

static HRESULT media_stream_get_id(IMFMediaStream *stream, DWORD *id)
{
    IMFStreamDescriptor *sd;
    HRESULT hr;

    if (SUCCEEDED(hr = IMFMediaStream_GetStreamDescriptor(stream, &sd)))
    {
        hr = IMFStreamDescriptor_GetStreamIdentifier(sd, id);
        IMFStreamDescriptor_Release(sd);
    }

    return hr;
}

static HRESULT WINAPI source_reader_callback_QueryInterface(IMFAsyncCallback *iface,
        REFIID riid, void **obj)
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

static ULONG WINAPI source_reader_source_events_callback_AddRef(IMFAsyncCallback *iface)
{
    struct source_reader *reader = impl_from_source_callback_IMFAsyncCallback(iface);
    return source_reader_addref(reader);
}

static ULONG WINAPI source_reader_source_events_callback_Release(IMFAsyncCallback *iface)
{
    struct source_reader *reader = impl_from_source_callback_IMFAsyncCallback(iface);
    return source_reader_release(reader);
}

static HRESULT WINAPI source_reader_callback_GetParameters(IMFAsyncCallback *iface,
        DWORD *flags, DWORD *queue)
{
    return E_NOTIMPL;
}

static HRESULT source_reader_get_serial_queue_parameters(struct source_reader *reader, DWORD *flags, DWORD *queue)
{
    *flags = 0;
    *queue = reader->queue;
    return S_OK;
}

static HRESULT WINAPI source_reader_source_events_callback_GetParameters(IMFAsyncCallback *iface,
        DWORD *flags, DWORD *queue)
{
    return source_reader_get_serial_queue_parameters(impl_from_source_callback_IMFAsyncCallback(iface), flags, queue);
}

static HRESULT WINAPI source_reader_stream_events_callback_GetParameters(IMFAsyncCallback *iface,
        DWORD *flags, DWORD *queue)
{
    return source_reader_get_serial_queue_parameters(impl_from_stream_callback_IMFAsyncCallback(iface), flags, queue);
}

static void source_reader_response_ready(struct source_reader *reader, struct stream_response *response)
{
    struct source_reader_async_command *command;
    struct media_stream *stream = &reader->streams[response->stream_index];
    HRESULT hr;

    if (reader->async_callback)
    {
        if (!stream->requests)
            return;

        if (SUCCEEDED(source_reader_create_async_op(SOURCE_READER_ASYNC_SAMPLE_READY, &command)))
        {
            command->u.sample.stream_index = stream->index;
            if (FAILED(hr = MFPutWorkItem(reader->queue, &reader->async_commands_callback, &command->IUnknown_iface)))
                WARN("Failed to submit async result, hr %#lx.\n", hr);
            IUnknown_Release(&command->IUnknown_iface);
        }
        stream->requests--;
    }
    else
    {
        WakeAllConditionVariable(&reader->sample_event);
        if (stream->requests)
            stream->requests--;
    }
}

static void source_reader_reset_stream_timing(struct media_stream *stream)
{
    stream->last_sample_ts = 0;
    stream->delivered_sample_ts = 0;
    stream->queued_sample_ts = 0;
    stream->last_sample_duration = 0;
    stream->timestamp_adjust = 0;
    stream->have_sample_ts = FALSE;
    stream->is_new_stream = TRUE;
    media_stream_release_retained_audio_samples(stream);
}

static void source_reader_adjust_queued_stream_timestamps(struct source_reader *reader, struct media_stream *stream,
        LONGLONG adjust)
{
    struct stream_response *response;

    if (!adjust)
        return;

    LIST_FOR_EACH_ENTRY(response, &reader->responses, struct stream_response, entry)
    {
        if (response->stream_index != stream->index)
            continue;
        if (!response->sample)
            continue;
        response->timestamp += adjust;
    }

    if (stream->have_sample_ts)
        stream->queued_sample_ts += adjust;
}

static BOOL source_reader_stream_is_video(struct media_stream *stream);
static BOOL source_reader_stream_is_audio(struct media_stream *stream);

static void source_reader_retain_delivered_sample(struct media_stream *stream, IMFSample *sample)
{
    struct retained_audio_sample *retained_sample;

    if (!sample || !source_reader_stream_is_audio(stream))
        return;

    if (!(retained_sample = calloc(1, sizeof(*retained_sample))))
    {
        WARN("Failed to retain delivered audio sample.\n");
        return;
    }

    retained_sample->sample = sample;
    IMFSample_AddRef(retained_sample->sample);
    list_add_tail(&stream->retained_audio_samples, &retained_sample->entry);
}

static HRESULT source_reader_queue_response(struct source_reader *reader, struct media_stream *stream, HRESULT status,
        DWORD stream_flags, LONGLONG timestamp, IMFSample *sample)
{
    struct stream_response *response;

    if (!(response = calloc(1, sizeof(*response))))
        return E_OUTOFMEMORY;

    response->status = status;
    response->stream_index = stream->index;
    response->stream_flags = stream_flags;
    if (timestamp && stream->timestamp_adjust)
        timestamp += stream->timestamp_adjust;
    response->timestamp = timestamp;
    response->sample = sample;
    if (response->sample)
        IMFSample_AddRef(response->sample);

    list_add_tail(&reader->responses, &response->entry);
    stream->responses++;

    source_reader_response_ready(reader, response);

    if (sample)
    {
        LONGLONG duration;

        stream->last_sample_ts = timestamp;
        stream->queued_sample_ts = timestamp;
        stream->last_sample_duration = SUCCEEDED(IMFSample_GetSampleDuration(sample, &duration)) ? duration : 0;
        stream->have_sample_ts = TRUE;
    }

    return S_OK;
}

static HRESULT source_reader_queue_sample(struct source_reader *reader, struct media_stream *stream,
        UINT flags, IMFSample *sample)
{
    const LONGLONG startup_threshold = 2000000; /* 200 ms in 100ns units */
    const LONGLONG discontinuity_threshold = 20000000; /* 2 seconds in 100ns units */
    const LONGLONG max_frame_duration = 1000000; /* 100 ms in 100ns units */
    LONGLONG duration = 0;
    LONGLONG timestamp = 0;
    LONGLONG adjusted_timestamp;

    stream->flags &= ~STREAM_FLAG_EOS_REPORTED;

    if (FAILED(IMFSample_GetSampleTime(sample, &timestamp)))
        WARN("Sample time wasn't set.\n");
    if (FAILED(IMFSample_GetSampleDuration(sample, &duration)))
        duration = 0;

    if (stream->is_new_stream)
    {
        if (!(reader->flags & SOURCE_READER_PRESERVE_SOURCE_TIMESTAMPS) && timestamp > startup_threshold)
        {
            stream->timestamp_adjust = -timestamp;
            source_reader_adjust_queued_stream_timestamps(reader, stream, stream->timestamp_adjust);
            IMFSample_SetSampleTime(sample, 0);
            timestamp = 0;
        }

        flags |= MF_SOURCE_READERF_NEWSTREAM;
        stream->is_new_stream = FALSE;
    }
    else if (!(reader->flags & SOURCE_READER_PRESERVE_SOURCE_TIMESTAMPS) && timestamp > startup_threshold)
    {
        LONGLONG adjusted = timestamp + stream->timestamp_adjust;

        if (adjusted - stream->last_sample_ts > discontinuity_threshold)
        {
            LONGLONG expected = stream->have_sample_ts ? stream->queued_sample_ts : stream->last_sample_ts;

            if (duration > 0 && duration < discontinuity_threshold)
                expected += duration;

            TRACE("Rebasing stream %u timestamp jump from %s to %s.\n",
                    stream->index, wine_dbgstr_longlong(adjusted), wine_dbgstr_longlong(expected));
            stream->timestamp_adjust += expected - adjusted;
            source_reader_adjust_queued_stream_timestamps(reader, stream, expected - adjusted);
            IMFSample_SetSampleTime(sample, expected);
        }
    }

    adjusted_timestamp = timestamp;
    if (adjusted_timestamp && stream->timestamp_adjust)
        adjusted_timestamp += stream->timestamp_adjust;

    if (source_reader_stream_is_video(stream) && stream->have_sample_ts)
    {
        LONGLONG delta = adjusted_timestamp - stream->queued_sample_ts;

        if (reader->flag_eos_for_all_streams && reader->unthrottled_audio_output
                && stream->last_sample_duration > 0
                && delta < stream->last_sample_duration && stream->last_sample_duration < max_frame_duration)
        {
            adjusted_timestamp = stream->queued_sample_ts + stream->last_sample_duration;
            timestamp = adjusted_timestamp - stream->timestamp_adjust;
            delta = stream->last_sample_duration;

            TRACE("Advancing stream %u video sample timestamp to %s from previous duration %s.\n",
                    stream->index, wine_dbgstr_longlong(adjusted_timestamp),
                    wine_dbgstr_longlong(stream->last_sample_duration));
            IMFSample_SetSampleTime(sample, adjusted_timestamp);
        }

        if (delta > 0 && delta < max_frame_duration && !duration)
        {
            TRACE("Updating stream %u video sample duration from %s to timestamp delta %s.\n",
                    stream->index, wine_dbgstr_longlong(duration), wine_dbgstr_longlong(delta));
            IMFSample_SetSampleDuration(sample, delta);
        }
    }

    return source_reader_queue_response(reader, stream, S_OK, flags, timestamp, sample);
}

static BOOL source_reader_request_sample_eos(HRESULT hr)
{
    return hr == MF_E_END_OF_STREAM;
}

static HRESULT source_reader_request_sample(struct source_reader *reader, struct media_stream *stream)
{
    HRESULT hr = S_OK;

    if (stream->stream && !(stream->flags & STREAM_FLAG_SAMPLE_REQUESTED))
    {
        if (FAILED(hr = IMFMediaStream_RequestSample(stream->stream, NULL)))
        {
            if (source_reader_request_sample_eos(hr))
            {
                stream->state = STREAM_STATE_EOS;
                stream->flags &= ~(STREAM_FLAG_SAMPLE_REQUESTED | STREAM_FLAG_EOS_REPORTED);
                while (stream->requests)
                    source_reader_queue_response(reader, stream, S_OK, MF_SOURCE_READERF_ENDOFSTREAM, 0, NULL);
                hr = S_OK;
            }
            else
                WARN("Sample request failed, hr %#lx.\n", hr);
        }
        else
        {
            stream->flags |= STREAM_FLAG_SAMPLE_REQUESTED;
        }
    }

    return hr;
}

static HRESULT source_reader_new_stream_handler(struct source_reader *reader, IMFMediaEvent *event)
{
    IMFMediaStream *stream;
    unsigned int i;
    DWORD id = 0;
    HRESULT hr;

    if (FAILED(hr = media_event_get_object(event, &IID_IMFMediaStream, (void **)&stream)))
    {
        WARN("Failed to get stream object, hr %#lx.\n", hr);
        return hr;
    }

    TRACE("Got new stream %p.\n", stream);

    if (FAILED(hr = media_stream_get_id(stream, &id)))
    {
        WARN("Unidentified stream %p, hr %#lx.\n", stream, hr);
        IMFMediaStream_Release(stream);
        return hr;
    }

    EnterCriticalSection(&reader->cs);

    for (i = 0; i < reader->stream_count; ++i)
    {
        if (id == reader->streams[i].id)
        {
            if (reader->streams[i].stream != stream)
            {
                unsigned int pending_requests = min(reader->streams[i].requests, 1u);
                struct list *ptr;

                source_reader_release_responses(reader, &reader->streams[i]);
                reader->next_stream_eos_index = 0;

                if ((ptr = list_head(&reader->streams[i].transforms)))
                {
                    struct transform_entry *entry = LIST_ENTRY(ptr, struct transform_entry, entry);
                    if (FAILED(hr = source_reader_flush_transform_samples(reader, &reader->streams[i], entry)))
                        WARN("Failed to flush transforms on stream replacement, hr %#lx.\n", hr);
                }

                if (reader->streams[i].stream)
                    IMFMediaStream_Release(reader->streams[i].stream);

                reader->streams[i].stream = stream;
                reader->streams[i].state = STREAM_STATE_READY;
                reader->streams[i].flags &= ~(STREAM_FLAG_SAMPLE_REQUESTED | STREAM_FLAG_EOS_REPORTED | STREAM_FLAG_STOPPED);
                reader->streams[i].requests = 0;
                reader->streams[i].responses = 0;
                source_reader_reset_stream_timing(&reader->streams[i]);
                IMFMediaStream_AddRef(reader->streams[i].stream);
                if (FAILED(hr = IMFMediaStream_BeginGetEvent(stream, &reader->stream_events_callback,
                        (IUnknown *)stream)))
                {
                    WARN("Failed to subscribe to stream events, hr %#lx.\n", hr);
                }

                if (pending_requests)
                {
                    reader->streams[i].requests = pending_requests;
                    if (FAILED(source_reader_request_sample(reader, &reader->streams[i])))
                        WakeAllConditionVariable(&reader->sample_event);
                }
            }
            break;
        }
    }

    if (i == reader->stream_count)
        WARN("Stream with id %#lx was not present in presentation descriptor.\n", id);

    LeaveCriticalSection(&reader->cs);

    WakeAllConditionVariable(&reader->sample_event);

    IMFMediaStream_Release(stream);

    return hr;
}

static HRESULT source_reader_source_state_handler(struct source_reader *reader, MediaEventType event_type)
{
    EnterCriticalSection(&reader->cs);

    switch (event_type)
    {
        case MESourceStarted:
            reader->source_state = SOURCE_STATE_STARTED;
            reader->flags &= ~SOURCE_READER_SEEKING;
            reader->next_stream_eos_index = 0;
            reader->presentation_ended = FALSE;
            break;
        case MESourceStopped:
            reader->source_state = SOURCE_STATE_STOPPED;
            reader->flags &= ~SOURCE_READER_SEEKING;
            break;
        case MESourceSeeked:
            reader->completed_seek_serial = reader->seek_serial;
            reader->flags &= ~SOURCE_READER_SEEKING;
            break;
        default:
            WARN("Unhandled event %ld.\n", event_type);
    }

    LeaveCriticalSection(&reader->cs);

    WakeAllConditionVariable(&reader->state_event);
    WakeAllConditionVariable(&reader->sample_event);
    if (event_type == MESourceStopped)
        WakeAllConditionVariable(&reader->stop_event);

    return S_OK;
}

static HRESULT source_reader_end_of_presentation_handler(struct source_reader *reader)
{
    EnterCriticalSection(&reader->cs);

    reader->presentation_ended = TRUE;
    reader->next_stream_eos_index = 0;

    LeaveCriticalSection(&reader->cs);

    WakeAllConditionVariable(&reader->sample_event);

    return S_OK;
}

static HRESULT WINAPI source_reader_source_events_callback_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct source_reader *reader = impl_from_source_callback_IMFAsyncCallback(iface);
    MediaEventType event_type;
    IMFMediaSource *source;
    IMFMediaEvent *event;
    HRESULT hr;

    TRACE("%p, %p.\n", iface, result);

    source = (IMFMediaSource *)IMFAsyncResult_GetStateNoAddRef(result);

    if (FAILED(hr = IMFMediaSource_EndGetEvent(source, result, &event)))
        return hr == MF_E_SHUTDOWN ? S_OK : hr;

    IMFMediaEvent_GetType(event, &event_type);

    TRACE("Got event %lu.\n", event_type);

    switch (event_type)
    {
        case MENewStream:
            hr = source_reader_new_stream_handler(reader, event);
            break;
        case MESourceStarted:
        case MESourcePaused:
        case MESourceStopped:
        case MESourceSeeked:
            hr = source_reader_source_state_handler(reader, event_type);
            break;
        case MEEndOfPresentation:
            hr = source_reader_end_of_presentation_handler(reader);
            break;
        case MEBufferingStarted:
        case MEBufferingStopped:
        case MEConnectStart:
        case MEConnectEnd:
        case MEExtendedType:
        case MESourceCharacteristicsChanged:
        case MESourceMetadataChanged:
        case MEContentProtectionMetadata:
        case MEDeviceThermalStateChanged:
            if (reader->async_callback)
                IMFSourceReaderCallback_OnEvent(reader->async_callback, MF_SOURCE_READER_MEDIASOURCE, event);
            break;
        default:
            ;
    }

    if (hr == MF_E_SHUTDOWN)
        TRACE("Ignoring late event %lu from shutdown media source %p.\n", event_type, source);
    else if (FAILED(hr))
        WARN("Failed while handling %ld event, hr %#lx.\n", event_type, hr);

    IMFMediaEvent_Release(event);

    if (event_type != MESourceStopped && hr != MF_E_SHUTDOWN)
    {
        HRESULT begin_hr;

        if (FAILED(begin_hr = IMFMediaSource_BeginGetEvent(source, iface, (IUnknown *)source))
                && begin_hr != MF_E_SHUTDOWN)
            WARN("Failed to subscribe to source events, hr %#lx.\n", begin_hr);
    }

    return S_OK;
}

static const IMFAsyncCallbackVtbl source_events_callback_vtbl =
{
    source_reader_callback_QueryInterface,
    source_reader_source_events_callback_AddRef,
    source_reader_source_events_callback_Release,
    source_reader_source_events_callback_GetParameters,
    source_reader_source_events_callback_Invoke,
};

static ULONG WINAPI source_reader_stream_events_callback_AddRef(IMFAsyncCallback *iface)
{
    struct source_reader *reader = impl_from_stream_callback_IMFAsyncCallback(iface);
    return source_reader_addref(reader);
}

static ULONG WINAPI source_reader_stream_events_callback_Release(IMFAsyncCallback *iface)
{
    struct source_reader *reader = impl_from_stream_callback_IMFAsyncCallback(iface);
    return source_reader_release(reader);
}

static void transform_entry_release_allocator(struct transform_entry *entry)
{
    if (entry->allocator_type)
    {
        IMFMediaType_Release(entry->allocator_type);
        entry->allocator_type = NULL;
    }
    if (entry->allocator)
    {
        IMFVideoSampleAllocator_Release(entry->allocator);
        entry->allocator = NULL;
    }
}

static HRESULT source_reader_allocate_d3d_sample(struct source_reader *reader, struct transform_entry *entry,
        IMFMediaType *media_type, IMFSample **out)
{
    IMFVideoSampleAllocator *allocator;
    GUID major_type;
    GUID subtype;
    DWORD flags;
    HRESULT hr;

    if (!reader->device_manager)
        return E_FAIL;
    if (FAILED(IMFMediaType_GetMajorType(media_type, &major_type)) || !IsEqualGUID(&major_type, &MFMediaType_Video))
        return E_FAIL;
    if (FAILED(IMFMediaType_GetGUID(media_type, &MF_MT_SUBTYPE, &subtype)) || !IsEqualGUID(&subtype, &MFVideoFormat_NV12))
        return E_FAIL;

    if (entry->allocator && entry->allocator_type
            && SUCCEEDED(IMFMediaType_IsEqual(entry->allocator_type, media_type, &flags))
            && (flags & MF_MEDIATYPE_EQUAL_FORMAT_DATA))
        return IMFVideoSampleAllocator_AllocateSample(entry->allocator, out);

    transform_entry_release_allocator(entry);

    if (FAILED(hr = MFCreateVideoSampleAllocatorEx(&IID_IMFVideoSampleAllocator, (void **)&allocator)))
        return hr;

    if (SUCCEEDED(hr = IMFVideoSampleAllocator_SetDirectXManager(allocator, reader->device_manager))
            && SUCCEEDED(hr = IMFVideoSampleAllocator_InitializeSampleAllocator(allocator, 6, media_type))
            && SUCCEEDED(hr = IMFVideoSampleAllocator_AllocateSample(allocator, out)))
    {
        entry->allocator = allocator;
        entry->allocator_type = media_type;
        IMFMediaType_AddRef(entry->allocator_type);
        return hr;
    }

    IMFVideoSampleAllocator_Release(allocator);
    return hr;
}

static HRESULT source_reader_allocate_stream_sample(struct source_reader *reader, struct transform_entry *entry,
        MFT_OUTPUT_STREAM_INFO *info, IMFSample **out)
{
    IMFMediaType *media_type;
    IMFMediaBuffer *buffer;
    IMFSample *sample;
    UINT64 frame_size;
    UINT32 height, sample_size, stride, width;
    GUID subtype;
    HRESULT hr;

    *out = NULL;
    if (SUCCEEDED(hr = IMFTransform_GetOutputCurrentType(entry->transform, 0, &media_type)))
    {
        if (SUCCEEDED(source_reader_allocate_d3d_sample(reader, entry, media_type, out)))
        {
            IMFMediaType_Release(media_type);
            return S_OK;
        }

        if (FAILED(hr = IMFMediaType_GetGUID(media_type, &MF_MT_SUBTYPE, &subtype))
                || FAILED(hr = IMFMediaType_GetUINT64(media_type, &MF_MT_FRAME_SIZE, &frame_size)))
            hr = MFCreateMediaBufferFromMediaType(media_type, 10000000, info->cbSize, info->cbAlignment, &buffer);
        else
        {
            width = frame_size >> 32;
            height = (UINT32)frame_size;

            if (IsEqualGUID(&subtype, &MFVideoFormat_NV12) && (width % 16 || height % 16)
                    && SUCCEEDED(MFCalculateImageSize(&subtype, width, height, &sample_size)))
                hr = MFCreateAlignedMemoryBuffer(sample_size, info->cbAlignment, &buffer);
            else
                hr = MFCreate2DMediaBuffer(width, height, subtype.Data1,
                        SUCCEEDED(IMFMediaType_GetUINT32(media_type, &MF_MT_DEFAULT_STRIDE, &stride))
                        && (INT32)stride < 0, &buffer);
            if (FAILED(hr))
                hr = MFCreateMediaBufferFromMediaType(media_type, 10000000, info->cbSize, info->cbAlignment, &buffer);
        }
        IMFMediaType_Release(media_type);
    }
    if (FAILED(hr) && FAILED(hr = MFCreateAlignedMemoryBuffer(info->cbSize, info->cbAlignment, &buffer)))
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

static void media_type_try_copy_attr(IMFMediaType *dst, IMFMediaType *src, const GUID *attr, HRESULT *hr)
{
    PROPVARIANT value;

    PropVariantInit(&value);
    if (SUCCEEDED(*hr) && FAILED(IMFMediaType_GetItem(dst, attr, NULL))
            && SUCCEEDED(IMFMediaType_GetItem(src, attr, &value)))
        *hr = IMFMediaType_SetItem(dst, attr, &value);
    PropVariantClear(&value);
}

/* update a media type with additional attributes reported by upstream element */
/* also present in mf/topology_loader.c pipeline */
static HRESULT update_media_type_from_upstream(IMFMediaType *media_type, IMFMediaType *upstream_type, BOOL advanced)
{
    GUID major_type, subtype, upstream_subtype;
    BOOL same_audio_subtype = TRUE;
    HRESULT hr = S_OK;

    /* propagate common video attributes */
    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_FRAME_SIZE, &hr);
    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_FRAME_RATE, &hr);
    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_VIDEO_ROTATION, &hr);
    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_FIXED_SIZE_SAMPLES, &hr);
    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_PIXEL_ASPECT_RATIO, &hr);
    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_ALL_SAMPLES_INDEPENDENT, &hr);
    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_MINIMUM_DISPLAY_APERTURE, &hr);

    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_VIDEO_CHROMA_SITING, &hr);
    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_INTERLACE_MODE, &hr);
    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_TRANSFER_FUNCTION, &hr);
    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_VIDEO_PRIMARIES, &hr);
    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_YUV_MATRIX, &hr);
    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_VIDEO_LIGHTING, &hr);
    media_type_try_copy_attr(media_type, upstream_type, &MF_MT_VIDEO_NOMINAL_RANGE, &hr);

    if (!advanced)
        media_type_try_copy_attr(media_type, upstream_type, &MF_MT_DEFAULT_STRIDE, &hr);

    if (SUCCEEDED(IMFMediaType_GetMajorType(media_type, &major_type)) && IsEqualGUID(&major_type, &MFMediaType_Audio)
            && SUCCEEDED(IMFMediaType_GetGUID(media_type, &MF_MT_SUBTYPE, &subtype))
            && SUCCEEDED(IMFMediaType_GetGUID(upstream_type, &MF_MT_SUBTYPE, &upstream_subtype)))
        same_audio_subtype = IsEqualGUID(&subtype, &upstream_subtype);

    /* propagate common audio attributes */
    if (same_audio_subtype)
    {
        media_type_try_copy_attr(media_type, upstream_type, &MF_MT_AUDIO_NUM_CHANNELS, &hr);
        media_type_try_copy_attr(media_type, upstream_type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, &hr);
        media_type_try_copy_attr(media_type, upstream_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &hr);
        media_type_try_copy_attr(media_type, upstream_type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &hr);
        media_type_try_copy_attr(media_type, upstream_type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &hr);
        media_type_try_copy_attr(media_type, upstream_type, &MF_MT_AUDIO_CHANNEL_MASK, &hr);
        media_type_try_copy_attr(media_type, upstream_type, &MF_MT_AUDIO_SAMPLES_PER_BLOCK, &hr);
        media_type_try_copy_attr(media_type, upstream_type, &MF_MT_AUDIO_VALID_BITS_PER_SAMPLE, &hr);
    }
    else
    {
        media_type_try_copy_attr(media_type, upstream_type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &hr);
    }

    return hr;
}

static HRESULT source_reader_pull_transform_samples(struct source_reader *reader, struct media_stream *stream,
        struct transform_entry *entry);
static inline BOOL source_reader_is_nonfatal_transform_status(HRESULT hr)
{
    return hr == MF_E_TRANSFORM_NEED_MORE_INPUT || hr == MF_E_END_OF_STREAM;
}

static BOOL source_reader_stream_is_audio(struct media_stream *stream)
{
    GUID major_type;

    return SUCCEEDED(IMFMediaType_GetMajorType(stream->current, &major_type))
            && IsEqualGUID(&major_type, &MFMediaType_Audio);
}

static BOOL source_reader_stream_is_video(struct media_stream *stream)
{
    GUID major_type;

    return SUCCEEDED(IMFMediaType_GetMajorType(stream->current, &major_type))
            && IsEqualGUID(&major_type, &MFMediaType_Video);
}

static BOOL source_reader_audio_needs_unthrottled_output(IMFMediaType *type)
{
    UINT32 avg_bytes_per_second = 0, bits_per_sample = 0, block_alignment = 0, channels = 0;
    GUID major_type, subtype;

    if (FAILED(IMFMediaType_GetMajorType(type, &major_type)) || !IsEqualGUID(&major_type, &MFMediaType_Audio))
        return FALSE;
    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return FALSE;
    if (IsEqualGUID(&subtype, &MFAudioFormat_AAC))
    {
        IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, &block_alignment);
        IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits_per_sample);
        return !(block_alignment && bits_per_sample);
    }

    if (!IsEqualGUID(&subtype, &MFAudioFormat_WMAudioV8)
            && !IsEqualGUID(&subtype, &MFAudioFormat_WMAudioV9)
            && !IsEqualGUID(&subtype, &MFAudioFormat_WMAudio_Lossless)
            && !IsEqualGUID(&subtype, &MFAudioFormat_WMASPDIF))
        return FALSE;

    IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &channels);
    IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &avg_bytes_per_second);
    IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, &block_alignment);

    return channels > 2 || block_alignment > 16000
            || (avg_bytes_per_second && avg_bytes_per_second < 16000);
}

static HRESULT source_reader_push_transform_samples(struct source_reader *reader, struct media_stream *stream,
        struct transform_entry *entry, IMFSample *sample)
{
    HRESULT hr;

    do
    {
        if (FAILED(hr = source_reader_pull_transform_samples(reader, stream, entry))
                && !source_reader_is_nonfatal_transform_status(hr))
            return hr;
        if (SUCCEEDED(hr = IMFTransform_ProcessInput(entry->transform, 0, sample, 0)))
            return source_reader_pull_transform_samples(reader, stream, entry);
    }
    while (hr == MF_E_NOTACCEPTING);

    return hr;
}

/* update the transform output type while keeping subtype which matches the desired type */
static HRESULT set_matching_transform_output_type(IMFTransform *transform, IMFMediaType *old_output_type)
{
    IMFMediaType *new_output_type;
    GUID subtype, desired;
    UINT i = 0;
    HRESULT hr;

    IMFMediaType_GetGUID(old_output_type, &MF_MT_SUBTYPE, &desired);

    /* find an available output type matching the desired subtype */
    while (SUCCEEDED(hr = IMFTransform_GetOutputAvailableType(transform, 0, i++, &new_output_type)))
    {
        IMFMediaType_GetGUID(new_output_type, &MF_MT_SUBTYPE, &subtype);
        if (IsEqualGUID(&subtype, &desired) && SUCCEEDED(hr = IMFTransform_SetOutputType(transform, 0, new_output_type, 0)))
        {
            IMFMediaType_Release(new_output_type);
            return S_OK;
        }
        IMFMediaType_Release(new_output_type);
    }

    return hr;
}

/* update the transform output type while keeping subtype which matches the old output type */
static HRESULT transform_entry_update_output_type(struct transform_entry *entry, IMFMediaType *old_output_type)
{
    IMFMediaType *new_output_type = NULL;
    DWORD flags = 0;
    HRESULT hr;

    if (FAILED(hr = set_matching_transform_output_type(entry->transform, old_output_type)))
        return hr;
    transform_entry_release_allocator(entry);

    /* Only signal a format change to the consumer if the output format data actually
     * differs from what it was before. This avoids spurious CURRENTMEDIATYPECHANGED
     * notifications when the decoder normalizes its output (e.g. mono→stereo, upsample)
     * and the effective consumer-visible format did not change. */
    {
        HRESULT get_hr = IMFTransform_GetOutputCurrentType(entry->transform, 0, &new_output_type);
        HRESULT eq_hr = new_output_type ? IMFMediaType_IsEqual(old_output_type, new_output_type, &flags) : E_FAIL;

        if (FAILED(get_hr) || FAILED(eq_hr) || !(flags & MF_MEDIATYPE_EQUAL_FORMAT_DATA))
            entry->pending_flags |= MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED;
    }

    if (new_output_type)
        IMFMediaType_Release(new_output_type);
    return hr;
}

/* update the transform input type while keeping an output type which matches the current output subtype */
static HRESULT transform_entry_update_input_type(struct transform_entry *entry, IMFMediaType *input_type)
{
    IMFMediaType *old_output_type, *new_output_type;
    HRESULT hr;

    if (FAILED(hr = IMFTransform_GetOutputCurrentType(entry->transform, 0, &old_output_type)))
        return hr;
    if (FAILED(hr = IMFTransform_SetInputType(entry->transform, 0, input_type, 0)))
        return hr;
    transform_entry_release_allocator(entry);

    /* check if transform output type is still valid or if we need to update it as well */
    if (FAILED(hr = IMFTransform_GetOutputCurrentType(entry->transform, 0, &new_output_type)))
        hr = transform_entry_update_output_type(entry, old_output_type);
    else
        IMFMediaType_Release(new_output_type);

    IMFMediaType_Release(old_output_type);
    return hr;
}

static void transform_entry_initialize_attributes(struct source_reader *reader, struct transform_entry *entry)
{
    IMFAttributes *attributes;

    if (SUCCEEDED(IMFTransform_GetAttributes(entry->transform, &attributes)))
    {
        if (FAILED(IMFAttributes_GetItem(attributes, &MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT, NULL)))
            IMFAttributes_SetUINT32(attributes, &MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT, 6);

        IMFAttributes_Release(attributes);
    }

    if (SUCCEEDED(IMFTransform_GetOutputStreamAttributes(entry->transform, 0, &attributes)))
    {
        UINT32 shared, shared_without_mutex, bind_flags;

        if (SUCCEEDED(IMFAttributes_GetUINT32(reader->attributes, &MF_SA_D3D11_SHARED, &shared)))
            IMFAttributes_SetUINT32(attributes, &MF_SA_D3D11_SHARED, shared);
        if (SUCCEEDED(IMFAttributes_GetUINT32(reader->attributes, &MF_SA_D3D11_SHARED_WITHOUT_MUTEX, &shared_without_mutex)))
            IMFAttributes_SetUINT32(attributes, &MF_SA_D3D11_SHARED_WITHOUT_MUTEX, shared_without_mutex);
        if (FAILED(IMFAttributes_GetItem(attributes, &MF_SA_D3D11_BINDFLAGS, NULL))
                && SUCCEEDED(IMFAttributes_GetUINT32(reader->attributes, &MF_SOURCE_READER_D3D11_BIND_FLAGS, &bind_flags)))
            IMFAttributes_SetUINT32(attributes, &MF_SA_D3D11_BINDFLAGS, bind_flags);
        else if ((reader->flags & SOURCE_READER_DXGI_DEVICE_MANAGER)
                && FAILED(IMFAttributes_GetItem(attributes, &MF_SA_D3D11_BINDFLAGS, NULL)))
            IMFAttributes_SetUINT32(attributes, &MF_SA_D3D11_BINDFLAGS, 1024);

        IMFAttributes_Release(attributes);
    }
}

static HRESULT source_reader_pull_transform_samples(struct source_reader *reader, struct media_stream *stream,
        struct transform_entry *entry)
{
    MFT_OUTPUT_STREAM_INFO stream_info = {0};
    struct transform_entry *next = NULL;
    struct list *ptr;
    DWORD status;
    HRESULT hr;

    if ((ptr = list_next(&stream->transforms, &entry->entry)))
        next = LIST_ENTRY(ptr, struct transform_entry, entry);

    if (!entry->attributes_initialized)
    {
        transform_entry_initialize_attributes(reader, entry);
        entry->attributes_initialized = TRUE;
    }

    if (FAILED(hr = IMFTransform_GetOutputStreamInfo(entry->transform, 0, &stream_info)))
        return hr;
    stream_info.cbSize = max(stream_info.cbSize, entry->min_buffer_size);

    while (SUCCEEDED(hr))
    {
        MFT_OUTPUT_DATA_BUFFER out_buffer = {0};
        IMFMediaType *media_type;

        if (!(stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))
                && FAILED(hr = source_reader_allocate_stream_sample(reader, entry, &stream_info, &out_buffer.pSample)))
            break;

        if (SUCCEEDED(hr = IMFTransform_ProcessOutput(entry->transform, 0, 1, &out_buffer, &status)))
        {
            /* propagate upstream type to the transform input type */
            if ((entry->pending_flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
                    && SUCCEEDED(hr = IMFTransform_GetOutputCurrentType(entry->transform, 0, &media_type)))
            {
                if (!next)
                    hr = IMFMediaType_CopyAllItems(media_type, (IMFAttributes *)stream->current);
                else
                    hr = transform_entry_update_input_type(next, media_type);
                IMFMediaType_Release(media_type);
            }

            if (FAILED(hr))
                source_reader_queue_response(reader, stream, hr, MF_SOURCE_READERF_ERROR, 0, NULL);
            else if (next)
                hr = source_reader_push_transform_samples(reader, stream, next, out_buffer.pSample);
            else
                hr = source_reader_queue_sample(reader, stream, entry->pending_flags, out_buffer.pSample);

            entry->pending_flags = 0;
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE && SUCCEEDED(hr = IMFTransform_GetOutputCurrentType(entry->transform, 0, &media_type)))
        {
            hr = transform_entry_update_output_type(entry, media_type);
            IMFMediaType_Release(media_type);

            if (SUCCEEDED(hr))
            {
                hr = IMFTransform_GetOutputStreamInfo(entry->transform, 0, &stream_info);
                stream_info.cbSize = max(stream_info.cbSize, entry->min_buffer_size);
            }
        }

        if (out_buffer.pSample)
            IMFSample_Release(out_buffer.pSample);
        if (out_buffer.pEvents)
            IMFCollection_Release(out_buffer.pEvents);

        if (!reader->async_callback && stream->responses && !stream->requests
                && (!source_reader_stream_is_audio(stream) || !stream->unthrottled_audio_output))
            break;
    }

    return hr;
}

static HRESULT source_reader_drain_transform_samples(struct source_reader *reader, struct media_stream *stream,
        struct transform_entry *entry)
{
    struct transform_entry *next = NULL;
    struct list *ptr;
    HRESULT hr;

    if ((ptr = list_next(&stream->transforms, &entry->entry)))
        next = LIST_ENTRY(ptr, struct transform_entry, entry);

    if (FAILED(hr = IMFTransform_ProcessMessage(entry->transform, MFT_MESSAGE_COMMAND_DRAIN, 0)))
        WARN("Failed to drain transform %p, hr %#lx\n", entry->transform, hr);
    if (FAILED(hr = source_reader_pull_transform_samples(reader, stream, entry))
            && !source_reader_is_nonfatal_transform_status(hr))
        WARN("Failed to pull pending samples, hr %#lx.\n", hr);

    return next ? source_reader_drain_transform_samples(reader, stream, next) : S_OK;
}

static HRESULT source_reader_flush_transform_samples(struct source_reader *reader, struct media_stream *stream,
        struct transform_entry *entry)
{
    struct transform_entry *next = NULL;
    struct list *ptr;
    HRESULT hr;

    if ((ptr = list_next(&stream->transforms, &entry->entry)))
        next = LIST_ENTRY(ptr, struct transform_entry, entry);

    if (FAILED(hr = IMFTransform_ProcessMessage(entry->transform, MFT_MESSAGE_COMMAND_FLUSH, 0)))
        WARN("Failed to flush transform %p, hr %#lx\n", entry->transform, hr);

    return next ? source_reader_flush_transform_samples(reader, stream, next) : S_OK;
}

static HRESULT source_reader_notify_transform(struct source_reader *reader, struct media_stream *stream,
        struct transform_entry *entry, UINT message)
{
    struct transform_entry *next = NULL;
    struct list *ptr;
    HRESULT hr;

    if ((ptr = list_next(&stream->transforms, &entry->entry)))
        next = LIST_ENTRY(ptr, struct transform_entry, entry);

    if (FAILED(hr = IMFTransform_ProcessMessage(entry->transform, message, 0)))
        WARN("Failed to notify transform %p message %#x, hr %#lx\n", entry->transform, message, hr);

    return next ? source_reader_notify_transform(reader, stream, next, message) : S_OK;
}

static HRESULT source_reader_process_sample(struct source_reader *reader, struct media_stream *stream,
        IMFSample *sample)
{
    struct transform_entry *entry;
    struct list *ptr;
    HRESULT hr;

    if (!(ptr = list_head(&stream->transforms)))
        return source_reader_queue_sample(reader, stream, 0, sample);
    entry = LIST_ENTRY(ptr, struct transform_entry, entry);

    /* It's assumed that decoder has 1 input and 1 output, both id's are 0. */
    if (SUCCEEDED(hr = source_reader_push_transform_samples(reader, stream, entry, sample))
            || source_reader_is_nonfatal_transform_status(hr))
        hr = stream->requests ? source_reader_request_sample(reader, stream) : S_OK;
    else
    {
        WARN("Transform failed to process output, hr %#lx.\n", hr);
        if (stream->requests)
            source_reader_queue_response(reader, stream, hr, MF_SOURCE_READERF_ERROR, 0, NULL);
    }

    return hr;
}

static HRESULT source_reader_media_sample_handler(struct source_reader *reader, IMFMediaStream *stream,
        IMFMediaEvent *event)
{
    IMFSample *sample;
    unsigned int i;
    DWORD id = 0;
    HRESULT hr;

    TRACE("Got new sample for stream %p.\n", stream);

    if (FAILED(hr = media_event_get_object(event, &IID_IMFSample, (void **)&sample)))
    {
        WARN("Failed to get sample object, hr %#lx.\n", hr);
        return hr;
    }

    if (FAILED(hr = media_stream_get_id(stream, &id)))
    {
        WARN("Unidentified stream %p, hr %#lx.\n", stream, hr);
        IMFSample_Release(sample);
        return hr;
    }

    EnterCriticalSection(&reader->cs);

    for (i = 0; i < reader->stream_count; ++i)
    {
        if (id == reader->streams[i].id)
        {
            /* FIXME: propagate processing errors? */
            reader->streams[i].flags &= ~STREAM_FLAG_SAMPLE_REQUESTED;
            hr = source_reader_process_sample(reader, &reader->streams[i], sample);
            break;
        }
    }

    if (i == reader->stream_count)
        WARN("Stream with id %#lx was not present in presentation descriptor.\n", id);

    LeaveCriticalSection(&reader->cs);

    IMFSample_Release(sample);

    return hr;
}

static HRESULT source_reader_media_stream_state_handler(struct source_reader *reader, IMFMediaStream *stream,
        IMFMediaEvent *event)
{
    MediaEventType event_type;
    LONGLONG timestamp;
    PROPVARIANT value;
    struct list *ptr;
    unsigned int i;
    HRESULT hr;
    DWORD id;

    IMFMediaEvent_GetType(event, &event_type);

    if (FAILED(hr = media_stream_get_id(stream, &id)))
    {
        WARN("Unidentified stream %p, hr %#lx.\n", stream, hr);
        return hr;
    }

    EnterCriticalSection(&reader->cs);

    for (i = 0; i < reader->stream_count; ++i)
    {
        struct media_stream *stream = &reader->streams[i];

        if (id == stream->id)
        {
            switch (event_type)
            {
                case MEEndOfStream:
                    stream->state = STREAM_STATE_EOS;
                    stream->flags &= ~(STREAM_FLAG_SAMPLE_REQUESTED | STREAM_FLAG_EOS_REPORTED);

                    if ((ptr = list_head(&stream->transforms)))
                    {
                        struct transform_entry *entry = LIST_ENTRY(ptr, struct transform_entry, entry);
                        if (FAILED(hr = source_reader_drain_transform_samples(reader, stream, entry)))
                            WARN("Failed to drain pending samples, hr %#lx.\n", hr);
                    }

                    while (stream->requests)
                        source_reader_queue_response(reader, stream, S_OK, MF_SOURCE_READERF_ENDOFSTREAM, 0, NULL);

                    break;
                case MEStreamSeeked:
                    stream->state = STREAM_STATE_READY;
                    stream->flags &= ~(STREAM_FLAG_SAMPLE_REQUESTED | STREAM_FLAG_EOS_REPORTED | STREAM_FLAG_STOPPED);
                    source_reader_reset_stream_timing(stream);

                    if ((ptr = list_head(&stream->transforms)))
                    {
                        struct transform_entry *entry = LIST_ENTRY(ptr, struct transform_entry, entry);
                        if (FAILED(hr = source_reader_flush_transform_samples(reader, stream, entry)))
                            WARN("Failed to flush transforms on seek, hr %#lx.\n", hr);
                        if (FAILED(hr = source_reader_notify_transform(reader, stream, entry, MFT_MESSAGE_NOTIFY_START_OF_STREAM)))
                            WARN("Failed to notify transforms of stream start, hr %#lx.\n", hr);
                    }
                    break;
                case MEStreamStarted:
                    stream->state = STREAM_STATE_READY;
                    stream->flags &= ~(STREAM_FLAG_SAMPLE_REQUESTED | STREAM_FLAG_EOS_REPORTED | STREAM_FLAG_STOPPED);
                    source_reader_reset_stream_timing(stream);

                    if ((ptr = list_head(&stream->transforms)))
                    {
                        struct transform_entry *entry = LIST_ENTRY(ptr, struct transform_entry, entry);
                        if (FAILED(hr = source_reader_notify_transform(reader, stream, entry, MFT_MESSAGE_NOTIFY_START_OF_STREAM)))
                            WARN("Failed to notify transforms of stream start, hr %#lx.\n", hr);
                    }
                    break;
                case MEStreamStopped:
                    stream->flags |= STREAM_FLAG_STOPPED;
                    break;
                case MEStreamTick:
                    value.vt = VT_EMPTY;
                    hr = SUCCEEDED(IMFMediaEvent_GetValue(event, &value)) && value.vt == VT_I8 ? S_OK : E_UNEXPECTED;
                    timestamp = SUCCEEDED(hr) ? value.hVal.QuadPart : 0;
                    PropVariantClear(&value);

                    source_reader_queue_response(reader, stream, hr, MF_SOURCE_READERF_STREAMTICK, timestamp, NULL);

                    break;
                default:
                    ;
            }

            break;
        }
    }

    LeaveCriticalSection(&reader->cs);

    WakeAllConditionVariable(&reader->sample_event);
    if (event_type == MEStreamStopped)
        WakeAllConditionVariable(&reader->stop_event);

    return S_OK;
}

static HRESULT WINAPI source_reader_stream_events_callback_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct source_reader *reader = impl_from_stream_callback_IMFAsyncCallback(iface);
    MediaEventType event_type;
    IMFMediaStream *stream;
    IMFMediaEvent *event;
    HRESULT hr;

    TRACE("%p, %p.\n", iface, result);

    stream = (IMFMediaStream *)IMFAsyncResult_GetStateNoAddRef(result);

    if (FAILED(hr = IMFMediaStream_EndGetEvent(stream, result, &event)))
        return hr == MF_E_SHUTDOWN ? S_OK : hr;

    IMFMediaEvent_GetType(event, &event_type);

    TRACE("Got event %lu.\n", event_type);

    switch (event_type)
    {
        case MEMediaSample:
            hr = source_reader_media_sample_handler(reader, stream, event);
            break;
        case MEStreamSeeked:
        case MEStreamStarted:
        case MEStreamStopped:
        case MEStreamTick:
        case MEEndOfStream:
            hr = source_reader_media_stream_state_handler(reader, stream, event);
            break;
        default:
            ;
    }

    if (hr == MF_E_SHUTDOWN)
        TRACE("Ignoring late event %lu from shutdown stream %p.\n", event_type, stream);
    else if (FAILED(hr))
        WARN("Failed while handling %ld event, hr %#lx.\n", event_type, hr);

    IMFMediaEvent_Release(event);

    if (event_type != MEStreamStopped && hr != MF_E_SHUTDOWN)
    {
        HRESULT begin_hr;

        if (FAILED(begin_hr = IMFMediaStream_BeginGetEvent(stream, iface, (IUnknown *)stream))
                && begin_hr != MF_E_SHUTDOWN)
            WARN("Failed to subscribe to stream events, hr %#lx.\n", begin_hr);
    }

    return S_OK;
}

static const IMFAsyncCallbackVtbl stream_events_callback_vtbl =
{
    source_reader_callback_QueryInterface,
    source_reader_stream_events_callback_AddRef,
    source_reader_stream_events_callback_Release,
    source_reader_stream_events_callback_GetParameters,
    source_reader_stream_events_callback_Invoke,
};

static ULONG WINAPI source_reader_async_commands_callback_AddRef(IMFAsyncCallback *iface)
{
    struct source_reader *reader = impl_from_async_commands_callback_IMFAsyncCallback(iface);
    return source_reader_addref(reader);
}

static ULONG WINAPI source_reader_async_commands_callback_Release(IMFAsyncCallback *iface)
{
    struct source_reader *reader = impl_from_async_commands_callback_IMFAsyncCallback(iface);
    return source_reader_release(reader);
}

static struct stream_response * media_stream_detach_response(struct source_reader *reader, struct stream_response *response)
{
    struct media_stream *stream;

    list_remove(&response->entry);

    if (response->stream_index < reader->stream_count)
    {
        stream = &reader->streams[response->stream_index];
        if (stream->responses)
            --stream->responses;
    }

    return response;
}

static struct stream_response *media_stream_pop_response(struct source_reader *reader, struct media_stream *stream)
{
    struct stream_response *response;

    LIST_FOR_EACH_ENTRY(response, &reader->responses, struct stream_response, entry)
    {
        if (stream && response->stream_index != stream->index)
            continue;

        if (!stream) stream = &reader->streams[response->stream_index];

        return media_stream_detach_response(reader, response);
    }

    return NULL;
}

static void source_reader_release_response(struct stream_response *response)
{
    if (response->sample)
        IMFSample_Release(response->sample);
    free(response);
}

static HRESULT source_reader_get_stream_selection(const struct source_reader *reader, DWORD index, BOOL *selected)
{
    IMFStreamDescriptor *sd;

    if (FAILED(IMFPresentationDescriptor_GetStreamDescriptorByIndex(reader->descriptor, index, selected, &sd)))
        return MF_E_INVALIDSTREAMNUMBER;
    IMFStreamDescriptor_Release(sd);

    return S_OK;
}

static BOOL source_reader_selected_streams_eos(struct source_reader *reader);
static HRESULT source_reader_get_next_selected_stream(struct source_reader *reader, DWORD *stream_index);

static struct stream_response *media_stream_pop_response_any_selected(struct source_reader *reader)
{
    struct stream_response *response, *best = NULL, *best_eos = NULL;
    BOOL selected;

    LIST_FOR_EACH_ENTRY(response, &reader->responses, struct stream_response, entry)
    {
        if (response->stream_index >= reader->stream_count)
            continue;
        if (FAILED(source_reader_get_stream_selection(reader, response->stream_index, &selected)) || !selected)
            continue;

        if ((response->stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) && !response->sample)
        {
            if (!best_eos)
                best_eos = response;
            continue;
        }

        if (!best || response->timestamp < best->timestamp)
            best = response;
    }

    if (best)
        return media_stream_detach_response(reader, best);
    if (best_eos && (reader->flag_eos_for_all_streams || source_reader_selected_streams_eos(reader)))
        return media_stream_detach_response(reader, best_eos);
    return NULL;
}

static HRESULT source_reader_start_source(struct source_reader *reader)
{
    BOOL selected, selection_changed = FALSE;
    PROPVARIANT position;
    HRESULT hr = S_OK;
    unsigned int i;

    for (i = 0; i < reader->stream_count; ++i)
    {
        source_reader_get_stream_selection(reader, i, &selected);
        if (selected)
            reader->streams[i].flags |= STREAM_FLAG_SELECTED;
        else
            reader->streams[i].flags &= ~STREAM_FLAG_SELECTED;
    }

    if (reader->source_state == SOURCE_STATE_STARTED)
    {
        for (i = 0; i < reader->stream_count; ++i)
        {
            selection_changed = !!(reader->streams[i].flags & STREAM_FLAG_SELECTED) ^
                    !!(reader->streams[i].flags & STREAM_FLAG_PRESENTED);
            if (selection_changed)
                break;
        }
    }

    position.hVal.QuadPart = 0;
    if (reader->source_state != SOURCE_STATE_STARTED || selection_changed)
    {
        position.vt = reader->source_state == SOURCE_STATE_STARTED ? VT_EMPTY : VT_I8;
        /* Update cached stream selection if descriptor was accepted. */
        if (SUCCEEDED(hr = IMFMediaSource_Start(reader->source, reader->descriptor, &GUID_NULL, &position)))
        {
            reader->presentation_ended = FALSE;
            reader->next_stream_eos_index = 0;
            for (i = 0; i < reader->stream_count; ++i)
            {
                if (reader->streams[i].flags & STREAM_FLAG_SELECTED)
                    reader->streams[i].flags |= STREAM_FLAG_PRESENTED;
            }
        }
    }

    return hr;
}

static BOOL source_reader_got_response_for_stream(struct source_reader *reader, struct media_stream *stream)
{
    struct stream_response *response;

    LIST_FOR_EACH_ENTRY(response, &reader->responses, struct stream_response, entry)
    {
        if (response->stream_index == stream->index)
            return TRUE;
    }

    return FALSE;
}

static struct stream_response *source_reader_peek_response_for_stream(struct source_reader *reader,
        struct media_stream *stream)
{
    struct stream_response *response;

    LIST_FOR_EACH_ENTRY(response, &reader->responses, struct stream_response, entry)
    {
        if (response->stream_index == stream->index)
            return response;
    }

    return NULL;
}

static struct media_stream *source_reader_get_selected_audio_stream(struct source_reader *reader)
{
    unsigned int i;
    BOOL selected;

    for (i = 0; i < reader->stream_count; ++i)
    {
        if (FAILED(source_reader_get_stream_selection(reader, i, &selected)) || !selected)
            continue;
        if (source_reader_stream_is_audio(&reader->streams[i]))
            return &reader->streams[i];
    }

    return NULL;
}

static void source_reader_request_audio_for_video_sync(struct source_reader *reader, struct media_stream *stream)
{
    const LONGLONG max_video_lead = 2000000; /* 200 ms in 100ns units. */
    struct stream_response *response;
    struct media_stream *audio;
    LONGLONG audio_lead;

    if (!reader->flag_eos_for_all_streams || reader->async_callback || !source_reader_stream_is_video(stream))
        return;
    if (!(response = source_reader_peek_response_for_stream(reader, stream)) || !response->sample)
        return;

    if ((audio = source_reader_get_selected_audio_stream(reader)) && audio->state != STREAM_STATE_EOS
            && audio->unthrottled_audio_output)
    {
        audio_lead = response->timestamp - audio->delivered_sample_ts;
        if (audio_lead > max_video_lead
                && !source_reader_got_response_for_stream(reader, audio)
                && !(audio->flags & STREAM_FLAG_SAMPLE_REQUESTED))
        {
            audio->requests++;
            if (FAILED(source_reader_request_sample(reader, audio)) && audio->requests)
                audio->requests--;
        }
    }
}

static void source_reader_prefetch_stream_sample(struct source_reader *reader, struct media_stream *stream, DWORD flags)
{
    unsigned int max_responses;

    if (!reader->flag_eos_for_all_streams || reader->async_callback)
        return;
    if ((flags & MF_SOURCE_READER_CONTROLF_DRAIN) || stream->state == STREAM_STATE_EOS)
        return;
    if (source_reader_stream_is_audio(stream))
        max_responses = 8;
    else if (source_reader_stream_is_video(stream))
        max_responses = 2;
    else
        return;
    if ((stream->flags & STREAM_FLAG_SAMPLE_REQUESTED) || stream->responses >= max_responses)
        return;

    stream->requests++;
    if (FAILED(source_reader_request_sample(reader, stream)) && stream->requests)
        stream->requests--;
}

static BOOL source_reader_got_response_for_any_selected_stream(struct source_reader *reader)
{
    struct stream_response *response;
    BOOL selected;

    LIST_FOR_EACH_ENTRY(response, &reader->responses, struct stream_response, entry)
    {
        if (response->stream_index >= reader->stream_count)
            continue;
        if (SUCCEEDED(source_reader_get_stream_selection(reader, response->stream_index, &selected)) && selected)
        {
            if ((response->stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) && !response->sample
                    && !reader->flag_eos_for_all_streams && !source_reader_selected_streams_eos(reader))
                continue;
            return TRUE;
        }
    }

    return FALSE;
}

static BOOL source_reader_selected_streams_eos(struct source_reader *reader)
{
    unsigned int i;
    BOOL selected;

    for (i = 0; i < reader->stream_count; ++i)
    {
        if (reader->flag_eos_for_all_streams && reader->presentation_ended)
            continue;
        if (SUCCEEDED(source_reader_get_stream_selection(reader, i, &selected)) && selected
                && reader->streams[i].state != STREAM_STATE_EOS)
            return FALSE;
    }

    return TRUE;
}

static HRESULT source_reader_get_next_selected_eos_stream(struct source_reader *reader, DWORD *stream_index)
{
    unsigned int i, start;
    BOOL selected;

    start = reader->next_stream_eos_index;
    for (i = 0; i < reader->stream_count; ++i)
    {
        DWORD index = (start + i) % reader->stream_count;

        if (SUCCEEDED(source_reader_get_stream_selection(reader, index, &selected)) && selected)
        {
            *stream_index = index;
            reader->next_stream_eos_index = index + 1;
            return S_OK;
        }
    }

    return MF_E_MEDIA_SOURCE_NO_STREAMS_SELECTED;
}

static HRESULT source_reader_get_next_unreported_selected_eos_stream(struct source_reader *reader, DWORD *stream_index)
{
    unsigned int i, start;
    BOOL selected;

    start = reader->next_stream_eos_index;
    for (i = 0; i < reader->stream_count; ++i)
    {
        DWORD index = (start + i) % reader->stream_count;
        struct media_stream *stream = &reader->streams[index];

        if (FAILED(source_reader_get_stream_selection(reader, index, &selected)) || !selected)
            continue;
        if (stream->state != STREAM_STATE_EOS && !(reader->flag_eos_for_all_streams && reader->presentation_ended))
            continue;
        if (stream->flags & STREAM_FLAG_EOS_REPORTED)
            continue;

        *stream_index = index;
        stream->flags |= STREAM_FLAG_EOS_REPORTED;
        reader->next_stream_eos_index = index + 1;
        return S_OK;
    }

    return MF_E_MEDIA_SOURCE_NO_STREAMS_SELECTED;
}

static HRESULT source_reader_request_any_selected_stream_samples(struct source_reader *reader)
{
    unsigned int i;
    BOOL selected;
    HRESULT hr = S_OK, tmp_hr;

    for (i = 0; i < reader->stream_count; ++i)
    {
        struct media_stream *stream = &reader->streams[i];

        if (FAILED(source_reader_get_stream_selection(reader, i, &selected)) || !selected)
            continue;
        if (stream->state == STREAM_STATE_EOS)
            continue;
        if ((stream->flags & STREAM_FLAG_SAMPLE_REQUESTED) || stream->responses)
            continue;

        stream->requests++;
        if (FAILED(tmp_hr = source_reader_request_sample(reader, stream)) && SUCCEEDED(hr))
            hr = tmp_hr;
    }

    return hr;
}

static HRESULT source_reader_request_next_selected_stream_sample(struct source_reader *reader)
{
    struct media_stream *stream;
    DWORD stream_index = MF_SOURCE_READER_INVALID_STREAM_INDEX;
    HRESULT hr;

    if (FAILED(hr = source_reader_get_next_selected_stream(reader, &stream_index)))
        return hr;

    if (stream_index >= reader->stream_count)
        return MF_E_INVALIDSTREAMNUMBER;

    stream = &reader->streams[stream_index];
    if (stream->state == STREAM_STATE_EOS)
        return S_OK;
    if ((stream->flags & STREAM_FLAG_SAMPLE_REQUESTED) || stream->responses)
        return S_OK;

    stream->requests++;
    return source_reader_request_sample(reader, stream);
}

static HRESULT source_reader_request_selected_stream_type_sample(struct source_reader *reader, const GUID *major_type,
        BOOL *handled, BOOL *requested)
{
    const unsigned int max_audio_responses = 8;
    unsigned int i;
    HRESULT hr;

    *handled = FALSE;
    *requested = FALSE;

    for (i = 0; i < reader->stream_count; ++i)
    {
        struct media_stream *stream = &reader->streams[i];
        GUID stream_major_type;
        BOOL selected;

        if (FAILED(source_reader_get_stream_selection(reader, i, &selected)) || !selected)
            continue;
        if (stream->state == STREAM_STATE_EOS)
            continue;
        if (FAILED(IMFMediaType_GetMajorType(stream->current, &stream_major_type))
                || !IsEqualGUID(&stream_major_type, major_type))
            continue;

        *handled = TRUE;
        if ((stream->flags & STREAM_FLAG_SAMPLE_REQUESTED)
                || (stream->responses
                && (!IsEqualGUID(major_type, &MFMediaType_Audio)
                || stream->responses >= max_audio_responses)))
            return S_OK;

        stream->requests++;
        *requested = TRUE;
        if (FAILED(hr = source_reader_request_sample(reader, stream)))
            return hr;
        return S_OK;
    }

    return S_OK;
}

static HRESULT source_reader_request_windows_media_stream_sample(struct source_reader *reader)
{
    BOOL handled, requested;
    HRESULT hr;

    if (FAILED(hr = source_reader_request_selected_stream_type_sample(reader, &MFMediaType_Audio, &handled,
            &requested)) || requested)
        return hr;

    if (FAILED(hr = source_reader_request_selected_stream_type_sample(reader, &MFMediaType_Video, &handled,
            &requested)) || handled)
        return hr;

    return source_reader_request_next_selected_stream_sample(reader);
}

static BOOL source_reader_get_read_result(struct source_reader *reader, struct media_stream *stream, DWORD flags,
        HRESULT *status, DWORD *stream_index, DWORD *stream_flags, LONGLONG *timestamp, IMFSample **sample)
{
    struct stream_response *response = NULL;
    BOOL request_sample = FALSE;

    while ((response = media_stream_pop_response(reader, stream)))
    {
        *status = response->status;
        *stream_index = stream->index;
        *stream_flags = response->stream_flags;
        *timestamp = response->timestamp;
        *sample = response->sample;
        if (*sample)
            IMFSample_AddRef(*sample);
        if (*sample)
        {
            stream->delivered_sample_ts = *timestamp;
            source_reader_retain_delivered_sample(stream, *sample);
        }

        if ((*stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) && !*sample)
            stream->flags |= STREAM_FLAG_EOS_REPORTED;

        source_reader_release_response(response);
        break;
    }

    if (!response)
    {
        *status = S_OK;
        *stream_index = stream->index;
        *timestamp = 0;
        *sample = NULL;

        if (stream->state == STREAM_STATE_EOS)
        {
            *stream_flags = MF_SOURCE_READERF_ENDOFSTREAM;
            stream->flags |= STREAM_FLAG_EOS_REPORTED;
        }
        else
        {
            request_sample = !(flags & MF_SOURCE_READER_CONTROLF_DRAIN);
            *stream_flags = 0;
        }
    }

    return !request_sample;
}

static BOOL source_reader_get_read_result_any(struct source_reader *reader, DWORD flags, HRESULT *status,
        DWORD *stream_index, DWORD *stream_flags, LONGLONG *timestamp, IMFSample **sample)
{
    struct stream_response *response;
    BOOL request_sample = FALSE;

    if ((response = media_stream_pop_response_any_selected(reader)))
    {
        *status = response->status;
        *stream_index = response->stream_index;
        *stream_flags = response->stream_flags;
        *timestamp = response->timestamp;
        *sample = response->sample;
        if (*sample)
            IMFSample_AddRef(*sample);
        if (*sample)
        {
            reader->streams[*stream_index].delivered_sample_ts = *timestamp;
            source_reader_retain_delivered_sample(&reader->streams[*stream_index], *sample);
        }

        if ((*stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) && !*sample)
            reader->streams[*stream_index].flags |= STREAM_FLAG_EOS_REPORTED;

        source_reader_release_response(response);
    }
    else
    {
        *status = S_OK;
        *stream_index = MF_SOURCE_READER_ANY_STREAM;
        *timestamp = 0;
        *sample = NULL;

        if (reader->flag_eos_for_all_streams
                && SUCCEEDED(source_reader_get_next_unreported_selected_eos_stream(reader, stream_index)))
        {
            *stream_flags = MF_SOURCE_READERF_ENDOFSTREAM;
        }
        else if (source_reader_selected_streams_eos(reader))
        {
            *stream_flags = MF_SOURCE_READERF_ENDOFSTREAM;
            if (reader->flag_eos_for_all_streams)
                source_reader_get_next_selected_eos_stream(reader, stream_index);
        }
        else
        {
            request_sample = !(flags & MF_SOURCE_READER_CONTROLF_DRAIN);
            *stream_flags = 0;
        }
    }

    return !request_sample;
}

static HRESULT source_reader_get_next_selected_stream(struct source_reader *reader, DWORD *stream_index)
{
    unsigned int i, first_selected = ~0u;
    BOOL selected, stream_drained;
    LONGLONG min_ts = MAXLONGLONG;
    HRESULT hr = S_OK;

    for (i = 0; i < reader->stream_count; ++i)
    {
        stream_drained = reader->streams[i].state == STREAM_STATE_EOS && !reader->streams[i].responses;
        selected = SUCCEEDED(source_reader_get_stream_selection(reader, i, &selected)) && selected;

        if (selected)
        {
            if (first_selected == ~0u)
                first_selected = i;

            /* Pick the stream whose last delivered sample had the lowest timestamp. */
            if (!stream_drained && reader->streams[i].delivered_sample_ts < min_ts)
            {
                min_ts = reader->streams[i].delivered_sample_ts;
                *stream_index = i;
            }
        }
    }

    /* If all selected streams reached EOS, use first selected. */
    if (first_selected != ~0u && min_ts == MAXLONGLONG)
    {
        if (reader->flag_eos_for_all_streams)
            hr = source_reader_get_next_selected_eos_stream(reader, stream_index);
        else
            *stream_index = first_selected;
    }

    if (first_selected == ~0u)
        return MF_E_MEDIA_SOURCE_NO_STREAMS_SELECTED;

    return min_ts == MAXLONGLONG && FAILED(hr) ? hr : S_OK;
}

static HRESULT source_reader_request_selected_stream_samples(struct source_reader *reader)
{
    if (reader->flag_eos_for_all_streams && reader->presentation_ended)
        return S_OK;

    if (reader->flag_eos_for_all_streams)
        return source_reader_request_windows_media_stream_sample(reader);

    return source_reader_request_any_selected_stream_samples(reader);
}

static HRESULT source_reader_get_stream_read_index(struct source_reader *reader, unsigned int index, DWORD *stream_index)
{
    BOOL selected;
    HRESULT hr;

    switch (index)
    {
        case MF_SOURCE_READER_FIRST_VIDEO_STREAM:
            *stream_index = reader->first_video_stream_index;
            break;
        case MF_SOURCE_READER_FIRST_AUDIO_STREAM:
            *stream_index = reader->first_audio_stream_index;
            break;
        case MF_SOURCE_READER_ANY_STREAM:
            return source_reader_get_next_selected_stream(reader, stream_index);
        default:
            *stream_index = index;
    }

    /* Can't read from deselected streams. */
    if (SUCCEEDED(hr = source_reader_get_stream_selection(reader, *stream_index, &selected)) && !selected)
        hr = MF_E_INVALIDREQUEST;

    return hr;
}

static void source_reader_release_responses(struct source_reader *reader, struct media_stream *stream)
{
    struct stream_response *ptr, *next;

    LIST_FOR_EACH_ENTRY_SAFE(ptr, next, &reader->responses, struct stream_response, entry)
    {
        if (stream && stream->index != ptr->stream_index &&
                ptr->stream_index != MF_SOURCE_READER_FIRST_VIDEO_STREAM &&
                ptr->stream_index != MF_SOURCE_READER_FIRST_AUDIO_STREAM &&
                ptr->stream_index != MF_SOURCE_READER_ANY_STREAM)
        {
            continue;
        }
        media_stream_detach_response(reader, ptr);
        source_reader_release_response(ptr);
    }
}

static void source_reader_flush_stream(struct source_reader *reader, DWORD stream_index)
{
    struct media_stream *stream = &reader->streams[stream_index];
    struct list *ptr;
    HRESULT hr;

    source_reader_release_responses(reader, stream);

    if ((ptr = list_head(&stream->transforms)))
    {
        struct transform_entry *entry = LIST_ENTRY(ptr, struct transform_entry, entry);
        if (FAILED(hr = source_reader_flush_transform_samples(reader, stream, entry)))
            WARN("Failed to drain pending samples, hr %#lx.\n", hr);
    }

    media_stream_release_retained_audio_samples(stream);
    stream->requests = 0;
    stream->flags &= ~(STREAM_FLAG_SAMPLE_REQUESTED | STREAM_FLAG_EOS_REPORTED);
}

static HRESULT source_reader_flush(struct source_reader *reader, unsigned int index)
{
    unsigned int stream_index;
    HRESULT hr = S_OK;

    if (index == MF_SOURCE_READER_ALL_STREAMS)
    {
        for (stream_index = 0; stream_index < reader->stream_count; ++stream_index)
            source_reader_flush_stream(reader, stream_index);
    }
    else
    {
        switch (index)
        {
            case MF_SOURCE_READER_FIRST_VIDEO_STREAM:
                stream_index = reader->first_video_stream_index;
                break;
            case MF_SOURCE_READER_FIRST_AUDIO_STREAM:
                stream_index = reader->first_audio_stream_index;
                break;
            default:
                stream_index = index;
        }

        if (stream_index < reader->stream_count)
            source_reader_flush_stream(reader, stream_index);
        else
            hr = MF_E_INVALIDSTREAMNUMBER;
    }

    return hr;
}

static HRESULT WINAPI source_reader_async_commands_callback_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct source_reader *reader = impl_from_async_commands_callback_IMFAsyncCallback(iface);
    struct media_stream *stream, stub_stream = { .requests = 1 };
    struct source_reader_async_command *command;
    struct stream_response *response;
    DWORD stream_index, stream_flags;
    BOOL report_sample = FALSE;
    IMFSample *sample = NULL;
    LONGLONG timestamp = 0;
    HRESULT hr, status;
    IUnknown *state;

    if (FAILED(hr = IMFAsyncResult_GetState(result, &state)))
        return hr;

    command = impl_from_async_command_IUnknown(state);

    switch (command->op)
    {
        case SOURCE_READER_ASYNC_READ:
            EnterCriticalSection(&reader->cs);

            if (SUCCEEDED(hr = source_reader_start_source(reader)))
            {
                if (command->u.read.stream_index == MF_SOURCE_READER_ANY_STREAM)
                {
                    report_sample = source_reader_get_read_result_any(reader, command->u.read.flags, &status,
                            &stream_index, &stream_flags, &timestamp, &sample);
                    if (FAILED(source_reader_request_selected_stream_samples(reader)))
                        WARN("Failed to request a sample, hr %#lx.\n", hr);
                }
                else if (SUCCEEDED(hr = source_reader_get_stream_read_index(reader, command->u.read.stream_index, &stream_index)))
                {
                    stream = &reader->streams[stream_index];

                    if (!(report_sample = source_reader_get_read_result(reader, stream, command->u.read.flags, &status,
                            &stream_index, &stream_flags, &timestamp, &sample)))
                    {
                        stream->requests++;
                        source_reader_request_sample(reader, stream);
                        /* FIXME: set error stream/reader state on request failure */
                    }
                }
                else
                {
                    stub_stream.index = command->u.read.stream_index;
                    source_reader_queue_response(reader, &stub_stream, hr, MF_SOURCE_READERF_ERROR, 0, NULL);
                }
            }

            LeaveCriticalSection(&reader->cs);

            if (report_sample)
                IMFSourceReaderCallback_OnReadSample(reader->async_callback, status, stream_index, stream_flags,
                        timestamp, sample);

            if (sample)
                IMFSample_Release(sample);

            break;

        case SOURCE_READER_ASYNC_SEEK:

            EnterCriticalSection(&reader->cs);
            reader->flags &= ~SOURCE_READER_ASYNC_SEEK_QUEUED;
            ++reader->seek_serial;
            reader->flags |= SOURCE_READER_SEEKING;
            if (FAILED(IMFMediaSource_Start(reader->source, reader->descriptor, &command->u.seek.format,
                    &command->u.seek.position)))
            {
                reader->completed_seek_serial = reader->seek_serial;
                reader->flags &= ~SOURCE_READER_SEEKING;
            }
            LeaveCriticalSection(&reader->cs);

            break;

        case SOURCE_READER_ASYNC_SAMPLE_READY:

            EnterCriticalSection(&reader->cs);
            stream = &reader->streams[command->u.sample.stream_index];
            response = media_stream_pop_response(reader, stream);
            LeaveCriticalSection(&reader->cs);

            if (response)
            {
                IMFSourceReaderCallback_OnReadSample(reader->async_callback, response->status, response->stream_index,
                        response->stream_flags, response->timestamp, response->sample);
                source_reader_release_response(response);
            }

            break;
        case SOURCE_READER_ASYNC_FLUSH:
            EnterCriticalSection(&reader->cs);
            source_reader_flush(reader, command->u.flush.stream_index);
            reader->flags &= ~SOURCE_READER_FLUSHING;
            LeaveCriticalSection(&reader->cs);

            IMFSourceReaderCallback_OnFlush(reader->async_callback, command->u.flush.stream_index);
            break;
        default:
            ;
    }

    IUnknown_Release(state);

    return S_OK;
}

static const IMFAsyncCallbackVtbl async_commands_callback_vtbl =
{
    source_reader_callback_QueryInterface,
    source_reader_async_commands_callback_AddRef,
    source_reader_async_commands_callback_Release,
    source_reader_callback_GetParameters,
    source_reader_async_commands_callback_Invoke,
};

static HRESULT WINAPI src_reader_QueryInterface(IMFSourceReaderEx *iface, REFIID riid, void **out)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);

    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), out);

    if (IsEqualGUID(riid, &IID_IUnknown)
            || IsEqualGUID(riid, &IID_IMFSourceReader)
            || IsEqualGUID(riid, &IID_IMFSourceReaderEx))
    {
        *out = &reader->IMFSourceReaderEx_iface;
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

static ULONG WINAPI src_reader_AddRef(IMFSourceReaderEx *iface)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);
    ULONG refcount = InterlockedIncrement(&reader->public_refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    return refcount;
}

static BOOL source_reader_is_source_stopped(const struct source_reader *reader)
{
    unsigned int i;

    if (reader->source_state != SOURCE_STATE_STOPPED)
        return FALSE;

    for (i = 0; i < reader->stream_count; ++i)
    {
        if (reader->streams[i].stream && !(reader->streams[i].flags & STREAM_FLAG_STOPPED))
            return FALSE;
    }

    return TRUE;
}

static ULONG WINAPI src_reader_Release(IMFSourceReaderEx *iface)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);
    ULONG refcount = InterlockedDecrement(&reader->public_refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        if (reader->flags & SOURCE_READER_SHUTDOWN_ON_RELEASE)
            IMFMediaSource_Shutdown(reader->source);
        else if (SUCCEEDED(IMFMediaSource_Stop(reader->source)))
        {
            EnterCriticalSection(&reader->cs);

            while (!source_reader_is_source_stopped(reader))
            {
                SleepConditionVariableCS(&reader->stop_event, &reader->cs, INFINITE);
            }

            LeaveCriticalSection(&reader->cs);
        }

        source_reader_release(reader);
    }

    return refcount;
}

static HRESULT WINAPI src_reader_GetStreamSelection(IMFSourceReaderEx *iface, DWORD index, BOOL *selected)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);

    TRACE("%p, %#lx, %p.\n", iface, index, selected);

    switch (index)
    {
        case MF_SOURCE_READER_FIRST_VIDEO_STREAM:
            index = reader->first_video_stream_index;
            break;
        case MF_SOURCE_READER_FIRST_AUDIO_STREAM:
            index = reader->first_audio_stream_index;
            break;
        default:
            ;
    }

    return source_reader_get_stream_selection(reader, index, selected);
}

static HRESULT WINAPI src_reader_SetStreamSelection(IMFSourceReaderEx *iface, DWORD index, BOOL selection)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);
    HRESULT hr = S_OK;
    BOOL selection_changed = FALSE, selected;
    unsigned int i;

    TRACE("%p, %#lx, %d.\n", iface, index, selection);

    selection = !!selection;

    EnterCriticalSection(&reader->cs);

    if (index == MF_SOURCE_READER_ALL_STREAMS)
    {
        for (i = 0; i < reader->stream_count; ++i)
        {
            if (!selection_changed)
            {
                source_reader_get_stream_selection(reader, i, &selected);
                selection_changed = !!(selected ^ selection);
            }

            if (selection)
                IMFPresentationDescriptor_SelectStream(reader->descriptor, i);
            else
                IMFPresentationDescriptor_DeselectStream(reader->descriptor, i);
        }
    }
    else
    {
        switch (index)
        {
            case MF_SOURCE_READER_FIRST_VIDEO_STREAM:
                index = reader->first_video_stream_index;
                break;
            case MF_SOURCE_READER_FIRST_AUDIO_STREAM:
                index = reader->first_audio_stream_index;
                break;
            default:
                ;
        }

        source_reader_get_stream_selection(reader, index, &selected);
        selection_changed = !!(selected ^ selection);

        if (selection)
            hr = IMFPresentationDescriptor_SelectStream(reader->descriptor, index);
        else
            hr = IMFPresentationDescriptor_DeselectStream(reader->descriptor, index);
    }

    if (selection_changed)
    {
        for (i = 0; i < reader->stream_count; ++i)
        {
            source_reader_reset_stream_timing(&reader->streams[i]);
        }
    }

    LeaveCriticalSection(&reader->cs);

    return SUCCEEDED(hr) ? S_OK : MF_E_INVALIDSTREAMNUMBER;
}

static HRESULT source_reader_get_native_media_type(struct source_reader *reader, DWORD index, DWORD type_index,
        IMFMediaType **type)
{
    IMFMediaTypeHandler *handler;
    IMFStreamDescriptor *sd;
    IMFMediaType *src_type;
    BOOL selected;
    HRESULT hr;

    switch (index)
    {
        case MF_SOURCE_READER_FIRST_VIDEO_STREAM:
            index = reader->first_video_stream_index;
            break;
        case MF_SOURCE_READER_FIRST_AUDIO_STREAM:
            index = reader->first_audio_stream_index;
            break;
        default:
            ;
    }

    if (FAILED(IMFPresentationDescriptor_GetStreamDescriptorByIndex(reader->descriptor, index, &selected, &sd)))
        return MF_E_INVALIDSTREAMNUMBER;

    hr = IMFStreamDescriptor_GetMediaTypeHandler(sd, &handler);
    IMFStreamDescriptor_Release(sd);
    if (FAILED(hr))
        return hr;

    if (type_index == MF_SOURCE_READER_CURRENT_TYPE_INDEX)
        hr = IMFMediaTypeHandler_GetCurrentMediaType(handler, &src_type);
    else
        hr = IMFMediaTypeHandler_GetMediaTypeByIndex(handler, type_index, &src_type);
    IMFMediaTypeHandler_Release(handler);

    if (SUCCEEDED(hr))
    {
        if (SUCCEEDED(hr = MFCreateMediaType(type)))
            hr = IMFMediaType_CopyAllItems(src_type, (IMFAttributes *)*type);
        IMFMediaType_Release(src_type);
    }

    return hr;
}

static void mediatype_get_stride_and_sample_size(IMFMediaType *mediatype, LONG *stride, DWORD *sample_size, HRESULT *hr);

static HRESULT WINAPI src_reader_GetNativeMediaType(IMFSourceReaderEx *iface, DWORD index, DWORD type_index,
            IMFMediaType **type)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);
    IMFMediaType *native_type;
    GUID major, subtype;
    BOOL compressed;
    HRESULT hr;

    TRACE("%p, %#lx, %#lx, %p.\n", iface, index, type_index, type);

    if (!type)
        return E_POINTER;
    *type = NULL;

    if (type_index == MF_SOURCE_READER_CURRENT_TYPE_INDEX)
        return source_reader_get_native_media_type(reader, index, type_index, type);

    if (FAILED(hr = source_reader_get_native_media_type(reader, index, 0, &native_type)))
        return hr;

    if (SUCCEEDED(IMFMediaType_GetMajorType(native_type, &major))
            && IsEqualGUID(&major, &MFMediaType_Video)
            && SUCCEEDED(IMFMediaType_GetGUID(native_type, &MF_MT_SUBTYPE, &subtype))
            && (IsEqualGUID(&subtype, &MFVideoFormat_H264) || IsEqualGUID(&subtype, &MFVideoFormat_H264_ES))
            && SUCCEEDED(IMFMediaType_IsCompressedFormat(native_type, &compressed)) && compressed)
    {
        LONG stride;
        DWORD sample_size;

        if (type_index)
        {
            IMFMediaType_Release(native_type);
            return source_reader_get_native_media_type(reader, index, type_index - 1, type);
        }

        hr = MFCreateMediaType(type);
        if (SUCCEEDED(hr))
            hr = IMFMediaType_CopyAllItems(native_type, (IMFAttributes *)*type);
        if (SUCCEEDED(hr))
            hr = IMFMediaType_SetGUID(*type, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
        if (SUCCEEDED(hr))
            hr = IMFMediaType_SetUINT32(*type, &MF_MT_COMPRESSED, FALSE);
        if (SUCCEEDED(hr))
        {
            mediatype_get_stride_and_sample_size(*type, &stride, &sample_size, &hr);
            if (SUCCEEDED(hr))
                hr = IMFMediaType_SetUINT32(*type, &MF_MT_DEFAULT_STRIDE, abs(stride));
            if (SUCCEEDED(hr))
                hr = IMFMediaType_SetUINT32(*type, &MF_MT_SAMPLE_SIZE, sample_size);
        }
        if (SUCCEEDED(hr))
        {
            IMFMediaType_DeleteItem(*type, &MF_MT_MPEG_SEQUENCE_HEADER);
            IMFMediaType_Release(native_type);
            return S_OK;
        }

        if (*type)
            IMFMediaType_Release(*type);
        *type = NULL;
    }

    IMFMediaType_Release(native_type);
    return source_reader_get_native_media_type(reader, index, type_index, type);
}

static HRESULT WINAPI src_reader_GetCurrentMediaType(IMFSourceReaderEx *iface, DWORD index, IMFMediaType **type)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);
    HRESULT hr;

    TRACE("%p, %#lx, %p.\n", iface, index, type);

    switch (index)
    {
        case MF_SOURCE_READER_FIRST_VIDEO_STREAM:
            index = reader->first_video_stream_index;
            break;
        case MF_SOURCE_READER_FIRST_AUDIO_STREAM:
            index = reader->first_audio_stream_index;
            break;
        default:
            ;
    }

    if (index >= reader->stream_count)
        return MF_E_INVALIDSTREAMNUMBER;

    if (FAILED(hr = MFCreateMediaType(type)))
        return hr;

    EnterCriticalSection(&reader->cs);

    hr = IMFMediaType_CopyAllItems(reader->streams[index].current, (IMFAttributes *)*type);

    LeaveCriticalSection(&reader->cs);

    return hr;
}

static HRESULT source_reader_get_source_type_handler(struct source_reader *reader, DWORD index,
        IMFMediaTypeHandler **handler)
{
    IMFStreamDescriptor *sd;
    BOOL selected;
    HRESULT hr;

    if (FAILED(hr = IMFPresentationDescriptor_GetStreamDescriptorByIndex(reader->descriptor, index, &selected, &sd)))
        return hr;

    hr = IMFStreamDescriptor_GetMediaTypeHandler(sd, handler);
    IMFStreamDescriptor_Release(sd);

    return hr;
}

static HRESULT source_reader_set_compatible_media_type(struct source_reader *reader, DWORD index, IMFMediaType *type)
{
    struct media_stream *stream = &reader->streams[index];
    struct transform_entry *entry, *next;
    IMFMediaTypeHandler *type_handler;
    IMFMediaType *native_type;
    BOOL type_set = FALSE;
    unsigned int i = 0;
    DWORD flags;
    HRESULT hr;

    if (FAILED(hr = IMFMediaType_IsEqual(type, stream->current, &flags)))
        return hr;

    if (!(flags & MF_MEDIATYPE_EQUAL_MAJOR_TYPES))
        return MF_E_INVALIDMEDIATYPE;

    /* No need for a decoder or type change. */
    if (flags & MF_MEDIATYPE_EQUAL_FORMAT_DATA)
        return S_OK;

    if (stream->transform_service)
    {
        IMFTransform_Release(stream->transform_service);
        stream->transform_service = NULL;
    }
    LIST_FOR_EACH_ENTRY_SAFE(entry, next, &stream->transforms, struct transform_entry, entry)
    {
        list_remove(&entry->entry);
        transform_entry_destroy(entry);
    }

    if (FAILED(hr = source_reader_get_source_type_handler(reader, index, &type_handler)))
        return hr;

    while (!type_set && IMFMediaTypeHandler_GetMediaTypeByIndex(type_handler, i++, &native_type) == S_OK)
    {
        static const DWORD compare_flags = MF_MEDIATYPE_EQUAL_MAJOR_TYPES | MF_MEDIATYPE_EQUAL_FORMAT_DATA;

        if (SUCCEEDED(IMFMediaType_IsEqual(native_type, type, &flags)) && (flags & compare_flags) == compare_flags)
        {
            if ((type_set = SUCCEEDED(IMFMediaTypeHandler_SetCurrentMediaType(type_handler, native_type))))
                IMFMediaType_CopyAllItems(native_type, (IMFAttributes *)stream->current);
        }

        IMFMediaType_Release(native_type);
    }

    IMFMediaTypeHandler_Release(type_handler);

    return type_set ? S_OK : S_FALSE;
}

static BOOL source_reader_allow_video_processor(struct source_reader *reader, BOOL *advanced)
{
    UINT32 value;

    *advanced = FALSE;
    if (!reader->attributes)
        return FALSE;

    if (SUCCEEDED(IMFAttributes_GetUINT32(reader->attributes, &MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, &value)))
        *advanced = value;
    if (SUCCEEDED(IMFAttributes_GetUINT32(reader->attributes, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, &value)))
        return value || *advanced;

    return *advanced;
}

static void mediatype_set_uint32(IMFMediaType *mediatype, const GUID *attr, unsigned int value, HRESULT *hr)
{
    if (SUCCEEDED(*hr))
        *hr = IMFMediaType_SetUINT32(mediatype, attr, value);
}

static void mediatype_get_stride_and_sample_size(IMFMediaType *mediatype, LONG *stride, DWORD *sample_size, HRESULT *hr)
{
    UINT64 frame_size;
    GUID subtype;

    if (SUCCEEDED(*hr))
        *hr = IMFMediaType_GetGUID(mediatype, &MF_MT_SUBTYPE, &subtype);

    if (SUCCEEDED(*hr))
        *hr = IMFMediaType_GetUINT64(mediatype, &MF_MT_FRAME_SIZE, &frame_size);

    if (SUCCEEDED(*hr))
        *hr = MFGetStrideForBitmapInfoHeader(subtype.Data1, frame_size >> 32, stride);

    if (SUCCEEDED(*hr))
        *hr = MFGetPlaneSize(subtype.Data1, frame_size >> 32, frame_size & 0xffffffff, sample_size);
}

static HRESULT set_default_video_attributes(struct source_reader *reader, IMFMediaType *output_type)
{
    DWORD sample_size;
    BOOL compressed;
    LONG stride;
    HRESULT hr;

    if (FAILED(hr = IMFMediaType_IsCompressedFormat(output_type, &compressed)))
        return hr;

    if (!compressed)
    {
        mediatype_get_stride_and_sample_size(output_type, &stride, &sample_size, &hr);

        mediatype_set_uint32(output_type, &MF_MT_COMPRESSED, compressed, &hr);
        mediatype_set_uint32(output_type, &MF_MT_DEFAULT_STRIDE, abs(stride), &hr);
        mediatype_set_uint32(output_type, &MF_MT_SAMPLE_SIZE, sample_size, &hr);
    }

    return hr;
}

static HRESULT source_reader_create_transform(struct source_reader *reader, BOOL decoder, BOOL allow_processor,
        IMFMediaType *input_type, IMFMediaType *output_type, struct transform_entry **out)
{
    MFT_REGISTER_TYPE_INFO in_type, out_type;
    struct transform_entry *entry;
    IMFActivate **activates;
    GUID category;
    IMFTransform *transform;
    UINT i, count;
    HRESULT hr;

    if (FAILED(hr = IMFMediaType_GetMajorType(input_type, &in_type.guidMajorType))
            || FAILED(hr = IMFMediaType_GetGUID(input_type, &MF_MT_SUBTYPE, &in_type.guidSubtype)))
        return hr;
    if (FAILED(hr = IMFMediaType_GetMajorType(output_type, &out_type.guidMajorType))
            || FAILED(hr = IMFMediaType_GetGUID(output_type, &MF_MT_SUBTYPE, &out_type.guidSubtype)))
        return hr;

    if (IsEqualGUID(&out_type.guidMajorType, &MFMediaType_Video))
        category = decoder ? MFT_CATEGORY_VIDEO_DECODER : MFT_CATEGORY_VIDEO_PROCESSOR;
    else if (IsEqualGUID(&out_type.guidMajorType, &MFMediaType_Audio))
        category = decoder ? MFT_CATEGORY_AUDIO_DECODER : MFT_CATEGORY_AUDIO_EFFECT;
    else
        return MF_E_TOPO_CODEC_NOT_FOUND;

    if (!(entry = calloc(1, sizeof(*entry))))
        return E_OUTOFMEMORY;
    list_init(&entry->entry);
    entry->category = category;

    if (IsEqualGUID(&out_type.guidMajorType, &MFMediaType_Audio))
    {
        UINT32 bytes_per_second;

        /* decoders require to have MF_MT_AUDIO_BITS_PER_SAMPLE attribute set, but the source reader doesn't */
        if (FAILED(IMFMediaType_GetItem(output_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, NULL)))
        {
            if (IsEqualGUID(&out_type.guidSubtype, &MFAudioFormat_PCM))
                IMFMediaType_SetUINT32(output_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            else if (IsEqualGUID(&out_type.guidSubtype, &MFAudioFormat_Float))
                IMFMediaType_SetUINT32(output_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        }

        if (SUCCEEDED(IMFMediaType_GetUINT32(output_type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, &entry->min_buffer_size))
                && SUCCEEDED(IMFMediaType_GetUINT32(output_type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes_per_second)))
            entry->min_buffer_size = max(entry->min_buffer_size, bytes_per_second / 100);
    }

    if (IsEqualGUID(&out_type.guidMajorType, &MFMediaType_Video) && IsEqualGUID(&out_type.guidSubtype, &MFVideoFormat_ABGR32)
            && IsEqualGUID(&category, &MFT_CATEGORY_VIDEO_PROCESSOR))
    {
        /* The video processor isn't registered for MFVideoFormat_ABGR32, and native even only supports that format when
         * D3D-enabled, we still want to instantiate a video processor in such case, so fixup the subtype for MFTEnumEx.
         */
        WARN("Fixing up MFVideoFormat_ABGR32 subtype for the video processor\n");
        out_type.guidSubtype = MFVideoFormat_RGB32;
    }

    if (IsEqualGUID(&out_type.guidMajorType, &MFMediaType_Video) && IsEqualGUID(&out_type.guidSubtype, &MFVideoFormat_IYUV)
            && IsEqualGUID(&category, &MFT_CATEGORY_VIDEO_DECODER))
    {
        /* The WMV video decoder isn't registered for MFVideoFormat_IYUV, but selecting it as an output format still succeeds,
         * the host decoders usually support IYUV as well, so fixup the subtype for MFTEnumEx.
         */
        WARN("Fixing up MFVideoFormat_IYUV subtype for the video processor\n");
        out_type.guidSubtype = MFVideoFormat_NV12;
    }

    count = 0;
    if (SUCCEEDED(hr = MFTEnumEx(category, 0, &in_type, allow_processor ? NULL : &out_type, &activates, &count)))
    {
        if (!count)
        {
            free(entry);
            return MF_E_TOPO_CODEC_NOT_FOUND;
        }

        for (i = 0; i < count; i++)
        {
            IMFAttributes *attributes;
            IMFMediaType *media_type;

            if (FAILED(hr = IMFActivate_ActivateObject(activates[i], &IID_IMFTransform, (void **)&transform)))
                continue;

            if (!reader->device_manager || FAILED(IMFTransform_GetAttributes(transform, &attributes)))
                entry->attributes_initialized = TRUE;
            else
            {
                UINT32 d3d_aware = FALSE;

                if (reader->flags & SOURCE_READER_DXGI_DEVICE_MANAGER)
                {
                    if (SUCCEEDED(IMFAttributes_GetUINT32(attributes, &MF_SA_D3D11_AWARE, &d3d_aware)) && d3d_aware)
                        IMFTransform_ProcessMessage(transform, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)reader->device_manager);
                }
                else if (reader->flags & SOURCE_READER_D3D9_DEVICE_MANAGER)
                {
                    if (SUCCEEDED(IMFAttributes_GetUINT32(attributes, &MF_SA_D3D_AWARE, &d3d_aware)) && d3d_aware)
                        IMFTransform_ProcessMessage(transform, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)reader->device_manager);
                }

                entry->attributes_initialized = !d3d_aware;
                IMFAttributes_Release(attributes);
            }

            if (SUCCEEDED(hr = IMFTransform_SetInputType(transform, 0, input_type, 0))
                    && SUCCEEDED(hr = IMFTransform_GetInputCurrentType(transform, 0, &media_type)))
            {
                BOOL enable_advanced;

                source_reader_allow_video_processor(reader, &enable_advanced);

                if ((SUCCEEDED(hr = update_media_type_from_upstream(output_type, media_type, enable_advanced)))
                        && FAILED(hr = IMFTransform_SetOutputType(transform, 0, output_type, 0))
                        && FAILED(hr = set_matching_transform_output_type(transform, output_type)) && allow_processor
                        && SUCCEEDED(hr = IMFTransform_GetOutputAvailableType(transform, 0, 0, &media_type)))
                {
                    struct transform_entry *converter;

                    if (SUCCEEDED(hr = IMFTransform_SetOutputType(transform, 0, media_type, 0))
                            && SUCCEEDED(hr = update_media_type_from_upstream(output_type, media_type, enable_advanced))
                            && (enable_advanced || SUCCEEDED(hr = set_default_video_attributes(reader, output_type)))
                            && SUCCEEDED(hr = source_reader_create_transform(reader, FALSE, FALSE, media_type, output_type, &converter)))
                        list_add_tail(&entry->entry, &converter->entry);

                    IMFMediaType_Release(media_type);
                }

                if (SUCCEEDED(hr))
                {
                    entry->transform = transform;
                    *out = entry;
                    break;
                }
            }

            IMFTransform_Release(transform);
        }

        for (i = 0; i < count; ++i)
            IMFActivate_Release(activates[i]);
        CoTaskMemFree(activates);
    }

    if (FAILED(hr))
        free(entry);
    return hr;
}

static HRESULT source_reader_create_decoder_for_stream(struct source_reader *reader, DWORD index, IMFMediaType *output_type)
{
    BOOL enable_advanced = FALSE, allow_processor = TRUE;
    struct media_stream *stream = &reader->streams[index];
    IMFMediaType *input_type;
    unsigned int i = 0;
    GUID major;
    HRESULT hr;

    if (SUCCEEDED(IMFMediaType_GetMajorType(output_type, &major)) && IsEqualGUID(&major, &MFMediaType_Video))
        allow_processor = source_reader_allow_video_processor(reader, &enable_advanced);

    while (SUCCEEDED(hr = source_reader_get_native_media_type(reader, index, i++, &input_type)))
    {
        struct transform_entry *entry;

        /* first, try to append a single processor, then try again with a decoder and a processor */
        if ((allow_processor && SUCCEEDED(hr = source_reader_create_transform(reader, FALSE, FALSE, input_type, output_type, &entry)))
                || SUCCEEDED(hr = source_reader_create_transform(reader, TRUE, allow_processor, input_type, output_type, &entry)))
        {
            struct list *ptr = list_head(&entry->entry);
            struct transform_entry *service = ptr ? LIST_ENTRY(ptr, struct transform_entry, entry) : entry;
            IMFMediaTypeHandler *type_handler;

            if (enable_advanced)
            {
                /* when advanced video processing is enabled, converters are exposed as stream transform service */
                stream->transform_service = service->transform;
                IMFTransform_AddRef(stream->transform_service);
            }
            else
            {
                /* when advanced video processing is disabled, only decoders are exposed as stream transform service */
                if (IsEqualGUID(&entry->category, &MFT_CATEGORY_AUDIO_DECODER)
                        || IsEqualGUID(&entry->category, &MFT_CATEGORY_VIDEO_DECODER))
                {
                    stream->transform_service = entry->transform;
                    IMFTransform_AddRef(stream->transform_service);

                    /* converters are hidden from the stream transforms */
                    if (service != entry)
                        service->hidden = TRUE;
                }
                else
                {
                    /* converters are hidden from the stream transforms */
                    entry->hidden = TRUE;
                }
            }

            /* move any additional transforms that have been created */
            list_move_head(&stream->transforms, &entry->entry);
            list_add_head(&stream->transforms, &entry->entry);

            if (SUCCEEDED(source_reader_get_source_type_handler(reader, index, &type_handler)))
            {
                if (FAILED(hr = IMFMediaTypeHandler_SetCurrentMediaType(type_handler, input_type)))
                    WARN("Failed to set current input media type, hr %#lx\n", hr);
                IMFMediaTypeHandler_Release(type_handler);
            }

            if (FAILED(hr = IMFTransform_GetOutputCurrentType(service->transform, 0, &output_type)))
                WARN("Failed to get decoder output media type, hr %#lx\n", hr);
            else
            {
                IMFMediaType_CopyAllItems(output_type, (IMFAttributes *)stream->current);
                IMFMediaType_Release(output_type);
            }

            IMFMediaType_Release(input_type);
            return S_OK;
        }

        IMFMediaType_Release(input_type);
    }

    return MF_E_TOPO_CODEC_NOT_FOUND;
}

static HRESULT WINAPI src_reader_SetCurrentMediaType(IMFSourceReaderEx *iface, DWORD index, DWORD *reserved,
        IMFMediaType *type)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);
    IMFMediaType *output_type;
    HRESULT hr;

    TRACE("%p, %#lx, %p, %p.\n", iface, index, reserved, type);

    switch (index)
    {
        case MF_SOURCE_READER_FIRST_VIDEO_STREAM:
            index = reader->first_video_stream_index;
            break;
        case MF_SOURCE_READER_FIRST_AUDIO_STREAM:
            index = reader->first_audio_stream_index;
            break;
        default:
            ;
    }

    if (index >= reader->stream_count)
        return MF_E_INVALIDSTREAMNUMBER;

    if (FAILED(hr = MFCreateMediaType(&output_type)))
        return hr;
    if (FAILED(IMFMediaType_CopyAllItems(type, (IMFAttributes *)output_type)))
    {
        IMFMediaType_Release(output_type);
        return hr;
    }

    /* FIXME: setting the output type while streaming should trigger a flush */

    EnterCriticalSection(&reader->cs);

    hr = source_reader_set_compatible_media_type(reader, index, output_type);
    if (hr == S_FALSE)
        hr = source_reader_create_decoder_for_stream(reader, index, output_type);

    LeaveCriticalSection(&reader->cs);

    IMFMediaType_Release(output_type);
    return hr;
}

static HRESULT WINAPI src_reader_SetCurrentPosition(IMFSourceReaderEx *iface, REFGUID format, REFPROPVARIANT position)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);
    struct source_reader_async_command *command;
    unsigned int i;
    DWORD flags;
    HRESULT hr;

    TRACE("%p, %s, %p.\n", iface, debugstr_guid(format), position);

    if (FAILED(hr = IMFMediaSource_GetCharacteristics(reader->source, &flags)))
        return hr;

    if (!(flags & MFMEDIASOURCE_CAN_SEEK))
        return MF_E_INVALIDREQUEST;

    EnterCriticalSection(&reader->cs);

    /* Check if we got pending requests. */
    for (i = 0; i < reader->stream_count; ++i)
    {
        if (reader->streams[i].requests)
        {
            hr = MF_E_INVALIDREQUEST;
            break;
        }
    }

    if (SUCCEEDED(hr))
    {
        for (i = 0; i < reader->stream_count; ++i)
        {
            source_reader_release_responses(reader, &reader->streams[i]);
            source_reader_reset_stream_timing(&reader->streams[i]);
        }

        if (reader->async_callback)
        {
            if (SUCCEEDED(hr = source_reader_create_async_op(SOURCE_READER_ASYNC_SEEK, &command)))
            {
                command->u.seek.format = *format;
                PropVariantCopy(&command->u.seek.position, position);

                reader->flags |= SOURCE_READER_ASYNC_SEEK_QUEUED;
                hr = MFPutWorkItem(reader->queue, &reader->async_commands_callback, &command->IUnknown_iface);
                if (FAILED(hr))
                    reader->flags &= ~SOURCE_READER_ASYNC_SEEK_QUEUED;
                IUnknown_Release(&command->IUnknown_iface);
            }
        }
        else
        {
            ++reader->seek_serial;
            reader->flags |= SOURCE_READER_SEEKING;
            if (FAILED(hr = IMFMediaSource_Start(reader->source, reader->descriptor, format, position)))
            {
                reader->completed_seek_serial = reader->seek_serial;
                reader->flags &= ~SOURCE_READER_SEEKING;
            }
        }
    }

    LeaveCriticalSection(&reader->cs);

    return hr;
}

static HRESULT source_reader_read_sample(struct source_reader *reader, DWORD index, DWORD flags, DWORD *actual_index,
        DWORD *stream_flags, LONGLONG *timestamp, IMFSample **sample)
{
    struct media_stream *stream;
    DWORD actual_index_tmp = MF_SOURCE_READER_INVALID_STREAM_INDEX;
    LONGLONG timestamp_tmp = 0;
    DWORD stream_index;
    HRESULT hr = S_OK;

    if (!stream_flags || !sample)
        return E_POINTER;

    *sample = NULL;

    if (!timestamp)
        timestamp = &timestamp_tmp;

    if (!actual_index)
        actual_index = &actual_index_tmp;

    *actual_index = MF_SOURCE_READER_INVALID_STREAM_INDEX;
    *stream_flags = 0;
    *timestamp = 0;

    if (SUCCEEDED(hr = source_reader_start_source(reader)))
    {
        if (index == MF_SOURCE_READER_ANY_STREAM)
        {
            BOOL have_result;

            have_result = source_reader_get_read_result_any(reader, flags, &hr, actual_index, stream_flags,
                    timestamp, sample);
            if (FAILED(source_reader_request_selected_stream_samples(reader)))
                WARN("Failed to request a sample, hr %#lx.\n", hr);
            if (!have_result)
            {
                while (!source_reader_got_response_for_any_selected_stream(reader)
                        && !source_reader_selected_streams_eos(reader))
                {
                    if (FAILED(hr = source_reader_request_selected_stream_samples(reader)))
                        WARN("Failed to request a sample, hr %#lx.\n", hr);
                    SleepConditionVariableCS(&reader->sample_event, &reader->cs, INFINITE);
                }
                if (SUCCEEDED(hr))
                    source_reader_get_read_result_any(reader, flags, &hr, actual_index, stream_flags,
                            timestamp, sample);
            }
        }
        else if (SUCCEEDED(hr = source_reader_get_stream_read_index(reader, index, &stream_index)))
        {
            *actual_index = stream_index;

            stream = &reader->streams[stream_index];

            source_reader_request_audio_for_video_sync(reader, stream);
            if (!source_reader_get_read_result(reader, stream, flags, &hr, actual_index, stream_flags,
                   timestamp, sample))
            {
                while (!source_reader_got_response_for_stream(reader, stream) && stream->state != STREAM_STATE_EOS)
                {
                    stream->requests++;
                    if (FAILED(hr = source_reader_request_sample(reader, stream)))
                        WARN("Failed to request a sample, hr %#lx.\n", hr);
                    if (!source_reader_got_response_for_stream(reader, stream)
                            && stream->stream && !(stream->flags & STREAM_FLAG_SAMPLE_REQUESTED))
                    {
                        *stream_flags = MF_SOURCE_READERF_ERROR;
                        *timestamp = 0;
                        break;
                    }
                    if (!source_reader_got_response_for_stream(reader, stream) && stream->state != STREAM_STATE_EOS)
                        SleepConditionVariableCS(&reader->sample_event, &reader->cs, INFINITE);
                }
                if (SUCCEEDED(hr))
                {
                    source_reader_request_audio_for_video_sync(reader, stream);
                    source_reader_get_read_result(reader, stream, flags, &hr, actual_index, stream_flags,
                       timestamp, sample);
                }
            }
            if (*sample)
                source_reader_prefetch_stream_sample(reader, stream, flags);
        }
        else
        {
            *actual_index = index;
            *stream_flags = MF_SOURCE_READERF_ERROR;
            *timestamp = 0;
        }
    }

    TRACE("Stream %lu, got sample %p, flags %#lx.\n", *actual_index, *sample, *stream_flags);

    return hr;
}

static HRESULT source_reader_read_sample_async(struct source_reader *reader, unsigned int index, unsigned int flags,
        DWORD *actual_index, DWORD *stream_flags, LONGLONG *timestamp, IMFSample **sample)
{
    struct source_reader_async_command *command;
    HRESULT hr;

    if (actual_index || stream_flags || timestamp || sample)
        return E_INVALIDARG;

    if (reader->flags & SOURCE_READER_FLUSHING)
        hr = MF_E_NOTACCEPTING;
    else
    {
        if (SUCCEEDED(hr = source_reader_create_async_op(SOURCE_READER_ASYNC_READ, &command)))
        {
            command->u.read.stream_index = index;
            command->u.read.flags = flags;

            hr = MFPutWorkItem(reader->queue, &reader->async_commands_callback, &command->IUnknown_iface);
            IUnknown_Release(&command->IUnknown_iface);
        }
    }

    return hr;
}

static HRESULT WINAPI src_reader_ReadSample(IMFSourceReaderEx *iface, DWORD index, DWORD flags, DWORD *actual_index,
        DWORD *stream_flags, LONGLONG *timestamp, IMFSample **sample)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);
    HRESULT hr;

    TRACE("%p, %#lx, %#lx, %p, %p, %p, %p\n", iface, index, flags, actual_index, stream_flags, timestamp, sample);

    EnterCriticalSection(&reader->cs);

    while (reader->flags & SOURCE_READER_SEEKING)
    {
        SleepConditionVariableCS(&reader->state_event, &reader->cs, INFINITE);
    }

    if (reader->async_callback)
        hr = source_reader_read_sample_async(reader, index, flags, actual_index, stream_flags, timestamp, sample);
    else
        hr = source_reader_read_sample(reader, index, flags, actual_index, stream_flags, timestamp, sample);

    LeaveCriticalSection(&reader->cs);

    return hr;
}

static HRESULT source_reader_flush_async(struct source_reader *reader, unsigned int index)
{
    struct source_reader_async_command *command;
    unsigned int stream_index;
    HRESULT hr;

    if ((reader->flags & SOURCE_READER_ASYNC_SEEK_QUEUED) || reader->source_state != SOURCE_STATE_STARTED)
        return S_OK;

    if (reader->flags & SOURCE_READER_FLUSHING)
        return MF_E_INVALIDREQUEST;

    switch (index)
    {
        case MF_SOURCE_READER_FIRST_VIDEO_STREAM:
            stream_index = reader->first_video_stream_index;
            break;
        case MF_SOURCE_READER_FIRST_AUDIO_STREAM:
            stream_index = reader->first_audio_stream_index;
            break;
        default:
            stream_index = index;
    }

    if (stream_index != MF_SOURCE_READER_ALL_STREAMS && stream_index >= reader->stream_count)
        return MF_E_INVALIDSTREAMNUMBER;

    if (FAILED(hr = source_reader_create_async_op(SOURCE_READER_ASYNC_FLUSH, &command)))
        return hr;

    reader->flags |= SOURCE_READER_FLUSHING;

    command->u.flush.stream_index = stream_index;

    hr = MFPutWorkItem(reader->queue, &reader->async_commands_callback, &command->IUnknown_iface);
    if (FAILED(hr))
        reader->flags &= ~SOURCE_READER_FLUSHING;
    IUnknown_Release(&command->IUnknown_iface);

    return hr;
}

static HRESULT WINAPI src_reader_Flush(IMFSourceReaderEx *iface, DWORD index)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);
    HRESULT hr;

    TRACE("%p, %#lx.\n", iface, index);

    EnterCriticalSection(&reader->cs);

    if (reader->async_callback)
        hr = source_reader_flush_async(reader, index);
    else
        hr = source_reader_flush(reader, index);

    LeaveCriticalSection(&reader->cs);

    return hr;
}

static HRESULT WINAPI src_reader_GetServiceForStream(IMFSourceReaderEx *iface, DWORD index, REFGUID service,
        REFIID riid, void **object)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);
    IUnknown *obj = NULL;
    HRESULT hr = S_OK;

    TRACE("%p, %#lx, %s, %s, %p\n", iface, index, debugstr_guid(service), debugstr_guid(riid), object);

    EnterCriticalSection(&reader->cs);

    switch (index)
    {
        case MF_SOURCE_READER_MEDIASOURCE:
            obj = (IUnknown *)reader->source;
            break;
        default:
            if (index == MF_SOURCE_READER_FIRST_VIDEO_STREAM)
                index = reader->first_video_stream_index;
            else if (index == MF_SOURCE_READER_FIRST_AUDIO_STREAM)
                index = reader->first_audio_stream_index;

            if (index >= reader->stream_count)
                hr = MF_E_INVALIDSTREAMNUMBER;
            else if (!(obj = (IUnknown *)reader->streams[index].transform_service))
                hr = E_NOINTERFACE;
            break;
    }

    if (obj)
        IUnknown_AddRef(obj);

    LeaveCriticalSection(&reader->cs);

    if (obj)
    {
        if (IsEqualGUID(service, &GUID_NULL))
        {
            hr = IUnknown_QueryInterface(obj, riid, object);
        }
        else
        {
            IMFGetService *gs;

            hr = IUnknown_QueryInterface(obj, &IID_IMFGetService, (void **)&gs);
            if (SUCCEEDED(hr))
            {
                hr = IMFGetService_GetService(gs, service, riid, object);
                IMFGetService_Release(gs);
            }
        }
    }

    if (obj)
        IUnknown_Release(obj);

    return hr;
}

static HRESULT WINAPI src_reader_GetPresentationAttribute(IMFSourceReaderEx *iface, DWORD index,
        REFGUID guid, PROPVARIANT *value)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);
    IMFStreamDescriptor *sd;
    BOOL selected;
    HRESULT hr;

    TRACE("%p, %#lx, %s, %p.\n", iface, index, debugstr_guid(guid), value);

    switch (index)
    {
        case MF_SOURCE_READER_MEDIASOURCE:
            if (IsEqualGUID(guid, &MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS))
            {
                DWORD flags;

                if (FAILED(hr = IMFMediaSource_GetCharacteristics(reader->source, &flags)))
                    return hr;

                value->vt = VT_UI4;
                value->ulVal = flags;
                return S_OK;
            }
            else
            {
                return IMFPresentationDescriptor_GetItem(reader->descriptor, guid, value);
            }
            break;
        case MF_SOURCE_READER_FIRST_VIDEO_STREAM:
            index = reader->first_video_stream_index;
            break;
        case MF_SOURCE_READER_FIRST_AUDIO_STREAM:
            index = reader->first_audio_stream_index;
            break;
        default:
            ;
    }

    if (FAILED(hr = IMFPresentationDescriptor_GetStreamDescriptorByIndex(reader->descriptor, index, &selected, &sd)))
        return hr;

    hr = IMFStreamDescriptor_GetItem(sd, guid, value);
    IMFStreamDescriptor_Release(sd);

    return hr;
}

static HRESULT WINAPI src_reader_SetNativeMediaType(IMFSourceReaderEx *iface, DWORD stream_index,
        IMFMediaType *media_type, DWORD *stream_flags)
{
    FIXME("%p, %#lx, %p, %p.\n", iface, stream_index, media_type, stream_flags);

    return E_NOTIMPL;
}

static HRESULT WINAPI src_reader_AddTransformForStream(IMFSourceReaderEx *iface, DWORD stream_index,
        IUnknown *transform)
{
    FIXME("%p, %#lx, %p.\n", iface, stream_index, transform);

    return E_NOTIMPL;
}

static HRESULT WINAPI src_reader_RemoveAllTransformsForStream(IMFSourceReaderEx *iface, DWORD stream_index)
{
    FIXME("%p, %#lx.\n", iface, stream_index);

    return E_NOTIMPL;
}

static struct transform_entry *get_transform_at_index(struct media_stream *stream, UINT index)
{
    struct transform_entry *entry;

    LIST_FOR_EACH_ENTRY(entry, &stream->transforms, struct transform_entry, entry)
        if (!entry->hidden && !index--)
            return entry;

    return NULL;
}

static HRESULT WINAPI src_reader_GetTransformForStream(IMFSourceReaderEx *iface, DWORD stream_index,
        DWORD transform_index, GUID *category, IMFTransform **transform)
{
    struct source_reader *reader = impl_from_IMFSourceReaderEx(iface);
    struct transform_entry *entry;
    HRESULT hr;

    TRACE("%p, %#lx, %#lx, %p, %p.\n", iface, stream_index, transform_index, category, transform);

    if (!transform)
        return E_POINTER;

    EnterCriticalSection(&reader->cs);

    if (stream_index == MF_SOURCE_READER_FIRST_VIDEO_STREAM)
        stream_index = reader->first_video_stream_index;
    else if (stream_index == MF_SOURCE_READER_FIRST_AUDIO_STREAM)
        stream_index = reader->first_audio_stream_index;

    if (stream_index >= reader->stream_count)
        hr = MF_E_INVALIDSTREAMNUMBER;
    else if (!(entry = get_transform_at_index(&reader->streams[stream_index], transform_index)))
        hr = MF_E_INVALIDINDEX;
    else
    {
        if (category)
            *category = entry->category;
        *transform = entry->transform;
        IMFTransform_AddRef(*transform);
        hr = S_OK;
    }

    LeaveCriticalSection(&reader->cs);

    return hr;
}

static const IMFSourceReaderExVtbl srcreader_vtbl =
{
    src_reader_QueryInterface,
    src_reader_AddRef,
    src_reader_Release,
    src_reader_GetStreamSelection,
    src_reader_SetStreamSelection,
    src_reader_GetNativeMediaType,
    src_reader_GetCurrentMediaType,
    src_reader_SetCurrentMediaType,
    src_reader_SetCurrentPosition,
    src_reader_ReadSample,
    src_reader_Flush,
    src_reader_GetServiceForStream,
    src_reader_GetPresentationAttribute,
    src_reader_SetNativeMediaType,
    src_reader_AddTransformForStream,
    src_reader_RemoveAllTransformsForStream,
    src_reader_GetTransformForStream,
};

static DWORD reader_get_first_stream_index(IMFPresentationDescriptor *descriptor, const GUID *major)
{
    DWORD count, i;
    BOOL selected;
    HRESULT hr;
    GUID guid;

    if (FAILED(IMFPresentationDescriptor_GetStreamDescriptorCount(descriptor, &count)))
        return MF_SOURCE_READER_INVALID_STREAM_INDEX;

    for (i = 0; i < count; ++i)
    {
        IMFMediaTypeHandler *handler;
        IMFStreamDescriptor *sd;

        if (SUCCEEDED(IMFPresentationDescriptor_GetStreamDescriptorByIndex(descriptor, i, &selected, &sd)))
        {
            hr = IMFStreamDescriptor_GetMediaTypeHandler(sd, &handler);
            IMFStreamDescriptor_Release(sd);
            if (SUCCEEDED(hr))
            {
                hr = IMFMediaTypeHandler_GetMajorType(handler, &guid);
                IMFMediaTypeHandler_Release(handler);
                if (FAILED(hr))
                {
                    WARN("Failed to get stream major type, hr %#lx.\n", hr);
                    continue;
                }

                if (IsEqualGUID(&guid, major))
                {
                    return i;
                }
            }
        }
    }

    return MF_SOURCE_READER_INVALID_STREAM_INDEX;
}

static BOOL source_reader_has_windows_media_av_streams(const struct source_reader *reader)
{
    BOOL has_windows_media_audio = FALSE, has_windows_media_video = FALSE;
    unsigned int i;

    for (i = 0; i < reader->stream_count; ++i)
    {
        GUID subtype = GUID_NULL;

        if (FAILED(IMFMediaType_GetGUID(reader->streams[i].current, &MF_MT_SUBTYPE, &subtype)))
            continue;

        if (IsEqualGUID(&subtype, &MFAudioFormat_WMAudioV8)
                || IsEqualGUID(&subtype, &MFAudioFormat_WMAudioV9)
                || IsEqualGUID(&subtype, &MFAudioFormat_WMAudio_Lossless)
                || IsEqualGUID(&subtype, &MFAudioFormat_WMASPDIF))
            has_windows_media_audio = TRUE;
        else if (IsEqualGUID(&subtype, &MFVideoFormat_WMV1)
                || IsEqualGUID(&subtype, &MFVideoFormat_WMV2)
                || IsEqualGUID(&subtype, &MFVideoFormat_WMV3)
                || IsEqualGUID(&subtype, &MFVideoFormat_WVC1))
            has_windows_media_video = TRUE;
    }

    return has_windows_media_audio && has_windows_media_video;
}

static HRESULT create_source_reader_from_source(IMFMediaSource *source, IMFAttributes *attributes,
        BOOL shutdown_on_release, REFIID riid, void **out)
{
    struct source_reader *object;
    unsigned int i;
    HRESULT hr;

    object = calloc(1, sizeof(*object));
    if (!object)
        return E_OUTOFMEMORY;

    object->IMFSourceReaderEx_iface.lpVtbl = &srcreader_vtbl;
    object->source_events_callback.lpVtbl = &source_events_callback_vtbl;
    object->stream_events_callback.lpVtbl = &stream_events_callback_vtbl;
    object->async_commands_callback.lpVtbl = &async_commands_callback_vtbl;
    object->public_refcount = 1;
    object->refcount = 1;
    list_init(&object->responses);
    if (shutdown_on_release)
        object->flags |= SOURCE_READER_SHUTDOWN_ON_RELEASE;
    object->source = source;
    IMFMediaSource_AddRef(object->source);
    InitializeCriticalSection(&object->cs);
    InitializeConditionVariable(&object->sample_event);
    InitializeConditionVariable(&object->state_event);
    InitializeConditionVariable(&object->stop_event);

    if (FAILED(hr = IMFMediaSource_CreatePresentationDescriptor(object->source, &object->descriptor)))
        goto failed;

    if (FAILED(hr = IMFPresentationDescriptor_GetStreamDescriptorCount(object->descriptor, &object->stream_count)))
        goto failed;

    if (!(object->streams = calloc(object->stream_count, sizeof(*object->streams))))
    {
        hr = E_OUTOFMEMORY;
        goto failed;
    }

    /* Set initial current media types. */
    for (i = 0; i < object->stream_count; ++i)
    {
        IMFMediaTypeHandler *handler;
        IMFStreamDescriptor *sd;
        IMFMediaType *src_type;
        BOOL selected;

        list_init(&object->streams[i].transforms);
        list_init(&object->streams[i].retained_audio_samples);

        if (FAILED(hr = MFCreateMediaType(&object->streams[i].current)))
            break;

        if (FAILED(hr = IMFPresentationDescriptor_GetStreamDescriptorByIndex(object->descriptor, i, &selected, &sd)))
            break;

        if (FAILED(hr = IMFStreamDescriptor_GetStreamIdentifier(sd, &object->streams[i].id)))
            WARN("Failed to get stream identifier, hr %#lx.\n", hr);

        hr = IMFStreamDescriptor_GetMediaTypeHandler(sd, &handler);
        IMFStreamDescriptor_Release(sd);
        if (FAILED(hr))
            break;

        hr = IMFMediaTypeHandler_GetMediaTypeByIndex(handler, 0, &src_type);
        IMFMediaTypeHandler_Release(handler);
        if (FAILED(hr))
            break;

        hr = IMFMediaType_CopyAllItems(src_type, (IMFAttributes *)object->streams[i].current);
        object->streams[i].unthrottled_audio_output = source_reader_audio_needs_unthrottled_output(src_type);
        object->unthrottled_audio_output |= object->streams[i].unthrottled_audio_output;
        IMFMediaType_Release(src_type);
        if (FAILED(hr))
            break;

        object->streams[i].reader = object;
        object->streams[i].index = i;
        source_reader_reset_stream_timing(&object->streams[i]);
    }

    if (FAILED(hr))
        goto failed;

    /* At least one major type has to be set. */
    object->first_audio_stream_index = reader_get_first_stream_index(object->descriptor, &MFMediaType_Audio);
    object->first_video_stream_index = reader_get_first_stream_index(object->descriptor, &MFMediaType_Video);

    if (object->first_audio_stream_index == MF_SOURCE_READER_INVALID_STREAM_INDEX &&
            object->first_video_stream_index == MF_SOURCE_READER_INVALID_STREAM_INDEX)
    {
        hr = MF_E_ATTRIBUTENOTFOUND;
    }

    if (source_reader_has_windows_media_av_streams(object))
    {
        /* Some Windows Media A/V callers keep reading each selected stream until
         * every selected stream reports EOS, instead of stopping on a single
         * MF_SOURCE_READER_ANY_STREAM EOS result.
         *
         * Keep these streams off the serial queue: WVC1 decode can run long
         * enough to starve WMA sample delivery when both stream event callbacks
         * are serialized behind the same reader queue. */
        object->flag_eos_for_all_streams = TRUE;
        if (FAILED(hr = MFLockSharedWorkQueue(L"", 0, NULL, &object->queue)))
        {
            WARN("Failed to acquire shared source reader work queue, hr %#lx.\n", hr);
            goto failed;
        }
    }
    else if (FAILED(hr = MFAllocateSerialWorkQueue(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, &object->queue)))
    {
        WARN("Failed to allocate source reader work queue, hr %#lx.\n", hr);
        goto failed;
    }

    if (FAILED(hr = IMFMediaSource_BeginGetEvent(object->source, &object->source_events_callback,
            (IUnknown *)object->source)))
    {
        goto failed;
    }

    if (attributes)
    {
        object->attributes = attributes;
        IMFAttributes_AddRef(object->attributes);

        IMFAttributes_GetUnknown(attributes, &MF_SOURCE_READER_ASYNC_CALLBACK, &IID_IMFSourceReaderCallback,
                (void **)&object->async_callback);
        if (object->async_callback)
            TRACE("Using async callback %p.\n", object->async_callback);

        IMFAttributes_GetUnknown(attributes, &MF_SOURCE_READER_D3D_MANAGER, &IID_IUnknown, (void **)&object->device_manager);
        if (object->device_manager)
        {
            IUnknown *unk = NULL;

            if (SUCCEEDED(IUnknown_QueryInterface(object->device_manager, &IID_IMFDXGIDeviceManager, (void **)&unk)))
                object->flags |= SOURCE_READER_DXGI_DEVICE_MANAGER;
            else if (SUCCEEDED(IUnknown_QueryInterface(object->device_manager, &IID_IDirect3DDeviceManager9, (void **)&unk)))
                object->flags |= SOURCE_READER_D3D9_DEVICE_MANAGER;

            if (!(object->flags & (SOURCE_READER_HAS_DEVICE_MANAGER)))
            {
                WARN("Unknown device manager.\n");
                IUnknown_Release(object->device_manager);
                object->device_manager = NULL;
            }

            if (unk)
                IUnknown_Release(unk);
        }
    }

    if (SUCCEEDED(hr))
        hr = IMFSourceReaderEx_QueryInterface(&object->IMFSourceReaderEx_iface, riid, out);

failed:
    IMFSourceReaderEx_Release(&object->IMFSourceReaderEx_iface);
    return hr;
}

static HRESULT create_source_reader_from_stream(IMFByteStream *stream, IMFAttributes *attributes,
        REFIID riid, void **out)
{
    IPropertyStore *props = NULL;
    IMFSourceResolver *resolver;
    MF_OBJECT_TYPE obj_type;
    IMFMediaSource *source;
    HRESULT hr;

    if (FAILED(hr = MFCreateSourceResolver(&resolver)))
        return hr;

    if (attributes)
        IMFAttributes_GetUnknown(attributes, &MF_SOURCE_READER_MEDIASOURCE_CONFIG, &IID_IPropertyStore,
                (void **)&props);

    hr = IMFSourceResolver_CreateObjectFromByteStream(resolver, stream, NULL, MF_RESOLUTION_MEDIASOURCE
            | MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE, props, &obj_type, (IUnknown **)&source);
    IMFSourceResolver_Release(resolver);
    if (props)
        IPropertyStore_Release(props);
    if (FAILED(hr))
        return hr;

    hr = create_source_reader_from_source(source, attributes, TRUE, riid, out);
    IMFMediaSource_Release(source);
    return hr;
}

static HRESULT create_source_reader_from_url(const WCHAR *url, IMFAttributes *attributes, REFIID riid, void **out)
{
    IPropertyStore *props = NULL;
    IMFSourceResolver *resolver;
    IUnknown *object = NULL;
    MF_OBJECT_TYPE obj_type;
    IMFMediaSource *source;
    HRESULT hr;

    if (FAILED(hr = MFCreateSourceResolver(&resolver)))
        return hr;

    if (attributes)
        IMFAttributes_GetUnknown(attributes, &MF_SOURCE_READER_MEDIASOURCE_CONFIG, &IID_IPropertyStore,
                (void **)&props);

    hr = IMFSourceResolver_CreateObjectFromURL(resolver, url, MF_RESOLUTION_MEDIASOURCE, props, &obj_type,
            &object);
    if (SUCCEEDED(hr))
    {
        switch (obj_type)
        {
            case MF_OBJECT_BYTESTREAM:
                hr = IMFSourceResolver_CreateObjectFromByteStream(resolver, (IMFByteStream *)object, NULL,
                        MF_RESOLUTION_MEDIASOURCE, props, &obj_type, (IUnknown **)&source);
                break;
            case MF_OBJECT_MEDIASOURCE:
                source = (IMFMediaSource *)object;
                IMFMediaSource_AddRef(source);
                break;
            default:
                WARN("Unknown object type %d.\n", obj_type);
                hr = E_UNEXPECTED;
        }
        IUnknown_Release(object);
    }

    IMFSourceResolver_Release(resolver);
    if (props)
        IPropertyStore_Release(props);
    if (FAILED(hr))
        return hr;

    hr = create_source_reader_from_source(source, attributes, TRUE, riid, out);
    IMFMediaSource_Release(source);
    return hr;
}

static HRESULT create_source_reader_from_object(IUnknown *unk, IMFAttributes *attributes, REFIID riid, void **out)
{
    IMFMediaSource *source = NULL;
    IMFByteStream *stream = NULL;
    HRESULT hr;

    hr = IUnknown_QueryInterface(unk, &IID_IMFMediaSource, (void **)&source);
    if (FAILED(hr))
        hr = IUnknown_QueryInterface(unk, &IID_IMFByteStream, (void **)&stream);

    if (source)
    {
        UINT32 disconnect = 0;

        if (attributes)
            IMFAttributes_GetUINT32(attributes, &MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, &disconnect);
        hr = create_source_reader_from_source(source, attributes, !disconnect, riid, out);
    }
    else if (stream)
        hr = create_source_reader_from_stream(stream, attributes, riid, out);

    if (source)
        IMFMediaSource_Release(source);
    if (stream)
        IMFByteStream_Release(stream);

    return hr;
}

/***********************************************************************
 *      MFCreateSourceReaderFromByteStream (mfreadwrite.@)
 */
HRESULT WINAPI MFCreateSourceReaderFromByteStream(IMFByteStream *stream, IMFAttributes *attributes,
        IMFSourceReader **reader)
{
    TRACE("%p, %p, %p.\n", stream, attributes, reader);

    return create_source_reader_from_object((IUnknown *)stream, attributes, &IID_IMFSourceReader, (void **)reader);
}

/***********************************************************************
 *      MFCreateSourceReaderFromMediaSource (mfreadwrite.@)
 */
HRESULT WINAPI MFCreateSourceReaderFromMediaSource(IMFMediaSource *source, IMFAttributes *attributes,
        IMFSourceReader **reader)
{
    TRACE("%p, %p, %p.\n", source, attributes, reader);

    return create_source_reader_from_object((IUnknown *)source, attributes, &IID_IMFSourceReader, (void **)reader);
}

/***********************************************************************
 *      MFCreateSourceReaderFromURL (mfreadwrite.@)
 */
HRESULT WINAPI MFCreateSourceReaderFromURL(const WCHAR *url, IMFAttributes *attributes, IMFSourceReader **reader)
{
    TRACE("%s, %p, %p.\n", debugstr_w(url), attributes, reader);

    return create_source_reader_from_url(url, attributes, &IID_IMFSourceReader, (void **)reader);
}

static HRESULT WINAPI readwrite_factory_QueryInterface(IMFReadWriteClassFactory *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IMFReadWriteClassFactory) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *out = iface;
        IMFReadWriteClassFactory_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI readwrite_factory_AddRef(IMFReadWriteClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI readwrite_factory_Release(IMFReadWriteClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI readwrite_factory_CreateInstanceFromURL(IMFReadWriteClassFactory *iface, REFCLSID clsid,
        const WCHAR *url, IMFAttributes *attributes, REFIID riid, void **out)
{
    TRACE("%s, %s, %p, %s, %p.\n", debugstr_guid(clsid), debugstr_w(url), attributes, debugstr_guid(riid), out);

    if (IsEqualGUID(clsid, &CLSID_MFSourceReader))
    {
        return create_source_reader_from_url(url, attributes, &IID_IMFSourceReader, out);
    }
    else if (IsEqualGUID(clsid, &CLSID_MFSinkWriter))
    {
        return create_sink_writer_from_url(url, NULL, attributes, riid, out);
    }

    FIXME("Unsupported %s.\n", debugstr_guid(clsid));

    return E_NOTIMPL;
}

static HRESULT WINAPI readwrite_factory_CreateInstanceFromObject(IMFReadWriteClassFactory *iface, REFCLSID clsid,
        IUnknown *unk, IMFAttributes *attributes, REFIID riid, void **out)
{
    HRESULT hr;

    TRACE("%s, %p, %p, %s, %p.\n", debugstr_guid(clsid), unk, attributes, debugstr_guid(riid), out);

    if (IsEqualGUID(clsid, &CLSID_MFSourceReader))
    {
        return create_source_reader_from_object(unk, attributes, riid, out);
    }
    else if (IsEqualGUID(clsid, &CLSID_MFSinkWriter))
    {
        IMFByteStream *stream = NULL;
        IMFMediaSink *sink = NULL;

        hr = IUnknown_QueryInterface(unk, &IID_IMFByteStream, (void **)&stream);
        if (FAILED(hr))
            hr = IUnknown_QueryInterface(unk, &IID_IMFMediaSink, (void **)&sink);

        if (stream)
            hr = create_sink_writer_from_url(NULL, stream, attributes, riid, out);
        else if (sink)
            hr = create_sink_writer_from_sink(sink, attributes, riid, out);

        if (sink)
            IMFMediaSink_Release(sink);
        if (stream)
            IMFByteStream_Release(stream);

        return hr;
    }
    else
    {
        WARN("Unsupported class %s.\n", debugstr_guid(clsid));
        *out = NULL;
        return E_FAIL;
    }
}

static const IMFReadWriteClassFactoryVtbl readwrite_factory_vtbl =
{
    readwrite_factory_QueryInterface,
    readwrite_factory_AddRef,
    readwrite_factory_Release,
    readwrite_factory_CreateInstanceFromURL,
    readwrite_factory_CreateInstanceFromObject,
};

static IMFReadWriteClassFactory readwrite_factory = { &readwrite_factory_vtbl };

static HRESULT WINAPI classfactory_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
    TRACE("%s, %p.\n", debugstr_guid(riid), out);

    if (IsEqualGUID(riid, &IID_IClassFactory) ||
            IsEqualGUID(riid, &IID_IUnknown))
    {
        IClassFactory_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("interface %s not implemented\n", debugstr_guid(riid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI classfactory_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI classfactory_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI classfactory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out)
{
    TRACE("%p, %s, %p.\n", outer, debugstr_guid(riid), out);

    *out = NULL;

    if (outer)
        return CLASS_E_NOAGGREGATION;

    return IMFReadWriteClassFactory_QueryInterface(&readwrite_factory, riid, out);
}

static HRESULT WINAPI classfactory_LockServer(IClassFactory *iface, BOOL dolock)
{
    FIXME("%d.\n", dolock);
    return S_OK;
}

static const IClassFactoryVtbl classfactoryvtbl =
{
    classfactory_QueryInterface,
    classfactory_AddRef,
    classfactory_Release,
    classfactory_CreateInstance,
    classfactory_LockServer,
};

static IClassFactory classfactory = { &classfactoryvtbl };

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
    TRACE("%s, %s, %p.\n", debugstr_guid(clsid), debugstr_guid(riid), out);

    if (IsEqualGUID(clsid, &CLSID_MFReadWriteClassFactory))
        return IClassFactory_QueryInterface(&classfactory, riid, out);

    WARN("Unsupported class %s.\n", debugstr_guid(clsid));
    *out = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}
