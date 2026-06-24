/* winedmo/main.c — DirectShow and Media Foundation COM class factory and DLL entry points
 *
 * Copyright 2002 Lionel Ulmer
 * Copyright 2010 Aric Stewart, CodeWeavers
 * Copyright 2024 Rémi Bernon for CodeWeavers
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

#define WINE_NO_NAMELESS_EXTENSION

#define EXTERN_GUID DEFINE_GUID

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "initguid.h"
#include "winedmo_private.h"
#include "winternl.h"
#include "rpcproxy.h"
#include "dmoreg.h"
#include "winedmo_guids.h"
#include "wmcodecdsp.h"
#include "mferror.h"
#include "mfapi.h"
#include "winnls.h"
#include "mfidl.h"
#include "unixlib.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(quartz);
WINE_DECLARE_DEBUG_CHANNEL(mfplat);
WINE_DECLARE_DEBUG_CHANNEL(wmvcore);
WINE_DECLARE_DEBUG_CHANNEL(dmo);

DEFINE_GUID(GUID_NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
DEFINE_GUID(MEDIASUBTYPE_VC1S,MAKEFOURCC('V','C','1','S'),0x0000,0x0010,0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71);
DEFINE_MEDIATYPE_GUID(MEDIASUBTYPE_RAW_AAC1, WAVE_FORMAT_RAW_AAC1);
DEFINE_MEDIATYPE_GUID(MEDIASUBTYPE_FRAUNHOFER_IIS_MPEG2_AAC, WAVE_FORMAT_FRAUNHOFER_IIS_MPEG2_AAC);
DEFINE_MEDIATYPE_GUID(MEDIASUBTYPE_MPEG_RAW_AAC, WAVE_FORMAT_MPEG_RAW_AAC);
DEFINE_MEDIATYPE_GUID(MEDIASUBTYPE_NOKIA_MPEG_ADTS_AAC, WAVE_FORMAT_NOKIA_MPEG_ADTS_AAC);
DEFINE_MEDIATYPE_GUID(MEDIASUBTYPE_NOKIA_MPEG_RAW_AAC, WAVE_FORMAT_NOKIA_MPEG_RAW_AAC);
DEFINE_MEDIATYPE_GUID(MEDIASUBTYPE_VODAFONE_MPEG_ADTS_AAC, WAVE_FORMAT_VODAFONE_MPEG_ADTS_AAC);
DEFINE_MEDIATYPE_GUID(MEDIASUBTYPE_VODAFONE_MPEG_RAW_AAC, WAVE_FORMAT_VODAFONE_MPEG_RAW_AAC);
DEFINE_MEDIATYPE_GUID(MEDIASUBTYPE_MPEG4_AAC, WAVE_FORMAT_MPEG4_AAC);


/* -------------------------------------------------------------------------
 * stream_context helpers (from winedmo/main.c)
 * ------------------------------------------------------------------------- */

static struct stream_context *stream_context_create( struct winedmo_stream *stream, UINT64 stream_size )
{
    static const UINT BUFFER_SIZE = 0x40000;
    struct stream_context *context;

    if (!(context = malloc( sizeof(*context) + BUFFER_SIZE ))) return NULL;
    context->stream = (UINT_PTR)stream;
    context->length = stream_size;
    context->position = 0;
    context->capacity = BUFFER_SIZE;
    context->size = 0;

    return context;
}

static void stream_context_destroy( struct stream_context *context )
{
    free( context );
}


static struct stream_context *get_stream_context( UINT64 handle )
{
    return (struct stream_context *)(UINT_PTR)handle;
}

static struct winedmo_stream *get_stream( UINT64 handle )
{
    return (struct winedmo_stream *)(UINT_PTR)handle;
}

static NTSTATUS WINAPI seek_callback( void *args, ULONG size )
{
    struct seek_callback_params *params = args;
    struct stream_context *context = get_stream_context( params->context );
    struct winedmo_stream *stream = get_stream( context->stream );
    NTSTATUS status = STATUS_NOT_IMPLEMENTED;
    UINT64 pos = params->offset;

    TRACE( "stream %p, offset %#I64x\n", stream, params->offset );

    if (!stream->p_seek || (status = stream->p_seek( stream, &pos )))
        WARN( "Failed to seek stream %p, status %#lx\n", stream, status );
    else
        TRACE( "Seeked stream %p to %#I64x\n", stream, pos );

    return NtCallbackReturn( &pos, sizeof(pos), status );
}

static NTSTATUS WINAPI read_callback( void *args, ULONG size )
{
    struct read_callback_params *params = args;
    struct stream_context *context = get_stream_context( params->context );
    struct winedmo_stream *stream = get_stream( context->stream );
    NTSTATUS status = STATUS_NOT_IMPLEMENTED;
    ULONG ret = params->size;

    TRACE( "stream %p, size %#x\n", stream, params->size );

    if (!stream->p_read || (status = stream->p_read( stream, context->buffer, &ret )))
        WARN( "Failed to read from stream %p, status %#lx\n", stream, status );
    else
        TRACE( "Read %#lx bytes from stream %p\n", ret, stream );

    return NtCallbackReturn( &ret, sizeof(ret), status );
}


/* -------------------------------------------------------------------------
 * array_reserve
 * ------------------------------------------------------------------------- */

