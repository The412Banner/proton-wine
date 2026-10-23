/*
 * winedmo muxer backed by FFmpeg libavformat
 *
 * Copyright 2024 GloriousEggroll
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

#ifdef HAVE_FFMPEG

#include <inttypes.h>
#include "unix_private.h"

#include "mfapi.h"
#include "uuids.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dmo);

/* -----------------------------------------------------------------------
 * Output chunk list — the AVIO write callback appends here;
 * muxer_read_data pops chunks one (partial) read at a time.
 * ----------------------------------------------------------------------- */

struct muxer_chunk
{
    uint8_t *data;
    UINT32   size;
    UINT32   consumed;  /* bytes already returned to caller */
    UINT64   offset;    /* byte offset in the output file */
    struct muxer_chunk *next;
};

struct muxer_stream
{
    UINT32 id;          /* Windows stream id */
    int    av_index;    /* AVStream index in ctx->streams[] */
};

struct muxer
{
    AVFormatContext  *ctx;
    struct muxer_stream *streams;
    UINT32            stream_count;
    UINT64            write_pos;        /* current AVIO write position */
    struct muxer_chunk *head, *tail;    /* pending output chunks */
};

/* -----------------------------------------------------------------------
 * AVIO callbacks
 * ----------------------------------------------------------------------- */

static int muxer_write_callback( void *opaque, const uint8_t *buf, int size )
{
    struct muxer *muxer = opaque;
    struct muxer_chunk *chunk;

    if (size <= 0) return 0;

    if (!(chunk = malloc( sizeof(*chunk) ))) return AVERROR(ENOMEM);
    if (!(chunk->data = malloc( size ))) { free(chunk); return AVERROR(ENOMEM); }

    memcpy( chunk->data, buf, size );
    chunk->size     = size;
    chunk->consumed = 0;
    chunk->offset   = muxer->write_pos;
    chunk->next     = NULL;

    muxer->write_pos += size;

    if (muxer->tail) muxer->tail->next = chunk;
    else             muxer->head       = chunk;
    muxer->tail = chunk;

    return size;
}

static int64_t muxer_seek_callback( void *opaque, int64_t offset, int whence )
{
    struct muxer *muxer = opaque;

    if (whence == AVSEEK_SIZE) return -1;   /* size unknown */
    if (whence == SEEK_SET)    muxer->write_pos = offset;
    else if (whence == SEEK_CUR) muxer->write_pos += offset;
    else return -1;

    return muxer->write_pos;
}

/* -----------------------------------------------------------------------
 * Codec-ID helpers (mirrors unix_transform.c)
 * ----------------------------------------------------------------------- */

static enum AVCodecID codec_id_from_audio_tag( WORD tag )
{
    const struct AVCodecTag *tables[] = { avformat_get_riff_audio_tags(),
                                          avformat_get_mov_audio_tags(), NULL };
    switch (tag)
    {
    case WAVE_FORMAT_PCM:           return AV_CODEC_ID_PCM_S16LE;
    case WAVE_FORMAT_IEEE_FLOAT:    return AV_CODEC_ID_PCM_F32LE;
    case WAVE_FORMAT_ALAW:          return AV_CODEC_ID_PCM_ALAW;
    case WAVE_FORMAT_MULAW:         return AV_CODEC_ID_PCM_MULAW;
    case WAVE_FORMAT_MPEG:          return AV_CODEC_ID_MP2;
    case WAVE_FORMAT_MPEGLAYER3:    return AV_CODEC_ID_MP3; /* 0x0055 */
    case 0x00FF:                    return AV_CODEC_ID_AAC;
    case 0x1610:                    return AV_CODEC_ID_AAC;
    case 0x0160:                    return AV_CODEC_ID_WMAV1;
    case 0x0161:                    return AV_CODEC_ID_WMAV2;
    case 0x0162:                    return AV_CODEC_ID_WMAPRO;
    default:                        return av_codec_get_id( tables, tag );
    }
}

static enum AVCodecID codec_id_from_audio_format( const WAVEFORMATEX *wfx )
{
    WORD tag = wfx->wFormatTag;

    if (tag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22)
    {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wfx;
        tag = (WORD)ext->SubFormat.Data1;
    }

    return codec_id_from_audio_tag( tag );
}

