/*
 * DirectShow parser filters
 *
 * Copyright 2010 Maarten Lankhorst for CodeWeavers
 * Copyright 2010 Aric Stewart for CodeWeavers
 * Copyright 2019-2020 Zebediah Figura
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

#include "winedmo_private.h"
#include "winedmo_guids.h"

#include "amvideo.h"

#include <limits.h>
#include "dvdmedia.h"
#include "d3d9types.h"
#include "mmreg.h"
#include "ks.h"
#include "wmcodecdsp.h"
#include "initguid.h"
#include "ksmedia.h"

WINE_DEFAULT_DEBUG_CHANNEL(quartz);

static const GUID MEDIASUBTYPE_CVID = {mmioFOURCC('c','v','i','d'), 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID MEDIASUBTYPE_VC1S = {mmioFOURCC('V','C','1','S'), 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID MEDIASUBTYPE_MP3  = {WAVE_FORMAT_MPEGLAYER3, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID MEDIASUBTYPE_WMV_Unknown = {0x7ce12ca9, 0xbfbf, 0x43d9, {0x9d, 0x00, 0x82, 0xb8, 0xed, 0x54, 0x31, 0x6b}};
DEFINE_GUID(MEDIASUBTYPE_ABGR32,D3DFMT_A8B8G8R8,0x524f,0x11ce,0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70);
static const GUID MEDIASUBTYPE_XMAUDIO2 = {0x0166, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

static bool winedmo_audio_format_is_float(enum winedmo_audio_format format)
{
    switch (format)
    {
        case WINEDMO_AUDIO_FORMAT_UNKNOWN: return false;
        case WINEDMO_AUDIO_FORMAT_U8: return false;
        case WINEDMO_AUDIO_FORMAT_S16LE: return false;
        case WINEDMO_AUDIO_FORMAT_S24LE: return false;
        case WINEDMO_AUDIO_FORMAT_S32LE: return false;
        case WINEDMO_AUDIO_FORMAT_F32LE: return true;
        case WINEDMO_AUDIO_FORMAT_F64LE: return true;
    }

    assert(0);
    return false;
}

static WORD winedmo_audio_format_get_depth(enum winedmo_audio_format format)
{
    switch (format)
    {
        case WINEDMO_AUDIO_FORMAT_UNKNOWN: return 0;
        case WINEDMO_AUDIO_FORMAT_U8: return 8;
        case WINEDMO_AUDIO_FORMAT_S16LE: return 16;
        case WINEDMO_AUDIO_FORMAT_S24LE: return 24;
        case WINEDMO_AUDIO_FORMAT_S32LE: return 32;
        case WINEDMO_AUDIO_FORMAT_F32LE: return 32;
        case WINEDMO_AUDIO_FORMAT_F64LE: return 64;
    }

    assert(0);
    return 0;
}

static bool amt_from_winedmo_format_audio(AM_MEDIA_TYPE *mt, const struct winedmo_codec_format *format)
{
    mt->majortype = MEDIATYPE_Audio;
    mt->formattype = FORMAT_WaveFormatEx;

    switch (format->u.audio.format)
    {
    case WINEDMO_AUDIO_FORMAT_UNKNOWN:
        return false;

    case WINEDMO_AUDIO_FORMAT_U8:
    case WINEDMO_AUDIO_FORMAT_S16LE:
    case WINEDMO_AUDIO_FORMAT_S24LE:
    case WINEDMO_AUDIO_FORMAT_S32LE:
    case WINEDMO_AUDIO_FORMAT_F32LE:
    case WINEDMO_AUDIO_FORMAT_F64LE:
    {
        bool is_float = winedmo_audio_format_is_float(format->u.audio.format);
        WORD depth = winedmo_audio_format_get_depth(format->u.audio.format);

        if (is_float || format->u.audio.channels > 2)
        {
            WAVEFORMATEXTENSIBLE *wave_format;

            if (!(wave_format = CoTaskMemAlloc(sizeof(*wave_format))))
                return false;
            memset(wave_format, 0, sizeof(*wave_format));

            mt->subtype = is_float ? MEDIASUBTYPE_IEEE_FLOAT : MEDIASUBTYPE_PCM;
            mt->bFixedSizeSamples = TRUE;
            mt->pbFormat = (BYTE *)wave_format;
            mt->cbFormat = sizeof(*wave_format);
            wave_format->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            wave_format->Format.nChannels = format->u.audio.channels;
            wave_format->Format.nSamplesPerSec = format->u.audio.rate;
            wave_format->Format.nAvgBytesPerSec = format->u.audio.rate * format->u.audio.channels * depth / 8;
            wave_format->Format.nBlockAlign = format->u.audio.channels * depth / 8;
            wave_format->Format.wBitsPerSample = depth;
            wave_format->Format.cbSize = sizeof(*wave_format) - sizeof(WAVEFORMATEX);
            wave_format->Samples.wValidBitsPerSample = depth;
            wave_format->dwChannelMask = format->u.audio.channel_mask;
            wave_format->SubFormat = is_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
            mt->lSampleSize = wave_format->Format.nBlockAlign;
        }
        else
        {
            WAVEFORMATEX *wave_format;

            if (!(wave_format = CoTaskMemAlloc(sizeof(*wave_format))))
                return false;
            memset(wave_format, 0, sizeof(*wave_format));

            mt->subtype = MEDIASUBTYPE_PCM;
            mt->bFixedSizeSamples = TRUE;
            mt->pbFormat = (BYTE *)wave_format;
            mt->cbFormat = sizeof(*wave_format);
            wave_format->wFormatTag = WAVE_FORMAT_PCM;
            wave_format->nChannels = format->u.audio.channels;
            wave_format->nSamplesPerSec = format->u.audio.rate;
            wave_format->nAvgBytesPerSec = format->u.audio.rate * format->u.audio.channels * depth / 8;
            wave_format->nBlockAlign = format->u.audio.channels * depth / 8;
            wave_format->wBitsPerSample = depth;
            wave_format->cbSize = 0;
            mt->lSampleSize = wave_format->nBlockAlign;
        }
        return true;
    }
    }

    assert(0);
    return false;
}

static bool amt_from_winedmo_format_audio_mpeg1(AM_MEDIA_TYPE *mt, const struct winedmo_codec_format *format)
{
    mt->majortype = MEDIATYPE_Audio;
    mt->formattype = FORMAT_WaveFormatEx;

    switch (format->u.audio.layer)
    {
        case 1:
        case 2:
        {
            MPEG1WAVEFORMAT *wave_format;

            if (!(wave_format = CoTaskMemAlloc(sizeof(*wave_format))))
                return false;
            memset(wave_format, 0, sizeof(*wave_format));

            mt->subtype = MEDIASUBTYPE_MPEG1AudioPayload;
            mt->cbFormat = sizeof(*wave_format);
            mt->pbFormat = (BYTE *)wave_format;
            wave_format->wfx.wFormatTag = WAVE_FORMAT_MPEG;
            wave_format->wfx.nChannels = format->u.audio.channels;
            wave_format->wfx.nSamplesPerSec = format->u.audio.rate;
            wave_format->wfx.cbSize = sizeof(*wave_format) - sizeof(WAVEFORMATEX);
            wave_format->fwHeadLayer = format->u.audio.layer;
            return true;
        }

        case 3:
        {
            MPEGLAYER3WAVEFORMAT *wave_format;

            if (!(wave_format = CoTaskMemAlloc(sizeof(*wave_format))))
                return false;
            memset(wave_format, 0, sizeof(*wave_format));

            mt->subtype = MEDIASUBTYPE_MP3;
            mt->cbFormat = sizeof(*wave_format);
            mt->pbFormat = (BYTE *)wave_format;
            wave_format->wfx.wFormatTag = WAVE_FORMAT_MPEGLAYER3;
            wave_format->wfx.nChannels = format->u.audio.channels;
            wave_format->wfx.nSamplesPerSec = format->u.audio.rate;
            wave_format->wfx.cbSize = sizeof(*wave_format) - sizeof(WAVEFORMATEX);
            /* FIXME: We can't get most of the MPEG data from the caps. We may have
             * to manually parse the header. */
            wave_format->wID = MPEGLAYER3_ID_MPEG;
            wave_format->fdwFlags = MPEGLAYER3_FLAG_PADDING_ON;
            wave_format->nFramesPerBlock = 1;
            wave_format->nCodecDelay = 1393;
            return true;
        }
    }

    assert(0);
    return false;
}