bool array_reserve(void **elements, size_t *capacity, size_t count, size_t size)
{
    unsigned int new_capacity, max_capacity;
    void *new_elements;

    if (count <= *capacity)
        return TRUE;

    max_capacity = ~(SIZE_T)0 / size;
    if (count > max_capacity)
        return FALSE;

    new_capacity = max(4, *capacity);
    while (new_capacity < count && new_capacity <= max_capacity / 2)
        new_capacity *= 2;
    if (new_capacity < count)
        new_capacity = max_capacity;

    if (!(new_elements = realloc(*elements, new_capacity * size)))
        return FALSE;

    *elements = new_elements;
    *capacity = new_capacity;

    return TRUE;
}


#define ALIGN(n, alignment) (((n) + (alignment) - 1) & ~((alignment) - 1))

unsigned int winedmo_format_get_stride(const struct winedmo_codec_format *format)
{
    const unsigned int width = format->u.video.width;

    switch (format->u.video.format)
    {
        case WINEDMO_VIDEO_FORMAT_AYUV:
            return width * 4;

        case WINEDMO_VIDEO_FORMAT_BGRA:
        case WINEDMO_VIDEO_FORMAT_BGRx:
        case WINEDMO_VIDEO_FORMAT_RGBA:
            return width * 4;

        case WINEDMO_VIDEO_FORMAT_BGR:
            return ALIGN(width * 3, 4);

        case WINEDMO_VIDEO_FORMAT_UYVY:
        case WINEDMO_VIDEO_FORMAT_YUY2:
        case WINEDMO_VIDEO_FORMAT_YVYU:
            return ALIGN(width * 2, 4);

        case WINEDMO_VIDEO_FORMAT_RGB15:
        case WINEDMO_VIDEO_FORMAT_RGB16:
            return ALIGN(width * 2, 4);

        case WINEDMO_VIDEO_FORMAT_I420:
        case WINEDMO_VIDEO_FORMAT_YV12:
            return ALIGN(width, 4); /* Y plane */

        /* NV12 stride in Windows has alignment 2. FFmpeg output is reformatted to alignment 2 where necessary. */
        case WINEDMO_VIDEO_FORMAT_NV12:
            return ALIGN(width, 2); /* Y plane */

        case WINEDMO_VIDEO_FORMAT_UNKNOWN:
            FIXME("Cannot calculate stride for unknown video format.\n");
    }

    return 0;
}

bool winedmo_video_format_is_rgb(enum winedmo_video_format format)
{
    switch (format)
    {
        case WINEDMO_VIDEO_FORMAT_BGRA:
        case WINEDMO_VIDEO_FORMAT_BGRx:
        case WINEDMO_VIDEO_FORMAT_BGR:
        case WINEDMO_VIDEO_FORMAT_RGB15:
        case WINEDMO_VIDEO_FORMAT_RGB16:
        case WINEDMO_VIDEO_FORMAT_RGBA:
            return true;

        default:
            break;
    }

    return false;
}


/* -------------------------------------------------------------------------
 * DllMain — merged: DisableThreadLibraryCalls + __wine_init_unix_call
 * ------------------------------------------------------------------------- */

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    TRACE( "instance %p, reason %lu, reserved %p\n", instance, reason, reserved );

    if (reason == DLL_PROCESS_ATTACH)
    {
        struct process_attach_params params =
        {
            .seek_callback = (UINT_PTR)seek_callback,
            .read_callback = (UINT_PTR)read_callback,
        };
        NTSTATUS status;

        DisableThreadLibraryCalls( instance );

        status = __wine_init_unix_call();
        if (!status) status = UNIX_CALL( process_attach, &params );
        if (status) WARN( "Failed to init unixlib, status %#lx\n", status );
    }

    return TRUE;
}


/* -------------------------------------------------------------------------
 * buffer_lock / buffer_unlock (from winedmo/main.c)
 * ------------------------------------------------------------------------- */

static void buffer_lock( DMO_OUTPUT_DATA_BUFFER *buffer, struct sample *sample )
{
    BYTE *data;
    HRESULT hr;
    DWORD size;

    if (FAILED(hr = IMediaBuffer_GetBufferAndLength( buffer->pBuffer, &data, &size )))
        ERR( "Failed to get media buffer data %p, hr %#lx\n", buffer, hr );
    if (FAILED(hr = IMediaBuffer_GetMaxLength( buffer->pBuffer, &size )))
        ERR( "Failed to get media buffer max length %p, hr %#lx\n", buffer, hr );

    sample->data = (UINT_PTR)data;
    sample->size = size;
}

static void buffer_unlock( DMO_OUTPUT_DATA_BUFFER *buffer, struct sample *sample, NTSTATUS status )
{
    IMFSample *object;
    HRESULT hr;
    INT64 sample_time = sample->pts;
    INT64 buffer_time = sample_time;

    if (sample_time != INT64_MIN && sample_time < 0)
        sample_time = 0;
    if (sample->dts != INT64_MIN && (buffer_time == INT64_MIN || buffer_time > sample->dts))
        buffer_time = sample->dts;

    if (FAILED(hr = IMediaBuffer_SetLength( buffer->pBuffer, status ? 0 : sample->size )))
        ERR( "Failed to update buffer length, hr %#lx\n", hr );

    buffer->dwStatus = 0;
    if (SUCCEEDED(hr = IMediaBuffer_QueryInterface( buffer->pBuffer, &IID_IMFSample, (void **)&object )))
    {
        if (sample->dts != INT64_MIN) IMFSample_SetUINT64( object, &MFSampleExtension_DecodeTimestamp, sample->dts );
        if (sample_time != INT64_MIN) IMFSample_SetSampleTime( object, sample_time );
        else if (sample->dts != INT64_MIN) IMFSample_SetSampleTime( object, sample->dts < 0 ? 0 : sample->dts );
        if (sample->duration != INT64_MIN) IMFSample_SetSampleDuration( object, sample->duration );
        if (sample->flags & SAMPLE_FLAG_SYNC_POINT) IMFSample_SetUINT32( object, &MFSampleExtension_CleanPoint, 1 );
        IMFSample_Release( object );
    }

    if ((buffer->rtTimestamp = buffer_time) != INT64_MIN) buffer->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_TIME;
    if ((buffer->rtTimelength = sample->duration) != INT64_MIN) buffer->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH;
    if (sample->flags & SAMPLE_FLAG_SYNC_POINT) buffer->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT;
    if (sample->flags & SAMPLE_FLAG_DISCONTINUITY) buffer->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_DISCONTINUITY;
    if (sample->flags & SAMPLE_FLAG_INCOMPLETE) buffer->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE;
}


