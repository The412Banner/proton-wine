/*
 * winedmo media format types shared between Windows and Unix code
 *
 * Copyright 2020-2021 Zebediah Figura for CodeWeavers
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

#ifndef __WINE_WINEDMO_MEDIA_FORMAT_H
#define __WINE_WINEDMO_MEDIA_FORMAT_H

#include <stdint.h>
#include "windef.h"
#include "winternl.h"
#include "wtypes.h"
#include "mmreg.h"
#include "vfw.h"
#include "dshow.h"
#include "dvdmedia.h"
#include "mfobjects.h"

#include "wine/unixlib.h"

/* same as MPEG1VIDEOINFO / MPEG2VIDEOINFO but with MFVIDEOFORMAT */
struct mpeg_video_format
{
    MFVIDEOFORMAT hdr;
    UINT32 start_time_code;
    UINT32 profile;
    UINT32 level;
    UINT32 flags;
    UINT32 sequence_header_count;
    UINT32 __pad;
    BYTE sequence_header[];
};

C_ASSERT(sizeof(struct mpeg_video_format) == offsetof(struct mpeg_video_format, sequence_header[0]));

struct winedmo_media_type
{
    GUID major;
    UINT32 format_size;
    union
    {
        void *format;
        WAVEFORMATEX *audio;
        MFVIDEOFORMAT *video;
    } u;
};

typedef UINT32 winedmo_major_type;
enum winedmo_major_type
{
    WINEDMO_MAJOR_TYPE_UNKNOWN = 0,
    WINEDMO_MAJOR_TYPE_AUDIO,
    WINEDMO_MAJOR_TYPE_AUDIO_MPEG1,
    WINEDMO_MAJOR_TYPE_AUDIO_MPEG4,
    WINEDMO_MAJOR_TYPE_AUDIO_WMA,
    WINEDMO_MAJOR_TYPE_VIDEO,
    WINEDMO_MAJOR_TYPE_VIDEO_CINEPAK,
    WINEDMO_MAJOR_TYPE_VIDEO_H264,
    WINEDMO_MAJOR_TYPE_VIDEO_WMV,
    WINEDMO_MAJOR_TYPE_VIDEO_INDEO,
    WINEDMO_MAJOR_TYPE_VIDEO_MPEG1,
};

typedef UINT32 winedmo_audio_format;
enum winedmo_audio_format
{
    WINEDMO_AUDIO_FORMAT_UNKNOWN,

    WINEDMO_AUDIO_FORMAT_U8,
    WINEDMO_AUDIO_FORMAT_S16LE,
    WINEDMO_AUDIO_FORMAT_S24LE,
    WINEDMO_AUDIO_FORMAT_S32LE,
    WINEDMO_AUDIO_FORMAT_F32LE,
    WINEDMO_AUDIO_FORMAT_F64LE,
};

typedef UINT32 winedmo_video_format;
enum winedmo_video_format
{
    WINEDMO_VIDEO_FORMAT_UNKNOWN,

    WINEDMO_VIDEO_FORMAT_BGRA,
    WINEDMO_VIDEO_FORMAT_BGRx,
    WINEDMO_VIDEO_FORMAT_BGR,
    WINEDMO_VIDEO_FORMAT_RGB15,
    WINEDMO_VIDEO_FORMAT_RGB16,
    WINEDMO_VIDEO_FORMAT_RGBA,

    WINEDMO_VIDEO_FORMAT_AYUV,
    WINEDMO_VIDEO_FORMAT_I420,
    WINEDMO_VIDEO_FORMAT_NV12,
    WINEDMO_VIDEO_FORMAT_P010_10LE,
    WINEDMO_VIDEO_FORMAT_UYVY,
    WINEDMO_VIDEO_FORMAT_YUY2,
    WINEDMO_VIDEO_FORMAT_YV12,
    WINEDMO_VIDEO_FORMAT_YVYU,

    WINEDMO_VIDEO_FORMAT_WMV1,
    WINEDMO_VIDEO_FORMAT_WMV2,
    WINEDMO_VIDEO_FORMAT_WMV3,
    WINEDMO_VIDEO_FORMAT_WMVA,
    WINEDMO_VIDEO_FORMAT_WVC1,
};