static bool amt_from_winedmo_format_audio_wma(AM_MEDIA_TYPE *mt, const struct winedmo_codec_format *format)
{
    DWORD codec_data_len, size;
    WAVEFORMATEX *wave_format;
    const GUID *subtype;
    WORD fmt_tag;

    mt->majortype = MEDIATYPE_Audio;
    mt->formattype = FORMAT_WaveFormatEx;

    switch (format->u.audio.version)
    {
        case 1:
            subtype = &MEDIASUBTYPE_MSAUDIO1;
            codec_data_len = MSAUDIO1_WFX_EXTRA_BYTES;
            fmt_tag = WAVE_FORMAT_MSAUDIO1;
            break;
        case 2:
            subtype = &MEDIASUBTYPE_WMAUDIO2;
            codec_data_len = WMAUDIO2_WFX_EXTRA_BYTES;
            fmt_tag = WAVE_FORMAT_WMAUDIO2;
            if (format->u.audio.is_xma)
            {
                subtype = &MEDIASUBTYPE_XMAUDIO2;
                fmt_tag = 0x0166;
            }
            break;
        case 3:
            subtype = &MEDIASUBTYPE_WMAUDIO3;
            codec_data_len = WMAUDIO3_WFX_EXTRA_BYTES;
            fmt_tag = WAVE_FORMAT_WMAUDIO3;
            break;
        case 4:
            subtype = &MEDIASUBTYPE_WMAUDIO_LOSSLESS;
            codec_data_len = 18;
            fmt_tag = WAVE_FORMAT_WMAUDIO_LOSSLESS;
            break;
        default:
            assert(false);
            return false;
    }

    size = sizeof(WAVEFORMATEX) + codec_data_len;
    if (!(wave_format = CoTaskMemAlloc(size)))
        return false;
    memset(wave_format, 0, size);

    mt->subtype = *subtype;
    mt->bFixedSizeSamples = TRUE;
    mt->lSampleSize = format->u.audio.block_align;
    mt->cbFormat = size;
    mt->pbFormat = (BYTE *)wave_format;
    wave_format->wFormatTag = fmt_tag;
    wave_format->nChannels = format->u.audio.channels;
    wave_format->nSamplesPerSec = format->u.audio.rate;
    wave_format->nAvgBytesPerSec = format->u.audio.bitrate / 8;
    wave_format->nBlockAlign = format->u.audio.block_align;
    wave_format->wBitsPerSample = format->u.audio.depth;
    wave_format->cbSize = codec_data_len;

    if (format->u.audio.codec_data_len == codec_data_len)
        memcpy(wave_format+1, format->u.audio.codec_data, format->u.audio.codec_data_len);
    else
        FIXME("Unexpected codec_data length; got %u, expected %lu\n", format->u.audio.codec_data_len, codec_data_len);
    return true;
}

#define ALIGN(n, alignment) (((n) + (alignment) - 1) & ~((alignment) - 1))