/* -------------------------------------------------------------------------
 * winedmo_muxer_* functions (from winedmo/main.c)
 * ------------------------------------------------------------------------- */

NTSTATUS CDECL winedmo_muxer_create( const char *format, struct winedmo_muxer *muxer )
{
    struct muxer_create_params params = {0};
    NTSTATUS status;

    TRACE( "format %s, muxer %p\n", debugstr_a(format), muxer );
    lstrcpynA( params.format, format, sizeof(params.format) );

    if ((status = UNIX_CALL( muxer_create, &params )))
        WARN( "muxer_create failed, status %#lx\n", status );
    else
    {
        *muxer = params.muxer;
        TRACE( "created muxer %#I64x\n", muxer->handle );
    }
    return status;
}

NTSTATUS CDECL winedmo_muxer_destroy( struct winedmo_muxer muxer )
{
    struct muxer_destroy_params params = {.muxer = muxer};
    NTSTATUS status;

    TRACE( "muxer %#I64x\n", muxer.handle );
    if (!muxer.handle) return STATUS_SUCCESS;

    if ((status = UNIX_CALL( muxer_destroy, &params )))
        WARN( "muxer_destroy failed, status %#lx\n", status );
    return status;
}

NTSTATUS CDECL winedmo_muxer_add_stream( struct winedmo_muxer muxer, UINT32 stream_id, const GUID *major_type,
                                          const union winedmo_format *format, UINT32 format_size )
{
    struct muxer_add_stream_params params =
    {
        .muxer       = muxer,
        .stream_id   = stream_id,
        .major_type  = *major_type,
        .format      = (UINT_PTR)format,
        .format_size = format_size,
    };
    NTSTATUS status;

    TRACE( "muxer %#I64x stream_id %u major %s format %p size %u\n",
           muxer.handle, stream_id, debugstr_guid(major_type), format, format_size );

    if ((status = UNIX_CALL( muxer_add_stream, &params )))
        WARN( "muxer_add_stream failed, status %#lx\n", status );
    return status;
}

NTSTATUS CDECL winedmo_muxer_start( struct winedmo_muxer muxer )
{
    struct muxer_start_params params = {.muxer = muxer};
    NTSTATUS status;

    TRACE( "muxer %#I64x\n", muxer.handle );
    if ((status = UNIX_CALL( muxer_start, &params )))
        WARN( "muxer_start failed, status %#lx\n", status );
    return status;
}

NTSTATUS CDECL winedmo_muxer_push_sample( struct winedmo_muxer muxer, UINT32 stream_id,
                                           const BYTE *data, UINT32 size, INT64 pts, INT64 duration, DWORD flags )
{
    struct muxer_push_sample_params params =
    {
        .muxer     = muxer,
        .stream_id = stream_id,
        .data      = (UINT_PTR)data,
        .size      = size,
        .pts       = pts,
        .duration  = duration,
        .flags     = flags,
    };
    NTSTATUS status;

    TRACE( "muxer %#I64x stream_id %u data %p size %u pts %I64d\n",
           muxer.handle, stream_id, data, size, pts );

    if ((status = UNIX_CALL( muxer_push_sample, &params )))
        WARN( "muxer_push_sample failed, status %#lx\n", status );
    return status;
}

NTSTATUS CDECL winedmo_muxer_read_data( struct winedmo_muxer muxer, BYTE *buffer, UINT32 *size, UINT64 *offset )
{
    struct muxer_read_data_params params =
    {
        .muxer = muxer,
        .data  = (UINT_PTR)buffer,
        .size  = *size,
    };
    NTSTATUS status;

    TRACE( "muxer %#I64x buffer %p size %u\n", muxer.handle, buffer, *size );

    if (!(status = UNIX_CALL( muxer_read_data, &params )))
    {
        *size   = params.size;
        *offset = params.offset;
        TRACE( "read %u bytes at offset %#I64x\n", *size, *offset );
    }
    return status;
}

NTSTATUS CDECL winedmo_muxer_finalize( struct winedmo_muxer muxer )
{
    struct muxer_finalize_params params = {.muxer = muxer};
    NTSTATUS status;

    TRACE( "muxer %#I64x\n", muxer.handle );
    if ((status = UNIX_CALL( muxer_finalize, &params )))
        WARN( "muxer_finalize failed, status %#lx\n", status );
    return status;
}


/* -------------------------------------------------------------------------
 * winedmo_demuxer_* functions (from winedmo/main.c)
 * ------------------------------------------------------------------------- */

NTSTATUS CDECL winedmo_demuxer_check( const char *mime_type )
{
    struct demuxer_check_params params = {0};
    NTSTATUS status;

    TRACE( "mime_type %s\n", debugstr_a(mime_type) );
    lstrcpynA( params.mime_type, mime_type, sizeof(params.mime_type) );

    if ((status = UNIX_CALL( demuxer_check, &params ))) WARN( "returning %#lx\n", status );
    return status;
}

