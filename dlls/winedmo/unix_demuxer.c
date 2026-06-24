/*
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

#if 0
#pragma makedep unix
#endif

#include "config.h"
#include "unix_private.h"

#include "wine/debug.h"

#ifdef HAVE_FFMPEG

WINE_DEFAULT_DEBUG_CHANNEL(dmo);

static inline const char *debugstr_averr( int err )
{
    return wine_dbg_sprintf( "%d (%s)", err, av_err2str(err) );
}

struct stream
{
    AVBSFContext *filter;
    BOOL eos;
    BOOL vc1_asf_startcode_fallback;
    INT64 timestamp_base;
    BOOL timestamp_base_set;
    INT64 last_pts;
    INT64 next_pts;
    INT64 final_last_pts;
    INT64 final_next_pts;
    UINT final_repeated_pts;
};

struct demuxer
{
    AVFormatContext *ctx;
    struct stream_context *stream_context;
    struct stream *streams;
    INT64 duration;

    AVPacket *last_packet; /* last read packet */
    struct stream *last_stream; /* last read packet stream */
};

static struct demuxer *get_demuxer( struct winedmo_demuxer demuxer )
{
    return (struct demuxer *)(UINT_PTR)demuxer.handle;
}

static INT64 get_user_time( INT64 time, AVRational time_base )
{
    static const AVRational USER_TIME_BASE_Q = {1, 10000000};
    return av_rescale_q_rnd( time, time_base, USER_TIME_BASE_Q, AV_ROUND_PASS_MINMAX );
}

static INT64 get_stream_time( const AVStream *stream, INT64 time )
{
    if (stream->time_base.num && stream->time_base.den) return get_user_time( time, stream->time_base );
    return get_user_time( time, AV_TIME_BASE_Q );
}

static UINT64 read_le64( const BYTE *data )
{
    return (UINT64)data[0] | ((UINT64)data[1] << 8) | ((UINT64)data[2] << 16) | ((UINT64)data[3] << 24)
            | ((UINT64)data[4] << 32) | ((UINT64)data[5] << 40) | ((UINT64)data[6] << 48) | ((UINT64)data[7] << 56);
}

static UINT32 read_le32( const BYTE *data )
{
    return (UINT32)data[0] | ((UINT32)data[1] << 8) | ((UINT32)data[2] << 16)
            | ((UINT32)data[3] << 24);
}

static BOOL read_stream_at( struct stream_context *context, UINT64 offset, BYTE *buffer, UINT32 size )
{
    UINT64 pos = context->position;
    int ret;

    if (context->length != (UINT64)-1 && (offset > context->length || size > context->length - offset))
        return FALSE;
    if (unix_seek_callback( context, offset, SEEK_SET ) < 0)
        return FALSE;

    ret = unix_read_callback( context, buffer, size );
    if (unix_seek_callback( context, pos, SEEK_SET ) < 0)
        return FALSE;

    return ret == size;
}

static INT64 get_asf_header_duration( AVFormatContext *ctx )
{
    static const BYTE asf_header_guid[16] =
    {
        0x30, 0x26, 0xb2, 0x75, 0x8e, 0x66, 0xcf, 0x11,
        0xa6, 0xd9, 0x00, 0xaa, 0x00, 0x62, 0xce, 0x6c
    };
    static const BYTE asf_file_properties_guid[16] =
    {
        0xa1, 0xdc, 0xab, 0x8c, 0x47, 0xa9, 0xcf, 0x11,
        0x8e, 0xe4, 0x00, 0xc0, 0x0c, 0x20, 0x53, 0x65
    };
    struct stream_context *context = ctx->pb ? ctx->pb->opaque : NULL;
    BYTE header[30], object[104];
    UINT64 header_size, offset;
    UINT32 object_count, i;

    if (!context || !ctx->iformat || !strstr( ctx->iformat->name, "asf" ))
        return AV_NOPTS_VALUE;
    if (!read_stream_at( context, 0, header, sizeof(header) ))
        return AV_NOPTS_VALUE;
    if (memcmp( header, asf_header_guid, sizeof(asf_header_guid) ))
        return AV_NOPTS_VALUE;

    header_size = read_le64( header + 16 );
    object_count = read_le32( header + 24 );
    if (header_size < sizeof(header) || (context->length != (UINT64)-1 && header_size > context->length))
        return AV_NOPTS_VALUE;

    for (i = 0, offset = sizeof(header); i < object_count && offset + 24 <= header_size; ++i)
    {
        BYTE object_header[24];
        UINT64 object_size, play_duration, preroll;

        if (!read_stream_at( context, offset, object_header, sizeof(object_header) ))
            return AV_NOPTS_VALUE;
        object_size = read_le64( object_header + 16 );
        if (object_size < sizeof(object_header) || object_size > header_size - offset)
            return AV_NOPTS_VALUE;

        if (!memcmp( object_header, asf_file_properties_guid, sizeof(asf_file_properties_guid) )
                && object_size >= sizeof(object))
        {
            if (!read_stream_at( context, offset, object, sizeof(object) ))
                return AV_NOPTS_VALUE;

            play_duration = read_le64( object + 64 );
            preroll = read_le64( object + 80 );
            if (play_duration > preroll * 10000)
            {
                TRACE( "ASF header duration play %s preroll %s duration %s\n",
                        wine_dbgstr_longlong( play_duration ), wine_dbgstr_longlong( preroll ),
                        wine_dbgstr_longlong( play_duration - preroll * 10000 ) );
                return play_duration - preroll * 10000;
            }
            return AV_NOPTS_VALUE;
        }

        offset += object_size;
    }

    return AV_NOPTS_VALUE;
}