static unsigned int winedmo_format_get_max_size_video_raw(enum winedmo_video_format format, unsigned int width, unsigned int height)
{
    switch (format)
    {
        case WINEDMO_VIDEO_FORMAT_BGRA:
        case WINEDMO_VIDEO_FORMAT_BGRx:
        case WINEDMO_VIDEO_FORMAT_AYUV:
        case WINEDMO_VIDEO_FORMAT_RGBA:
            return width * height * 4;

        case WINEDMO_VIDEO_FORMAT_BGR:
            return ALIGN(width * 3, 4) * height;

        case WINEDMO_VIDEO_FORMAT_RGB15:
        case WINEDMO_VIDEO_FORMAT_RGB16:
        case WINEDMO_VIDEO_FORMAT_UYVY:
        case WINEDMO_VIDEO_FORMAT_YUY2:
        case WINEDMO_VIDEO_FORMAT_YVYU:
            return ALIGN(width * 2, 4) * height;

        case WINEDMO_VIDEO_FORMAT_I420:
        case WINEDMO_VIDEO_FORMAT_YV12:
            return ALIGN(width, 4) * ALIGN(height, 2) /* Y plane */
                    + 2 * ALIGN((width + 1) / 2, 4) * ((height + 1) / 2); /* U and V planes */

        case WINEDMO_VIDEO_FORMAT_NV12:
            return ALIGN(width, 4) * ALIGN(height, 2) /* Y plane */
                    + ALIGN(width, 4) * ((height + 1) / 2); /* U/V plane */

        default:
            FIXME("Cannot guess maximum sample size for video format %d.\n", format);
            return 0;
    }

    assert(0);
    return 0;
}

unsigned int winedmo_format_get_max_size(const struct winedmo_codec_format *format)
{
    switch (format->major_type)
    {
        case WINEDMO_MAJOR_TYPE_VIDEO:
            return winedmo_format_get_max_size_video_raw(format->u.video.format,
                    format->u.video.width, abs(format->u.video.height));

        case WINEDMO_MAJOR_TYPE_VIDEO_CINEPAK:
            /* Both ffmpeg's encoder and a Cinepak file seen in the wild report
             * 24 bpp. ffmpeg sets biSizeImage as below; others may be smaller,
             * but as long as every sample fits into our allocator, we're fine. */
            return format->u.video.width * format->u.video.height * 3;

        case WINEDMO_MAJOR_TYPE_VIDEO_MPEG1:
        case WINEDMO_MAJOR_TYPE_VIDEO_WMV:
            /* Estimated max size of a compressed video frame.
             * There's no way to no way to know the real upper bound,
             * so let's just use the decompressed size and hope it works. */
            return winedmo_format_get_max_size_video_raw(WINEDMO_VIDEO_FORMAT_YV12,
                    format->u.video.width, format->u.video.height);

        case WINEDMO_MAJOR_TYPE_AUDIO:
        {
            unsigned int rate = format->u.audio.rate, channels = format->u.audio.channels;

            /* Actually we don't know how large of a sample the decoder will give
             * us. Hopefully 1 second is enough... */

            switch (format->u.audio.format)
            {
                case WINEDMO_AUDIO_FORMAT_U8:
                    return rate * channels;

                case WINEDMO_AUDIO_FORMAT_S16LE:
                    return rate * channels * 2;

                case WINEDMO_AUDIO_FORMAT_S24LE:
                    return rate * channels * 3;

                case WINEDMO_AUDIO_FORMAT_S32LE:
                case WINEDMO_AUDIO_FORMAT_F32LE:
                    return rate * channels * 4;

                case WINEDMO_AUDIO_FORMAT_F64LE:
                    return rate * channels * 8;

                case WINEDMO_AUDIO_FORMAT_UNKNOWN:
                    FIXME("Cannot guess maximum sample size for unknown audio format.\n");
                    return 0;
            }
            break;
        }

        case WINEDMO_MAJOR_TYPE_AUDIO_MPEG1:
            switch (format->u.audio.layer)
            {
            case 1:
                return 56000;

            case 2:
                return 48000;

            case 3:
                return 40000;
            }
            break;

        case WINEDMO_MAJOR_TYPE_AUDIO_WMA:
            /* Estimated max size of a compressed audio frame.
             * There's no way to no way to know the real upper bound,
             * so let's just use one second of decompressed size and hope it works. */
            return format->u.audio.rate * format->u.audio.channels * format->u.audio.depth / 8;

        case WINEDMO_MAJOR_TYPE_AUDIO_MPEG4:
        case WINEDMO_MAJOR_TYPE_VIDEO_H264:
        case WINEDMO_MAJOR_TYPE_VIDEO_INDEO:
            FIXME("Format %u not implemented!\n", format->major_type);
            return 0;

        case WINEDMO_MAJOR_TYPE_UNKNOWN:
            FIXME("Cannot guess maximum sample size for unknown format.\n");
            return 0;
    }

    assert(0);
    return 0;
}

