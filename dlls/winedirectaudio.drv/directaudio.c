/*
 * Unixlib for the DirectAudio driver (native Wine -> Android AAudio).
 *
 * mmdevapi owns IAudioClient/IAudioRenderClient; this backend only implements
 * the mmdevapi driver unixlib vtable (see ../mmdevapi/unixlib.h) against AAudio,
 * with no PulseAudio daemon and no ALSA aserver in the path.
 *
 * Structure and the mmdevapi ring-buffer bookkeeping are modelled on
 * winecoreaudio.drv (single native backend, render callback ~ AAudio callback);
 * the CoreAudio device layer is replaced with AAudio.
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

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <aaudio/AAudio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "winternl.h"
#include "mmdeviceapi.h"
#include "initguid.h"
#include "audioclient.h"
#include "wine/debug.h"
#include "wine/unixlib.h"

#include "../mmdevapi/unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(directaudio);

struct directaudio_stream
{
    pthread_mutex_t lock;
    AAudioStream *aq;             /* current AAudio output stream */

    EDataFlow flow;
    DWORD flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;

    BOOL playing, please_quit;
    REFERENCE_TIME period;
    UINT32 period_frames;
    UINT32 bufsize_frames;
    UINT32 lcl_offs_frames, held_frames, wri_offs_frames, tmp_buffer_frames;
    UINT64 written_frames;
    INT32 getbuf_last;
    WAVEFORMATEX *fmt;
    BYTE *local_buffer, *tmp_buffer;

    /* AAudio open parameters, kept so the stream can be re-opened on a route
     * change (headphone/BT/HDMI) without losing the guest's negotiated format. */
    aaudio_format_t aa_format;
    int32_t aa_channels, aa_rate;
    aaudio_performance_mode_t aa_perf;

    /* adaptive buffer control (ported from the ALSA/PA adaptive stacks) */
    BOOL adaptive;
    int32_t target_buf_frames;   /* initial buffer target, 0 = leave default */
    int32_t max_buf_frames;      /* cap for adaptive growth, 0 = capacity */
    int32_t last_xrun;

    /* software gain (AAudio NDK has no per-stream volume) */
    float vols[8];
    BOOL vols_active;

    /* route-follow reopen guard */
    int need_reopen;

    /* measurement */
    unsigned int cb_count;
};

static const REFERENCE_TIME def_period = 100000;
static const REFERENCE_TIME min_period = 50000;

static ULONG_PTR zero_bits = 0;

static NTSTATUS unix_not_implemented(void *args)
{
    return STATUS_SUCCESS;
}

static struct directaudio_stream *handle_get_stream(stream_handle h)
{
    return (struct directaudio_stream *)(UINT_PTR)h;
}

/* copied from kernelbase */
static int muldiv(int a, int b, int c)
{
    LONGLONG ret;

    if (!c) return -1;

    if (c < 0)
    {
        a = -a;
        c = -c;
    }

    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        ret = (((LONGLONG)a * b) + (c / 2)) / c;
    else
        ret = (((LONGLONG)a * b) - (c / 2)) / c;

    if (ret > 2147483647 || ret < -2147483647) return -1;
    return ret;
}

static WAVEFORMATEX *clone_format(const WAVEFORMATEX *fmt)
{
    WAVEFORMATEX *ret;
    size_t size;

    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        size = sizeof(WAVEFORMATEXTENSIBLE);
    else
        size = sizeof(WAVEFORMATEX);

    ret = malloc(size);
    if (!ret) return NULL;

    memcpy(ret, fmt, size);
    ret->cbSize = size - sizeof(WAVEFORMATEX);
    return ret;
}

static void silence_buffer(struct directaudio_stream *stream, BYTE *buffer, UINT32 frames)
{
    WAVEFORMATEXTENSIBLE *fmtex = (WAVEFORMATEXTENSIBLE *)stream->fmt;
    if ((stream->fmt->wFormatTag == WAVE_FORMAT_PCM ||
         (stream->fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
          IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))) &&
        stream->fmt->wBitsPerSample == 8)
        memset(buffer, 128, frames * stream->fmt->nBlockAlign);
    else
        memset(buffer, 0, frames * stream->fmt->nBlockAlign);
}

static aaudio_format_t fmt_to_aaudio(const WAVEFORMATEX *fmt)
{
    const WAVEFORMATEXTENSIBLE *fex = (const WAVEFORMATEXTENSIBLE *)fmt;
    BOOL is_float = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         IsEqualGUID(&fex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    BOOL is_pcm = fmt->wFormatTag == WAVE_FORMAT_PCM ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         IsEqualGUID(&fex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM));

    if (is_float && fmt->wBitsPerSample == 32) return AAUDIO_FORMAT_PCM_FLOAT;
    if (is_pcm && fmt->wBitsPerSample == 16) return AAUDIO_FORMAT_PCM_I16;
    if (is_pcm && fmt->wBitsPerSample == 32) return AAUDIO_FORMAT_PCM_I32;
    return AAUDIO_FORMAT_UNSPECIFIED;
}