NTSTATUS CDECL winedmo_demuxer_create( const WCHAR *url, struct winedmo_stream *stream, UINT64 stream_size, INT64 *duration,
                                       UINT *stream_count, WCHAR *mime_type, struct winedmo_demuxer *demuxer )
{
    struct demuxer_create_params params = {0};
    char *tmp = NULL;
    NTSTATUS status;
    UINT len;

    TRACE( "url %s, stream %p, stream_size %#I64x, mime_type %p, demuxer %p\n", debugstr_w(url),
           stream, stream_size, mime_type, demuxer );

    if (!(params.context = stream_context_create( stream, stream_size ))) return STATUS_NO_MEMORY;

    if (url && (len = WideCharToMultiByte( CP_ACP, 0, url, -1, NULL, 0, NULL, NULL )) && (tmp = malloc( len )))
    {
        WideCharToMultiByte( CP_ACP, 0, url, -1, tmp, len, NULL, NULL );
        params.url = tmp;
    }
    status = UNIX_CALL( demuxer_create, &params );
    free( tmp );

    if (status)
    {
        WARN( "demuxer_create failed, status %#lx\n", status );
        stream_context_destroy( params.context );
        return status;
    }

    *duration = params.duration;
    *stream_count = params.stream_count;
    MultiByteToWideChar( CP_ACP, 0, params.mime_type, -1, mime_type, 256 );
    *demuxer = params.demuxer;
    TRACE( "created demuxer %#I64x, stream %p, duration %I64d, stream_count %u, mime_type %s\n",
           demuxer->handle, stream, params.duration, params.stream_count, debugstr_a(params.mime_type) );
    return STATUS_SUCCESS;
}

NTSTATUS CDECL winedmo_demuxer_destroy( struct winedmo_demuxer *demuxer )
{
    struct demuxer_destroy_params params = {.demuxer = *demuxer};
    NTSTATUS status;

    if (!demuxer->handle) return STATUS_SUCCESS;

    TRACE( "demuxer %#I64x\n", demuxer->handle );

    demuxer->handle = 0;
    status = UNIX_CALL( demuxer_destroy, &params );
    if (status) WARN( "demuxer_destroy failed, status %#lx\n", status );
    else stream_context_destroy( params.context );

    return status;
}