static const GUID *winedmo_video_format_get_mediasubtype(enum winedmo_video_format format)
{
    switch (format)
    {
        case WINEDMO_VIDEO_FORMAT_UNKNOWN: return &GUID_NULL;
        case WINEDMO_VIDEO_FORMAT_BGRA: return &MEDIASUBTYPE_ARGB32;
        case WINEDMO_VIDEO_FORMAT_BGRx: return &MEDIASUBTYPE_RGB32;
        case WINEDMO_VIDEO_FORMAT_BGR: return &MEDIASUBTYPE_RGB24;
        case WINEDMO_VIDEO_FORMAT_RGB15: return &MEDIASUBTYPE_RGB555;
        case WINEDMO_VIDEO_FORMAT_RGB16: return &MEDIASUBTYPE_RGB565;
        case WINEDMO_VIDEO_FORMAT_RGBA: return &MEDIASUBTYPE_ABGR32;
        case WINEDMO_VIDEO_FORMAT_AYUV: return &MEDIASUBTYPE_AYUV;
        case WINEDMO_VIDEO_FORMAT_I420: return &MEDIASUBTYPE_I420;
        case WINEDMO_VIDEO_FORMAT_NV12: return &MEDIASUBTYPE_NV12;
        case WINEDMO_VIDEO_FORMAT_P010_10LE: return &MEDIASUBTYPE_P010;
        case WINEDMO_VIDEO_FORMAT_UYVY: return &MEDIASUBTYPE_UYVY;
        case WINEDMO_VIDEO_FORMAT_YUY2: return &MEDIASUBTYPE_YUY2;
        case WINEDMO_VIDEO_FORMAT_YV12: return &MEDIASUBTYPE_YV12;
        case WINEDMO_VIDEO_FORMAT_YVYU: return &MEDIASUBTYPE_YVYU;
        case WINEDMO_VIDEO_FORMAT_WMV1: return &MEDIASUBTYPE_WMV1;
        case WINEDMO_VIDEO_FORMAT_WMV2: return &MEDIASUBTYPE_WMV2;
        case WINEDMO_VIDEO_FORMAT_WMV3: return &MEDIASUBTYPE_WMV3;
        case WINEDMO_VIDEO_FORMAT_WMVA: return &MEDIASUBTYPE_WMVA;
        case WINEDMO_VIDEO_FORMAT_WVC1: return &MEDIASUBTYPE_WVC1;
    }

    assert(0);
    return NULL;
}

static DWORD winedmo_video_format_get_compression(enum winedmo_video_format format)
{
    switch (format)
    {
        case WINEDMO_VIDEO_FORMAT_UNKNOWN: return 0;
        case WINEDMO_VIDEO_FORMAT_BGRA: return BI_RGB;
        case WINEDMO_VIDEO_FORMAT_BGRx: return BI_RGB;
        case WINEDMO_VIDEO_FORMAT_BGR: return BI_RGB;
        case WINEDMO_VIDEO_FORMAT_RGB15: return BI_RGB;
        case WINEDMO_VIDEO_FORMAT_RGB16: return BI_BITFIELDS;
        case WINEDMO_VIDEO_FORMAT_RGBA: return BI_RGB;
        case WINEDMO_VIDEO_FORMAT_AYUV: return mmioFOURCC('A','Y','U','V');
        case WINEDMO_VIDEO_FORMAT_I420: return mmioFOURCC('I','4','2','0');
        case WINEDMO_VIDEO_FORMAT_NV12: return mmioFOURCC('N','V','1','2');
        case WINEDMO_VIDEO_FORMAT_UYVY: return mmioFOURCC('U','Y','V','Y');
        case WINEDMO_VIDEO_FORMAT_YUY2: return mmioFOURCC('Y','U','Y','2');
        case WINEDMO_VIDEO_FORMAT_YV12: return mmioFOURCC('Y','V','1','2');
        case WINEDMO_VIDEO_FORMAT_YVYU: return mmioFOURCC('Y','V','Y','U');
        default:
            ERR("Cannot get compression for video format %d.", format);
            return 0;
    }
}

static WORD winedmo_video_format_get_depth(enum winedmo_video_format format)
{
    switch (format)
    {
        case WINEDMO_VIDEO_FORMAT_UNKNOWN: return 0;
        case WINEDMO_VIDEO_FORMAT_BGRA: return 32;
        case WINEDMO_VIDEO_FORMAT_BGRx: return 32;
        case WINEDMO_VIDEO_FORMAT_BGR: return 24;
        case WINEDMO_VIDEO_FORMAT_RGB15: return 16;
        case WINEDMO_VIDEO_FORMAT_RGB16: return 16;
        case WINEDMO_VIDEO_FORMAT_RGBA: return 32;
        case WINEDMO_VIDEO_FORMAT_AYUV: return 32;
        case WINEDMO_VIDEO_FORMAT_I420: return 12;
        case WINEDMO_VIDEO_FORMAT_NV12: return 12;
        case WINEDMO_VIDEO_FORMAT_UYVY: return 16;
        case WINEDMO_VIDEO_FORMAT_YUY2: return 16;
        case WINEDMO_VIDEO_FORMAT_YV12: return 12;
        case WINEDMO_VIDEO_FORMAT_YVYU: return 16;
        default:
            ERR("Cannot get depth for video format %d.", format);
            return 0;
    }
}

static bool amt_from_winedmo_format_video(AM_MEDIA_TYPE *mt, const struct winedmo_codec_format *format, bool wm)
{
    VIDEOINFO *video_format;
    uint32_t frame_time;

    if (format->u.video.format == WINEDMO_VIDEO_FORMAT_UNKNOWN)
        return false;

    if (!(video_format = CoTaskMemAlloc(sizeof(*video_format))))
        return false;

    mt->majortype = MEDIATYPE_Video;
    mt->subtype = *winedmo_video_format_get_mediasubtype(format->u.video.format);
    if (wm)
        mt->bFixedSizeSamples = TRUE;
    else
        mt->bTemporalCompression = TRUE;
    mt->lSampleSize = 1;
    mt->formattype = FORMAT_VideoInfo;
    mt->cbFormat = sizeof(VIDEOINFOHEADER);
    mt->pbFormat = (BYTE *)video_format;

    memset(video_format, 0, sizeof(*video_format));

    if (wm)
    {
        SetRect(&video_format->rcSource, 0, 0, format->u.video.width, abs(format->u.video.height));
        video_format->rcTarget = video_format->rcSource;
    }
    if ((frame_time = MulDiv(10000000, format->u.video.fps_d, format->u.video.fps_n)) != -1)
        video_format->AvgTimePerFrame = frame_time;
    video_format->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    video_format->bmiHeader.biWidth = format->u.video.width;
    if (winedmo_video_format_is_rgb(format->u.video.format))
        video_format->bmiHeader.biHeight = -format->u.video.height;
    else
        video_format->bmiHeader.biHeight = abs(format->u.video.height);
    video_format->bmiHeader.biPlanes = 1;
    video_format->bmiHeader.biBitCount = winedmo_video_format_get_depth(format->u.video.format);
    video_format->bmiHeader.biCompression = winedmo_video_format_get_compression(format->u.video.format);
    video_format->bmiHeader.biSizeImage = winedmo_format_get_max_size(format);

    if (format->u.video.format == WINEDMO_VIDEO_FORMAT_RGB16)
    {
        mt->cbFormat = offsetof(VIDEOINFO, dwBitMasks[3]);
        video_format->dwBitMasks[iRED]   = 0xf800;
        video_format->dwBitMasks[iGREEN] = 0x07e0;
        video_format->dwBitMasks[iBLUE]  = 0x001f;
    }

    return true;
}