struct winedmo_codec_format
{
    winedmo_major_type major_type;

    union
    {
        /* Valid members for different audio formats:
         *
         * Uncompressed(PCM): channels, channel_mask, rate.
         * MPEG1: channels, rate, layer.
         * MPEG4: payload_type, codec_data_len, codec_data.
         * WMA: channels, rate, bitrate, depth, block_align, version, layer,
         *         payload_type, codec_data_len, codec_data */
        struct
        {
            winedmo_audio_format format;

            uint32_t channels;
            uint32_t channel_mask; /* In WinMM format. */
            uint32_t rate;
            uint32_t bitrate;
            uint32_t depth;
            uint32_t block_align;
            uint32_t version;
            uint32_t layer;
            uint32_t payload_type;
            uint32_t codec_data_len;
            unsigned char codec_data[64];
            UINT8 is_xma;
        } audio;

        /* Valid members for different video formats:
         *
         * Uncompressed(RGB and YUV): width, height, fps_n, fps_d, padding.
         * CINEPAK: width, height, fps_n, fps_d.
         * H264: width, height, fps_n, fps_d, profile, level, codec_data_len, codec_data.
         * WMV: width, height, fps_n, fps_d, codec_data_len, codec_data.
         * INDEO: width, height, fps_n, fps_d, version.
         * MPEG1: width, height, fps_n, fps_d. */
        struct
        {
            winedmo_video_format format;

            /* Positive height indicates top-down video; negative height
             * indicates bottom-up video. */
            int32_t width, height;
            uint32_t fps_n, fps_d;
            RECT padding;
            uint32_t profile;
            uint32_t level;
            uint32_t version;
            uint32_t codec_data_len;
            unsigned char codec_data[64];
        } video;
    } u;
};

enum winedmo_sample_flag
{
    WINEDMO_SAMPLE_FLAG_INCOMPLETE = 1,
    WINEDMO_SAMPLE_FLAG_HAS_PTS = 2,
    WINEDMO_SAMPLE_FLAG_HAS_DURATION = 4,
    WINEDMO_SAMPLE_FLAG_SYNC_POINT = 8,
    WINEDMO_SAMPLE_FLAG_DISCONTINUITY = 0x10,
    WINEDMO_SAMPLE_FLAG_PRESERVE_TIMESTAMPS = 0x20,
};

struct winedmo_sample
{
    /* timestamp and duration are in 100-nanosecond units. */
    INT64 pts;
    UINT64 duration;
    LONG refcount; /* unix refcount */
    UINT32 flags;
    UINT32 max_size;
    UINT32 size;
    UINT64 data; /* pointer to user memory */
};

struct winedmo_parser_buffer
{
    /* pts and duration are in 100-nanosecond units. */
    UINT64 pts, duration;
    UINT32 size;
    UINT32 stream;
    UINT8 discontinuity, preroll, delta, has_pts, has_duration;
};
C_ASSERT(sizeof(struct winedmo_parser_buffer) == 32);

typedef UINT64 winedmo_parser_t;
typedef UINT64 winedmo_parser_stream_t;
typedef UINT64 winedmo_transform_t;
typedef UINT64 winedmo_muxer_t;

struct winedmo_init_params
{
    UINT8 trace_on;
    UINT8 warn_on;
    UINT8 err_on;
};

struct winedmo_parser_create_params
{
    winedmo_parser_t parser;
    UINT8 output_compressed;
    UINT8 use_opengl;
    UINT8 err_on;
    UINT8 warn_on;
};

struct winedmo_parser_connect_params
{
    winedmo_parser_t parser;
    const WCHAR *uri;
    UINT64 file_size;
};

struct winedmo_parser_get_next_read_offset_params
{
    winedmo_parser_t parser;
    UINT32 size;
    UINT64 offset;
};

struct winedmo_parser_push_data_params
{
    winedmo_parser_t parser;
    const void *data;
    UINT32 size;
};