static void apply_gains(struct directaudio_stream *stream, void *data, int32_t frames)
{
    UINT32 ch = stream->fmt->nChannels, i;
    int32_t f;

    if (stream->aa_format == AAUDIO_FORMAT_PCM_FLOAT)
    {
        float *p = data;
        for (f = 0; f < frames; f++)
            for (i = 0; i < ch; i++)
                *p++ *= stream->vols[i < 8 ? i : 7];
    }
    else if (stream->aa_format == AAUDIO_FORMAT_PCM_I16)
    {
        INT16 *p = data;
        for (f = 0; f < frames; f++)
            for (i = 0; i < ch; i++)
            {
                int v = (int)(*p * stream->vols[i < 8 ? i : 7]);
                if (v > 32767) v = 32767;
                else if (v < -32768) v = -32768;
                *p++ = (INT16)v;
            }
    }
}

/* AAudio pulls data from us on its high-priority audio thread. */
static aaudio_data_callback_result_t aaudio_data_cb(AAudioStream *aq, void *user,
                                                    void *audioData, int32_t numFrames)
{
    struct directaudio_stream *stream = user;
    UINT32 to_copy_bytes, to_copy_frames, chunk_bytes, lcl_offs_bytes;

    pthread_mutex_lock(&stream->lock);

    if (stream->playing)
    {
        lcl_offs_bytes = stream->lcl_offs_frames * stream->fmt->nBlockAlign;
        to_copy_frames = min((UINT32)numFrames, stream->held_frames);
        to_copy_bytes = to_copy_frames * stream->fmt->nBlockAlign;

        chunk_bytes = (stream->bufsize_frames - stream->lcl_offs_frames) * stream->fmt->nBlockAlign;

        if (to_copy_bytes > chunk_bytes)
        {
            memcpy(audioData, stream->local_buffer + lcl_offs_bytes, chunk_bytes);
            memcpy((BYTE *)audioData + chunk_bytes, stream->local_buffer, to_copy_bytes - chunk_bytes);
        }
        else
            memcpy(audioData, stream->local_buffer + lcl_offs_bytes, to_copy_bytes);

        stream->lcl_offs_frames += to_copy_frames;
        stream->lcl_offs_frames %= stream->bufsize_frames;
        stream->held_frames -= to_copy_frames;
    }
    else
        to_copy_bytes = to_copy_frames = 0;

    if ((UINT32)numFrames > to_copy_frames)
        silence_buffer(stream, (BYTE *)audioData + to_copy_bytes, numFrames - to_copy_frames);

    if (stream->vols_active)
        apply_gains(stream, audioData, numFrames);

    /* Adaptive: grow the device buffer by a burst whenever the xrun count
     * climbs, capped at max_buf_frames (or capacity). Cheap and callback-safe. */
    if (stream->adaptive)
    {
        int32_t xruns = AAudioStream_getXRunCount(aq);
        if (xruns > stream->last_xrun)
        {
            int32_t burst = AAudioStream_getFramesPerBurst(aq);
            int32_t cur = AAudioStream_getBufferSizeInFrames(aq);
            int32_t cap = AAudioStream_getBufferCapacityInFrames(aq);
            int32_t want = cur + (burst > 0 ? burst : 1);

            if (stream->max_buf_frames > 0 && want > stream->max_buf_frames)
                want = stream->max_buf_frames;
            if (want > cap) want = cap;
            if (want > cur)
            {
                AAudioStream_setBufferSizeInFrames(aq, want);
                TRACE("grow: xruns %d->%d buf %d->%d cap %d\n",
                      stream->last_xrun, xruns, cur, want, cap);
            }
            stream->last_xrun = xruns;
        }
    }

    if (TRACE_ON(directaudio) && !(++stream->cb_count % 1000))
        TRACE("hb: cb=%u held=%u buf=%d cap=%d xruns=%d\n", stream->cb_count,
              stream->held_frames, AAudioStream_getBufferSizeInFrames(aq),
              AAudioStream_getBufferCapacityInFrames(aq), AAudioStream_getXRunCount(aq));

    pthread_mutex_unlock(&stream->lock);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void *reopen_thread(void *user);

static void aaudio_error_cb(AAudioStream *aq, void *user, aaudio_result_t error)
{
    struct directaudio_stream *stream = user;
    int expected = 0;

    if (error != AAUDIO_ERROR_DISCONNECTED)
        return;

    /* Route change (headphone/BT/HDMI). AAudio requires the reopen to happen on
     * another thread, never inside this callback. */
    if (__atomic_compare_exchange_n(&stream->need_reopen, &expected, 1, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        pthread_t th;
        WARN("route change (err=%d) - reopening AAudio stream\n", error);
        if (pthread_create(&th, NULL, reopen_thread, stream))
            __atomic_store_n(&stream->need_reopen, 0, __ATOMIC_SEQ_CST);
        else
            pthread_detach(th);
    }
}

static aaudio_result_t open_aaudio(struct directaudio_stream *stream, AAudioStream **out)
{
    AAudioStreamBuilder *builder = NULL;
    AAudioStream *aq = NULL;
    aaudio_result_t r;

    r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK || !builder)
        return r != AAUDIO_OK ? r : AAUDIO_ERROR_NO_MEMORY;

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, stream->aa_perf);
    AAudioStreamBuilder_setFormat(builder, stream->aa_format);
    AAudioStreamBuilder_setChannelCount(builder, stream->aa_channels);
    AAudioStreamBuilder_setSampleRate(builder, stream->aa_rate);
    AAudioStreamBuilder_setDataCallback(builder, aaudio_data_cb, stream);
    AAudioStreamBuilder_setErrorCallback(builder, aaudio_error_cb, stream);

    r = AAudioStreamBuilder_openStream(builder, &aq);
    AAudioStreamBuilder_delete(builder);
    if (r != AAUDIO_OK || !aq)
        return r != AAUDIO_OK ? r : AAUDIO_ERROR_INTERNAL;

    if (stream->target_buf_frames > 0)
    {
        int32_t cap = AAudioStream_getBufferCapacityInFrames(aq);
        int32_t want = stream->target_buf_frames;
        if (want > cap) want = cap;
        AAudioStream_setBufferSizeInFrames(aq, want);
    }
    stream->last_xrun = AAudioStream_getXRunCount(aq);

    r = AAudioStream_requestStart(aq);
    if (r != AAUDIO_OK)
    {
        AAudioStream_close(aq);
        return r;
    }

    TRACE("open: fmt=%d ch=%d rate=%d perf=%d burst=%d buf=%d cap=%d\n",
          stream->aa_format, stream->aa_channels, stream->aa_rate, stream->aa_perf,
          AAudioStream_getFramesPerBurst(aq), AAudioStream_getBufferSizeInFrames(aq),
          AAudioStream_getBufferCapacityInFrames(aq));

    *out = aq;
    return AAUDIO_OK;
}