static bool amt_from_winedmo_format_video_cinepak(AM_MEDIA_TYPE *mt, const struct winedmo_codec_format *format)
{
    VIDEOINFOHEADER *video_format;
    uint32_t frame_time;

    if (!(video_format = CoTaskMemAlloc(sizeof(*video_format))))
        return false;

    mt->majortype = MEDIATYPE_Video;
    mt->subtype = MEDIASUBTYPE_CVID;
    mt->bTemporalCompression = TRUE;
    mt->lSampleSize = 1;
    mt->formattype = FORMAT_VideoInfo;
    mt->cbFormat = sizeof(*video_format);
    mt->pbFormat = (BYTE *)video_format;

    memset(video_format, 0, sizeof(*video_format));
    if ((frame_time = MulDiv(10000000, format->u.video.fps_d, format->u.video.fps_n)) != -1)
        video_format->AvgTimePerFrame = frame_time;
    video_format->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    video_format->bmiHeader.biWidth = format->u.video.width;
    video_format->bmiHeader.biHeight = format->u.video.height;
    video_format->bmiHeader.biPlanes = 1;
    video_format->bmiHeader.biBitCount = 24;
    video_format->bmiHeader.biCompression = mt->subtype.Data1;
    video_format->bmiHeader.biSizeImage = winedmo_format_get_max_size(format);

    return true;
}

static bool amt_from_winedmo_format_video_wmv(AM_MEDIA_TYPE *mt, const struct winedmo_codec_format *format, bool wm)
{
    VIDEOINFOHEADER *video_format;
    uint32_t frame_time;
    const GUID *subtype;

    switch (format->u.video.format)
    {
        case WINEDMO_VIDEO_FORMAT_WMV1:
            subtype = &MEDIASUBTYPE_WMV1;
            break;
        case WINEDMO_VIDEO_FORMAT_WMV2:
            subtype = &MEDIASUBTYPE_WMV2;
            break;
        case WINEDMO_VIDEO_FORMAT_WMV3:
            subtype = &MEDIASUBTYPE_WMV3;
            break;
        case WINEDMO_VIDEO_FORMAT_WMVA:
            subtype = &MEDIASUBTYPE_WMVA;
            break;
        case WINEDMO_VIDEO_FORMAT_WVC1:
            subtype = &MEDIASUBTYPE_WVC1;
            break;
        default:
            WARN("Invalid WMV format %u.\n", format->u.video.format);
            return false;
    }

    if (!(video_format = CoTaskMemAlloc(sizeof(*video_format) + format->u.video.codec_data_len)))
        return false;

    mt->majortype = MEDIATYPE_Video;
    mt->subtype = *subtype;
    mt->bFixedSizeSamples = FALSE;
    mt->bTemporalCompression = TRUE;
    mt->lSampleSize = 0;
    mt->formattype = FORMAT_VideoInfo;
    mt->cbFormat = sizeof(*video_format) + format->u.video.codec_data_len;
    mt->pbFormat = (BYTE *)video_format;

    memset(video_format, 0, sizeof(*video_format));
    if (wm)
        SetRect(&video_format->rcSource, 0, 0, format->u.video.width, format->u.video.height);
    video_format->rcTarget = video_format->rcSource;
    if ((frame_time = MulDiv(10000000, format->u.video.fps_d, format->u.video.fps_n)) != -1)
        video_format->AvgTimePerFrame = frame_time;
    video_format->bmiHeader.biSize = sizeof(BITMAPINFOHEADER) + format->u.video.codec_data_len;
    video_format->bmiHeader.biWidth = format->u.video.width;
    video_format->bmiHeader.biHeight = format->u.video.height;
    video_format->bmiHeader.biPlanes = 1;
    video_format->bmiHeader.biCompression = mt->subtype.Data1;
    video_format->bmiHeader.biBitCount = 24;
    if (!wm)
        video_format->bmiHeader.biSizeImage = 3 * format->u.video.width * format->u.video.height;
    video_format->dwBitRate = 0;
    memcpy(video_format+1, format->u.video.codec_data, format->u.video.codec_data_len);

    return true;
}

static bool amt_from_winedmo_format_video_mpeg1(AM_MEDIA_TYPE *mt, const struct winedmo_codec_format *format)
{
    MPEG1VIDEOINFO *video_format;
    uint32_t frame_time;

    if (!(video_format = CoTaskMemAlloc(sizeof(*video_format))))
        return false;

    mt->majortype = MEDIATYPE_Video;
    mt->subtype = MEDIASUBTYPE_MPEG1Payload;
    mt->bTemporalCompression = TRUE;
    mt->lSampleSize = 1;
    mt->formattype = FORMAT_MPEGVideo;
    mt->cbFormat = sizeof(MPEG1VIDEOINFO);
    mt->pbFormat = (BYTE *)video_format;

    memset(video_format, 0, sizeof(*video_format));
    if ((frame_time = MulDiv(10000000, format->u.video.fps_d, format->u.video.fps_n)) != -1)
        video_format->hdr.AvgTimePerFrame = frame_time;
    video_format->hdr.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    video_format->hdr.bmiHeader.biWidth = format->u.video.width;
    video_format->hdr.bmiHeader.biHeight = format->u.video.height;
    video_format->hdr.bmiHeader.biPlanes = 1;
    video_format->hdr.bmiHeader.biBitCount = 12;
    video_format->hdr.bmiHeader.biCompression = mt->subtype.Data1;
    video_format->hdr.bmiHeader.biSizeImage = winedmo_format_get_max_size(format);

    return true;
}