NTSTATUS CDECL winedmo_demuxer_read( struct winedmo_demuxer demuxer, UINT *stream, DMO_OUTPUT_DATA_BUFFER *buffer, UINT *buffer_size )
{
    struct demuxer_read_params params = {.demuxer = demuxer};
    NTSTATUS status;
    DWORD capacity = 0;

    TRACE( "demuxer %#I64x, stream %p, buffer %p, buffer_size %p\n", demuxer.handle, stream, buffer, buffer_size );

    IMediaBuffer_GetMaxLength( buffer->pBuffer, &capacity );
    buffer_lock( buffer, &params.sample );
    status = UNIX_CALL( demuxer_read, &params );
    buffer_unlock( buffer, &params.sample, status );
    *buffer_size = params.sample.size;
    *stream = params.stream;

    if (!status && params.sample.size > capacity)
    {
        WARN( "Demuxer sample size %s exceeds buffer capacity %#lx.\n",
                wine_dbgstr_longlong( params.sample.size ), capacity );
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (status)
    {
        if (status == STATUS_END_OF_FILE) WARN( "Reached end of media file on demuxer %#I64x.\n", demuxer.handle );
        else if (status != STATUS_BUFFER_TOO_SMALL) ERR( "Failed to read sample, status %#lx\n", status );
        return status;
    }

    TRACE( "Got buffer %p, buffer_size %#x on stream %u\n", buffer->pBuffer, *buffer_size, *stream );
    return status;
}

NTSTATUS CDECL winedmo_demuxer_seek( struct winedmo_demuxer demuxer, INT64 timestamp )
{
    struct demuxer_seek_params params = {.demuxer = demuxer, .timestamp = timestamp};
    NTSTATUS status;

    TRACE( "demuxer %#I64x, timestamp %I64d\n", demuxer.handle, timestamp );

    if ((status = UNIX_CALL( demuxer_seek, &params )))
    {
        WARN( "Failed to set position, status %#lx\n", status );
        return status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS CDECL winedmo_demuxer_stream_lang( struct winedmo_demuxer demuxer, UINT stream, WCHAR *buffer, UINT len )
{
    struct demuxer_stream_lang_params params = {.demuxer = demuxer, .stream = stream};
    NTSTATUS status;

    TRACE( "demuxer %#I64x, stream %u\n", demuxer.handle, stream );

    if ((status = UNIX_CALL( demuxer_stream_lang, &params )))
    {
        WARN( "Failed to get stream lang, status %#lx\n", status );
        return status;
    }

    len = MultiByteToWideChar( CP_UTF8, 0, params.buffer, -1, buffer, len );
    buffer[len - 1] = 0;
    return STATUS_SUCCESS;
}

NTSTATUS CDECL winedmo_demuxer_stream_name( struct winedmo_demuxer demuxer, UINT stream, WCHAR *buffer, UINT len )
{
    struct demuxer_stream_name_params params = {.demuxer = demuxer, .stream = stream};
    NTSTATUS status;

    TRACE( "demuxer %#I64x, stream %u\n", demuxer.handle, stream );

    if ((status = UNIX_CALL( demuxer_stream_name, &params )))
    {
        WARN( "Failed to get stream name, status %#lx\n", status );
        return status;
    }

    len = MultiByteToWideChar( CP_UTF8, 0, params.buffer, -1, buffer, len );
    buffer[len - 1] = 0;
    return STATUS_SUCCESS;
}


static HRESULT get_media_type( UINT code, void *params, struct media_type *media_type,
                               GUID *major, union winedmo_format **format )
{
    NTSTATUS status;

    media_type->format = NULL;
    if ((status = WINE_UNIX_CALL( code, params )) && status == STATUS_BUFFER_TOO_SMALL)
    {
        if (!(media_type->format = malloc( media_type->format_size ))) return STATUS_NO_MEMORY;
        status = WINE_UNIX_CALL( code, params );
    }

    if (!status)
    {
        *major = media_type->major;
        *format = media_type->format;
        return STATUS_SUCCESS;
    }

    WARN( "Failed to get media type, code %#x, status %#lx\n", code, status );
    free( media_type->format );
    return status;
}

NTSTATUS CDECL winedmo_demuxer_stream_type( struct winedmo_demuxer demuxer, UINT stream,
                                            GUID *major, union winedmo_format **format )
{
    struct demuxer_stream_type_params params = {.demuxer = demuxer, .stream = stream};
    TRACE( "demuxer %#I64x, stream %u, major %p, format %p\n", demuxer.handle, stream, major, format );
    return get_media_type( unix_demuxer_stream_type, &params, &params.media_type, major, format );
}

/* -------------------------------------------------------------------------
 * winedmo_transform_* functions (from winedmo/main.c)
 * ------------------------------------------------------------------------- */

NTSTATUS CDECL winedmo_transform_create( GUID major_type,
                                         const union winedmo_format *input_format,  UINT32 input_format_size,
                                         const union winedmo_format *output_format, UINT32 output_format_size,
                                         struct winedmo_transform *transform )
{
    struct transform_create_params params =
    {
        .major_type         = major_type,
        .input_format       = (UINT_PTR)input_format,
        .input_format_size  = input_format_size,
        .output_format      = (UINT_PTR)output_format,
        .output_format_size = output_format_size,
    };
    NTSTATUS status;

    TRACE( "major %s input_format %p (size %u) output_format %p (size %u) transform %p\n",
           debugstr_guid( &major_type ), input_format, input_format_size,
           output_format, output_format_size, transform );

    if ((status = UNIX_CALL( transform_create, &params )))
    {
        WARN( "transform_create failed, status %#lx\n", status );
        return status;
    }

    *transform = params.transform;
    TRACE( "created transform %#I64x\n", transform->handle );
    return STATUS_SUCCESS;
}

NTSTATUS CDECL winedmo_transform_destroy( struct winedmo_transform transform )
{
    struct transform_destroy_params params = {.transform = transform};
    NTSTATUS status;

    TRACE( "transform %#I64x\n", transform.handle );
    if (!transform.handle) return STATUS_SUCCESS;

    if ((status = UNIX_CALL( transform_destroy, &params )))
        WARN( "transform_destroy failed, status %#lx\n", status );
    return status;
}

NTSTATUS CDECL winedmo_transform_push_input( struct winedmo_transform transform,
                                              const BYTE *data, UINT32 size,
                                              INT64 pts, INT64 dts, INT64 duration, DWORD flags )
{
    struct transform_push_input_params params =
    {
        .transform = transform,
        .data      = (UINT_PTR)data,
        .size      = size,
        .pts       = pts,
        .dts       = dts,
        .duration  = duration,
        .flags     = flags,
    };
    NTSTATUS status;

    TRACE( "transform %#I64x data %p size %u pts %I64d dts %I64d\n", transform.handle, data, size, pts, dts );

    if ((status = UNIX_CALL( transform_push_input, &params )) && status != STATUS_DEVICE_BUSY)
        WARN( "transform_push_input failed, status %#lx\n", status );
    return status;
}

NTSTATUS CDECL winedmo_transform_get_output( struct winedmo_transform transform,
                                              BYTE *data, UINT32 *size,
                                              INT64 *pts, INT64 *duration, DWORD *flags )
{
    struct transform_get_output_params params =
    {
        .transform = transform,
        .data      = (UINT_PTR)data,
        .size      = *size,
        .pts       = INT64_MIN,
        .duration  = INT64_MIN,
        .flags     = 0,
    };
    NTSTATUS status;

    TRACE( "transform %#I64x data %p size %u\n", transform.handle, data, *size );

    status = UNIX_CALL( transform_get_output, &params );
    *size     = params.size;
    *pts      = params.pts;
    *duration = params.duration;
    *flags    = params.flags;

    if (status && status != STATUS_MORE_PROCESSING_REQUIRED && status != STATUS_BUFFER_TOO_SMALL
               && status != STATUS_END_OF_FILE)
        WARN( "transform_get_output failed, status %#lx\n", status );
    return status;
}

NTSTATUS CDECL winedmo_transform_drain( struct winedmo_transform transform )
{
    struct transform_drain_params params = {.transform = transform};
    NTSTATUS status;

    TRACE( "transform %#I64x\n", transform.handle );
    WARN( "Draining transform %#I64x.\n", transform.handle );
    if ((status = UNIX_CALL( transform_drain, &params )))
        WARN( "transform_drain failed, status %#lx\n", status );
    return status;
}

NTSTATUS CDECL winedmo_transform_flush( struct winedmo_transform transform )
{
    struct transform_flush_params params = {.transform = transform};
    NTSTATUS status;

    TRACE( "transform %#I64x\n", transform.handle );
    if ((status = UNIX_CALL( transform_flush, &params )))
        WARN( "transform_flush failed, status %#lx\n", status );
    return status;
}

NTSTATUS CDECL winedmo_transform_get_output_format( struct winedmo_transform transform,
                                                     GUID *major, union winedmo_format **format )
{
    struct transform_get_output_format_params params = {.transform = transform};
    TRACE( "transform %#I64x major %p format %p\n", transform.handle, major, format );
    return get_media_type( unix_transform_get_output_format, &params, &params.media_type, major, format );
}

NTSTATUS CDECL winedmo_transform_set_output_format( struct winedmo_transform transform,
                                                     const union winedmo_format *format, UINT32 format_size )
{
    struct transform_set_output_format_params params =
    {
        .transform   = transform,
        .format      = (UINT_PTR)format,
        .format_size = format_size,
    };
    NTSTATUS status;

    TRACE( "transform %#I64x format %p size %u\n", transform.handle, format, format_size );
    if ((status = UNIX_CALL( transform_set_output_format, &params )))
        WARN( "transform_set_output_format failed, status %#lx\n", status );
    return status;
}


/* -------------------------------------------------------------------------
 * COM class factory
 * ------------------------------------------------------------------------- */

struct class_factory
{
    IClassFactory IClassFactory_iface;
    HRESULT (*create_instance)(IUnknown *outer, IUnknown **out);
};

static inline struct class_factory *impl_from_IClassFactory(IClassFactory *iface)
{
    return CONTAINING_RECORD(iface, struct class_factory, IClassFactory_iface);
}

static HRESULT WINAPI class_factory_QueryInterface(IClassFactory *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IClassFactory))
    {
        *out = iface;
        IClassFactory_AddRef(iface);
        return S_OK;
    }

    *out = NULL;
    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    return E_NOINTERFACE;
}

static ULONG WINAPI class_factory_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI class_factory_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI class_factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
    struct class_factory *factory = impl_from_IClassFactory(iface);
    IUnknown *unk;
    HRESULT hr;

    TRACE("iface %p, outer %p, iid %s, out %p.\n", iface, outer, debugstr_guid(iid), out);

    if (outer && !IsEqualGUID(iid, &IID_IUnknown))
        return E_NOINTERFACE;

    *out = NULL;
    if (SUCCEEDED(hr = factory->create_instance(outer, &unk)))
    {
        hr = IUnknown_QueryInterface(unk, iid, out);
        IUnknown_Release(unk);
    }
    return hr;
}

