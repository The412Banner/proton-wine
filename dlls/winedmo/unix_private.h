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

#ifndef __WINE_CONFIG_H
# error You must include config.h to use this header
#endif

#include <stdint.h>

#ifdef HAVE_FFMPEG
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#ifdef HAVE_LIBAVCODEC_BSF_H
# include <libavcodec/bsf.h>
#endif
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#else
typedef struct AVFormatContext AVFormatContext;
typedef struct AVCodecParameters AVCodecParameters;
typedef struct AVRational AVRational;
#endif /* HAVE_FFMPEG */

#include "unixlib.h"
#include "wine/debug.h"

/* media-converter/demuxer.c */
extern void mediaconv_demuxer_init(void);
extern void mediaconv_demuxer_exit(void);
extern int mediaconv_demuxer_open( AVFormatContext **ctx, struct stream_context *context );

/* libavcodec/pcm_byte_order_reverse.c */
#ifdef HAVE_FFMPEG
#ifndef WINE_FF_BSF_DEFINED
#define WINE_FF_BSF_DEFINED
/* WineFFBitStreamFilter mirrors FFmpeg's internal FFBitStreamFilter layout. */
typedef struct WineFFBitStreamFilter
{
    AVBitStreamFilter p;
    int  priv_data_size;
    int  (*init)(AVBSFContext *ctx);
    int  (*filter)(AVBSFContext *ctx, AVPacket *pkt);
    void (*close)(AVBSFContext *ctx);
    void (*flush)(AVBSFContext *ctx);
} WineFFBitStreamFilter;
#endif /* WINE_FF_BSF_DEFINED */
extern const WineFFBitStreamFilter ff_pcm_byte_order_reverse_bsf;
#endif

/* unixlib.c */
extern int64_t unix_seek_callback( void *opaque, int64_t offset, int whence );
extern int unix_read_callback( void *opaque, uint8_t *buffer, int size );

/* unix_demuxer.c */
extern NTSTATUS demuxer_check( void * );
extern NTSTATUS demuxer_create( void * );
extern NTSTATUS demuxer_destroy( void * );
extern NTSTATUS demuxer_read( void * );
extern NTSTATUS demuxer_seek( void * );
extern NTSTATUS demuxer_stream_lang( void * );
extern NTSTATUS demuxer_stream_name( void * );
extern NTSTATUS demuxer_stream_type( void * );

/* unix_media_type.c */
extern NTSTATUS media_type_from_codec_params( const AVCodecParameters *params, const AVRational *sar, const AVRational *fps,
                                              UINT32 align, struct media_type *media_type );

/* unix_muxer.c */
extern NTSTATUS muxer_create( void * );
extern NTSTATUS muxer_destroy( void * );
extern NTSTATUS muxer_add_stream( void * );
extern NTSTATUS muxer_start( void * );
extern NTSTATUS muxer_push_sample( void * );
extern NTSTATUS muxer_read_data( void * );
extern NTSTATUS muxer_finalize( void * );

/* unix_transform.c */
extern NTSTATUS transform_create( void * );
extern NTSTATUS transform_destroy( void * );
extern NTSTATUS transform_push_input( void * );
extern NTSTATUS transform_get_output( void * );
extern NTSTATUS transform_drain( void * );
extern NTSTATUS transform_flush( void * );
extern NTSTATUS transform_get_output_format( void * );
extern NTSTATUS transform_set_output_format( void * );