bool amt_from_winedmo_format(AM_MEDIA_TYPE *mt, const struct winedmo_codec_format *format, bool wm)
{
    memset(mt, 0, sizeof(*mt));

    switch (format->major_type)
    {
    case WINEDMO_MAJOR_TYPE_AUDIO_MPEG4:
    case WINEDMO_MAJOR_TYPE_VIDEO_H264:
    case WINEDMO_MAJOR_TYPE_VIDEO_INDEO:
        FIXME("Format %u not implemented!\n", format->major_type);
        /* fallthrough */
    case WINEDMO_MAJOR_TYPE_UNKNOWN:
        return false;

    case WINEDMO_MAJOR_TYPE_AUDIO:
        return amt_from_winedmo_format_audio(mt, format);

    case WINEDMO_MAJOR_TYPE_AUDIO_MPEG1:
        return amt_from_winedmo_format_audio_mpeg1(mt, format);

    case WINEDMO_MAJOR_TYPE_AUDIO_WMA:
        return amt_from_winedmo_format_audio_wma(mt, format);

    case WINEDMO_MAJOR_TYPE_VIDEO:
        return amt_from_winedmo_format_video(mt, format, wm);

    case WINEDMO_MAJOR_TYPE_VIDEO_CINEPAK:
        return amt_from_winedmo_format_video_cinepak(mt, format);

    case WINEDMO_MAJOR_TYPE_VIDEO_WMV:
        return amt_from_winedmo_format_video_wmv(mt, format, wm);

    case WINEDMO_MAJOR_TYPE_VIDEO_MPEG1:
        return amt_from_winedmo_format_video_mpeg1(mt, format);
    }

    assert(0);
    return false;
}

static bool amt_to_winedmo_format_audio(const AM_MEDIA_TYPE *mt, struct winedmo_codec_format *format)
{
    static const struct
    {
        const GUID *subtype;
        WORD depth;
        enum winedmo_audio_format format;
    }
    format_map[] =
    {
        {&MEDIASUBTYPE_PCM,          8, WINEDMO_AUDIO_FORMAT_U8},
        {&MEDIASUBTYPE_PCM,         16, WINEDMO_AUDIO_FORMAT_S16LE},
        {&MEDIASUBTYPE_PCM,         24, WINEDMO_AUDIO_FORMAT_S24LE},
        {&MEDIASUBTYPE_PCM,         32, WINEDMO_AUDIO_FORMAT_S32LE},
        {&MEDIASUBTYPE_IEEE_FLOAT,  32, WINEDMO_AUDIO_FORMAT_F32LE},
        {&MEDIASUBTYPE_IEEE_FLOAT,  64, WINEDMO_AUDIO_FORMAT_F64LE},
    };

    const WAVEFORMATEX *audio_format = (const WAVEFORMATEX *)mt->pbFormat;
    unsigned int i;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx))
    {
        FIXME("Unknown format type %s.\n", debugstr_guid(&mt->formattype));
        return false;
    }
    if (mt->cbFormat < sizeof(WAVEFORMATEX) || !mt->pbFormat)
    {
        ERR("Unexpected format size %lu.\n", mt->cbFormat);
        return false;
    }

    format->major_type = WINEDMO_MAJOR_TYPE_AUDIO;
    format->u.audio.channels = audio_format->nChannels;
    format->u.audio.rate = audio_format->nSamplesPerSec;

    if (audio_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE *ext_format = (const WAVEFORMATEXTENSIBLE *)mt->pbFormat;

        format->u.audio.channel_mask = ext_format->dwChannelMask;
    }
    else
    {
        if (audio_format->nChannels == 1)
            format->u.audio.channel_mask = KSAUDIO_SPEAKER_MONO;
        else if (audio_format->nChannels == 2)
            format->u.audio.channel_mask = KSAUDIO_SPEAKER_STEREO;
        else
        {
            ERR("Unexpected channel count %u.\n", audio_format->nChannels);
            return false;
        }
    }

    for (i = 0; i < ARRAY_SIZE(format_map); ++i)
    {
        if (IsEqualGUID(&mt->subtype, format_map[i].subtype)
                && audio_format->wBitsPerSample == format_map[i].depth)
        {
            format->u.audio.format = format_map[i].format;
            return true;
        }
    }

    FIXME("Unknown subtype %s, depth %u.\n", debugstr_guid(&mt->subtype), audio_format->wBitsPerSample);
    return false;
}

static bool amt_to_winedmo_format_audio_mpeg1(const AM_MEDIA_TYPE *mt, struct winedmo_codec_format *format)
{
    const MPEG1WAVEFORMAT *audio_format = (const MPEG1WAVEFORMAT *)mt->pbFormat;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx))
    {
        FIXME("Unknown format type %s.\n", debugstr_guid(&mt->formattype));
        return false;
    }
    if (mt->cbFormat < sizeof(*audio_format) || !mt->pbFormat)
    {
        ERR("Unexpected format size %lu.\n", mt->cbFormat);
        return false;
    }

    format->major_type = WINEDMO_MAJOR_TYPE_AUDIO_MPEG1;
    format->u.audio.channels = audio_format->wfx.nChannels;
    format->u.audio.rate = audio_format->wfx.nSamplesPerSec;
    format->u.audio.layer = audio_format->fwHeadLayer;
    return true;
}