static enum AVCodecID codec_id_from_video_format( const MFVIDEOFORMAT *mfvf )
{
    const struct AVCodecTag *tables[] = { avformat_get_riff_video_tags(), NULL };
    DWORD fourcc = mfvf->guidFormat.Data1;
    enum AVCodecID id = av_codec_get_id( tables, fourcc );

    if (id != AV_CODEC_ID_NONE) return id;

    switch (fourcc)
    {
    case MAKEFOURCC('H','2','6','4'): case MAKEFOURCC('h','2','6','4'):
    case MAKEFOURCC('A','V','C','1'): case MAKEFOURCC('a','v','c','1'):
        return AV_CODEC_ID_H264;
    case MAKEFOURCC('H','E','V','C'): case MAKEFOURCC('h','e','v','c'):
    case MAKEFOURCC('H','2','6','5'): case MAKEFOURCC('h','2','6','5'):
        return AV_CODEC_ID_HEVC;
    case MAKEFOURCC('V','P','8','0'):  return AV_CODEC_ID_VP8;
    case MAKEFOURCC('V','P','9','0'):  return AV_CODEC_ID_VP9;
    case MAKEFOURCC('A','V','0','1'): case MAKEFOURCC('a','v','0','1'):
        return AV_CODEC_ID_AV1;
    default: return AV_CODEC_ID_NONE;
    }
}

/* -----------------------------------------------------------------------
 * muxer_create
 * ----------------------------------------------------------------------- */