static HRESULT WINAPI class_factory_LockServer(IClassFactory *iface, BOOL lock)
{
    TRACE("iface %p, lock %d.\n", iface, lock);
    return S_OK;
}

static const IClassFactoryVtbl class_factory_vtbl =
{
    class_factory_QueryInterface,
    class_factory_AddRef,
    class_factory_Release,
    class_factory_CreateInstance,
    class_factory_LockServer,
};

static struct class_factory avi_splitter_cf = {{&class_factory_vtbl}, avi_splitter_create};
static struct class_factory asf_splitter_cf = {{&class_factory_vtbl}, asf_splitter_create};
static struct class_factory decodebin_parser_cf = {{&class_factory_vtbl}, decodebin_parser_create};
static struct class_factory aac_audio_decoder_cf = {{&class_factory_vtbl}, aac_audio_decoder_create};
static struct class_factory ac3_audio_decoder_cf = {{&class_factory_vtbl}, ac3_audio_decoder_create};
static struct class_factory mpeg_audio_codec_cf = {{&class_factory_vtbl}, mpeg_audio_codec_create};
static struct class_factory mpeg_video_codec_cf = {{&class_factory_vtbl}, mpeg_video_codec_create};
static struct class_factory mpeg_layer3_decoder_cf = {{&class_factory_vtbl}, mpeg_layer3_decoder_create};
static struct class_factory mpeg_splitter_cf = {{&class_factory_vtbl}, mpeg_splitter_create};
static struct class_factory wave_parser_cf = {{&class_factory_vtbl}, wave_parser_create};
static struct class_factory wma_decoder_cf = {{&class_factory_vtbl}, wma_decoder_create};
static struct class_factory wmv_decoder_cf = {{&class_factory_vtbl}, wmv_decoder_create};
static struct class_factory resampler_cf = {{&class_factory_vtbl}, resampler_create};
static struct class_factory color_convert_cf = {{&class_factory_vtbl}, color_convert_create};
static struct class_factory mp3_sink_class_factory_cf = {{&class_factory_vtbl}, mp3_sink_class_factory_create};
static struct class_factory mpeg4_sink_class_factory_cf = {{&class_factory_vtbl}, mpeg4_sink_class_factory_create};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    static const GUID CLSID_winedmo_avi_splitter = {0x272bfbfb,0x50d0,0x4078,{0xb6,0x00,0x1e,0x95,0x9c,0x30,0x13,0x37}};
    static const GUID CLSID_winedmo_asf_splitter = {0x6d3cd6e1,0x5862,0x4d7e,{0xb4,0xb7,0x25,0x27,0x6b,0x59,0x76,0x22}};
    static const GUID CLSID_winedmo_color_converter = {0xf47e2da5,0xe370,0x47b7,{0x90,0x3a,0x07,0x8d,0xdd,0x45,0xa5,0xcc}};
    static const GUID CLSID_winedmo_mp3_sink_factory = {0x1f302877,0xaaab,0x40a3,{0xb9,0xe0,0x9f,0x48,0xda,0xf3,0x5b,0xc8}};
    static const GUID CLSID_winedmo_mpeg4_sink_factory = {0x5d5407d9,0xc6ca,0x4770,{0xa7,0xcc,0x27,0xc0,0xcb,0x8a,0x76,0x27}};
    static const GUID CLSID_winedmo_mpeg_audio_decoder = {0xc9f285f8,0x4380,0x4121,{0x97,0x1f,0x49,0xa9,0x53,0x16,0xc2,0x7b}};
    static const GUID CLSID_winedmo_mpeg_video_decoder = {0x5ed2e5f6,0xbf3e,0x4180,{0x83,0xa4,0x48,0x47,0xcc,0x5b,0x4e,0xa3}};
    static const GUID CLSID_winedmo_resampler = {0x92f35e78,0x15a5,0x486b,{0x88,0x8e,0x57,0x5f,0x99,0x65,0x1c,0xe2}};
    static const GUID CLSID_winedmo_wma_decoder = {0x5b4d4e54,0x0620,0x4cf9,{0x94,0xae,0x78,0x23,0x96,0x5c,0x28,0xb6}};
    static const GUID CLSID_winedmo_wmv_decoder = {0x62ee5ddb,0x4f52,0x48e2,{0x89,0x28,0x78,0x7b,0x02,0x53,0xa0,0xbc}};
    static const GUID CLSID_winedmo_mp3_decoder = {0x84cd8e3e,0xb221,0x434a,{0x88,0x82,0x9d,0x6c,0x8d,0xf4,0x90,0xe1}};
    static const GUID CLSID_winedmo_mpeg1_splitter = {0xa8edbf98,0x2442,0x42c5,{0x85,0xa1,0xab,0x05,0xa5,0x80,0xdf,0x53}};
    static const GUID CLSID_winedmo_wave_parser = {0x3f839ec7,0x5ea6,0x49e1,{0x80,0xc2,0x1e,0xa3,0x00,0xf8,0xb0,0xe0}};
    struct class_factory *factory;
    HRESULT hr;

    TRACE("clsid %s, iid %s, out %p.\n", debugstr_guid(clsid), debugstr_guid(iid), out);

    if (SUCCEEDED(hr = mfplat_get_class_object(clsid, iid, out)))
        return hr;

    if (IsEqualGUID(clsid, &CLSID_winedmo_avi_splitter))
        factory = &avi_splitter_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_asf_splitter))
        factory = &asf_splitter_cf;
    else if (IsEqualGUID(clsid, &CLSID_decodebin_parser))
        factory = &decodebin_parser_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_aac_audio_decoder))
        factory = &aac_audio_decoder_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_ac3_audio_decoder))
        factory = &ac3_audio_decoder_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_mpeg_audio_decoder))
        factory = &mpeg_audio_codec_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_mpeg_video_decoder))
        factory = &mpeg_video_codec_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_mp3_decoder))
        factory = &mpeg_layer3_decoder_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_mpeg1_splitter))
        factory = &mpeg_splitter_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_wave_parser))
        factory = &wave_parser_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_wma_decoder))
        factory = &wma_decoder_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_wmv_decoder))
        factory = &wmv_decoder_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_resampler))
        factory = &resampler_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_color_converter))
        factory = &color_convert_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_mp3_sink_factory))
        factory = &mp3_sink_class_factory_cf;
    else if (IsEqualGUID(clsid, &CLSID_winedmo_mpeg4_sink_factory))
        factory = &mpeg4_sink_class_factory_cf;
    else
    {
        FIXME("%s not implemented, returning CLASS_E_CLASSNOTAVAILABLE.\n", debugstr_guid(clsid));
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    return IClassFactory_QueryInterface(&factory->IClassFactory_iface, iid, out);
}


