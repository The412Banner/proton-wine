/*
 * winedmo audio/video decoder transform backed by FFmpeg libavcodec
 *
 * Copyright 2024 GloriousEggroll
 *
 * Based on winedmo_transform.c (upstream Wine):
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#ifdef HAVE_FFMPEG

#include <stdbool.h>
#include <inttypes.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include "unix_private.h"

#include "d3d9.h"
#include "mfapi.h"
#include "uuids.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dmo);

/* -------------------------------------------------------------------------
 * Internal transform state
 * ------------------------------------------------------------------------- */

struct unix_transform
{
    AVCodecContext *avctx;
    AVCodecParserContext *parser;
    AVFrame        *frame;         /* reusable decoded-frame container */
    AVPacket       *packet;        /* reusable input-packet container */
    AVPacket       *parsed_packet; /* reusable packet for parser output */

    bool is_audio;
    bool is_raw_video;
    bool draining;       /* NULL packet sent — flushing decoder */
    bool frame_pending;  /* frame holds undispatched output */

    /* Audio output conversion */
    SwrContext         *swr;
    BYTE               *audio_buf;
    UINT32              audio_buf_capacity; /* allocated bytes in audio_buf */
    UINT32              audio_buf_filled;   /* valid bytes (may be < capacity) */
    UINT32              audio_buf_pos;      /* bytes already returned to caller */
    INT64               audio_base_pts;  /* absolute pts of first byte in audio_buf */
    INT64               last_input_pts;  /* pts of the most recently pushed input packet (100ns units), INT64_MIN if unknown */
    INT64               audio_pts_offset;
    INT64               audio_output_pts_adjust;
    INT64               cumulative_samples; /* PCM samples emitted since last pts anchor; used to synthesise pts for codecs
                                             * (e.g. MS-ADPCM) that only stamp the first decoded frame in a packet */
    enum AVSampleFormat out_sample_fmt;
    int                 out_channels;
    int                 out_sample_rate;
    bool                audio_output_format_set;
    bool                audio_started;
    bool                split_aac_output;

    /* Video output conversion */
    SwsContext         *sws;
    enum AVPixelFormat  out_pix_fmt;     /* AV_PIX_FMT_NONE = native */
    DWORD               out_video_fourcc;
    enum AVPixelFormat  in_pix_fmt;
    int                 input_fps_num, input_fps_den;
    int                 out_width, out_height;
    bool                out_bottom_up;
    enum AVCodecID      parser_codec_id;
    bool                mpeg_es_assemble;
    BYTE               *mpeg_es_buf;
    UINT32              mpeg_es_capacity;
    UINT32              mpeg_es_size;
    BYTE               *video_buf;
    UINT32              video_buf_capacity;
    UINT32              video_buf_filled;
    INT64               video_buf_pts;
    INT64               video_buf_duration;
    INT64               last_video_pts;   /* last emitted video pts, kept monotonic across decoder reordering */
    INT64               last_video_duration;
    INT64               video_input_anchor_pts; /* first input pts for the current video clip/segment */
    INT64               video_pts_offset; /* startup offset to subtract from decoded video pts for late first frames */
    unsigned int        video_trace_count;
    /* Format-change tracking */
    int                 last_width, last_height;
    enum AVPixelFormat  last_pix_fmt;
    int                 last_channels, last_sample_rate;
    enum AVSampleFormat last_sample_fmt;
};

static inline BOOL is_wmv_video_codec( enum AVCodecID codec_id );

/* -------------------------------------------------------------------------
 * Codec-ID helpers
 * ------------------------------------------------------------------------- */

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
    case 0x00FF:                    return AV_CODEC_ID_AAC; /* WAVE_FORMAT_AAC */
    case 0x1610:                    return AV_CODEC_ID_AAC; /* WAVE_FORMAT_HEAAC */
    case 0x0160:                    return AV_CODEC_ID_WMAV1;
    case 0x0161:                    return AV_CODEC_ID_WMAV2;
    case 0x0162:                    return AV_CODEC_ID_WMAPRO;
    case 0x0163:                    return AV_CODEC_ID_WMALOSSLESS;
    case 0x000A:                    return AV_CODEC_ID_WMAVOICE;
    case 0x674f: case 0x6750: case 0x6751: return AV_CODEC_ID_VORBIS;
    case WAVE_FORMAT_OPUS:          return AV_CODEC_ID_OPUS; /* 0x704f */
    default:                        return av_codec_get_id( tables, tag );
    }
}

static enum AVCodecID codec_id_from_audio_format( const WAVEFORMATEX *wfx )
{
    WORD tag = wfx->wFormatTag;

    if (tag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22)
    {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wfx;
        /* MFAudioFormat_Vorbis has a non-standard Data1 (0x8D2FD10B); its
         * lower WORD (0xD10B) is not a valid WAVE tag, so check the full GUID. */
        if (IsEqualGUID( &ext->SubFormat, &MFAudioFormat_Vorbis )) return AV_CODEC_ID_VORBIS;
        if (IsEqualGUID( &ext->SubFormat, &MFAudioFormat_Dolby_AC3 )) return AV_CODEC_ID_AC3;
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
    case MAKEFOURCC('X','2','6','4'): case MAKEFOURCC('x','2','6','4'):
        return AV_CODEC_ID_H264;
    case MAKEFOURCC('H','E','V','C'): case MAKEFOURCC('h','e','v','c'):
    case MAKEFOURCC('H','2','6','5'): case MAKEFOURCC('h','2','6','5'):
        return AV_CODEC_ID_HEVC;
    case MAKEFOURCC('V','P','8','0'):  return AV_CODEC_ID_VP8;
    case MAKEFOURCC('V','P','9','0'):  return AV_CODEC_ID_VP9;
    case MAKEFOURCC('A','V','0','1'): case MAKEFOURCC('a','v','0','1'):
        return AV_CODEC_ID_AV1;
    case MAKEFOURCC('W','M','V','1'):  return AV_CODEC_ID_WMV1;
    case MAKEFOURCC('W','M','V','2'):  return AV_CODEC_ID_WMV2;
    case MAKEFOURCC('W','M','V','3'):  return AV_CODEC_ID_WMV3;
    case MAKEFOURCC('W','V','C','1'):  return AV_CODEC_ID_VC1;
    case MAKEFOURCC('M','4','S','2'):
    case MAKEFOURCC('M','P','4','S'):
    case MAKEFOURCC('M','P','4','V'): case MAKEFOURCC('m','p','4','v'):
    case MAKEFOURCC('D','I','V','X'):
    case MAKEFOURCC('D','X','5','0'):
    case MAKEFOURCC('F','M','P','4'):
    case MAKEFOURCC('X','V','I','D'):
        return AV_CODEC_ID_MPEG4;
    case MAKEFOURCC('M','P','G','1'): case MAKEFOURCC('M','P','E','G'):
    case 0xe436eb81: /* MEDIASUBTYPE_MPEG1Payload */
        return AV_CODEC_ID_MPEG1VIDEO;
    case MAKEFOURCC('M','P','G','2'): case MAKEFOURCC('m','p','g','2'):
    case MAKEFOURCC('M','2','V','S'):
    case 0xe06d8026: /* MEDIASUBTYPE_MPEG2_VIDEO */
        return AV_CODEC_ID_MPEG2VIDEO;
    default: return AV_CODEC_ID_NONE;
    }
}

/* -------------------------------------------------------------------------
 * Format helpers
 * ------------------------------------------------------------------------- */

static WORD format_tag_from_wfx( const WAVEFORMATEX *wfx )
{
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22)
    {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wfx;
        return (WORD)ext->SubFormat.Data1;
    }

    return wfx->wFormatTag;
}

static enum AVSampleFormat sample_fmt_from_wfx( const WAVEFORMATEX *wfx )
{
    switch (format_tag_from_wfx( wfx ))
    {
    case WAVE_FORMAT_IEEE_FLOAT:
        return (wfx->wBitsPerSample == 64) ? AV_SAMPLE_FMT_DBL : AV_SAMPLE_FMT_FLT;
    }

    switch (wfx->wBitsPerSample)
    {
    case 8:  return AV_SAMPLE_FMT_U8;
    case 32: return AV_SAMPLE_FMT_S32;
    default: return AV_SAMPLE_FMT_S16;
    }
}

