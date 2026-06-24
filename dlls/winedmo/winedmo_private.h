/*
 * winedmo DirectShow and Media Foundation backend
 *
 * Copyright 2010 Maarten Lankhorst for CodeWeavers
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

#ifndef __WINEDMO_PRIVATE_INCLUDED__
#define __WINEDMO_PRIVATE_INCLUDED__

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define COBJMACROS
#define NONAMELESSSTRUCT
#define NONAMELESSUNION
#include "dshow.h"
#include "mfidl.h"
#include "wine/debug.h"
#include "wine/strmbase.h"
#include "wine/mfinternal.h"

#include "media_format.h"

bool array_reserve(void **elements, size_t *capacity, size_t count, size_t size);

#define MEDIATIME_FROM_BYTES(x) ((LONGLONG)(x) * 10000000)

static inline BOOL is_mf_video_area_empty(const MFVideoArea *area)
{
    return !area->OffsetX.value && !area->OffsetY.value && !area->Area.cx && !area->Area.cy;
}

static inline void get_mf_video_content_rect(const MFVideoInfo *info, RECT *rect)
{
    if (!is_mf_video_area_empty(&info->MinimumDisplayAperture))
    {
        rect->left = info->MinimumDisplayAperture.OffsetX.value;
        rect->top = info->MinimumDisplayAperture.OffsetY.value;
        rect->right = rect->left + info->MinimumDisplayAperture.Area.cx;
        rect->bottom = rect->top + info->MinimumDisplayAperture.Area.cy;
    }
    else
    {
        rect->left = 0;
        rect->top = 0;
        rect->right = info->dwWidth;
        rect->bottom = info->dwHeight;
    }
}

struct winedmo_sample_queue;

HRESULT winedmo_sample_queue_create(struct winedmo_sample_queue **out);
void winedmo_sample_queue_destroy(struct winedmo_sample_queue *queue);
void winedmo_sample_queue_flush(struct winedmo_sample_queue *queue, bool all);


unsigned int winedmo_format_get_bytes_for_uncompressed(winedmo_video_format format, unsigned int width, unsigned int height);
unsigned int winedmo_format_get_max_size(const struct winedmo_codec_format *format);

HRESULT avi_splitter_create(IUnknown *outer, IUnknown **out);
HRESULT asf_splitter_create(IUnknown *outer, IUnknown **out);
HRESULT decodebin_parser_create(IUnknown *outer, IUnknown **out);
HRESULT aac_audio_decoder_create(IUnknown *outer, IUnknown **out);
HRESULT ac3_audio_decoder_create(IUnknown *outer, IUnknown **out);
HRESULT mpeg_audio_codec_create(IUnknown *outer, IUnknown **out);
HRESULT mpeg_video_codec_create(IUnknown *outer, IUnknown **out);
HRESULT mpeg_layer3_decoder_create(IUnknown *outer, IUnknown **out);
HRESULT mpeg_splitter_create(IUnknown *outer, IUnknown **out);
HRESULT wave_parser_create(IUnknown *outer, IUnknown **out);
HRESULT wma_decoder_create(IUnknown *outer, IUnknown **out);
HRESULT wmv_decoder_create(IUnknown *outer, IUnknown **out);
HRESULT resampler_create(IUnknown *outer, IUnknown **out);
HRESULT color_convert_create(IUnknown *outer, IUnknown **out);
HRESULT mp3_sink_class_factory_create(IUnknown *outer, IUnknown **out);
HRESULT mpeg4_sink_class_factory_create(IUnknown *outer, IUnknown **out);

bool amt_from_winedmo_format(AM_MEDIA_TYPE *mt, const struct winedmo_codec_format *format, bool wm);
bool amt_to_winedmo_format(const AM_MEDIA_TYPE *mt, struct winedmo_codec_format *format);

extern HRESULT mfplat_get_class_object(REFCLSID rclsid, REFIID riid, void **obj);

IMFMediaType *mf_media_type_from_winedmo_format(const struct winedmo_codec_format *format);
void mf_media_type_to_winedmo_format(IMFMediaType *type, struct winedmo_codec_format *format);

HRESULT winedmo_sample_create_mf(IMFSample *sample, struct winedmo_sample **out);
void winedmo_sample_release(struct winedmo_sample *winedmo_sample);

HRESULT winedmo_byte_stream_handler_create(REFIID riid, void **obj);

unsigned int winedmo_format_get_stride(const struct winedmo_codec_format *format);

bool winedmo_video_format_is_rgb(enum winedmo_video_format format);

HRESULT audio_decoder_create(REFIID riid, void **ret);
HRESULT aac_decoder_create(REFIID riid, void **ret);
HRESULT h264_decoder_create(REFIID riid, void **ret);
HRESULT h264_decoder_create_aggregated(IUnknown *outer, REFIID riid, void **ret);
HRESULT video_processor_create(REFIID riid, void **ret);
HRESULT winedmo_scheme_handler_create(REFIID riid, void **ret);

HRESULT h264_encoder_create(REFIID riid, void **ret);

extern const GUID MFAudioFormat_RAW_AAC;

#endif /* __WINEDMO_PRIVATE_INCLUDED__ */