static void *reopen_thread(void *user)
{
    struct directaudio_stream *stream = user;
    AAudioStream *old = NULL, *neu = NULL;

    if (open_aaudio(stream, &neu) == AAUDIO_OK)
    {
        pthread_mutex_lock(&stream->lock);
        old = stream->aq;
        stream->aq = neu;
        pthread_mutex_unlock(&stream->lock);
    }
    else
        WARN("route-change reopen failed; keeping old stream\n");

    /* close() blocks until the old stream's in-flight callback returns; do it
     * outside the lock so the callback can drain. */
    if (old)
    {
        AAudioStream_requestStop(old);
        AAudioStream_close(old);
        TRACE("reopened AAudio stream on route change\n");
    }

    __atomic_store_n(&stream->need_reopen, 0, __ATOMIC_SEQ_CST);
    return NULL;
}

static void read_config_from_env(struct directaudio_stream *stream)
{
    const char *e;

    /* Engine-scoped keys per the app's NO-BLEED audio contract: the DirectAudio
     * engine tag is DIRECT, so config arrives as BANNER_AUDIO_DIRECT_* in the
     * container/shortcut env (perf is 0=NONE, 1=LOW_LATENCY, 2=POWER_SAVING).
     * NONE keeps AAudio capacity large enough to honour big guest buffers,
     * matching the device-proven ALSA/PA adaptive result. */
    stream->aa_perf = AAUDIO_PERFORMANCE_MODE_NONE;
    stream->adaptive = TRUE;
    stream->target_buf_frames = 0;
    stream->max_buf_frames = 0;

    if ((e = getenv("BANNER_AUDIO_DIRECT_PERF")))
    {
        int v = atoi(e);
        if (v == 1) stream->aa_perf = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
        else if (v == 2) stream->aa_perf = AAUDIO_PERFORMANCE_MODE_POWER_SAVING;
        else stream->aa_perf = AAUDIO_PERFORMANCE_MODE_NONE;
    }
    if ((e = getenv("BANNER_AUDIO_DIRECT_ADAPTIVE"))) stream->adaptive = atoi(e) != 0;
    if ((e = getenv("BANNER_AUDIO_DIRECT_BF"))) stream->target_buf_frames = atoi(e);
    if ((e = getenv("BANNER_AUDIO_DIRECT_MBF"))) stream->max_buf_frames = atoi(e);
}

static NTSTATUS unix_process_attach(void *args)
{
#ifdef _WIN64
    if (NtCurrentTeb()->WowTebOffset)
    {
        SYSTEM_BASIC_INFORMATION info;

        NtQuerySystemInformation(SystemEmulationBasicInformation, &info, sizeof(info), NULL);
        zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
    }
#endif
    return STATUS_SUCCESS;
}