static enum AVPixelFormat pix_fmt_from_mf_fourcc( DWORD fourcc )
{
    switch (fourcc)
    {
    case MAKEFOURCC('N','V','1','2'): return AV_PIX_FMT_NV12;
    case MAKEFOURCC('I','4','2','0'): return AV_PIX_FMT_YUV420P;
    case MAKEFOURCC('I','Y','U','V'): return AV_PIX_FMT_YUV420P;
    case MAKEFOURCC('Y','V','1','2'): return AV_PIX_FMT_YUV420P;
    case MAKEFOURCC('Y','U','Y','2'): return AV_PIX_FMT_YUYV422;
    case MAKEFOURCC('U','Y','V','Y'): return AV_PIX_FMT_UYVY422;
    case MAKEFOURCC('P','0','1','0'): return AV_PIX_FMT_P010LE;
    case 0xe436eb7b:                  return AV_PIX_FMT_RGB565LE; /* MEDIASUBTYPE_RGB565 */
    case 0xe436eb7c:                  return AV_PIX_FMT_RGB555LE; /* MEDIASUBTYPE_RGB555 */
    case 0xe436eb7d:                  return AV_PIX_FMT_BGR24;    /* MEDIASUBTYPE_RGB24 */
    case 0xe436eb7e:                  return AV_PIX_FMT_BGR0;     /* MEDIASUBTYPE_RGB32 */
    case 0x00000015:                  return AV_PIX_FMT_BGRA;   /* D3DFMT_A8R8G8B8 */
    case 0x00000016:                  return AV_PIX_FMT_BGR0;   /* D3DFMT_X8R8G8B8 */
    case 0x00000014:                  return AV_PIX_FMT_BGR24;  /* D3DFMT_R8G8B8 */
    case 0x00000020:                  return AV_PIX_FMT_RGBA;   /* D3DFMT_A8B8G8R8 */
    default:                          return AV_PIX_FMT_NONE;
    }
}

static DWORD mf_fourcc_from_pix_fmt( enum AVPixelFormat fmt )
{
    switch (fmt)
    {
    case AV_PIX_FMT_NV12:    return MAKEFOURCC('N','V','1','2');
    case AV_PIX_FMT_YUV420P: return MAKEFOURCC('I','4','2','0');
    case AV_PIX_FMT_YUYV422: return MAKEFOURCC('Y','U','Y','2');
    case AV_PIX_FMT_UYVY422: return MAKEFOURCC('U','Y','V','Y');
    case AV_PIX_FMT_P010LE:  return MAKEFOURCC('P','0','1','0');
    case AV_PIX_FMT_BGRA:    return 0x00000015;
    case AV_PIX_FMT_BGR0:    return 0x00000016;
    case AV_PIX_FMT_BGR24:   return 0x00000014;
    case AV_PIX_FMT_RGBA:    return 0x00000020;
    default:                 return 0;
    }
}

static int fill_output_image_arrays( struct unix_transform *t, uint8_t *data[4], int linesize[4],
                                     BYTE *dst, enum AVPixelFormat fmt, int width, int height );

static NTSTATUS copy_yuv420_frame_to_yv12( const AVFrame *frame, BYTE *dst, UINT32 dst_size )
{
    const int width = frame->width;
    const int height = frame->height;
    const int chroma_width = AV_CEIL_RSHIFT( width, 1 );
    const int chroma_height = AV_CEIL_RSHIFT( height, 1 );
    const UINT32 y_size = width * height;
    const UINT32 chroma_size = chroma_width * chroma_height;
    BYTE *dst_y = dst;
    BYTE *dst_v = dst_y + y_size;
    BYTE *dst_u = dst_v + chroma_size;
    int y;

    if (dst_size < y_size + chroma_size * 2) return STATUS_BUFFER_TOO_SMALL;
    if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)
        return STATUS_NOT_SUPPORTED;

    for (y = 0; y < height; ++y)
        memcpy( dst_y + y * width, frame->data[0] + y * frame->linesize[0], width );
    for (y = 0; y < chroma_height; ++y)
    {
        memcpy( dst_v + y * chroma_width, frame->data[2] + y * frame->linesize[2], chroma_width );
        memcpy( dst_u + y * chroma_width, frame->data[1] + y * frame->linesize[1], chroma_width );
    }

    return STATUS_SUCCESS;
}

static NTSTATUS copy_yuv420_frame_to_nv12( struct unix_transform *t, const AVFrame *frame,
                                           BYTE *dst, UINT32 dst_size, int width, int height )
{
    uint8_t *dst_data[4];
    int dst_linesize[4];
    int copy_width = width < frame->width ? width : frame->width;
    int copy_height = height < frame->height ? height : frame->height;
    int x, y;

    if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)
        return STATUS_NOT_SUPPORTED;
    if (t->out_pix_fmt != AV_PIX_FMT_NV12)
        return STATUS_NOT_SUPPORTED;
    if (copy_width & 1 || copy_height & 1)
        return STATUS_NOT_SUPPORTED;
    if (av_image_get_buffer_size(AV_PIX_FMT_NV12, width, height, 1) > dst_size)
        return STATUS_BUFFER_TOO_SMALL;
    if (fill_output_image_arrays(t, dst_data, dst_linesize, dst, AV_PIX_FMT_NV12, width, height) < 0)
        return STATUS_UNSUCCESSFUL;

    memset(dst, 0, dst_size);
    memset(dst_data[1], 0x80, dst_linesize[1] * height / 2);

    for (y = 0; y < copy_height; ++y)
        memcpy(dst_data[0] + y * dst_linesize[0], frame->data[0] + y * frame->linesize[0], copy_width);

    for (y = 0; y < copy_height / 2; ++y)
    {
        const uint8_t *src_u = frame->data[1] + y * frame->linesize[1];
        const uint8_t *src_v = frame->data[2] + y * frame->linesize[2];
        uint8_t *dst_uv = dst_data[1] + y * dst_linesize[1];

        for (x = 0; x < copy_width / 2; ++x)
        {
            dst_uv[x * 2] = src_u[x];
            dst_uv[x * 2 + 1] = src_v[x];
        }
    }

    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Audio conversion helper
 * ------------------------------------------------------------------------- */