static bool amt_to_winedmo_format_audio_mpeg1_layer3(const AM_MEDIA_TYPE *mt, struct winedmo_codec_format *format)
{
    const MPEGLAYER3WAVEFORMAT *audio_format = (const MPEGLAYER3WAVEFORMAT *)mt->pbFormat;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx))
    {
        FIXME("Unknown format type %s.\n", debugstr_guid(&mt->formattype));
        return false;
    }
    if (mt->cbFormat < sizeof(*audio_format) || !mt->pbFormat)
    {
        ERR("Unexpected format size %lu.\n", mt->cbFormat);
        return false;
    }

    format->major_type = WINEDMO_MAJOR_TYPE_AUDIO_MPEG1;
    format->u.audio.channels = audio_format->wfx.nChannels;
    format->u.audio.rate = audio_format->wfx.nSamplesPerSec;
    format->u.audio.layer = 3;
    return true;
}

static bool amt_to_winedmo_format_audio_wma(const AM_MEDIA_TYPE *mt, struct winedmo_codec_format *format,
        bool is_xma)
{
    const WAVEFORMATEX *audio_format = (const WAVEFORMATEX *)mt->pbFormat;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx))
    {
        FIXME("Unknown format type %s.\n", debugstr_guid(&mt->formattype));
        return false;
    }
    if (mt->cbFormat < sizeof(*audio_format) || !mt->pbFormat)
    {
        ERR("Unexpected format size %lu.\n", mt->cbFormat);
        return false;
    }

    if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MSAUDIO1))
        format->u.audio.version = 1;
    else if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMAUDIO2))
        format->u.audio.version = 2;
    else if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMAUDIO3))
        format->u.audio.version = 3;
    else if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMAUDIO_LOSSLESS))
        format->u.audio.version = 4;
    else
        assert(false);
    format->major_type = WINEDMO_MAJOR_TYPE_AUDIO_WMA;
    format->u.audio.is_xma = is_xma;
    format->u.audio.bitrate = audio_format->nAvgBytesPerSec * 8;
    format->u.audio.rate = audio_format->nSamplesPerSec;
    format->u.audio.depth = audio_format->wBitsPerSample;
    format->u.audio.channels = audio_format->nChannels;
    format->u.audio.block_align = audio_format->nBlockAlign;

    format->u.audio.codec_data_len = 0;
    if (format->u.audio.version == 1)
        format->u.audio.codec_data_len = 4;
    if (format->u.audio.version == 2)
        format->u.audio.codec_data_len = 10;
    if (format->u.audio.version == 3)
        format->u.audio.codec_data_len = 18;
    if (format->u.audio.version == 4)
        format->u.audio.codec_data_len = 18;
    if (mt->cbFormat >= sizeof(WAVEFORMATEX) + format->u.audio.codec_data_len)
        memcpy(format->u.audio.codec_data, audio_format+1, format->u.audio.codec_data_len);
    else
        FIXME("Too small format block, can't copy codec data\n");

    return true;
}

static bool amt_to_winedmo_format_video(const AM_MEDIA_TYPE *mt, struct winedmo_codec_format *format)
{
    static const struct
    {
        const GUID *subtype;
        enum winedmo_video_format format;
    }
    format_map[] =
    {
        {&MEDIASUBTYPE_ARGB32,  WINEDMO_VIDEO_FORMAT_BGRA},
        {&MEDIASUBTYPE_RGB32,   WINEDMO_VIDEO_FORMAT_BGRx},
        {&MEDIASUBTYPE_RGB24,   WINEDMO_VIDEO_FORMAT_BGR},
        {&MEDIASUBTYPE_RGB555,  WINEDMO_VIDEO_FORMAT_RGB15},
        {&MEDIASUBTYPE_RGB565,  WINEDMO_VIDEO_FORMAT_RGB16},
        {&MEDIASUBTYPE_AYUV,    WINEDMO_VIDEO_FORMAT_AYUV},
        {&MEDIASUBTYPE_I420,    WINEDMO_VIDEO_FORMAT_I420},
        {&MEDIASUBTYPE_NV12,    WINEDMO_VIDEO_FORMAT_NV12},
        {&MEDIASUBTYPE_UYVY,    WINEDMO_VIDEO_FORMAT_UYVY},
        {&MEDIASUBTYPE_YUY2,    WINEDMO_VIDEO_FORMAT_YUY2},
        {&MEDIASUBTYPE_YV12,    WINEDMO_VIDEO_FORMAT_YV12},
        {&MEDIASUBTYPE_YVYU,    WINEDMO_VIDEO_FORMAT_YVYU},
    };

    const VIDEOINFOHEADER *video_format = (const VIDEOINFOHEADER *)mt->pbFormat;
    unsigned int i;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo))
    {
        FIXME("Unknown format type %s.\n", debugstr_guid(&mt->formattype));
        return false;
    }
    if (mt->cbFormat < sizeof(VIDEOINFOHEADER) || !mt->pbFormat)
    {
        ERR("Unexpected format size %lu.\n", mt->cbFormat);
        return false;
    }

    format->major_type = WINEDMO_MAJOR_TYPE_VIDEO;
    format->u.video.width = video_format->bmiHeader.biWidth;
    format->u.video.height = video_format->bmiHeader.biHeight;
    format->u.video.fps_n = 10000000;
    format->u.video.fps_d = video_format->AvgTimePerFrame;

    for (i = 0; i < ARRAY_SIZE(format_map); ++i)
    {
        if (IsEqualGUID(&mt->subtype, format_map[i].subtype))
        {
            format->u.video.format = format_map[i].format;
            if (winedmo_video_format_is_rgb(format->u.video.format))
                format->u.video.height = -format->u.video.height;
            return true;
        }
    }

    FIXME("Unknown subtype %s.\n", debugstr_guid(&mt->subtype));
    return false;
}