static void normalize_stream_timestamps( struct stream *stream, struct sample *sample )
{
    INT64 base = INT64_MIN;

    if (!stream->timestamp_base_set)
    {
        if (sample->pts != AV_NOPTS_VALUE && sample->dts != AV_NOPTS_VALUE)
            base = min( sample->pts, sample->dts );
        else if (sample->pts != AV_NOPTS_VALUE)
            base = sample->pts;
        else if (sample->dts != AV_NOPTS_VALUE)
            base = sample->dts;

        /* Some ASF streams expose the first compressed sample with a large positive
         * timestamp even though the clip itself starts at zero. Preserve normal
         * small startup offsets, but rebase obviously late first samples so MF does
         * not hold video for the first second or more of the clip. */
        if (base != INT64_MIN && base > 2000000)
        {
            stream->timestamp_base = base;
            stream->timestamp_base_set = TRUE;
        }
    }

    if (!stream->timestamp_base_set)
        return;

    if (sample->pts != AV_NOPTS_VALUE)
        sample->pts = max( 0, sample->pts - stream->timestamp_base );
    if (sample->dts != AV_NOPTS_VALUE)
        sample->dts = max( 0, sample->dts - stream->timestamp_base );
}

static void fixup_asf_mpeg4_timestamps( const AVFormatContext *ctx, AVStream *avstream,
        struct stream *stream, struct sample *sample )
{
    const AVDictionaryEntry *entry;
    INT64 duration = sample->duration;
    AVRational frame_rate;

    if (!ctx->iformat || !strstr( ctx->iformat->name, "asf" )) return;
    if (avstream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) return;
    if (avstream->codecpar->codec_id != AV_CODEC_ID_MPEG4) return;
    if (!(entry = av_dict_get( ctx->metadata, "WMFSDKVersion", NULL, 0 )) ||
        strcmp( entry->value, "12.0.7601.17514" ))
        return;
    if (!av_dict_get( ctx->metadata, "Buffer Average", NULL, 0 )) return;

    if (duration <= 0 || duration == AV_NOPTS_VALUE)
    {
        frame_rate = av_guess_frame_rate( (AVFormatContext *)ctx, avstream, NULL );
        if (frame_rate.num > 0 && frame_rate.den > 0)
            duration = av_rescale_q( 1, av_inv_q( frame_rate ), (AVRational){1, 10000000} );
    }
    if (duration <= 0 || duration == AV_NOPTS_VALUE)
        duration = 333333;

    if (stream->next_pts == INT64_MIN)
    {
        if (sample->pts == AV_NOPTS_VALUE)
            sample->pts = 0;
        if (sample->dts == AV_NOPTS_VALUE || sample->dts <= sample->pts)
            sample->dts = sample->pts;
        stream->last_pts = sample->pts;
        stream->next_pts = sample->pts + duration;
        return;
    }

    if (sample->pts == AV_NOPTS_VALUE || sample->pts <= stream->last_pts)
        sample->pts = stream->next_pts;
    if (sample->dts == AV_NOPTS_VALUE || sample->dts <= stream->last_pts)
        sample->dts = sample->pts;

    stream->last_pts = sample->pts;
    stream->next_pts = sample->pts + duration;
}

static void fixup_asf_mpeg4_final_timestamps( const AVFormatContext *ctx, AVStream *avstream,
        struct stream *stream, struct sample *sample )
{
    const AVDictionaryEntry *entry;
    INT64 duration = sample->duration;
    AVRational frame_rate;

    if (!ctx->iformat || !strstr( ctx->iformat->name, "asf" )) return;
    if (avstream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) return;
    if (avstream->codecpar->codec_id != AV_CODEC_ID_MPEG4) return;
    if (!(entry = av_dict_get( ctx->metadata, "WMFSDKVersion", NULL, 0 )) ||
        strcmp( entry->value, "12.0.7601.17514" ))
        return;
    if (!av_dict_get( ctx->metadata, "Buffer Average", NULL, 0 )) return;

    if (duration <= 0 || duration == AV_NOPTS_VALUE)
    {
        frame_rate = av_guess_frame_rate( (AVFormatContext *)ctx, avstream, NULL );
        if (frame_rate.num > 0 && frame_rate.den > 0)
            duration = av_rescale_q( 1, av_inv_q( frame_rate ), (AVRational){1, 10000000} );
    }
    if (duration <= 0 || duration == AV_NOPTS_VALUE)
        duration = 333333;

    if (stream->final_next_pts == INT64_MIN)
    {
        stream->final_last_pts = sample->pts;
        stream->final_next_pts = sample->pts != AV_NOPTS_VALUE ? sample->pts + duration : duration;
        return;
    }

    if (sample->pts != AV_NOPTS_VALUE && sample->pts > stream->final_last_pts)
    {
        stream->final_repeated_pts = 0;
        stream->final_last_pts = sample->pts;
        stream->final_next_pts = sample->pts + duration;
        return;
    }

    if (++stream->final_repeated_pts < 5)
        return;

    sample->pts = stream->final_next_pts;
    if (sample->dts == AV_NOPTS_VALUE || sample->dts <= stream->final_last_pts)
        sample->dts = sample->pts;