static NTSTATUS convert_audio_frame( struct unix_transform *t, const AVFrame *frame )
{
    enum AVSampleFormat out_fmt  = t->out_sample_fmt;
    int out_rate     = t->out_sample_rate ? t->out_sample_rate : frame->sample_rate;
    int out_channels;
    int in_channels;
    int out_samples;
    UINT32 needed;
    uint8_t *out_ptr;
    int got;

#if LIBAVUTIL_VERSION_MAJOR >= 58
    in_channels  = frame->ch_layout.nb_channels;
#else
    in_channels  = frame->channels;
#endif
    out_channels = t->out_channels ? t->out_channels : in_channels;

    /* (Re)build SwrContext if not yet initialised */
    if (!t->swr)
    {
#if LIBAVUTIL_VERSION_MAJOR >= 58
        AVChannelLayout in_layout = {0}, out_layout = {0};

        if (!frame->ch_layout.nb_channels || frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC
                || !av_channel_layout_check( &frame->ch_layout ))
            av_channel_layout_default( &in_layout, in_channels );
        else
            av_channel_layout_copy( &in_layout, &frame->ch_layout );
        av_channel_layout_default( &out_layout, out_channels );
        swr_alloc_set_opts2( &t->swr, &out_layout, out_fmt, out_rate,
                             &in_layout, frame->format, frame->sample_rate, 0, NULL );
        av_channel_layout_uninit( &in_layout );
        av_channel_layout_uninit( &out_layout );
#else
        int64_t in_mask  = frame->channel_layout
                           ? frame->channel_layout
                           : av_get_default_channel_layout( in_channels );
        int64_t out_mask = av_get_default_channel_layout( out_channels );
        t->swr = swr_alloc_set_opts( NULL, out_mask, out_fmt, out_rate,
                                     in_mask, frame->format, frame->sample_rate, 0, NULL );
#endif
        if (!t->swr || swr_init( t->swr ) < 0)
        {
            WARN( "Failed to initialise SwrContext\n" );
            if (t->swr) swr_free( &t->swr );
            return STATUS_UNSUCCESSFUL;
        }
    }

    out_samples = swr_get_out_samples( t->swr, frame->nb_samples );
    needed      = out_samples * out_channels * av_get_bytes_per_sample( out_fmt );

    /* Grow the buffer to fit the new frame appended after any already-accumulated data.
     * Use exponential doubling to avoid O(n²) memcpy cost when accumulating many frames
     * (e.g. MS-ADPCM files with thousands of blocks sent as a single input packet). */
    if (t->audio_buf_filled + needed > t->audio_buf_capacity || !t->audio_buf)
    {
        UINT32 min_cap = t->audio_buf_filled + needed;
        UINT32 new_cap = t->audio_buf_capacity ? t->audio_buf_capacity * 2 : min_cap;
        void *buf;
        if (new_cap < min_cap) new_cap = min_cap; /* handle overflow or small initial cap */
        buf = realloc( t->audio_buf, new_cap );
        if (!buf) return STATUS_NO_MEMORY;
        t->audio_buf          = buf;
        t->audio_buf_capacity = new_cap;
    }

    out_ptr = (uint8_t *)(t->audio_buf + t->audio_buf_filled);
    got = swr_convert( t->swr, &out_ptr, out_samples,
                       (const uint8_t **)frame->data, frame->nb_samples );
    if (got < 0) return STATUS_UNSUCCESSFUL;

    t->audio_buf_filled += (UINT32)(got * out_channels * av_get_bytes_per_sample( out_fmt ));
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Video output helpers
 * ------------------------------------------------------------------------- */

static UINT32 video_frame_size( enum AVPixelFormat fmt, int w, int h )
{
    int size = av_image_get_buffer_size( fmt, w, h, 1 );
    return (size > 0) ? (UINT32)size : 0;
}

static int raw_video_height_from_size( enum AVPixelFormat fmt, int width, int height, UINT32 size )
{
    int expected, line_size;

    if (width <= 0 || height <= 0 || !size)
        return height;

    expected = av_image_get_buffer_size( fmt, width, height, 1 );
    if (expected > 0 && size >= expected)
        return height;

    switch (fmt)
    {
    case AV_PIX_FMT_NV12:
        if (!(size % (width * 3 / 2)))
            return min( height, (int)(size / (width * 3 / 2)) );
        break;
    case AV_PIX_FMT_YUV420P:
        if (!(size % (width * 3 / 2)))
            return min( height, (int)(size / (width * 3 / 2)) );
        break;
    default:
        line_size = av_image_get_linesize( fmt, width, 0 );
        if (line_size > 0 && size >= line_size)
            return min( height, (int)(size / line_size) );
        break;
    }

    return 0;
}

static INT64 video_duration_from_rate( int num, int den )
{
    if (num > 0 && den > 0)
        return (INT64)(((INT64)10000000 * den) / num);

    return INT64_MIN;
}

static bool get_video_frame_pts( struct unix_transform *t, INT64 *pts )
{
    INT64 video_pts = AV_NOPTS_VALUE;

    if (t->frame->best_effort_timestamp != AV_NOPTS_VALUE)
        video_pts = t->frame->best_effort_timestamp;
    else if (t->frame->pts != AV_NOPTS_VALUE)
        video_pts = t->frame->pts;

    if (video_pts == AV_NOPTS_VALUE)
        return false;

    if (t->avctx->codec_id == AV_CODEC_ID_VC1)
    {
        if (t->last_video_pts == INT64_MIN && t->video_pts_offset == INT64_MIN)
            t->video_pts_offset = video_pts;

        if (t->video_pts_offset != INT64_MIN && video_pts >= t->video_pts_offset)
            video_pts -= t->video_pts_offset;
    }

    *pts = video_pts;
    return true;
}

static INT64 get_video_frame_duration( struct unix_transform *t )
{
    INT64 duration;

    duration = video_duration_from_rate( t->input_fps_num, t->input_fps_den );
    if (t->frame->duration > 0)
        duration = t->frame->duration;

    return duration;
}

static inline BOOL is_wmv_video_codec( enum AVCodecID codec_id )
{
    return codec_id == AV_CODEC_ID_WMV3 || codec_id == AV_CODEC_ID_VC1;
}

static UINT32 mpeg_find_picture_start( const BYTE *data, UINT32 size, UINT32 offset )
{
    UINT32 i;

    if (size < 4 || offset > size - 4)
        return UINT32_MAX;

    for (i = offset; i <= size - 4; ++i)
    {
        if (!data[i] && !data[i + 1] && data[i + 2] == 1 && data[i + 3] == 0)
            return i;
    }

    return UINT32_MAX;
}

static bool is_packed_rgb_format( enum AVPixelFormat fmt )
{
    switch (fmt)
    {
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_BGR0:
    case AV_PIX_FMT_BGR24:
    case AV_PIX_FMT_RGBA:
        return true;
    default:
        return false;
    }
}

static int fill_output_image_arrays( struct unix_transform *t, uint8_t *data[4], int linesize[4],
                                     BYTE *dst, enum AVPixelFormat fmt, int width, int height )
{
    int ret = av_image_fill_arrays( data, linesize, dst, fmt, width, height, 1 );

    if (ret >= 0 && t->out_bottom_up && is_packed_rgb_format( fmt ))
    {
        data[0] += linesize[0] * (height - 1);
        linesize[0] = -linesize[0];
    }

    return ret;
}

static NTSTATUS copy_video_frame( struct unix_transform *t, const AVFrame *frame,
                                  BYTE *dst, UINT32 dst_size )
{
    enum AVPixelFormat out_fmt = (t->out_pix_fmt != AV_PIX_FMT_NONE)
                                 ? t->out_pix_fmt : (enum AVPixelFormat)frame->format;
    int out_width = t->out_width > 0 ? t->out_width : frame->width;
    int out_height = t->out_height > 0 ? t->out_height : frame->height;
    int src_width = frame->width, src_height = frame->height;

    if (out_width <= frame->width && out_height <= frame->height
            && frame->width - out_width < 16 && frame->height - out_height < 16)
    {
        src_width = out_width;
        src_height = out_height;
    }

    if (t->out_video_fourcc == MAKEFOURCC('Y','V','1','2')
            && out_width == frame->width && out_height == frame->height)
    {
        NTSTATUS status = copy_yuv420_frame_to_yv12( frame, dst, dst_size );
        if (status != STATUS_NOT_SUPPORTED) return status;
    }

    if (out_fmt == AV_PIX_FMT_NV12
            && out_width >= src_width && out_height >= src_height
            && out_width - src_width < 16 && out_height - src_height < 16)
    {
        NTSTATUS status = copy_yuv420_frame_to_nv12( t, frame, dst, dst_size, out_width, out_height );
        if (status != STATUS_NOT_SUPPORTED) return status;
    }

    if (out_fmt != (enum AVPixelFormat)frame->format
            || out_width != src_width || out_height != src_height)
    {
        /* Pixel-format conversion and scaling via SwsContext. */
        uint8_t *dst_data[4];
        int dst_linesize[4];
        int sws_flags = (out_width == src_width && out_height == src_height) ? SWS_POINT : SWS_BILINEAR;

        t->sws = sws_getCachedContext( t->sws,
                                       src_width, src_height, frame->format,
                                       out_width, out_height, out_fmt,
                                       sws_flags, NULL, NULL, NULL );
        if (!t->sws) return STATUS_UNSUCCESSFUL;

        if (fill_output_image_arrays( t, dst_data, dst_linesize, dst, out_fmt, out_width, out_height ) < 0)
            return STATUS_UNSUCCESSFUL;

        sws_scale( t->sws,
                   (const uint8_t *const *)frame->data, frame->linesize,
                   0, src_height, dst_data, dst_linesize );
    }
    else if (t->out_bottom_up && is_packed_rgb_format( out_fmt ))
    {
        uint8_t *dst_data[4];
        int dst_linesize[4];

        if (fill_output_image_arrays( t, dst_data, dst_linesize, dst, out_fmt, out_width, out_height ) < 0)
            return STATUS_UNSUCCESSFUL;

        av_image_copy( dst_data, dst_linesize, (const uint8_t *const *)frame->data,
                       frame->linesize, out_fmt, src_width, src_height );
    }
    else
    {
        /* Direct copy with stride normalisation */
        if (av_image_copy_to_buffer( dst, dst_size,
                                     (const uint8_t *const *)frame->data,
                                     frame->linesize, out_fmt,
                                     src_width, src_height, 1 ) < 0)
            return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Unix-callable functions
 * ------------------------------------------------------------------------- */

NTSTATUS transform_create( void *arg )
{
    struct transform_create_params *params = arg;
    const union winedmo_format *in_fmt  = (void *)(UINT_PTR)params->input_format;
    const union winedmo_format *out_fmt = (void *)(UINT_PTR)params->output_format;
    const BYTE    *extradata      = NULL;
    int            extradata_size = 0;
    enum AVCodecID codec_id;
    const AVCodec *codec;
    struct unix_transform *t;

    TRACE( "major %s input_fmt %p (size %u) output_fmt %p (size %u)\n",
           debugstr_guid( &params->major_type ), in_fmt, params->input_format_size,
           out_fmt, params->output_format_size );

    if (!(t = calloc( 1, sizeof(*t) ))) return STATUS_NO_MEMORY;
    t->last_input_pts = INT64_MIN;
    t->audio_output_pts_adjust = INT64_MIN;
    t->last_video_pts = INT64_MIN;
    t->last_video_duration = INT64_MIN;
    t->video_input_anchor_pts = INT64_MIN;
    t->video_pts_offset = INT64_MIN;

    /* ---- Determine codec and desired output format ---- */

    if (IsEqualGUID( &params->major_type, &MFMediaType_Audio ))
    {
        const WAVEFORMATEX *wfx = &in_fmt->audio;

        t->is_audio = true;
        codec_id    = codec_id_from_audio_format( wfx );

        if (wfx->cbSize && params->input_format_size >= sizeof(*wfx) + wfx->cbSize)
        {
            extradata      = (const BYTE *)(wfx + 1);
            extradata_size = wfx->cbSize;
            /* For WAVEFORMATEXTENSIBLE, extra codec data starts past the fixed ext part */
            if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize > 22)
            {
                extradata      += 22;
                extradata_size -= 22;
            }
            if (codec_id == AV_CODEC_ID_AAC)
            {
                t->split_aac_output = !(wfx->nBlockAlign && wfx->wBitsPerSample);

                /* MP4 AAC exposes a short AudioSpecificConfig directly, while
                 * HEAACWAVEINFO-backed streams prefix it with a 12-byte header.
                 * MPEG-TS/HLS AAC may not provide usable codec config here, so
                 * keep the no-extradata path unless the format looks like the
                 * MP4-style AAC media type that needs the short config. */
                if (extradata_size > 12)
                {
                    extradata      += 12;
                    extradata_size -= 12;
                }
                else if (!(extradata_size > 0 && wfx->nBlockAlign && wfx->wBitsPerSample))
                {
                    extradata      = NULL;
                    extradata_size = 0;
                }
            }
        }

        if (out_fmt && params->output_format_size >= sizeof(WAVEFORMATEX))
        {
            t->out_sample_fmt  = sample_fmt_from_wfx( &out_fmt->audio );
            t->out_channels    = out_fmt->audio.nChannels;
            t->out_sample_rate = out_fmt->audio.nSamplesPerSec;
        }
        else
        {
            t->out_sample_fmt  = AV_SAMPLE_FMT_S16;
            t->out_channels    = 0; /* match decoder output */
            t->out_sample_rate = 0;
        }
    }
    else if (IsEqualGUID( &params->major_type, &MFMediaType_Video ))
    {
        const MFVIDEOFORMAT *mfvf = &in_fmt->video;

        t->is_audio = false;
        codec_id    = codec_id_from_video_format( mfvf );

        if (mfvf->dwSize > sizeof(*mfvf) && params->input_format_size >= mfvf->dwSize)
        {
            extradata      = (const BYTE *)(mfvf + 1);
            extradata_size = mfvf->dwSize - sizeof(*mfvf);
        }

        t->out_video_fourcc = out_fmt ? out_fmt->video.guidFormat.Data1 : 0;
        t->out_pix_fmt = (out_fmt && params->output_format_size >= sizeof(MFVIDEOFORMAT))
                         ? pix_fmt_from_mf_fourcc( out_fmt->video.guidFormat.Data1 )
                         : AV_PIX_FMT_NONE;
        t->out_width = (out_fmt && params->output_format_size >= sizeof(MFVIDEOFORMAT))
                       ? out_fmt->video.videoInfo.dwWidth : 0;
        t->out_height = (out_fmt && params->output_format_size >= sizeof(MFVIDEOFORMAT))
                        ? out_fmt->video.videoInfo.dwHeight : 0;
        t->out_bottom_up = out_fmt && params->output_format_size >= sizeof(MFVIDEOFORMAT)
                           && (out_fmt->video.videoInfo.VideoFlags & MFVideoFlag_BottomUpLinearRep);
        t->in_pix_fmt = pix_fmt_from_mf_fourcc( mfvf->guidFormat.Data1 );
        t->input_fps_num = mfvf->videoInfo.FramesPerSecond.Numerator;
        t->input_fps_den = mfvf->videoInfo.FramesPerSecond.Denominator;
    }
    else
    {
        WARN( "Unsupported major type %s\n", debugstr_guid( &params->major_type ) );
        free( t );
        return STATUS_NOT_SUPPORTED;
    }

    if (!t->is_audio && (codec_id == AV_CODEC_ID_NONE || codec_id == AV_CODEC_ID_RAWVIDEO)
            && t->in_pix_fmt != AV_PIX_FMT_NONE)
    {
        t->is_raw_video = true;
        if (t->out_pix_fmt == AV_PIX_FMT_NONE) t->out_pix_fmt = t->in_pix_fmt;
        if (!t->out_video_fourcc) t->out_video_fourcc = mf_fourcc_from_pix_fmt( t->out_pix_fmt );
        t->last_width = in_fmt->video.videoInfo.dwWidth;
        t->last_height = in_fmt->video.videoInfo.dwHeight;
        t->last_pix_fmt = t->in_pix_fmt;
        params->transform.handle = (UINT64)(UINT_PTR)t;
        TRACE( "created raw video transform %p input=%#lx output=%#lx\n",
               t, (unsigned long)in_fmt->video.guidFormat.Data1, (unsigned long)t->out_video_fourcc );
        return STATUS_SUCCESS;
    }

    if (codec_id == AV_CODEC_ID_NONE || !(codec = avcodec_find_decoder( codec_id )))
    {
        WARN( "No decoder for codec id %d\n", codec_id );
        free( t );
        return STATUS_NOT_SUPPORTED;
    }

    if (!(t->avctx = avcodec_alloc_context3( codec )))
    {
        free( t );
        return STATUS_NO_MEMORY;
    }

    /* ---- Populate codec context from input format ---- */

    if (t->is_audio)
    {
        const WAVEFORMATEX *wfx = &in_fmt->audio;
        t->avctx->sample_rate  = wfx->nSamplesPerSec;
        t->avctx->block_align  = wfx->nBlockAlign;
        t->avctx->bit_rate     = (int64_t)wfx->nAvgBytesPerSec * 8;
#if LIBAVUTIL_VERSION_MAJOR >= 58
        av_channel_layout_default( &t->avctx->ch_layout, wfx->nChannels );
#else
        t->avctx->channels       = wfx->nChannels;
        t->avctx->channel_layout = av_get_default_channel_layout( wfx->nChannels );
#endif
    }
    else
    {
        const MFVIDEOFORMAT *mfvf = &in_fmt->video;
        t->avctx->width  = mfvf->videoInfo.dwWidth;
        t->avctx->height = mfvf->videoInfo.dwHeight;
#ifdef AV_CODEC_CAP_TRUNCATED
        if (codec->capabilities & AV_CODEC_CAP_TRUNCATED)
            t->avctx->flags |= AV_CODEC_FLAG_TRUNCATED;
#endif
        if (mfvf->videoInfo.FramesPerSecond.Numerator && mfvf->videoInfo.FramesPerSecond.Denominator)
        {
            t->avctx->framerate.num = t->input_fps_num;
            t->avctx->framerate.den = t->input_fps_den;
        }
    }

    /* ---- Set extradata ---- */

    if (extradata && extradata_size > 0)
    {
        if ((t->avctx->extradata = av_malloc( extradata_size + AV_INPUT_BUFFER_PADDING_SIZE )))
        {
            memcpy( t->avctx->extradata, extradata, extradata_size );
            memset( t->avctx->extradata + extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE );
            t->avctx->extradata_size = extradata_size;
        }
    }

    if (avcodec_open2( t->avctx, codec, NULL ) < 0)
    {
        WARN( "avcodec_open2 failed for codec %s\n", codec->name );
        avcodec_free_context( &t->avctx );
        free( t );
        return STATUS_NOT_SUPPORTED;
    }

    if (!t->is_audio && (codec_id == AV_CODEC_ID_MPEG1VIDEO || codec_id == AV_CODEC_ID_MPEG2VIDEO))
    {
        t->parser = av_parser_init( codec_id );
        t->parser_codec_id = codec_id;
        t->mpeg_es_assemble = !t->parser;
    }

    if (!(t->frame = av_frame_alloc()) || !(t->packet = av_packet_alloc())
            || ((t->parser || t->mpeg_es_assemble) && !(t->parsed_packet = av_packet_alloc())))
    {
        if (t->parser) av_parser_close( t->parser );
        av_frame_free( &t->frame );
        av_packet_free( &t->packet );
        av_packet_free( &t->parsed_packet );
        avcodec_free_context( &t->avctx );
        free( t );
        return STATUS_NO_MEMORY;
    }

    /* Initialise tracking to "unknown" */
    t->last_width       = -1;
    t->last_height      = -1;
    t->last_pix_fmt     = AV_PIX_FMT_NONE;
    t->last_channels    = -1;
    t->last_sample_rate = -1;
    t->last_sample_fmt  = AV_SAMPLE_FMT_NONE;

    params->transform.handle = (UINT64)(UINT_PTR)t;
    TRACE( "created transform %p codec=%s\n", t, codec->name );
    return STATUS_SUCCESS;
}

NTSTATUS transform_destroy( void *arg )
{
    const struct transform_destroy_params *params = arg;
    struct unix_transform *t = (void *)(UINT_PTR)params->transform.handle;

    if (!t) return STATUS_INVALID_PARAMETER;
    TRACE( "transform %p\n", t );

    if (t->parser) av_parser_close( t->parser );
    avcodec_free_context( &t->avctx );
    av_frame_free( &t->frame );
    av_packet_free( &t->packet );
    av_packet_free( &t->parsed_packet );
    if (t->swr) swr_free( &t->swr );
    if (t->sws) sws_freeContext( t->sws );
    free( t->mpeg_es_buf );
    free( t->audio_buf );
    free( t->video_buf );
    free( t );
    return STATUS_SUCCESS;
}

NTSTATUS transform_push_input( void *arg )
{
    struct transform_push_input_params *params = arg;
    struct unix_transform *t  = (void *)(UINT_PTR)params->transform.handle;
    const BYTE            *in = (void *)(UINT_PTR)params->data;
    const INT64 prev_input_pts = t->last_input_pts;
    int ret;

    TRACE( "transform %p size %u pts %"PRId64" flags %#lx\n",
           t, params->size, (INT64)params->pts, (unsigned long)params->flags );

    if (t->is_raw_video)
    {
        void *buf;

        if (t->frame_pending) return STATUS_DEVICE_BUSY;
        if (params->size > t->video_buf_capacity)
        {
            if (!(buf = realloc( t->video_buf, params->size ))) return STATUS_NO_MEMORY;
            t->video_buf = buf;
            t->video_buf_capacity = params->size;
        }

        if (in && params->size) memcpy( t->video_buf, in, params->size );
        t->video_buf_filled = params->size;
        t->video_buf_pts = params->pts;
        t->video_buf_duration = params->duration;
        t->frame_pending = true;
        return STATUS_SUCCESS;
    }

    if (t->draining) return STATUS_DEVICE_BUSY;

    if (!t->is_audio)
    {
        if (t->frame_pending) return STATUS_DEVICE_BUSY;

        av_frame_unref( t->frame );
        ret = avcodec_receive_frame( t->avctx, t->frame );
        if (ret >= 0)
        {
            t->frame_pending = true;
            return STATUS_DEVICE_BUSY;
        }
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        {
            WARN( "avcodec_receive_frame before input: %d\n", ret );
            return STATUS_UNSUCCESSFUL;
        }
    }

    av_packet_unref( t->packet );

    if (in && params->size)
    {
        if ((ret = av_new_packet( t->packet, params->size )) < 0) return STATUS_NO_MEMORY;
        memcpy( t->packet->data, in, params->size );
    }

    t->packet->pts = (params->pts != INT64_MIN) ? params->pts
            : (params->dts != INT64_MIN && (t->is_audio || t->avctx->codec_id != AV_CODEC_ID_VC1))
            ? params->dts : AV_NOPTS_VALUE;

    if (!t->is_audio)
    {
        if (params->pts != INT64_MIN && prev_input_pts != INT64_MIN && params->pts < prev_input_pts)
        {
            t->video_input_anchor_pts = params->pts;
            t->video_pts_offset = INT64_MIN;
            t->last_video_pts = INT64_MIN;
            t->last_video_duration = INT64_MIN;
            t->video_trace_count = 0;
        }
        else if ((params->flags & WINEDMO_SAMPLE_FLAG_DISCONTINUITY) && params->pts != INT64_MIN)
        {
            t->video_input_anchor_pts = params->pts;
            t->video_pts_offset = INT64_MIN;
            t->last_video_pts = INT64_MIN;
            t->last_video_duration = INT64_MIN;
            t->video_trace_count = 0;
        }
        else if (t->video_input_anchor_pts == INT64_MIN && params->pts != INT64_MIN)
        {
            t->video_input_anchor_pts = params->pts;
        }
    }

    t->packet->dts = (params->dts != INT64_MIN) ? params->dts : t->packet->pts;
    t->packet->duration = params->duration != INT64_MIN ? params->duration : 0;
    if (params->flags & WINEDMO_SAMPLE_FLAG_SYNC_POINT)
        t->packet->flags |= AV_PKT_FLAG_KEY;
    t->last_input_pts = params->pts; /* track for codecs that don't propagate pts to output frames */
    if (params->pts != INT64_MIN)
        t->cumulative_samples = 0; /* new pts anchor — restart incremental sample counting */

    if (t->is_audio && params->pts != INT64_MIN && params->pts < t->audio_pts_offset)
        t->audio_pts_offset = params->pts;

    if (t->is_audio && (params->flags & WINEDMO_SAMPLE_FLAG_DISCONTINUITY))
    {
        t->audio_pts_offset = 0;
        t->audio_output_pts_adjust = INT64_MIN;
        t->audio_started = false;
    }

    if (t->parser && in && params->size)
    {
        const uint8_t *data = t->packet->data;
        int size = t->packet->size;

        while (size > 0)
        {
            uint8_t *parsed_data = NULL;
            int parsed_size = 0;
            int consumed;

            consumed = av_parser_parse2( t->parser, t->avctx, &parsed_data, &parsed_size,
                                         data, size, t->packet->pts, t->packet->dts, AV_NOPTS_VALUE );
            if (consumed < 0)
                return STATUS_UNSUCCESSFUL;

            data += consumed;
            size -= consumed;

            if (!parsed_size)
            {
                if (!consumed)
                    break;
                continue;
            }

            av_packet_unref( t->parsed_packet );
            if ((ret = av_new_packet( t->parsed_packet, parsed_size )) < 0)
                return STATUS_NO_MEMORY;
            memcpy( t->parsed_packet->data, parsed_data, parsed_size );
            t->parsed_packet->pts = t->parser->pts != AV_NOPTS_VALUE ? t->parser->pts : t->packet->pts;
            t->parsed_packet->dts = t->parser->dts != AV_NOPTS_VALUE ? t->parser->dts : t->packet->dts;
            t->parsed_packet->duration = t->packet->duration;
            t->parsed_packet->flags = t->packet->flags;

            if ((ret = avcodec_send_packet( t->avctx, t->parsed_packet )) < 0)
            {
                if (ret == AVERROR(EAGAIN)) return STATUS_DEVICE_BUSY;
                WARN( "avcodec_send_packet: %d\n", ret );
                return STATUS_UNSUCCESSFUL;
            }
        }

        return STATUS_SUCCESS;
    }

    if (t->mpeg_es_assemble && in && params->size)
    {
        UINT32 first_pic, next_pic;
        void *buf;

        if (params->size > UINT32_MAX - t->mpeg_es_size)
            return STATUS_NO_MEMORY;
        if (t->mpeg_es_size + params->size > t->mpeg_es_capacity)
        {
            UINT32 new_capacity = t->mpeg_es_capacity ? t->mpeg_es_capacity * 2 : 0x10000;
            while (new_capacity < t->mpeg_es_size + params->size)
                new_capacity *= 2;
            if (!(buf = realloc( t->mpeg_es_buf, new_capacity )))
                return STATUS_NO_MEMORY;
            t->mpeg_es_buf = buf;
            t->mpeg_es_capacity = new_capacity;
        }

        memcpy( t->mpeg_es_buf + t->mpeg_es_size, t->packet->data, params->size );
        t->mpeg_es_size += params->size;

        first_pic = mpeg_find_picture_start( t->mpeg_es_buf, t->mpeg_es_size, 0 );
        if (first_pic == UINT32_MAX)
            return STATUS_SUCCESS;
        next_pic = mpeg_find_picture_start( t->mpeg_es_buf, t->mpeg_es_size, first_pic + 4 );
        if (next_pic == UINT32_MAX)
            return STATUS_SUCCESS;

        av_packet_unref( t->parsed_packet );
        if ((ret = av_new_packet( t->parsed_packet, next_pic )) < 0)
            return STATUS_NO_MEMORY;
        memcpy( t->parsed_packet->data, t->mpeg_es_buf, next_pic );
        t->parsed_packet->pts = t->packet->pts;
        t->parsed_packet->dts = t->packet->dts;
        t->parsed_packet->duration = t->packet->duration;
        t->parsed_packet->flags = t->packet->flags;

        if ((ret = avcodec_send_packet( t->avctx, t->parsed_packet )) < 0)
        {
            if (ret == AVERROR(EAGAIN)) return STATUS_DEVICE_BUSY;
            WARN( "avcodec_send_packet: %d\n", ret );
            return STATUS_UNSUCCESSFUL;
        }

        memmove( t->mpeg_es_buf, t->mpeg_es_buf + next_pic, t->mpeg_es_size - next_pic );
        t->mpeg_es_size -= next_pic;
        return STATUS_SUCCESS;
    }

    if ((ret = avcodec_send_packet( t->avctx, t->packet )) < 0)
    {
        if (ret == AVERROR(EAGAIN)) return STATUS_DEVICE_BUSY;
        WARN( "avcodec_send_packet: %d\n", ret );
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS transform_get_output( void *arg )
{
    struct transform_get_output_params *params = arg;
    struct unix_transform *t   = (void *)(UINT_PTR)params->transform.handle;
    BYTE                  *dst = (void *)(UINT_PTR)params->data;
    UINT32 capacity = params->size;
    int ret;

    params->flags    = 0;
    params->pts      = INT64_MIN;
    params->duration = INT64_MIN;

    if (t->is_raw_video)
    {
        AVFrame frame = {0};
        enum AVPixelFormat out_fmt = (t->out_pix_fmt != AV_PIX_FMT_NONE) ? t->out_pix_fmt : t->in_pix_fmt;
        int out_width = t->out_width > 0 ? t->out_width : t->last_width;
        int out_height = t->out_height > 0 ? t->out_height : t->last_height;
        int input_height = raw_video_height_from_size( t->in_pix_fmt, t->last_width, t->last_height,
                                                       t->video_buf_filled );
        UINT32 required = video_frame_size( out_fmt, out_width, out_height );
        NTSTATUS st;

        if (!t->frame_pending) { params->size = 0; return STATUS_MORE_PROCESSING_REQUIRED; }
        if (capacity < required)
        {
            params->size = required;
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (input_height <= 0)
            return STATUS_UNSUCCESSFUL;
        if (av_image_fill_arrays( frame.data, frame.linesize, t->video_buf, t->in_pix_fmt,
                                  t->last_width, input_height, 1 ) < 0)
            return STATUS_UNSUCCESSFUL;

        frame.width = t->last_width;
        frame.height = input_height;
        frame.format = t->in_pix_fmt;

        st = copy_video_frame( t, &frame, dst, capacity );
        if (FAILED(st)) return st;

        params->pts = t->video_buf_pts;
        params->duration = t->video_buf_duration;
        params->size = required;
        t->frame_pending = false;
        t->video_buf_filled = 0;
        return STATUS_SUCCESS;
    }

    /* ---- Return buffered audio if available ---- */

    if (t->is_audio && t->audio_buf_pos < t->audio_buf_filled)
    {
        UINT32 remaining = t->audio_buf_filled - t->audio_buf_pos;
        UINT32 copy      = (remaining <= capacity) ? remaining : capacity;

        if (copy == 0) { params->size = remaining; return STATUS_BUFFER_TOO_SMALL; }

        {
            int out_ch  = t->out_channels    ? t->out_channels    : t->last_channels;
            int out_r   = t->out_sample_rate ? t->out_sample_rate : t->last_sample_rate;
            int bps     = av_get_bytes_per_sample( t->out_sample_fmt );
            if (out_ch > 0 && out_r > 0 && bps > 0)
            {
                INT64 bytes_per_frame = (INT64)out_ch * bps;
                params->pts      = t->audio_base_pts + (INT64)((INT64)(t->audio_buf_pos / bytes_per_frame) * 10000000LL / out_r);
                params->duration = (INT64)((INT64)(copy / bytes_per_frame) * 10000000LL / out_r);
            }
        }

        memcpy( dst, t->audio_buf + t->audio_buf_pos, copy );
        t->audio_buf_pos += copy;
        params->size      = copy;

        if (t->audio_buf_pos < t->audio_buf_filled)
            params->flags |= WINEDMO_SAMPLE_FLAG_INCOMPLETE;
        return STATUS_SUCCESS;
    }

    /* ---- Receive next frame if none pending ---- */

    if (!t->frame_pending)
    {
        for (;;)
        {
            av_frame_unref( t->frame );
            ret = avcodec_receive_frame( t->avctx, t->frame );
            if (ret == AVERROR(EAGAIN)) { params->size = 0; return STATUS_MORE_PROCESSING_REQUIRED; }
            if (ret == AVERROR_EOF)     { params->size = 0; return STATUS_END_OF_FILE; }
            if (ret < 0)
            {
                WARN( "avcodec_receive_frame: %d\n", ret );
                params->size = 0;
                return STATUS_UNSUCCESSFUL;
            }
            break;
        }
        t->frame_pending = true;
    }

    /* ---- Detect format change ---- */

    if (t->is_audio)
    {
        int cur_channels;
        BOOL output_format_fixed = t->audio_output_format_set && t->out_channels > 0 && t->out_sample_rate > 0;
#if LIBAVUTIL_VERSION_MAJOR >= 58
        cur_channels = t->frame->ch_layout.nb_channels;
#else
        cur_channels = t->frame->channels;
#endif
        if (cur_channels   != t->last_channels    ||
            t->frame->sample_rate != t->last_sample_rate ||
            (enum AVSampleFormat)t->frame->format != t->last_sample_fmt)
        {
            t->last_channels    = cur_channels;
            t->last_sample_rate = t->frame->sample_rate;
            t->last_sample_fmt  = (enum AVSampleFormat)t->frame->format;
            /* Rebuild SwrContext on next audio conversion */
            if (t->swr) { swr_free( &t->swr ); }
            if (!output_format_fixed)
            {
                params->flags |= WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED;
                params->size   = 0;
                return STATUS_SUCCESS; /* frame_pending stays true */
            }
        }
    }
    else
    {
        if (t->frame->width  != t->last_width  ||
            t->frame->height != t->last_height ||
            (enum AVPixelFormat)t->frame->format != t->last_pix_fmt)
        {
            t->last_width   = t->frame->width;
            t->last_height  = t->frame->height;
            t->last_pix_fmt = (enum AVPixelFormat)t->frame->format;
            if (t->sws) { sws_freeContext( t->sws ); t->sws = NULL; }
            if (!t->is_raw_video)
            {
                params->flags |= WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED;
                params->size   = 0;
                return STATUS_SUCCESS;
            }
        }
    }

    /* ---- Output the frame ---- */

    if (!t->is_audio)
    {
        INT64 video_pts = AV_NOPTS_VALUE;

        params->duration = get_video_frame_duration( t );
        get_video_frame_pts( t, &video_pts );

        if (video_pts != AV_NOPTS_VALUE)
        {
            if (t->avctx->codec_id == AV_CODEC_ID_VC1 && t->last_video_pts != INT64_MIN
                    && params->duration > 0 && video_pts <= t->last_video_pts)
                video_pts = t->last_video_pts + params->duration;
            params->pts = video_pts;
            if (t->avctx->codec_id == AV_CODEC_ID_VC1)
            {
                t->last_video_pts = video_pts;
                t->last_video_duration = params->duration;
            }
        }

    }
    else if (t->frame->pts != AV_NOPTS_VALUE)
    {
        params->pts = t->frame->pts;

        /* Some ASF/WMA streams expose decoded audio frame timestamps relative to the
         * start of the current stream while the source-reader timeline for a later
         * clip starts at a non-zero packet timestamp. Rebase those frame-relative
         * timestamps onto the current input packet anchor so audio and video start
         * on the same timeline for clip transitions. */
        if (t->is_audio && t->avctx->codec_id != AV_CODEC_ID_WMAPRO
                && t->last_input_pts > 0 && params->pts >= 0 && params->pts < t->last_input_pts)
            params->pts += t->last_input_pts;
    }
    else if (t->is_audio && t->last_input_pts != INT64_MIN)
    {
        /* Codecs such as MS-ADPCM only stamp the first decoded frame in a packet;
         * subsequent frames have AV_NOPTS_VALUE.  Use the cumulative PCM sample
         * count (reset on each new input packet) to synthesise a monotonically
         * increasing timestamp anchored at last_input_pts. */
        if (t->frame->sample_rate > 0)
            params->pts = t->last_input_pts + t->cumulative_samples * 10000000LL / t->frame->sample_rate;
        else
            params->pts = t->last_input_pts;
    }
    if (t->is_audio)
    {
        INT64 base_pts = params->pts;
        NTSTATUS st;

        if (base_pts != INT64_MIN && t->audio_pts_offset)
        {
            base_pts -= t->audio_pts_offset;
            params->pts = base_pts;
        }

        /* Reset accumulation buffer and convert the first (already-received) frame. */
        t->audio_buf_filled = 0;
        t->audio_buf_pos    = 0;
        st = convert_audio_frame( t, t->frame );
        if (FAILED(st)) { WARN( "convert_audio_frame failed %#lx\n", (unsigned long)st ); return st; }
        t->cumulative_samples += t->frame->nb_samples;
        t->frame_pending = false;

        /* Drain all remaining frames from the same input packet into audio_buf.
         * For codecs that produce multiple frames per input packet (e.g. some
         * container formats), this ensures all decoded frames are accumulated
         * before returning so the caller gets a complete chunk per ProcessOutput.
         *
         * AAC in MPEG-TS/HLS may expose several decoded AAC frames for one input
         * packet. Aggregating them into a single output sample advances the audio
         * stream too far on each SourceReader audio request when applications pull
         * audio and video alternately. Return one AAC frame per ProcessOutput call
         * and let the next call drain the decoder normally. */
        while (t->avctx->codec_id != AV_CODEC_ID_AAC || !t->split_aac_output)
        {
            int cur_ch;
            av_frame_unref( t->frame );
            ret = avcodec_receive_frame( t->avctx, t->frame );
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { WARN( "avcodec_receive_frame in accumulate: %d\n", ret ); break; }

            /* Stop if audio format changes; format-change handling happens on the next call. */
#if LIBAVUTIL_VERSION_MAJOR >= 58
            cur_ch = t->frame->ch_layout.nb_channels;
#else
            cur_ch = t->frame->channels;
#endif
            if (cur_ch != t->last_channels ||
                t->frame->sample_rate != t->last_sample_rate ||
                (enum AVSampleFormat)t->frame->format != t->last_sample_fmt)
            {
                t->frame_pending = true;
                break;
            }

            st = convert_audio_frame( t, t->frame );
            if (FAILED(st)) break;
            t->cumulative_samples += t->frame->nb_samples;
        }

        /* Return audio from the accumulated buffer.  The fast path at the top of this
         * function handles subsequent calls when audio_buf_pos < audio_buf_filled,
         * serving the next chunk without decoding again.  Callers that pass capacity=0
         * (legacy PROVIDES_SAMPLES query) get STATUS_BUFFER_TOO_SMALL with the full
         * size; all other callers receive up to capacity bytes with INCOMPLETE set when
         * more data remains. */
        if (!t->audio_started && base_pts != INT64_MIN
                && (t->avctx->codec_id != AV_CODEC_ID_AAC || !t->split_aac_output))
            t->audio_output_pts_adjust = base_pts;
        if (base_pts != INT64_MIN && t->audio_output_pts_adjust != INT64_MIN)
        {
            base_pts -= t->audio_output_pts_adjust;
            params->pts = base_pts;
        }

        params->flags |= WINEDMO_SAMPLE_FLAG_SYNC_POINT;
        if (!t->audio_started)
            params->flags |= WINEDMO_SAMPLE_FLAG_DISCONTINUITY;
        t->audio_started = true;
        t->audio_base_pts = (base_pts != INT64_MIN) ? base_pts : 0;
        t->audio_buf_pos  = 0;
        {
            UINT32 remaining = t->audio_buf_filled;
            UINT32 copy      = (remaining <= capacity) ? remaining : capacity;

            /* For PROVIDES_SAMPLES phase-1 query (capacity==0), signal the caller the exact
             * byte count needed; it will allocate a buffer of that size and call again.
             * For pre-allocated callers (capacity>0, e.g. wma_decoder.c), copy what fits
             * and mark INCOMPLETE so they iterate to drain the rest. */
            if (capacity == 0)
            { params->size = remaining; return STATUS_BUFFER_TOO_SMALL; }

            /* Copy up to capacity bytes; set INCOMPLETE if more remains. */
            {
                int out_ch2 = t->out_channels    ? t->out_channels    : t->last_channels;
                int out_r2  = t->out_sample_rate ? t->out_sample_rate : t->last_sample_rate;
                int bps2    = av_get_bytes_per_sample( t->out_sample_fmt );

                if (out_ch2 > 0 && out_r2 > 0 && bps2 > 0)
                {
                    INT64 bytes_per_frame = (INT64)out_ch2 * bps2;
                    params->duration = (INT64)((INT64)(copy / bytes_per_frame) * 10000000LL / out_r2);
                }
            }

            if (copy > 0)
                memcpy( dst, t->audio_buf, copy );
            t->audio_buf_pos = copy;
            params->size     = copy;

            if (copy < remaining)
                params->flags |= WINEDMO_SAMPLE_FLAG_INCOMPLETE;
        }
    }
    else
    {
        enum AVPixelFormat out_fmt = (t->out_pix_fmt != AV_PIX_FMT_NONE)
                                     ? t->out_pix_fmt
                                     : (enum AVPixelFormat)t->frame->format;
        int out_width = t->out_width > 0 ? t->out_width : t->frame->width;
        int out_height = t->out_height > 0 ? t->out_height : t->frame->height;
        UINT32 required = video_frame_size( out_fmt, out_width, out_height );
        NTSTATUS st;

        if (capacity < required)
        {
            params->size = required;
            return STATUS_BUFFER_TOO_SMALL;
        }

        st = copy_video_frame( t, t->frame, dst, capacity );
        if (FAILED(st)) return st;

        t->frame_pending = false;
        params->size     = required;
    }

    return STATUS_SUCCESS;
}

NTSTATUS transform_drain( void *arg )
{
    const struct transform_drain_params *params = arg;
    struct unix_transform *t = (void *)(UINT_PTR)params->transform.handle;
    int ret;

    TRACE( "transform %p\n", t );

    if (t->is_raw_video)
    {
        t->draining = true;
        return STATUS_SUCCESS;
    }

    if (!t->draining)
    {
        if (t->mpeg_es_assemble && t->mpeg_es_size)
        {
            av_packet_unref( t->parsed_packet );
            if ((ret = av_new_packet( t->parsed_packet, t->mpeg_es_size )) < 0)
                return STATUS_NO_MEMORY;
            memcpy( t->parsed_packet->data, t->mpeg_es_buf, t->mpeg_es_size );
            if ((ret = avcodec_send_packet( t->avctx, t->parsed_packet )) < 0 && ret != AVERROR(EAGAIN))
            {
                WARN( "avcodec_send_packet: %d\n", ret );
                return STATUS_UNSUCCESSFUL;
            }
            t->mpeg_es_size = 0;
        }
        avcodec_send_packet( t->avctx, NULL );
        t->draining = true;
    }
    return STATUS_SUCCESS;
}

NTSTATUS transform_flush( void *arg )
{
    const struct transform_flush_params *params = arg;
    struct unix_transform *t = (void *)(UINT_PTR)params->transform.handle;

    TRACE( "transform %p\n", t );

    if (t->avctx) avcodec_flush_buffers( t->avctx );
    if (t->parser)
    {
        av_parser_close( t->parser );
        t->parser = av_parser_init( t->parser_codec_id );
    }
    if (t->frame) av_frame_unref( t->frame );
    if (t->packet) av_packet_unref( t->packet );
    if (t->parsed_packet) av_packet_unref( t->parsed_packet );
    t->frame_pending      = false;
    t->draining           = false;
    t->audio_buf_filled   = 0;
    t->audio_buf_pos      = 0;
    t->video_buf_filled   = 0;
    t->mpeg_es_size       = 0;
    t->video_trace_count  = 0;
    t->audio_base_pts     = 0;
    t->last_input_pts     = INT64_MIN;
    t->audio_pts_offset   = 0;
    t->audio_output_pts_adjust = INT64_MIN;
    t->audio_started      = false;
    t->last_video_pts     = INT64_MIN;
    t->last_video_duration = INT64_MIN;
    t->video_input_anchor_pts = INT64_MIN;
    t->video_pts_offset   = INT64_MIN;
    t->cumulative_samples = 0;
    /* Don't free swr/sws — parameters haven't changed */
    return STATUS_SUCCESS;
}

NTSTATUS transform_get_output_format( void *arg )
{
    struct transform_get_output_format_params *params = arg;
    struct unix_transform *t = (void *)(UINT_PTR)params->transform.handle;
    struct media_type *mt    = &params->media_type;

    if (t->is_audio)
    {
        WAVEFORMATEX wfx = {0};
        int channels, rate;
        enum AVSampleFormat fmt;

#if LIBAVUTIL_VERSION_MAJOR >= 58
        channels = t->avctx->ch_layout.nb_channels;
#else
        channels = t->avctx->channels;
#endif
        if (t->out_channels > 0)
            channels = t->out_channels;
        else if (t->last_channels > 0)
            channels = t->last_channels;

        if (t->out_sample_rate > 0)
            rate = t->out_sample_rate;
        else
            rate = t->last_sample_rate > 0 ? t->last_sample_rate : t->avctx->sample_rate;
        fmt  = t->out_sample_fmt;

        wfx.wFormatTag     = (fmt == AV_SAMPLE_FMT_FLT || fmt == AV_SAMPLE_FMT_DBL)
                             ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        wfx.nChannels      = channels ? channels : 2;
        wfx.nSamplesPerSec = rate ? rate : 44100;
        wfx.wBitsPerSample = av_get_bytes_per_sample( fmt ) * 8;
        wfx.nBlockAlign    = wfx.nChannels * wfx.wBitsPerSample / 8;
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        wfx.cbSize          = 0;

        mt->major       = MFMediaType_Audio;
        mt->format_size = sizeof(wfx);

        if (!mt->format) return STATUS_BUFFER_TOO_SMALL;
        memcpy( mt->format, &wfx, sizeof(wfx) );
    }
    else
    {
        MFVIDEOFORMAT mfvf = {0};
        int visible_w = t->out_width > 0 ? t->out_width
                : (t->last_width  > 0 ? t->last_width  : (t->avctx ? t->avctx->width : 0));
        int visible_h = t->out_height > 0 ? t->out_height
                : (t->last_height > 0 ? t->last_height : (t->avctx ? t->avctx->height : 0));
        int w = visible_w, h = visible_h;
        enum AVPixelFormat pf = (t->out_pix_fmt != AV_PIX_FMT_NONE)
                                ? t->out_pix_fmt
                                : (t->last_pix_fmt != AV_PIX_FMT_NONE
                                   ? t->last_pix_fmt : AV_PIX_FMT_NV12);
        DWORD fourcc = t->out_video_fourcc ? t->out_video_fourcc : mf_fourcc_from_pix_fmt( pf );

        if (t->avctx && t->avctx->coded_width >= visible_w && t->avctx->coded_height >= visible_h
                && t->avctx->coded_width - visible_w < 16 && t->avctx->coded_height - visible_h < 16)
        {
            w = t->avctx->coded_width;
            h = t->avctx->coded_height;
        }

        mfvf.dwSize = sizeof(mfvf);
        /* Build guidFormat from FOURCC: {fourcc, 0, 0x10, MF guid tail} */
        mfvf.guidFormat.Data1 = fourcc;
        mfvf.guidFormat.Data2 = 0;
        mfvf.guidFormat.Data3 = 0x0010;
        mfvf.guidFormat.Data4[0] = 0x80; mfvf.guidFormat.Data4[1] = 0x00;
        mfvf.guidFormat.Data4[2] = 0x00; mfvf.guidFormat.Data4[3] = 0xaa;
        mfvf.guidFormat.Data4[4] = 0x00; mfvf.guidFormat.Data4[5] = 0x38;
        mfvf.guidFormat.Data4[6] = 0x9b; mfvf.guidFormat.Data4[7] = 0x71;

        mfvf.videoInfo.dwWidth  = w;
        mfvf.videoInfo.dwHeight = h;
        if (t->input_fps_num > 0 && t->input_fps_den > 0)
        {
            mfvf.videoInfo.FramesPerSecond.Numerator   = t->input_fps_num;
            mfvf.videoInfo.FramesPerSecond.Denominator = t->input_fps_den;
        }
        else if (t->avctx && t->avctx->framerate.num && t->avctx->framerate.den)
        {
            mfvf.videoInfo.FramesPerSecond.Numerator   = t->avctx->framerate.num;
            mfvf.videoInfo.FramesPerSecond.Denominator = t->avctx->framerate.den;
        }
        mfvf.videoInfo.VideoFlags  = 0;
        mfvf.videoInfo.GeometricAperture.Area.cx = visible_w;
        mfvf.videoInfo.GeometricAperture.Area.cy = visible_h;
        mfvf.videoInfo.MinimumDisplayAperture = mfvf.videoInfo.GeometricAperture;
        mfvf.videoInfo.PanScanAperture = mfvf.videoInfo.GeometricAperture;

        mt->major       = MFMediaType_Video;
        mt->format_size = sizeof(mfvf);

        if (!mt->format) return STATUS_BUFFER_TOO_SMALL;
        memcpy( mt->format, &mfvf, sizeof(mfvf) );
    }

    return STATUS_SUCCESS;
}

NTSTATUS transform_set_output_format( void *arg )
{
    struct transform_set_output_format_params *params = arg;
    struct unix_transform *t   = (void *)(UINT_PTR)params->transform.handle;
    const union winedmo_format *fmt = (void *)(UINT_PTR)params->format;

    if (!fmt || !params->format_size) return STATUS_INVALID_PARAMETER;

    if (t->is_audio && params->format_size >= sizeof(WAVEFORMATEX))
    {
        enum AVSampleFormat new_fmt  = sample_fmt_from_wfx( &fmt->audio );
        int                 new_ch   = fmt->audio.nChannels;
        int                 new_rate = fmt->audio.nSamplesPerSec;

        if (new_fmt != t->out_sample_fmt || new_ch != t->out_channels || new_rate != t->out_sample_rate)
        {
            t->out_sample_fmt  = new_fmt;
            t->out_channels    = new_ch;
            t->out_sample_rate = new_rate;
            if (t->swr) { swr_free( &t->swr ); }
        }
        t->audio_output_format_set = true;
    }
    else if (!t->is_audio && params->format_size >= sizeof(MFVIDEOFORMAT))
    {
        enum AVPixelFormat new_pf = pix_fmt_from_mf_fourcc( fmt->video.guidFormat.Data1 );
        t->out_video_fourcc = fmt->video.guidFormat.Data1;
        t->out_width = fmt->video.videoInfo.dwWidth;
        t->out_height = fmt->video.videoInfo.dwHeight;
        t->out_bottom_up = !!(fmt->video.videoInfo.VideoFlags & MFVideoFlag_BottomUpLinearRep);
        if (new_pf != t->out_pix_fmt)
        {
            t->out_pix_fmt = new_pf;
            if (t->sws) { sws_freeContext( t->sws ); t->sws = NULL; }
        }
    }
    return STATUS_SUCCESS;
}

#endif /* HAVE_FFMPEG */