static const REGPINTYPES reg_audio_mt = {&MEDIATYPE_Audio, &GUID_NULL};
static const REGPINTYPES reg_aac_decoder_inputs[] =
{
    {&MEDIATYPE_Audio, &MFAudioFormat_AAC},
    {&MEDIATYPE_Audio, &MFAudioFormat_ADTS},
    {&MEDIATYPE_Audio, &MEDIASUBTYPE_RAW_AAC1},
    {&MEDIATYPE_Audio, &MEDIASUBTYPE_FRAUNHOFER_IIS_MPEG2_AAC},
    {&MEDIATYPE_Audio, &MEDIASUBTYPE_MPEG_RAW_AAC},
    {&MEDIATYPE_Audio, &MEDIASUBTYPE_NOKIA_MPEG_ADTS_AAC},
    {&MEDIATYPE_Audio, &MEDIASUBTYPE_NOKIA_MPEG_RAW_AAC},
    {&MEDIATYPE_Audio, &MEDIASUBTYPE_VODAFONE_MPEG_ADTS_AAC},
    {&MEDIATYPE_Audio, &MEDIASUBTYPE_VODAFONE_MPEG_RAW_AAC},
    {&MEDIATYPE_Audio, &MEDIASUBTYPE_MPEG4_AAC},
};
static const REGPINTYPES reg_ac3_decoder_inputs[] =
{
    {&MEDIATYPE_Audio, &MFAudioFormat_Dolby_AC3},
    {&MEDIATYPE_Audio, &MFAudioFormat_Dolby_AC3_SPDIF},
};
static const REGPINTYPES reg_pcm_audio_mt = {&MEDIATYPE_Audio, &MEDIASUBTYPE_PCM};
static const REGPINTYPES reg_asf_stream_mt = {&MEDIATYPE_Stream, &MEDIASUBTYPE_Asf};
static const REGPINTYPES reg_stream_mt = {&MEDIATYPE_Stream, &GUID_NULL};
static const REGPINTYPES reg_video_mt = {&MEDIATYPE_Video, &GUID_NULL};