    stream->final_last_pts = sample->pts;
    stream->final_next_pts = sample->pts + duration;
    stream->final_repeated_pts = 0;
}

static AVRational get_mpeg_sequence_frame_rate( const AVCodecParameters *params )
{
    static const AVRational rates[] =
    {
        {0, 1},
        {24000, 1001},
        {24, 1},
        {25, 1},
        {30000, 1001},
        {30, 1},
        {50, 1},
        {60000, 1001},
        {60, 1},
    };
    unsigned int i;

    if (params->codec_id != AV_CODEC_ID_MPEG1VIDEO && params->codec_id != AV_CODEC_ID_MPEG2VIDEO)
        return (AVRational){0, 1};

    for (i = 0; i + 7 < params->extradata_size; ++i)
    {
        if (params->extradata[i] == 0x00 && params->extradata[i + 1] == 0x00
                && params->extradata[i + 2] == 0x01 && params->extradata[i + 3] == 0xb3)
        {
            unsigned int frame_rate_code = params->extradata[i + 7] & 0x0f;

            if (frame_rate_code < ARRAY_SIZE(rates))
                return rates[frame_rate_code];
            break;
        }
    }

    return (AVRational){0, 1};
}

static AVRational get_video_display_frame_rate( const AVFormatContext *ctx, AVStream *avstream )
{
    AVRational frame_rate = avstream->avg_frame_rate;

    if (ctx->iformat && (!strcmp( ctx->iformat->name, "mpegvideo" )
            || !strcmp( ctx->iformat->name, "mpeg" )))
    {
        AVRational header_rate;

        frame_rate = av_guess_frame_rate( (AVFormatContext *)ctx, avstream, NULL );
        if (frame_rate.num <= 0 || frame_rate.den <= 0
                || (frame_rate.num < frame_rate.den * 5
                    && avstream->avg_frame_rate.num > 0 && avstream->avg_frame_rate.den > 0))
            frame_rate = avstream->avg_frame_rate;
        if (frame_rate.num <= 0 || frame_rate.den <= 0
                || (frame_rate.num < frame_rate.den * 5
                    && avstream->r_frame_rate.num > 0 && avstream->r_frame_rate.den > 0))
            frame_rate = avstream->r_frame_rate;
        if (frame_rate.num > 0 && frame_rate.den > 0 && frame_rate.num >= frame_rate.den * 5)
            return frame_rate;
        if ((header_rate = get_mpeg_sequence_frame_rate( avstream->codecpar )).num > 0)
            return header_rate;
        return frame_rate;
    }

    if (frame_rate.num <= 0 || frame_rate.den <= 0)
        frame_rate = av_guess_frame_rate( (AVFormatContext *)ctx, avstream, NULL );
    if (frame_rate.num <= 0 || frame_rate.den <= 0)
        frame_rate = avstream->r_frame_rate;

    return frame_rate;
}

static INT64 get_video_frame_duration( const AVFormatContext *ctx, AVStream *avstream )
{
    AVRational frame_rate = get_video_display_frame_rate( ctx, avstream );

    if (frame_rate.num <= 0 || frame_rate.den <= 0)
        return INT64_MIN;
    return av_rescale_q( 1, av_inv_q( frame_rate ), (AVRational){1, 10000000} );
}

static BOOL context_has_video_missing_frame_rate( const AVFormatContext *ctx )
{
    UINT i;

    for (i = 0; i < ctx->nb_streams; i++)
    {
        AVStream *stream = ctx->streams[i];
        AVRational frame_rate;

        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;

        frame_rate = get_video_display_frame_rate( ctx, stream );
        if (frame_rate.num <= 0 || frame_rate.den <= 0)
            return TRUE;
    }

    return FALSE;
}

static INT64 get_raw_mpegvideo_duration( const AVFormatContext *ctx )
{
    struct stream_context *context = ctx->pb ? ctx->pb->opaque : NULL;
    const UINT32 buffer_size = 64 * 1024;
    UINT64 offset, length;
    AVStream *video = NULL;
    UINT64 picture_count = 0;
    INT64 frame_duration;
    BYTE *buffer;
    UINT32 state = 0xffffffff;
    unsigned int i, j;

    if (!ctx->iformat || strcmp( ctx->iformat->name, "mpegvideo" )) return AV_NOPTS_VALUE;
    if (!context || context->length == (UINT64)-1) return AV_NOPTS_VALUE;

    for (i = 0; i < ctx->nb_streams; ++i)
    {
        AVStream *stream = ctx->streams[i];

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            video = stream;
            break;
        }
    }
    if (!video) return AV_NOPTS_VALUE;
    if ((frame_duration = get_video_frame_duration( ctx, video )) <= 0) return AV_NOPTS_VALUE;
    if (!(buffer = malloc( buffer_size ))) return AV_NOPTS_VALUE;

    length = context->length;
    for (offset = 0; offset < length; offset += buffer_size)
    {
        UINT32 chunk = (UINT32)min( (UINT64)buffer_size, length - offset );

        if (!read_stream_at( context, offset, buffer, chunk ))
        {
            free( buffer );
            return AV_NOPTS_VALUE;
        }

        for (j = 0; j < chunk; ++j)
        {
            state = (state << 8) | buffer[j];
            if (state == 0x00000100)
                ++picture_count;
        }
    }

    free( buffer );
    if (!picture_count) return AV_NOPTS_VALUE;

    TRACE( "raw MPEG video picture count %s, frame duration %s, duration %s.\n",
            wine_dbgstr_longlong( (LONGLONG)picture_count ), wine_dbgstr_longlong( frame_duration ),
            wine_dbgstr_longlong( (LONGLONG)(picture_count * frame_duration) ) );

    return picture_count * frame_duration;
}