static bool amt_to_winedmo_format_video_cinepak(const AM_MEDIA_TYPE *mt, struct winedmo_codec_format *format)
{
    const VIDEOINFOHEADER *video_format = (const VIDEOINFOHEADER *)mt->pbFormat;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo))
    {
        FIXME("Unknown format type %s.\n", debugstr_guid(&mt->formattype));
        return false;
    }
    if (mt->cbFormat < sizeof(VIDEOINFOHEADER) || !mt->pbFormat)
    {
        ERR("Unexpected format size %lu.\n", mt->cbFormat);
        return false;
    }

    format->major_type = WINEDMO_MAJOR_TYPE_VIDEO_CINEPAK;
    format->u.video.width = video_format->bmiHeader.biWidth;
    format->u.video.height = video_format->bmiHeader.biHeight;
    format->u.video.fps_n = 10000000;
    format->u.video.fps_d = video_format->AvgTimePerFrame;

    return true;
}

static bool amt_to_winedmo_format_video_wmv(const AM_MEDIA_TYPE *mt, struct winedmo_codec_format *format)
{
    const VIDEOINFOHEADER *video_format = (const VIDEOINFOHEADER *)mt->pbFormat;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo))
    {
        FIXME("Unknown format type %s.\n", debugstr_guid(&mt->formattype));
        return false;
    }
    if (mt->cbFormat < sizeof(VIDEOINFOHEADER) || !mt->pbFormat)
    {
        ERR("Unexpected format size %lu.\n", mt->cbFormat);
        return false;
    }

    format->major_type = WINEDMO_MAJOR_TYPE_VIDEO_WMV;
    format->u.video.width = video_format->bmiHeader.biWidth;
    format->u.video.height = video_format->bmiHeader.biHeight;
    format->u.video.fps_n = 10000000;
    format->u.video.fps_d = video_format->AvgTimePerFrame;

    if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMV1))
        format->u.video.format = WINEDMO_VIDEO_FORMAT_WMV1;
    else if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMV2))
        format->u.video.format = WINEDMO_VIDEO_FORMAT_WMV2;
    else if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMV3))
        format->u.video.format = WINEDMO_VIDEO_FORMAT_WMV3;
    else if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMVA))
        format->u.video.format = WINEDMO_VIDEO_FORMAT_WMVA;
    else if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WVC1))
        format->u.video.format = WINEDMO_VIDEO_FORMAT_WVC1;
    else
        format->u.video.format = WINEDMO_VIDEO_FORMAT_UNKNOWN;

    format->u.video.codec_data_len = mt->cbFormat - sizeof(VIDEOINFOHEADER);
    if (format->u.video.codec_data_len > sizeof(format->u.video.codec_data))
    {
        ERR("Too big codec_data value (%u).\n", format->u.video.codec_data_len);
        format->u.video.codec_data_len = 0;
    }
    memcpy(format->u.video.codec_data, video_format+1, format->u.video.codec_data_len);
    return true;
}

static bool amt_to_winedmo_format_video_mpeg1(const AM_MEDIA_TYPE *mt, struct winedmo_codec_format *format)
{
    const VIDEOINFOHEADER *video_format = (const VIDEOINFOHEADER *)mt->pbFormat;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_MPEGVideo)
            && !IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo))
    {
        FIXME("Unknown format type %s.\n", debugstr_guid(&mt->formattype));
        return false;
    }
    if (mt->cbFormat < sizeof(VIDEOINFOHEADER) || !mt->pbFormat)
    {
        ERR("Unexpected format size %lu.\n", mt->cbFormat);
        return false;
    }

    format->major_type = WINEDMO_MAJOR_TYPE_VIDEO_MPEG1;
    format->u.video.width = video_format->bmiHeader.biWidth;
    format->u.video.height = video_format->bmiHeader.biHeight;
    format->u.video.fps_n = 10000000;
    format->u.video.fps_d = video_format->AvgTimePerFrame;

    return true;
}

bool amt_to_winedmo_format(const AM_MEDIA_TYPE *mt, struct winedmo_codec_format *format)
{
    memset(format, 0, sizeof(*format));

    if (IsEqualGUID(&mt->majortype, &MEDIATYPE_Video))
    {
        if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_CVID))
            return amt_to_winedmo_format_video_cinepak(mt, format);
        if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMV1)
                || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMV2)
                || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMVA)
                || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMVP)
                || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WVP2)
                || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMV_Unknown)
                || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WVC1)
                || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMV3)
                || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_VC1S))
            return amt_to_winedmo_format_video_wmv(mt, format);
        if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1Payload))
            return amt_to_winedmo_format_video_mpeg1(mt, format);
        return amt_to_winedmo_format_video(mt, format);
    }
    if (IsEqualGUID(&mt->majortype, &MEDIATYPE_Audio))
    {
        if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1AudioPayload))
            return amt_to_winedmo_format_audio_mpeg1(mt, format);
        if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MP3))
            return amt_to_winedmo_format_audio_mpeg1_layer3(mt, format);
        if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MSAUDIO1)
                || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMAUDIO2)
                || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMAUDIO3)
                || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WMAUDIO_LOSSLESS))
            return amt_to_winedmo_format_audio_wma(mt, format, false);
        if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_XMAUDIO2))
            return amt_to_winedmo_format_audio_wma(mt, format, true);
        return amt_to_winedmo_format_audio(mt, format);
    }

    FIXME("Unknown major type %s.\n", debugstr_guid(&mt->majortype));
    return false;
}

/* avi_splitter_create, wave_parser_create, mpeg_splitter_create, decodebin_parser_create are in winedmo_quartz_parser.c */