struct winedmo_parser_get_stream_count_params
{
    winedmo_parser_t parser;
    UINT32 count;
};

struct winedmo_parser_get_stream_params
{
    winedmo_parser_t parser;
    UINT32 index;
    winedmo_parser_stream_t stream;
};

struct winedmo_parser_stream_get_current_format_params
{
    winedmo_parser_stream_t stream;
    struct winedmo_codec_format *format;
};

struct winedmo_parser_stream_get_codec_format_params
{
    winedmo_parser_stream_t stream;
    struct winedmo_codec_format *format;
};

struct winedmo_parser_stream_enable_params
{
    winedmo_parser_stream_t stream;
    const struct winedmo_codec_format *format;
};

struct winedmo_parser_stream_get_buffer_params
{
    winedmo_parser_t parser;
    winedmo_parser_stream_t stream;
    struct winedmo_parser_buffer *buffer;
};

struct winedmo_parser_stream_copy_buffer_params
{
    winedmo_parser_stream_t stream;
    void *data;
    UINT32 offset;
    UINT32 size;
};

struct winedmo_parser_stream_notify_qos_params
{
    winedmo_parser_stream_t stream;
    UINT8 underflow;
    DOUBLE proportion;
    INT64 diff;
    UINT64 timestamp;
};

struct winedmo_parser_stream_get_duration_params
{
    winedmo_parser_stream_t stream;
    UINT64 duration;
};

typedef UINT64 winedmo_parser_tag;
enum winedmo_parser_tag
{
    WINEDMO_PARSER_TAG_LANGUAGE,
    WINEDMO_PARSER_TAG_NAME,
    WINEDMO_PARSER_TAG_COUNT
};

struct winedmo_parser_stream_get_tag_params
{
    winedmo_parser_stream_t stream;
    winedmo_parser_tag tag;
    char *buffer;
    UINT32 *size;
};

struct winedmo_parser_stream_seek_params
{
    winedmo_parser_stream_t stream;
    DOUBLE rate;
    UINT64 start_pos, stop_pos;
    DWORD start_flags, stop_flags;
};

struct winedmo_transform_attrs
{
    UINT32 output_plane_align;
    UINT32 input_queue_length;
    BOOL allow_format_change;
    BOOL low_latency;
    BOOL preserve_timestamps;
};

struct winedmo_transform_create_params
{
    winedmo_transform_t transform;
    struct winedmo_media_type input_type;
    struct winedmo_media_type output_type;
    struct winedmo_transform_attrs attrs;
};

struct winedmo_transform_push_data_params
{
    winedmo_transform_t transform;
    struct winedmo_sample *sample;
    HRESULT result;
};

struct winedmo_transform_read_data_params
{
    winedmo_transform_t transform;
    struct winedmo_sample *sample;
    HRESULT result;
};

struct winedmo_transform_get_output_type_params
{
    winedmo_transform_t transform;
    struct winedmo_media_type media_type;
};

struct winedmo_transform_set_output_type_params
{
    winedmo_transform_t transform;
    struct winedmo_media_type media_type;
};

struct winedmo_transform_get_status_params
{
    winedmo_transform_t transform;
    UINT32 accepts_input;
};

struct winedmo_transform_notify_qos_params
{
    winedmo_transform_t transform;
    UINT8 underflow;
    DOUBLE proportion;
    INT64 diff;
    UINT64 timestamp;
};

struct winedmo_muxer_create_params
{
    winedmo_muxer_t muxer;
    const char *format;
};

struct winedmo_muxer_add_stream_params
{
    winedmo_muxer_t muxer;
    UINT32 stream_id;
    const struct winedmo_codec_format *format;
};

struct winedmo_muxer_push_sample_params
{
    winedmo_muxer_t muxer;
    struct winedmo_sample *sample;
    UINT32 stream_id;
};

struct winedmo_muxer_read_data_params
{
    winedmo_muxer_t muxer;
    void *buffer;
    UINT32 size;
    UINT64 offset;
};

#endif /* __WINE_WINEDMO_MEDIA_FORMAT_H */