static NTSTATUS raw_mpegvideo_seek( struct demuxer *demuxer, INT64 timestamp )
{
    struct stream_context *context = demuxer->stream_context;
    const UINT32 buffer_size = 64 * 1024;
    UINT64 target, offset, length, last_gop = 0, last_picture = 0;
    BYTE *buffer;
    UINT32 state = 0xffffffff;
    unsigned int i;

    if (!demuxer->ctx->iformat || strcmp( demuxer->ctx->iformat->name, "mpegvideo" ))
        return STATUS_NOT_SUPPORTED;
    if (!context || context->length == (UINT64)-1 || demuxer->duration <= 0)
        return STATUS_NOT_SUPPORTED;

    length = context->length;
    target = min( length, (UINT64)av_rescale( timestamp, length, demuxer->duration ) );
    if (!(buffer = malloc( buffer_size ))) return STATUS_NO_MEMORY;

    for (offset = 0; offset < target; offset += buffer_size)
    {
        UINT32 chunk = (UINT32)min( (UINT64)buffer_size, target - offset );

        if (!read_stream_at( context, offset, buffer, chunk ))
        {
            free( buffer );
            return STATUS_UNSUCCESSFUL;
        }

        for (i = 0; i < chunk; ++i)
        {
            state = (state << 8) | buffer[i];
            if (state == 0x000001b8)
                last_gop = offset + i - 3;
            else if (state == 0x00000100)
                last_picture = offset + i - 3;
        }
    }

    free( buffer );
    offset = last_gop ? last_gop : last_picture;
    TRACE( "raw MPEG video seek timestamp %s, target offset %s, seek offset %s.\n",
            wine_dbgstr_longlong( timestamp ), wine_dbgstr_longlong( target ), wine_dbgstr_longlong( offset ) );

    avformat_flush( demuxer->ctx );
    if (avio_seek( demuxer->ctx->pb, offset, SEEK_SET ) < 0)
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}

static void fixup_asf_vc1_timestamps( const AVFormatContext *ctx, AVStream *avstream,
        struct stream *stream, struct sample *sample )
{
    INT64 duration;

    if (!ctx->iformat || !strstr( ctx->iformat->name, "asf" )) return;
    if (avstream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) return;
    if (avstream->codecpar->codec_id != AV_CODEC_ID_VC1) return;

    duration = sample->duration;
    if (duration <= 0 || duration == AV_NOPTS_VALUE)
        duration = get_video_frame_duration( ctx, avstream );
    if (duration <= 0 || duration == AV_NOPTS_VALUE)
        duration = 333333;

    sample->duration = duration;

    if (sample->pts != AV_NOPTS_VALUE && (stream->next_pts == INT64_MIN || sample->pts > stream->last_pts))
    {
        if (sample->dts != AV_NOPTS_VALUE && sample->dts > sample->pts)
            sample->dts = AV_NOPTS_VALUE;
        stream->last_pts = sample->pts;
        stream->next_pts = sample->pts + sample->duration;
        return;
    }

    if (sample->dts != AV_NOPTS_VALUE)
    {
        if (stream->next_pts != INT64_MIN && sample->dts <= stream->last_pts)
            sample->pts = stream->next_pts;
        else
            sample->pts = sample->dts;
        sample->dts = sample->pts;
        stream->last_pts = sample->pts;
        stream->next_pts = sample->pts + sample->duration;
        return;
    }

    if (stream->next_pts == INT64_MIN)
    {
        sample->pts = 0;
        sample->dts = AV_NOPTS_VALUE;
        stream->last_pts = sample->pts;
        stream->next_pts = sample->pts + sample->duration;
        return;
    }

    if (sample->pts == AV_NOPTS_VALUE || sample->pts - stream->last_pts < sample->duration * 3 / 4)
        sample->pts = stream->next_pts;
    /* ASF packet DTS values for these VC1 streams can restart independently of
     * the generated presentation timeline. Do not expose them as MF decode
     * timestamps; the decoder can derive decode order from the bitstream. */
    sample->dts = AV_NOPTS_VALUE;

    stream->last_pts = sample->pts;
    stream->next_pts = sample->pts + sample->duration;

}

static INT64 get_context_duration( const AVFormatContext *ctx )
{
    INT64 i, max_duration = AV_NOPTS_VALUE, max_video_duration = AV_NOPTS_VALUE;
    INT64 duration;

    if (ctx->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE)
    {
        duration = get_asf_header_duration( (AVFormatContext *)ctx );
        if (duration != AV_NOPTS_VALUE)
            return duration;

        duration = get_raw_mpegvideo_duration( ctx );
        if (duration != AV_NOPTS_VALUE)
            return duration;

        return AV_NOPTS_VALUE;
    }

    for (i = 0; i < ctx->nb_streams; i++)
    {
        const AVStream *stream = ctx->streams[i];
        INT64 duration = get_stream_time( stream, stream->duration );
        if (duration == AV_NOPTS_VALUE) continue;
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            if (duration >= max_video_duration) max_video_duration = duration;
            if (max_video_duration == AV_NOPTS_VALUE) max_video_duration = duration;
        }
        if (duration >= max_duration) max_duration = duration;
        if (max_duration == AV_NOPTS_VALUE) max_duration = duration;
    }

    if (max_video_duration != AV_NOPTS_VALUE) return max_video_duration;
    if (max_duration == AV_NOPTS_VALUE) return get_user_time( ctx->duration, AV_TIME_BASE_Q );
    return max_duration;
}