NTSTATUS muxer_create( void *arg )
{
    struct muxer_create_params *params = arg;
    struct muxer *muxer;
    AVFormatContext *ctx = NULL;
    AVIOContext *avio = NULL;
    uint8_t *avio_buf;
    int ret;

    if (!(muxer = calloc( 1, sizeof(*muxer) ))) return STATUS_NO_MEMORY;

    if (!(avio_buf = av_malloc( 4096 )))
    {
        free( muxer );
        return STATUS_NO_MEMORY;
    }

    if (!(avio = avio_alloc_context( avio_buf, 4096, 1, muxer,
                                     NULL, muxer_write_callback, muxer_seek_callback )))
    {
        av_free( avio_buf );
        free( muxer );
        return STATUS_NO_MEMORY;
    }
    avio->seekable = AVIO_SEEKABLE_NORMAL;

    if ((ret = avformat_alloc_output_context2( &ctx, NULL, params->format, NULL )) < 0)
    {
        WARN( "avformat_alloc_output_context2(%s) failed: %s\n", params->format, av_err2str(ret) );
        avio_context_free( &avio );
        free( muxer );
        return STATUS_NOT_SUPPORTED;
    }

    ctx->pb    = avio;
    muxer->ctx = ctx;

    params->muxer.handle = (UINT_PTR)muxer;
    TRACE( "created muxer %p for format '%s'\n", muxer, params->format );
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * muxer_destroy
 * ----------------------------------------------------------------------- */

NTSTATUS muxer_destroy( void *arg )
{
    struct muxer_destroy_params *params = arg;
    struct muxer *muxer = (struct muxer *)(UINT_PTR)params->muxer.handle;
    struct muxer_chunk *chunk, *next;

    if (!muxer) return STATUS_SUCCESS;

    TRACE( "muxer %p\n", muxer );

    if (muxer->ctx)
    {
        AVIOContext *avio = muxer->ctx->pb;
        muxer->ctx->pb = NULL;
        avformat_free_context( muxer->ctx );
        if (avio) avio_context_free( &avio );
    }

    for (chunk = muxer->head; chunk; chunk = next)
    {
        next = chunk->next;
        free( chunk->data );
        free( chunk );
    }

    free( muxer->streams );
    free( muxer );
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * muxer_add_stream
 * ----------------------------------------------------------------------- */

NTSTATUS muxer_add_stream( void *arg )
{
    struct muxer_add_stream_params *params = arg;
    struct muxer *muxer = (struct muxer *)(UINT_PTR)params->muxer.handle;
    const union winedmo_format *fmt = (const union winedmo_format *)(UINT_PTR)params->format;
    struct muxer_stream *streams;
    AVStream *avstream;
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;
    const uint8_t *extra_data = NULL;
    int extra_size = 0;

    TRACE( "muxer %p stream_id %u\n", muxer, params->stream_id );

    if (!muxer || !fmt) return STATUS_INVALID_PARAMETER;

    if (IsEqualGUID( &params->major_type, &MFMediaType_Audio ))
    {
        const WAVEFORMATEX *wfx = &fmt->audio;

        codec_id = codec_id_from_audio_format( wfx );

        /* Extract extradata: bytes after the WAVEFORMATEX header */
        if (wfx->cbSize > 0)
        {
            const uint8_t *after_wfx = (const uint8_t *)(wfx + 1);
            int cb = wfx->cbSize;

            if (codec_id == AV_CODEC_ID_AAC && wfx->wFormatTag == 0x1610)
            {
                /* HEAACWAVEINFO has 12 extra bytes before the AudioSpecificConfig */
                if (cb > 12) { extra_data = after_wfx + 12; extra_size = cb - 12; }
            }
            else if (codec_id == AV_CODEC_ID_AAC || codec_id == AV_CODEC_ID_WMAPRO
                     || codec_id == AV_CODEC_ID_WMAV1 || codec_id == AV_CODEC_ID_WMAV2)
            {
                extra_data = after_wfx;
                extra_size = cb;
            }
        }
    }
    else if (IsEqualGUID( &params->major_type, &MFMediaType_Video ))
    {
        const MFVIDEOFORMAT *mfvf = &fmt->video;

        codec_id = codec_id_from_video_format( mfvf );

        /* Extra bytes after MFVIDEOFORMAT are codec-private data (e.g. SPS/PPS for H264) */
        if (params->format_size > sizeof(MFVIDEOFORMAT))
        {
            extra_data = (const uint8_t *)(mfvf + 1);
            extra_size = params->format_size - sizeof(MFVIDEOFORMAT);
        }
    }

    if (codec_id == AV_CODEC_ID_NONE)
    {
        WARN( "could not determine codec id for stream %u\n", params->stream_id );
        return STATUS_NOT_SUPPORTED;
    }

    if (!(avstream = avformat_new_stream( muxer->ctx, NULL )))
        return STATUS_NO_MEMORY;

    avstream->codecpar->codec_type = IsEqualGUID( &params->major_type, &MFMediaType_Audio )
                                     ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;
    avstream->codecpar->codec_id   = codec_id;

    if (IsEqualGUID( &params->major_type, &MFMediaType_Audio ))
    {
        const WAVEFORMATEX *wfx = &fmt->audio;
        avstream->codecpar->sample_rate = wfx->nSamplesPerSec;
        avstream->codecpar->ch_layout.nb_channels = wfx->nChannels;
        avstream->codecpar->bits_per_coded_sample = wfx->wBitsPerSample;
        avstream->codecpar->bit_rate = wfx->nAvgBytesPerSec * 8;
        avstream->time_base = (AVRational){ 1, wfx->nSamplesPerSec };
    }
    else
    {
        const MFVIDEOFORMAT *mfvf = &fmt->video;
        avstream->codecpar->width  = mfvf->videoInfo.dwWidth;
        avstream->codecpar->height = mfvf->videoInfo.dwHeight;
        avstream->time_base = (AVRational){ 1, 90000 };
    }

    if (extra_data && extra_size > 0)
    {
        if (!(avstream->codecpar->extradata = av_mallocz( extra_size + AV_INPUT_BUFFER_PADDING_SIZE )))
            return STATUS_NO_MEMORY;
        memcpy( avstream->codecpar->extradata, extra_data, extra_size );
        avstream->codecpar->extradata_size = extra_size;
    }

    /* Record the stream mapping */
    if (!(streams = realloc( muxer->streams, (muxer->stream_count + 1) * sizeof(*streams) )))
        return STATUS_NO_MEMORY;
    muxer->streams = streams;
    muxer->streams[muxer->stream_count].id       = params->stream_id;
    muxer->streams[muxer->stream_count].av_index = avstream->index;
    muxer->stream_count++;

    TRACE( "added stream id=%u codec=%s av_index=%d\n",
           params->stream_id, avcodec_get_name(codec_id), avstream->index );
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * muxer_start
 * ----------------------------------------------------------------------- */

NTSTATUS muxer_start( void *arg )
{
    struct muxer_start_params *params = arg;
    struct muxer *muxer = (struct muxer *)(UINT_PTR)params->muxer.handle;
    int ret;

    TRACE( "muxer %p\n", muxer );

    if (!muxer) return STATUS_INVALID_PARAMETER;

    if ((ret = avformat_write_header( muxer->ctx, NULL )) < 0)
    {
        WARN( "avformat_write_header failed: %s\n", av_err2str(ret) );
        return STATUS_UNSUCCESSFUL;
    }

    avio_flush( muxer->ctx->pb );
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * muxer_push_sample
 * ----------------------------------------------------------------------- */

NTSTATUS muxer_push_sample( void *arg )
{
    struct muxer_push_sample_params *params = arg;
    struct muxer *muxer = (struct muxer *)(UINT_PTR)params->muxer.handle;
    const uint8_t *data = (const uint8_t *)(UINT_PTR)params->data;
    static const AVRational HUNDRED_NS = { 1, 10000000 };
    AVStream *avstream = NULL;
    AVPacket *pkt;
    UINT32 i;
    int ret;

    TRACE( "muxer %p stream_id %u size %u pts %"PRId64"\n",
           muxer, params->stream_id, params->size, (INT64)params->pts );

    if (!muxer) return STATUS_INVALID_PARAMETER;

    /* Find the AVStream for this Windows stream id */
    for (i = 0; i < muxer->stream_count; i++)
    {
        if (muxer->streams[i].id == params->stream_id)
        {
            avstream = muxer->ctx->streams[muxer->streams[i].av_index];
            break;
        }
    }
    if (!avstream)
    {
        WARN( "unknown stream_id %u\n", params->stream_id );
        return STATUS_INVALID_PARAMETER;
    }

    if (!(pkt = av_packet_alloc())) return STATUS_NO_MEMORY;

    if ((ret = av_new_packet( pkt, params->size )) < 0)
    {
        av_packet_free( &pkt );
        return STATUS_NO_MEMORY;
    }
    memcpy( pkt->data, data, params->size );

    pkt->stream_index = avstream->index;

    if (params->pts != INT64_MIN)
        pkt->pts = av_rescale_q( params->pts, HUNDRED_NS, avstream->time_base );
    else
        pkt->pts = AV_NOPTS_VALUE;

    if (params->duration > 0)
        pkt->duration = av_rescale_q( params->duration, HUNDRED_NS, avstream->time_base );

    pkt->dts = pkt->pts;

    if (params->flags & WINEDMO_SAMPLE_FLAG_SYNC_POINT)
        pkt->flags |= AV_PKT_FLAG_KEY;

    ret = av_write_frame( muxer->ctx, pkt );
    av_packet_free( &pkt );

    if (ret < 0)
    {
        WARN( "av_write_frame failed: %s\n", av_err2str(ret) );
        return STATUS_UNSUCCESSFUL;
    }

    avio_flush( muxer->ctx->pb );
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * muxer_read_data
 * ----------------------------------------------------------------------- */

NTSTATUS muxer_read_data( void *arg )
{
    struct muxer_read_data_params *params = arg;
    struct muxer *muxer = (struct muxer *)(UINT_PTR)params->muxer.handle;
    uint8_t *dst = (uint8_t *)(UINT_PTR)params->data;
    struct muxer_chunk *chunk;
    UINT32 to_copy;

    if (!muxer) return STATUS_INVALID_PARAMETER;
    if (!muxer->head) return STATUS_NO_MORE_ENTRIES;

    chunk = muxer->head;
    to_copy = min( params->size, chunk->size - chunk->consumed );

    memcpy( dst, chunk->data + chunk->consumed, to_copy );

    params->offset       = chunk->offset + chunk->consumed;
    params->size         = to_copy;
    chunk->consumed     += to_copy;

    if (chunk->consumed >= chunk->size)
    {
        muxer->head = chunk->next;
        if (!muxer->head) muxer->tail = NULL;
        free( chunk->data );
        free( chunk );
    }

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * muxer_finalize
 * ----------------------------------------------------------------------- */

NTSTATUS muxer_finalize( void *arg )
{
    struct muxer_finalize_params *params = arg;
    struct muxer *muxer = (struct muxer *)(UINT_PTR)params->muxer.handle;
    int ret;

    TRACE( "muxer %p\n", muxer );

    if (!muxer) return STATUS_INVALID_PARAMETER;

    if ((ret = av_write_trailer( muxer->ctx )) < 0)
    {
        WARN( "av_write_trailer failed: %s\n", av_err2str(ret) );
        return STATUS_UNSUCCESSFUL;
    }

    avio_flush( muxer->ctx->pb );
    return STATUS_SUCCESS;
}

#endif /* HAVE_FFMPEG */