static const REGFILTERPINS2 reg_asf_splitter_pins[3] =
{
    {
        .nMediaTypes = 1,
        .lpMediaType = &reg_asf_stream_mt,
    },
    {
        .dwFlags = REG_PINFLAG_B_ZERO | REG_PINFLAG_B_OUTPUT,
        .nMediaTypes = 1,
        .lpMediaType = &reg_audio_mt,
    },
    {
        .dwFlags = REG_PINFLAG_B_ZERO | REG_PINFLAG_B_OUTPUT,
        .nMediaTypes = 1,
        .lpMediaType = &reg_video_mt,
    },
};

static const REGFILTER2 reg_asf_splitter =
{
    .dwVersion = 2,
    .dwMerit = MERIT_NORMAL,
    .u.s2.cPins2 = 3,
    .u.s2.rgPins2 = reg_asf_splitter_pins,
};

static const REGFILTERPINS2 reg_aac_decoder_pins[2] =
{
    {
        .nMediaTypes = ARRAY_SIZE(reg_aac_decoder_inputs),
        .lpMediaType = reg_aac_decoder_inputs,
    },
    {
        .dwFlags = REG_PINFLAG_B_OUTPUT,
        .nMediaTypes = 1,
        .lpMediaType = &reg_pcm_audio_mt,
    },
};

static const REGFILTER2 reg_aac_decoder =
{
    .dwVersion = 2,
    .dwMerit = MERIT_NORMAL,
    .u.s2.cPins2 = 2,
    .u.s2.rgPins2 = reg_aac_decoder_pins,
};

static const REGFILTERPINS2 reg_ac3_decoder_pins[2] =
{
    {
        .nMediaTypes = ARRAY_SIZE(reg_ac3_decoder_inputs),
        .lpMediaType = reg_ac3_decoder_inputs,
    },
    {
        .dwFlags = REG_PINFLAG_B_OUTPUT,
        .nMediaTypes = 1,
        .lpMediaType = &reg_pcm_audio_mt,
    },
};

static const REGFILTER2 reg_ac3_decoder =
{
    .dwVersion = 2,
    .dwMerit = MERIT_NORMAL,
    .u.s2.cPins2 = 2,
    .u.s2.rgPins2 = reg_ac3_decoder_pins,
};

static const REGFILTERPINS2 reg_decodebin_parser_pins[3] =
{
    {
        .nMediaTypes = 1,
        .lpMediaType = &reg_stream_mt,
    },
    {
        .dwFlags = REG_PINFLAG_B_OUTPUT,
        .nMediaTypes = 1,
        .lpMediaType = &reg_audio_mt,
    },
    {
        .dwFlags = REG_PINFLAG_B_OUTPUT,
        .nMediaTypes = 1,
        .lpMediaType = &reg_video_mt,
    },
};

static const REGFILTER2 reg_decodebin_parser =
{
    .dwVersion = 2,
    .dwMerit = MERIT_NORMAL - 1,
    .u.s2.cPins2 = 3,
    .u.s2.rgPins2 = reg_decodebin_parser_pins,
};

HRESULT WINAPI DllRegisterServer(void)
{
    IFilterMapper2 *mapper;
    HRESULT hr;

    TRACE(".\n");

    if (FAILED(hr = __wine_register_resources()))
        return hr;

    if (FAILED(hr = CoCreateInstance(&CLSID_FilterMapper2, NULL, CLSCTX_INPROC_SERVER,
            &IID_IFilterMapper2, (void **)&mapper)))
        return hr;

    IFilterMapper2_RegisterFilter(mapper, &CLSID_decodebin_parser,
            L"winedmo splitter filter", NULL, NULL, NULL, &reg_decodebin_parser);
    IFilterMapper2_RegisterFilter(mapper, &CLSID_winedmo_asf_splitter,
            L"winedmo ASF splitter", NULL, NULL, NULL, &reg_asf_splitter);
    IFilterMapper2_RegisterFilter(mapper, &CLSID_winedmo_aac_audio_decoder,
            L"winedmo AAC decoder", NULL, NULL, NULL, &reg_aac_decoder);
    IFilterMapper2_RegisterFilter(mapper, &CLSID_winedmo_ac3_audio_decoder,
            L"winedmo AC3 decoder", NULL, NULL, NULL, &reg_ac3_decoder);

    IFilterMapper2_Release(mapper);

    return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    IFilterMapper2 *mapper;
    HRESULT hr;

    TRACE(".\n");

    if (FAILED(hr = __wine_unregister_resources()))
        return hr;

    if (FAILED(hr = CoCreateInstance(&CLSID_FilterMapper2, NULL, CLSCTX_INPROC_SERVER,
            &IID_IFilterMapper2, (void **)&mapper)))
        return hr;

    IFilterMapper2_UnregisterFilter(mapper, NULL, NULL, &CLSID_decodebin_parser);
    IFilterMapper2_UnregisterFilter(mapper, NULL, NULL, &CLSID_winedmo_asf_splitter);
    IFilterMapper2_UnregisterFilter(mapper, NULL, NULL, &CLSID_winedmo_aac_audio_decoder);
    IFilterMapper2_UnregisterFilter(mapper, NULL, NULL, &CLSID_winedmo_ac3_audio_decoder);

    IFilterMapper2_Release(mapper);

    return S_OK;
}