NTSTATUS demuxer_check( void *arg )
{
    struct demuxer_check_params *params = arg;
    const AVInputFormat *format = NULL;

    if (!strcmp( params->mime_type, "video/mp4" )) format = av_find_input_format( "mp4" );
    else if (!strcmp( params->mime_type, "video/avi" )) format = av_find_input_format( "avi" );
    else if (!strcmp( params->mime_type, "audio/wav" )) format = av_find_input_format( "wav" );
    else if (!strcmp( params->mime_type, "audio/x-ms-wma" )) format = av_find_input_format( "asf" );
    else if (!strcmp( params->mime_type, "video/x-ms-wmv" )) format = av_find_input_format( "asf" );
    else if (!strcmp( params->mime_type, "video/x-ms-asf" )) format = av_find_input_format( "asf" );
    else if (!strcmp( params->mime_type, "video/mpeg" )) format = av_find_input_format( "mpeg" );
    else if (!strcmp( params->mime_type, "audio/mp3" )) format = av_find_input_format( "mp3" );

    if (format) TRACE( "Found format %s (%s)\n", format->name, format->long_name );
    else WARN( "Found MIME type %s\n", debugstr_a(params->mime_type) );

    return STATUS_SUCCESS;
}

static BOOL codec_is_big_endian_pcm(enum AVCodecID codec_id)
{
    switch (codec_id)
    {
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S24BE:
    case AV_CODEC_ID_PCM_S32BE:
    case AV_CODEC_ID_PCM_S64BE:
    case AV_CODEC_ID_PCM_F32BE:
    case AV_CODEC_ID_PCM_F64BE:
        return TRUE;
    default:
        return FALSE;
    }
}

static NTSTATUS demuxer_create_streams( struct demuxer *demuxer )
{
    UINT i;

    for (i = 0; i < demuxer->ctx->nb_streams; i++)
    {
        AVCodecParameters *par = demuxer->ctx->streams[i]->codecpar;
        struct stream *stream = demuxer->streams + i;
        const AVBitStreamFilter *filter;

        stream->last_pts = INT64_MIN;
        stream->next_pts = INT64_MIN;
        stream->final_last_pts = INT64_MIN;
        stream->final_next_pts = INT64_MIN;
        stream->final_repeated_pts = 0;
        if (par->codec_id == AV_CODEC_ID_H264)
        {
            if (!(filter = av_bsf_get_by_name( "h264_mp4toannexb" )))
                ERR( "Failed to find H264 bitstream filter\n" );
            else
            {
                if (av_bsf_alloc( filter, &stream->filter ) < 0) return STATUS_UNSUCCESSFUL;
                avcodec_parameters_copy( stream->filter->par_in, par );
                if (av_bsf_init( stream->filter ) < 0) return STATUS_UNSUCCESSFUL;
                continue;
            }
        }
        else if (par->codec_id == AV_CODEC_ID_VC1 && demuxer->ctx->iformat &&
                 strstr( demuxer->ctx->iformat->name, "asf" ))
        {
            if (!(filter = av_bsf_get_by_name( "vc1_asftorcv" )))
            {
                TRACE( "VC1 ASF bitstream filter unavailable, relying on demuxed packet normalization\n" );
            }
            else
            {
                if (av_bsf_alloc( filter, &stream->filter ) < 0) return STATUS_UNSUCCESSFUL;
                avcodec_parameters_copy( stream->filter->par_in, par );
                if (av_bsf_init( stream->filter ) < 0) return STATUS_UNSUCCESSFUL;
                continue;
            }
        }
        else if (codec_is_big_endian_pcm(par->codec_id))
        {
            /* WAVEFORMATEX does not contain endianness info, so this needs to be converted here. */
            if (av_bsf_alloc( &ff_pcm_byte_order_reverse_bsf.p, &stream->filter ) < 0) return STATUS_UNSUCCESSFUL;
            avcodec_parameters_copy( stream->filter->par_in, par );
            if (av_bsf_init( stream->filter ) < 0) return STATUS_UNSUCCESSFUL;
            continue;
        }

        av_bsf_get_null_filter( &stream->filter );
        avcodec_parameters_copy( stream->filter->par_in, demuxer->ctx->streams[i]->codecpar );
        avcodec_parameters_copy( stream->filter->par_out, demuxer->ctx->streams[i]->codecpar );
    }

    return STATUS_SUCCESS;
}

static int next_mov_atom( struct stream_context *context, UINT32 *type, UINT64 *size )
{
    struct
    {
        UINT32 size;
        UINT32 type;
    } atom;
    int ret;

    if ((ret = unix_read_callback( context, (uint8_t *)&atom, sizeof(atom) )) < 0) return ret;
    if (!(*size = RtlUlongByteSwap( atom.size )) || (*size > 1 && *size < sizeof(atom))) return -1;
    if (*size == 1 && (ret = unix_read_callback( context, (uint8_t *)size, sizeof(*size) )) < 0) return ret;
    *size -= sizeof(atom);
    *type = atom.type;
    return 0;
}