static NTSTATUS unix_main_loop(void *args)
{
    struct main_loop_params *params = args;
    NtSetEvent(params->event, NULL);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_test_connect(void *args)
{
    struct test_connect_params *params = args;
    AAudioStreamBuilder *builder = NULL;

    if (AAudio_createStreamBuilder(&builder) == AAUDIO_OK && builder)
    {
        AAudioStreamBuilder_delete(builder);
        params->priority = Priority_Preferred;
    }
    else
        params->priority = Priority_Unavailable;

    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_endpoint_ids(void *args)
{
    static const WCHAR ep_name[] = {'D','i','r','e','c','t','A','u','d','i','o',0};
    static const char ep_dev[] = "aaudio";
    struct get_endpoint_ids_params *params = args;
    unsigned int name_bytes = sizeof(ep_name);
    unsigned int dev_bytes = sizeof(ep_dev);
    unsigned int needed, offset;
    struct endpoint *endpoint = params->endpoints;

    params->default_idx = 0;
    params->num = (params->flow == eRender) ? 1 : 0; /* capture not yet supported */

    TRACE("get_endpoint_ids: flow=%d -> num=%u\n", params->flow, params->num);

    if (params->num == 0)
    {
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    offset = needed = sizeof(*endpoint) * params->num;
    needed += name_bytes + ((dev_bytes + 1) & ~1);

    if (needed <= params->size)
    {
        endpoint->name = offset;
        memcpy((char *)params->endpoints + offset, ep_name, name_bytes);
        offset += name_bytes;

        endpoint->device = offset;
        memcpy((char *)params->endpoints + offset, ep_dev, dev_bytes);

        params->result = S_OK;
    }
    else
    {
        params->size = needed;
        params->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS unix_create_stream(void *args)
{
    struct create_stream_params *params = args;
    struct directaudio_stream *stream;
    aaudio_result_t r;
    SIZE_T size;
    int i;

    TRACE("create_stream: flow=%d share=%d flags=%#x tag=%#x ch=%u rate=%u bits=%u\n",
          params->flow, params->share, (unsigned)params->flags, params->fmt->wFormatTag,
          params->fmt->nChannels, (unsigned)params->fmt->nSamplesPerSec, params->fmt->wBitsPerSample);

    params->result = S_OK;

    if (!(stream = calloc(1, sizeof(*stream))))
    {
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }
    pthread_mutex_init(&stream->lock, NULL);
    for (i = 0; i < 8; i++) stream->vols[i] = 1.0f;

    stream->fmt = clone_format(params->fmt);
    if (!stream->fmt)
    {
        params->result = E_OUTOFMEMORY;
        goto end;
    }

    stream->period = params->period;
    stream->period_frames = muldiv(params->period, stream->fmt->nSamplesPerSec, 10000000);
    stream->flow = params->flow;
    stream->flags = params->flags;
    stream->share = params->share;

    if (stream->flow != eRender)
    {
        /* capture is a later phase */
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        goto end;
    }

    stream->aa_format = fmt_to_aaudio(stream->fmt);
    if (stream->aa_format == AAUDIO_FORMAT_UNSPECIFIED)
    {
        params->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
        goto end;
    }
    stream->aa_channels = stream->fmt->nChannels;
    stream->aa_rate = stream->fmt->nSamplesPerSec;

    read_config_from_env(stream);

    stream->bufsize_frames = muldiv(params->duration, stream->fmt->nSamplesPerSec, 10000000);
    if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
        stream->bufsize_frames -= stream->bufsize_frames % stream->period_frames;

    size = stream->bufsize_frames * stream->fmt->nBlockAlign;
    if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, zero_bits,
                                &size, MEM_COMMIT, PAGE_READWRITE))
    {
        params->result = E_OUTOFMEMORY;
        goto end;
    }
    silence_buffer(stream, stream->local_buffer, stream->bufsize_frames);

    /* We play continuously; stream->playing gates real audio vs. silence. */
    r = open_aaudio(stream, &stream->aq);
    if (r != AAUDIO_OK)
    {
        WARN("AAudio open failed: %d\n", r);
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        goto end;
    }
    params->result = S_OK;

end:
    if (FAILED(params->result))
    {
        if (stream->aq)
        {
            AAudioStream_requestStop(stream->aq);
            AAudioStream_close(stream->aq);
        }
        if (stream->local_buffer)
        {
            size = 0;
            NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer,
                                &size, MEM_RELEASE);
        }
        free(stream->fmt);
        pthread_mutex_destroy(&stream->lock);
        free(stream);
    }
    else
    {
        *params->channel_count = params->fmt->nChannels;
        *params->stream = (stream_handle)(UINT_PTR)stream;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS unix_release_stream(void *args)
{
    struct release_stream_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    SIZE_T size;

    if (params->timer_thread)
    {
        stream->please_quit = TRUE;
        NtWaitForSingleObject(params->timer_thread, FALSE, NULL);
        NtClose(params->timer_thread);
    }

    if (stream->aq)
    {
        AAudioStream_requestStop(stream->aq);
        AAudioStream_close(stream->aq);
    }

    if (stream->local_buffer)
    {
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer,
                            &size, MEM_RELEASE);
    }
    if (stream->tmp_buffer)
    {
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer,
                            &size, MEM_RELEASE);
    }
    free(stream->fmt);
    pthread_mutex_destroy(&stream->lock);
    free(stream);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static DWORD get_channel_mask(unsigned int channels)
{
    switch (channels)
    {
    case 0:  return 0;
    case 1:  return KSAUDIO_SPEAKER_MONO;
    case 2:  return KSAUDIO_SPEAKER_STEREO;
    case 3:  return KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY;
    case 4:  return KSAUDIO_SPEAKER_QUAD;
    case 5:  return KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY;
    case 6:  return KSAUDIO_SPEAKER_5POINT1;
    case 7:  return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
    case 8:  return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    }
    return 0;
}

static NTSTATUS unix_get_mix_format(void *args)
{
    struct get_mix_format_params *params = args;

    /* AAudio has no NDK device enumeration before API 34; advertise a standard
     * AAudio-friendly shared mix format. mmdevapi resamples the guest to this. */
    params->fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    params->fmt->Format.nChannels = 2;
    params->fmt->Format.nSamplesPerSec = 48000;
    params->fmt->Format.wBitsPerSample = 32;
    params->fmt->dwChannelMask = get_channel_mask(2);
    params->fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    params->fmt->Format.nBlockAlign = params->fmt->Format.wBitsPerSample *
        params->fmt->Format.nChannels / 8;
    params->fmt->Format.nAvgBytesPerSec = params->fmt->Format.nSamplesPerSec *
        params->fmt->Format.nBlockAlign;
    params->fmt->Samples.wValidBitsPerSample = params->fmt->Format.wBitsPerSample;
    params->fmt->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    TRACE("get_mix_format: flow=%d -> 48000/32f/2ch\n", params->flow);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_is_format_supported(void *args)
{
    struct is_format_supported_params *params = args;
    const WAVEFORMATEX *fmt = params->fmt_in;

    if (!fmt || (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                 fmt->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)))
    {
        params->result = E_INVALIDARG;
        return STATUS_SUCCESS;
    }

    if (params->flow != eRender)
    {
        params->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
        return STATUS_SUCCESS;
    }

    if (fmt_to_aaudio(fmt) == AAUDIO_FORMAT_UNSPECIFIED ||
        fmt->nChannels < 1 || fmt->nChannels > 8 ||
        fmt->nSamplesPerSec < 8000 || fmt->nSamplesPerSec > 192000)
        params->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
    else
        params->result = S_OK;

    TRACE("is_format_supported: share=%d tag=%#x ch=%u rate=%u bits=%u aafmt=%d -> %#x\n",
          params->share, fmt->wFormatTag, fmt->nChannels, (unsigned)fmt->nSamplesPerSec,
          fmt->wBitsPerSample, fmt_to_aaudio(fmt), (unsigned)params->result);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_device_period(void *args)
{
    struct get_device_period_params *params = args;

    if (params->def_period) *params->def_period = def_period;
    if (params->min_period) *params->min_period = min_period;
    TRACE("get_device_period: flow=%d def=%d min=%d\n", params->flow, (int)def_period, (int)min_period);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_buffer_size(void *args)
{
    struct get_buffer_size_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);
    *params->frames = stream->bufsize_frames;
    pthread_mutex_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_latency(void *args)
{
    struct get_latency_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    int32_t buf_frames;

    pthread_mutex_lock(&stream->lock);
    buf_frames = stream->aq ? AAudioStream_getBufferSizeInFrames(stream->aq) : 0;
    if (buf_frames < 0) buf_frames = 0;
    /* pretend we process audio in Period chunks, so max latency includes it */
    *params->latency = muldiv(buf_frames, 10000000, stream->fmt->nSamplesPerSec) + stream->period;
    pthread_mutex_unlock(&stream->lock);

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static UINT32 get_current_padding_nolock(struct directaudio_stream *stream)
{
    return stream->held_frames;
}

static NTSTATUS unix_get_current_padding(void *args)
{
    struct get_current_padding_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);
    *params->padding = get_current_padding_nolock(stream);
    pthread_mutex_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_start(void *args)
{
    struct start_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);

    if ((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
        params->result = AUDCLNT_E_EVENTHANDLE_NOT_SET;
    else if (stream->playing)
        params->result = AUDCLNT_E_NOT_STOPPED;
    else
    {
        stream->playing = TRUE;
        params->result = S_OK;
    }

    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_stop(void *args)
{
    struct stop_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);

    if (!stream->playing)
        params->result = S_FALSE;
    else
    {
        stream->playing = FALSE;
        params->result = S_OK;
    }

    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_reset(void *args)
{
    struct reset_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);

    if (stream->playing)
        params->result = AUDCLNT_E_NOT_STOPPED;
    else if (stream->getbuf_last)
        params->result = AUDCLNT_E_BUFFER_OPERATION_PENDING;
    else
    {
        stream->written_frames = 0;
        stream->held_frames = 0;
        stream->lcl_offs_frames = 0;
        stream->wri_offs_frames = 0;
        params->result = S_OK;
    }

    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_timer_loop(void *args)
{
    struct timer_loop_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    LARGE_INTEGER delay, next, last;
    int adjust;

    delay.QuadPart = -stream->period;
    NtQueryPerformanceCounter(&last, NULL);
    next.QuadPart = last.QuadPart + stream->period;

    while (!stream->please_quit)
    {
        if (stream->event)
            NtSetEvent(stream->event, NULL);
        NtDelayExecution(FALSE, &delay);
        NtQueryPerformanceCounter(&last, NULL);

        adjust = next.QuadPart - last.QuadPart;
        if (adjust > stream->period / 2)
            adjust = stream->period / 2;
        else if (adjust < -stream->period / 2)
            adjust = -stream->period / 2;

        delay.QuadPart = -(stream->period + adjust);
        next.QuadPart += stream->period;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_render_buffer(void *args)
{
    struct get_render_buffer_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    SIZE_T size;
    UINT32 pad;

    pthread_mutex_lock(&stream->lock);

    pad = get_current_padding_nolock(stream);

    if (stream->getbuf_last)
    {
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        goto end;
    }
    if (!params->frames)
    {
        params->result = S_OK;
        goto end;
    }
    if (pad + params->frames > stream->bufsize_frames)
    {
        params->result = AUDCLNT_E_BUFFER_TOO_LARGE;
        goto end;
    }

    if (stream->wri_offs_frames + params->frames > stream->bufsize_frames)
    {
        if (stream->tmp_buffer_frames < params->frames)
        {
            if (stream->tmp_buffer)
            {
                size = 0;
                NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer,
                                    &size, MEM_RELEASE);
                stream->tmp_buffer = NULL;
            }
            size = params->frames * stream->fmt->nBlockAlign;
            if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, zero_bits,
                                        &size, MEM_COMMIT, PAGE_READWRITE))
            {
                stream->tmp_buffer_frames = 0;
                params->result = E_OUTOFMEMORY;
                goto end;
            }
            stream->tmp_buffer_frames = params->frames;
        }
        *params->data = stream->tmp_buffer;
        stream->getbuf_last = -params->frames;
    }
    else
    {
        *params->data = stream->local_buffer + stream->wri_offs_frames * stream->fmt->nBlockAlign;
        stream->getbuf_last = params->frames;
    }

    silence_buffer(stream, *params->data, params->frames);
    params->result = S_OK;

end:
    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static void wrap_buffer(BYTE *dst, UINT32 dst_offs, UINT32 dst_bytes, BYTE *src, UINT32 src_bytes)
{
    UINT32 chunk_bytes = dst_bytes - dst_offs;

    if (chunk_bytes < src_bytes)
    {
        memcpy(dst + dst_offs, src, chunk_bytes);
        memcpy(dst, src + chunk_bytes, src_bytes - chunk_bytes);
    }
    else
        memcpy(dst + dst_offs, src, src_bytes);
}

static NTSTATUS unix_release_render_buffer(void *args)
{
    struct release_render_buffer_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    BYTE *buffer;

    pthread_mutex_lock(&stream->lock);

    if (!params->written_frames)
    {
        stream->getbuf_last = 0;
        params->result = S_OK;
    }
    else if (!stream->getbuf_last)
        params->result = AUDCLNT_E_OUT_OF_ORDER;
    else if (params->written_frames > (stream->getbuf_last >= 0 ? stream->getbuf_last : -stream->getbuf_last))
        params->result = AUDCLNT_E_INVALID_SIZE;
    else
    {
        if (stream->getbuf_last >= 0)
            buffer = stream->local_buffer + stream->wri_offs_frames * stream->fmt->nBlockAlign;
        else
            buffer = stream->tmp_buffer;

        if (params->flags & AUDCLNT_BUFFERFLAGS_SILENT)
            silence_buffer(stream, buffer, params->written_frames);

        if (stream->getbuf_last < 0)
            wrap_buffer(stream->local_buffer,
                        stream->wri_offs_frames * stream->fmt->nBlockAlign,
                        stream->bufsize_frames * stream->fmt->nBlockAlign,
                        buffer, params->written_frames * stream->fmt->nBlockAlign);

        stream->wri_offs_frames += params->written_frames;
        stream->wri_offs_frames %= stream->bufsize_frames;
        stream->held_frames += params->written_frames;
        stream->written_frames += params->written_frames;
        stream->getbuf_last = 0;

        params->result = S_OK;
    }

    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_capture_buffer(void *args)
{
    struct get_capture_buffer_params *params = args;
    *params->frames = 0;
    params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_release_capture_buffer(void *args)
{
    struct release_capture_buffer_params *params = args;
    params->result = params->done ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_next_packet_size(void *args)
{
    struct get_next_packet_size_params *params = args;
    *params->frames = 0;
    params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_position(void *args)
{
    struct get_position_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    LARGE_INTEGER stamp, freq;

    if (params->device)
    {
        params->result = E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    pthread_mutex_lock(&stream->lock);

    *params->pos = stream->written_frames - stream->held_frames;
    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->pos *= stream->fmt->nBlockAlign;

    if (params->qpctime)
    {
        NtQueryPerformanceCounter(&stamp, &freq);
        *params->qpctime = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    pthread_mutex_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_frequency(void *args)
{
    struct get_frequency_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->freq = (UINT64)stream->fmt->nSamplesPerSec * stream->fmt->nBlockAlign;
    else
        *params->freq = stream->fmt->nSamplesPerSec;

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_is_started(void *args)
{
    struct is_started_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    params->result = stream->playing ? S_OK : S_FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_prop_value(void *args)
{
    /* PKEY_AudioEndpoint_* live under this fmtid; {...},3 = PhysicalSpeakers. */
    static const GUID PKEY_AudioEndpoint_GUID = {
        0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}
    };
    static const PROPERTYKEY devicepath_key = {
        {0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 2
    };
    struct get_prop_value_params *params = args;

    TRACE("get_prop_value: flow=%d prop=%s,%u\n", params->flow,
          wine_dbgstr_guid(&params->prop->fmtid), params->prop->pid);

    /* Games (e.g. DiRT 3) refuse to call IAudioClient::Initialize until they can
     * read PhysicalSpeakers from the endpoint, so (like winealsa/winepulse) we
     * must supply it (winecoreaudio's E_NOTIMPL stub hangs those titles here). */
    if (params->flow == eRender &&
        IsEqualGUID(&params->prop->fmtid, &PKEY_AudioEndpoint_GUID) &&
        params->prop->pid == 3)
    {
        params->value->vt = VT_UI4;
        params->value->ulVal = KSAUDIO_SPEAKER_STEREO; /* our AAudio endpoint is stereo */
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    if (IsEqualPropertyKey(*params->prop, devicepath_key))
    {
        static const WCHAR path[] =
            {'{','1','}','.','R','O','O','T','\\','M','E','D','I','A','\\','0','0','0','0',0};
        UINT len = ARRAYSIZE(path);

        if (*params->buffer_size < len * sizeof(WCHAR))
        {
            *params->buffer_size = len * sizeof(WCHAR);
            params->result = E_NOT_SUFFICIENT_BUFFER;
            return STATUS_SUCCESS;
        }
        params->value->vt = VT_LPWSTR;
        params->value->pwszVal = params->buffer;
        memcpy(params->buffer, path, len * sizeof(WCHAR));
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    params->result = E_NOTIMPL;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_set_volumes(void *args)
{
    struct set_volumes_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    BOOL active = FALSE;
    UINT32 i;

    pthread_mutex_lock(&stream->lock);
    for (i = 0; i < stream->fmt->nChannels && i < 8; i++)
    {
        stream->vols[i] = params->master_volume *
            params->session_volumes[i] * params->volumes[i];
        if (stream->vols[i] < 0.999f || stream->vols[i] > 1.001f) active = TRUE;
    }
    stream->vols_active = active;
    pthread_mutex_unlock(&stream->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS unix_set_event_handle(void *args)
{
    struct set_event_handle_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    HRESULT hr = S_OK;

    pthread_mutex_lock(&stream->lock);
    if (!stream->aq)
        hr = AUDCLNT_E_DEVICE_INVALIDATED;
    else if (!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        hr = AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
    else if (stream->event)
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    else
        stream->event = params->event;
    pthread_mutex_unlock(&stream->lock);

    params->result = hr;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    unix_process_attach,
    unix_not_implemented,        /* process_detach */
    unix_main_loop,
    unix_get_endpoint_ids,
    unix_create_stream,
    unix_release_stream,
    unix_start,
    unix_stop,
    unix_reset,
    unix_timer_loop,
    unix_get_render_buffer,
    unix_release_render_buffer,
    unix_get_capture_buffer,
    unix_release_capture_buffer,
    unix_is_format_supported,
    unix_not_implemented,        /* get_loopback_capture_device */
    unix_get_mix_format,
    unix_get_device_period,
    unix_get_buffer_size,
    unix_get_latency,
    unix_get_current_padding,
    unix_get_next_packet_size,
    unix_get_frequency,
    unix_get_position,
    unix_set_volumes,
    unix_set_event_handle,
    unix_not_implemented,        /* set_sample_rate */
    unix_test_connect,
    unix_is_started,
    unix_get_prop_value,
    unix_not_implemented,        /* midi_get_driver */
    unix_not_implemented,        /* midi_init */
    unix_not_implemented,        /* midi_release */
    unix_not_implemented,        /* midi_out_message */
    unix_not_implemented,        /* midi_in_message */
    unix_not_implemented,        /* midi_notify_wait */
    unix_not_implemented,        /* aux_message */
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == funcs_count);

#ifdef _WIN64

typedef UINT PTR32;

static NTSTATUS unix_wow64_main_loop(void *args)
{
    struct
    {
        PTR32 event;
    } *params32 = args;
    struct main_loop_params params =
    {
        .event = ULongToHandle(params32->event)
    };
    return unix_main_loop(&params);
}

static NTSTATUS unix_wow64_test_connect(void *args)
{
    struct
    {
        PTR32 name;
        enum driver_priority priority;
    } *params32 = args;
    struct test_connect_params params =
    {
        .name = ULongToPtr(params32->name)
    };
    unix_test_connect(&params);
    params32->priority = params.priority;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_endpoint_ids(void *args)
{
    struct
    {
        EDataFlow flow;
        PTR32 endpoints;
        unsigned int size;
        HRESULT result;
        unsigned int num;
        unsigned int default_idx;
    } *params32 = args;
    struct get_endpoint_ids_params params =
    {
        .flow = params32->flow,
        .endpoints = ULongToPtr(params32->endpoints),
        .size = params32->size
    };
    unix_get_endpoint_ids(&params);
    params32->size = params.size;
    params32->result = params.result;
    params32->num = params.num;
    params32->default_idx = params.default_idx;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_create_stream(void *args)
{
    struct
    {
        PTR32 name;
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        DWORD flags;
        REFERENCE_TIME duration;
        REFERENCE_TIME period;
        PTR32 fmt;
        HRESULT result;
        PTR32 channel_count;
        PTR32 stream;
    } *params32 = args;
    struct create_stream_params params =
    {
        .name = ULongToPtr(params32->name),
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .flags = params32->flags,
        .duration = params32->duration,
        .period = params32->period,
        .fmt = ULongToPtr(params32->fmt),
        .channel_count = ULongToPtr(params32->channel_count),
        .stream = ULongToPtr(params32->stream)
    };
    unix_create_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_release_stream(void *args)
{
    struct
    {
        stream_handle stream;
        PTR32 timer_thread;
        HRESULT result;
    } *params32 = args;
    struct release_stream_params params =
    {
        .stream = params32->stream,
        .timer_thread = ULongToHandle(params32->timer_thread)
    };
    unix_release_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_render_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        UINT32 frames;
        HRESULT result;
        PTR32 data;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_render_buffer_params params =
    {
        .stream = params32->stream,
        .frames = params32->frames,
        .data = &data
    };
    unix_get_render_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_capture_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 data;
        PTR32 frames;
        PTR32 flags;
        PTR32 devpos;
        PTR32 qpcpos;
    } *params32 = args;
    UINT32 frames = 0;
    struct get_capture_buffer_params params =
    {
        .stream = params32->stream,
        .frames = &frames
    };
    unix_get_capture_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->frames) = frames;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_is_format_supported(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        PTR32 fmt_in;
        HRESULT result;
    } *params32 = args;
    struct is_format_supported_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .fmt_in = ULongToPtr(params32->fmt_in),
    };
    unix_is_format_supported(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_mix_format(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 fmt;
        HRESULT result;
    } *params32 = args;
    struct get_mix_format_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .fmt = ULongToPtr(params32->fmt)
    };
    unix_get_mix_format(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_device_period(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        HRESULT result;
        PTR32 def_period;
        PTR32 min_period;
    } *params32 = args;
    struct get_device_period_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .def_period = ULongToPtr(params32->def_period),
        .min_period = ULongToPtr(params32->min_period),
    };
    unix_get_device_period(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_buffer_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_buffer_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    unix_get_buffer_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_latency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 latency;
    } *params32 = args;
    struct get_latency_params params =
    {
        .stream = params32->stream,
        .latency = ULongToPtr(params32->latency)
    };
    unix_get_latency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_current_padding(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 padding;
    } *params32 = args;
    struct get_current_padding_params params =
    {
        .stream = params32->stream,
        .padding = ULongToPtr(params32->padding)
    };
    unix_get_current_padding(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_next_packet_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_next_packet_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    unix_get_next_packet_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_position(void *args)
{
    struct
    {
        stream_handle stream;
        BOOL device;
        HRESULT result;
        PTR32 pos;
        PTR32 qpctime;
    } *params32 = args;
    struct get_position_params params =
    {
        .stream = params32->stream,
        .device = params32->device,
        .pos = ULongToPtr(params32->pos),
        .qpctime = ULongToPtr(params32->qpctime)
    };
    unix_get_position(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_frequency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 freq;
    } *params32 = args;
    struct get_frequency_params params =
    {
        .stream = params32->stream,
        .freq = ULongToPtr(params32->freq)
    };
    unix_get_frequency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_set_volumes(void *args)
{
    struct
    {
        stream_handle stream;
        float master_volume;
        PTR32 volumes;
        PTR32 session_volumes;
    } *params32 = args;
    struct set_volumes_params params =
    {
        .stream = params32->stream,
        .master_volume = params32->master_volume,
        .volumes = ULongToPtr(params32->volumes),
        .session_volumes = ULongToPtr(params32->session_volumes),
    };
    return unix_set_volumes(&params);
}

static NTSTATUS unix_wow64_set_event_handle(void *args)
{
    struct
    {
        stream_handle stream;
        PTR32 event;
        HRESULT result;
    } *params32 = args;
    struct set_event_handle_params params =
    {
        .stream = params32->stream,
        .event = ULongToHandle(params32->event)
    };
    unix_set_event_handle(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_prop_value(void *args)
{
    struct propvariant32
    {
        WORD vt;
        WORD pad1, pad2, pad3;
        union
        {
            ULONG ulVal;
            PTR32 ptr;
            ULARGE_INTEGER uhVal;
        };
    } *value32;
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 guid;
        PTR32 prop;
        HRESULT result;
        PTR32 value;
        PTR32 buffer;
        PTR32 buffer_size;
    } *params32 = args;
    PROPVARIANT value;
    struct get_prop_value_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .guid = ULongToPtr(params32->guid),
        .prop = ULongToPtr(params32->prop),
        .value = &value,
        .buffer = ULongToPtr(params32->buffer),
        .buffer_size = ULongToPtr(params32->buffer_size)
    };
    unix_get_prop_value(&params);
    params32->result = params.result;
    if (SUCCEEDED(params.result))
    {
        value32 = UlongToPtr(params32->value);
        value32->vt = value.vt;
        switch (value.vt)
        {
        case VT_UI4:
            value32->ulVal = value.ulVal;
            break;
        case VT_LPWSTR:
            value32->ptr = params32->buffer;
            break;
        default:
            FIXME("Unhandled vt %04x\n", value.vt);
        }
    }
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    unix_process_attach,
    unix_not_implemented,        /* process_detach */
    unix_wow64_main_loop,
    unix_wow64_get_endpoint_ids,
    unix_wow64_create_stream,
    unix_wow64_release_stream,
    unix_start,
    unix_stop,
    unix_reset,
    unix_timer_loop,
    unix_wow64_get_render_buffer,
    unix_release_render_buffer,
    unix_wow64_get_capture_buffer,
    unix_release_capture_buffer,
    unix_wow64_is_format_supported,
    unix_not_implemented,        /* get_loopback_capture_device */
    unix_wow64_get_mix_format,
    unix_wow64_get_device_period,
    unix_wow64_get_buffer_size,
    unix_wow64_get_latency,
    unix_wow64_get_current_padding,
    unix_wow64_get_next_packet_size,
    unix_wow64_get_frequency,
    unix_wow64_get_position,
    unix_wow64_set_volumes,
    unix_wow64_set_event_handle,
    unix_not_implemented,        /* set_sample_rate */
    unix_wow64_test_connect,
    unix_is_started,
    unix_wow64_get_prop_value,
    unix_not_implemented,        /* midi_get_driver */
    unix_not_implemented,        /* midi_init */
    unix_not_implemented,        /* midi_release */
    unix_not_implemented,        /* midi_out_message */
    unix_not_implemented,        /* midi_in_message */
    unix_not_implemented,        /* midi_notify_wait */
    unix_not_implemented,        /* aux_message */
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == funcs_count);

#endif /* _WIN64 */