static void parse_stream_names( struct demuxer *demuxer, UINT32 root, UINT64 size, int index )
{
    struct stream_context *context = demuxer->ctx->pb->opaque;
    UINT64 end = context->position + size;
    UINT32 atom;
    char *name;

    TRACE( "demuxer %p, root %s\n", demuxer, debugstr_fourcc(root) );

    while (context->position < end && !next_mov_atom( context, &atom, &size ))
    {
#define CASE(l,h) (((UINT64)(h) << 32) | (l))
        switch (CASE(root, atom))
        {
        case CASE(MAKEFOURCC('r','o','o','t'), MAKEFOURCC('m','o','o','v')):
            parse_stream_names( demuxer, atom, size, 0 );
            break;
        case CASE(MAKEFOURCC('m','o','o','v'), MAKEFOURCC('t','r','a','k')):
            parse_stream_names( demuxer, atom, size, index++ );
            break;
        case CASE(MAKEFOURCC('t','r','a','k'), MAKEFOURCC('u','d','t','a')):
            parse_stream_names( demuxer, atom, size, index );
            break;
        case CASE(MAKEFOURCC('u','d','t','a'), MAKEFOURCC('n','a','m','e')):
            if ((name = calloc( 1, size + 1 )))
            {
                unix_read_callback( context, (uint8_t *)name, size );
                TRACE( "found name %s for stream %u\n", debugstr_a(name), index );
                av_dict_set( &demuxer->ctx->streams[index]->metadata, "name", name, 0 );
                free( name );
                break;
            }
            /* fallthrough */
        default:
            unix_seek_callback( context, size, SEEK_CUR );
            break;
#undef CASE
        }
    }
}

static void parse_mp4_streams_metadata( struct demuxer *demuxer )
{
    struct stream_context *context = demuxer->ctx->pb->opaque;
    int64_t pos = context->position;

    if (context->length == -1) return;

    unix_seek_callback( context, 0, SEEK_SET );
    parse_stream_names( demuxer, MAKEFOURCC('r','o','o','t'), context->length, 0 );
    unix_seek_callback( context, pos, SEEK_SET );
}

NTSTATUS demuxer_create( void *arg )
{
    struct demuxer_create_params *params = arg;
    const char *ext = params->url ? strrchr( params->url, '.' ) : NULL;
    const AVInputFormat *format;
    struct demuxer *demuxer;
    int i, ret;

    TRACE( "context %p, url %s, mime %s\n", params->context, debugstr_a(params->url), debugstr_a(params->mime_type) );

    /* Persona 4 Arena Ultimax loads extensionless ASF assets; keep this
     * non-NULL before the format-name MIME checks below. */
    if (!ext)
        ext = "";

    mediaconv_demuxer_init();

    if (!(demuxer = calloc( 1, sizeof(*demuxer) ))) return STATUS_NO_MEMORY;
    demuxer->stream_context = params->context;

    if (!(demuxer->ctx = avformat_alloc_context())) goto failed;
    if (!(demuxer->ctx->pb = avio_alloc_context( NULL, 0, 0, params->context, unix_read_callback, NULL,
                                                 params->context->length == (UINT64)-1 ? NULL : unix_seek_callback ))) goto failed;

    if ((ret = avformat_open_input( &demuxer->ctx, NULL, NULL, NULL )) < 0)
        WARN( "Failed to open input, error %s.\n", debugstr_averr(ret) );
    if ((ret = mediaconv_demuxer_open( &demuxer->ctx, params->context )) < 0)
    {
        ERR( "Failed to open input, error %s.\n", debugstr_averr(ret) );
        goto failed;
    }
    format = demuxer->ctx->iformat;
    if ((params->duration = get_context_duration( demuxer->ctx )) == AV_NOPTS_VALUE ||
        strstr( format->name, "mp3" ) || strstr( format->name, "mpeg" ) ||
        context_has_video_missing_frame_rate( demuxer->ctx ))
    {
        if ((ret = avformat_find_stream_info( demuxer->ctx, NULL )) < 0)
        {
            ERR( "Failed to find stream info, error %s.\n", debugstr_averr(ret) );
            goto failed;
        }
        params->duration = get_context_duration( demuxer->ctx );
    }
    demuxer->duration = params->duration;
    if (!(demuxer->streams = calloc( demuxer->ctx->nb_streams, sizeof(*demuxer->streams) ))) goto failed;
    if (demuxer_create_streams( demuxer )) goto failed;

    params->demuxer.handle = (UINT_PTR)demuxer;
    params->stream_count = demuxer->ctx->nb_streams;
    if (strstr( format->name, "mp4" )) strcpy( params->mime_type, "video/mp4" );
    else if (strstr( format->name, "avi" )) strcpy( params->mime_type, "video/avi" );
    else if (strstr( format->name, "mpeg" )) strcpy( params->mime_type, "video/mpeg" );
    else if (strstr( format->name, "mp3" )) strcpy( params->mime_type, "audio/mp3" );
    else if (strstr( format->name, "wav" )) strcpy( params->mime_type, "audio/wav" );
    else if (strstr( format->name, "asf" ))
    {
        if (!strcmp( ext, ".wma" )) strcpy( params->mime_type, "audio/x-ms-wma" );
        else if (!strcmp( ext, ".wmv" )) strcpy( params->mime_type, "video/x-ms-wmv" );
        else strcpy( params->mime_type, "video/x-ms-asf" );
    }
    else if (strstr( format->name, "ogg" ))
    {
        if (!strcmp( ext, ".oga" ) || !strcmp( ext, ".opus" )) strcpy( params->mime_type, "audio/ogg" );
        else strcpy( params->mime_type, "video/ogg" );
    }
    else
    {
        FIXME( "Unknown MIME type for format %s, url %s\n", debugstr_a(format->name), debugstr_a(params->url) );
        strcpy( params->mime_type, "video/x-application" );
    }

    if (strstr( format->name, "mp4" )) parse_mp4_streams_metadata( demuxer );
    return STATUS_SUCCESS;

failed:
    i = demuxer->ctx ? demuxer->ctx->nb_streams : 0;
    if (demuxer->ctx)
    {
        avio_context_free( &demuxer->ctx->pb );
        avformat_free_context( demuxer->ctx );
    }
    if (demuxer->streams)
    {
        for (UINT j = 0; j < i; j++)
        {
            av_bsf_free( &demuxer->streams[j].filter );
        }
    }
    free( demuxer->streams );
    free( demuxer );

    mediaconv_demuxer_exit();
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS demuxer_destroy( void *arg )
{
    struct demuxer_destroy_params *params = arg;
    struct demuxer *demuxer = get_demuxer( params->demuxer );
    int i;

    TRACE( "demuxer %p\n", demuxer );

    params->context = demuxer->stream_context;
    for (i = 0; i < demuxer->ctx->nb_streams; i++)
    {
        av_bsf_free( &demuxer->streams[i].filter );
    }
    avio_context_free( &demuxer->ctx->pb );
    avformat_free_context( demuxer->ctx );
    free( demuxer->streams );
    free( demuxer );

    mediaconv_demuxer_exit();
    return STATUS_SUCCESS;
}

static NTSTATUS demuxer_filter_packet( struct demuxer *demuxer, AVPacket **packet )
{
    struct stream *stream;
    int i, ret;

    do
    {
        if ((*packet = demuxer->last_packet)) return STATUS_SUCCESS;
        if (!(*packet = av_packet_alloc())) return STATUS_NO_MEMORY;

        if (!(stream = demuxer->last_stream)) ret = 0;
        else
        {
            if (!(ret = av_bsf_receive_packet( stream->filter, *packet ))) return STATUS_SUCCESS;
            if (ret == AVERROR_EOF) stream->eos = TRUE;
            if (!ret || ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) ret = 0;
            else WARN( "Failed to read packet from filter, error %s.\n", debugstr_averr( ret ) );
            stream = demuxer->last_stream = NULL;
        }

        if (!ret && !(ret = av_read_frame( demuxer->ctx, *packet )))
        {
            stream = demuxer->streams + (*packet)->stream_index;
            ret = av_bsf_send_packet( stream->filter, (*packet) );
            if (ret < 0) WARN( "Failed to send packet to filter, error %s.\n", debugstr_averr( ret ) );
            else demuxer->last_stream = stream;
        }
        av_packet_free( packet );

        if (ret == AVERROR_EOF)
        {
            for (i = 0; ret == AVERROR_EOF && i < demuxer->ctx->nb_streams; i++)
            {
                if (demuxer->streams[i].eos) continue;
                stream = demuxer->streams + i;
                ret = av_bsf_send_packet( stream->filter, NULL );
                if (ret < 0) WARN( "Failed to send packet to filter, error %s.\n", debugstr_averr( ret ) );
                else demuxer->last_stream = stream;
            }

            if (ret == AVERROR_EOF) return STATUS_END_OF_FILE;
        }
    } while (!ret || ret == AVERROR(EAGAIN));

    ERR( "Failed to read packet from demuxer %p, error %s.\n", demuxer, debugstr_averr( ret ) );
    return STATUS_END_OF_FILE;
}

NTSTATUS demuxer_read( void *arg )
{
    struct demuxer_read_params *params = arg;
    struct demuxer *demuxer = get_demuxer( params->demuxer );
    struct sample *sample = &params->sample;
    struct stream *demuxer_stream;
    UINT capacity = params->sample.size;
    UINT packet_prefix = 0;
    UINT packet_skip = 0;
    AVStream *stream;
    AVPacket *packet;
    NTSTATUS status;

    TRACE( "demuxer %p, capacity %#x\n", demuxer, capacity );

    sample->flags = 0;
    packet_prefix = 0;
    packet_skip = 0;
    if ((status = demuxer_filter_packet( demuxer, &packet ))) return status;

    stream = demuxer->ctx->streams[packet->stream_index];
    demuxer_stream = demuxer->streams + packet->stream_index;

    if (demuxer_stream->vc1_asf_startcode_fallback && packet->size >= packet_skip + 4
            && !(packet->data[packet_skip + 0] == 0x00 && packet->data[packet_skip + 1] == 0x00
                    && packet->data[packet_skip + 2] == 0x01))
        packet_prefix = 4;

    params->sample.size = packet->size - packet_skip + packet_prefix;
    if (capacity < params->sample.size)
    {
        demuxer->last_packet = packet;
        return STATUS_BUFFER_TOO_SMALL;
    }

    sample->pts = get_stream_time( stream, packet->pts );
    sample->dts = get_stream_time( stream, packet->dts );
    sample->duration = get_stream_time( stream, packet->duration );
    fixup_asf_mpeg4_timestamps( demuxer->ctx, stream, demuxer_stream, sample );
    if (demuxer->ctx->iformat && strstr( demuxer->ctx->iformat->name, "asf" ))
        normalize_stream_timestamps( demuxer_stream, sample );
    fixup_asf_vc1_timestamps( demuxer->ctx, stream, demuxer_stream, sample );
    fixup_asf_mpeg4_final_timestamps( demuxer->ctx, stream, demuxer_stream, sample );

    if (packet->flags & AV_PKT_FLAG_KEY) sample->flags |= SAMPLE_FLAG_SYNC_POINT;
    if (packet_prefix)
    {
        BYTE *dst = (BYTE *)(UINT_PTR)sample->data;

        dst[0] = 0x00;
        dst[1] = 0x00;
        dst[2] = 0x01;
        dst[3] = 0x0d;
        dst += packet_prefix;
        memcpy( dst, packet->data + packet_skip, packet->size - packet_skip );
    }
    else
    {
        memcpy( (void *)(UINT_PTR)sample->data, packet->data + packet_skip, packet->size - packet_skip );
    }
    params->stream = packet->stream_index;
    av_packet_free( &packet );
    demuxer->last_packet = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS demuxer_seek( void *arg )
{
    struct demuxer_seek_params *params = arg;
    struct demuxer *demuxer = get_demuxer( params->demuxer );
    int64_t timestamp = params->timestamp * AV_TIME_BASE / 10000000;
    NTSTATUS status;
    int i, ret;

    TRACE( "demuxer %p, timestamp 0x%s\n", demuxer, wine_dbgstr_longlong( params->timestamp ) );

    if (!(status = raw_mpegvideo_seek( demuxer, params->timestamp )))
        goto done;
    if (status != STATUS_NOT_SUPPORTED)
        return status;

    if ((ret = avformat_seek_file( demuxer->ctx, -1, INT64_MIN, timestamp, timestamp, AVSEEK_FLAG_BACKWARD )) < 0)
    {
        ERR( "Failed to seek demuxer %p, error %s.\n", demuxer, debugstr_averr(ret) );
        return STATUS_UNSUCCESSFUL;
    }

done:
    for (i = 0; i < demuxer->ctx->nb_streams; i++)
    {
        av_bsf_flush( demuxer->streams[i].filter );
        demuxer->streams[i].eos = FALSE;
        demuxer->streams[i].timestamp_base = 0;
        demuxer->streams[i].timestamp_base_set = FALSE;
        demuxer->streams[i].last_pts = INT64_MIN;
        demuxer->streams[i].next_pts = INT64_MIN;
        demuxer->streams[i].final_last_pts = INT64_MIN;
        demuxer->streams[i].final_next_pts = INT64_MIN;
        demuxer->streams[i].final_repeated_pts = 0;
    }
    av_packet_free( &demuxer->last_packet );
    demuxer->last_stream = NULL;

    return STATUS_SUCCESS;
}

NTSTATUS demuxer_stream_lang( void *arg )
{
    struct demuxer_stream_lang_params *params = arg;
    struct demuxer *demuxer = get_demuxer( params->demuxer );
    AVStream *stream = demuxer->ctx->streams[params->stream];
    AVDictionaryEntry *tag;

    TRACE( "demuxer %p, stream %u\n", demuxer, params->stream );

    if (!(tag = av_dict_get( stream->metadata, "language", NULL, 0 )))
        return STATUS_NOT_FOUND;

    TRACE( "stream %u language %s\n", params->stream, debugstr_a(tag->value) );
    lstrcpynA( params->buffer, tag->value, ARRAY_SIZE( params->buffer ) );
    return STATUS_SUCCESS;
}

NTSTATUS demuxer_stream_name( void *arg )
{
    struct demuxer_stream_name_params *params = arg;
    struct demuxer *demuxer = get_demuxer( params->demuxer );
    AVStream *stream = demuxer->ctx->streams[params->stream];
    AVDictionaryEntry *tag;

    TRACE( "demuxer %p, stream %u\n", demuxer, params->stream );

    if (!(tag = av_dict_get( stream->metadata, "title", NULL, AV_DICT_IGNORE_SUFFIX )) &&
        !(tag = av_dict_get( stream->metadata, "name", NULL, AV_DICT_IGNORE_SUFFIX )))
        return STATUS_NOT_FOUND;

    lstrcpynA( params->buffer, tag->value, ARRAY_SIZE( params->buffer ) );
    return STATUS_SUCCESS;
}

NTSTATUS demuxer_stream_type( void *arg )
{
    struct demuxer_stream_type_params *params = arg;
    struct demuxer *demuxer = get_demuxer( params->demuxer );
    AVStream *stream = demuxer->ctx->streams[params->stream];
    AVCodecParameters *par = demuxer->streams[params->stream].filter->par_out;
    AVRational fps;

    TRACE( "demuxer %p, stream %u, stream %p, index %u\n", demuxer, params->stream, stream, stream->index );
    if (par->codec_type == AVMEDIA_TYPE_VIDEO)
        fps = get_video_display_frame_rate( demuxer->ctx, stream );
    else
        fps = stream->avg_frame_rate;
    return media_type_from_codec_params( par, &stream->sample_aspect_ratio, &fps, 0, &params->media_type );
}

#endif /* HAVE_FFMPEG */
